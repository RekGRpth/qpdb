# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# SELECT from distributed tables: the coordinator gathers all rows from the
# segments and runs the rest of the query.  The results are compared with those
# of the same queries on ordinary tables with the same rows.

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
foreach my $segment_id (1 .. 2)
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
my ($seg1, $seg2) = @segments;
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.cluster_segments()')
  or die 'segments did not register';

sub run { return $coordinator->safe_psql('postgres', shift); }

sub fails
{
	my ($sql, $message, $test, %params) = @_;
	my ($ret, $stdout, $stderr) = $coordinator->psql('postgres', $sql, %params);
	like($stderr, $message, $test);
}

# What each segment has by a query that gives one value, as "seg1 seg2"
sub on_segments
{
	my ($query) = @_;
	return join ' ', map { $_->safe_psql('postgres', $query) } @segments;
}

# A query on distributed tables d and d2 gives what it gives on the ordinary
# tables r and r2 with the same rows
sub same_result
{
	my ($sql, $test) = @_;
	(my $ref = $sql) =~ s/\bd2\b/r2/g;
	$ref =~ s/\bd\b/r/g;
	my $got = run($sql);
	is($got, run($ref), $test);
	return $got;
}

# The same tables, distributed and not
my $fill = q{SELECT g, 'v' || (g % 10), g / 7.0,
	timestamptz '2026-01-01 00:00:00+00' + g * interval '1 hour'
FROM generate_series(1, 500) g};
run('CREATE TABLE d (a int, b text, c numeric, ts timestamptz)');
run(q{SELECT gp.distribute('d', ARRAY['a'])});
run("INSERT INTO d $fill");
run('CREATE TABLE r (a int, b text, c numeric, ts timestamptz)');
run("INSERT INTO r $fill");

run('CREATE TABLE d2 (a int, e text)');
run(q{SELECT gp.distribute('d2', ARRAY['a'])});
run(q{INSERT INTO d2 SELECT g * 3, 'e' || g FROM generate_series(1, 100) g});
run('CREATE TABLE r2 (a int, e text)');
run(q{INSERT INTO r2 SELECT g * 3, 'e' || g FROM generate_series(1, 100) g});

is(run(q{SELECT pg_relation_size('d')}), 0, 'the coordinator has no rows of its own');
like(on_segments('SELECT count(*) FROM d'), qr/^[1-9]\d* [1-9]\d*$/,
	'both segments have rows');

# Queries
my $got = same_result('SELECT count(*), sum(a), min(b), max(c), max(ts) FROM d',
	'aggregates');
like($got, qr/^500\|125250\|/, 'which are all 500 rows');
same_result('SELECT * FROM d WHERE a % 13 = 0 ORDER BY a', 'a condition');
same_result('SELECT b, count(*), sum(a), avg(c) FROM d GROUP BY b ORDER BY b',
	'GROUP BY');
same_result('SELECT a, b FROM d ORDER BY c DESC, a LIMIT 5 OFFSET 10', 'LIMIT and OFFSET');
same_result('SELECT d.a, d2.e FROM d JOIN d2 ON d.a = d2.a ORDER BY d.a',
	'a join of two distributed tables');
same_result('SELECT a FROM d WHERE a IN (SELECT a FROM d2) ORDER BY a',
	'a subquery');
same_result('SELECT a FROM d WHERE NOT EXISTS (SELECT 1 FROM d2 WHERE d2.a = d.a) AND a < 20 ORDER BY a',
	'NOT EXISTS');
same_result('WITH x AS (SELECT a FROM d WHERE a < 40) SELECT count(*) FROM x JOIN d2 USING (a)',
	'a CTE');
same_result('SELECT count(*) FROM d d1 JOIN d d3 ON d1.a = d3.a % 100', 'a self join');
same_result('SELECT d.b, count(d2.a) FROM d LEFT JOIN d2 ON d.a = d2.a GROUP BY d.b ORDER BY d.b',
	'an outer join');
same_result('SELECT a, rank() OVER (PARTITION BY b ORDER BY a) FROM d WHERE a < 30 ORDER BY a',
	'a window function');
same_result('SELECT a FROM d WHERE a < 10 UNION ALL SELECT a FROM d2 WHERE a < 10 ORDER BY 1',
	'UNION ALL');

# A join that rescans the gathered rows, in a nested loop
same_result(q{SET enable_hashjoin = off; SET enable_mergejoin = off;
SELECT d.a, d2.e FROM d JOIN d2 ON d.a = d2.a ORDER BY d.a},
	'a nested loop that rescans');

# A join of a distributed and an ordinary table
run('CREATE TABLE loc (a int, z text)');
run(q{INSERT INTO loc SELECT g * 5, 'z' FROM generate_series(1, 20) g});
is(run('SELECT count(*) FROM d JOIN loc ON d.a = loc.a'),
	run('SELECT count(*) FROM r JOIN loc ON r.a = loc.a'),
	'a join with an ordinary table');

# A replicated table is read once, not from all segments
run('CREATE TABLE rep (a int, w text)');
run(q{SELECT gp.distribute('rep', NULL)});
run(q{INSERT INTO rep SELECT g, 'w' FROM generate_series(1, 50) g});
is(run('SELECT count(*) FROM rep'), 50, 'a replicated table has its rows once');
is(on_segments('SELECT count(*) FROM rep'), '50 50', 'though every segment has them');
is(run('SELECT count(*) FROM d JOIN rep ON d.a = rep.a'), 50, 'and joins as such');

