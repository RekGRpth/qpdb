/* -------------------------------------------------------------------------
 *
 * gather.c
 *		SELECT from a distributed table: all rows to the coordinator
 *
 * The table on the coordinator holds no rows, and the rows are on the
 * segments.  The hook of the paths of a relation puts a custom scan,
 * GpGather, in place of every path to a distributed table; the planner
 * plans the rest, the joins, the aggregates and the sorts, on the
 * coordinator as it would for any table.  GpGather sends a serialized scan to all segments and reads their binary
 * rows into a tuplestore, from which the rows go up the plan.  The conditions
 * of the query and the projection run on the coordinator, so what comes is
 * always correct, if not always cheap.  A replicated table has all its rows on
 * every segment, so only one of them is asked.
 *
 * A clean read goes over the interconnect under the lock of the distributed
 * snapshot.  A segment that the transaction of the coordinator has written to
 * instead answers on its dispatched libpq connection, so it shows those writes.
 * FOR UPDATE and TABLESAMPLE on a distributed table are refused.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/gather.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/value.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "gp.h"

/* The cost of the trip to the segments, before the first row */
#define GP_GATHER_STARTUP_COST 100.0

/* A row from the segments costs more than one of a table */
#define GP_GATHER_ROW_COST_FACTOR 10.0

static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;
static bool fragment_plan = false;

/* Let a fragment for a segment have an ordinary scan, not another gather */
bool
gp_gather_fragment(bool enable)
{
	bool		old = fragment_plan;

	fragment_plan = enable;
	return old;
}

typedef struct GpGatherState
{
	CustomScanState css;
	char	   *sql;
	bool		replicated;
	bool		inherit;
	Tuplestorestate *store;		/* all rows, once read */
} GpGatherState;

static Node *gp_gather_create_state(CustomScan *cscan);

static const CustomScanMethods gp_gather_plan_methods = {
	.CustomName = "GpGather",
	.CreateCustomScanState = gp_gather_create_state,
};

/*
 * Read a clean query over the interconnect.  Dropped columns are explicit
 * typed nulls in the fragment, so its answer has the table's full tupledesc.
 * The statement's commit lock covers every remote scan, and every worker
 * imports the snapshot exported for this statement on its own segment.
 */
static bool
gp_gather_load_interconnect(GpGatherState *state)
{
	ScanState  *ss = &state->css.ss;
	Relation	rel = ss->ss_currentRelation;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	StringInfoData cols;
	StringInfoData sql;
	GpSegment  *segments;
	char	   *plan;
	char	   *db;
	GpSnapshots *snapshots;
	const char *user;
	int			nsegments;
	int			only = 0;

	/* The listener does not carry session_user or the session's RLS settings */
	if (GetUserId() != GetSessionUserId() || rel->rd_rel->relrowsecurity ||
		rel->rd_rel->relforcerowsecurity)
		return false;

	initStringInfo(&cols);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		appendStringInfo(&cols, "%s", cols.len > 0 ? ", " : "");
		if (att->attisdropped)
			appendStringInfo(&cols, "NULL::pg_catalog.int4 AS %s",
							 quote_identifier(NameStr(att->attname)));
		else
			appendStringInfoString(&cols, quote_identifier(NameStr(att->attname)));
	}
	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT %s FROM %s%s", cols.data,
					 state->inherit ? "" : "ONLY ",
					 quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
											  RelationGetRelationName(rel)));
	plan = gp_interconnect_plan(sql.data);
	db = get_database_name(MyDatabaseId);
	user = GetUserNameFromId(GetUserId(), false);
	nsegments = gp_segments_get(&segments);
	if (state->replicated)
	{
		if (nsegments == 0)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("the cluster has no segments"));
		only = segments[MyProcPid % nsegments].segment_id;
	}

	/* A segment whose broker has not reported its port is not ready yet */
	for (int i = 0; i < nsegments; i++)
	{
		if (only != 0 && segments[i].segment_id != only)
			continue;
		if (segments[i].interconnect_port == 0)
			return false;
	}

	snapshots = gp_dtx_statement_snapshots(ss->ps.state);
	if (snapshots == NULL)
		return false;
	state->store = tuplestore_begin_heap(true, false, work_mem);
	for (int i = 0; i < nsegments; i++)
	{
		if (only != 0 && segments[i].segment_id != only)
			continue;
		if (!gp_interconnect_query_try(state->store, tupdesc, db, user, plan,
								   segments[i].host,
								   segments[i].interconnect_port, 0,
								   snapshots))
		{
			tuplestore_end(state->store);
			state->store = NULL;
			return false;
		}
	}
	return true;
}

/*
 * A dispatched connection is still needed to see this transaction's writes.
 */
