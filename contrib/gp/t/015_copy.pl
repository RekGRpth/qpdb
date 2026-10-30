# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# COPY FROM into a distributed table: the coordinator parses the rows and
# sends each to the segment of its bucket; a replicated table gets all rows
# on every segment

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

# Compare what the segments have, by a query that gives one value, with what
# the coordinator computes for each segment: a query of "segment|value" rows
sub placed
{
	my ($expected, $query, $test) = @_;
	my %want = map { split /\|/, $_, 2 } split /\n/, run($expected);
	is( on_segments($query),
		join(' ', map { $want{$_} // '' } 1 .. 2),
		$test);
}

# Rows of a table with an integer key
run(q{CREATE TABLE t (a int, b text DEFAULT 'd',
					   c int GENERATED ALWAYS AS (a * 2) STORED)});
run(q{SELECT gp.distribute('t', ARRAY['a'])});

my ($ret, $stdout, $stderr) = $coordinator->psql('postgres',
	q{DO $$ DECLARE n bigint; BEGIN
COPY t (a) FROM PROGRAM 'seq 1 1000';
GET DIAGNOSTICS n = ROW_COUNT; RAISE NOTICE 'rows %', n; END $$});
like($stderr, qr/NOTICE:  rows 1000/, 'the number of rows is that of the input');
is(run('SELECT count(*) FROM t'), 0, 'the coordinator has no rows');
my $by_segment = q{
SELECT gp.bucket_segment(p.colocation_id, gp.bucket(1024, g)),
	   string_agg(g::text, ',' ORDER BY g)
FROM generate_series(1, 1000) g, gp.distribution_policy p
WHERE p.relid = 't'::regclass GROUP BY 1 ORDER BY 1};
placed($by_segment, 'SELECT string_agg(a::text, \',\' ORDER BY a) FROM t',
	'each row is on the segment of its bucket');
like(on_segments('SELECT count(*) FROM t'), qr/^[1-9]\d* [1-9]\d*$/,
	'both segments have rows');
is(on_segments(q{SELECT count(*) FROM t WHERE b = 'd'}),
	on_segments('SELECT count(*) FROM t'), 'the default is that of the table');
is(on_segments('SELECT count(*) FROM t WHERE c <> a * 2'), '0 0',
	'a generated column is computed on the segment');

# A key of several columns, with NULLs, in CSV on the standard input
run('CREATE TABLE k (a int, s text)');
run(q{SELECT gp.distribute('k', ARRAY['a', 's'])});
run(qq{COPY k FROM STDIN (FORMAT csv);
1,x
2,
3,"y z"
,w
5,""
\\.
});
placed(
	q{SELECT gp.bucket_segment(p.colocation_id, gp.bucket(1024, v.a, v.s)),
	   string_agg(coalesce(v.a::text, '~') || '|' || coalesce(v.s, '~'), ','
				  ORDER BY coalesce(v.a, 0), v.s)
FROM (VALUES (1, 'x'), (2, NULL), (3, 'y z'), (NULL, 'w'), (5, '')) v(a, s),
	 gp.distribution_policy p
WHERE p.relid = 'k'::regclass GROUP BY 1 ORDER BY 1},
	q{SELECT string_agg(coalesce(a::text, '~') || '|' || coalesce(s, '~'), ','
					ORDER BY coalesce(a, 0), s) FROM k},
	'a key of two columns with NULLs');

# Every type goes as the segment takes it
run(q{CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy')});
run(q{CREATE TABLE ty (id int, m mood, arr int[], ts timestamptz, n numeric,
					   j jsonb, tx text)});
run(q{SELECT gp.distribute('ty', ARRAY['id'])});
my $rows = qq{1\tsad\t{1,2,3}\t2026-10-30 12:00:00+00\t1.50\t{"a": [1, 2]}\tone
2\tok\t{}\t2026-01-01 00:00:00+00\t-0.001\t[]\ttwo\\ttab
3\thappy\t{NULL,5}\tinfinity\t1e20\tnull\t\\N
4\t\\N\t\\N\t\\N\t\\N\t\\N\t\\N
};
run("COPY ty FROM STDIN;\n${rows}\\.\n");
run('CREATE TABLE ty_ref (LIKE ty)');
run("COPY ty_ref FROM STDIN;\n${rows}\\.\n");
my @want = sort split /\n/,
  run(q{SELECT md5(r::text) FROM ty_ref r});
my @got = sort map { split /\n/, $_->safe_psql('postgres', q{SELECT md5(r::text) FROM ty r}) } @segments;
is_deeply(\@got, \@want, 'rows of many types are as the coordinator parses them');
is(scalar @got, 4, 'there are all four rows');

# A replicated table has all rows on every segment
run('CREATE TABLE rep (a int)');
run(q{SELECT gp.distribute('rep', NULL)});
run(q{COPY rep FROM PROGRAM 'seq 1 50'});
is(on_segments('SELECT count(*) FROM rep'), '50 50', 'rows of a replicated table');
is(on_segments('SELECT sum(a) FROM rep'), '1275 1275', 'on every segment');

# A transaction of the coordinator covers the COPY
run('CREATE TABLE tx (a int)');
run(q{SELECT gp.distribute('tx', ARRAY['a'])});
run(qq{BEGIN;
COPY tx FROM PROGRAM 'seq 1 100';
ROLLBACK});
is(on_segments('SELECT count(*) FROM tx'), '0 0', 'a rollback undoes the COPY');
run(qq{BEGIN;
COPY tx FROM PROGRAM 'seq 1 100';
COPY tx FROM PROGRAM 'seq 101 200';
COMMIT});
my $total = 0;
$total += $_ foreach split / /, on_segments('SELECT count(*) FROM tx');
is($total, 200, 'a commit keeps the rows of two COPY commands');
fails(qq{BEGIN;
SAVEPOINT s;
COPY tx FROM PROGRAM 'seq 201 300';
ROLLBACK TO s;
COMMIT},
	qr/cannot commit a transaction whose DDL the segments have lost/,
	'a rolled back subtransaction with COPY fails the commit');

# Errors
run('CREATE TABLE e (a int)');
run(q{SELECT gp.distribute('e', ARRAY['a'])});
fails(q{COPY e FROM PROGRAM 'printf "1\n2\nx\n"'},
	qr/invalid input syntax for type integer: "x"/,
	'a row that the coordinator cannot parse');
is(on_segments('SELECT count(*) FROM e'), '0 0', 'no row of it stays');

run('CREATE TABLE chk (a int CHECK (a < 100))');
run(q{SELECT gp.distribute('chk', ARRAY['a'])});
fails(q{COPY chk FROM PROGRAM 'seq 1 500'},
	qr/violates check constraint "chk_a_check"\n.*segment \d at /s,
	'a row that a segment refuses');
is(on_segments('SELECT count(*) FROM chk'), '0 0', 'no row of it stays');

fails(q{COPY t FROM PROGRAM 'seq 1 5' WHERE a > 1},
	qr/COPY FROM with WHERE is not supported on a distributed table/,
	'WHERE is refused');
fails(q{COPY t (a) FROM PROGRAM 'seq 1 5' (ON_ERROR ignore)},
	qr/ON_ERROR other than stop is not supported/, 'ON_ERROR ignore is refused');

# Privileges are those of the user, on the coordinator and on the segments
run('CREATE ROLE u1 LOGIN');
run('CREATE ROLE u2 LOGIN');
run('GRANT INSERT ON e TO u1');
($ret, $stdout, $stderr) = $coordinator->psql('postgres',
	qq{COPY e FROM STDIN;\n1\n2\n3\n\\.\n}, extra_params => [ '-U', 'u1' ]);
is($ret, 0, 'a user with INSERT can COPY');
$total = 0;
$total += $_ foreach split / /, on_segments('SELECT count(*) FROM e');
is($total, 3, 'and the rows are on the segments');
($ret, $stdout, $stderr) = $coordinator->psql('postgres',
	qq{COPY e FROM STDIN;\n4\n\\.\n}, extra_params => [ '-U', 'u2' ]);
like($stderr, qr/permission denied for table e/, 'a user without INSERT cannot');

# A table that is not distributed is as before
run('CREATE TABLE loc (a int)');
run(q{COPY loc FROM PROGRAM 'seq 1 7'});
is(run('SELECT count(*) FROM loc'), 7, 'a local table keeps its rows on the coordinator');
is(on_segments('SELECT count(*) FROM loc'), '0 0', 'and has none on the segments');

$_->stop foreach (@segments, $coordinator);

done_testing();
