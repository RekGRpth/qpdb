# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A subquery in FROM has a locus of its own: an aggregate of a distributed
# table grouped by the key, or a colocated join, runs on the segments even
# when it sits inside a subquery of a query that is not colocated itself

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
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }

run('CREATE TABLE t (a int, b text, c numeric)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run('CREATE TABLE u (a int, d text)');
run("SELECT gp.distribute('u', ARRAY['a'])");
run("INSERT INTO t SELECT g % 10, 'x' || g, g * 1.5 FROM generate_series(1, 1000) g");
run("INSERT INTO u SELECT g, 'd' || g FROM generate_series(41, 120) g");
# The same rows in tables of the coordinator, to compare the answers with
run('CREATE TABLE l AS SELECT * FROM t');
run('CREATE TABLE lu AS SELECT * FROM u');

like(plan_for('SELECT s.a, s.cnt FROM (SELECT a, count(*) AS cnt FROM t GROUP BY a) s ORDER BY s.a LIMIT 5'),
	qr/GpSlice/, 'an aggregate subquery runs on the segments');
like(plan_for('SELECT s.a, s.cnt FROM (SELECT a, count(*) AS cnt FROM t GROUP BY a) s LIMIT 5'),
	qr/GpSlice/, 'an aggregate subquery without a sort of the outer query is pushed');
like(plan_for('SELECT s.a, s.cnt FROM (SELECT t.a, count(*) AS cnt FROM t JOIN u ON t.a = u.a GROUP BY t.a) s LIMIT 5'),
	qr/GpSlice/, 'a colocated join inside a subquery is pushed');
unlike(plan_for('SELECT * FROM (SELECT a, count(*) FROM t GROUP BY a LIMIT 3) s'),
	qr/GpSlice/, 'a limit inside the subquery stops the pushdown');
unlike(plan_for('SELECT s.a FROM (SELECT a, count(*) AS cnt FROM t GROUP BY a) s WHERE s.cnt > 900'),
	qr/GpSlice/, 'a condition of the outer query stops the pushdown');
unlike(plan_for('SELECT u.d, s.cnt FROM u JOIN (SELECT a, count(*) AS cnt FROM t GROUP BY a) s ON u.a = s.a'),
	qr/GpSlice/, 'a subquery joined to a table stays on the coordinator');

my @queries = (
	'SELECT s.a, s.cnt FROM (SELECT a, count(*) AS cnt FROM t GROUP BY a) s ORDER BY s.a LIMIT 5',
	'SELECT sum(s.c) FROM (SELECT a, sum(c) AS c FROM t GROUP BY a) s',
	'SELECT s.a, s.cnt FROM (SELECT t.a, count(*) AS cnt FROM t JOIN u ON t.a = u.a GROUP BY t.a) s ORDER BY s.a LIMIT 5',
	'SELECT s.a, s.c, s.cnt FROM (SELECT t.a, u.d AS c, count(*) AS cnt FROM t JOIN u ON t.a = u.a GROUP BY t.a, u.d) s ORDER BY s.a, s.c LIMIT 5',
	'SELECT count(*) FROM (SELECT a, count(*) AS cnt FROM t GROUP BY a) s',
	'SELECT s.cnt + 1 FROM (SELECT a, count(*) AS cnt FROM t GROUP BY a) s ORDER BY 1 LIMIT 3',
	'SELECT s.a FROM (SELECT a, count(*) AS cnt FROM t GROUP BY a) s WHERE s.cnt = 100 ORDER BY s.a LIMIT 4',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bt\b/l/g;
	$local =~ s/\bu\b/lu/g;
	is(run($query), run($local), "the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