static void
gp_gather_load_libpq(GpGatherState *state)
{
	ScanState  *ss = &state->css.ss;
	TupleDesc	tupdesc = RelationGetDescr(ss->ss_currentRelation);
	int			natts = tupdesc->natts;
	int		   *colmap = palloc_array(int, natts);
	FmgrInfo   *recv = palloc0_array(FmgrInfo, natts);
	Oid		   *ioparam = palloc_array(Oid, natts);
	int			ncols = 0;
	TupleTableSlot *loadslot;
	MemoryContext rowcxt;
	GpQuery    *query;
	PGresult   *res;
	int			only = 0;

	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			recvfn;

		if (att->attisdropped)
			continue;
		getTypeBinaryInputInfo(att->atttypid, &recvfn, &ioparam[i]);
		fmgr_info(recvfn, &recv[i]);
		colmap[ncols++] = i;
	}

	state->store = tuplestore_begin_heap(true, false, work_mem);
	loadslot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	rowcxt = AllocSetContextCreate(CurrentMemoryContext, "gather row",
								   ALLOCSET_DEFAULT_SIZES);

	if (state->replicated)
	{
		GpSegment  *segments;
		int			n = gp_segments_get(&segments);

		if (n == 0)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("the cluster has no segments"));
		only = segments[MyProcPid % n].segment_id;
	}

	query = gp_query_start(state->sql, only);
	while ((res = gp_query_next(query)) != NULL)
	{
		MemoryContext oldcontext;

		if (PQnfields(res) != ncols)
			ereport(ERROR,
					errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("a segment returned %d columns, but table \"%s\" has %d",
						   PQnfields(res),
						   RelationGetRelationName(ss->ss_currentRelation),
						   ncols));

		ExecClearTuple(loadslot);
		oldcontext = MemoryContextSwitchTo(rowcxt);
		for (int i = 0; i < natts; i++)
		{
			loadslot->tts_values[i] = (Datum) 0;
			loadslot->tts_isnull[i] = true;
		}
		for (int c = 0; c < ncols; c++)
		{
			int			i = colmap[c];
			StringInfoData buf;

			if (PQgetisnull(res, 0, c))
				continue;

			/* libpq ends every value with a zero byte, as the receive functions expect */
			buf.data = PQgetvalue(res, 0, c);
			buf.len = PQgetlength(res, 0, c);
			buf.maxlen = buf.len + 1;
			buf.cursor = 0;
			loadslot->tts_values[i] = ReceiveFunctionCall(&recv[i], &buf,
														  ioparam[i],
														  TupleDescAttr(tupdesc, i)->atttypmod);
			loadslot->tts_isnull[i] = false;
		}
		ExecStoreVirtualTuple(loadslot);
		tuplestore_puttupleslot(state->store, loadslot);
		MemoryContextSwitchTo(oldcontext);
		MemoryContextReset(rowcxt);
		PQclear(res);
	}

	ExecDropSingleTupleTableSlot(loadslot);
	MemoryContextDelete(rowcxt);
}

static void
gp_gather_load(GpGatherState *state)
{
	gp_dtx_statement_start(state->css.ss.ps.state);
	if (XactIsoLevel == XACT_READ_COMMITTED && !gp_dispatch_in_xact() &&
		gp_gather_load_interconnect(state))
		return;
	gp_gather_load_libpq(state);
}

static void
gp_gather_begin(CustomScanState *node, EState *estate, int eflags)
{
	GpGatherState *state = (GpGatherState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	state->sql = strVal(linitial(cscan->custom_private));
	state->replicated = boolVal(lsecond(cscan->custom_private));
	state->inherit = boolVal(lthird(cscan->custom_private));
}

static TupleTableSlot *
gp_gather_next(ScanState *ss)
{
	GpGatherState *state = (GpGatherState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	gp_dtx_statement_start(state->css.ss.ps.state);
	if (state->store == NULL)
		gp_gather_load(state);

	/* At the end the slot is empty, which is what ExecScan() looks for */
	(void) tuplestore_gettupleslot(state->store, true, false, slot);
	return slot;
}

static bool
gp_gather_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
gp_gather_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, gp_gather_next, gp_gather_recheck);
}

static void
gp_gather_end(CustomScanState *node)
{
	GpGatherState *state = (GpGatherState *) node;

	if (state->store != NULL)
		tuplestore_end(state->store);
	state->store = NULL;
}

/* The rows are all here, so a rescan reads them again from the store */
static void
gp_gather_rescan(CustomScanState *node)
{
	GpGatherState *state = (GpGatherState *) node;

	if (state->store != NULL)
		tuplestore_rescan(state->store);
	ExecScanReScan(&node->ss);
}

static void
gp_gather_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GpGatherState *state = (GpGatherState *) node;

	if (es->verbose)
		ExplainPropertyText("Remote SQL", state->sql, es);
}

