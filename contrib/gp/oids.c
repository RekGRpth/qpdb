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
 * A segment creates no objects of its own unless gp.allow_local_ddl is on:
 * they would take OIDs that the coordinator may assign later.
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

#include "access/xact.h"
#include "catalog/catalog.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "gp.h"

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
bool		gp_allow_local_ddl = false;

/* Coordinator: OIDs of the current transaction, in TopTransactionContext */
static GpOid *recorded = NULL;
static int	nrecorded = 0;
static int	maxrecorded = 0;

/* Coordinator: GetNewOidWithIndex() is called from the hook */
static bool recording = false;

/* Segment: OIDs from the coordinator, and how many are used */
static GpOidList *dispatched = NULL;
static int	nused = 0;

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

	if (nrecorded == maxrecorded)
	{
		if (recorded == NULL)
		{
			maxrecorded = 64;
			recorded = MemoryContextAlloc(TopTransactionContext,
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
		if (!gp_allow_local_ddl)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot create objects on a segment"),
					errdetail("Objects of a segment get their OIDs from the coordinator."),
					errhint("Run the command on the coordinator, or set \"gp.allow_local_ddl\" for maintenance."));
		return InvalidOid;
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
			if (dispatched != NULL && nused < dispatched->count)
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
			/* TopTransactionContext is gone with the list */
			recorded = NULL;
			nrecorded = maxrecorded = 0;
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
