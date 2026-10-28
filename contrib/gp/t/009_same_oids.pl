# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A segment gives the objects of a command the OIDs that the coordinator
# gave them, and creates no objects of its own

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\ngp.dispatch_ddl = off\n");
$coordinator->start;
$coordinator->safe_psql('postgres', 'CREATE EXTENSION gp');

my $segment = PostgreSQL::Test::Cluster->new('seg1');
$segment->init;
$segment->append_conf(
	'postgresql.conf', "shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = 1
gp.coordinator = 'host=${\$coordinator->host} port=${\$coordinator->port}'
gp.host = '${\$segment->host}'
gp.register_retry_interval = 1
");
$segment->start;
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 1 FROM gp.cluster_segments()')
  or die 'the segment did not register';

# Run a command on the coordinator and return the OIDs it recorded
sub on_coordinator
{
	my ($sql) = @_;
	return $coordinator->safe_psql('postgres',
		"BEGIN; $sql; SELECT gp.recorded_oids(); COMMIT");
}

# Run a command on the segment with OIDs, return the error if any
sub on_segment
{
	my ($sql, $oids) = @_;
	my ($ret, $stdout, $stderr) = $coordinator->psql('postgres',
		"SELECT status FROM gp.exec_on_all(\$cmd\$SET LOCAL gp.dispatch_oids = '$oids'; $sql\$cmd\$)"
	);
	return $stderr;
}

sub both
{
	my ($sql) = @_;
	is(on_segment($sql, on_coordinator($sql)), '', $sql);
}

# A segment creates no objects of its own
my ($ret, $stdout, $stderr) =
  $segment->psql('postgres', 'CREATE TABLE local (a int)');
like($stderr, qr/cannot create objects on a segment/,
	'a segment creates no table of its own');
($ret, $stdout, $stderr) =
  $segment->psql('postgres', 'CREATE TEMP TABLE local (a int)');
like($stderr, qr/cannot create objects on a segment/,
	'a segment creates no temporary table of its own');
$segment->safe_psql('postgres',
	'SET gp.allow_local_ddl = on; CREATE TABLE local (a int); DROP TABLE local'
);
pass('gp.allow_local_ddl lets a segment create objects');

is($coordinator->safe_psql('postgres', 'SELECT gp.recorded_oids()'),
	'', 'a transaction starts with no OIDs');

# Objects get the same OIDs
both('CREATE SCHEMA s');
both('CREATE TABLE s.t (a int PRIMARY KEY, b text)');
both('CREATE INDEX ON s.t (b)');
both("CREATE TYPE s.e AS ENUM ('x', 'y', 'z')");
both('CREATE FUNCTION s.f(i int) RETURNS int LANGUAGE sql RETURN i + 1');
both('CREATE SEQUENCE s.q');
both('CREATE VIEW s.v AS SELECT a FROM s.t');
both('CREATE ROLE r1');

my $objects = q{
SELECT 'class', oid::int, relname FROM pg_class
WHERE relnamespace = 's'::regnamespace OR
	  relname LIKE ANY (SELECT 'pg_toast\_' || oid || '%' FROM pg_class
						WHERE relnamespace = 's'::regnamespace)
UNION ALL SELECT 'type', oid::int, typname FROM pg_type
WHERE typnamespace = 's'::regnamespace
UNION ALL SELECT 'proc', oid::int, proname FROM pg_proc
WHERE pronamespace = 's'::regnamespace
UNION ALL SELECT 'constraint', oid::int, conname FROM pg_constraint
WHERE connamespace = 's'::regnamespace
UNION ALL SELECT 'enum', oid::int, enumlabel FROM pg_enum
WHERE enumtypid = 's.e'::regtype
UNION ALL SELECT 'namespace', oid::int, nspname FROM pg_namespace
WHERE nspname = 's'
UNION ALL SELECT 'role', oid::int, rolname FROM pg_authid
WHERE rolname = 'r1'
ORDER BY 1, 3};
my $expected = $coordinator->safe_psql('postgres', $objects);
like($expected, qr/\|pg_toast_\d+\n.*\|pg_toast_\d+_index\n/s,
	'the objects include a TOAST table and its index');
is($segment->safe_psql('postgres', $objects),
	$expected, 'the segment has the objects with the OIDs of the coordinator');

# A segment that runs a command differently fails it
my $oids = on_coordinator('CREATE TYPE s.c AS (a int)');
like(
	on_segment('CREATE SCHEMA s2', $oids),
	qr/OID \d+ from the coordinator is for catalog "pg_class", not "pg_namespace"/,
	'an OID for another catalog');
is($segment->safe_psql('postgres', "SELECT to_regnamespace('s2')"),
	'', 'the failed command creates nothing');

$oids = on_coordinator('CREATE TABLE s.t2 (a int)');
like(
	on_segment('CREATE TABLE s.t2 (a int)', (split /,/, $oids)[0]),
	qr/the coordinator assigned 1 OIDs, but the segment needs more/,
	'too few OIDs');

$oids = on_coordinator('CREATE SCHEMA s3');
like(
	on_segment('CREATE SCHEMA s3', "$oids,2615:99999"),
	qr/the coordinator assigned 2 OIDs, but the segment used 1/,
	'too many OIDs');
is($segment->safe_psql('postgres', "SELECT to_regnamespace('s3')"),
	'', 'a command with OIDs left over creates nothing');

# An OID in use on the segment is an error, not a duplicate
my $taken = $segment->safe_psql('postgres', "SELECT 's'::regnamespace::oid");
like(
	on_segment('CREATE SCHEMA s4', "2615:$taken"),
	qr/proposed OID $taken is already in use in relation "pg_namespace"/,
	'an OID in use on the segment');

($ret, $stdout, $stderr) =
  $segment->psql('postgres', "SET gp.dispatch_oids = 'nonsense'");
like(
	$stderr,
	qr/invalid value for parameter "gp.dispatch_oids".*\nDETAIL:  A list of OIDs is "catalog:oid" pairs/,
	'a malformed list of OIDs');

$segment->stop;
$coordinator->stop;

done_testing();
