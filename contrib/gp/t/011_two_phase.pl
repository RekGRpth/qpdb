# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The segments end the transactions of DDL by two-phase commit

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

# Segment 2 cannot prepare transactions at first
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
max_prepared_transactions = ${\($segment_id == 1 ? 10 : 0)}
");
	$node->start;
	push @segments, $node;
}
my ($seg1, $seg2) = @segments;
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.cluster_segments()')
  or die 'segments did not register';

sub where
{
	my ($relname) = @_;
	return join ' ',
	  map { $_->safe_psql('postgres', "SELECT to_regclass('$relname') IS NOT NULL") }
	  ($coordinator, @segments);
}

sub prepared
{
	return join ' ',
	  map { $_->safe_psql('postgres', 'SELECT count(*) FROM pg_prepared_xacts') }
	  @segments;
}

# A segment that cannot prepare fails the commit everywhere
my ($ret, $stdout, $stderr) =
  $coordinator->psql('postgres', 'CREATE TABLE t1 (a int)');
like(
	$stderr,
	qr/prepared transactions are disabled\n.*CONTEXT:  preparing the transaction on segment 2 at /s,
	'a segment that cannot prepare fails the commit');
is(scalar(() = $stderr =~ /segment 2 at /g), 1, 'the error has one context');
is(where('t1'), 'f f f', 'the table is nowhere');
is(prepared(), '0 0', 'the prepared transaction of segment 1 is rolled back');

$seg2->append_conf('postgresql.conf', 'max_prepared_transactions = 10');
$seg2->restart;
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.cluster_segments()')
  or die 'segment 2 did not register again';

# Commit and rollback leave no prepared transactions
$coordinator->safe_psql('postgres', 'CREATE TABLE t2 (a int)');
is(where('t2'), 't t t', 'the segments commit the DDL');
is(prepared(), '0 0', 'a commit leaves no prepared transactions');
$coordinator->safe_psql('postgres', 'BEGIN; CREATE TABLE t3 (a int); ROLLBACK');
is(where('t3'), 'f f f', 'the segments roll back the DDL');
is(prepared(), '0 0', 'a rollback leaves no prepared transactions');

# A segment lost between the commit of the coordinator and COMMIT PREPARED
SKIP:
{
	skip 'injection points are not supported by this build', 6
	  unless $ENV{enable_injection_points} eq 'yes';

	$coordinator->safe_psql('postgres',
		"CREATE EXTENSION injection_points;
SELECT injection_points_attach('gp-before-commit-prepared', 'wait')");
	my $session = $coordinator->background_psql('postgres', on_error_stop => 0);
	$session->query_until(qr/start/, "\\echo start\nCREATE TABLE t4 (a int);\n");
	$coordinator->wait_for_event('client backend', 'gp-before-commit-prepared');
	$seg2->stop;
	$coordinator->safe_psql('postgres',
		"SELECT injection_points_detach('gp-before-commit-prepared');
SELECT injection_points_wakeup('gp-before-commit-prepared')");
	$session->query('SELECT 1');
	like(
		$session->{stderr},
		qr/WARNING:  could not run COMMIT PREPARED 'gp_\d+' on segment 2 at .*\nHINT:  The transaction stays prepared on the segment; run COMMIT PREPARED 'gp_\d+' there\./s,
		'a segment that cannot commit gets a warning');
	$session->quit;

	$seg2->start;
	is(where('t4'), 't t f', 'segment 2 has not committed');
	my $gid = $seg2->safe_psql('postgres', 'SELECT gid FROM pg_prepared_xacts');
	like($gid, qr/^gp_\d+$/, 'segment 2 keeps the transaction prepared');
	my ($xid) = $gid =~ /^gp_(\d+)$/;
	is($coordinator->safe_psql('postgres', "SELECT pg_xact_status('$xid')"),
		'committed', 'the coordinator tells the transaction committed');
	$seg2->safe_psql('postgres', "COMMIT PREPARED '$gid'");
	is(where('t4'), 't t t', 'COMMIT PREPARED by hand completes the commit');
	is(prepared(), '0 0', 'no prepared transactions are left');
}

$_->stop foreach (@segments, $coordinator);

done_testing();
