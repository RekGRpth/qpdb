/* -------------------------------------------------------------------------
 *
 * segments.c
 *		Copy of gp.segments in the shared memory of the coordinator
 *
 * gp.segments lives in one database, gp.database, but sessions in any
 * database need the segments.  A background worker of the coordinator
 * reads the table into shared memory at start, and again whenever a
 * transaction that changed the table commits: a statement trigger on the
 * table asks for that, and an object access hook when the table is dropped
 * (DROP EXTENSION, DROP TABLE or DROP SCHEMA ... CASCADE).  The table is
 * small and changes rarely, so the worker reads all of it each time.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/segments.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "gp.h"

/* Seconds before the postmaster restarts a failed loader */
#define GP_LOADER_RESTART 5

typedef struct GpSegmentsShared
{
	LWLock		lock;			/* protects the fields below */
	Latch	   *loader_latch;	/* NULL while the loader is not running */
	bool		reload;			/* gp.segments changed since the last load */
	bool		loaded;			/* segments hold gp.segments */
	int			nsegments;
	GpSegment	segments[FLEXIBLE_ARRAY_MEMBER];
} GpSegmentsShared;

/* NULL except on the coordinator */
static GpSegmentsShared *shared = NULL;

/* The current transaction changed gp.segments */
static bool reload_at_commit = false;

static object_access_hook_type prev_object_access_hook = NULL;

static void gp_segments_shmem_request(void *arg);
static void gp_segments_shmem_init(void *arg);
static void gp_segments_object_access(ObjectAccessType access, Oid classId,
									  Oid objectId, int subId, void *arg);

static const ShmemCallbacks gp_segments_shmem_callbacks = {
	.request_fn = gp_segments_shmem_request,
	.init_fn = gp_segments_shmem_init,
};

pg_noreturn PGDLLEXPORT void gp_segments_loader_main(Datum main_arg);

PG_FUNCTION_INFO_V1(gp_segments_changed);
PG_FUNCTION_INFO_V1(gp_cluster_segments);

/*
 * Set up the copy on the coordinator; called from _PG_init()
 */
void
gp_segments_init(void)
{
	BackgroundWorker worker = {0};

	RegisterShmemCallbacks(&gp_segments_shmem_callbacks);

	prev_object_access_hook = object_access_hook;
	object_access_hook = gp_segments_object_access;

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = GP_LOADER_RESTART;
	strlcpy(worker.bgw_library_name, "gp", BGW_MAXLEN);
	strlcpy(worker.bgw_function_name, "gp_segments_loader_main", BGW_MAXLEN);
	strlcpy(worker.bgw_name, "gp segments loader", BGW_MAXLEN);
	strlcpy(worker.bgw_type, "gp segments loader", BGW_MAXLEN);
	RegisterBackgroundWorker(&worker);
}

static void
gp_segments_shmem_request(void *arg)
{
	ShmemRequestStruct(.name = "gp segments",
					   .size = add_size(offsetof(GpSegmentsShared, segments),
										mul_size(gp_max_segments,
												 sizeof(GpSegment))),
					   .ptr = (void **) &shared,
		);
}

static void
gp_segments_shmem_init(void *arg)
{
	LWLockInitialize(&shared->lock, LWLockNewTrancheId("gp segments"));
	shared->loader_latch = NULL;
	shared->reload = false;
	shared->loaded = false;
	shared->nsegments = 0;
}

/*
 * Read gp.segments of the database gp.database into the shared memory.
 * Without the extension in that database there are no segments.
 */
static void
gp_segments_load(void)
{
	GpSegment  *segments = palloc0_array(GpSegment, gp_max_segments);
	int			nsegments = 0;

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	if (SPI_execute("SELECT to_regclass('gp.segments') IS NOT NULL",
					true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not look for gp.segments");
	if (strcmp(SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1),
			   "t") == 0)
	{
		if (SPI_execute("SELECT segment_id, host, port FROM gp.segments "
						"ORDER BY segment_id", true, 0) != SPI_OK_SELECT)
			elog(ERROR, "could not read gp.segments");
		if (SPI_processed > gp_max_segments)
			ereport(WARNING,
					errmsg("gp.segments has %" PRIu64 " segments, only the first %d are used",
						   SPI_processed, gp_max_segments),
					errhint("Raise \"gp.max_segments\" on the coordinator."));

		for (; nsegments < SPI_processed && nsegments < gp_max_segments; nsegments++)
		{
			HeapTuple	tuple = SPI_tuptable->vals[nsegments];
			TupleDesc	tupdesc = SPI_tuptable->tupdesc;
			GpSegment  *segment = &segments[nsegments];

			segment->segment_id = pg_strtoint32(SPI_getvalue(tuple, tupdesc, 1));
			strlcpy(segment->host, SPI_getvalue(tuple, tupdesc, 2),
					sizeof(segment->host));
			segment->port = pg_strtoint32(SPI_getvalue(tuple, tupdesc, 3));
		}
	}

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	LWLockAcquire(&shared->lock, LW_EXCLUSIVE);
	memcpy(shared->segments, segments, sizeof(GpSegment) * nsegments);
	shared->nsegments = nsegments;
	shared->loaded = true;
	LWLockRelease(&shared->lock);

	pfree(segments);
	elog(DEBUG1, "loaded %d segments", nsegments);
}

static void
gp_segments_loader_detach(int code, Datum arg)
{
	LWLockAcquire(&shared->lock, LW_EXCLUSIVE);
	shared->loader_latch = NULL;
	LWLockRelease(&shared->lock);
}

/*
 * Background worker of the coordinator: load gp.segments, then load it
 * again whenever a change of it commits
 */
void
gp_segments_loader_main(Datum main_arg)
{
	uint32		wait_event;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(gp_database, NULL, 0);
	wait_event = WaitEventExtensionNew("GpSegmentsLoader");

	LWLockAcquire(&shared->lock, LW_EXCLUSIVE);
	shared->loader_latch = MyLatch;
	shared->reload = true;
	LWLockRelease(&shared->lock);
	before_shmem_exit(gp_segments_loader_detach, (Datum) 0);

	for (;;)
	{
		bool		reload;

		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		LWLockAcquire(&shared->lock, LW_EXCLUSIVE);
		reload = shared->reload;
		shared->reload = false;
		LWLockRelease(&shared->lock);

		if (reload)
			gp_segments_load();

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, -1L,
						 wait_event);
	}
}

