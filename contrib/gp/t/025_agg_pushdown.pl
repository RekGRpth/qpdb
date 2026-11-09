# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Aggregates whose grouping covers the key of the distribution run on the
# segments.  A grouping by other plain columns redistributes the rows by
# them first, so every group still reaches one segment.  A replicated table
# is aggregated by one segment as a whole

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
run("INSERT INTO t SELECT g % 10, 'x' || g, g * 1.5 FROM generate_series(1, 1000) g");
# The same rows in a table of the coordinator, to compare the answers with
run('CREATE TABLE l AS SELECT * FROM t');
run('CREATE TABLE r (a int, b text)');
run("SELECT gp.distribute('r', NULL)");
run("INSERT INTO r SELECT g % 5, 'x' || g FROM generate_series(1, 50) g");

like(plan_for('SELECT a, count(*) FROM t GROUP BY a'), qr/GpSlice/,
	'grouping by the key runs on the segments');
like(plan_for('SELECT a, count(*), sum(c) FROM t WHERE b > \'x100\' GROUP BY a ORDER BY a LIMIT 5'),
	qr/GpSlice/, 'filter, sort and limit do not stop the pushdown');
like(plan_for('SELECT a, count(*) FROM t GROUP BY a ORDER BY count(*) DESC, a LIMIT 4'),
	qr/GpSlice/, 'an order by the aggregate is kept on the coordinator');
like(plan_for('SELECT b, count(*) FROM t GROUP BY b'), qr/GpSlice/,
	'grouping by another column redistributes the rows by it');
like(plan_for('SELECT b, avg(c), count(*) FROM t GROUP BY b'), qr/GpSlice/,
	'a redistributed group is aggregated to the end');
like(plan_for('SELECT b, count(DISTINCT a) FROM t GROUP BY b'), qr/GpSlice/,
	'a distinct aggregate works after redistribution too');
like(plan_for('SELECT b, n FROM (SELECT b, count(*) AS n FROM t GROUP BY b) s'),
	qr/GpSlice/, 'a non-key aggregate redistributes inside a subquery too');
unlike(plan_for('SELECT count(*) FROM t'), qr/GpSlice/,
	'a global aggregate of a hashed table is not pushed');
unlike(plan_for('SELECT a, count(DISTINCT b) FROM t GROUP BY a ORDER BY a'),
	qr/GpSlice/,
	'a sorted aggregate that answers the order by itself is not pushed');
like(plan_for('SELECT a, count(DISTINCT b) FROM t GROUP BY a'), qr/GpSlice/,
	'a distinct aggregate without an order by is pushed');
like(plan_for('SELECT count(*) FROM t GROUP BY a'), qr/GpSlice/,
	'a grouping key that is not selected is not a stopper either');
like(plan_for('SELECT count(*) FROM t GROUP BY a ORDER BY count(*) DESC, a'),
	qr/GpSlice/, 'a junk grouping column goes to the segments for the sort');
like(plan_for('SELECT count(*) FROM t GROUP BY a, b ORDER BY a, b'), qr/GpSlice/,
	'junk grouping columns of a wider key are pushed');

my @queries = (
	'SELECT a, count(*) FROM t GROUP BY a ORDER BY a',
	'SELECT a, count(*), sum(c) FROM t WHERE b > \'x100\' GROUP BY a ORDER BY a LIMIT 5',
	'SELECT a, b, count(*) FROM t GROUP BY a, b ORDER BY a, b LIMIT 7',
	'SELECT a, count(DISTINCT b) FROM t GROUP BY a ORDER BY a',
	'SELECT a, count(DISTINCT b) FROM t GROUP BY a ORDER BY a DESC LIMIT 3',
	'SELECT a, sum(c) FROM t GROUP BY a HAVING sum(c) > 70000 ORDER BY a',
	'SELECT b, count(*) FROM t GROUP BY b ORDER BY b LIMIT 3',
	'SELECT b, avg(c), count(*) FROM t GROUP BY b ORDER BY b',
	'SELECT b, count(DISTINCT a) FROM t GROUP BY b ORDER BY b',
	'SELECT b, n FROM (SELECT b, count(*) AS n FROM t GROUP BY b) s ORDER BY b',
	'SELECT count(*) FROM t',
	'SELECT a, count(*) FROM t WHERE a > 5 GROUP BY a ORDER BY a DESC LIMIT 3',
	'SELECT a, count(*) FROM t GROUP BY a ORDER BY count(*) DESC, a LIMIT 4',
	'SELECT count(*) FROM t GROUP BY a ORDER BY a',
	'SELECT count(*) FROM t GROUP BY a ORDER BY count(*) DESC, a',
	'SELECT count(*) FROM t GROUP BY a ORDER BY a+1',
	'SELECT count(*) FROM t GROUP BY a, b ORDER BY a, b',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bt\b/l/g;
	is(run($query), run($local), "the same answer as on the coordinator: $query");
}

