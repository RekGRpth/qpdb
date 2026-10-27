/* src/test/modules/test_new_oid_hook/test_new_oid_hook--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_new_oid_hook" to load this file. \quit

CREATE FUNCTION test_new_oid_hook_propose(catalog regclass, proposed oid)
  RETURNS void STRICT
  AS 'MODULE_PATHNAME' LANGUAGE C;
