/* -------------------------------------------------------------------------
 *
 * recovery.c
 *		Recovery of the transactions the segments keep prepared
 *
 * A distributed transaction is committed on the segments by COMMIT
 * PREPARED, which reaches them one by one, and a segment that misses it
 * keeps the transaction prepared, holding its locks and its rows from
 * everyone.  The coordinator used to leave such a transaction to a human;
 * a background worker of the coordinator resolves it now.  Every few
 * seconds it asks every segment for the prepared transactions whose
 * global name is one of ours, looks into the clog of the coordinator for
 * the fate of the transaction, and commits or rolls back what it finds.
 * The fate is what the clog says: committed, aborted, and what it does
 * not know — the transaction the coordinator died in the middle of — it
 * rolls back, as the safest of the two.
 *
 * The worker takes the lock of the distributed snapshot for a pass, so it
 * never resolves a transaction whose commit is running right now: that
 * one holds the lock from the moment the segments are all prepared to the
 * moment they all hear COMMIT PREPARED, and the worker waits it out.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/recovery.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "fmgr.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "gp.h"

/*
 * The helpers header wraps the libpq functions in versions that attach the
 * results to the memory context they are made in, so a result goes away with
 * its context even when an error jumps over its clearing.  A pass works in a
 * context of its own for that, and takes its results with it when it ends.
 */

#define GP_RECOVERY_RESTART 5

/* The wait event of the connections of the worker to the segments */
static uint32 gp_recovery_wait_event = 0;

/* Seconds between the passes of the worker */
static int	gp_recovery_interval = 5;

pg_noreturn PGDLLEXPORT void gp_recovery_worker_main(Datum main_arg);

static void gp_recovery_pass(void);

PG_FUNCTION_INFO_V1(gp_recovery_pass_now);

Datum
gp_recovery_pass_now(PG_FUNCTION_ARGS)
{
	gp_recovery_pass();
	PG_RETURN_VOID();
}

/*
 * Set up the worker and its setting on the coordinator; called from _PG_init()
 */
