# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A join of two hashed tables of different colocation groups, joined by the
# whole key of both, moves the rows of one table: an inner join redistributes
# them by the key, an outer join broadcasts its null-supplying side, and a
# full join broadcasts both sides and runs on one segment

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
	# The motion of the segment calls the functions of the extension there
	$node->safe_psql('postgres',
		"SET gp.allow_local_ddl = on; CREATE EXTENSION gp");
	push @segments, $node;
}
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 3 FROM gp.cluster_segments()')
  or die 'segments did not register';

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }

run('CREATE TABLE t1 (a int, b text)');
run("SELECT gp.distribute('t1', ARRAY['a'])");
run('CREATE TABLE t2 (a int, c text)');
run("SELECT gp.distribute('t2', ARRAY['a'])");
run("INSERT INTO t1 SELECT g, 'b' || g FROM generate_series(1, 100) g");
run("INSERT INTO t2 SELECT g, 'c' || g FROM generate_series(41, 120) g");
run("INSERT INTO t1 VALUES (NULL, 'bnull1'), (NULL, 'bnull2')");
run("INSERT INTO t2 VALUES (NULL, 'cnull1'), (NULL, 'cnull2')");

# t2 goes to a colocation group of its own, with the buckets turned: that
# is what a cluster looks like after it grew and its tables spread over
# the segments differently
run(<<'SQL');
INSERT INTO gp.colocation_groups (key_types, nbuckets, map)
SELECT key_types, nbuckets,
       (SELECT array_agg(map[(i % 1024) + 1] ORDER BY i) FROM generate_series(1, 1024) i)
FROM gp.colocation_groups WHERE colocation_id = 1
SQL
run(<<'SQL');
UPDATE gp.distribution_policy p
SET colocation_id = (SELECT max(colocation_id) FROM gp.colocation_groups)
WHERE p.relid = 't2'::regclass
SQL
run("SELECT gp.policy_changed('t2')");

# The same rows in tables of the coordinator, to compare the answers with
run('CREATE TABLE l1 AS SELECT * FROM t1');
run('CREATE TABLE l2 AS SELECT * FROM t2');