# A table without rows, and one whose columns have been dropped
run('CREATE TABLE empty (a int)');
run(q{SELECT gp.distribute('empty', ARRAY['a'])});
is(run('SELECT count(*) FROM empty'), 0, 'an empty distributed table');
run('ALTER TABLE d DROP COLUMN c');
run('ALTER TABLE r DROP COLUMN c');
same_result('SELECT * FROM d WHERE a < 6 ORDER BY a', 'a table with a dropped column');
run('ALTER TABLE d ADD COLUMN k int DEFAULT 7');
run('ALTER TABLE r ADD COLUMN k int DEFAULT 7');
same_result('SELECT sum(k), count(*) FROM d', 'and a column that was added');

# Types
run(q{CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy')});
run(q{CREATE TABLE ty (id int, m mood, arr int[], ts timestamptz, n numeric,
					   j jsonb, tx text, f float8, bx bytea, u uuid)});
run(q{SELECT gp.distribute('ty', ARRAY['id'])});
run('CREATE TABLE ty_ref (LIKE ty)');
my $rows = q{(1, 'sad', '{1,2,3}', '2026-10-30 12:00:00+00', 1.50, '{"a": [1, 2]}', 'one', 0.1, '\x00ff', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'),
(2, 'ok', '{}', '2026-01-01 00:00:00+00', -0.001, '[]', E'two\ttab', 1e300, '\x', NULL),
(3, 'happy', '{NULL,5}', 'infinity', 1e20, 'null', NULL, 'NaN', NULL, NULL),
(4, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)};
run("INSERT INTO ty VALUES $rows");
run("INSERT INTO ty_ref VALUES $rows");
is(run('SELECT md5(t::text) FROM ty t ORDER BY 1'),
	run('SELECT md5(t::text) FROM ty_ref t ORDER BY 1'), 'many types');

# What the transaction has written is seen, and what it has rolled back is not
is(run(q{BEGIN;
INSERT INTO d (a, b) VALUES (1000, 'z');
SELECT count(*) FROM d;
ROLLBACK}), 501, 'a transaction sees its own rows');
is(run('SELECT count(*) FROM d'), 500, 'and not after a rollback');
is(run(q{BEGIN;
COPY d (a) FROM PROGRAM 'seq 1001 1010';
SELECT count(*) FROM d WHERE a > 1000;
COMMIT}), 10, 'including those of COPY');
is(run('SELECT count(*) FROM d WHERE a > 1000'), 10, 'and after a commit');

# A prepared statement
is(run(q{PREPARE q (int) AS SELECT count(*) FROM d WHERE a > $1;
EXECUTE q (100); EXECUTE q (200); EXECUTE q (300); EXECUTE q (400);
EXECUTE q (450); EXECUTE q (480); EXECUTE q (490)}),
	"410\n310\n210\n110\n60\n30\n20", 'a prepared statement');

# A view, and EXPLAIN
run('CREATE VIEW v AS SELECT a FROM d2 WHERE a < 50');
is(run('SELECT count(*) FROM v'), 16, 'a view of a distributed table');
my $plan = run(q{EXPLAIN (VERBOSE, COSTS off) SELECT a FROM d2 WHERE a < 50});
like($plan, qr/Custom Scan \(GpGather\) on public\.d2/, 'the plan has the node of the cluster');
like($plan, qr/Remote SQL: SELECT a, e FROM ONLY public\.d2/, 'and shows what the segments run');
unlike(run('EXPLAIN (COSTS off) SELECT a FROM loc'), qr/GpGather/, 'a local table has the usual plan');

# A partitioned table
run('CREATE TABLE pt (a int, b text) PARTITION BY RANGE (a)');
run('CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (100)');
run('CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (100) TO (1000)');
run(q{SELECT gp.distribute('pt', ARRAY['a'])});
run(q{INSERT INTO pt SELECT g, 'p' FROM generate_series(1, 300) g});
is(run('SELECT count(*), min(a), max(a) FROM pt'), '300|1|300', 'a partitioned table');
is(run('SELECT count(*) FROM pt WHERE a < 100'), 99, 'and its partition by a condition');

# TRUNCATE goes to the segments
run('TRUNCATE d2');
is(run('SELECT count(*) FROM d2'), 0, 'TRUNCATE empties a distributed table');
is(on_segments('SELECT count(*) FROM d2'), '0 0', 'on the segments');

# Privileges are those of the user, on the coordinator and on the segments
run('CREATE ROLE u1 LOGIN');
run('CREATE ROLE u2 LOGIN');
run('GRANT SELECT ON d TO u1');
my $out;
$coordinator->psql('postgres', 'SELECT count(*) FROM d',
	extra_params => [ '-U', 'u1' ], stdout => \$out);
chomp $out;
is($out, '510', 'a user with SELECT reads the rows');
fails('SELECT count(*) FROM d', qr/permission denied for table d/,
	'a user without SELECT cannot', extra_params => [ '-U', 'u2' ]);

# What is not supported yet is refused, not done on the coordinator alone
fails('UPDATE d SET b = \'u\' WHERE a = 1',
	qr/UPDATE of a distributed table is not supported yet/, 'UPDATE');
fails('DELETE FROM d WHERE a = 1',
	qr/DELETE of a distributed table is not supported yet/, 'DELETE');
fails(q{WITH ins AS (INSERT INTO d (a) VALUES (7)) SELECT 1},
	qr/a data-modifying statement in WITH of a distributed table is not supported yet/,
	'INSERT in WITH');
fails('SELECT * FROM d WHERE a = 1 FOR UPDATE',
	qr/row locks are not supported on a distributed table/, 'FOR UPDATE');
fails('SELECT * FROM d TABLESAMPLE SYSTEM (10)',
	qr/TABLESAMPLE is not supported on a distributed table/, 'TABLESAMPLE');
is(run('SELECT count(*) FROM d'), 510, 'and they leave the rows as they were');

$_->stop foreach (@segments, $coordinator);

done_testing();
