/* -------------------------------------------------------------------------
 *
 * oids.c
 *		The same OIDs of objects on all servers of the cluster
 *
 * On the coordinator, new_oid_hook records the OID of every new catalog row
 * of a transaction, which it takes from GetNewOidWithIndex() itself.  A
 * segment gets the OIDs of a command in gp.dispatch_oids, as "catalog:oid"
 * pairs separated by commas, and its hook proposes them in turn, checking
 * that each is for the same catalog.  So a command that the segment runs
 * as the coordinator did gives its objects the same OIDs.
 *
 * A segment creates no objects of its own unless gp.allow_local_ddl is on.
 * Their OIDs come from the allocator with a large offset, so they do not
 * collide with the OIDs that the coordinator assigns.
 *
 * A command that cannot run in a transaction block gets its OIDs by SET in
 * a session, not SET LOCAL, with gp.dispatch_oids_session on.  The
 * transaction of that SET does not check that the OIDs are used; those of
 * the command do.  On the coordinator, such a command may commit several
 * times, so its OIDs are kept beyond a transaction while gp_oids_keep() is on.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/oids.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "gp.h"

/* The coordinator never assigns this range; segments use it for local objects */
#define GP_LOCAL_OID_OFFSET (UINT64_C(1) << 30)
#define GP_OIDS_FILE_MAX (16 * 1024 * 1024)

/* A new catalog row and its OID */
typedef struct GpOid
{
	Oid			catalog;
	Oid			oid;
} GpOid;

/* A list of OIDs, as gp.dispatch_oids gives it */
typedef struct GpOidList
{
	int			count;
	GpOid		oids[FLEXIBLE_ARRAY_MEMBER];
} GpOidList;

/* GUC variables */
char	   *gp_dispatch_oids = NULL;
bool		gp_dispatch_oids_session = false;
bool		gp_allow_local_ddl = false;

/*
 * Coordinator: OIDs of the current transaction, in TopTransactionContext,
 * or in TopMemoryContext while kept
 */
static GpOid *recorded = NULL;
static int	nrecorded = 0;
static int	maxrecorded = 0;
static bool keeping = false;

/* Coordinator: GetNewOidWithIndex() is called from the hook */
static bool recording = false;

/* Segment: OIDs from the coordinator, and how many are used */
static GpOidList *dispatched = NULL;
static int	nused = 0;

/* Segment: the OIDs were set in this transaction for the next ones */
static bool skip_check = false;

PG_FUNCTION_INFO_V1(gp_recorded_oids);

static Oid
gp_new_oid_coordinator(Relation relation, Oid indexId, AttrNumber oidcolumn)
{
	Oid			oid;

	if (recording || !IsUnderPostmaster)
		return InvalidOid;

	recording = true;
	PG_TRY();
	{
		oid = GetNewOidWithIndex(relation, indexId, oidcolumn);
	}
	PG_FINALLY();
	{
		recording = false;
	}
	PG_END_TRY();

	if (oid >= GP_LOCAL_OID_OFFSET)
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("the OID range for coordinator objects is exhausted"),
				errdetail("OIDs at and above %u are reserved for local objects of segments.",
						  (unsigned int) GP_LOCAL_OID_OFFSET));

	if (nrecorded == maxrecorded)
	{
		if (recorded == NULL)
		{
			maxrecorded = 64;
			recorded = MemoryContextAlloc(keeping ? TopMemoryContext :
										  TopTransactionContext,
										  sizeof(GpOid) * maxrecorded);
		}
		else
		{
			maxrecorded *= 2;
			recorded = repalloc(recorded, sizeof(GpOid) * maxrecorded);
		}
	}
	recorded[nrecorded].catalog = RelationGetRelid(relation);
	recorded[nrecorded].oid = oid;
	nrecorded++;

	return oid;
}

static Oid
gp_new_oid_segment(Relation relation, Oid indexId, AttrNumber oidcolumn)
{
	const GpOid *next;

	if (!IsUnderPostmaster)
		return InvalidOid;

	if (dispatched == NULL)
	{
		uint64		oid;

		if (!gp_allow_local_ddl)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot create objects on a segment"),
					errdetail("Objects of a segment get their OIDs from the coordinator."),
					errhint("Run the command on the coordinator, or set \"gp.allow_local_ddl\" for maintenance."));

		/*
		 * The allocator gives a fresh, persistent sequence, and the offset puts
		 * local objects above the OIDs that the coordinator assigns
		 */
		oid = (uint64) GetNewObjectId() + GP_LOCAL_OID_OFFSET;
		if (oid > PG_UINT32_MAX)
			ereport(ERROR,
					errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					errmsg("the OID range for local objects of a segment is exhausted"));
		return (Oid) oid;
	}

	if (nused >= dispatched->count)
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("the coordinator assigned %d OIDs, but the segment needs more",
					   dispatched->count),
				errdetail("A new row of catalog \"%s\" has no OID.",
						  RelationGetRelationName(relation)));

	next = &dispatched->oids[nused];
	if (next->catalog != RelationGetRelid(relation))
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("OID %u from the coordinator is for catalog \"%s\", not \"%s\"",
					   next->oid, get_rel_name(next->catalog),
					   RelationGetRelationName(relation)),
				errdetail("The segment runs the command differently from the coordinator."));
	nused++;
	return next->oid;
}

