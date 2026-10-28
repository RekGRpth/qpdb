# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The segments get the settings of the session of the coordinator

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

my $segment = PostgreSQL::Test::Cluster->new('seg1');
$segment->init;
$segment->append_conf(
	'postgresql.conf', "shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = 1
gp.coordinator = 'host=${\$coordinator->host} port=${\$coordinator->port}'
gp.host = '${\$segment->host}'
gp.register_retry_interval = 1
log_statement = 'all'
");
$segment->start;
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 1 FROM gp.cluster_segments()')
  or die 'the segment did not register';

# Schemas for the tests, and one where anybody may leave what it sees
$segment->safe_psql('postgres',
	'CREATE SCHEMA s; CREATE SCHEMA probe; GRANT ALL ON SCHEMA probe TO PUBLIC'
);

my $session = $coordinator->background_psql('postgres', on_error_stop => 0);

# The value of an expression on the segment, through the session
sub on_segment
{
	my ($expr) = @_;

	$session->query_safe(
		"SELECT gp.exec_on_all(\$\$CREATE TABLE probe.v AS SELECT ($expr)::text AS v\$\$)"
	);
	my $value = $segment->safe_psql('postgres', 'SELECT v FROM probe.v');
	$segment->safe_psql('postgres', 'DROP TABLE probe.v');
	return $value;
}

my $default_work_mem = on_segment("current_setting('work_mem')");

$session->query_safe("SET search_path = s, public");
is(on_segment("current_setting('search_path')"),
	's, public', 'a list setting reaches the segment');

$session->query_safe("SET work_mem = '64MB'");
is(on_segment("current_setting('work_mem')"),
	'64MB', 'a setting with units reaches the segment');

$session->query_safe("SET myapp.x = 'hello'");
is(on_segment("current_setting('myapp.x')"),
	'hello', 'a custom setting reaches the segment');

is(on_segment("current_setting('application_name')"),
	'gp coordinator', 'the connection keeps its application_name');

$session->query_safe("BEGIN; SET LOCAL work_mem = '8MB'");
is(on_segment("current_setting('work_mem')"),
	'8MB', 'SET LOCAL reaches the segment');
$session->query_safe('COMMIT');
is(on_segment("current_setting('work_mem')"),
	'64MB', 'the value of the session is back after the transaction');

$session->query_safe('RESET work_mem');
is(on_segment("current_setting('work_mem')"),
	$default_work_mem, 'RESET reaches the segment');

# A setting the segment refuses names the segment
$coordinator->safe_psql(
	'postgres', 'CREATE ROLE alice;
GRANT USAGE ON SCHEMA gp TO alice;
GRANT EXECUTE ON FUNCTION gp.exec_on_all(text) TO alice');
$session->query_safe('SET ROLE alice');
my ($output, $failed) =
  $session->query("SELECT gp.exec_on_all('SELECT 1')");
ok($failed, 'a role unknown on the segment fails the command');
like(
	$session->{stderr},
	qr/role "alice" does not exist\nCONTEXT:  setting the parameters of the session on segment 1 at /,
	'the error names the segment');
$session->{stderr} = '';

$segment->safe_psql('postgres', 'CREATE ROLE alice');
is(on_segment('current_user'), 'alice', 'SET ROLE reaches the segment');
$session->query_safe('RESET ROLE');
is(on_segment('current_user'),
	$coordinator->safe_psql('postgres', 'SELECT current_user'),
	'RESET ROLE reaches the segment');

# Settings are sent only when they change
my $offset = -s $segment->logfile;
on_segment('1');
on_segment('2');
unlike(slurp_file($segment->logfile, $offset),
	qr/set_config/, 'unchanged settings are not sent again');

$session->quit;
$segment->stop;
$coordinator->stop;

done_testing();
