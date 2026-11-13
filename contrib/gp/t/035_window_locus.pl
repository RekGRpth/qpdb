# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A window partition by the whole distribution key is local to its segment.
# Other partition keys stay on the coordinator; Sort and Limit stay above

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
	'SELECT count(*) = 3 FROM gp.cluster_segments() WHERE interconnect_port IS NOT NULL')
  or die 'the segments did not report their interconnect ports';

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }
sub local_query
{
	my ($query) = @_;

	$query =~ s/\bt\b/l/g;
	$query =~ s/\bm\b/lm/g;
	$query =~ s/\br\b/lr/g;
	return $query;
}

run('CREATE TABLE t (a int, b text, v numeric)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run("INSERT INTO t VALUES (1, 'x', 1), (1, 'y', 2), (2, 'x', 3),
	(2, 'y', 4), (3, 'x', 5), (3, 'y', 6)");
run('CREATE TABLE m (a int, b int, v numeric)');
run("SELECT gp.distribute('m', ARRAY['a', 'b'])");
run("INSERT INTO m VALUES (1, 2, 10), (1, 2, 20), (2, 1, 30), (2, 1, 40)");
run('CREATE TABLE r (a int, v int)');
run("SELECT gp.distribute('r', NULL)");
run('INSERT INTO r VALUES (1, 1), (1, 2), (2, 3)');
run('CREATE TABLE l AS SELECT * FROM t');
run('CREATE TABLE lm AS SELECT * FROM m');
run('CREATE TABLE lr AS SELECT * FROM r');

like(plan_for('SELECT a, row_number() OVER (PARTITION BY a ORDER BY v) FROM t'),
	qr/GpSlice/, 'a partition by the distribution key is local');
unlike(plan_for('SELECT a, row_number() OVER (PARTITION BY a ORDER BY v) FROM t'),
	qr/GpMotion/, 'a local window needs no motion');
like(plan_for('SELECT a, sum(v) OVER (PARTITION BY a ORDER BY v) FROM t'),
	qr/GpSlice/, 'a window aggregate is local too');
like(plan_for('SELECT a, rank() OVER (PARTITION BY a ORDER BY v) FROM t'),
	qr/GpSlice/, 'rank uses the same partition locus');
unlike(plan_for('SELECT a, row_number() OVER (PARTITION BY b ORDER BY v) FROM t'),
	qr/GpSlice/, 'another partition key stays on the coordinator');
like(plan_for('SELECT a, b, row_number() OVER (PARTITION BY a, b ORDER BY v) FROM m'),
	qr/GpSlice/, 'the whole compound key partitions locally');
like(plan_for('SELECT a, rank() OVER (PARTITION BY a ORDER BY v),
	rank() OVER (PARTITION BY a ORDER BY v DESC) FROM t'),
	qr/GpSlice/, 'multiple windows with the same partition stay local');
unlike(plan_for('SELECT a, row_number() OVER (PARTITION BY a ORDER BY v),
	row_number() OVER (PARTITION BY b ORDER BY v) FROM t'),
	qr/GpSlice/, 'different partitions stop the pushdown');
like(plan_for('SELECT a, row_number() OVER (PARTITION BY a ORDER BY v) FROM r'),
	qr/GpSlice/, 'a replicated window uses one segment');
like(plan_for('SELECT a, row_number() OVER (PARTITION BY a ORDER BY v) FROM t ORDER BY a, b LIMIT 2'),
	qr/GpSlice/, 'sort and limit stay above the window');

my @queries = (
	'SELECT a, b, v, row_number() OVER (PARTITION BY a ORDER BY v) FROM t ORDER BY a, v',
	'SELECT a, b, v, sum(v) OVER (PARTITION BY a ORDER BY v) FROM t ORDER BY a, v',
	'SELECT a, b, v, rank() OVER (PARTITION BY a ORDER BY v) FROM t ORDER BY a, v',
	'SELECT a, b, v, row_number() OVER (PARTITION BY b ORDER BY v) FROM t ORDER BY b, v',
	'SELECT a, b, v, row_number() OVER (PARTITION BY a, b ORDER BY v) FROM m ORDER BY a, b, v',
	'SELECT a, b, v, rank() OVER (PARTITION BY a ORDER BY v),
	 rank() OVER (PARTITION BY a ORDER BY v DESC) FROM t ORDER BY a, v',
	'SELECT a, b, v, row_number() OVER (PARTITION BY a ORDER BY v),
	 row_number() OVER (PARTITION BY b ORDER BY v) FROM t ORDER BY a, b, v',
	'SELECT a, v, row_number() OVER (PARTITION BY a ORDER BY v) FROM r ORDER BY a, v',
	'SELECT a, b, v, row_number() OVER (PARTITION BY a ORDER BY v) FROM t ORDER BY a, b LIMIT 2',
);
foreach my $query (@queries)
{
	is(run($query), run(local_query($query)),
		"the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
