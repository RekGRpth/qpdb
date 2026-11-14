# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Correlated SQL can run locally on each segment when every distributed input
# is connected by the whole distribution key.  Uncorrelated or noncolocated
# inputs stay on the coordinator

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

run('CREATE TABLE t1 (a int, v numeric)');
run("SELECT gp.distribute('t1', ARRAY['a'])");
run('INSERT INTO t1 SELECT i, i * 1.5 FROM generate_series(1, 100) i');
run('CREATE TABLE t2 (a int, v numeric)');
run("SELECT gp.distribute('t2', ARRAY['a'], colocate_with => 't1')");
run('INSERT INTO t2 SELECT i, i * 2.0 FROM generate_series(41, 120) i');
run('CREATE TABLE r (a int, v numeric)');
run("SELECT gp.distribute('r', NULL)");
run('INSERT INTO r SELECT i, i FROM generate_series(1, 100) i');
run('CREATE TABLE u (a int, v numeric)');
run("SELECT gp.distribute('u', ARRAY['a'], colocate_with => 't1')");
run('INSERT INTO u SELECT i, i FROM generate_series(1, 100) i');
run(<<'SQL');
INSERT INTO gp.colocation_groups (key_types, nbuckets, map)
SELECT key_types, nbuckets,
       (SELECT array_agg(map[(i % 1024) + 1] ORDER BY i)
        FROM generate_series(1, 1024) i)
FROM gp.colocation_groups
WHERE colocation_id = (SELECT colocation_id FROM gp.distribution_policy
                       WHERE relid = 't1'::regclass)
SQL
run("UPDATE gp.distribution_policy SET colocation_id = (SELECT max(colocation_id) FROM gp.colocation_groups) WHERE relid = 'u'::regclass");
run("SELECT gp.policy_changed('u')");
run('CREATE TABLE l1 AS SELECT * FROM t1');
run('CREATE TABLE l2 AS SELECT * FROM t2');
run('CREATE TABLE lr AS SELECT * FROM r');
run('CREATE TABLE lu AS SELECT * FROM u');

like(plan_for('SELECT t1.a FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.a = t1.a)'),
	qr/GpSlice/, 'a correlated EXISTS with the full key is local');
like(plan_for('SELECT t1.a, (SELECT max(t2.v) FROM t2 WHERE t2.a = t1.a) FROM t1'),
	qr/GpSlice/, 'a correlated scalar aggregate sees its whole key on one segment');
like(plan_for('SELECT t1.a, count(*) FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.a = t1.a) GROUP BY t1.a'),
	qr/GpSlice/, 'an outer aggregate can follow a correlated filter');
like(plan_for('SELECT t1.a FROM t1 WHERE EXISTS (SELECT 1 FROM r WHERE r.a = t1.a)'),
	qr/GpSlice/, 'a replicated subquery is available on every segment');
like(plan_for('SELECT t1.a, (SELECT count(*) FROM r) FROM t1'),
	qr/GpSlice/, 'an uncorrelated replicated scalar subquery is safe');
unlike(plan_for('SELECT t1.a, (SELECT count(*) FROM t2) FROM t1'),
	qr/GpSlice/, 'an uncorrelated hashed scalar subquery stays on the coordinator');
unlike(plan_for('SELECT t1.a FROM t1 WHERE EXISTS (SELECT 1 FROM u WHERE u.a = t1.a)'),
	qr/GpSlice/, 'a noncolocated correlated input stays on the coordinator');
unlike(plan_for('SELECT t1.a FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.a = t1.a AND EXISTS (SELECT 1 FROM t2 t2x WHERE t2x.a = t2.a))'),
	qr/GpSlice/, 'nested correlated levels stay on the coordinator');

run('SET enable_mergejoin = off');
my @queries = (
	'SELECT t1.a FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.a = t1.a) ORDER BY t1.a',
	'SELECT t1.a, (SELECT max(t2.v) FROM t2 WHERE t2.a = t1.a) FROM t1 ORDER BY t1.a',
	'SELECT t1.a, count(*) FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.a = t1.a) GROUP BY t1.a ORDER BY t1.a',
	'SELECT t1.a FROM t1 WHERE EXISTS (SELECT 1 FROM r WHERE r.a = t1.a) ORDER BY t1.a',
	'SELECT t1.a, (SELECT count(*) FROM r) FROM t1 ORDER BY t1.a',
	'SELECT t1.a, (SELECT count(*) FROM t2) FROM t1 ORDER BY t1.a',
	'SELECT t1.a FROM t1 WHERE EXISTS (SELECT 1 FROM u WHERE u.a = t1.a) ORDER BY t1.a',
	'SELECT t1.a FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.a = t1.a AND EXISTS (SELECT 1 FROM t2 t2x WHERE t2x.a = t2.a)) ORDER BY t1.a',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bt1\b/l1/g;
	$local =~ s/\bt2\b/l2/g;
	$local =~ s/\br\b/lr/g;
	$local =~ s/\bu\b/lu/g;
	is(run($query), run($local), "the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
