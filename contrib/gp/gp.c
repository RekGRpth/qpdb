/* -------------------------------------------------------------------------
 *
 * gp.c
 *		Cluster of PostgreSQL servers: a coordinator and segments
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/gp.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "catalog/catalog.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/wait_event.h"

#if !defined(NEW_OID_HOOK_VERSION) || NEW_OID_HOOK_VERSION < 1
#error "gp requires a server with new_oid_hook"
#endif

PG_MODULE_MAGIC_EXT(
					.name = "gp",
					.version = PG_VERSION
);

/* Role of the server in the cluster */
enum
{
	GP_ROLE_NONE,				/* not in a cluster */
	GP_ROLE_COORDINATOR,
	GP_ROLE_SEGMENT,
};

static const struct config_enum_entry gp_role_options[] = {
	{"none", GP_ROLE_NONE, false},
	{"coordinator", GP_ROLE_COORDINATOR, false},
	{"segment", GP_ROLE_SEGMENT, false},
	{NULL, 0, false}
};

/* GUC Variables */
static int	gp_role = GP_ROLE_NONE;
static int	gp_segment_id = 0;
static char *gp_database = NULL;
static char *gp_coordinator = NULL;
static char *gp_host = NULL;
static int	gp_register_retry_interval = 5;

pg_noreturn PGDLLEXPORT void gp_register_main(Datum main_arg);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* The role is set at server start, so the hooks come only from there */
	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomEnumVariable("gp.role",
							 "Role of the server in the cluster.",
							 "With none, the server works on its own.",
							 &gp_role,
							 GP_ROLE_NONE,
							 gp_role_options,
							 PGC_POSTMASTER,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("gp.segment_id",
							"Number of the server in the cluster.",
							"0 on the coordinator, from 1 on segments.",
							&gp_segment_id,
							0,
							0,
							INT_MAX,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("gp.database",
							   "Database that holds the configuration of the cluster.",
							   "Segments register in this database of the coordinator.",
							   &gp_database,
							   "postgres",
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("gp.coordinator",
							   "Connection string of the coordinator.",
							   "Required on a segment, which registers there.",
							   &gp_coordinator,
							   "",
							   PGC_POSTMASTER,
							   GUC_SUPERUSER_ONLY,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("gp.host",
							   "Host name or address of the server for the other servers of the cluster.",
							   "A segment registers with this host and its port.",
							   &gp_host,
							   "localhost",
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomIntVariable("gp.register_retry_interval",
							"Time between attempts of a segment to register on the coordinator.",
							NULL,
							&gp_register_retry_interval,
							5,
							1,
							3600,
							PGC_POSTMASTER,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);

	MarkGUCPrefixReserved("gp");

	/* A misconfigured node does not start */
	if (gp_role == GP_ROLE_SEGMENT && gp_segment_id == 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("\"gp.segment_id\" must be set on a segment"));
	if (gp_role != GP_ROLE_SEGMENT && gp_segment_id != 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("\"gp.segment_id\" is set, but \"gp.role\" is not \"segment\""));
	if (gp_role == GP_ROLE_SEGMENT && gp_coordinator[0] == '\0')
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("\"gp.coordinator\" must be set on a segment"));
	if (gp_role != GP_ROLE_SEGMENT && gp_coordinator[0] != '\0')
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("\"gp.coordinator\" is set, but \"gp.role\" is not \"segment\""));

	if (gp_role == GP_ROLE_SEGMENT)
	{
		BackgroundWorker worker = {0};

		worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = gp_register_retry_interval;
		strlcpy(worker.bgw_library_name, "gp", BGW_MAXLEN);
		strlcpy(worker.bgw_function_name, "gp_register_main", BGW_MAXLEN);
		strlcpy(worker.bgw_name, "gp register segment", BGW_MAXLEN);
		strlcpy(worker.bgw_type, "gp register segment", BGW_MAXLEN);
		RegisterBackgroundWorker(&worker);
	}
}

/*
 * Background worker of a segment: register the segment on the coordinator,
 * then exit.  Exit code 0 tells the postmaster not to start the worker
 * again; on a failure, exit code 1 makes it retry after
 * gp.register_retry_interval, so a segment may start before the
 * coordinator.
 */
void
gp_register_main(Datum main_arg)
{
	/* A second dbname overrides one in the connection string */
	const char *keywords[] = {"dbname", "dbname", "application_name", NULL};
	const char *values[] = {gp_coordinator, gp_database, "gp register segment", NULL};
	char		segment_id[12];
	char		port[12];
	const char *params[3] = {segment_id, gp_host, port};
	uint32		wait_event;
	PGconn	   *conn;
	PGresult   *res;

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	wait_event = WaitEventExtensionNew("GpRegisterSegment");

	conn = libpqsrv_connect_params(keywords, values, true, wait_event);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		ereport(LOG,
				errmsg("could not connect to the coordinator: %s",
					   pchomp(PQerrorMessage(conn))));
		libpqsrv_disconnect(conn);
		proc_exit(1);
	}

	snprintf(segment_id, sizeof(segment_id), "%d", gp_segment_id);
	snprintf(port, sizeof(port), "%d", PostPortNumber);
	res = libpqsrv_exec_params(conn,
							   "SELECT gp.register_segment($1, $2, $3)",
							   3, NULL, params, NULL, NULL, 0, wait_event);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		ereport(LOG,
				errmsg("could not register as segment %d on the coordinator: %s",
					   gp_segment_id, pchomp(PQerrorMessage(conn))));
		PQclear(res);
		libpqsrv_disconnect(conn);
		proc_exit(1);
	}

	PQclear(res);
	libpqsrv_disconnect(conn);
	ereport(LOG,
			errmsg("registered as segment %d on the coordinator at %s:%d",
				   gp_segment_id, gp_host, PostPortNumber));
	proc_exit(0);
}
