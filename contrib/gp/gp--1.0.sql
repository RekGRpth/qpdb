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
