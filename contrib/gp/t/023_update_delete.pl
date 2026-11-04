# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# UPDATE and DELETE of a distributed table: the segments run the statement
# over their own rows, in the transaction of the coordinator, under the
# two-phase commit.  The coordinator takes an exclusive lock on the table
# and answers with the count of all segments.  What is refused: a change of
# the distribution key, RETURNING, a join or a subquery, parameters.

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

sub fails
{
	my ($sql, $message, $test) = @_;
	my ($ret, $stdout, $stderr) = $coordinator->psql('postgres', $sql);
	like($stderr, $message, $test);
}

# The counts of rows per segment, in their order
sub on_segments
{
	return join ' ',
	  map { $_->safe_psql('postgres', 'SELECT count(*) FROM t') } @segments;
}

run('CREATE TABLE t (a int, b text)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run('INSERT INTO t SELECT g, g::text FROM generate_series(1, 30) g');

# UPDATE and DELETE reach every segment, over its own rows
run('UPDATE t SET b = b || \'!\' WHERE a <= 10');
is(run('SELECT count(*) FROM t WHERE b LIKE \'%!\''), 10,
	'UPDATE touched its rows');
my @counts = map { $_->safe_psql('postgres', 'SELECT count(*) FROM t') } @segments;
cmp_ok($counts[0] + $counts[1] + $counts[2], '==', 30,
	'and the rows stay in the cluster, on every segment');
run('DELETE FROM t WHERE a > 25');
is(run('SELECT count(*) FROM t'), 25, 'DELETE removed its rows');

# A statement that matches no row changes nothing
run('UPDATE t SET b = \'no\' WHERE a = 0');
is(run('SELECT count(*) FROM t WHERE b = \'no\''), 0, 'no rows, no change');

# The changes of a transaction roll back everywhere
run('BEGIN; UPDATE t SET b = \'gone\' WHERE a < 100; ROLLBACK');
is(run('SELECT count(*) FROM t WHERE b = \'gone\''), 0,
	'a rollback rolls the segments back');
run('BEGIN; DELETE FROM t WHERE a < 100; ROLLBACK');
is(run('SELECT count(*) FROM t'), 25, 'DELETE rolls back the same way');

# The lock of the coordinator: a second writer waits out the first
my $busy = $coordinator->background_psql('postgres', on_error_stop => 1);
$busy->query_until(qr/start/,
	"\\echo start\nBEGIN; UPDATE t SET b = 'held' WHERE a = 1;\n");
# The first writer holds the lock of the table once its UPDATE has run;
# starting the second writer before that would let it win the lock and
# commit first, and then nothing would wait
$coordinator->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity
	 WHERE query LIKE '%held%' AND state = 'idle in transaction'")
  or die 'the first writer did not update';
my $second = $coordinator->background_psql('postgres', on_error_stop => 1);
$second->query_until(qr/start/,
	"\\echo start\nUPDATE t SET b = 'taken' WHERE a = 2;\n");
$coordinator->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity
	 WHERE wait_event_type = 'Lock' AND query LIKE '%taken%'")
  or die 'the second writer does not wait';
$busy->query('COMMIT');
$second->query('SELECT 1');
$busy->quit;
$second->quit;
is(run('SELECT b FROM t WHERE a IN (1, 2) ORDER BY a'), "held\ntaken",
	'the second writer ran after the first committed');

# What is refused
fails('UPDATE t SET a = 1 WHERE a = 2',
	qr/changing the distribution key of a distributed table is not supported/,
	'UPDATE of the distribution key');
is(run('SELECT count(*) FROM t WHERE a = 1'), 1,
	'and a key the statement sets is refused before any of it runs');
fails('UPDATE t SET b = \'x\' RETURNING a',
	qr/RETURNING is not supported on a distributed table/, 'RETURNING');
fails('UPDATE t SET b = \'x\' FROM generate_series(1, 3) g WHERE a = g',
	qr/can only use its own table/, 'a FROM item');
fails('UPDATE t SET b = (SELECT max(b) FROM t) WHERE a = 1',
	qr/subqueries|only its own table/s, 'a subquery');
fails('DELETE FROM t WHERE a IN (SELECT a FROM t WHERE a < 5)',
	qr/subqueries|only its own table|row locks are not supported/s,
	'a subquery of DELETE');
fails('WITH moved AS (DELETE FROM t WHERE a = 1 RETURNING a)
	SELECT count(*) FROM moved',
	qr/a data-modifying statement in WITH/, 'a data-modifying WITH');
fails('PREPARE upd(int) AS UPDATE t SET b = \'p\' WHERE a = $1; EXECUTE upd(1)',
	qr/parameters are not supported/, 'parameters of a prepared statement');
fails('MERGE INTO t USING (VALUES (1)) s(a) ON t.a = s.a WHEN MATCHED
	THEN UPDATE SET b = \'m\'',
	qr/MERGE of a distributed table is not supported yet/, 'MERGE');

# A replicated table takes its UPDATE on every segment
run('CREATE TABLE r (a int)');
run("SELECT gp.distribute('r', NULL)");
run('INSERT INTO r VALUES (1), (2)');
run('UPDATE r SET a = a + 10');
is(run('SELECT sum(a) FROM r'), 23, 'UPDATE of a replicated table takes every copy');

$_->stop foreach (@segments, $coordinator);

done_testing();
