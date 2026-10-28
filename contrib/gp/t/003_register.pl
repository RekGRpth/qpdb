# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Segments register on the coordinator; a misconfigured node does not start

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# A node with the given settings must not start
sub fails_to_start
{
	my ($name, $conf, $message, $test) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);

	$node->init;
	$node->append_conf('postgresql.conf',
		"shared_preload_libraries = 'gp'\n$conf");
	ok(!$node->start(fail_ok => 1), $test);
	like(slurp_file($node->logfile), $message, "$test: message");
}

fails_to_start('segment_without_id', "gp.role = 'segment'\n",
	qr/"gp.segment_id" must be set on a segment/,
	'segment without gp.segment_id');
fails_to_start(
	'coordinator_with_id',
	"gp.role = 'coordinator'\ngp.segment_id = 1\n",
	qr/"gp.segment_id" is set, but "gp.role" is not "segment"/,
	'coordinator with gp.segment_id');
fails_to_start(
	'segment_without_coordinator',
	"gp.role = 'segment'\ngp.segment_id = 1\n",
	qr/"gp.coordinator" must be set on a segment/,
	'segment without gp.coordinator');
fails_to_start(
	'coordinator_with_coordinator',
	"gp.role = 'coordinator'\ngp.coordinator = 'port=1'\n",
	qr/"gp.coordinator" is set, but "gp.role" is not "segment"/,
	'coordinator with gp.coordinator');

my $node = PostgreSQL::Test::Cluster->new('coordinator');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\ngp.dispatch_ddl = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION gp');

# Run gp.register_segment() in postgres, return the error if any
sub register
{
	my ($args, %params) = @_;
	my ($stdout, $stderr);

	$node->psql(
		$params{dbname} // 'postgres',
		"SELECT gp.register_segment($args)",
		stdout => \$stdout,
		stderr => \$stderr,
		extra_params => $params{extra_params} // []);
	return $stderr;
}

sub segments
{
	return $node->safe_psql('postgres',
		'SELECT segment_id, host, port FROM gp.segments ORDER BY 1');
}

is(register("1, 'localhost', 9001"), '', 'register segment 1');
is(register("2, 'localhost', 9002"), '', 'register segment 2');
is(segments(), "1|localhost|9001\n2|localhost|9002", 'segments registered');

my $before = $node->safe_psql('postgres',
	'SELECT registered FROM gp.segments WHERE segment_id = 1');
is(register("1, 'localhost', 9001"), '', 'register segment 1 again');
isnt(
	$node->safe_psql(
		'postgres', 'SELECT registered FROM gp.segments WHERE segment_id = 1'),
	$before,
	'registering again updates the time');

like(
	register("1, 'otherhost', 9001"),
	qr/segment 1 is already registered at localhost:9001/,
	'number registered at another address');
like(
	register("3, 'localhost', 9002"),
	qr/localhost:9002 is already registered as segment 2/,
	'address registered under another number');
like(register("0, 'localhost', 9003"),
	qr/violates check constraint/, 'number of the coordinator');
is(segments(), "1|localhost|9001\n2|localhost|9002",
	'failed registrations change nothing');

$node->safe_psql('postgres', 'CREATE DATABASE other');
$node->safe_psql('other', 'CREATE EXTENSION gp');
like(
	register("3, 'localhost', 9003", dbname => 'other'),
	qr/segments register in database "postgres" only/,
	'register outside gp.database');

$node->safe_psql('postgres', 'CREATE ROLE nobody LOGIN');
like(
	register("3, 'localhost', 9003", extra_params => [ '-U', 'nobody' ]),
	qr/permission denied for (schema gp|function register_segment)/,
	'register as a non-superuser');

$node->stop;

done_testing();
