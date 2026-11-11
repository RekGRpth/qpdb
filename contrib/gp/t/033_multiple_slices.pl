# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Every autonomous reader of one statement imports the snapshot exported on
# its own segment before the first worker starts.  A root and its self-motion
# child are separate READ ONLY transactions with one snapshot; nested
# cancellation and the end of the statement close the holders of those exports

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
foreach my $segment_id (1 .. 3)
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
	'SELECT count(*) = 3 FROM gp.cluster_segments() WHERE interconnect_port IS NOT NULL')
  or die 'the segments did not report their interconnect ports';

sub run { return $coordinator->safe_psql('postgres', shift); }

run('CREATE TABLE t1 (a int, b text)');
run("SELECT gp.distribute('t1', ARRAY['a'])");
run('CREATE TABLE t2 (a int, c text)');
run("SELECT gp.distribute('t2', ARRAY['a'])");
run("INSERT INTO t1 VALUES (1, 'root-old')");
run("INSERT INTO t2 VALUES (1, 'peer-old')");
like($coordinator->safe_psql('postgres',
	'EXPLAIN (COSTS OFF) SELECT t1.b, t2.c FROM ONLY t1 CROSS JOIN ONLY t2'),
	qr/GpSlice/, 'a cross join runs as a slice with a broadcast motion');

