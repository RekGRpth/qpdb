# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The broker of a segment takes the port of the segment plus 10000 when it
# is free, and any free port when another process holds that one.  The
# broker reports the port it took to the coordinator, and clients connect to
# the port each segment reported, not to the one a rule computes

use strict;
use warnings FATAL => 'all';
use IO::Socket::IP;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\n");
$coordinator->start;
$coordinator->safe_psql('postgres', 'CREATE EXTENSION gp');

# The brokers listen on TCP, where another process can hold the usual port
my $seg1 = PostgreSQL::Test::Cluster->new('seg1');
$seg1->init;
$seg1->append_conf('postgresql.conf', "listen_addresses = '127.0.0.1'
shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = 1
gp.coordinator = 'host=${\$coordinator->host} port=${\$coordinator->port}'
gp.host = '127.0.0.1'
gp.register_retry_interval = 1
max_prepared_transactions = 10
");
$seg1->start;

# The usual port of segment 1 is free: its broker takes it and reports it
$coordinator->poll_query_until('postgres',
	'SELECT interconnect_port = port + 10000 FROM gp.cluster_segments() WHERE segment_id = 1')
  or die 'the broker of segment 1 did not report its port';

# Another process holds the usual port of segment 2 before it starts.  Any
# connection the process answers is a client that ignores the report, which
# counts against the query below
my $seg2 = PostgreSQL::Test::Cluster->new('seg2');
my $busy = IO::Socket::IP->new(
	LocalAddr => '127.0.0.1',
	LocalPort => $seg2->port + 10000,
	Proto => 'tcp',
	Listen => 1,
	Blocking => 0,
	ReuseAddr => 1,
) or die 'could not hold the usual interconnect port of segment 2';
my $hits = 0;
sub drain_busy
{
	while (my $client = $busy->accept)
	{
		$hits++;
		close $client;
	}
}

$seg2->init;
$seg2->append_conf('postgresql.conf', "listen_addresses = '127.0.0.1'
shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = 2
gp.coordinator = 'host=${\$coordinator->host} port=${\$coordinator->port}'
gp.host = '127.0.0.1'
gp.register_retry_interval = 1
max_prepared_transactions = 10
");
$seg2->start;

# The broker of segment 2 takes another port and reports that one instead
$coordinator->poll_query_until('postgres',
	'SELECT interconnect_port IS NOT NULL AND interconnect_port <> port + 10000 FROM gp.cluster_segments() WHERE segment_id = 2')
  or die 'the broker of segment 2 did not take another port';
my ($reported) = $coordinator->safe_psql('postgres',
	'SELECT interconnect_port FROM gp.cluster_segments() WHERE segment_id = 2');
like($reported, qr/^\d+$/, "segment 2 reports its own port $reported");

# A clean query reads both segments through their reported ports
$coordinator->safe_psql('postgres', 'CREATE TABLE t (a int)');
$coordinator->safe_psql('postgres', q{SELECT gp.distribute('t', ARRAY['a'])});
$coordinator->safe_psql('postgres',
	'INSERT INTO t SELECT g FROM generate_series(1, 20) g');
drain_busy();
is($coordinator->safe_psql('postgres', 'SELECT count(*) FROM t'), 20,
	'a query reads both segments through their reported ports');
drain_busy();
is($hits, 0, 'no client connects to the port another process holds');

close($busy);
$_->stop foreach ($seg2, $seg1, $coordinator);

done_testing();
