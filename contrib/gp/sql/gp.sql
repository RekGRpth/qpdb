CREATE EXTENSION gp;
SELECT extnamespace::regnamespace FROM pg_extension WHERE extname = 'gp';

-- Without a role the server works on its own
SHOW gp.role;

-- The role is set at server start
SET gp.role = 'segment';

-- The prefix of the extension is reserved
SET gp.no_such_parameter = 1;

-- The coordinator and a server on its own are number 0
SHOW gp.segment_id;
SET gp.segment_id = 1;

-- Database of the configuration of the cluster
SHOW gp.database;

-- Only a segment connects to the coordinator, with its own host
SHOW gp.coordinator;
SHOW gp.host;
SHOW gp.register_retry_interval;

-- Segments are dumped with the extension
SELECT extconfig::regclass[] FROM pg_extension WHERE extname = 'gp';
SELECT * FROM gp.segments;

-- The coordinator keeps the segments in shared memory
SHOW gp.max_segments;
SELECT * FROM gp.cluster_segments();

-- Segments register on the coordinator only
SELECT gp.register_segment(1, 'localhost', 9001);

-- Only a superuser registers segments by default
SELECT has_function_privilege('public', 'gp.register_segment(int, text, int)',
							  'EXECUTE');

-- Commands go to the segments from the coordinator only, by a superuser
SELECT * FROM gp.exec_on_all('SELECT 1');
SELECT has_function_privilege('public', 'gp.exec_on_all(text)', 'EXECUTE');

-- OIDs are recorded on the coordinator and given to segments
SHOW gp.allow_local_ddl;
SHOW gp.dispatch_oids;
SELECT gp.recorded_oids();
SET gp.dispatch_oids = '1259:16384,';
SET gp.dispatch_oids = '1259:16384,1247:16385';
SET gp.dispatch_oids = '1259:16386';
RESET gp.dispatch_oids;

-- Buckets of keys are the same on every server
SELECT gp.bucket(1024, 1), gp.bucket(1024, 1::bigint), gp.bucket(1024, 'abc'::text),
	   gp.bucket(1024, 1, 'abc'::text), gp.bucket(1024, NULL::int),
	   gp.bucket(1024, NULL::int, 1), gp.bucket(1024, VARIADIC ARRAY[1]);
SELECT count(DISTINCT gp.bucket(16, i)) FROM generate_series(1, 1000) i;
SELECT gp.bucket(0, 1);
SELECT gp.bucket(1024, point(1, 1));

-- Tables are distributed on the coordinator only
CREATE TABLE t (a int);
SELECT gp.distribute('t', '{a}');
DROP TABLE t;

DROP EXTENSION gp;
