# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Direct dispatch: an INSERT whose keys are constants that fall in one segment
# goes to that segment alone, and the others are not asked at all

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
log_statement = 'all'
");
	$node->start;
	push @segments, $node;
}
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.cluster_segments()')
  or die 'segments did not register';

sub run { return $coordinator->safe_psql('postgres', shift); }

# What each segment has by a query that gives one value, as "seg1 seg2"
sub on_segments
{
	my ($query) = @_;
	return join ' ', map { $_->safe_psql('postgres', $query) } @segments;
}

# Which segments a command has sent a COPY to, as "seg1 seg2" of 0 and 1
sub asked
{
	my ($sql) = @_;
	my @pos = map { -s $_->logfile } @segments;
	run($sql);
	return join ' ',
	  map { slurp_file($segments[$_]->logfile, $pos[$_]) =~ /COPY / ? 1 : 0 } 0 .. 1;
}

# The segment of a key of table t, as a number
sub segment_of
{
	my ($table, $key) = @_;
	return run(qq{SELECT gp.bucket_segment(p.colocation_id, gp.bucket(1024, $key))
FROM gp.distribution_policy p WHERE p.relid = '$table'::regclass});
}

# As asked() answers for rows that all go to a segment
sub only
{
	my ($segment) = @_;
	return $segment == 1 ? '1 0' : '0 1';
}

run('CREATE TABLE t (a int, b text)');
run(q{SELECT gp.distribute('t', ARRAY['a'])});

# Keys of each segment
my %keys;
foreach my $segment (1 .. 2)
{
	$keys{$segment} = [ split /\n/, run(qq{SELECT g FROM generate_series(1, 200) g
WHERE gp.bucket_segment((SELECT colocation_id FROM gp.distribution_policy
						 WHERE relid = 't'::regclass), gp.bucket(1024, g)) = $segment
ORDER BY g LIMIT 3}) ];
}

# A row with a constant key goes to the segment of its bucket alone
foreach my $segment (1 .. 2)
{
	my $key = $keys{$segment}[0];
	is(asked("INSERT INTO t VALUES ($key, 'x')"), only($segment),
		"a row for segment $segment asks it alone");
}
is(on_segments('SELECT count(*) FROM t'), '1 1', 'and is there');
is(on_segments('SELECT string_agg(a::text, \',\') FROM t'),
	"$keys{1}[0] $keys{2}[0]", 'with its key');

# Several rows of VALUES, all for one segment, or not
my $same = join ', ', map { "($_, 'm')" } @{ $keys{1} }[ 1, 2 ];
is(asked("INSERT INTO t VALUES $same"), '1 0', 'rows of one segment ask it alone');
is(on_segments(q{SELECT count(*) FROM t WHERE b = 'm'}), '2 0', 'and are there');
my $mixed = "($keys{1}[0], 'z'), ($keys{2}[1], 'z')";
is(asked("INSERT INTO t VALUES $mixed"), '1 1', 'rows of two segments ask both');
is(on_segments(q{SELECT count(*) FROM t WHERE b = 'z'}), '1 1', 'and are placed');

# What the planner folds to a constant
my $folded = $keys{2}[2];
is(asked("INSERT INTO t VALUES ($folded - 1 + 1, 'f')"), '0 1',
	'a constant expression of the key');
is(asked("INSERT INTO t SELECT $keys{1}[0], 'q'"), '1 0',
	'a SELECT without a table');
run('CREATE TABLE d (a int DEFAULT 7, b int DEFAULT 8)');
run(q{SELECT gp.distribute('d', ARRAY['a'])});
is(asked('INSERT INTO d DEFAULT VALUES'), only(segment_of('d', 7)),
	'a default is a constant too');

# A NULL key has a bucket
is(asked(q{INSERT INTO t VALUES (NULL, 'n')}), only(segment_of('t', 'NULL::int')),
	'a NULL key goes to the segment of its bucket');
is(on_segments(q{SELECT count(*) FROM t WHERE a IS NULL}),
	segment_of('t', 'NULL::int') == 1 ? '1 0' : '0 1', 'and is there');

# What is not a constant is routed as before
is(asked("INSERT INTO t VALUES ($keys{1}[0] + (random() * 0)::int, 'v')"), '1 1',
	'a key that is not a constant asks all');
is(on_segments(q{SELECT count(*) FROM t WHERE b = 'v'}), '1 0',
	'and is placed');
run('CREATE TABLE src (a int)');
run("INSERT INTO src VALUES ($keys{1}[0])");
is(asked('INSERT INTO t SELECT a, \'s\' FROM src'), '1 1', 'a source that is a table asks all');
is(on_segments(q{SELECT count(*) FROM t WHERE b = 's'}), '1 0', 'and is placed');

# Keys of several columns, text among them
run('CREATE TABLE k (a int, s text)');
run(q{SELECT gp.distribute('k', ARRAY['a', 's'])});
is(asked(q{INSERT INTO k VALUES (1, 'x')}), only(segment_of('k', q{1, 'x'::text})),
	'a key of two columns');
is(asked(q{INSERT INTO k VALUES (2, NULL)}), only(segment_of('k', q{2, NULL::text})),
	'with a NULL among them');

# A replicated table goes to all
run('CREATE TABLE rep (a int)');
run(q{SELECT gp.distribute('rep', NULL)});
is(asked('INSERT INTO rep VALUES (1)'), '1 1', 'a replicated table asks all');
is(on_segments('SELECT count(*) FROM rep'), '1 1', 'and is on all');

# A prepared statement puts the rows where they belong
my @pkeys = (@{ $keys{1} }, @{ $keys{2} });
run(join '; ',
	q{PREPARE p (int) AS INSERT INTO t VALUES ($1, 'p')},
	map { "EXECUTE p ($_)" } @pkeys, @pkeys);
is(on_segments(q{SELECT count(*) FROM t WHERE b = 'p'}), '6 6',
	'a prepared INSERT places its rows');

# Transactions that touch one segment, or two
run('CREATE TABLE tx (a int)');
run(q{SELECT gp.distribute('tx', ARRAY['a'])});
my $k1 = $keys{1}[0];
my $k2 = $keys{2}[0];
run("BEGIN; INSERT INTO tx VALUES ($k1); ROLLBACK");
is(on_segments('SELECT count(*) FROM tx'), '0 0', 'a rollback undoes the row');
run("BEGIN; INSERT INTO tx VALUES ($k1); COMMIT");
is(on_segments('SELECT count(*) FROM tx'), '1 0', 'a commit with one segment');
run("BEGIN; INSERT INTO tx VALUES ($k1 + 0), ($k2); INSERT INTO tx VALUES ($k2); COMMIT");
is(on_segments('SELECT count(*) FROM tx'), '2 2', 'a commit with two segments');
is(on_segments('SELECT count(*) FROM pg_prepared_xacts'), '0 0',
	'leaves no prepared transactions');

$_->stop foreach (@segments, $coordinator);

done_testing();
