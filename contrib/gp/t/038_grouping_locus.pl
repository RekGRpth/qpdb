# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# GROUPING SETS whose every set contains the whole distribution key keep each
# group on one segment.  Empty sets and prefixes stay on the coordinator

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

run('CREATE TABLE t (a int, v numeric)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run("INSERT INTO t VALUES (1, 10), (1, 20), (2, 30), (2, 40), (3, 50)");
run('CREATE TABLE m (a int, b int, v numeric)');
run("SELECT gp.distribute('m', ARRAY['a', 'b'])");
run("INSERT INTO m VALUES (1, 2, 10), (1, 2, 20), (1, 3, 30), (2, 1, 40)");
run('CREATE TABLE r (a int)');
run("SELECT gp.distribute('r', NULL)");
run('INSERT INTO r VALUES (10), (10), (20), (30)');
run('CREATE TABLE l AS SELECT * FROM t');
run('CREATE TABLE lm AS SELECT * FROM m');
run('CREATE TABLE lr AS SELECT * FROM r');

like(plan_for('SELECT a, count(*) FROM t GROUP BY GROUPING SETS ((a))'),
	qr/GpSlice/, 'a nonempty grouping set with the key is local');
unlike(plan_for('SELECT a, count(*) FROM t GROUP BY GROUPING SETS ((a))'),
	qr/GpMotion/, 'a grouping set with the key needs no motion');
like(plan_for('SELECT a, sum(v) FROM t GROUP BY GROUPING SETS ((a))'),
	qr/GpSlice/, 'a grouping-set aggregate is local');
like(plan_for('SELECT a, b, count(*) FROM m GROUP BY GROUPING SETS ((a, b))'),
	qr/GpSlice/, 'a compound key keeps grouping sets local');
like(plan_for('SELECT a, b, count(*) FROM m GROUP BY GROUPING SETS ((a, b), (a, b, v))'),
	qr/GpSlice/, 'different grouping sets can all contain a compound key');
unlike(plan_for('SELECT a, count(*) FROM m GROUP BY GROUPING SETS ((a, b), (a))'),
	qr/GpSlice/, 'a prefix of a compound key stays on the coordinator');
unlike(plan_for('SELECT a, count(*) FROM t GROUP BY GROUPING SETS ((a), ())'),
	qr/GpSlice/, 'an empty grouping set stays on the coordinator');
unlike(plan_for('SELECT a, count(*) FROM t GROUP BY ROLLUP(a)'),
	qr/GpSlice/, 'ROLLUP has an empty set and stays on the coordinator');
unlike(plan_for('SELECT a, count(*) FROM t GROUP BY CUBE(a)'),
	qr/GpSlice/, 'CUBE stays on the coordinator');
like(plan_for('SELECT a, count(*) FROM t GROUP BY GROUPING SETS ((a), (a))'),
	qr/GpSlice/, 'duplicate grouping sets remain local');
like(plan_for('SELECT a, count(*) FROM t GROUP BY GROUPING SETS ((a)) ORDER BY a LIMIT 1'),
	qr/GpSlice/, 'sort and limit stay above grouping sets');
like(plan_for('SELECT a, count(*) FROM r GROUP BY GROUPING SETS ((a))'),
	qr/GpSlice/, 'a replicated grouping set uses one segment');

my @queries = (
	'SELECT a, count(*) FROM t GROUP BY GROUPING SETS ((a)) ORDER BY a',
	'SELECT a, sum(v) FROM t GROUP BY GROUPING SETS ((a)) ORDER BY a',
	'SELECT a, b, count(*) FROM m GROUP BY GROUPING SETS ((a, b)) ORDER BY a, b',
	'SELECT a, b, count(*) FROM m GROUP BY GROUPING SETS ((a, b), (a, b, v)) ORDER BY a, b, count(*)',
	'SELECT a, count(*) FROM m GROUP BY GROUPING SETS ((a, b), (a)) ORDER BY a',
	'SELECT a, count(*) FROM t GROUP BY GROUPING SETS ((a), ()) ORDER BY a',
	'SELECT a, count(*) FROM t GROUP BY ROLLUP(a) ORDER BY a',
	'SELECT a, count(*) FROM t GROUP BY CUBE(a) ORDER BY a',
	'SELECT a, count(*) FROM t GROUP BY GROUPING SETS ((a), (a)) ORDER BY a',
	'SELECT a, count(*) FROM t GROUP BY GROUPING SETS ((a)) ORDER BY a LIMIT 1',
	'SELECT a, count(*) FROM r GROUP BY GROUPING SETS ((a)) ORDER BY a',
);
foreach my $query (@queries)
{
	is(run($query), run(local_query($query)),
		"the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
