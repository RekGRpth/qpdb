/* -------------------------------------------------------------------------
 *
 * dispatch.c
 *		Connections of a session of the coordinator to the segments
 *
 * A session opens a connection to a segment when it needs it first, and
 * keeps it until the session ends.  It connects to the database of the
 * session as its session user, without a password: the segments trust the
 * coordinator by pg_hba.conf, certificates or a password file.  A
 * connection to a segment that has left gp.segments or moved is closed,
 * and a broken one is dropped, to be opened again on the next use.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/dispatch.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "gp.h"

/* Seconds to wait for a segment to confirm a cancel */
#define GP_CANCEL_TIMEOUT 5

typedef struct GpConnection
{
	int			segment_id;		/* hash key */
	char		host[GP_HOST_LEN];	/* address the connection is open to */
	int			port;
	PGconn	   *conn;
	bool		busy;			/* a command runs, its result is not read */
} GpConnection;

/* Connections of the session by segment_id; NULL until the first one */
static HTAB *connections = NULL;

static uint32 wait_event_connect = 0;
static uint32 wait_event_result = 0;

PG_FUNCTION_INFO_V1(gp_exec_on_all);

static void
gp_connection_close(GpConnection *entry)
{
	libpqsrv_disconnect(entry->conn);
	hash_search(connections, &entry->segment_id, HASH_REMOVE, NULL);
}

/*
 * On an abort, cancel the commands still running on the segments, which
 * would go on with nobody waiting for them, and drop their connections:
 * their results are unknown.
 */
static void
gp_connections_abort(void)
{
	HASH_SEQ_STATUS status;
	GpConnection *entry;

	hash_seq_init(&status, connections);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		if (!entry->busy)
			continue;
		(void) libpqsrv_cancel(entry->conn,
							   TimestampTzPlusSeconds(GetCurrentTimestamp(),
													  GP_CANCEL_TIMEOUT));
		gp_connection_close(entry);
	}
}

static void
gp_connections_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT)
		gp_connections_abort();
}

static void
gp_connections_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
								SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		gp_connections_abort();
}

/*
 * Close the connections to segments that are not in segments any more
 */
static void
gp_connections_forget(const GpSegment *segments, int nsegments)
{
	HASH_SEQ_STATUS status;
	GpConnection *entry;

	if (connections == NULL)
		return;

	hash_seq_init(&status, connections);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		bool		known = false;

		for (int i = 0; i < nsegments && !known; i++)
			known = segments[i].segment_id == entry->segment_id;
		if (!known)
			gp_connection_close(entry);
	}
}

/*
 * The connection of the session to a segment, opened if there is none or
 * the one there is cannot be used
 */
static GpConnection *
gp_connection_open(const GpSegment *segment)
{
	const char *keywords[] = {"host", "port", "dbname", "user",
	"application_name", "client_encoding", NULL};
	const char *values[7];
	char		port[12];
	GpConnection *entry;
	PGconn	   *conn;

	if (connections == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(int);
		ctl.entrysize = sizeof(GpConnection);
		connections = hash_create("gp connections", 16, &ctl,
								  HASH_ELEM | HASH_BLOBS);
		RegisterXactCallback(gp_connections_xact_callback, NULL);
		RegisterSubXactCallback(gp_connections_subxact_callback, NULL);
		wait_event_connect = WaitEventExtensionNew("GpConnect");
		wait_event_result = WaitEventExtensionNew("GpResult");
	}

	entry = hash_search(connections, &segment->segment_id, HASH_FIND, NULL);
	if (entry != NULL)
	{
		if (strcmp(entry->host, segment->host) == 0 &&
			entry->port == segment->port &&
			PQstatus(entry->conn) == CONNECTION_OK)
			return entry;
		gp_connection_close(entry);
	}

	snprintf(port, sizeof(port), "%d", segment->port);
	values[0] = segment->host;
	values[1] = port;
	values[2] = get_database_name(MyDatabaseId);
	values[3] = GetUserNameFromId(GetSessionUserId(), false);
	values[4] = "gp coordinator";
	values[5] = GetDatabaseEncodingName();
	values[6] = NULL;

	conn = libpqsrv_connect_params_start(keywords, values, false);
	if (conn != NULL)
		PQsetNoticeReceiver(conn, libpqsrv_notice_receiver,
							"received message from a segment");
	libpqsrv_connect_complete(conn, wait_event_connect);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		char	   *message = pchomp(PQerrorMessage(conn));

		libpqsrv_disconnect(conn);
		ereport(ERROR,
				errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				errmsg("could not connect to segment %d at %s:%d",
					   segment->segment_id, segment->host, segment->port),
				errdetail_internal("%s", message));
	}

	entry = hash_search(connections, &segment->segment_id, HASH_ENTER, NULL);
	strlcpy(entry->host, segment->host, sizeof(entry->host));
	entry->port = segment->port;
	entry->conn = conn;
	entry->busy = false;
	return entry;
}

