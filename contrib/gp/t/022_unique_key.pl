# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A unique index of a distributed table must include the whole key of the
# distribution: a segment sees its own rows only

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\ngp.default_distribution = 'first_column'\n");
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
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.cluster_segments()')
  or die 'segments did not register';

sub run { return $coordinator->safe_psql('postgres', shift); }

sub fails
{
	my ($sql, $message, $test) = @_;
	my ($ret, $stdout, $stderr) = $coordinator->psql('postgres', $sql);
	like($stderr, $message, $test);
}

# A table distributed by hand: an index that covers the key works, one
# without it does not.  The setting leaves these tables on the coordinator,
# for gp.distribute() to place them.
$coordinator->safe_psql('postgres',
	"ALTER DATABASE postgres SET gp.default_distribution = 'coordinator'");
run('CREATE TABLE u1 (a int, b int)');
run("SELECT gp.distribute('u1', ARRAY['a'])");
run('ALTER TABLE u1 ADD UNIQUE (a, b)');
fails('ALTER TABLE u1 ADD UNIQUE (b)',
	qr/unique indexes of table .*u1.* must include the whole key/,
	'a unique index without the key of a distributed table');
run('DROP TABLE IF EXISTS u1');

# Distributing by a column of every unique index works; by another column
# does not
run('CREATE TABLE u2 (a int, b int)');
run('ALTER TABLE u2 ADD UNIQUE (b)');
run("SELECT gp.distribute('u2', ARRAY['b'])");
run('CREATE TABLE u3 (a int, b int)');
run('ALTER TABLE u3 ADD UNIQUE (b)');
fails("SELECT gp.distribute('u3', ARRAY['a'])",
	qr/unique indexes of table .*u3.* must include the whole key/,
	'distributing by a column the unique index lacks');
run('DROP TABLE IF EXISTS u2, u3');

# A key of two columns needs the whole key in the index
run('CREATE TABLE u8 (a int, b int, c int)');
run("SELECT gp.distribute('u8', ARRAY['a', 'b'])");
run('ALTER TABLE u8 ADD UNIQUE (a, b)');
run('CREATE TABLE u9 (a int, b int, c int)');
run("SELECT gp.distribute('u9', ARRAY['a', 'b'])");
fails('ALTER TABLE u9 ADD UNIQUE (a)',
	qr/must include the whole key/,
	'half of the key does not cover it');
run('DROP TABLE IF EXISTS u8, u9');

# A replicated table knows no key: any unique index works
run('CREATE TABLE r1 (a int, b int)');
run("SELECT gp.distribute('r1', NULL)");
run('ALTER TABLE r1 ADD UNIQUE (b)');
run('DROP TABLE r1');

# A table of the default distribution: the first column lands in the index
$coordinator->safe_psql('postgres',
	"ALTER DATABASE postgres SET gp.default_distribution = 'first_column'");
run('CREATE TABLE u4 (a int UNIQUE, b int)');
run('DROP TABLE u4');
fails('CREATE TABLE u5 (a int, b int UNIQUE)',
	qr/unique indexes of table .*u5.* must include the whole key/,
	'CREATE TABLE with a unique index without the key');
run('DROP TABLE IF EXISTS u5');
run('CREATE TABLE u6 (a int, b int)');
fails('CREATE UNIQUE INDEX ON u6 (b)',
	qr/unique indexes of table .*u6.* must include the whole key/,
	'CREATE UNIQUE INDEX without the key');
run('DROP TABLE IF EXISTS u6');
run('CREATE TABLE u7 (a int, b int)');
fails('ALTER TABLE u7 ADD PRIMARY KEY (b)',
	qr/unique indexes of table .*u7.* must include the whole key/,
	'a primary key without the key');
run('ALTER TABLE u7 ADD UNIQUE (a, b)');
run('DROP TABLE u7');






# The uniqueness itself: equal keys sit on one segment, where the index
# tells them apart
run('CREATE TABLE u10 (a int UNIQUE)');
run('INSERT INTO u10 VALUES (1), (2), (3)');
fails('INSERT INTO u10 VALUES (1)',
	qr/duplicate key value violates unique constraint/,
	'the segment catches equal keys');
run('DROP TABLE u10');

# What stays on the coordinator knows nothing of the key
$coordinator->safe_psql('postgres',
	"ALTER DATABASE postgres SET gp.default_distribution = 'coordinator'");
run('CREATE TABLE c1 (a int, b int UNIQUE)');
run('DROP TABLE c1');

$_->stop foreach (@segments, $coordinator);

done_testing();
