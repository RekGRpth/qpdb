/* contrib/gp/gp--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION gp" to load this file. \quit

-- Segments of the cluster, numbered from 1; the coordinator is 0.  The
-- table is used in the database named by gp.database on the coordinator;
-- segments add themselves to it with gp.register_segment().
CREATE TABLE gp.segments (
	segment_id int PRIMARY KEY CHECK (segment_id > 0),
	host text NOT NULL CHECK (octet_length(host) < 256),
	port int NOT NULL CHECK (port BETWEEN 1 AND 65535),
	registered timestamptz NOT NULL DEFAULT now(),	-- last registration
	UNIQUE (host, port)
);

SELECT pg_catalog.pg_extension_config_dump('gp.segments', '');

-- The coordinator keeps a copy of gp.segments in shared memory, for any
-- database.  A transaction that changes the table updates the copy when it
-- commits.
CREATE FUNCTION gp.segments_changed()
RETURNS trigger
AS 'MODULE_PATHNAME', 'gp_segments_changed'
LANGUAGE C;

CREATE TRIGGER segments_changed
AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON gp.segments
FOR EACH STATEMENT EXECUTE FUNCTION gp.segments_changed();

-- Segments of the cluster as the coordinator knows them, from any database
CREATE FUNCTION gp.cluster_segments(OUT segment_id int, OUT host text,
									OUT port int)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_cluster_segments'
LANGUAGE C STRICT;

-- Register a segment, or confirm the registration of a known one.  A
-- segment keeps its number and address: a number registered at another
-- address, or an address registered under another number, is an error.
CREATE FUNCTION gp.register_segment(segment_id int, host text, port int)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	known gp.segments;
BEGIN
	IF current_setting('gp.role', true) IS DISTINCT FROM 'coordinator' THEN
		RAISE EXCEPTION 'segments register on the coordinator only';
	END IF;
	IF current_database() <> current_setting('gp.database') THEN
		RAISE EXCEPTION 'segments register in database "%" only',
			current_setting('gp.database');
	END IF;

	-- Registrations come one at a time, so the checks below hold
	LOCK TABLE gp.segments IN SHARE ROW EXCLUSIVE MODE;

	SELECT * INTO known FROM gp.segments s
	WHERE s.segment_id = register_segment.segment_id;
	IF FOUND THEN
		IF known.host <> register_segment.host OR
			known.port <> register_segment.port THEN
			RAISE EXCEPTION 'segment % is already registered at %:%',
				known.segment_id, known.host, known.port;
		END IF;
		UPDATE gp.segments s SET registered = now()
		WHERE s.segment_id = register_segment.segment_id;
		RETURN;
	END IF;

	SELECT * INTO known FROM gp.segments s
	WHERE s.host = register_segment.host AND s.port = register_segment.port;
	IF FOUND THEN
		RAISE EXCEPTION '%:% is already registered as segment %',
			known.host, known.port, known.segment_id;
	END IF;

	IF (SELECT count(*) FROM gp.segments) >=
		current_setting('gp.max_segments')::int THEN
		RAISE EXCEPTION 'cannot register more than % segments',
			current_setting('gp.max_segments')
			USING HINT = 'Raise "gp.max_segments" on the coordinator.';
	END IF;

	INSERT INTO gp.segments (segment_id, host, port)
	VALUES (register_segment.segment_id, register_segment.host,
			register_segment.port);
END
$$;

REVOKE ALL ON FUNCTION gp.register_segment(int, text, int) FROM PUBLIC;

-- Run a command on all segments at once and return the command status of
-- each; rows of a query are discarded.  Each segment runs the command in a
-- transaction of its own: a rollback on the coordinator does not undo it.
-- An error on a segment is reported after all segments have answered.
CREATE FUNCTION gp.exec_on_all(sql text, OUT segment_id int, OUT status text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_exec_on_all'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION gp.exec_on_all(text) FROM PUBLIC;

-- OIDs of the new catalog rows of the current transaction on the
-- coordinator, as a segment takes them in gp.dispatch_oids
CREATE FUNCTION gp.recorded_oids()
RETURNS text
AS 'MODULE_PATHNAME', 'gp_recorded_oids'
LANGUAGE C STRICT;

-- Colocation groups: tables of a group have keys of the same types and the
-- same number of buckets, so rows with equal keys are on the same segment
-- and a join of the tables by the key is local.  map[b + 1] is the segment
-- of bucket b; moving a bucket to another segment changes one element.
CREATE TABLE gp.colocation_groups (
	colocation_id serial PRIMARY KEY,
	key_types regtype[] NOT NULL CHECK (cardinality(key_types) > 0),
	nbuckets int NOT NULL CHECK (nbuckets > 0),
	map int[] NOT NULL CHECK (cardinality(map) = nbuckets)
);

-- How the tables of this database are distributed: by hash of the key
-- columns, in a colocation group, or replicated to all segments
CREATE TABLE gp.distribution_policy (
	relid regclass PRIMARY KEY,
	kind text NOT NULL CHECK (kind IN ('hash', 'replicated')),
	key_columns int2[],
	colocation_id int REFERENCES gp.colocation_groups,
	CHECK ((kind = 'hash') =
		   (key_columns IS NOT NULL AND colocation_id IS NOT NULL)),
	CHECK (kind = 'hash' OR (key_columns IS NULL AND colocation_id IS NULL))
);

SELECT pg_catalog.pg_extension_config_dump('gp.colocation_groups', '');
SELECT pg_catalog.pg_extension_config_dump('gp.colocation_groups_colocation_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('gp.distribution_policy', '');

-- The bucket of a key among nbuckets
CREATE FUNCTION gp.bucket(nbuckets int, VARIADIC key "any")
RETURNS int
AS 'MODULE_PATHNAME', 'gp_bucket'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- The segment of a bucket of a colocation group
CREATE FUNCTION gp.bucket_segment(colocation_id int, bucket int)
RETURNS int
LANGUAGE sql STABLE PARALLEL SAFE
BEGIN ATOMIC
	SELECT g.map[bucket + 1] FROM gp.colocation_groups g
	WHERE g.colocation_id = bucket_segment.colocation_id;
END;

CREATE FUNCTION gp.check_distribution_key(relation regclass, attnums int2[])
RETURNS void
AS 'MODULE_PATHNAME', 'gp_check_distribution_key'
LANGUAGE C STRICT;

-- Make every session read the policy of a table again
CREATE FUNCTION gp.policy_changed(relation regclass)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_policy_changed'
LANGUAGE C STRICT;

-- Distribute an empty table by hash of the key columns, or replicate it to
-- all segments if key is NULL.  A table distributed by hash joins the
-- colocation group of colocate_with, or else the first group with its key
-- types and number of buckets; a new group spreads its buckets over the
-- segments in turn.
CREATE FUNCTION gp.distribute(relation regclass, key name[],
							  colocate_with regclass DEFAULT NULL)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	buckets constant int := 1024;
	attnums int2[];
	types regtype[];
	group_id int;
	group_types regtype[];
	segments int[];
	nonempty bool;
BEGIN
	IF current_setting('gp.role', true) IS DISTINCT FROM 'coordinator' THEN
		RAISE EXCEPTION 'tables are distributed on the coordinator only';
	END IF;
	IF (SELECT c.relkind NOT IN ('r', 'p') OR c.relpersistence = 't'
		FROM pg_class c WHERE c.oid = relation) THEN
		RAISE EXCEPTION '"%" is not a permanent table', relation;
	END IF;
	IF EXISTS (SELECT FROM gp.distribution_policy p WHERE p.relid = relation)
	THEN
		RAISE EXCEPTION 'table "%" is distributed already', relation;
	END IF;
	EXECUTE format('SELECT EXISTS (SELECT FROM %s)', relation) INTO nonempty;
	IF nonempty THEN
		RAISE EXCEPTION 'table "%" is not empty', relation
			USING HINT = 'Distribute a table before filling it.';
	END IF;

	IF key IS NULL THEN
		IF colocate_with IS NOT NULL THEN
			RAISE EXCEPTION 'a replicated table has no colocation group';
		END IF;
		INSERT INTO gp.distribution_policy (relid, kind)
		VALUES (relation, 'replicated');
		PERFORM gp.policy_changed(relation);
		RETURN;
	END IF;

	IF cardinality(key) = 0 THEN
		RAISE EXCEPTION 'a key has at least one column';
	END IF;
	SELECT array_agg(a.attnum ORDER BY k.ord),
		   array_agg(a.atttypid::regtype ORDER BY k.ord)
	INTO attnums, types
	FROM unnest(key) WITH ORDINALITY AS k(name, ord)
	LEFT JOIN pg_attribute a
	ON a.attrelid = relation AND a.attname = k.name AND a.attnum > 0 AND
	   NOT a.attisdropped;
	IF array_position(attnums, NULL) IS NOT NULL THEN
		RAISE EXCEPTION 'column "%" of table "%" does not exist',
			key[array_position(attnums, NULL)], relation;
	END IF;
	IF (SELECT count(DISTINCT n) FROM unnest(attnums) n) < cardinality(attnums)
	THEN
		RAISE EXCEPTION 'a key has a column more than once';
	END IF;
	PERFORM gp.check_distribution_key(relation, attnums);

	-- Groups are made one at a time, so the search below holds
	LOCK TABLE gp.colocation_groups IN SHARE ROW EXCLUSIVE MODE;

	IF colocate_with IS NOT NULL THEN
		SELECT p.colocation_id, g.key_types INTO group_id, group_types
		FROM gp.distribution_policy p
		JOIN gp.colocation_groups g USING (colocation_id)
		WHERE p.relid = colocate_with AND p.kind = 'hash';
		IF NOT FOUND THEN
			RAISE EXCEPTION 'table "%" is not distributed by hash',
				colocate_with;
		END IF;
		IF group_types <> types THEN
			RAISE EXCEPTION 'key of table "%" is %, but key of table "%" is %',
				relation, types, colocate_with, group_types;
		END IF;
	ELSE
		SELECT g.colocation_id INTO group_id FROM gp.colocation_groups g
		WHERE g.key_types = types AND g.nbuckets = buckets
		ORDER BY g.colocation_id LIMIT 1;
		IF NOT FOUND THEN
			SELECT array_agg(s.segment_id ORDER BY s.segment_id) INTO segments
			FROM gp.cluster_segments() s;
			IF segments IS NULL THEN
				RAISE EXCEPTION 'the cluster has no segments';
			END IF;
			INSERT INTO gp.colocation_groups (key_types, nbuckets, map)
			SELECT types, buckets,
				   array_agg(segments[b % cardinality(segments) + 1] ORDER BY b)
			FROM generate_series(0, buckets - 1) b
			RETURNING colocation_id INTO group_id;
		END IF;
	END IF;

	INSERT INTO gp.distribution_policy (relid, kind, key_columns, colocation_id)
	VALUES (relation, 'hash', attnums, group_id);
	PERFORM gp.policy_changed(relation);
END
$$;

REVOKE ALL ON FUNCTION gp.distribute(regclass, name[], regclass) FROM PUBLIC;

-- A dropped table loses its policy, lest a new table get its OID and the
-- policy with it
CREATE FUNCTION gp.drop_distribution_policy()
RETURNS event_trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
	DELETE FROM gp.distribution_policy p
	USING pg_event_trigger_dropped_objects() d
	WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND
		  p.relid = d.objid;
END
$$;

CREATE EVENT TRIGGER gp_drop_distribution_policy ON sql_drop
EXECUTE FUNCTION gp.drop_distribution_policy();