static void
gp_oids_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:

			/*
			 * Not in a parallel worker: it gets gp.dispatch_oids from the
			 * leader, but creates no catalog rows
			 */
			if (dispatched != NULL && !skip_check && nused < dispatched->count)
				ereport(ERROR,
						errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("the coordinator assigned %d OIDs, but the segment used %d",
							   dispatched->count, nused),
						errdetail("The segment runs the command differently from the coordinator."));
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			skip_check = false;
			if (!keeping)
			{
				/* TopTransactionContext is gone with the list */
				recorded = NULL;
				nrecorded = maxrecorded = 0;
			}
			break;
		default:
			break;
	}
}

/*
 * Install the hook of the role; called from _PG_init()
 */
void
gp_oids_init(void)
{
	if (gp_role == GP_ROLE_COORDINATOR)
		new_oid_hook = gp_new_oid_coordinator;
	else if (gp_role == GP_ROLE_SEGMENT)
		new_oid_hook = gp_new_oid_segment;
	else
		return;
	RegisterXactCallback(gp_oids_xact_callback, NULL);
}

/*
 * GUC check hook of gp.dispatch_oids: parse the list, and refuse a new one
 * while the OIDs of the last are not all used
 */
bool
gp_check_dispatch_oids(char **newval, void **extra, GucSource source)
{
	const char *p = *newval;
	int			count = 0;
	GpOidList  *list;

	if (gp_role == GP_ROLE_SEGMENT && dispatched != NULL &&
		nused < dispatched->count && **newval != '\0')
	{
		GUC_check_errdetail("The segment used %d of %d OIDs of the last command.",
							nused, dispatched->count);
		return false;
	}

	if (*p == '\0')
		return true;

	for (const char *c = p; *c; c++)
		if (*c == ',')
			count++;
	count++;

	list = guc_malloc(LOG, offsetof(GpOidList, oids) + sizeof(GpOid) * count);
	if (list == NULL)
		return false;
	list->count = 0;
	while (*p != '\0')
	{
		unsigned long catalog;
		unsigned long oid;
		char	   *end;

		errno = 0;
		catalog = strtoul(p, &end, 10);
		if (end == p || *end != ':' || errno != 0)
			break;
		p = end + 1;
		oid = strtoul(p, &end, 10);
		if (end == p || (*end != ',' && *end != '\0') || errno != 0 ||
			catalog > PG_UINT32_MAX || oid > PG_UINT32_MAX)
			break;
		list->oids[list->count].catalog = (Oid) catalog;
		list->oids[list->count].oid = (Oid) oid;
		list->count++;
		p = *end == ',' ? end + 1 : end;
	}
	if (*p != '\0' || list->count != count)
	{
		guc_free(list);
		GUC_check_errdetail("A list of OIDs is \"catalog:oid\" pairs separated by commas.");
		return false;
	}

	*extra = list;
	return true;
}

void
gp_assign_dispatch_oids(const char *newval, void *extra)
{
	dispatched = (GpOidList *) extra;
	nused = 0;
	skip_check = gp_dispatch_oids_session;
}

/*
 * Coordinator: keep the OIDs recorded from now on beyond the transaction,
 * for a command that commits on its own, until called with false
 */
void
gp_oids_keep(bool keep)
{
	if (keep == keeping)
		return;
	keeping = keep;

	if (keep)
	{
		if (recorded != NULL)
		{
			GpOid	   *copy = MemoryContextAlloc(TopMemoryContext,
												  sizeof(GpOid) * maxrecorded);

			memcpy(copy, recorded, sizeof(GpOid) * nrecorded);
			recorded = copy;
		}
	}
	else
	{
		if (recorded != NULL)
			pfree(recorded);
		recorded = NULL;
		nrecorded = maxrecorded = 0;
	}
}

/*
 * Coordinator: the position in the OIDs of the transaction, and the OIDs
 * recorded since one, as gp.dispatch_oids takes them
 */
int
gp_oids_mark(void)
{
	return nrecorded;
}

char *
gp_oids_since(int mark)
{
	StringInfoData buf;

	initStringInfo(&buf);
	for (int i = mark; i < nrecorded; i++)
		appendStringInfo(&buf, "%s%u:%u", i > mark ? "," : "",
						 recorded[i].catalog, recorded[i].oid);
	return buf.data;
}

/* Keep the actual creation list for segments that register later */
void
gp_oids_save_extension(int mark)
{
	char	   *list = gp_oids_since(mark);
	char	   *path = psprintf("%s/gp_extension_oids", DataDir);
	char	   *tmp = psprintf("%s/gp_extension_oids.tmp", DataDir);
	int			fd;
	size_t		len;
	size_t		written = 0;

	if (gp_role != GP_ROLE_COORDINATOR || list[0] == '\0')
		return;
	fd = OpenTransientFilePerm(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	len = strlen(list);
	if (fd < 0)
		ereport(ERROR,
				errmsg("could not create the extension OID file: %m"));
	while (written < len)
	{
		ssize_t		n = write(fd, list + written, len - written);

		if (n <= 0)
			ereport(ERROR,
					errmsg("could not write the extension OID file: %m"));
		written += n;
	}
	if (fsync(fd) != 0)
		ereport(ERROR,
				errmsg("could not sync the extension OID file: %m"));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				errmsg("could not close the extension OID file: %m"));
	if (rename(tmp, path) != 0)
		ereport(ERROR,
				errmsg("could not install the extension OID file: %m"));
}

