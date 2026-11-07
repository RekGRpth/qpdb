/* -------------------------------------------------------------------------
 *
 * motion.c
 *		Rows of a query from every segment of the cluster
 *
 * A query that joins tables of different colocation groups needs the rows
 * of some of them to move, so that every row meets its partners on one
 * segment.  The movement goes the pull way: every segment, as it runs the
 * query, asks every segment — itself among them — for rows.  A fragment is
 * sent as a serialized PlannedStmt, never as SQL text, and the answer is a
 * stream of binary tuples.  An inner join
 * either asks for the rows that the hash of the join key places on it, however
 * many tables move, or broadcasts a whole table when there is no such key.  An
 * outer join of two tables broadcasts a whole side instead, keeping rows with
 * null keys and rows without a partner.  The asking is this function: it takes
 * the query of the rows and answers with the rows of every segment together.
 *
 * An aggregate whose grouping columns do not cover the distribution key moves
 * its rows the same way, but hashes those grouping columns: every group lands
 * on one segment, which computes the whole aggregate.  An INSERT ... SELECT
 * hashes its source rows by the distribution key of the target table, so
 * every segment inserts only the rows that belong to it.
 *
 * The addresses of the segments are not kept here: they are asked of the
 * coordinator at every call, so a cluster that changed stays correct.  The
 * segment the rows are for is told to the others by a setting of the
 * connection, for the query itself is one for all of them.  Every segment
 * answers as the user that runs the query, in the transaction of its
 * connection: the lock of the distributed snapshot, held by the
 * coordinator from before its question to after the last row, keeps the
 * answers of all the segments on one state of the cluster.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/motion.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "gp.h"

/* The wait event of the connections of the motion */
static uint32 gp_motion_wait_event = 0;

static uint32
gp_motion_event(void)
{
	if (gp_motion_wait_event == 0)
		gp_motion_wait_event = WaitEventExtensionNew("GpMotion");
	return gp_motion_wait_event;
}

/*
 * The rows of the query from every segment of the cluster, among them
 * this one.  The query returns the columns the caller says it does
 */
PG_FUNCTION_INFO_V1(gp_peer_query);

Datum
gp_peer_query(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *store;
	TupleDesc	tupdesc;
	char	   *sql;
	const char *coord_keywords[] = {"dbname", "dbname", "application_name",
	NULL};
	const char *coord_values[3];
	char	   *db;
	PGconn	   *conn;
	PGresult   *segments;
	MemoryContext oldcontext;

	/* The planner of the coordinator writes the calls; none runs by hand */
	if (gp_role != GP_ROLE_SEGMENT)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("gp.peer_query runs on a segment only"));

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("set-valued function called in context that cannot accept a set"));
	if (rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("gp.peer_query needs the columns of its answer"));
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("the query of the rows is required"));

	/* The result must survive the per-tuple context of the function call */
	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	store = tuplestore_begin_heap(true, false, work_mem);
	MemoryContextSwitchTo(oldcontext);
	sql = text_to_cstring(PG_GETARG_TEXT_PP(0));

	/* The segments of the cluster, asked of the coordinator */
	db = get_database_name(MyDatabaseId);
	coord_values[0] = gp_coordinator_conninfo();
	coord_values[1] = gp_database;
	coord_values[2] = "gp motion";
	conn = libpqsrv_connect_params(coord_keywords, coord_values, true,
								   gp_motion_event());
	if (PQstatus(conn) != CONNECTION_OK)
	{
		char	   *err = pchomp(PQerrorMessage(conn));

		libpqsrv_disconnect(conn);
		ereport(ERROR,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("could not ask the coordinator for the segments"),
				errdetail("%s", err));
	}
	segments = libpqsrv_exec_params(conn,
									"SELECT segment_id, host, port FROM gp.cluster_segments()",
									0, NULL, NULL, NULL, NULL, 0,
									gp_motion_event());
	if (PQresultStatus(segments) != PGRES_TUPLES_OK)
	{
		char	   *err = pchomp(PQresultErrorMessage(segments));

		PQclear(segments);
		libpqsrv_disconnect(conn);
		ereport(ERROR,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("could not read the segments of the cluster"),
				errdetail("%s", err));
	}

	for (int i = 0; i < PQntuples(segments); i++)
	{
		const char *host = PQgetvalue(segments, i, 1);
		int			port = atoi(PQgetvalue(segments, i, 2));
		int			requester = gp_segment_id;

		gp_interconnect_query(store, tupdesc, db, GetUserNameFromId(GetUserId(), false),
							  sql, host, port, requester);
	}

	PQclear(segments);
	libpqsrv_disconnect(conn);

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = store;
	rsinfo->setDesc = tupdesc;

	return (Datum) 0;
}
