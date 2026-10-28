CREATE EXTENSION test_new_oid_hook;

-- A proposed OID is used, and the relation's file is named after it
SELECT test_new_oid_hook_propose('pg_class', 4000000000);
CREATE TABLE proposed_oid_table (a int);
SELECT oid, relfilenode FROM pg_class WHERE relname = 'proposed_oid_table';

-- Also for catalogs other than pg_class
SELECT test_new_oid_hook_propose('pg_namespace', 4000000001);
CREATE SCHEMA proposed_oid_schema;
SELECT oid FROM pg_namespace WHERE nspname = 'proposed_oid_schema';

-- A proposed OID in use is an error, not a duplicate
SELECT test_new_oid_hook_propose('pg_class', 4000000000);
CREATE TABLE proposed_oid_in_use (a int);
SELECT count(*) FROM pg_class WHERE oid = 4000000000;

-- So is one in the range of system objects
SELECT test_new_oid_hook_propose('pg_class', 100);
CREATE TABLE proposed_oid_system (a int);

-- A proposed OID that names an existing file gets a relfilenumber of its own
CREATE TABLE truncated_table (a int);
TRUNCATE truncated_table;
SELECT test_new_oid_hook_propose('pg_class', relfilenode)
  FROM pg_class WHERE relname = 'truncated_table';
CREATE TABLE proposed_oid_file (a int);
SELECT p.oid = r.relfilenode AS oid_names_file,
       p.relfilenode <> p.oid AS own_relfilenode
  FROM pg_class p, pg_class r
  WHERE p.relname = 'proposed_oid_file' AND r.relname = 'truncated_table';
INSERT INTO proposed_oid_file VALUES (1);
SELECT * FROM proposed_oid_file;

-- The same for an index, with a file the table above did not take
TRUNCATE truncated_table;
SELECT test_new_oid_hook_propose('pg_class', relfilenode)
  FROM pg_class WHERE relname = 'truncated_table';
CREATE INDEX proposed_oid_index ON proposed_oid_table (a);
SELECT p.oid = r.relfilenode AS oid_names_file,
       p.relfilenode <> p.oid AS own_relfilenode
  FROM pg_class p, pg_class r
  WHERE p.relname = 'proposed_oid_index' AND r.relname = 'truncated_table';

DROP TABLE proposed_oid_table, proposed_oid_file, truncated_table;
DROP SCHEMA proposed_oid_schema;
DROP EXTENSION test_new_oid_hook;
