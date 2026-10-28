CREATE EXTENSION gp;
SELECT extnamespace::regnamespace FROM pg_extension WHERE extname = 'gp';

-- Without a role the server works on its own
SHOW gp.role;

-- The role is set at server start
SET gp.role = 'segment';

-- The prefix of the extension is reserved
SET gp.no_such_parameter = 1;

DROP EXTENSION gp;