void
gp_recovery_init(void)
{
	BackgroundWorker worker = {0};

	if (gp_role != GP_ROLE_COORDINATOR)
		return;

	DefineCustomIntVariable("gp.recovery_interval",
							"Seconds between the passes of the recovery of the transactions the segments keep prepared.",
							NULL,
							&gp_recovery_interval,
							5,
							1,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = GP_RECOVERY_RESTART;
	strlcpy(worker.bgw_library_name, "gp", BGW_MAXLEN);
	strlcpy(worker.bgw_function_name, "gp_recovery_worker_main", BGW_MAXLEN);
	strlcpy(worker.bgw_name, "gp recovery", BGW_MAXLEN);
	strlcpy(worker.bgw_type, "gp recovery", BGW_MAXLEN);
	RegisterBackgroundWorker(&worker);
}

static void
gp_recovery_segment(int segment_id, const char *shost, int sport)
{
	const char *keywords[] = {"host", "port", "dbname", "application_name",
	NULL};
	const char *values[5];
	char		port[12];
	PGconn	   *conn = NULL;
	PGresult   *res;
	int			nrows;

	snprintf(port, sizeof(port), "%d", sport);
	values[0] = shost;
	values[1] = port;
	values[2] = gp_database;
	values[3] = "gp recovery";
	values[4] = NULL;

	PG_TRY();
	{
		conn = libpqsrv_connect_params(keywords, values, false,
									   gp_recovery_wait_event);
		if (PQstatus(conn) != CONNECTION_OK)
		{
			elog(WARNING, "could not connect to segment %d at %s:%d: %s",
				 segment_id, shost, sport,
				 pchomp(PQerrorMessage(conn)));
			libpqsrv_disconnect(conn);
			conn = NULL;
			return;
		}

		res = libpqsrv_exec_params(conn,
								   "SELECT gid FROM pg_prepared_xacts "
								   "WHERE gid LIKE 'gp\\_%'",
								   0, NULL, NULL, NULL, NULL, 0,
								   gp_recovery_wait_event);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			elog(WARNING, "could not read the prepared transactions of segment %d: %s",
				 segment_id, pchomp(PQresultErrorMessage(res)));
			PQclear(res);
			libpqsrv_disconnect(conn);
			conn = NULL;
			return;
		}

		nrows = PQntuples(res);
		for (int i = 0; i < nrows; i++)
		{
			char	   *gid = pstrdup(PQgetvalue(res, i, 0));
			const char *fate;
			uint64		xid64;
			TransactionId xid;
			StringInfoData cmd;
			PGresult   *done;

			/* The fate of the transaction, as the clog of the coordinator says */
			xid64 = strtoull(gid + strlen("gp_"), NULL, 10);
			{
				FullTransactionId fxid;

				fxid.value = xid64;
				xid = XidFromFullTransactionId(fxid);
			}

			if (TransactionIdIsInProgress(xid))
			{
				elog(DEBUG1, "transaction %u of %s is still running", xid, gid);
				continue;
			}
			if (TransactionIdDidCommit(xid))
				fate = "COMMIT";
			else
			{
				/*
				 * Aborted, or the coordinator died before it said: the
				 * safest of the two is to undo
				 */
				fate = "ROLLBACK";
				if (!TransactionIdDidAbort(xid))
					elog(WARNING, "the fate of transaction %u of %s is unknown; rolling it back",
						 xid, gid);
			}

			initStringInfo(&cmd);
			appendStringInfoString(&cmd, fate);
			appendStringInfo(&cmd, " PREPARED '%s'", gid);

			done = libpqsrv_exec_params(conn, cmd.data, 0, NULL, NULL, NULL,
										NULL, 0, gp_recovery_wait_event);
			pfree(cmd.data);
			if (PQresultStatus(done) == PGRES_COMMAND_OK)
				elog(LOG, "%s PREPARED '%s' on segment %d", fate, gid,
					 segment_id);
			else
			{
				const char *sqlstate = PQresultErrorField(done, PG_DIAG_SQLSTATE);

				/*
				 * Another pass, or the commit of the transaction itself, has
				 * resolved it already
				 */
				if (sqlstate == NULL || strcmp(sqlstate, "42704") != 0)
					elog(WARNING, "could not %s PREPARED '%s' on segment %d: %s",
						 fate, gid, segment_id,
						 pchomp(PQresultErrorMessage(done)));
			}
			PQclear(done);
		}

		PQclear(res);
		libpqsrv_disconnect(conn);
		conn = NULL;
	}
	PG_CATCH();
	{
		/* What the segment still keeps prepared stays for a later pass */
		libpqsrv_disconnect(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * One pass: every segment, under the lock of the distributed snapshot.  The
 * pass runs in a context of its own, thrown away at its end: whatever the
 * pass leaves behind goes with the context, and an error that jumps over the
 * clearing of a result does not outlive the pass either.  No error of a pass
 * gets out: the worker tries again later.
 */
static void
gp_recovery_pass(void)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	MemoryContext passcxt = AllocSetContextCreate(TopMemoryContext,
												  "gp recovery pass",
												  ALLOCSET_SMALL_SIZES);
	bool		locked = false;

	PG_TRY();
	{
		GpSegment  *segments;
		int			nsegments;

		MemoryContextSwitchTo(passcxt);
		nsegments = gp_segments_get(&segments);
		if (nsegments > 0)
		{
			gp_dtx_lock();
			locked = true;
			for (int i = 0; i < nsegments; i++)
			{
				/*
				 * Copy the address out: the array is shared with the
				 * loader, which rewrites it when the cluster changes
				 */
				char		shost[GP_HOST_LEN];
				int			sid = segments[i].segment_id;
				int			sport = segments[i].port;

				strlcpy(shost, segments[i].host, sizeof(shost));
				gp_recovery_segment(sid, shost, sport);
			}
		}
	}
	PG_CATCH();
	{
		EmitErrorReport();
		FlushErrorState();
		MemoryContextSwitchTo(oldcxt);
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(passcxt);

	if (locked)
		gp_dtx_unlock();
}

pg_noreturn void
gp_recovery_worker_main(Datum main_arg)
{
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(gp_database, NULL, 0);
	gp_recovery_wait_event = WaitEventExtensionNew("GpRecovery");

	for (;;)
	{
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		gp_recovery_pass();

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 gp_recovery_interval * 1000L,
						 PG_WAIT_EXTENSION);
	}
}