/*
 * Wake the loader when a transaction that changed gp.segments commits
 */
static void
gp_segments_xact_callback(XactEvent event, void *arg)
{
	Latch	   *latch;

	if (!reload_at_commit)
		return;

	switch (event)
	{
		case XACT_EVENT_PRE_PREPARE:
			/* COMMIT PREPARED would not wake the loader */
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot PREPARE a transaction that has changed gp.segments"));
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			reload_at_commit = false;
			if (shared == NULL)
				break;
			LWLockAcquire(&shared->lock, LW_EXCLUSIVE);
			shared->reload = true;
			latch = shared->loader_latch;
			LWLockRelease(&shared->lock);
			if (latch != NULL)
				SetLatch(latch);
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			reload_at_commit = false;
			break;
		default:
			break;
	}
}

/*
 * Load gp.segments again when the current transaction commits
 */
static void
gp_segments_reload_at_commit(void)
{
	static bool registered = false;

	if (!registered)
	{
		RegisterXactCallback(gp_segments_xact_callback, NULL);
		registered = true;
	}
	reload_at_commit = true;
}

/*
 * Statement trigger of gp.segments
 */
Datum
gp_segments_changed(PG_FUNCTION_ARGS)
{
	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				errmsg("gp_segments_changed: not called by trigger manager"));

	gp_segments_reload_at_commit();

	return PointerGetDatum(NULL);
}

/*
 * Object access hook: a dropped gp.segments empties the copy.  The trigger
 * does not fire then, and the copy would keep the segments until a restart.
 */
static void
gp_segments_object_access(ObjectAccessType access, Oid classId,
						  Oid objectId, int subId, void *arg)
{
	char	   *relname;

	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	if (access != OAT_DROP || classId != RelationRelationId || subId != 0)
		return;

	/* The relation is still in the catalog before its deletion */
	relname = get_rel_name(objectId);
	if (relname != NULL && strcmp(relname, "segments") == 0)
	{
		char	   *nspname = get_namespace_name(get_rel_namespace(objectId));

		if (nspname != NULL && strcmp(nspname, "gp") == 0)
			gp_segments_reload_at_commit();
	}
}

/*
 * Copy the segments, ordered by number, into *segments, palloc'd, and
 * return their number.  Only the coordinator knows them, and only once the
 * loader has read gp.segments.
 */
int
gp_segments_get(GpSegment **segments)
{
	int			nsegments;
	bool		loaded;

	if (shared == NULL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("segments of the cluster are known on the coordinator only"));

	*segments = palloc_array(GpSegment, gp_max_segments);
	LWLockAcquire(&shared->lock, LW_SHARED);
	loaded = shared->loaded;
	nsegments = shared->nsegments;
	memcpy(*segments, shared->segments, sizeof(GpSegment) * nsegments);
	LWLockRelease(&shared->lock);

	if (!loaded)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("segments of the cluster are not loaded yet"));

	return nsegments;
}

/*
 * gp.cluster_segments(): the copy of gp.segments, from any database
 */
Datum
gp_cluster_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	GpSegment  *segments;
	int			nsegments;

	nsegments = gp_segments_get(&segments);
	InitMaterializedSRF(fcinfo, 0);

	for (int i = 0; i < nsegments; i++)
	{
		Datum		values[3];
		bool		nulls[3] = {0};

		values[0] = Int32GetDatum(segments[i].segment_id);
		values[1] = CStringGetTextDatum(segments[i].host);
		values[2] = Int32GetDatum(segments[i].port);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	pfree(segments);
	return (Datum) 0;
}
