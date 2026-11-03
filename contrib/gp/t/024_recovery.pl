# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The recovery of the transactions the segments keep prepared: a segment
# lost during COMMIT PREPARED is committed by the worker of the
# coordinator, and a coordinator lost between prepare and commit leaves
# the segments rolling their transactions back

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
	'SELECT count(*) = 3 FROM gp.cluster_segments()')
  or die 'segments did not register';

sub run { return $coordinator->safe_psql('postgres', shift); }

# Whether a segment keeps a transaction of ours prepared
sub prepared
{
	my ($node) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_prepared_xacts WHERE gid LIKE 'gp\\_%'");
}

sub nothing_prepared
{
	foreach my $node (@segments)
	{
		is(prepared($node), 0, 'no prepared transaction on ' . $node->name);
	}
}

# The rows of t on one segment
sub rows_on
{
	my ($node) = @_;
	return $node->safe_psql('postgres', 'SELECT count(*) FROM t');
}

SKIP:
{
	skip 'injection points are not supported by this build', 12
	  unless $ENV{enable_injection_points} eq 'yes';

	run('CREATE TABLE t (a int)');
	run("SELECT gp.distribute('t', ARRAY['a'])");
	run('INSERT INTO t VALUES (1), (2), (3)');
	is(rows_on($_), 1, 'a row of the base on ' . $_->name) foreach @segments;

	# A segment lost during COMMIT PREPARED: the worker of the coordinator
	# commits what the clog says committed
	$coordinator->safe_psql('postgres',
		"CREATE EXTENSION injection_points;
SELECT injection_points_attach('gp-before-commit-prepared', 'wait')");
	my $session = $coordinator->background_psql('postgres', on_error_stop => 0);
	$session->query_until(qr/start/, "\\echo start\nINSERT INTO t VALUES (1), (2), (3);\n");
	$coordinator->wait_for_event('client backend', 'gp-before-commit-prepared');
	is(prepared($segments[1]), 1, 'segment 2 keeps the transaction prepared');

	$segments[1]->stop;
	$coordinator->safe_psql('postgres',
		"SELECT injection_points_detach('gp-before-commit-prepared');
SELECT injection_points_wakeup('gp-before-commit-prepared')");
	$session->query('SELECT 1');
	like(
		$session->{stderr},
		qr/WARNING:  could not run COMMIT PREPARED 'gp_\d+' on segment 2/,
		'the commit warns about the lost segment');
	$session->quit;

	$segments[1]->start;
	$coordinator->poll_query_until('postgres',
		'SELECT count(*) = 3 FROM gp.cluster_segments()')
	  or die 'segment 2 did not register again';

	# The worker resolves it: no prepared transactions, the row is there
	foreach my $node (@segments)
	{
		$node->poll_query_until('postgres',
			"SELECT count(*) = 0 FROM pg_prepared_xacts WHERE gid LIKE 'gp\\_%'")
		  or die 'the transactions stay prepared on ' . $node->name;
	}
	nothing_prepared();
	is(rows_on($_), 2, 'the committed rows are on ' . $_->name) foreach @segments;

	# A coordinator lost between the prepare of the segments and its own
	# commit: the crash takes its transaction with it, and the segments
	# roll what they keep back
	$coordinator->safe_psql('postgres',
		"SELECT injection_points_attach('gp-after-prepare-transaction', 'wait')");
	$session = $coordinator->background_psql('postgres', on_error_stop => 0);
	$session->query_until(qr/start/, "\\echo start\nINSERT INTO t VALUES (4), (5), (6);\n");
	$coordinator->wait_for_event('client backend', 'gp-after-prepare-transaction');
	is(scalar(() = grep { prepared($_) eq '1' } @segments), 3,
		'every segment keeps the transaction prepared');

	$coordinator->stop('immediate');
	$coordinator->start;
	$coordinator->poll_query_until('postgres',
		'SELECT count(*) = 3 FROM gp.cluster_segments()')
	  or die 'the segments did not register on the new coordinator';

	# The crash lost the fate of the transaction: the number of it still
	# looks like one of a transaction to come, and nothing new runs on the
	# restarted coordinator to tell otherwise.  A transaction that rolls
	# back moves past the number and settles the fate the safe way
	run("BEGIN; CREATE TEMP TABLE settle (a int); ROLLBACK");

	foreach my $node (@segments)
	{
		$node->poll_query_until('postgres',
			"SELECT count(*) = 0 FROM pg_prepared_xacts WHERE gid LIKE 'gp\\_%'")
		  or die 'the transactions stay prepared on ' . $node->name;
	}
	nothing_prepared();
	is(rows_on($_), 2, 'the lost transaction rolled back on ' . $_->name)
	  foreach @segments;
	is(run('SELECT count(*) FROM t'), 6, 'and the coordinator sees the same');
}

# An immediate stop: the test is over, and on Windows the coordinator,
# crash-restarted above, takes its time letting its last children go,
# which a fast stop waits out and fails on
$_->stop('immediate') foreach (@segments, $coordinator);

done_testing();
