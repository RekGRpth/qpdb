# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# An aggregate grouped by expressions, and a global aggregate that does not
# move all rows to one segment, is answered in two steps: the segments
# aggregate their own rows, and the coordinator finishes the aggregate over
# the columns of every segment.  min, max and sum of a closing type combine
# by the same aggregate again

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

like(plan_for('SELECT a % 2, sum(c) FROM t GROUP BY a % 2'),
	qr/GpSlice/, 'a sum by another column is answered partly on the segments');
like(plan_for('SELECT a % 2, min(c), max(c) FROM t GROUP BY a % 2'),
	qr/GpSlice/, 'min and max by another column are answered partly');
like(plan_for('SELECT min(c), max(c), sum(c) FROM t'),
	qr/GpSlice/, 'a global aggregate is answered partly on the segments');
like(plan_for('SELECT a % 2, min(c) FROM t GROUP BY a % 2 HAVING min(c) > 100'),
	qr/GpSlice/, 'HAVING stays on the coordinator and does not stop the answer');
unlike(plan_for('SELECT a % 2, count(*) FROM t GROUP BY a % 2'), qr/GpSlice/,
	'count counts the partial columns, so it stays on the coordinator');
unlike(plan_for('SELECT avg(c) FROM t'), qr/GpSlice/,
	'the average divides, so it stays on the coordinator');
unlike(plan_for('SELECT a % 2, sum(c), count(*) FROM t GROUP BY a % 2'),
	qr/GpSlice/, 'a mix with count stays on the coordinator');

my @queries = (
	'SELECT a % 2, sum(c) FROM t GROUP BY a % 2 ORDER BY 1',
	'SELECT a % 3, min(c), max(c) FROM t GROUP BY a % 3 ORDER BY 1',
	'SELECT min(c), max(c), sum(c) FROM t',
	'SELECT upper(substring(b, 1, 2)), sum(c) FROM t GROUP BY 1 ORDER BY 1',
	'SELECT a % 2, min(c) FROM t GROUP BY a % 2 HAVING min(c) > 100 ORDER BY 1',
	'SELECT a % 2, sum(c) FROM t GROUP BY a % 2 HAVING sum(c) > 300000 ORDER BY 1',
	'SELECT a % 2, sum(c) FROM t WHERE c > 100 GROUP BY a % 2 ORDER BY 1',
	'SELECT sum(c) FROM t HAVING max(c) > 1000',
	'SELECT a % 2, count(*), avg(c) FROM t GROUP BY a % 2 ORDER BY 1',
	'SELECT b, sum(c) FROM t GROUP BY b ORDER BY b LIMIT 5',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bt\b/l/g;
	is(run($query), run($local), "the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