PG_FUNCTION_INFO_V1(gp_interconnect_secret);

Datum
gp_interconnect_secret(PG_FUNCTION_ARGS)
{
	unsigned char bytes[32];
	char		hex[65];

	if (!pg_strong_random(bytes, sizeof(bytes)))
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate the interconnect token"));
	hex_encode((const char *) bytes, sizeof(bytes), hex);
	hex[64] = '\0';
	PG_RETURN_TEXT_P(cstring_to_text(hex));
}

PG_FUNCTION_INFO_V1(gp_extension_oids);

Datum
gp_extension_oids(PG_FUNCTION_ARGS)
{
	char		path[MAXPGPATH];
	FILE	   *file;
	StringInfoData list;
	char		buf[8192];

	if (gp_role != GP_ROLE_COORDINATOR)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("extension OIDs are read on the coordinator only"));
	snprintf(path, sizeof(path), "%s/gp_extension_oids", DataDir);
	file = AllocateFile(path, "r");
	if (file == NULL)
		PG_RETURN_NULL();
	initStringInfo(&list);
	while (fgets(buf, sizeof(buf), file) != NULL)
	{
		if (list.len > GP_OIDS_FILE_MAX)
			ereport(ERROR,
					errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("the extension OID file is too long"));
		appendStringInfoString(&list, buf);
	}
	FreeFile(file);
	PG_RETURN_TEXT_P(cstring_to_text(list.data));
}

/*
 * Coordinator: whether an object made since the mark depends on what the
 * segments do not have, as pg_depend tells: a temporary object, or a
 * materialized view or a foreign table, which never leave the coordinator.
 * Such an object stays with what it depends on on the coordinator.
 */
bool
gp_oids_depend_on_local(int mark)
{
	StringInfoData sql;
	bool		dependent;
	bool		fresh;

	if (nrecorded == mark)
		return false;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT 1 FROM pg_catalog.pg_depend d "
					 "WHERE ROW (d.classid, d.objid) IN (VALUES ");
	for (int i = mark; i < nrecorded; i++)
		appendStringInfo(&sql, "%s(%u::pg_catalog.oid, %u::pg_catalog.oid)",
						 i > mark ? ", " : "",
						 recorded[i].catalog, recorded[i].oid);
	appendStringInfo(&sql,
					 ") AND (EXISTS (SELECT 1 FROM pg_catalog.pg_class t "
					 "WHERE t.oid OPERATOR(pg_catalog.=) d.refobjid "
					 "AND d.refclassid OPERATOR(pg_catalog.=) 'pg_catalog.pg_class'::regclass "
					 "AND (t.relnamespace OPERATOR(pg_catalog.=) pg_catalog.pg_my_temp_schema() "
					 "OR t.relkind IN ('m', 'f')))"
					 "OR EXISTS (SELECT 1 FROM pg_catalog.pg_type t "
					 "WHERE t.oid OPERATOR(pg_catalog.=) d.refobjid "
					 "AND d.refclassid OPERATOR(pg_catalog.=) 'pg_catalog.pg_type'::regclass "
					 "AND t.typnamespace OPERATOR(pg_catalog.=) pg_catalog.pg_my_temp_schema()) "
					 "OR EXISTS (SELECT 1 FROM pg_catalog.pg_proc t "
					 "WHERE t.oid OPERATOR(pg_catalog.=) d.refobjid "
					 "AND d.refclassid OPERATOR(pg_catalog.=) 'pg_catalog.pg_proc'::regclass "
					 "AND t.pronamespace OPERATOR(pg_catalog.=) pg_catalog.pg_my_temp_schema()))");

	/*
	 * The rows of the statement are newer than its snapshot, as it may run
	 * inside a function that has planned the next statement already; a
	 * parallel operation cannot take a snapshot, and writes nothing
	 */
	fresh = !IsInParallelMode();
	SPI_connect();
	if (fresh)
		PushActiveSnapshot(GetLatestSnapshot());
	dependent = SPI_execute(sql.data, true, 1) == SPI_OK_SELECT &&
		SPI_processed > 0;
	if (fresh)
		PopActiveSnapshot();
	SPI_finish();
	pfree(sql.data);
	return dependent;
}

/*
 * gp.recorded_oids(): the OIDs of the current transaction on the
 * coordinator, as gp.dispatch_oids takes them
 */
Datum
gp_recorded_oids(PG_FUNCTION_ARGS)
{
	if (gp_role != GP_ROLE_COORDINATOR)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("OIDs are recorded on the coordinator only"));

	PG_RETURN_TEXT_P(cstring_to_text(gp_oids_since(0)));
}
