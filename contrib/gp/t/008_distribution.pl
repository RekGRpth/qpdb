# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Tables are distributed by hash in colocation groups, or replicated

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('coordinator');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\ngp.dispatch_ddl = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION gp');

# Without segments there is nothing to distribute over
$node->safe_psql('postgres', 'CREATE TABLE t0 (a int)');
my ($ret, $stdout, $stderr) =
  $node->psql('postgres', "SELECT gp.distribute('t0', '{a}')");
like($stderr, qr/the cluster has no segments/, 'no segments, no groups');

# Segments 1, 2 and 5: numbers need not follow each other
$node->safe_psql('postgres',
	"SELECT gp.register_segment(n, 'localhost', 9000 + n) FROM unnest('{1,2,5}'::int[]) n"
);
$node->poll_query_until('postgres',
	'SELECT count(*) = 3 FROM gp.cluster_segments()')
  or die 'segments are not in the copy';

sub query { return $node->safe_psql('postgres', shift); }

sub fails
{
	my ($sql, $message, $test) = @_;
	my ($ret, $stdout, $stderr) = $node->psql('postgres', $sql);
	like($stderr, $message, $test);
}

query(
	'CREATE TABLE t1 (a int, b text); CREATE TABLE t2 (x int);
CREATE TABLE t3 (s text); CREATE TABLE t4 (a int, b text);
CREATE TABLE t5 (x bigint); CREATE TABLE t6 (y int);
CREATE TABLE r (a int)');

query("SELECT gp.distribute('t1', '{a}')");
is( query(
		"SELECT kind, key_columns, colocation_id FROM gp.distribution_policy WHERE relid = 't1'::regclass"
	),
	'hash|{1}|1',
	'a table is distributed by hash in a new group');
is( query(
		'SELECT key_types, nbuckets, map[1:7] FROM gp.colocation_groups WHERE colocation_id = 1'
	),
	'{integer}|1024|{1,2,5,1,2,5,1}',
	'a new group spreads its buckets over the segments in turn');
is( query(
		'SELECT string_agg(format(\'%s:%s\', m, n), \' \' ORDER BY m) FROM (SELECT m, count(*) n FROM gp.colocation_groups, unnest(map) m WHERE colocation_id = 1 GROUP BY m)'
	),
	'1:342 2:341 5:341',
	'the segments get their shares of buckets');

query("SELECT gp.distribute('t2', '{x}')");
is(query("SELECT colocation_id FROM gp.distribution_policy WHERE relid = 't2'::regclass"),
	'1', 'a key of the same type joins the group');

query("SELECT gp.distribute('t3', '{s}')");
query("SELECT gp.distribute('t4', '{a,b}')");
is( query(
		"SELECT string_agg(format('%s:%s:%s', relid, colocation_id, key_columns), ' ' ORDER BY relid::text) FROM gp.distribution_policy WHERE relid IN ('t3'::regclass, 't4'::regclass)"
	),
	't3:2:{1} t4:3:{1,2}',
	'keys of other types make other groups');

query("SELECT gp.distribute('t6', '{y}', colocate_with => 't1')");
is(query("SELECT colocation_id FROM gp.distribution_policy WHERE relid = 't6'::regclass"),
	'1', 'colocate_with joins the group of a table');
fails("SELECT gp.distribute('t5', '{x}', colocate_with => 't1')",
	qr/key of table "public.t5" is \{bigint\}, but key of table "public.t1" is \{integer\}/,
	'colocate_with needs a key of the same types');

query("SELECT gp.distribute('r', NULL)");
is(query("SELECT kind, key_columns, colocation_id FROM gp.distribution_policy WHERE relid = 'r'::regclass"),
	'replicated||', 'a replicated table has no key and no group');

# The segment of a key
is( query(
		'SELECT gp.bucket_segment(1, gp.bucket(1024, 42)) = map[gp.bucket(1024, 42) + 1] FROM gp.colocation_groups WHERE colocation_id = 1'
	),
	't',
	'gp.bucket_segment() reads the map');

# What cannot be distributed
fails("SELECT gp.distribute('t1', '{a}')",
	qr/table "public.t1" is distributed already/, 'a table is distributed once');
query('CREATE TABLE full_table (a int); INSERT INTO full_table VALUES (1)');
fails("SELECT gp.distribute('full_table', '{a}')",
	qr/table "public.full_table" is not empty/, 'a table is distributed empty');
query('CREATE TABLE t7 (a int, p point)');
fails("SELECT gp.distribute('t7', '{nope}')",
	qr/column "nope" of table "public.t7" does not exist/, 'a key of no column');
fails("SELECT gp.distribute('t7', '{a,a}')",
	qr/a key has a column more than once/, 'a key with a column twice');
fails("SELECT gp.distribute('t7', '{p}')",
	qr/type point has no default hash operator class/, 'a key that has no hash');
fails("SELECT gp.distribute('t7', '{}')",
	qr/a key has at least one column/, 'an empty key');
fails("SELECT gp.distribute('t7', NULL, colocate_with => 't1')",
	qr/a replicated table has no colocation group/,
	'a replicated table with colocate_with');
fails("SELECT gp.distribute('r', '{a}', colocate_with => 'r')",
	qr/table "public.r" is distributed already/, 'colocate_with a replicated table');
query('CREATE TABLE t8 (a int)');
fails("SELECT gp.distribute('t8', '{a}', colocate_with => 'r')",
	qr/table "public.r" is not distributed by hash/,
	'colocate_with a table not distributed by hash');
query('CREATE VIEW v AS SELECT 1 AS a');
fails("SELECT gp.distribute('v', '{a}')",
	qr/"public.v" is not a permanent table/, 'a view is not distributed');
fails(
	"CREATE TEMP TABLE tt (a int); SELECT gp.distribute('tt', '{a}')",
	qr/"tt" is not a permanent table/,
	'a temporary table is not distributed');

# A dropped table loses its policy
query('CREATE SCHEMA s; CREATE TABLE s.t (a int)');
query("SELECT gp.distribute('s.t', '{a}')");
query('DROP TABLE t2; DROP SCHEMA s CASCADE');
is( query(
		"SELECT string_agg(relid::text, ' ' ORDER BY relid::text) FROM gp.distribution_policy"
	),
	'r t1 t3 t4 t6',
	'DROP TABLE and DROP SCHEMA CASCADE drop policies');

# The extension goes with its event trigger
query('DROP EXTENSION gp');
pass('DROP EXTENSION works with distributed tables');

$node->stop;

done_testing();