# Both rows live on one segment, chosen by their actual distribution map
my $segment_id = run(<<'SQL');
SELECT gp.bucket_segment(colocation_id, gp.bucket(1024, 1))
FROM gp.distribution_policy WHERE relid = 't1'::regclass
SQL
my $segment = $segments[$segment_id - 1];
$segment->safe_psql('postgres',
	"SET gp.allow_local_ddl = on;
CREATE EXTENSION injection_points;
SELECT injection_points_attach('gp-interconnect-parent', 'wait');
SELECT injection_points_attach('gp-interconnect-reader', 'wait')");

my ($stdin, $stdout, $stderr) = ('', '', '');
my $timer = IPC::Run::timer(180);
my $session = IPC::Run::start(
	['psql', '-X', '-A', '-t', '-d',
	 $coordinator->connstr('postgres') . ' application_name=gp_multiple_slices'],
	'<', \$stdin, '>', \$stdout, '2>', \$stderr, $timer);
$stdin .= "SELECT t1.b, t2.c FROM ONLY t1 CROSS JOIN ONLY t2;\n";
$session->pump_nb while length $stdin;

# The root has imported the holder's snapshot, but has not started its child
$segment->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'gp interconnect request' AND wait_event = 'gp-interconnect-parent'")
  or BAIL_OUT('the root did not reach its motion');
my ($parent) = $segment->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE backend_type = 'gp interconnect request' AND wait_event = 'gp-interconnect-parent'")
  =~ /(\d+)/;
is($segment->safe_psql('postgres',
	"SELECT count(*) FROM pg_stat_activity WHERE application_name = 'gp snapshot' AND state = 'idle in transaction'"),
	1, 'the exported snapshot has a living holder');

# A segment-local commit is deliberately outside the coordinator's DTX lock:
# independent snapshots would return root-old joined to peer-new
$segment->safe_psql('postgres',
	"UPDATE t1 SET b = 'root-new' WHERE a = 1;
UPDATE t2 SET c = 'peer-new' WHERE a = 1");
$segment->safe_psql('postgres',
	"SELECT injection_points_wakeup('gp-interconnect-parent');
SELECT injection_points_detach('gp-interconnect-parent')");

# The self-reader is now parked beside the root that waits for its rows
$segment->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'gp interconnect request' AND wait_event = 'gp-interconnect-reader'")
  or BAIL_OUT('the self-motion reader did not appear: '
	. $segment->safe_psql('postgres',
		"SELECT pid || ':' || coalesce(wait_event_type, '?') || ':' || coalesce(wait_event, '?') || ':' || state FROM pg_stat_activity WHERE backend_type = 'gp interconnect request'")
	. " stdout=$stdout stderr=$stderr");
my ($reader) = $segment->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE backend_type = 'gp interconnect request' AND wait_event = 'gp-interconnect-reader'")
  =~ /(\d+)/;
isnt($reader, $parent, 'the root and its self-reader are different processes');
is($segment->safe_psql('postgres',
	"SELECT count(*) FROM pg_stat_activity WHERE pid IN ($parent, $reader)"),
	2, 'both slices of one segment are alive together');
$segment->safe_psql('postgres',
	"SELECT injection_points_wakeup('gp-interconnect-reader');
SELECT injection_points_detach('gp-interconnect-reader')");
$session->pump
  until $stdout =~ /root-old\|peer-old/ || $stderr =~ /ERROR/
  || $timer->is_expired;
is($stdout, "root-old|peer-old\n",
	'both readers see the state before the segment-local commit');
is($stderr, '', 'the snapshot-sharing query has no error');
$session->finish;
$segment->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE application_name = 'gp snapshot'")
  or BAIL_OUT('the snapshot holder survived the end of the statement');
pass('the end of the statement closes its snapshot holder');

# An immutable function is trusted by the planner, but the fragment transaction
# itself must still prevent a write
run('CREATE TABLE audit (a int)');
run("SELECT gp.distribute('audit', ARRAY['a'])");
run(<<'SQL');
CREATE FUNCTION gp_actual_write(a int) RETURNS int LANGUAGE plpgsql VOLATILE AS
$$BEGIN INSERT INTO audit VALUES (a); RETURN a; END$$
SQL
run(<<'SQL');
CREATE FUNCTION gp_test_write(a int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS
$$BEGIN RETURN gp_actual_write(a); END$$
SQL
my $check = $coordinator->background_psql('postgres', on_error_stop => 0);
$check->query('SELECT gp_test_write(t1.a) FROM ONLY t1 CROSS JOIN ONLY t2');
like($check->{stderr}, qr/read-only transaction/,
	'a fragment cannot write through an immutable function');
$check->{stderr} = '';
is(run('SELECT count(*) FROM audit'), 0, 'the refused fragment wrote nothing');

# Cancel after both the parent and its self-reader exist.  The coordinator's
# cancel stops the root, whose own client then stops its child
$segment->safe_psql('postgres',
	"SELECT injection_points_attach('gp-interconnect-parent', 'wait');
SELECT injection_points_attach('gp-interconnect-reader', 'wait')");
($stdin, $stdout, $stderr) = ('', '', '');
$timer = IPC::Run::timer(180);
$session = IPC::Run::start(
	['psql', '-X', '-A', '-t', '-d',
	 $coordinator->connstr('postgres') . ' application_name=gp_multiple_slices_cancel'],
	'<', \$stdin, '>', \$stdout, '2>', \$stderr, $timer);
$stdin .= "SELECT t1.b, t2.c FROM ONLY t1 CROSS JOIN ONLY t2;\n";
$session->pump_nb while length $stdin;
$segment->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'gp interconnect request' AND wait_event = 'gp-interconnect-parent'")
  or BAIL_OUT('the second root did not reach its motion');
$segment->safe_psql('postgres',
	"SELECT injection_points_wakeup('gp-interconnect-parent');
SELECT injection_points_detach('gp-interconnect-parent')");
$segment->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'gp interconnect request' AND wait_event = 'gp-interconnect-reader'")
  or BAIL_OUT('the second self-reader did not appear');
my ($coordinator_pid) = $coordinator->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE application_name = 'gp_multiple_slices_cancel'")
  =~ /(\d+)/;
is($coordinator->safe_psql('postgres',
	"SELECT pg_cancel_backend($coordinator_pid)"), 't',
	'the nested query is cancelled');
$session->pump
  until $stderr =~ /canceling statement due to user request/
  || $timer->is_expired;
like($stderr, qr/canceling statement due to user request/,
	'the nested query dies of the cancellation itself');
$session->finish;
$segment->safe_psql('postgres',
	"SELECT injection_points_wakeup('gp-interconnect-reader');
SELECT injection_points_detach('gp-interconnect-reader')");
foreach my $node (@segments)
{
	$node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'gp interconnect request'")
	  or BAIL_OUT('a request worker survived the nested cancellation');
	$node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE application_name = 'gp snapshot'")
	  or BAIL_OUT('a snapshot holder survived the nested cancellation');
}
pass('nested cancellation stops the readers and closes every holder');

# Reading first must not turn a later write of the same transaction READ ONLY,
# and the write must remain visible through the ordinary dispatched connection
$check->query_safe('BEGIN');
is($check->query_safe('SELECT t1.b, t2.c FROM ONLY t1 CROSS JOIN ONLY t2'),
	'root-new|peer-new', 'a later statement takes a fresh shared snapshot');
$check->query_safe("INSERT INTO t1 VALUES (2, 'written')");
is($check->query_safe('SELECT b FROM t1 WHERE a = 2'), 'written',
	'a read followed by a write sees its own write');
$check->query_safe('ROLLBACK');
$check->quit;

$_->stop foreach (@segments, $coordinator);

done_testing();
