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

-- Segments are dumped with the extension
SELECT extconfig::regclass[] FROM pg_extension WHERE extname = 'gp';
SELECT * FROM gp.segments;

-- Segments register on the coordinator only
SELECT gp.register_segment(1, 'localhost', 9001);

-- Only a superuser registers segments by default
SELECT has_function_privilege('public', 'gp.register_segment(int, text, int)',
							  'EXECUTE');

DROP EXTENSION gp;
