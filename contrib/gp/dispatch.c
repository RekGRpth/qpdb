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
 * Before a command, the segments get the settings of the session: those a
 * user may change, set for the database, the role, the client or the
 * session rather than in the configuration of the server.  A connection
 * remembers what it has got, and gets the difference only when the
 * settings change.
 *
 * DDL runs on the segments in transactions that follow the transaction of
 * the coordinator: the first command opens one, and the end of the
 * transaction of the coordinator commits or rolls them back.  Without
 * two-phase commit, a subtransaction cannot be rolled back on the segments,
 * so a transaction that rolled back one after sending DDL cannot commit.
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
#include "lib/stringinfo.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc_tables.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
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
	bool		in_xact;		/* in a transaction of the coordinator */
	char	   *settings;		/* settings sent, "name\0value\0" pairs */
	int			settings_len;	/* -1 if unknown after a rollback */
} GpConnection;

/* Connections of the session by segment_id; NULL until the first one */
static HTAB *connections = NULL;

static uint32 wait_event_connect = 0;
static uint32 wait_event_result = 0;

/* Commands sent in transactions of segments in this transaction */
static int	ndispatched = 0;

/* ndispatched at the start of each open subtransaction, innermost first */
static List *subxact_marks = NIL;

/* The segments do not have what this transaction has sent them */
static bool diverged = false;

PG_FUNCTION_INFO_V1(gp_exec_on_all);

static void
gp_connection_close(GpConnection *entry)
{
	/* The segment rolls back what it has got in the transaction */
	if (entry->in_xact)
		diverged = true;
	libpqsrv_disconnect(entry->conn);
	if (entry->settings != NULL)
		pfree(entry->settings);
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

	if (connections == NULL)
		return;

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

/*
 * End the transactions of the segments with COMMIT or ROLLBACK.  The
 * transaction of the coordinator has ended already, so a failure is only a
 * warning.
 */
static void
gp_connections_finish(const char *command)
{
	HASH_SEQ_STATUS status;
	GpConnection *entry;

	if (connections == NULL)
		return;

	hash_seq_init(&status, connections);
	while ((entry = hash_seq_search(&status)) != NULL)
		if (entry->in_xact)
			entry->busy = PQsendQuery(entry->conn, command);

	hash_seq_init(&status, connections);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		PGresult   *res = NULL;

		if (!entry->in_xact)
			continue;
		if (entry->busy)
			res = libpqsrv_get_result_last(entry->conn, wait_event_result);
		entry->busy = false;
		entry->in_xact = false;

		/* Settings sent in a rolled back transaction are gone */
		if (strcmp(command, "ROLLBACK") == 0)
			entry->settings_len = -1;

		if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			ereport(WARNING,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not run %s on segment %d at %s:%d",
						   command, entry->segment_id, entry->host,
						   entry->port),
					errdetail_internal("%s", pchomp(PQerrorMessage(entry->conn))));
			if (PQstatus(entry->conn) != CONNECTION_OK)
				gp_connection_close(entry);
		}
		PQclear(res);
	}
}

static void
gp_connections_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
			if (diverged)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot commit a transaction whose DDL the segments have lost"),
						errdetail("A subtransaction that sent DDL to the segments was rolled back, or a connection to a segment was closed."),
						errhint("Roll back the whole transaction."));
			break;
		case XACT_EVENT_PRE_PREPARE:
			if (diverged || ndispatched > 0)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot PREPARE a transaction that has sent DDL to the segments"));
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			gp_connections_finish("COMMIT");
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			gp_connections_abort();
			gp_connections_finish("ROLLBACK");
			break;
		default:
			break;
	}

	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT ||
		event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT ||
		event == XACT_EVENT_PREPARE)
	{
		/* TopTransactionContext is gone with the list */
		ndispatched = 0;
		subxact_marks = NIL;
		diverged = false;
	}
}

static void
gp_connections_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
								SubTransactionId parentSubid, void *arg)
{
	MemoryContext oldcontext;

	switch (event)
	{
		case SUBXACT_EVENT_START_SUB:
			oldcontext = MemoryContextSwitchTo(TopTransactionContext);
			subxact_marks = lcons_int(ndispatched, subxact_marks);
			MemoryContextSwitchTo(oldcontext);
			break;
		case SUBXACT_EVENT_COMMIT_SUB:
			if (subxact_marks != NIL)
				subxact_marks = list_delete_first(subxact_marks);
			break;
		case SUBXACT_EVENT_ABORT_SUB:
			gp_connections_abort();
			if (subxact_marks != NIL)
			{
				if (ndispatched > linitial_int(subxact_marks))
					diverged = true;
				subxact_marks = list_delete_first(subxact_marks);
			}
			break;
		default:
			break;
	}
}

/*
 * Follow the transactions of the coordinator; called from _PG_init()
 */
