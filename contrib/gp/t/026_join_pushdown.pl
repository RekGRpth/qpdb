# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A join whose partners are on the same segment runs on the segments: two
# tables of one colocation group joined by the whole key of the
# distribution, or a replicated table joined to anything, or two
# replicated tables.  The coordinator collects the rows of the join

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

# Two tables of one colocation group (the same key type), two replicated
run('CREATE TABLE j1 (a int, b text)');
run("SELECT gp.distribute('j1', ARRAY['a'])");
run('CREATE TABLE j2 (a int, c text)');
run("SELECT gp.distribute('j2', ARRAY['a'])");
run('CREATE TABLE j3 (a int, e text)');
run("SELECT gp.distribute('j3', ARRAY['a'])");
run('CREATE TABLE r1 (a int, d text)');
run("SELECT gp.distribute('r1', NULL)");
run('CREATE TABLE r2 (a int, e text)');
run("SELECT gp.distribute('r2', NULL)");
run("INSERT INTO j1 SELECT g, 'b' || g FROM generate_series(1, 100) g");
run("INSERT INTO j2 SELECT g, 'c' || g FROM generate_series(41, 120) g");
run("INSERT INTO j3 SELECT g, 'e' || g FROM generate_series(50, 110) g");
run("INSERT INTO r1 SELECT g, 'd' || g FROM generate_series(1, 100) g");
run("INSERT INTO r2 SELECT g, 'e' || g FROM generate_series(1, 100) g");
# The same rows in tables of the coordinator, to compare the answers with
run('CREATE TABLE l1 AS SELECT * FROM j1');
run('CREATE TABLE l2 AS SELECT * FROM j2');
run('CREATE TABLE l3 AS SELECT * FROM j3');
run('CREATE TABLE lr1 AS SELECT * FROM r1');
run('CREATE TABLE lr2 AS SELECT * FROM r2');

like(plan_for('SELECT j1.a, j2.c FROM j1 JOIN j2 ON j1.a = j2.a'), qr/GpSlice/,
	'a join by the key of one colocation group runs on the segments');
like(plan_for('SELECT j1.a, j2.c FROM j1 LEFT JOIN j2 ON j1.a = j2.a'), qr/GpSlice/,
	'a left join by the key is pushed');
like(plan_for('SELECT j1.a, j2.a FROM j1 RIGHT JOIN j2 ON j1.a = j2.a'), qr/GpSlice/,
	'a right join by the key is pushed');
like(plan_for('SELECT j1.a, j2.a FROM j1 FULL JOIN j2 ON j1.a = j2.a'), qr/GpSlice/,
	'a full join by the key is pushed');
like(plan_for('SELECT j1.a, r1.d FROM j1 JOIN r1 ON j1.a = r1.a'), qr/GpSlice/,
	'a replicated table joins a hashed one on the segments');
like(plan_for('SELECT r1.a, r2.e FROM r1 JOIN r2 ON r1.a = r2.a'), qr/GpSlice/,
	'two replicated tables join on one segment');
like(plan_for('SELECT j1.a, j2.c FROM j1 JOIN j2 ON j1.a = j2.a AND j1.b = j2.c'),
	qr/GpSlice/, 'a condition on top of the key is a filter, not a redistribution');
unlike(plan_for('SELECT j1.a, j2.c FROM j1 JOIN j2 ON j1.b = j2.c'), qr/GpSlice/,
	'a join not by the key is not pushed');
like(plan_for('SELECT j1.a, count(*) FROM j1 JOIN j2 ON j1.a = j2.a GROUP BY j1.a'),
	qr/GpSlice/, 'an aggregate over a colocated join by the key is pushed');
unlike(plan_for('SELECT j1.a, count(*) FROM j1 JOIN j2 ON j1.a = j2.a GROUP BY j1.a ORDER BY j1.a'),
	qr/GpSlice/,
	'a sorted aggregate over a join that answers the order by itself is not pushed');
unlike(plan_for('SELECT j1.b, count(*) FROM j1 JOIN j2 ON j1.a = j2.a GROUP BY j1.b'),
	qr/GpSlice/, 'an aggregate over a join not by the key is not pushed');
unlike(plan_for('SELECT count(*) FROM j1 JOIN j2 ON j1.a = j2.a'), qr/GpSlice/,
	'a global aggregate over a join is not pushed');
like(plan_for('SELECT j1.a, r1.d, count(*) FROM j1 JOIN r1 ON j1.a = r1.a GROUP BY j1.a, r1.d'),
	qr/GpSlice/, 'an aggregate over a hashed and a replicated table is pushed');
