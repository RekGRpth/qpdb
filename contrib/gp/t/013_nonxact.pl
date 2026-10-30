# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Commands that cannot run in a transaction block go to the segments after the
# coordinator: CREATE DATABASE, DROP DATABASE, VACUUM and the CONCURRENTLY
# commands

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
autovacuum = off
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

# Whether a database is on each node, as "coordinator seg1 seg2" answers
sub where
{
	my ($datname) = @_;
	return join ' ',
	  map {
		$_->safe_psql('postgres',
			"SELECT count(*) FROM pg_database WHERE datname = '$datname'")
	  } ($coordinator, @segments);
}

# CREATE DATABASE and DROP DATABASE
run('CREATE DATABASE d1');
my $oid = same(q{SELECT oid::int, datname FROM pg_database WHERE datname = 'd1'},
	'the OID of a new database');
like($oid, qr/^\d+\|d1$/, 'the database is there');
is(where('d1'), '1 1 1', 'CREATE DATABASE on all nodes');

run('DROP DATABASE d1');
is(where('d1'), '0 0 0', 'DROP DATABASE on all nodes');
run('DROP DATABASE IF EXISTS d1');

# A command of a transaction block fails on the coordinator, as it would
# without the cluster, and goes nowhere
fails('BEGIN; CREATE DATABASE d4',
	qr/cannot run inside a transaction block/, 'CREATE DATABASE in a block');
is(where('d4'), '0 0 0', 'nothing of it on any node');

# A segment that fails drops the database on the others, and the coordinator
# rolls back
{
	local $ENV{PGOPTIONS} = '-c gp.allow_local_ddl=on';
	$seg1->safe_psql('postgres', 'CREATE DATABASE d2');
}
fails('CREATE DATABASE d2', qr/database "d2" already exists/,
	'CREATE DATABASE that a segment has');
is(where('d2'), '0 1 0', 'it is dropped on the others');

# The next command goes through
run('CREATE DATABASE d3');
is(where('d3'), '1 1 1', 'CREATE DATABASE after a failure');

# A segment that fails to drop leaves a warning: the coordinator has dropped
# the database already
my $busy = $seg1->background_psql('d3');
my ($ret, $stdout, $stderr) = $coordinator->psql('postgres', 'DROP DATABASE d3');
is($ret, 0, 'DROP DATABASE that a segment fails to drop is not an error');
like($stderr, qr/WARNING:.*being accessed by other users/,
	'it is a warning');
is(where('d3'), '0 1 0', 'it stays on the segment');
$busy->quit;
run('DROP DATABASE IF EXISTS d3');
is(where('d3'), '0 0 0', 'DROP DATABASE IF EXISTS finishes it');

# VACUUM
run('CREATE TABLE v (a int, b text)');
$_->safe_psql('postgres',
	"INSERT INTO v SELECT g, repeat('x', 100) FROM generate_series(1, 1000) g")
  foreach @segments;
$_->safe_psql('postgres', 'DELETE FROM v') foreach @segments;
my $count = q{SELECT pg_stat_get_vacuum_count('v'::regclass)};
is($_->safe_psql('postgres', $count), 0, 'not vacuumed on ' . $_->name)
  foreach @segments;
run('VACUUM v');
is($_->safe_psql('postgres', $count), 1, 'VACUUM of a table on ' . $_->name)
  foreach @segments;
run('VACUUM');
is($_->safe_psql('postgres', $count), 2, 'VACUUM of all on ' . $_->name)
  foreach @segments;

my $filenode = q{SELECT pg_relation_filenode('v'::regclass)};
my @before = map { $_->safe_psql('postgres', $filenode) } @segments;
$_->safe_psql('postgres', 'INSERT INTO v VALUES (1, NULL)') foreach @segments;
run('VACUUM FULL v');
my @after = map { $_->safe_psql('postgres', $filenode) } @segments;
isnt($after[$_], $before[$_], 'VACUUM FULL rewrites the table on seg' . ($_ + 1))
  foreach 0 .. 1;
same(q{SELECT oid::int, relname FROM pg_class WHERE relname = 'v'},
	'the OID of a vacuumed table');

# Temporary tables stay on the coordinator
run('CREATE TEMP TABLE tt (a int); VACUUM tt');
pass('VACUUM of a temporary table');
fails('CREATE TEMP TABLE tt (a int); VACUUM tt, v',
	qr/cannot operate on objects of the coordinator and of the cluster at once/,
	'VACUUM of temporary and ordinary tables');

# CREATE INDEX CONCURRENTLY and DROP INDEX CONCURRENTLY
run('CREATE TABLE ci (a int, b int)');
$_->safe_psql('postgres', 'INSERT INTO ci SELECT g, g FROM generate_series(1, 100) g')
  foreach @segments;
my $indexes = q{
SELECT c.oid::int, c.relname, i.indisvalid FROM pg_class c
JOIN pg_index i ON i.indexrelid = c.oid
WHERE i.indrelid = 'ci'::regclass ORDER BY 2};

run('CREATE INDEX CONCURRENTLY ci_b ON ci (b)');
run('CREATE INDEX CONCURRENTLY ON ci (a)');
my $list = same($indexes, 'CREATE INDEX CONCURRENTLY');
like($list, qr/^\d+\|ci_a_idx\|t\n\d+\|ci_b\|t$/, 'both indexes are valid');

run('DROP INDEX CONCURRENTLY ci_b');
$list = same($indexes, 'DROP INDEX CONCURRENTLY');
like($list, qr/^\d+\|ci_a_idx\|t$/, 'one is left');

# A duplicate on a segment fails the index there only
$seg1->safe_psql('postgres', 'INSERT INTO ci VALUES (1, 1)');
fails('CREATE UNIQUE INDEX CONCURRENTLY ci_u ON ci (a)',
	qr/could not create unique index "ci_u"/,
	'CREATE UNIQUE INDEX CONCURRENTLY with a duplicate');
is($seg1->safe_psql('postgres',
		q{SELECT indisvalid FROM pg_index WHERE indexrelid = 'ci_u'::regclass}),
	'f', 'the index is invalid on the segment');
run('DROP INDEX ci_u');
$seg1->safe_psql('postgres', 'DELETE FROM ci WHERE a = 1 AND b = 1 AND ctid = (SELECT max(ctid) FROM ci WHERE a = 1)');
run('CREATE UNIQUE INDEX CONCURRENTLY ci_u ON ci (a)');
same($indexes, 'the unique index after a failure');

# gp.dispatch_oids_session does not stay set on a segment
is($seg1->safe_psql('postgres', 'SHOW gp.dispatch_oids_session'),
	'off', 'a new session of the segment has it off');

$coordinator->stop;
$_->stop foreach @segments;

done_testing();