like(plan_for('SELECT t1.a, t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/, 'a join of two colocation groups moves one table');
like(plan_for('SELECT t1.a, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/, 'a left join broadcasts its right side');
like(plan_for('SELECT t1.a, t2.c FROM t1 RIGHT JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/, 'a right join broadcasts its left side');
like(plan_for('SELECT t1.a, t2.c FROM t1 FULL JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/, 'a full join broadcasts both sides to one segment');
like(plan_for('SELECT t1.a, count(t2.c) FROM t1 LEFT JOIN t2 ON t1.a = t2.a GROUP BY t1.a'),
	qr/GpSlice/, 'an aggregate by the preserved side stays on the segments');
unlike(plan_for('SELECT t2.a, count(*) FROM t1 LEFT JOIN t2 ON t1.a = t2.a GROUP BY t2.a'),
	qr/GpSlice/,
	'an aggregate by the null-supplying side keeps its null group on the coordinator');
unlike(plan_for('SELECT a, n FROM (SELECT t2.a AS a, count(*) AS n FROM t1 LEFT JOIN t2 ON t1.a = t2.a GROUP BY t2.a) s'),
	qr/GpSlice/,
	'the null group of a subquery stays on the coordinator too');

# Execute the ordered queries through hash joins too: a merge join answers
# their ORDER BY itself and deliberately stays on the coordinator
run('SET enable_mergejoin = off');

my @queries = (
	'SELECT t1.a, t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a ORDER BY t1.a',
	'SELECT t1.a, t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a WHERE t1.b > \'b90\' ORDER BY t1.a',
	'SELECT t1.a, t2.c FROM t2 JOIN t1 ON t2.a = t1.a ORDER BY t1.a LIMIT 5',
	'SELECT t1.a, count(*) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.a ORDER BY t1.a',
	'SELECT count(*) FROM t1 JOIN t2 ON t1.a = t2.a',
	'SELECT t1.a, min(t2.c) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.a ORDER BY t1.a LIMIT 5',
	'SELECT t1.a, t1.b, t2.a, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a ORDER BY t1.a, t1.b, t2.a, t2.c',
	'SELECT t2.a, t2.c, t1.a, t1.b FROM t2 LEFT JOIN t1 ON t2.a = t1.a ORDER BY t2.a, t2.c, t1.a, t1.b',
	'SELECT t1.a, t1.b, t2.a, t2.c FROM t1 RIGHT JOIN t2 ON t1.a = t2.a ORDER BY t2.a, t2.c, t1.a, t1.b',
	'SELECT t1.a, t1.b, t2.a, t2.c FROM t1 FULL JOIN t2 ON t1.a = t2.a ORDER BY t1.a, t1.b, t2.a, t2.c',
	'SELECT t1.a, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a ORDER BY t1.a, t2.c LIMIT 7',
	'SELECT t1.a, count(t2.c) FROM t1 LEFT JOIN t2 ON t1.a = t2.a GROUP BY t1.a ORDER BY t1.a',
	'SELECT t2.a, count(*) FROM t1 LEFT JOIN t2 ON t1.a = t2.a GROUP BY t2.a ORDER BY t2.a',
	'SELECT t2.a, count(t1.b) FROM t1 RIGHT JOIN t2 ON t1.a = t2.a GROUP BY t2.a ORDER BY t2.a',
	'SELECT t1.a, count(*) FROM t1 RIGHT JOIN t2 ON t1.a = t2.a GROUP BY t1.a ORDER BY t1.a',
	'SELECT t1.a, t2.a, count(*) FROM t1 FULL JOIN t2 ON t1.a = t2.a GROUP BY t1.a, t2.a ORDER BY t1.a, t2.a',
	'SELECT count(*) FROM t1 FULL JOIN t2 ON t1.a = t2.a',
	'SELECT a, n FROM (SELECT t1.a AS a, count(t2.c) AS n FROM t1 LEFT JOIN t2 ON t1.a = t2.a GROUP BY t1.a) s ORDER BY a',
	'SELECT a, n FROM (SELECT t2.a AS a, count(*) AS n FROM t1 LEFT JOIN t2 ON t1.a = t2.a GROUP BY t2.a) s ORDER BY a',
	'SELECT a, n FROM (SELECT t1.a AS a, count(t2.c) AS n FROM t1 FULL JOIN t2 ON t1.a = t2.a GROUP BY t1.a) s ORDER BY a',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bt1\b/l1/g;
	$local =~ s/\bt2\b/l2/g;
	is(run($query), run($local), "the same answer as on the coordinator: $query");
}

# Autonomous motion connections cannot see the writes of this transaction.
# New plans therefore use the gather, whose connections are in it already
my $session = $coordinator->background_psql('postgres', on_error_stop => 0);
$session->query_safe('BEGIN');
$session->query_safe("INSERT INTO t1 VALUES (1000, 'b1000')");
$session->query_safe("INSERT INTO t2 VALUES (1000, 'c1000')");
$session->query_safe("INSERT INTO l1 VALUES (1000, 'b1000')");
$session->query_safe("INSERT INTO l2 VALUES (1000, 'c1000')");
my @own_writes = (
	'SELECT t1.a, t2.a FROM t1 JOIN t2 ON t1.a = t2.a WHERE t1.a = 1000',
	'SELECT t1.a, t2.a FROM t1 LEFT JOIN t2 ON t1.a = t2.a WHERE t1.a = 1000',
	'SELECT t1.a, t2.a FROM t1 FULL JOIN t2 ON t1.a = t2.a WHERE t1.a = 1000',
);
foreach my $query (@own_writes)
{
	my $local = $query;
	$local =~ s/\bt1\b/l1/g;
	$local =~ s/\bt2\b/l2/g;
	unlike($session->query_safe("EXPLAIN (COSTS OFF) $query"), qr/GpSlice/,
		"a motion after a write stays on the coordinator: $query");
	is($session->query_safe($query), $session->query_safe($local),
		"a query after a write sees it: $query");
}
$session->query_safe('ROLLBACK');

# A generic plan made before the write cannot switch to the gather by itself
$session->query_safe('SET enable_mergejoin = off');
$session->query_safe('SET plan_cache_mode = force_generic_plan');
$session->query_safe('PREPARE full_motion AS SELECT t1.a, t2.a FROM t1 FULL JOIN t2 ON t1.a = t2.a WHERE t1.a = 1000 OR t2.a = 1000');
$session->query_safe('EXECUTE full_motion');
$session->query_safe('BEGIN');
$session->query_safe("INSERT INTO t1 VALUES (1000, 'b1000')");
$session->query('EXECUTE full_motion');
like($session->{stderr}, qr/a motion query cannot see writes/,
	'a prepared motion does not miss the writes of its transaction');
$session->{stderr} = '';
$session->query_safe('ROLLBACK');
$session->query_safe('DEALLOCATE full_motion');
$session->quit;

# A dropped column cannot be returned by the record of a motion.  It must
# cancel the rewrite rather than leave an ordinary table without its motion
run('ALTER TABLE t1 ADD COLUMN junk int');
run('ALTER TABLE t1 DROP COLUMN junk');
run('ALTER TABLE t2 ADD COLUMN junk int');
run('ALTER TABLE t2 DROP COLUMN junk');
unlike(plan_for('SELECT t1.a, t2.a FROM t1 LEFT JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/, 'a dropped column cancels a broadcast');
unlike(plan_for('SELECT t1.a, t2.a FROM t1 FULL JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/, 'dropped columns cancel a two-sided broadcast');
foreach my $query (
	'SELECT t1.a, t1.b, t2.a, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a ORDER BY t1.a, t1.b, t2.a, t2.c',
	'SELECT t1.a, t1.b, t2.a, t2.c FROM t1 FULL JOIN t2 ON t1.a = t2.a ORDER BY t1.a, t1.b, t2.a, t2.c')
{
	my $local = $query;
	$local =~ s/\bt1\b/l1/g;
	$local =~ s/\bt2\b/l2/g;
	is(run($query), run($local), "a dropped column falls back correctly: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
