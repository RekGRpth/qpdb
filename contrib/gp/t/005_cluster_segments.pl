# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The coordinator keeps a copy of gp.segments in shared memory, which
# sessions in any database read, and updates it when a change commits

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('coordinator');
$node->init;
$node->append_conf(
	'postgresql.conf', "shared_preload_libraries = 'gp'
gp.role = 'coordinator'
gp.dispatch_ddl = off
gp.max_segments = 2
max_prepared_transactions = 2
");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION gp');
$node->safe_psql('postgres', 'CREATE DATABASE other');
$node->safe_psql('other', 'CREATE EXTENSION gp');

# The copy as a session of the database other sees it
my $copy = "SELECT coalesce(string_agg(format('%s:%s:%s', segment_id, host, port), ' ' ORDER BY segment_id), '') FROM gp.cluster_segments()";

$node->poll_query_until('other', $copy, '')
  or die 'the copy is not loaded';
pass('the copy is loaded at start');

$node->safe_psql('postgres',
	"SELECT gp.register_segment(1, 'localhost', 9001)");
$node->safe_psql('postgres',
	"SELECT gp.register_segment(2, 'localhost', 9002)");
$node->poll_query_until('other', $copy,
	'1:localhost:9001 2:localhost:9002')
  or die 'the copy has no registered segments';
pass('registered segments are seen in another database');

my ($ret, $stdout, $stderr) = $node->psql('postgres',
	"SELECT gp.register_segment(3, 'localhost', 9003)");
like(
	$stderr,
	qr/cannot register more than 2 segments/,
	'no more segments than gp.max_segments');

($ret, $stdout, $stderr) = $node->psql('postgres',
	"INSERT INTO gp.segments (segment_id, host, port) VALUES (3, repeat('h', 256), 9003)"
);
like($stderr, qr/violates check constraint "segments_host_check"/,
	'no host longer than the copy keeps');

$node->safe_psql('postgres',
	'BEGIN; DELETE FROM gp.segments WHERE segment_id = 2; ROLLBACK');
is($node->safe_psql('other', $copy),
	'1:localhost:9001 2:localhost:9002',
	'a rolled back change keeps the copy');

($ret, $stdout, $stderr) = $node->psql('postgres',
	"BEGIN; DELETE FROM gp.segments WHERE segment_id = 2; PREPARE TRANSACTION 'p'"
);
like(
	$stderr,
	qr/cannot PREPARE a transaction that has changed gp.segments/,
	'a change of gp.segments is not prepared');

$node->safe_psql('postgres', 'DELETE FROM gp.segments WHERE segment_id = 2');
$node->poll_query_until('other', $copy, '1:localhost:9001')
  or die 'the copy has a deleted segment';
pass('a committed delete updates the copy');

# Rows beyond gp.max_segments, inserted around gp.register_segment()
my $offset = -s $node->logfile;
$node->safe_psql('postgres',
	"INSERT INTO gp.segments (segment_id, host, port) VALUES (2, 'localhost', 9002), (3, 'localhost', 9003)"
);
$node->wait_for_log(
	qr/gp.segments has 3 segments, only the first 2 are used/, $offset);
is($node->safe_psql('other', $copy),
	'1:localhost:9001 2:localhost:9002',
	'the copy takes the first gp.max_segments segments');

$node->safe_psql('postgres', 'DELETE FROM gp.segments WHERE segment_id = 3');
$node->restart;
$node->poll_query_until('other', $copy,
	'1:localhost:9001 2:localhost:9002')
  or die 'the copy is not loaded after a restart';
pass('the copy is loaded again after a restart');

$node->safe_psql('postgres', 'BEGIN; DROP EXTENSION gp; ROLLBACK');
is($node->safe_psql('other', $copy),
	'1:localhost:9001 2:localhost:9002',
	'a rolled back DROP EXTENSION keeps the copy');

$node->safe_psql('postgres', 'DROP EXTENSION gp');
$node->poll_query_until('other', $copy, '')
  or die 'the copy keeps the segments of a dropped extension';
pass('DROP EXTENSION in gp.database empties the copy');

$node->safe_psql('postgres', 'CREATE EXTENSION gp');
$node->safe_psql('postgres',
	"SELECT gp.register_segment(1, 'localhost', 9001)");
$node->poll_query_until('other', $copy, '1:localhost:9001')
  or die 'the copy has no segments of a new extension';
$node->safe_psql('postgres', 'DROP SCHEMA gp CASCADE');
$node->poll_query_until('other', $copy, '')
  or die 'the copy keeps the segments of a dropped schema';
pass('DROP SCHEMA gp CASCADE empties the copy');

$node->stop;

done_testing();