like(plan_for('SELECT a, count(*) FROM r GROUP BY a ORDER BY a'), qr/GpSlice/,
	'a replicated table is aggregated by one segment');
like(plan_for('SELECT count(*) FROM r'), qr/GpSlice/,
	'a global aggregate of a replicated table is pushed');
is(run('SELECT a, count(*) FROM r GROUP BY a ORDER BY a'),
	"0|10\n1|10\n2|10\n3|10\n4|10", 'the groups of the replicated table');
is(run('SELECT count(*) FROM r'), 50, 'the global count of the replicated table');

# A grouping type without an extended hash function cannot move rows
run('CREATE TABLE mt (a int, m money)');
run("SELECT gp.distribute('mt', ARRAY['a'])");
run("INSERT INTO mt VALUES (1, 10), (2, 10), (3, 20)");
run('CREATE TABLE ml AS SELECT * FROM mt');
unlike(plan_for('SELECT m, count(*) FROM mt GROUP BY m'), qr/GpSlice/,
	'a group by an unhashable type stays on the coordinator');
is(run('SELECT m, count(*) FROM mt GROUP BY m ORDER BY m'),
	run('SELECT m, count(*) FROM ml GROUP BY m ORDER BY m'),
	'the unhashable type still has a sorted aggregate');

# ONLY keeps children out of the motion, while the default includes them
run('CREATE TABLE parent_t (a int, b text)');
run("SELECT gp.distribute('parent_t', ARRAY['a'])");
run('CREATE TABLE child_t () INHERITS (parent_t)');
run("SELECT gp.distribute('child_t', ARRAY['a'])");
run("INSERT INTO parent_t VALUES (1, 'parent')");
run("INSERT INTO child_t VALUES (2, 'child')");
like(plan_for('SELECT b, count(*) FROM ONLY parent_t GROUP BY b'), qr/GpSlice/,
	'ONLY still allows aggregate redistribution');
is(run('SELECT b, count(*) FROM ONLY parent_t GROUP BY b ORDER BY b'),
	'parent|1', 'ONLY does not read the child through the motion');
is(run('SELECT b, count(*) FROM parent_t GROUP BY b ORDER BY b'),
	"child|1\nparent|1", 'the default inheritance scan includes the child');

# An autonomous motion cannot see the writes of this transaction
my $session = $coordinator->background_psql('postgres', on_error_stop => 1);
$session->query_safe('PREPARE own_slice AS SELECT a, count(*) FROM t WHERE a = 11 GROUP BY a');
$session->query_safe('BEGIN');
$session->query_safe("INSERT INTO t VALUES (11, 'new', 1)");
$session->query_safe("INSERT INTO l VALUES (11, 'new', 1)");
unlike($session->query_safe('EXPLAIN (COSTS OFF) SELECT b, count(*) FROM t GROUP BY b'),
	qr/GpSlice/, 'an aggregate motion after a write stays on the coordinator');
is($session->query_safe("SELECT b, count(*) FROM t WHERE b = 'new' GROUP BY b"),
	$session->query_safe("SELECT b, count(*) FROM l WHERE b = 'new' GROUP BY b"),
	'an aggregate after a write sees the new row');
is($session->query_safe('EXECUTE own_slice'),
	'11|1', 'a prepared slice falls back and sees the new row');
$session->query_safe('ROLLBACK');
$session->query_safe('DEALLOCATE own_slice');
$session->quit;

# A stable expression must see the session settings, not those of a fresh
# autonomous worker.  This slice stays on the synchronized connections.
run('CREATE TABLE ts (a int, t timestamptz)');
run("SELECT gp.distribute('ts', ARRAY['a'])");
run("INSERT INTO ts VALUES (1, '2024-01-01 02:00+00'), (2, '2024-01-01 12:00+00')");
run('CREATE TABLE ts_l AS SELECT * FROM ts');
my $tz_query = q{SET TIME ZONE 'America/New_York'; SELECT a, date_trunc('day', min(t)) FROM %s GROUP BY a ORDER BY a};
like(plan_for("SELECT a, date_trunc('day', min(t)) FROM ts GROUP BY a ORDER BY a"),
	qr/GpSlice/, 'a stable expression keeps its slice plan');
is(run(sprintf($tz_query, 'ts')), run(sprintf($tz_query, 'ts_l')),
	'a stable expression uses the TimeZone of the session');

# A dropped column cannot be returned by the record of a motion
run('ALTER TABLE t ADD COLUMN junk int');
run('ALTER TABLE t DROP COLUMN junk');
unlike(plan_for('SELECT b, count(*) FROM t GROUP BY b'), qr/GpSlice/,
	'a dropped column cancels aggregate redistribution');
is(run('SELECT b, count(*) FROM t GROUP BY b ORDER BY b LIMIT 3'),
	run('SELECT b, count(*) FROM l GROUP BY b ORDER BY b LIMIT 3'),
	'a dropped column leaves the right answer on the coordinator');

$_->stop foreach (@segments, $coordinator);

done_testing();
