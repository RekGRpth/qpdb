# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A UNION ALL keeps the hashed locus when all its branches are compatible:
# every key of a group stays on its segment, and the coordinator only appends
# the answers.  Other distributions and UNION DISTINCT stay on the coordinator

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\nlog_min_messages = debug1\n");
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

	$query =~ s/\bt1\b/lt1/g;
	$query =~ s/\bt2\b/lt2/g;
	$query =~ s/\bt3\b/lt3/g;
	$query =~ s/\bm1\b/lm1/g;
	$query =~ s/\bm2\b/lm2/g;
	$query =~ s/\br1\b/lr1/g;
	$query =~ s/\br2\b/lr2/g;
	return $query;
}

run('CREATE TABLE t1 (a int, b text)');
run("SELECT gp.distribute('t1', ARRAY['a'])");
run('CREATE TABLE t2 (a int, b text)');
run("SELECT gp.distribute('t2', ARRAY['a'])");
run('CREATE TABLE t3 (a int, b text)');
run("SELECT gp.distribute('t3', ARRAY['a'])");
run("INSERT INTO t1 VALUES (1, 'a'), (2, 'b'), (4, 'd')");
run("INSERT INTO t2 VALUES (2, 'bb'), (3, 'c'), (5, 'e')");
run("INSERT INTO t3 VALUES (6, 'f'), (7, 'g')");

# A different colocation map, as after a cluster grew and moved its buckets
run(<<'SQL');
INSERT INTO gp.colocation_groups (key_types, nbuckets, map)
SELECT key_types, nbuckets,
       (SELECT array_agg(map[(i % 1024) + 1] ORDER BY i) FROM generate_series(1, 1024) i)
FROM gp.colocation_groups WHERE colocation_id = 1
SQL
run(<<'SQL');
UPDATE gp.distribution_policy p
SET colocation_id = (SELECT max(colocation_id) FROM gp.colocation_groups)
WHERE p.relid = 't3'::regclass
SQL
run("SELECT gp.policy_changed('t3')");

run('CREATE TABLE m1 (a int, b int, v text)');
run("SELECT gp.distribute('m1', ARRAY['a', 'b'])");
run('CREATE TABLE m2 (a int, b int, v text)');
run("SELECT gp.distribute('m2', ARRAY['a', 'b'])");
run("INSERT INTO m1 VALUES (1, 2, 'm12'), (2, 1, 'm21')");
run("INSERT INTO m2 VALUES (1, 2, 'n12'), (3, 2, 'n32')");

run('CREATE TABLE r1 (a int)');
run("SELECT gp.distribute('r1', NULL)");
run('CREATE TABLE r2 (a int)');
run("SELECT gp.distribute('r2', NULL)");
run('INSERT INTO r1 VALUES (10), (20)');
run('INSERT INTO r2 VALUES (20), (30)');

run('CREATE TABLE lt1 AS SELECT * FROM t1');
run('CREATE TABLE lt2 AS SELECT * FROM t2');
run('CREATE TABLE lt3 AS SELECT * FROM t3');
run('CREATE TABLE lm1 AS SELECT * FROM m1');
run('CREATE TABLE lm2 AS SELECT * FROM m2');
run('CREATE TABLE lr1 AS SELECT * FROM r1');
run('CREATE TABLE lr2 AS SELECT * FROM r2');

my $union = 'SELECT a FROM t1 UNION ALL SELECT a FROM t2';
like(plan_for($union), qr/GpSlice/, 'equal hash loci of two branches make one slice');
unlike(plan_for($union), qr/GpMotion/, 'a compatible UNION ALL needs no motion');
like(plan_for('SELECT b, a FROM t1 UNION ALL SELECT b, a FROM t2'),
	qr/GpSlice/, 'a projection keeps the hash of the branches');
like(plan_for('SELECT b, a FROM m1 UNION ALL SELECT b, a FROM m2'),
	qr/GpSlice/, 'a compound key survives the projection too');
like(plan_for('SELECT a FROM t1 UNION ALL SELECT a FROM t3'),
	qr/GpSlice/, 'different hash maps still make one distributed union');
my $replicated = 'SELECT a FROM r1 UNION ALL SELECT a FROM r2';
like(plan_for($replicated), qr/GpSlice/, 'replicated branches make one slice');
unlike(plan_for($replicated), qr/GpMotion/, 'replicated branches need no motion');
unlike(plan_for('SELECT a FROM t1 UNION SELECT a FROM t2'),
	qr/GpSlice/, 'UNION DISTINCT is not a UNION ALL pushdown');
like(plan_for("$union ORDER BY a LIMIT 3"),
	qr/GpSlice/, 'a sort and a limit stay above the union');

my @queries = (
	$union . ' ORDER BY a',
	'SELECT b, a FROM t1 UNION ALL SELECT b, a FROM t2 ORDER BY b, a',
	'SELECT b, a, v FROM m1 UNION ALL SELECT b, a, v FROM m2 ORDER BY b, a, v',
	'SELECT a FROM t1 UNION ALL SELECT a FROM t3 ORDER BY a',
	$replicated . ' ORDER BY a',
	'SELECT a FROM t1 UNION SELECT a FROM t2 ORDER BY a',
	"$union ORDER BY a LIMIT 3",
	'SELECT a FROM t1 UNION ALL SELECT a FROM t2 UNION ALL SELECT a FROM t1 ORDER BY a',
);
foreach my $query (@queries)
{
	is(run($query), run(local_query($query)),
		"the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