like(plan_for('SELECT r1.a, count(*) FROM r1 JOIN r2 ON r1.a = r2.a GROUP BY r1.a'),
	qr/GpSlice/, 'an aggregate over two replicated tables is pushed');
like(plan_for('SELECT j1.a, j2.c, j3.e FROM j1 JOIN j2 ON j1.a = j2.a JOIN j3 ON j2.a = j3.a'),
	qr/GpSlice/, 'a join of three tables of one colocation group is pushed');
like(plan_for('SELECT j1.a, j2.c, r1.d FROM j1 JOIN j2 ON j1.a = j2.a JOIN r1 ON j2.a = r1.a'),
	qr/GpSlice/, 'a join of two hashed tables with a replicated one is pushed');
unlike(plan_for('SELECT j1.a, j2.c, j3.e FROM j1 JOIN j2 ON j1.a = j2.a JOIN j3 ON j1.b = j3.e'),
	qr/GpSlice/, 'a join with a table not joined by the key is not pushed');
like(plan_for('SELECT j1.a, count(*) FROM j1 JOIN j2 ON j1.a = j2.a JOIN j3 ON j2.a = j3.a GROUP BY j1.a'),
	qr/GpSlice/, 'an aggregate over a three-way colocated join is pushed');

my @queries = (
	'SELECT j1.a, j1.b, j2.c FROM j1 JOIN j2 ON j1.a = j2.a ORDER BY j1.a',
	'SELECT j1.a, j1.b, j2.c FROM j1 LEFT JOIN j2 ON j1.a = j2.a ORDER BY j1.a',
	'SELECT j1.a, j2.a, j2.c FROM j1 RIGHT JOIN j2 ON j1.a = j2.a ORDER BY j2.a',
	'SELECT j1.a, j2.a, j1.b, j2.c FROM j1 FULL JOIN j2 ON j1.a = j2.a ORDER BY j1.a, j2.a',
	'SELECT j1.a, j1.b, j2.c FROM j1 JOIN j2 ON j1.a = j2.a WHERE j1.b > \'b90\' ORDER BY j1.a LIMIT 5',
	'SELECT j1.a, r1.d FROM j1 JOIN r1 ON j1.a = r1.a ORDER BY j1.a LIMIT 5',
	'SELECT r1.a, r2.e FROM r1 JOIN r2 ON r1.a = r2.a ORDER BY r1.a LIMIT 5',
	'SELECT j1.a, count(*) FROM j1 JOIN j2 ON j1.a = j2.a GROUP BY j1.a ORDER BY j1.a LIMIT 3',
	'SELECT j1.a, r1.d, count(*) FROM j1 JOIN r1 ON j1.a = r1.a GROUP BY j1.a, r1.d ORDER BY j1.a, r1.d LIMIT 5',
	'SELECT r1.a, count(*) FROM r1 JOIN r2 ON r1.a = r2.a GROUP BY r1.a ORDER BY r1.a LIMIT 5',
	'SELECT j1.b, count(*) FROM j1 JOIN j2 ON j1.a = j2.a GROUP BY j1.b ORDER BY j1.b LIMIT 3',
	'SELECT count(*) FROM j1 JOIN j2 ON j1.a = j2.a',
	'SELECT j1.a, sum(j2.a) FROM j1 JOIN j2 ON j1.a = j2.a GROUP BY j1.a ORDER BY j1.a DESC LIMIT 4',
	'SELECT j1.a, count(*) FROM j1 LEFT JOIN j2 ON j1.a = j2.a GROUP BY j1.a ORDER BY j1.a DESC LIMIT 4',
	'SELECT j1.a, j1.b, j2.c, j3.e FROM j1 JOIN j2 ON j1.a = j2.a JOIN j3 ON j2.a = j3.a ORDER BY j1.a LIMIT 5',
	'SELECT j1.a, j2.c, r1.d FROM j1 JOIN j2 ON j1.a = j2.a JOIN r1 ON j2.a = r1.a ORDER BY j1.a LIMIT 5',
	'SELECT j1.a, count(*) FROM j1 JOIN j2 ON j1.a = j2.a JOIN j3 ON j2.a = j3.a GROUP BY j1.a ORDER BY j1.a DESC LIMIT 4',
	'SELECT j1.a, j2.c, j3.e, r1.d FROM j1 JOIN j2 ON j1.a = j2.a JOIN j3 ON j2.a = j3.a JOIN r1 ON j1.a = r1.a ORDER BY j1.a LIMIT 4',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bj1\b/l1/g;
	$local =~ s/\bj2\b/l2/g;
	$local =~ s/\bj3\b/l3/g;
	$local =~ s/\br1\b/lr1/g;
	$local =~ s/\br2\b/lr2/g;
	is(run($query), run($local), "the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
