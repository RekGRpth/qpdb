# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A query that the coordinator gives up does not leave its fragments
# running on the segments: the client of every interconnect request asks
# the broker to stop its worker, and a worker dies on the signal from
# the middle of a long fragment too, not only between two rows.  The
# query is cancelled once its worker is seen parked, so a slow runner
# that starts the worker for seconds races nothing

use strict;
use warnings FATAL => 'all';
use IPC::Run;
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
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.cluster_segments() WHERE interconnect_port IS NOT NULL')
  or die 'the segments did not report their interconnect ports';

$coordinator->safe_psql('postgres', 'CREATE TABLE t (a int)');
$coordinator->safe_psql('postgres', q{SELECT gp.distribute('t', ARRAY['a'])});
$coordinator->safe_psql('postgres',
	'INSERT INTO t SELECT g FROM generate_series(1, 10) g');

# The fragments of the next query stop before they begin, and stay there
# until the test lets them go
foreach my $segment (@segments)
{
	$segment->safe_psql('postgres',
		"SET gp.allow_local_ddl = on;
CREATE EXTENSION injection_points;
SELECT injection_points_attach('gp-interconnect-fragment', 'wait')");
}

# The query runs in a session of its own, named so the test finds it
# whatever PGAPPNAME the environment hands its psql; the timeout is only
# a safety net that keeps the session moving if the cancel goes missing
my ($stdin, $stdout, $stderr) = ('', '', '');
my $timer = IPC::Run::timer(180);
my $session = IPC::Run::start(
	['psql', '-X', '-A', '-t', '-d',
	 $coordinator->connstr('postgres') . ' application_name=gp_cancel_target'],
	'<', \$stdin, '>', \$stdout, '2>', \$stderr, $timer);
$stdin .= "SET statement_timeout = '10s';\nSELECT count(*) FROM t;\n";
$session->pump_nb while length $stdin;

# The query parks its worker on the first segment: the cancel comes
# after the fragments hold it, however long the runner took to get there
$coordinator->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE application_name = 'gp_cancel_target' AND state = 'active'")
  or BAIL_OUT('the query never reached the coordinator');
$segments[0]->poll_query_until('postgres',
	"SELECT count(*) > 0 FROM pg_stat_activity WHERE backend_type = 'gp interconnect request'")
  or BAIL_OUT('no worker of the query appeared on the first segment');
my ($pid) = $coordinator->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE application_name = 'gp_cancel_target'")
  =~ /(\d+)/;
my $wait = $coordinator->safe_psql('postgres',
	"SELECT wait_event_type || ':' || wait_event FROM pg_stat_activity WHERE pid = $pid");
my $cancelled = $coordinator->safe_psql('postgres',
	"SELECT pg_cancel_backend($pid)");
is($cancelled, 't', "the cancel was sent (wait was $wait)")
  or BAIL_OUT("pg_cancel_backend($pid) failed while the target waited $wait");
$session->pump
  until $stderr =~ qr/canceling statement due to user request/
  || $timer->is_expired;
like($stderr, qr/canceling statement due to user request/,
	'the cancelled query dies of the cancel itself');

# The workers of the dead query are gone, though no row ever came to
# tell them so; a leftover is named, with what it waits for
foreach my $segment (@segments)
{
	$segment->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'gp interconnect request'")
	  or die 'the worker of the cancelled query is still running: '
		. $segment->safe_psql('postgres',
			"SELECT pid || ':' || coalesce(wait_event_type, '?') || ':' || coalesce(wait_event, '?') || ':' || state FROM pg_stat_activity WHERE backend_type = 'gp interconnect request'");
}
pass('the segments stop the workers of a cancelled query');

# A next query runs as before, through the same brokers and slots
foreach my $segment (@segments)
{
	$segment->safe_psql('postgres',
		"SELECT injection_points_detach('gp-interconnect-fragment')");
}
is($coordinator->safe_psql('postgres', 'SELECT count(*) FROM t'), 10,
	'a query after the cancellation reads the same rows');
$session->finish;

$_->stop foreach (@segments, $coordinator);

done_testing();
