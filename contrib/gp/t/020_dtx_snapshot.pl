# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The distributed snapshot: a query that reads the segments takes its
# snapshots while no distributed transaction commits, so it sees one in
# full or not at all, never on some segments only

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

# Compare a query on the coordinator and on each segment
sub same
{
	my ($query, $test) = @_;
	my $expected = run($query);

	$test //= 'the segments answer';
	is($_->safe_psql('postgres', $query), $expected, "$test on " . $_->name)
	  foreach @segments;
	return $expected;
}

# What each segment has, in their order
sub on_segments
{
	return join ' ',
	  map { $_->safe_psql('postgres', 'SELECT count(*) FROM t') } @segments;
}

run('CREATE TABLE t (a int)');
run("SELECT gp.distribute('t', ARRAY['a'])");

# A transaction held between the commit of the coordinator and COMMIT
# PREPARED is invisible in full, and after it finishes visible in full
SKIP:
{
	skip 'injection points are not supported by this build', 5
	  unless $ENV{enable_injection_points} eq 'yes';

	$coordinator->safe_psql('postgres',
		"CREATE EXTENSION injection_points;
SELECT injection_points_attach('gp-before-commit-prepared', 'wait')");
	my $session = $coordinator->background_psql('postgres', on_error_stop => 0);
	$session->query_until(qr/start/,
		"\\echo start\nINSERT INTO t SELECT g FROM generate_series(1, 300) g;\n");
	$coordinator->wait_for_event('client backend', 'gp-before-commit-prepared');

	# The segments keep the transaction prepared: it is on none of them
	is(on_segments(), '0 0 0', 'the segments do not have the rows');
	is(scalar(() = grep { $_->safe_psql('postgres',
			"SELECT count(*) FROM pg_prepared_xacts WHERE gid ~ '^gp_'") } @segments),
		3, 'every segment keeps the transaction prepared');

	# A reader waits for the commit to finish rather than see a half of
	# it, and then sees it in full
	my $reader = $coordinator->background_psql('postgres', on_error_stop => 1);
	$reader->query_until(qr/start/,
		"\\echo start\nSELECT count(*) FROM t;\n");
	$coordinator->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'gp dtx'")
	  or die 'the reader does not wait for the committing transaction';

	$coordinator->safe_psql('postgres',
		"SELECT injection_points_detach('gp-before-commit-prepared');
SELECT injection_points_wakeup('gp-before-commit-prepared')");
	$reader->quit;
	ok(1, 'the reader finished after the commit');
	$session->quit;

	is(run('SELECT count(*) FROM t'), 300, 'visible in full after the commit');
	my @counts = map { $_->safe_psql('postgres', 'SELECT count(*) FROM t') } @segments;
	cmp_ok($counts[0] + $counts[1] + $counts[2], '==', 300,
		'the segments add up to the whole');
}

# Concurrent writers and one reader: the reader never sees a half of a
# transaction.  Without the lock it would race with COMMIT PREPARED of
# several transactions at once.
run('TRUNCATE t');
my @writers;
for my $i (1 .. 4)
{
	push @writers, $coordinator->background_psql('postgres',
		on_error_stop => 1);
	$writers[-1]->query_until(qr/start/,
		"\\echo start\nINSERT INTO t SELECT g * 10 + $i FROM generate_series(1, 500) g;\n");
}
run('SELECT 1');	# let them run into their commits
my $counts;
for my $i (1 .. 200)
{
	my $count = run('SELECT count(*) FROM t');
	like($count, qr/^\d+$/, "read $i has a count");
	$counts = $count if $count > ($counts // 0);
}
$_->quit foreach @writers;
my $final = run('SELECT count(*) FROM t');
is($final, 2000, 'all writers committed');
my @seg_counts = map { $_->safe_psql('postgres', 'SELECT count(*) FROM t') } @segments;
cmp_ok($seg_counts[0] + $seg_counts[1] + $seg_counts[2], '==', 2000,
	'the segments add up');

# Counts seen during the run are multiples of 500: a transaction shows
# in full or not at all
cmp_ok($counts % 500, '==', 0, "the largest seen count ($counts) is whole transactions");

$_->stop foreach (@segments, $coordinator);

done_testing();
