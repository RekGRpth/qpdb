# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A nonrecursive CTE has a locus of its own through PlannedStmt.subplans.
# Projection, a condition, sort and limit above CteScan preserve it

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
	$query =~ s/\br\b/lr/g;
	return $query;
}

run('CREATE TABLE t (a int, b text, v numeric)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run("INSERT INTO t VALUES (1, 'x', 1), (1, 'y', 2), (2, 'x', 3), (2, 'y', 4)");
run('CREATE TABLE r (a int)');
run("SELECT gp.distribute('r', NULL)");
run('INSERT INTO r VALUES (10), (20)');
run('CREATE TABLE l AS SELECT * FROM t');
run('CREATE TABLE lr AS SELECT * FROM r');

like(plan_for('WITH s AS MATERIALIZED (SELECT a, b, v FROM t) SELECT a, b, v FROM s'),
	qr/GpSlice/, 'a nonrecursive CTE runs on the segments');
unlike(plan_for('WITH s AS MATERIALIZED (SELECT a, b, v FROM t) SELECT a, b, v FROM s'),
	qr/GpMotion/, 'a CTE needs no motion');
like(plan_for('WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a FROM s WHERE a > 1'),
	qr/GpSlice/, 'a condition above CteScan keeps its locus');
like(plan_for('WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a + 1 FROM s ORDER BY 1'),
	qr/GpSlice/, 'a projection above CteScan keeps its locus');
like(plan_for('WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a FROM s ORDER BY a LIMIT 2'),
	qr/GpSlice/, 'sort and limit stay above the CTE');
like(plan_for('WITH s AS MATERIALIZED (SELECT a FROM r) SELECT a FROM s ORDER BY a'),
	qr/GpSlice/, 'a replicated CTE uses one segment');
unlike(plan_for('WITH s AS MATERIALIZED (SELECT a FROM t) SELECT s.a, t.a FROM s, t'),
	qr/GpSlice/, 'a join of the outer query stays on the coordinator');
unlike(plan_for('WITH s AS MATERIALIZED (SELECT a FROM t), q AS (SELECT a FROM s) SELECT a FROM q'),
	qr/GpSlice/, 'two CTEs stay on the coordinator');
unlike(plan_for('WITH RECURSIVE s(a) AS (SELECT 1 UNION ALL SELECT a + 1 FROM s WHERE a < 3) SELECT a FROM s'),
	qr/GpSlice/, 'a recursive CTE stays on the coordinator');

my @queries = (
	'WITH s AS MATERIALIZED (SELECT a, b, v FROM t) SELECT a, b, v FROM s ORDER BY a, b',
	'WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a FROM s WHERE a > 1 ORDER BY a',
	'WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a + 1 FROM s ORDER BY 1',
	'WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a FROM s ORDER BY a LIMIT 2',
	'WITH s AS MATERIALIZED (SELECT a FROM r) SELECT a FROM s ORDER BY a',
	'WITH s AS MATERIALIZED (SELECT a FROM t) SELECT s.a, t.a FROM s, t ORDER BY s.a, t.a',
);
foreach my $query (@queries)
{
	is(run($query), run(local_query($query)),
		"the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
