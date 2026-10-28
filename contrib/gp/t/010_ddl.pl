# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# DDL of the coordinator runs on the segments, with the same OIDs, in
# transactions that follow the transaction of the coordinator

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\n");
$coordinator->start;
$coordinator->safe_psql('postgres', 'CREATE EXTENSION gp');

my @segments;
foreach my $segment_id (1 .. 2)
{
	my $node = PostgreSQL::Test::Cluster->new("seg$segment_id");
	$node->init;
	$node->append_conf(
		'postgresql.conf', "shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = $segment_id
gp.coordinator = 'host=${\$coordinator->host} port=${\$coordinator->port}'
gp.host = '${\$node->host}'
gp.register_retry_interval = 1
max_prepared_transactions = 10
");
	$node->start;
	push @segments, $node;
}
my ($seg1, $seg2) = @segments;
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.cluster_segments()')
  or die 'segments did not register';

sub run { return $coordinator->safe_psql('postgres', shift); }

sub fails
{
	my ($sql, $message, $test) = @_;
	my ($ret, $stdout, $stderr) = $coordinator->psql('postgres', $sql);
	like($stderr, $message, $test);
}

# Whether a relation is on each segment, as "seg1 seg2" answers
sub where
{
	my ($relname) = @_;
	return join ' ',
	  map { $_->safe_psql('postgres', "SELECT to_regclass('$relname') IS NOT NULL") }
	  @segments;
}

# Catalog rows of schema s with their OIDs
my $objects = q{
SELECT 'class', oid::int, relname, relkind::text FROM pg_class
WHERE relnamespace IN ('s'::regnamespace, 's2'::regnamespace)
UNION ALL SELECT 'attribute', attrelid::int, attname, atttypid::regtype::text
FROM pg_attribute
WHERE attrelid IN (SELECT oid FROM pg_class
				   WHERE relnamespace = 's'::regnamespace) AND
	  attnum > 0 AND NOT attisdropped
UNION ALL SELECT 'type', oid::int, typname, typtype::text FROM pg_type
WHERE typnamespace = 's'::regnamespace
UNION ALL SELECT 'proc', oid::int, proname, provolatile::text FROM pg_proc
WHERE pronamespace = 's'::regnamespace
UNION ALL SELECT 'enum', enumtypid::int, enumlabel, enumsortorder::text
FROM pg_enum WHERE enumtypid = 's.e'::regtype
UNION ALL SELECT 'sequence', seqrelid::int, seqincrement::text, ''
FROM pg_sequence WHERE seqrelid = 's.q'::regclass
ORDER BY 1, 2, 3};

# DDL of the list
run('CREATE SCHEMA s');
run('CREATE SCHEMA s2');
run('CREATE TABLE s.t (a int PRIMARY KEY, b text)');
run('CREATE INDEX ON s.t (b)');
run('CREATE TYPE s.c AS (x int, y text)');
run("CREATE TYPE s.e AS ENUM ('x', 'z')");
run("ALTER TYPE s.e ADD VALUE 'y' BEFORE 'z'");
run('CREATE TYPE s.r AS RANGE (subtype = int4)');
run('CREATE DOMAIN s.d AS int CHECK (VALUE > 0)');
run('CREATE FUNCTION s.f(i int) RETURNS int LANGUAGE sql RETURN i + 1');
run('ALTER FUNCTION s.f(int) IMMUTABLE');
run('CREATE SEQUENCE s.q');
run('ALTER SEQUENCE s.q INCREMENT 2');
run('CREATE VIEW s.v AS SELECT a, b FROM s.t');
run('ALTER TABLE s.t ADD COLUMN c int DEFAULT 5, ADD COLUMN dt date DEFAULT $$2026-01-01$$');
run('ALTER TABLE s.t RENAME COLUMN b TO bb');
run('CREATE TABLE s.moved (a int)');
run('ALTER TABLE s.moved SET SCHEMA s2');
run('CREATE TABLE s.gone (a int)');
run('DROP TABLE s.gone');

my $expected = run($objects);
like($expected, qr/^enum\|\d+\|y\|1\.5$/m, 'the objects include the new enum label');
is($seg1->safe_psql('postgres', $objects), $expected,
	'segment 1 has the objects of the coordinator with their OIDs');
is($seg2->safe_psql('postgres', $objects), $expected,
	'segment 2 has the objects of the coordinator with their OIDs');

# Statements in one query string, and in functions
command_ok(
	[
		'psql', '-X', '-d', $coordinator->connstr('postgres'),
		'-c', 'CREATE TABLE s.m1 (a int); CREATE TABLE s.m2 (a int)'
	],
	'two statements in one query string');
is(where('s.m1') . ' ' . where('s.m2'), 't t t t',
	'each statement of a query string goes to the segments');

run(q{CREATE FUNCTION s.make() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	CREATE TABLE s.static (a int);
	EXECUTE 'CREATE TABLE s.dynamic (a int)';
END
$$});
run('SELECT s.make()');
is(where('s.static') . ' ' . where('s.dynamic'), 't t t t',
	'DDL of a function goes to the segments');

run(q{CREATE SCHEMA s3 CREATE TABLE inner_t (a int) CREATE VIEW inner_v AS SELECT a FROM inner_t});
is(where('s3.inner_t') . ' ' . where('s3.inner_v'), 't t t t',
	'CREATE SCHEMA with elements goes to the segments once');

# Transactions
run('BEGIN; CREATE TABLE s.rolled_back (a int); ROLLBACK');
is(where('s.rolled_back'), 'f f', 'a rollback on the coordinator rolls back the segments');

run('BEGIN; CREATE TABLE s.vis (a int); SELECT gp.exec_on_all($$SELECT * FROM s.vis$$); COMMIT');
pass('gp.exec_on_all() sees the DDL of its transaction on the segments');

$seg2->safe_psql('postgres',
	'SET gp.allow_local_ddl = on; CREATE TABLE s.conflict (a int)');
fails('CREATE TABLE s.conflict (a int)',
	qr/relation "conflict" already exists\nCONTEXT:  segment 2 at /,
	'an error of a segment fails the command');
is( $coordinator->safe_psql('postgres', "SELECT to_regclass('s.conflict') IS NOT NULL")
	  . ' ' . where('s.conflict'),
	'f f t',
	'the coordinator and the other segments roll the command back');
$seg2->safe_psql('postgres',
	'SET gp.allow_local_ddl = on; DROP TABLE s.conflict');

fails(
	'BEGIN; SAVEPOINT a; CREATE TABLE s.sp (a int); ROLLBACK TO a; COMMIT',
	qr/cannot commit a transaction whose DDL the segments have lost/,
	'a rolled back subtransaction with DDL fails the commit');
is(where('s.sp'), 'f f', 'the failed commit leaves nothing on the segments');
run('BEGIN; SAVEPOINT a; CREATE TABLE s.sp_kept (a int); RELEASE a; COMMIT');
is(where('s.sp_kept'), 't t', 'a released subtransaction with DDL commits');

# What stays on the coordinator
run(q{CREATE TEMP TABLE tt (a int);
CREATE INDEX ON tt (a);
ALTER TABLE tt ADD COLUMN b int DEFAULT 1;
CREATE VIEW tv AS SELECT a FROM tt;
DROP VIEW tv;
DROP TABLE tt});
pass('temporary objects stay on the coordinator');
fails(
	'CREATE TEMP TABLE tt2 (a int); DROP TABLE tt2, s.m1',
	qr/cannot drop objects of the coordinator and of the cluster at once/,
	'temporary and other tables are not dropped at once');

run('CREATE EXTENSION hstore');
is( join(' ', map { $_->safe_psql('postgres', "SELECT to_regtype('hstore') IS NOT NULL") } @segments),
	'f f',
	'objects of an extension script stay on the coordinator');

run('SET gp.dispatch_ddl = off; CREATE TABLE s.only_here (a int)');
is(where('s.only_here'), 'f f', 'gp.dispatch_ddl = off keeps DDL on the coordinator');

# What is refused
fails('ALTER TABLE s.t ADD COLUMN at timestamptz DEFAULT now()',
	qr/cannot compute column "at" with a non-immutable expression on a cluster/,
	'a column with a non-immutable default');
fails('ALTER TABLE s.t ADD COLUMN n int DEFAULT nextval($$s.q$$)',
	qr/cannot compute column "n" with a non-immutable expression/,
	'a column with a default from a sequence');
fails('ALTER TABLE s.t ADD COLUMN id serial',
	qr/cannot add a serial column on a cluster/, 'a serial column');
fails('ALTER TABLE s.t ADD COLUMN id int GENERATED ALWAYS AS IDENTITY',
	qr/cannot add an identity column on a cluster/, 'an identity column');
fails('ALTER TABLE s.t ALTER COLUMN c TYPE float8 USING random()',
	qr/cannot compute column "c" with a non-immutable expression/,
	'a type change with a non-immutable USING');
run('ALTER TABLE s.t ALTER COLUMN c TYPE bigint USING c * 2');
run('CREATE TABLE s.dflt (a int, at timestamptz DEFAULT now())');
run('ALTER TABLE s.dflt ALTER COLUMN a SET DEFAULT random()');
is(where('s.dflt'), 't t',
	'defaults that are only stored are allowed');
fails('CREATE INDEX CONCURRENTLY ON s.t (c)',
	qr/CREATE INDEX CONCURRENTLY is not supported on a cluster yet/,
	'CREATE INDEX CONCURRENTLY');

# The end
run('DROP SCHEMA s, s2, s3 CASCADE');
is(where('s.t'), 'f f', 'DROP SCHEMA CASCADE goes to the segments');

$_->stop foreach (@segments, $coordinator);

done_testing();
