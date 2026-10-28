# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# gp.exec_on_all() runs a command on all segments over the connections of
# the session

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

sub new_segment
{
	my ($segment_id) = @_;
	my $node = PostgreSQL::Test::Cluster->new("seg$segment_id");

	$node->init;
	$node->append_conf(
		'postgresql.conf', "shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = $segment_id
gp.coordinator = 'host=${\$coordinator->host} port=${\$coordinator->port}'
gp.host = '${\$node->host}'
gp.register_retry_interval = 1
gp.allow_local_ddl = on
");
	$node->start;
	return $node;
}

my ($seg1, $seg2) = map { new_segment($_) } 1 .. 2;
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.cluster_segments()')
  or die 'segments did not register';

# Run gp.exec_on_all() in a session of its own
sub exec_on_all
{
	my ($sql) = @_;
	my ($ret, $stdout, $stderr) = $coordinator->psql('postgres',
		"SELECT segment_id, status FROM gp.exec_on_all(\$\$$sql\$\$)");
	return ($stdout, $stderr);
}

# Whether a relation is on a segment
sub has
{
	my ($node, $relname) = @_;
	return $node->safe_psql('postgres',
		"SELECT to_regclass('$relname') IS NOT NULL");
}

# Connections of the coordinator on a segment
my $connections =
  "SELECT string_agg(pid::text, ' ') FROM pg_stat_activity WHERE application_name = 'gp coordinator'";

my ($stdout, $stderr) = exec_on_all('CREATE TABLE t (a int)');
is($stdout, "1|CREATE TABLE\n2|CREATE TABLE", 'a command runs on all segments');
ok(has($seg1, 't') eq 't' && has($seg2, 't') eq 't',
	'both segments have the table');

($stdout, $stderr) = exec_on_all('SELECT 1');
is($stdout, "1|SELECT 1\n2|SELECT 1", 'rows of a query are discarded');

$seg1->safe_psql('postgres', 'CREATE TABLE u (a int)');
($stdout, $stderr) = exec_on_all('CREATE TABLE u (a int)');
like(
	$stderr,
	qr/ERROR:  relation "u" already exists\nCONTEXT:  segment 1 at /,
	'an error of a segment names the segment');
is(has($seg2, 'u'), 't', 'other segments keep the command');

$coordinator->safe_psql('postgres',
	"BEGIN; SELECT gp.exec_on_all('CREATE TABLE v (a int)'); ROLLBACK");
is(has($seg1, 'v'), 't',
	'a rollback on the coordinator does not undo the command');

# One session keeps one connection to a segment
my $session = $coordinator->background_psql('postgres', on_error_stop => 0);
$session->query_safe("SELECT gp.exec_on_all('SELECT 1')");
my $pids = $seg1->safe_psql('postgres', $connections);
like($pids, qr/^\d+$/, 'a session has one connection to a segment');
$session->query_safe("SELECT gp.exec_on_all('SELECT 1')");
is($seg1->safe_psql('postgres', $connections),
	$pids, 'the session uses the connection again');

# A broken connection fails once and is opened again
$seg2->restart;
my ($output, $failed) =
  $session->query("SELECT gp.exec_on_all('SELECT 1')");
ok($failed, 'a broken connection fails the command');
like(
	$session->{stderr},
	qr/lost the connection to segment 2 at /,
	'the error names the segment of the broken connection');
$session->{stderr} = '';
$session->query_safe("SELECT gp.exec_on_all('SELECT 1')");
pass('the next command opens a new connection');

# A segment that is down
$seg2->stop;
($stdout, $stderr) = exec_on_all('SELECT 1');
like(
	$stderr,
	qr/could not connect to segment 2 at /,
	'a segment that is down cannot be connected to');
$seg2->start;
($stdout, $stderr) = exec_on_all('SELECT 1');
is($stdout, "1|SELECT 1\n2|SELECT 1", 'the segment is used again once up');

# A cancel on the coordinator cancels the command on the segments
(undef, $stdout, $stderr) = $coordinator->psql('postgres',
	"SET statement_timeout = '1s'; SELECT gp.exec_on_all('SELECT pg_sleep(60)')"
);
like(
	$stderr,
	qr/canceling statement due to statement timeout/,
	'the command times out on the coordinator');
$seg1->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE query = 'SELECT pg_sleep(60)'"
) or die 'the command still runs on the segment';
pass('the command is canceled on the segments');

# A removed segment loses its connection
$coordinator->safe_psql('postgres',
	'DELETE FROM gp.segments WHERE segment_id = 2');
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 1 FROM gp.cluster_segments()')
  or die 'segment 2 is still in the copy';
is( $session->query_safe(
		"SELECT segment_id, status FROM gp.exec_on_all('SELECT 1')"),
	'1|SELECT 1',
	'a removed segment gets no command');
$seg2->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE application_name = 'gp coordinator'"
) or die 'the connection to a removed segment is still open';
pass('the connection to a removed segment is closed');

$session->quit;
$_->stop foreach ($seg1, $seg2, $coordinator);

done_testing();