void
gp_dispatch_init(void)
{
	RegisterXactCallback(gp_connections_xact_callback, NULL);
	RegisterSubXactCallback(gp_connections_subxact_callback, NULL);
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
	entry->in_xact = false;
	entry->settings = NULL;
	entry->settings_len = 0;
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
 * Whether the segments get a setting of the session
 */
static bool
gp_setting_synced(const struct config_generic *var, bool superuser)
{
	if (var->context != PGC_USERSET &&
		!(var->context == PGC_SUSET && superuser))
		return false;

	/* Not from the configuration of the server: segments have their own */
	switch (var->source)
	{
		case PGC_S_GLOBAL:
		case PGC_S_DATABASE:
		case PGC_S_USER:
		case PGC_S_DATABASE_USER:
		case PGC_S_CLIENT:
		case PGC_S_SESSION:
			break;
		default:
			return false;
	}

	/* Settings of a transaction or of the session user, but SET ROLE */
	if ((var->flags & GUC_NO_RESET_ALL) && strcmp(var->name, "role") != 0)
		return false;

	/* The connection sets these itself */
	if (strcmp(var->name, "application_name") == 0 ||
		strcmp(var->name, "client_encoding") == 0 ||
		strncmp(var->name, "gp.", 3) == 0)
		return false;

	return true;
}

static int
gp_setting_cmp(const void *a, const void *b)
{
	return strcmp((*(struct config_generic *const *) a)->name,
				  (*(struct config_generic *const *) b)->name);
}

/*
 * The settings of the session that the segments get, as "name\0value\0"
 * pairs sorted by name
 */
static void
gp_settings_collect(StringInfo settings)
{
	int			nvars;
	struct config_generic **vars = get_guc_variables(&nvars);
	struct config_generic **synced = palloc_array(struct config_generic *, nvars);
	int			nsynced = 0;
	bool		superuser = superuser_arg(GetSessionUserId());

	for (int i = 0; i < nvars; i++)
		if (gp_setting_synced(vars[i], superuser))
			synced[nsynced++] = vars[i];
	qsort(synced, nsynced, sizeof(*synced), gp_setting_cmp);

	for (int i = 0; i < nsynced; i++)
	{
		char	   *value = ShowGUCOption(synced[i], true);

		appendBinaryStringInfo(settings, synced[i]->name,
							   strlen(synced[i]->name) + 1);
		appendBinaryStringInfo(settings, value, strlen(value) + 1);
	}
	pfree(synced);
}

static bool
gp_settings_has(const char *settings, int len, const char *name)
{
	for (const char *p = settings; p < settings + len;)
	{
		if (strcmp(p, name) == 0)
			return true;
		p += strlen(p) + 1;		/* name */
		p += strlen(p) + 1;		/* value */
	}
	return false;
}

/* Append a setting name, quoting each part of a name like "myapp.x" */
static void
gp_append_setting_name(StringInfo buf, const char *name)
{
	char	   *copy = pstrdup(name);
	char	   *part = copy;

	for (char *dot; (dot = strchr(part, '.')) != NULL; part = dot + 1)
	{
		*dot = '\0';
		appendStringInfo(buf, "%s.", quote_identifier(part));
	}
	appendStringInfoString(buf, quote_identifier(part));
	pfree(copy);
}

/*
 * The command that gives a connection the settings wanted: RESET of those
 * it has got but are not wanted any more, set_config() of the others, which
 * parses a list like search_path as SET does.  NULL if it has them already.
 */
static char *
gp_settings_command(const GpConnection *entry, const StringInfo wanted)
{
	StringInfoData cmd;
	const char *sent = entry->settings ? entry->settings : "";
	bool		first = true;

	if (entry->settings_len == wanted->len &&
		memcmp(sent, wanted->data, wanted->len) == 0)
		return NULL;

	initStringInfo(&cmd);

	/* After a rollback, start from the settings of the server */
	if (entry->settings_len < 0)
		appendStringInfoString(&cmd, "RESET ALL; RESET role; ");

	for (const char *p = sent; p < sent + Max(entry->settings_len, 0);)
	{
		const char *name = p;

		p += strlen(p) + 1;
		p += strlen(p) + 1;
		if (gp_settings_has(wanted->data, wanted->len, name))
			continue;
		appendStringInfoString(&cmd, "RESET ");
		gp_append_setting_name(&cmd, name);
		appendStringInfoString(&cmd, "; ");
	}
	for (const char *p = wanted->data; p < wanted->data + wanted->len;)
	{
		const char *name = p;
		const char *value = (p += strlen(p) + 1);

		p += strlen(p) + 1;
		appendStringInfo(&cmd, "%s pg_catalog.set_config(%s, %s, false)",
						 first ? "SELECT" : ",",
						 quote_literal_cstr(name), quote_literal_cstr(value));
		first = false;
	}
	return cmd.data;
}

static void
gp_settings_remember(GpConnection *entry, const StringInfo settings)
{
	if (entry->settings != NULL)
		pfree(entry->settings);
	entry->settings = MemoryContextAlloc(TopMemoryContext,
										 Max(settings->len, 1));
	memcpy(entry->settings, settings->data, settings->len);
	entry->settings_len = settings->len;
}

/*
 * Send each connection its command, none for NULL, then wait for all, so
 * the segments work in parallel.  If a segment fails, report the error of
 * the first one by number once all have answered; what says what the
 * commands do, NULL for a command of the user.
 */
static PGresult **
gp_dispatch(const GpSegment *segments, GpConnection **entries, int nsegments,
			char **commands, const char *what)
{
	PGresult  **results = palloc0_array(PGresult *, nsegments);
	int			failed = -1;

	for (int i = 0; i < nsegments; i++)
		if (commands[i] != NULL)
			entries[i]->busy = PQsendQuery(entries[i]->conn, commands[i]);
	for (int i = 0; i < nsegments; i++)
	{
		if (!entries[i]->busy)
			continue;
		results[i] = libpqsrv_get_result_last(entries[i]->conn,
											  wait_event_result);
		entries[i]->busy = false;
	}

	for (int i = 0; i < nsegments && failed < 0; i++)
	{
		ExecStatusType status;

		if (commands[i] == NULL)
			continue;
		status = results[i] ? PQresultStatus(results[i]) : PGRES_FATAL_ERROR;
		if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
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
				what ?
				errcontext("%s on segment %d at %s:%d", what,
						   segment->segment_id, segment->host, segment->port) :
				errcontext("segment %d at %s:%d",
						   segment->segment_id, segment->host, segment->port));
	}

	return results;
}

