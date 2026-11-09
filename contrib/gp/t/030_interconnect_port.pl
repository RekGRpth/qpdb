# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The broker of a segment takes any free TCP port and reports it to the
# coordinator; clients connect to the reported port alone, and no rule
# computes an address.  A Unix-domain broker keeps its usual socket in
# the directory of its own segment

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Connections that reach the server holding a port by mistake: the
# interconnect protocol is no startup packet, and the server says so
sub holder_complaints
{
	my ($holder) = @_;
	open(my $log, '<', $holder->logfile) or return 0;
	my $hits = 0;
	while (<$log>)
	{
		$hits++ if /invalid (frontend message type|length of startup packet)/
			|| /incomplete startup packet/;
	}
	close($log);
	return $hits;
}

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\n");
$coordinator->start;
$coordinator->safe_psql('postgres', 'CREATE EXTENSION gp');

# The brokers listen on TCP, where the port of the server says nothing
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

# Another server sits on the port of segment 2 plus 10000, the address a
# rule would compute: nothing of the cluster may ever go there
my $seg2 = PostgreSQL::Test::Cluster->new('seg2');
my $holder = PostgreSQL::Test::Cluster->new('holder',
	port => $seg2->port + 10000);
$holder->init;
$holder->append_conf('postgresql.conf', "listen_addresses = '*'");
$holder->start;

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

# Each segment reports the free port its broker took; the holder keeps
# the one a rule would compute, so the report cannot be that one
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.cluster_segments() WHERE interconnect_port IS NOT NULL')
  or die 'the brokers did not report their ports';
my ($seg2_port, $seg2_ic) = split /\|/, $coordinator->safe_psql('postgres',
	'SELECT port, interconnect_port FROM gp.cluster_segments() WHERE segment_id = 2');
isnt($seg2_ic, $seg2_port + 10000,
	"segment 2 reports its own port $seg2_ic, not $seg2_port + 10000");

# A clean query reads both segments through their reported ports; a client
# that ignored the report would speak to the server holding the port
$coordinator->safe_psql('postgres', 'CREATE TABLE t (a int)');
$coordinator->safe_psql('postgres', q{SELECT gp.distribute('t', ARRAY['a'])});
$coordinator->safe_psql('postgres',
	'INSERT INTO t SELECT g FROM generate_series(1, 20) g');
my $complaints = holder_complaints($holder);
is($coordinator->safe_psql('postgres', 'SELECT count(*) FROM t'), 20,
	'a query reads both segments through their reported ports');
is(holder_complaints($holder), $complaints,
	'no client connects to the port another server holds');

$_->stop foreach ($holder, $seg2, $seg1, $coordinator);

done_testing();
