# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A query that the coordinator gives up does not leave its fragments
# running on the segments: the client of every interconnect request asks
# the broker to stop its worker, and a worker dies on the signal from
# the middle of a long fragment too, not only between two rows

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
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.cluster_segments() WHERE interconnect_port IS NOT NULL')
  or die 'the segments did not report their interconnect ports';

$coordinator->safe_psql('postgres', 'CREATE TABLE t (a int)');
$coordinator->safe_psql('postgres', q{SELECT gp.distribute('t', ARRAY['a'])});
$coordinator->safe_psql('postgres',
	'INSERT INTO t SELECT g FROM generate_series(1, 10) g');

# The fragments of the next query stop before they begin, and stay there
# until the test lets them go
foreach my $segment (@segments)
{
	$segment->safe_psql('postgres',
		"SET gp.allow_local_ddl = on;
CREATE EXTENSION injection_points;
SELECT injection_points_attach('gp-interconnect-fragment', 'wait')");
}

my $session = $coordinator->background_psql('postgres', on_error_stop => 0);
$session->query_safe("SET statement_timeout = '2s'");
$session->query("SELECT count(*) FROM t");
like($session->{stderr}, qr/canceling statement due to statement timeout/,
	'the query that never gets a row dies of its own timeout');

# The workers of the dead query are gone, though no row ever came to
# tell them so
foreach my $segment (@segments)
{
	$segment->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'gp interconnect request'")
	  or die 'the worker of the cancelled query is still running on a segment';
}
pass('the segments stop the workers of a cancelled query');

# A next query runs as before, through the same brokers and slots
foreach my $segment (@segments)
{
	$segment->safe_psql('postgres',
		"SELECT injection_points_detach('gp-interconnect-fragment')");
}
is($coordinator->safe_psql('postgres', 'SELECT count(*) FROM t'), 10,
	'a query after the cancellation reads the same rows');
$session->quit;

$_->stop foreach (@segments, $coordinator);

done_testing();
