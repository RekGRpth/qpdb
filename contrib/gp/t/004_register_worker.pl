# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A segment registers itself on the coordinator in the background, retrying
# until the coordinator is up and has the extension

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\n");

sub new_segment
{
	my ($name, $segment_id) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);

	$node->init;
	$node->append_conf(
		'postgresql.conf', "shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = $segment_id
gp.coordinator = 'host=${\$coordinator->host} port=${\$coordinator->port}'
gp.host = '${\$node->host}'
gp.register_retry_interval = 1
");
	return $node;
}

sub segments
{
	return $coordinator->safe_psql('postgres',
		'SELECT segment_id, host, port FROM gp.segments ORDER BY 1');
}

# Segments start before the coordinator
my @segments = (new_segment('seg1', 1), new_segment('seg2', 2));
$_->start foreach @segments;
my ($seg1, $seg2) = @segments;

$seg1->wait_for_log(qr/could not connect to the coordinator/);
pass('a segment retries while the coordinator is down');

$coordinator->start;
$seg1->wait_for_log(
	qr/could not register as segment 1 on the coordinator: .*does not exist/);
pass('a segment retries while the coordinator has no extension');

$coordinator->safe_psql('postgres', 'CREATE EXTENSION gp');
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.segments')
  or die 'segments did not register';
is( segments(),
	join("\n",
		map { $_ + 1 . '|' . $segments[$_]->host . '|' . $segments[$_]->port }
		  0 .. 1),
	'both segments registered');
$seg1->wait_for_log(
	qr/registered as segment 1 on the coordinator at \Q${\$seg1->host}\E:${\$seg1->port}/
);
pass('a segment logs its registration');

# The worker exits for good once the segment is registered
$seg1->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'gp register segment'"
) or die 'the worker is still running';
pass('the worker exits after the registration');

# A restarted segment registers again
my $before = $coordinator->safe_psql('postgres',
	'SELECT registered FROM gp.segments WHERE segment_id = 1');
$seg1->restart;
$coordinator->poll_query_until('postgres',
	"SELECT registered > '$before' FROM gp.segments WHERE segment_id = 1")
  or die 'segment 1 did not register again';
pass('a restarted segment registers again');

# A segment with the number of another one does not take its place
my $seg3 = new_segment('seg3', 1);
$seg3->start;
$seg3->wait_for_log(
	qr/could not register as segment 1 on the coordinator: .*segment 1 is already registered at/
);
is( segments(),
	join("\n",
		map { $_ + 1 . '|' . $segments[$_]->host . '|' . $segments[$_]->port }
		  0 .. 1),
	'a number registered at another address is refused');

$_->stop foreach ($seg3, @segments, $coordinator);

done_testing();
