# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Extensions of the coordinator on the segments, with the same OIDs; extension
# gp stays on the coordinator

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

# Compare a query on the coordinator and on each segment
sub same
{
	my ($query, $test) = @_;
	my $expected = run($query);
	is($_->safe_psql('postgres', $query), $expected, "$test on " . $_->name)
	  foreach @segments;
	return $expected;
}

# Whether an extension is on each node, as "coordinator seg1 seg2" answers
sub where
{
	my ($extname) = @_;
	return join ' ',
	  map {
		$_->safe_psql('postgres',
			"SELECT count(*) FROM pg_extension WHERE extname = '$extname'")
	  } ($coordinator, @segments);
}

# The extensions with their OIDs, and what belongs to them
my $extensions = q{
SELECT oid::int, extname, extversion, extnamespace::regnamespace::text
FROM pg_extension WHERE extname IN ('hstore', 'cube', 'earthdistance')
ORDER BY 2};
my $members = q{
SELECT d.classid::regclass::text, d.objid::int,
	   pg_describe_object(d.classid, d.objid, 0)
FROM pg_depend d
WHERE d.refclassid = 'pg_extension'::regclass AND d.deptype = 'e' AND
	  d.refobjid IN (SELECT oid FROM pg_extension
					 WHERE extname IN ('hstore', 'cube', 'earthdistance'))
ORDER BY 1, 2};

# CREATE EXTENSION, and the objects of its script
run("CREATE EXTENSION hstore VERSION '1.7'");
my $list = same($extensions, 'the extension');
like($list, qr/^\d+\|hstore\|1\.7\|public$/, 'it has the version asked');
$list = same($members, 'the objects of the extension script');
like($list, qr/\|type hstore$/m, 'the script has made the type');

# ALTER EXTENSION UPDATE runs its scripts on the segments
run("ALTER EXTENSION hstore UPDATE TO '1.8'");
$list = same($extensions, 'the extension after an update');
like($list, qr/^\d+\|hstore\|1\.8\|public$/, 'it has the new version');
same($members, 'the objects of the extension after an update');

# Objects of an extension are on the segments, so DDL on them goes there
run('CREATE ROLE r1');
run('GRANT EXECUTE ON FUNCTION hstore(text, text) TO r1');
run("COMMENT ON FUNCTION hstore(text, text) IS 'makes an hstore'");
$list = same(
	q{SELECT oid::int, proacl::text, obj_description(oid, 'pg_proc')
FROM pg_proc WHERE oid = 'hstore(text, text)'::regprocedure},
	'privileges and a comment on a function of an extension');
like($list, qr/r1=X\/.*\|makes an hstore$/, 'they are set');

run('CREATE TABLE h (a hstore)');
run('CREATE INDEX ON h USING gin (a)');
same(q{SELECT atttypid::int, atttypid::regtype::text FROM pg_attribute
WHERE attrelid = 'h'::regclass AND attnum = 1},
	'a table with a type of an extension');
same(q{SELECT oid::int, relname FROM pg_class WHERE relname = 'h_a_idx'},
	'an index of a type of an extension');

run('CREATE SCHEMA s');
run('ALTER EXTENSION hstore SET SCHEMA s');
$list = same($extensions, 'the extension in another schema');
like($list, qr/^\d+\|hstore\|1\.8\|s$/, 'it is in the schema');

# A segment that cannot create the extension fails it everywhere
{
	local $ENV{PGOPTIONS} = '-c gp.allow_local_ddl=on';
	$seg1->safe_psql('postgres', 'CREATE EXTENSION cube');
}
fails('CREATE EXTENSION cube', qr/extension "cube" already exists/,
	'CREATE EXTENSION that a segment has');
is(where('cube'), '0 1 0', 'it is on the segment only');
{
	local $ENV{PGOPTIONS} = '-c gp.allow_local_ddl=on';
	$seg1->safe_psql('postgres', 'DROP EXTENSION cube');
}

# CASCADE creates what the extension requires, on the segments too
run('CREATE EXTENSION earthdistance CASCADE');
is(where('cube'), '1 1 1', 'the required extension is on all nodes');
$list = same($extensions, 'both extensions');
like($list, qr/\|cube\|.*\n\d+\|earthdistance\|/, 'there are both');
same($members, 'the objects of both extensions');

# DROP EXTENSION goes to the segments
run('DROP TABLE h');
run('DROP EXTENSION earthdistance, cube, hstore');
is(where('hstore'), '0 0 0', 'DROP EXTENSION');
is(where('cube'), '0 0 0', 'DROP EXTENSION of several');
same($members, 'nothing of them is left');

# Extension gp stays on the coordinator
run('CREATE DATABASE other');
$coordinator->safe_psql('other', 'CREATE EXTENSION gp');
is( join(' ',
		map { $_->safe_psql('other', q{SELECT count(*) FROM pg_extension WHERE extname = 'gp'}) }
		  @segments),
	'0 0',
	'CREATE EXTENSION gp stays on the coordinator');
$coordinator->safe_psql('other', 'DROP EXTENSION gp');
fails('DROP EXTENSION gp, hstore',
	qr/cannot operate on objects of the coordinator and of the cluster at once/,
	'extension gp and another one in one command');

$_->stop foreach (@segments, $coordinator);

done_testing();