static const CustomExecMethods gp_gather_exec_methods = {
	.CustomName = "GpGather",
	.BeginCustomScan = gp_gather_begin,
	.ExecCustomScan = gp_gather_exec,
	.EndCustomScan = gp_gather_end,
	.ReScanCustomScan = gp_gather_rescan,
	.ExplainCustomScan = gp_gather_explain,
};

static Node *
gp_gather_create_state(CustomScan *cscan)
{
	GpGatherState *state = (GpGatherState *) newNode(sizeof(GpGatherState),
													 T_CustomScanState);

	state->css.flags = cscan->flags;
	state->css.methods = &gp_gather_exec_methods;
	/* A tuplestore hands out minimal tuples */
	state->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) state;
}

/*
 * Make a plan of the path: the conditions and the projection stay with the
 * node, the scan tuple is a row of the table
 */
static Plan *
gp_gather_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			   List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_private = best_path->custom_private;
	cscan->methods = &gp_gather_plan_methods;
	return &cscan->scan.plan;
}

static const CustomPathMethods gp_gather_path_methods = {
	.CustomName = "GpGather",
	.PlanCustomPath = gp_gather_plan,
};

/* Whether a plan node is the gather of a distributed table */
bool
gp_is_gather(Plan *plan)
{
	return IsA(plan, CustomScan) &&
		((CustomScan *) plan)->methods == &gp_gather_plan_methods;
}

/*
 * The query that the segments run, and their columns, as the node reads
 * them
 */
static char *
gp_gather_sql(RangeTblEntry *rte)
{
	Relation	rel = table_open(rte->relid, NoLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	StringInfoData cols;
	char	   *sql;

	initStringInfo(&cols);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		appendStringInfo(&cols, "%s%s", cols.len > 0 ? ", " : "",
						 quote_identifier(NameStr(att->attname)));
	}
	sql = psprintf("SELECT %s FROM %s%s", cols.data,
				   rte->inh ? "" : "ONLY ",
				   quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
											  RelationGetRelationName(rel)));
	table_close(rel, NoLock);
	return sql;
}

/*
 * The only path to a distributed table is GpGather
 */
static void
gp_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					RangeTblEntry *rte)
{
	GpPolicy	policy;
	CustomPath *cpath;
	Relation	relation;

	if (prev_set_rel_pathlist)
		prev_set_rel_pathlist(root, rel, rti, rte);

	if (!IsUnderPostmaster || fragment_plan || rte->rtekind != RTE_RELATION ||
		rel->reloptkind != RELOPT_BASEREL ||
		(rte->relkind != RELKIND_RELATION &&
		 rte->relkind != RELKIND_PARTITIONED_TABLE))
		return;
	if (!gp_policy_get(rte->relid, &policy))
		return;

	/*
	 * The table itself is empty here.  ANALYZE imports the sizes of the
	 * cluster into pg_class, but the ordinary heap estimate still looks at
	 * that empty file.  Recompute the sizes before making the only path.
	 */
	relation = table_open(rte->relid, NoLock);
	if (relation->rd_rel->reltuples >= 0)
	{
		rel->pages = relation->rd_rel->relpages;
		rel->tuples = relation->rd_rel->reltuples;
		set_baserel_size_estimates(root, rel);
	}
	table_close(relation, NoLock);

	if (rte->tablesample != NULL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("TABLESAMPLE is not supported on a distributed table"));
	foreach_node(PlanRowMark, rc, root->rowMarks)
	{
		if (rc->rti == rti)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("row locks are not supported on a distributed table"));
	}

	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	/*
	 * A partitioned table is read as a whole: the planner must not build its
	 * paths again from those of the partitions, which are empty here
	 */
	rel->part_scheme = NULL;
	rel->nparts = 0;
	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = rel->rows;
	cpath->path.startup_cost = GP_GATHER_STARTUP_COST +
		rel->baserestrictcost.startup;
	cpath->path.total_cost = cpath->path.startup_cost +
		rel->tuples * cpu_tuple_cost * GP_GATHER_ROW_COST_FACTOR +
		rel->baserestrictcost.per_tuple * rel->tuples;
	cpath->path.pathkeys = NIL;
	cpath->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cpath->custom_paths = NIL;
	cpath->custom_private = list_make3(makeString(gp_gather_sql(rte)),
									   makeBoolean(policy.replicated),
									   makeBoolean(rte->inh));
	cpath->methods = &gp_gather_path_methods;

	add_path(rel, &cpath->path);
}

/*
 * Install the hook on the coordinator; called from _PG_init()
 */
void
gp_gather_init(void)
{
	RegisterCustomScanMethods(&gp_gather_plan_methods);
	prev_set_rel_pathlist = set_rel_pathlist_hook;
	set_rel_pathlist_hook = gp_set_rel_pathlist;
}
