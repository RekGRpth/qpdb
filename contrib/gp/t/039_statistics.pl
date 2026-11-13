# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# ANALYZE reads the real rows on the segments.  The coordinator sums relation
# sizes, samples attribute statistics from one segment, and uses both in plans

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

run('CREATE TABLE t (a int, b int)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run('INSERT INTO t SELECT i, i % 10 FROM generate_series(1, 100) i');
run('CREATE TABLE r (a int)');
run("SELECT gp.distribute('r', NULL)");
run('INSERT INTO r VALUES (10), (10), (20), (30)');
run('CREATE TABLE l (a int)');
run('INSERT INTO l VALUES (1), (2)');

run('ANALYZE t, r, l');
is(run("SELECT relpages > 0 AND reltuples = 100 FROM pg_class WHERE oid = 't'::regclass"),
	't', 'hashed relation sizes are summed over the segments');
is(run("SELECT reltuples = 4 FROM pg_class WHERE oid = 'r'::regclass"),
	't', 'a replicated relation is counted once');
is(run("SELECT reltuples = 2 FROM pg_class WHERE oid = 'l'::regclass"),
	't', 'a mixed ANALYZE keeps local statistics local');
is(run("SELECT null_frac = 0 AND n_distinct <> 0 FROM pg_stats WHERE tablename = 't' AND attname = 'b'"),
	't', 'attribute statistics reach the coordinator');

my $attribute_stats = "SELECT null_frac, avg_width, n_distinct, most_common_freqs
FROM pg_stats WHERE tablename = 't' AND attname = 'a'";
is(run($attribute_stats), $segments[0]->safe_psql('postgres', $attribute_stats),
	'the attribute sample comes from the first segment');
like($coordinator->safe_psql('postgres', 'EXPLAIN SELECT * FROM t'),
	qr/rows=100/, 'the planner uses the number of rows of the cluster');
like($coordinator->safe_psql('postgres', 'EXPLAIN SELECT * FROM r WHERE a = 10'),
	qr/rows=2/, 'restriction selectivity uses the imported statistics');

run('ANALYZE (SKIP_LOCKED true) t(a)');
is(run("SELECT n_distinct = -1 FROM pg_stats WHERE tablename = 't' AND attname = 'a'"),
	't', 'ANALYZE options and a column list survive dispatch');
run('INSERT INTO t SELECT i, i % 10 FROM generate_series(101, 105) i');
run('ANALYZE t');
is(run("SELECT reltuples = 105 FROM pg_class WHERE oid = 't'::regclass"),
	't', 'a later ANALYZE refreshes the cluster row count');

run('ANALYZE');
is(run("SELECT reltuples FROM pg_class WHERE oid = 't'::regclass"),
	105, 'ANALYZE without a relation list covers distributed tables');
is(run('SELECT count(*) FROM t'), '105',
	'statistics collection does not change the answer');

$_->stop foreach (@segments, $coordinator);

done_testing();
