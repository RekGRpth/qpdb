# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A join of two hashed tables of different colocation groups, joined by the
# whole key of both, moves the rows of one table: every segment asks every
# segment — itself among them — for the rows that the hash of the join key
# places on it, and joins what comes with its own rows

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
unlike(plan_for('SELECT t1.a, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/,
	'an outer join is not redistributed: a row with a null key would meet no partner');

my @queries = (
	'SELECT t1.a, t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a ORDER BY t1.a',
	'SELECT t1.a, t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a WHERE t1.b > \'b90\' ORDER BY t1.a',
	'SELECT t1.a, t2.c FROM t2 JOIN t1 ON t2.a = t1.a ORDER BY t1.a LIMIT 5',
	'SELECT t1.a, count(*) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.a ORDER BY t1.a',
	'SELECT count(*) FROM t1 JOIN t2 ON t1.a = t2.a',
	'SELECT t1.a, min(t2.c) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.a ORDER BY t1.a LIMIT 5',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bt1\b/l1/g;
	$local =~ s/\bt2\b/l2/g;
	is(run($query), run($local), "the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
