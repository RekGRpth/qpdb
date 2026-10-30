# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# INSERT into a distributed table: the coordinator evaluates the source of the
# rows and sends each to the segment of its bucket

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
	my ($sql, $message, $test, %params) = @_;
	my ($ret, $stdout, $stderr) = $coordinator->psql('postgres', $sql, %params);
	like($stderr, $message, $test);
}

# What each segment has by a query that gives one value, as "seg1 seg2"
sub on_segments
{
	my ($query) = @_;
	return join ' ', map { $_->safe_psql('postgres', $query) } @segments;
}

# The sum of a number that each segment gives
sub total
{
	my ($query) = @_;
	my $total = 0;
	$total += $_->safe_psql('postgres', $query) foreach @segments;
	return $total;
}

# Compare what the segments have, by a query that gives one value, with what
# the coordinator computes for each segment: a query of "segment|value" rows
sub placed
{
	my ($expected, $query, $test) = @_;
	my %want = map { split /\|/, $_, 2 } split /\n/, run($expected);
	is( on_segments($query),
		join(' ', map { $want{$_} // '' } 1 .. 2),
		$test);
}

# Rows of VALUES, with the defaults of the table
run(q{CREATE TABLE t (a int, b text DEFAULT 'd',
					   c int GENERATED ALWAYS AS (a * 2) STORED,
					   ts timestamptz DEFAULT now())});
run(q{SELECT gp.distribute('t', ARRAY['a'])});

my $values = join ',', map { "($_)" } 1 .. 100;
my ($ret, $stdout, $stderr) = $coordinator->psql('postgres',
	qq{DO \$\$ DECLARE n bigint; BEGIN
INSERT INTO t (a) VALUES $values;
GET DIAGNOSTICS n = ROW_COUNT; RAISE NOTICE 'rows %', n; END \$\$});
like($stderr, qr/NOTICE:  rows 100/, 'the number of rows is that of the source');
is(run('SELECT count(*) FROM t'), 0, 'the coordinator has no rows');
placed(
	q{SELECT gp.bucket_segment(p.colocation_id, gp.bucket(1024, g)),
	   string_agg(g::text, ',' ORDER BY g)
FROM generate_series(1, 100) g, gp.distribution_policy p
WHERE p.relid = 't'::regclass GROUP BY 1 ORDER BY 1},
	q{SELECT string_agg(a::text, ',' ORDER BY a) FROM t},
	'each row is on the segment of its bucket');
like(on_segments('SELECT count(*) FROM t'), qr/^[1-9]\d* [1-9]\d*$/,
	'both segments have rows');
is(total(q{SELECT count(*) FROM t WHERE b = 'd'}), 100,
	'the default of a column is that of the table');
is(on_segments('SELECT count(*) FROM t WHERE c <> a * 2'), '0 0',
	'a generated column is computed on the segment');

# now() is that of the coordinator, one for all rows
my %stamps = map { $_ => 1 }
  map { split /,/, $_->safe_psql('postgres', 'SELECT string_agg(DISTINCT ts::text, \',\') FROM t') }
  @segments;
is(scalar keys %stamps, 1, 'now() is the same on all segments');

# A SELECT as the source, from the coordinator
run('CREATE TABLE loc (a int)');
run('INSERT INTO loc SELECT g FROM generate_series(101, 150) g');
is(run('SELECT count(*) FROM loc'), 50, 'a local table keeps its rows');
($ret, $stdout, $stderr) = $coordinator->psql('postgres',
	q{DO $$ DECLARE n bigint; BEGIN
INSERT INTO t (a) SELECT a FROM loc;
GET DIAGNOSTICS n = ROW_COUNT; RAISE NOTICE 'rows %', n; END $$});
like($stderr, qr/NOTICE:  rows 50/, 'INSERT ... SELECT counts its rows');
is(total('SELECT count(*) FROM t'), 150, 'they are on the segments');
placed(
	q{SELECT gp.bucket_segment(p.colocation_id, gp.bucket(1024, g)),
	   string_agg(g::text, ',' ORDER BY g)
FROM generate_series(101, 150) g, gp.distribution_policy p
WHERE p.relid = 't'::regclass GROUP BY 1 ORDER BY 1},
	q{SELECT string_agg(a::text, ',' ORDER BY a) FROM t WHERE a > 100},
	'and in the segments of their buckets');

# DEFAULT VALUES, and a key from a default
run('CREATE TABLE d (a int DEFAULT 7, b int DEFAULT 8)');
run(q{SELECT gp.distribute('d', ARRAY['a'])});
run('INSERT INTO d DEFAULT VALUES');
is(total(q{SELECT count(*) FROM d WHERE a = 7 AND b = 8}), 1,
	'DEFAULT VALUES makes a row');
placed(
	q{SELECT gp.bucket_segment(p.colocation_id, gp.bucket(1024, 7)), '1'
FROM gp.distribution_policy p WHERE p.relid = 'd'::regclass},
	'SELECT nullif(count(*), 0) FROM d',
	'on the segment of the default key');

# A serial column is numbered on the coordinator
run('CREATE TABLE sq (id serial, v text)');
run(q{SELECT gp.distribute('sq', ARRAY['id'])});
run(q{INSERT INTO sq (v) VALUES ('a'), ('b'), ('c'), ('d'), ('e'), ('f')});
my @ids = sort { $a <=> $b }
  map { split /\n/, $_->safe_psql('postgres', 'SELECT id FROM sq') } @segments;
is("@ids", '1 2 3 4 5 6', 'the numbers are of the sequence of the coordinator');

# Every type goes as the segment takes it
run(q{CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy')});
run(q{CREATE TABLE ty (id int, m mood, arr int[], ts timestamptz, n numeric,
					   j jsonb, tx text)});
run(q{SELECT gp.distribute('ty', ARRAY['id'])});
run('CREATE TABLE ty_ref (LIKE ty)');
my $rows = q{(1, 'sad', '{1,2,3}', '2026-10-30 12:00:00+00', 1.50, '{"a": [1, 2]}', 'one'),
(2, 'ok', '{}', '2026-01-01 00:00:00+00', -0.001, '[]', E'two\ttab'),
(3, 'happy', '{NULL,5}', 'infinity', 1e20, 'null', NULL),
(4, NULL, NULL, NULL, NULL, NULL, NULL)};
run("INSERT INTO ty VALUES $rows");
run("INSERT INTO ty_ref VALUES $rows");
my @want = sort split /\n/, run(q{SELECT md5(r::text) FROM ty_ref r});
my @got = sort map { split /\n/, $_->safe_psql('postgres', q{SELECT md5(r::text) FROM ty r}) } @segments;
is_deeply(\@got, \@want, 'rows of many types are as the coordinator takes them');
is(scalar @got, 4, 'there are all four rows');

# A replicated table has all rows on every segment
run('CREATE TABLE rep (a int)');
run(q{SELECT gp.distribute('rep', NULL)});
run('INSERT INTO rep VALUES (1), (2), (3)');
is(on_segments('SELECT count(*) FROM rep'), '3 3', 'rows of a replicated table');

# A transaction of the coordinator covers the INSERT, with COPY as well
run('CREATE TABLE tx (a int)');
run(q{SELECT gp.distribute('tx', ARRAY['a'])});
run(qq{BEGIN;
INSERT INTO tx VALUES (1), (2), (3);
ROLLBACK});
is(on_segments('SELECT count(*) FROM tx'), '0 0', 'a rollback undoes the INSERT');
run(qq{BEGIN;
INSERT INTO tx VALUES (1), (2), (3);
COPY tx FROM PROGRAM 'seq 4 6';
INSERT INTO tx SELECT g FROM generate_series(7, 9) g;
COMMIT});
is(total('SELECT count(*) FROM tx'), 9, 'a commit keeps the rows of all three');

# A prepared statement, with its generic plan as well
run(q{PREPARE p (int) AS INSERT INTO tx VALUES ($1);
EXECUTE p (10); EXECUTE p (11); EXECUTE p (12); EXECUTE p (13);
EXECUTE p (14); EXECUTE p (15); EXECUTE p (16); EXECUTE p (17)});
is(total('SELECT count(*) FROM tx'), 17, 'a prepared INSERT');

# INSERT in a function
run(q{CREATE FUNCTION fill() RETURNS void LANGUAGE plpgsql AS
$$ BEGIN FOR i IN 20 .. 24 LOOP INSERT INTO tx VALUES (i); END LOOP; END $$});
run('SELECT fill()');
is(total('SELECT count(*) FROM tx'), 22, 'INSERT in a function');

# What is refused
fails('INSERT INTO tx VALUES (1) RETURNING a',
	qr/INSERT \.\.\. RETURNING is not supported on a distributed table/,
	'RETURNING is refused');
fails('INSERT INTO tx VALUES (1) ON CONFLICT DO NOTHING',
	qr/INSERT \.\.\. ON CONFLICT is not supported on a distributed table/,
	'ON CONFLICT is refused');
is(total('SELECT count(*) FROM tx'), 22, 'and inserts nothing');

# A row that a segment refuses
run('CREATE TABLE chk (a int CHECK (a < 100))');
run(q{SELECT gp.distribute('chk', ARRAY['a'])});
fails('INSERT INTO chk VALUES (1), (2), (500)',
	qr/violates check constraint "chk_a_check"\n.*segment \d at /s,
	'a row that a segment refuses');
is(on_segments('SELECT count(*) FROM chk'), '0 0', 'no row of it stays');

# The plan
my $plan = run(q{EXPLAIN (COSTS off) INSERT INTO tx VALUES (1)});
like($plan, qr/Custom Scan \(GpInsert\)/, 'the plan has the node of the cluster');
is(run('EXPLAIN (COSTS off) INSERT INTO loc VALUES (1)') =~ /GpInsert/ ? 1 : 0, 0,
	'and a local table has the usual one');

# Privileges are those of the user, on the coordinator and on the segments
run('CREATE ROLE u1 LOGIN');
run('CREATE ROLE u2 LOGIN');
run('GRANT INSERT ON chk TO u1');
($ret, $stdout, $stderr) = $coordinator->psql('postgres',
	'INSERT INTO chk VALUES (1), (2), (3)', extra_params => [ '-U', 'u1' ]);
is($ret, 0, 'a user with INSERT can INSERT');
is(total('SELECT count(*) FROM chk'), 3, 'and the rows are on the segments');
($ret, $stdout, $stderr) = $coordinator->psql('postgres',
	'INSERT INTO chk VALUES (4)', extra_params => [ '-U', 'u2' ]);
like($stderr, qr/permission denied for table chk/, 'a user without INSERT cannot');

# A table that is not distributed is as before
run('INSERT INTO loc VALUES (1), (2)');
is(run('SELECT count(*) FROM loc'), 52, 'a local table keeps its rows on the coordinator');
is(on_segments('SELECT count(*) FROM loc'), '0 0', 'and has none on the segments');

$_->stop foreach (@segments, $coordinator);

done_testing();
