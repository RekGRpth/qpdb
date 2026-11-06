/* -------------------------------------------------------------------------
 *
 * motion.c
 *		Rows of a query from every segment of the cluster
 *
 * A query that joins tables of different colocation groups needs the rows
 * of some of them to move, so that every row meets its partners on one
 * segment.  The movement goes the pull way: every segment, as it runs the
 * query, asks every segment — itself among them — for rows.  An inner join
 * asks for the rows that the hash of the join key places on it, however many
 * tables move; an outer join of two tables broadcasts a whole side instead,
 * keeping rows with null keys and rows without a partner.  The asking is this
 * function: it takes the query of the rows and answers with the rows of every
 * segment together.
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
 * Read the rows of one answer into the tuplestore.  The columns of the
 * answer are those the caller says the query returns, in the binary
 * format, which takes every type as it is
 */
static void
gp_motion_read(Tuplestorestate *store, TupleDesc tupdesc, PGresult *res,
			   int segment_id)
{
	TupleTableSlot *slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	FmgrInfo   *recv = palloc0_array(FmgrInfo, tupdesc->natts);
	Oid		   *ioparam = palloc_array(Oid, tupdesc->natts);
	MemoryContext rowcxt;
	MemoryContext oldcontext;

	if (PQnfields(res) != tupdesc->natts)
		ereport(ERROR,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("segment %d returned %d columns, but the query takes %d",
					   segment_id, PQnfields(res), tupdesc->natts));

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Oid			recvfn;

		getTypeBinaryInputInfo(TupleDescAttr(tupdesc, i)->atttypid,
							   &recvfn, &ioparam[i]);
		fmgr_info(recvfn, &recv[i]);
	}

	rowcxt = AllocSetContextCreate(CurrentMemoryContext, "motion row",
								   ALLOCSET_DEFAULT_SIZES);
	for (int row = 0; row < PQntuples(res); row++)
	{
		ExecClearTuple(slot);
		oldcontext = MemoryContextSwitchTo(rowcxt);
		for (int i = 0; i < tupdesc->natts; i++)
		{
			StringInfoData buf;

			slot->tts_isnull[i] = true;
			slot->tts_values[i] = (Datum) 0;
			if (PQgetisnull(res, row, i))
				continue;

			/* libpq ends every value with a zero byte, as the receive functions expect */
			buf.data = PQgetvalue(res, row, i);
			buf.len = PQgetlength(res, row, i);
			buf.maxlen = buf.len + 1;
			buf.cursor = 0;
			slot->tts_values[i] = ReceiveFunctionCall(&recv[i], &buf,
													  ioparam[i],
													  TupleDescAttr(tupdesc, i)->atttypmod);
			slot->tts_isnull[i] = false;
		}
		MemoryContextSwitchTo(oldcontext);
		ExecStoreVirtualTuple(slot);
		tuplestore_puttupleslot(store, slot);
		MemoryContextReset(rowcxt);
		CHECK_FOR_INTERRUPTS();
	}

	MemoryContextDelete(rowcxt);
	ExecDropSingleTupleTableSlot(slot);
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
		const char *keywords[] = {"host", "port", "dbname", "user",
			"application_name", NULL};
		const char *values[5];
		char		set[64];
		char	   *host = pstrdup(PQgetvalue(segments, i, 1));
		char	   *port = pstrdup(PQgetvalue(segments, i, 2));
		int			segment_id = atoi(PQgetvalue(segments, i, 0));
		PGconn	   *peer;
		PGresult   *rows;

		values[0] = host;
		values[1] = port;
		values[2] = db;
		values[3] = GetUserNameFromId(GetUserId(), false);
		values[4] = "gp motion";

		peer = libpqsrv_connect_params(keywords, values, false,
									   gp_motion_event());
		if (PQstatus(peer) != CONNECTION_OK)
		{
			char	   *err = pchomp(PQerrorMessage(peer));

			libpqsrv_disconnect(peer);
			PQclear(segments);
			libpqsrv_disconnect(conn);
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not connect to segment %d at %s:%s",
						   segment_id, host, port),
					errdetail("%s", err));
		}

		/* Tell the segment which segment its rows are for */
		snprintf(set, sizeof(set), "SET gp.requester_segment = %d",
				 gp_segment_id);
		rows = libpqsrv_exec_params(peer, set, 0, NULL, NULL, NULL, NULL, 0,
									gp_motion_event());
		if (PQresultStatus(rows) != PGRES_COMMAND_OK)
		{
			char	   *err = pchomp(PQresultErrorMessage(rows));

			PQclear(rows);
			libpqsrv_disconnect(peer);
			PQclear(segments);
			libpqsrv_disconnect(conn);
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not set the receiving segment on segment %d",
						   segment_id),
					errdetail("%s", err));
		}
		PQclear(rows);

		rows = libpqsrv_exec_params(peer, sql, 0, NULL, NULL, NULL, NULL, 1,
									gp_motion_event());
		if (PQresultStatus(rows) != PGRES_TUPLES_OK)
		{
			char	   *err = pchomp(PQresultErrorMessage(rows));

			PQclear(rows);
			libpqsrv_disconnect(peer);
			PQclear(segments);
			libpqsrv_disconnect(conn);
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not read the rows of segment %d", segment_id),
					errdetail("%s", err));
		}
		gp_motion_read(store, tupdesc, rows, segment_id);
		PQclear(rows);
		libpqsrv_disconnect(peer);
	}

	PQclear(segments);
	libpqsrv_disconnect(conn);

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = store;
	rsinfo->setDesc = tupdesc;

	return (Datum) 0;
}