/*
 * Connections to all segments, with the settings of the session
 */
static int
gp_connections_ready(GpSegment **segments, GpConnection ***entries)
{
	int			nsegments = gp_segments_get(segments);
	char	  **commands;
	PGresult  **results;
	StringInfoData settings;

	gp_connections_forget(*segments, nsegments);
	*entries = palloc_array(GpConnection *, nsegments);
	for (int i = 0; i < nsegments; i++)
		(*entries)[i] = gp_connection_open(&(*segments)[i]);

	/*
	 * The settings go in a command of their own: in one with the command of
	 * the user, they would make it a transaction block, where CREATE DATABASE
	 * and the like cannot run.
	 */
	initStringInfo(&settings);
	gp_settings_collect(&settings);
	commands = palloc_array(char *, nsegments);
	for (int i = 0; i < nsegments; i++)
		commands[i] = gp_settings_command((*entries)[i], &settings);
	results = gp_dispatch(*segments, *entries, nsegments, commands,
						  "setting the parameters of the session");
	for (int i = 0; i < nsegments; i++)
	{
		if (commands[i] != NULL)
			gp_settings_remember((*entries)[i], &settings);
		PQclear(results[i]);
	}

	return nsegments;
}

/*
 * Run a command on all segments in the transactions that follow the
 * transaction of the coordinator, opening them if needed
 */
void
gp_dispatch_xact(const char *sql)
{
	GpSegment  *segments;
	GpConnection **entries;
	int			nsegments = gp_connections_ready(&segments, &entries);
	char	  **commands = palloc_array(char *, nsegments);
	PGresult  **results;

	for (int i = 0; i < nsegments; i++)
	{
		commands[i] = entries[i]->in_xact ? (char *) sql :
			psprintf("BEGIN; %s", sql);
		entries[i]->in_xact = true;
	}
	ndispatched++;

	results = gp_dispatch(segments, entries, nsegments, commands, NULL);
	for (int i = 0; i < nsegments; i++)
		PQclear(results[i]);
}

/*
 * gp.exec_on_all(sql): run a command on all segments at once, and return
 * the command status of each.  Each segment runs it in a transaction of
 * its own, so a rollback on the coordinator does not undo it, unless the
 * transaction of the coordinator has sent DDL to the segment already: then
 * the command runs in that transaction.  If a segment fails, the error of
 * the first one by number is reported after all have answered.
 */
Datum
gp_exec_on_all(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	GpSegment  *segments;
	int			nsegments;
	GpConnection **entries;
	char	  **commands;
	PGresult  **results;

	InitMaterializedSRF(fcinfo, 0);
	nsegments = gp_connections_ready(&segments, &entries);

	commands = palloc_array(char *, nsegments);
	for (int i = 0; i < nsegments; i++)
		commands[i] = sql;
	results = gp_dispatch(segments, entries, nsegments, commands, NULL);

	for (int i = 0; i < nsegments; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {0};

		values[0] = Int32GetDatum(segments[i].segment_id);
		values[1] = CStringGetTextDatum(PQcmdStatus(results[i]));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
		PQclear(results[i]);
	}
	return (Datum) 0;
}
