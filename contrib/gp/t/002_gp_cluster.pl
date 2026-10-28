# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# tools/gp-cluster.sh creates, starts, stops and removes a demo cluster

use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'gp-cluster.sh is a shell script' if $windows_os;

my $script = dirname(__FILE__) . '/../tools/gp-cluster.sh';
my $segments = 2;

# The script takes neighbouring ports.  get_free_port() goes up from the
# last port it has given and reserves each one, so call it until the ports
# are neighbours.
my @ports = (PostgreSQL::Test::Cluster::get_free_port());
while (@ports < $segments + 1)
{
	my $port = PostgreSQL::Test::Cluster::get_free_port();
	@ports = $port == $ports[-1] + 1 ? (@ports, $port) : ($port);
}
my $port = $ports[0];

my $dir = PostgreSQL::Test::Utils::tempdir() . '/cluster';
$ENV{GP_CLUSTER_DIR} = $dir;
$ENV{GP_PORT} = $port;
$ENV{GP_SEGMENTS} = $segments;
delete $ENV{PGBIN};

# Do not leave the servers running if the test fails half-way
END
{
	system($script, 'stop') if -f "$dir/nodes";
}

# Lines of status, segments first
sub status
{
	my ($state) = @_;
	my @lines;

	foreach my $i (0 .. $segments - 1)
	{
		push @lines, "seg$i\tport " . ($port + 1 + $i) . "\t$state";
	}
	push @lines, "coordinator\tport $port\t$state";
	if ($state eq 'running')
	{
		$_ .= "\tgp.role = segment" foreach @lines[ 0 .. $segments - 1 ];
		$lines[-1] .= "\tgp.role = coordinator";
	}
	return join('', map { "$_\n" } @lines);
}

command_fails_like([ $script, 'start' ],
	qr/no cluster in \Q$dir\E, run init first/,
	'start without a cluster');
command_fails_like([ $script, 'status' ],
	qr/run init first/, 'status without a cluster');
command_fails_like([ $script, 'destroy' ],
	qr/run init first/, 'destroy without a cluster');
command_fails_like([ $script, 'bogus' ], qr/Usage:/, 'unknown command');

command_ok([ $script, 'init' ], 'init');
command_fails_like([ $script, 'init' ],
	qr/\Q$dir\E already exists/,
	'init of an existing cluster');

command_like([ $script, 'status' ],
	qr/\A\Q${\status('stopped')}\E\z/,
	'status after init');
command_like([ $script, 'start' ],
	qr/^seg0 started on port ${\($port + 1)}\n.*^coordinator started on port $port$/ms,
	'start, segments first');
command_like([ $script, 'start' ],
	qr/^coordinator is already running$/m,
	'start of a running cluster');
command_like([ $script, 'status' ],
	qr/\A\Q${\status('running')}\E\z/,
	'status of a running cluster');

command_like([ $script, 'stop' ],
	qr/\Acoordinator stopped\n.*^seg0 stopped\n\z/ms,
	'stop, coordinator first');
command_like([ $script, 'status' ],
	qr/\A\Q${\status('stopped')}\E\z/,
	'status after stop');

command_ok([ $script, 'start' ], 'start again');
command_like([ $script, 'destroy' ], qr/^removed \Q$dir\E$/m, 'destroy');
ok(!-e $dir, 'destroy removes the cluster directory');

done_testing();