/*
 * The connection of the session to a segment
 */
PGconn *
gp_connection_get(const GpSegment *segment)
{
	return gp_connection_open(segment)->conn;
}

/*
 * gp.exec_on_all(sql): run a command on all segments at once, and return
 * the command status of each.  Each segment runs it in a transaction of
 * its own, so a rollback on the coordinator does not undo it.  If a
 * segment fails, the error of the first one by number is reported after
 * all have answered.
 */
Datum
gp_exec_on_all(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	GpSegment  *segments;
	int			nsegments;
	GpConnection **entries;
	PGresult  **results;
	int			failed = -1;

	nsegments = gp_segments_get(&segments);
	InitMaterializedSRF(fcinfo, 0);

	gp_connections_forget(segments, nsegments);
	entries = palloc_array(GpConnection *, nsegments);
	results = palloc0_array(PGresult *, nsegments);
	for (int i = 0; i < nsegments; i++)
		entries[i] = gp_connection_open(&segments[i]);

	/* Send to all, then wait for all, so the segments work in parallel */
	for (int i = 0; i < nsegments; i++)
		entries[i]->busy = PQsendQuery(entries[i]->conn, sql);
	for (int i = 0; i < nsegments; i++)
	{
		if (!entries[i]->busy)
			continue;
		results[i] = libpqsrv_get_result_last(entries[i]->conn,
											  wait_event_result);
		entries[i]->busy = false;
	}

	for (int i = 0; i < nsegments; i++)
	{
		ExecStatusType status = results[i] ? PQresultStatus(results[i]) :
			PGRES_FATAL_ERROR;

		if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK)
		{
			Datum		values[2];
			bool		nulls[2] = {0};

			values[0] = Int32GetDatum(segments[i].segment_id);
			values[1] = CStringGetTextDatum(PQcmdStatus(results[i]));
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
		else if (failed < 0)
			failed = i;
	}

	if (failed >= 0)
	{
		const GpSegment *segment = &segments[failed];
		PGresult   *res = results[failed];
		int			code = ERRCODE_CONNECTION_FAILURE;
		char	   *primary = NULL;
		char	   *detail = NULL;
		char	   *hint = NULL;
		bool		broken;

		if (res != NULL)
		{
			char	   *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
			char	   *field;

			if (sqlstate != NULL && strlen(sqlstate) == 5)
				code = MAKE_SQLSTATE(sqlstate[0], sqlstate[1], sqlstate[2],
									 sqlstate[3], sqlstate[4]);
			if ((field = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY)))
				primary = pstrdup(field);
			if ((field = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL)))
				detail = pstrdup(field);
			if ((field = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT)))
				hint = pstrdup(field);
		}
		broken = res == NULL || PQstatus(entries[failed]->conn) != CONNECTION_OK;
		if (broken)
			detail = primary ? primary :
				pchomp(PQerrorMessage(entries[failed]->conn));

		/* A broken connection is no use any more */
		for (int i = 0; i < nsegments; i++)
			if (PQstatus(entries[i]->conn) != CONNECTION_OK)
				gp_connection_close(entries[i]);

		if (broken)
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("lost the connection to segment %d at %s:%d",
						   segment->segment_id, segment->host, segment->port),
					errdetail_internal("%s", detail));
		ereport(ERROR,
				errcode(code),
				errmsg_internal("%s", primary),
				detail ? errdetail_internal("%s", detail) : 0,
				hint ? errhint("%s", hint) : 0,
				errcontext("segment %d at %s:%d",
						   segment->segment_id, segment->host, segment->port));
	}

	for (int i = 0; i < nsegments; i++)
		PQclear(results[i]);
	return (Datum) 0;
}
