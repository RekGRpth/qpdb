# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Roles, privileges, comments and owners of the coordinator on the segments

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
	'SELECT count(*) = 2 FROM gp.cluster_segments()')
  or die 'segments did not register';

sub run { return $coordinator->safe_psql('postgres', shift); }

sub fails
{
	my ($sql, $message, $test) = @_;
	my ($ret, $stdout, $stderr) = $coordinator->psql('postgres', $sql);
	like($stderr, $message, $test);
}

# Compare a query on the coordinator and on each segment
sub same
{
	my ($query, $test) = @_;
	my $expected = run($query);
	is($_->safe_psql('postgres', $query), $expected, "$test on " . $_->name)
	  foreach @segments;
	return $expected;
}

my $catalog = q{
SELECT 'role', oid::int, rolname, rolcanlogin || ' ' || rolconnlimit ||
	   ' ' || (rolpassword IS NOT NULL)
FROM pg_authid WHERE rolname IN ('r1', 'r2', 'g1')
UNION ALL SELECT 'member', oid::int, roleid::regrole || ' ' || member::regrole,
	   admin_option::text
FROM pg_auth_members
WHERE roleid::regrole::text IN ('r2', 'g1') OR member::regrole::text IN ('r2', 'g1')
UNION ALL SELECT 'setting', setrole::int, setrole::regrole::text,
	   array_to_string(setconfig, ',')
FROM pg_db_role_setting WHERE setrole::regrole::text IN ('r1', 'r2')
UNION ALL SELECT 'schema', oid::int, nspname, nspowner::regrole || ' ' || coalesce(nspacl::text, '')
FROM pg_namespace WHERE nspname = 's'
UNION ALL SELECT 'class', oid::int, relname, relowner::regrole || ' ' || coalesce(relacl::text, '')
FROM pg_class WHERE relnamespace = 's'::regnamespace
UNION ALL SELECT 'column', attrelid::int, attname, attacl::text
FROM pg_attribute WHERE attrelid = 's.t'::regclass AND attacl IS NOT NULL
UNION ALL SELECT 'proc', oid::int, proname, proowner::regrole || ' ' || coalesce(proacl::text, '')
FROM pg_proc WHERE pronamespace = 's'::regnamespace
UNION ALL SELECT 'type', oid::int, typname, typowner::regrole || ' ' || coalesce(typacl::text, '')
FROM pg_type WHERE oid = 's.ty'::regtype
UNION ALL SELECT 'database', 0, datname, coalesce(datacl::text, '')
FROM pg_database WHERE datname = 'postgres'
UNION ALL SELECT 'default', oid::int, defaclobjtype::text, defaclacl::text
FROM pg_default_acl
UNION ALL SELECT 'comment', objoid::int, classoid::regclass || ' ' || objsubid, description
FROM pg_description
WHERE objoid IN (SELECT oid FROM pg_class WHERE relnamespace = 's'::regnamespace
				 UNION ALL SELECT oid FROM pg_proc WHERE pronamespace = 's'::regnamespace
				 UNION ALL SELECT 's'::regnamespace
				 UNION ALL SELECT 's.ty'::regtype)
UNION ALL SELECT 'shcomment', objoid::int, classoid::regclass::text, description
FROM pg_shdescription WHERE objoid IN (SELECT oid FROM pg_authid WHERE rolname = 'r2')
ORDER BY 1, 2, 3};

# Roles
run("CREATE ROLE r1 LOGIN PASSWORD 'secret' CONNECTION LIMIT 5");
run('ALTER ROLE r1 NOLOGIN');
run("ALTER ROLE r1 SET work_mem = '8MB'");
run('ALTER ROLE r1 RENAME TO r2');
run('CREATE ROLE g1');
run('GRANT g1 TO r2 WITH ADMIN OPTION');

# Objects, their privileges, comments and owners
run('CREATE SCHEMA s');
run('CREATE TABLE s.t (a int, b text)');
run('CREATE FUNCTION s.f() RETURNS int LANGUAGE sql RETURN 1');
run('CREATE TYPE s.ty AS (x int)');
run('CREATE SEQUENCE s.q');
run('GRANT USAGE ON SCHEMA s TO r2');
run('GRANT SELECT, INSERT ON s.t TO r2');
run('REVOKE INSERT ON s.t FROM r2');
run('GRANT SELECT (a) ON s.t TO g1');
run('GRANT EXECUTE ON FUNCTION s.f() TO r2');
run('GRANT USAGE ON TYPE s.ty TO r2');
run('GRANT USAGE ON SEQUENCE s.q TO r2');
run('GRANT SELECT ON ALL SEQUENCES IN SCHEMA s TO g1');
run('GRANT CONNECT ON DATABASE postgres TO r2');
run('ALTER DEFAULT PRIVILEGES IN SCHEMA s GRANT SELECT ON TABLES TO g1');
run("COMMENT ON SCHEMA s IS 'a schema'");
run("COMMENT ON TABLE s.t IS 'a table'");
run("COMMENT ON COLUMN s.t.a IS 'a column'");
run("COMMENT ON FUNCTION s.f() IS 'a function'");
run("COMMENT ON TYPE s.ty IS 'a type'");
run("COMMENT ON ROLE r2 IS 'a role'");
run('ALTER TABLE s.t OWNER TO r2');
run('ALTER FUNCTION s.f() OWNER TO r2');
run('ALTER TYPE s.ty OWNER TO r2');
run('ALTER SCHEMA s OWNER TO r2');

my $expected = same($catalog, 'roles, privileges, comments and owners');
like($expected, qr/^role\|\d+\|r2\|false 5 true$/m, 'the role has a password');
like($expected, qr/^member\|\d+\|g1 r2\|true$/m, 'the role is a member');
like($expected, qr/^column\|\d+\|a\|\{g1=r\//m, 'the column has privileges');
like($expected, qr/^shcomment\|\d+\|pg_authid\|a role$/m, 'the role has a comment');

# Objects of extensions and temporary objects stay on the coordinator
run('GRANT USAGE ON SCHEMA gp TO r2');
run('GRANT EXECUTE ON FUNCTION gp.cluster_segments() TO r2');
run("COMMENT ON FUNCTION gp.cluster_segments() IS 'segments'");
run(q{CREATE TEMP TABLE tt (a int);
GRANT SELECT ON tt TO r2;
COMMENT ON TABLE tt IS 'temporary'});
pass('privileges and comments of the coordinator only stay there');
fails('GRANT USAGE ON SCHEMA gp, s TO g1',
	qr/cannot operate on objects of the coordinator and of the cluster at once/,
	'objects of both kinds in one GRANT');

# Roles go away
run('REASSIGN OWNED BY r2 TO CURRENT_USER');
run('DROP OWNED BY r2, g1');
run('DROP ROLE r2, g1');
same(q{SELECT count(*) FROM pg_authid WHERE rolname IN ('r2', 'g1')},
	'roles are dropped');

# The schema of extension gp is not on the segments
run('DROP SCHEMA gp CASCADE');
pass('DROP SCHEMA of the schema of extension gp stays on the coordinator');

$_->stop foreach (@segments, $coordinator);

done_testing();
