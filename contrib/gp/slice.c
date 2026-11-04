/* -------------------------------------------------------------------------
 *
 * slice.c
 *		Aggregates of a distributed table, computed on the segments
 *
 * GpGather brings every row of a distributed table to the coordinator,
 * and the aggregate above it runs there.  When the query groups by the
 * whole key of the distribution, every row of a group is on one segment,
 * so the segment can compute the group to the end, and the coordinator
 * only collects the finished groups.  A replicated table has all its rows
 * on every segment, so one segment answers the whole aggregate.  GpSlice
 * is the scan that runs such an aggregate on the segments and reads their
 * rows; the sort and the limit of the query stay on the coordinator.
 *
 * The aggregate is replaced after the planner has run, by a scan that
 * returns exactly the target list of the aggregate.  Queries whose target
 * list has junk entries — a grouping or a sort key that is not selected —
 * are left to the planner, as are joins, subqueries, windows and grouping
 * sets.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/slice.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/value.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/tuplestore.h"

#include "gp.h"

typedef struct GpSliceState
{
	CustomScanState css;
	char	   *sql;
	bool		replicated;
	Tuplestorestate *store;		/* all rows, once read */
} GpSliceState;

static Node *gp_slice_create_state(CustomScan *cscan);

static const CustomScanMethods gp_slice_plan_methods = {
	.CustomName = "GpSlice",
	.CreateCustomScanState = gp_slice_create_state,
};

/*
 * Read all rows of the aggregate from the segments into the tuplestore.
 * The columns of the result are those of the target list of the node.
 */
static void
gp_slice_load(GpSliceState *state)
{
	ScanState  *ss = &state->css.ss;
	TupleDesc	tupdesc = ss->ss_ScanTupleSlot->tts_tupleDescriptor;
	int			natts = tupdesc->natts;
	FmgrInfo   *recv = palloc0_array(FmgrInfo, natts);
	Oid		   *ioparam = palloc_array(Oid, natts);
	TupleTableSlot *loadslot;
	MemoryContext rowcxt;
	GpQuery    *query;
	PGresult   *res;
	int			only = 0;

	for (int i = 0; i < natts; i++)
	{
		Oid			recvfn;

		getTypeBinaryInputInfo(TupleDescAttr(tupdesc, i)->atttypid,
							   &recvfn, &ioparam[i]);
		fmgr_info(recvfn, &recv[i]);
	}

	state->store = tuplestore_begin_heap(true, false, work_mem);
	loadslot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	rowcxt = AllocSetContextCreate(CurrentMemoryContext, "slice row",
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

		if (PQnfields(res) != natts)
			ereport(ERROR,
					errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("a segment returned %d columns, but the aggregate has %d",
						   PQnfields(res), natts));

		ExecClearTuple(loadslot);
		oldcontext = MemoryContextSwitchTo(rowcxt);
		for (int i = 0; i < natts; i++)
		{
			StringInfoData buf;

			loadslot->tts_isnull[i] = true;
			loadslot->tts_values[i] = (Datum) 0;
			if (PQgetisnull(res, 0, i))
				continue;

			/* libpq ends every value with a zero byte, as the receive functions expect */
			buf.data = PQgetvalue(res, 0, i);
			buf.len = PQgetlength(res, 0, i);
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
gp_slice_begin(CustomScanState *node, EState *estate, int eflags)
{
	GpSliceState *state = (GpSliceState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	state->sql = strVal(linitial(cscan->custom_private));
	state->replicated = boolVal(lsecond(cscan->custom_private));
}

static TupleTableSlot *
gp_slice_next(ScanState *ss)
{
	GpSliceState *state = (GpSliceState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (state->store == NULL)
		gp_slice_load(state);

	/* At the end the slot is empty, which is what ExecScan() looks for */
	(void) tuplestore_gettupleslot(state->store, true, false, slot);
	return slot;
}

static bool
gp_slice_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
gp_slice_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, gp_slice_next, gp_slice_recheck);
}

static void
gp_slice_end(CustomScanState *node)
{
	GpSliceState *state = (GpSliceState *) node;

	if (state->store != NULL)
		tuplestore_end(state->store);
	state->store = NULL;
}

/* The rows are all here, so a rescan reads them again from the store */
static void
gp_slice_rescan(CustomScanState *node)
{
	GpSliceState *state = (GpSliceState *) node;

	if (state->store != NULL)
		tuplestore_rescan(state->store);
	ExecScanReScan(&node->ss);
}

static void
gp_slice_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GpSliceState *state = (GpSliceState *) node;

	if (es->verbose)
		ExplainPropertyText("Remote SQL", state->sql, es);
}

static const CustomExecMethods gp_slice_exec_methods = {
	.CustomName = "GpSlice",
	.BeginCustomScan = gp_slice_begin,
	.ExecCustomScan = gp_slice_exec,
	.EndCustomScan = gp_slice_end,
	.ReScanCustomScan = gp_slice_rescan,
	.ExplainCustomScan = gp_slice_explain,
};

static Node *
gp_slice_create_state(CustomScan *cscan)
{
	GpSliceState *state = (GpSliceState *) newNode(sizeof(GpSliceState),
												   T_CustomScanState);

	state->css.flags = cscan->flags;
	state->css.methods = &gp_slice_exec_methods;
	/* A tuplestore hands out minimal tuples */
	state->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) state;
}

/*
 * Whether the query is an aggregate that the segments can compute whole:
 * one distributed table, nothing else.  For a table distributed by hash,
 * the grouping must include the whole key of the distribution, so that
 * every group is on one segment.  A copy of the query is returned for the
 * SQL of the segments, or NULL when the query is not such an aggregate.
 */
Query *
gp_slice_agg_query(Query *parse)
{
	RangeTblRef *ref;
	RangeTblEntry *rte;
	Index		rti;
	GpPolicy	policy;
	ListCell   *lc;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		!parse->hasAggs)
		return NULL;
	if (parse->hasWindowFuncs || parse->hasSubLinks ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupingSets != NIL || parse->rowMarks != NIL ||
		parse->distinctClause != NIL)
		return NULL;

	/* One table, and the FROM is that table alone */
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef))
		return NULL;
	ref = linitial_node(RangeTblRef, parse->jointree->fromlist);
	rti = ref->rtindex;
	rte = rt_fetch(rti, parse->rtable);
	if (rte->rtekind != RTE_RELATION || !gp_policy_get(rte->relid, &policy))
		return NULL;

	/* Junk columns of the target list would not come back from the segments */
	foreach(lc, parse->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (tle->resjunk)
			return NULL;
	}

	/* A replicated table is answered by one segment as a whole */
	if (policy.replicated)
		return copyObject(parse);

	/* A table by hash without a grouping cannot be answered by one segment */
	if (parse->groupClause == NIL)
		return NULL;

	return copyObject(parse);
}

/*
 * Replace the aggregate of the plan with a scan that runs it on the
 * segments, when the plan is the aggregate over the gather of the one
 * table.  The sort and the limit above the aggregate are left in place.
 */
void
gp_slice_agg_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	Agg		   *agg;
	Plan	   *below;
	Query	   *remote;
	CustomScan *cscan;
	List	   *tlist = NIL;
	ListCell   *lc;
	bool		replicated;

	/* Find the aggregate, below whatever sorts and limits sit on top */
	while (plan != NULL && !IsA(plan, Agg))
	{
		if (plan->righttree != NULL)
			return;
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL)
		return;
	agg = (Agg *) plan;

	/*
	 * A sorted aggregate answers the ORDER BY of the query by itself, with
	 * no sort on top: its replacement would not answer it.  A sort on top
	 * sorts the groups of the segments as it sorted the groups of the gather
	 */
	if (original->sortClause != NIL && !sort_above)
		return;

	/* Below the aggregate there is the gather of the table and nothing else */
	below = agg->plan.lefttree;
	while (below != NULL && IsA(below, Sort))
		below = below->lefttree;
	if (below == NULL || !gp_is_gather(below))
		return;

	replicated = false;
	{
		GpPolicy	policy = {0};
		RangeTblEntry *rte = NULL;

		foreach(lc, original->rtable)
		{
			RangeTblEntry *r = lfirst_node(RangeTblEntry, lc);

			if (r->rtekind == RTE_RELATION)
				rte = r;
		}
		if (rte != NULL && gp_policy_get(rte->relid, &policy))
			replicated = policy.replicated;

		/*
		 * Every column of the distribution key must be a grouping column of
		 * the aggregate, so that every group is on one segment.  The grouping
		 * columns are read from the plan, as columns of the table below it.
		 */
		if (!replicated)
		{
			for (int k = 0; k < policy.nkeys; k++)
			{
				bool		grouped = false;

				for (int i = 0; i < agg->numCols; i++)
				{
					TargetEntry *tle = list_nth(agg->plan.lefttree->targetlist,
												agg->grpColIdx[i] - 1);
					Var		   *var;

					if (!IsA(tle->expr, Var))
						return;
					var = (Var *) tle->expr;
					if (var->varattno == policy.keys[k])
					{
						grouped = true;
						break;
					}
				}
				if (!grouped)
					return;
			}
		}
	}

	/* The columns the segments return are the target list of the aggregate */
	if (list_length(agg->plan.targetlist) !=
		list_length(original->targetList))
		return;

	/* The query of the segments, without the sort and the limit of here */
	remote = copyObject(original);
	remote->sortClause = NIL;
	remote->limitCount = NULL;
	remote->limitOffset = NULL;

	cscan = makeNode(CustomScan);
	foreach(lc, agg->plan.targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		TargetEntry *out;
		Var		   *var;

		var = makeVar(INDEX_VAR, tle->resno,
					  exprType((Node *) tle->expr),
					  exprTypmod((Node *) tle->expr),
					  exprCollation((Node *) tle->expr), 0);
		out = makeTargetEntry((Expr *) var, tle->resno,
							  tle->resname ? pstrdup(tle->resname) : NULL,
							  false);
		tlist = lappend(tlist, out);
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->scan.plan.startup_cost = agg->plan.startup_cost;
	cscan->scan.plan.total_cost = agg->plan.total_cost;
	cscan->scan.plan.plan_rows = agg->plan.plan_rows;
	cscan->scan.plan.plan_width = agg->plan.plan_width;
	cscan->custom_scan_tlist = copyObject(original->targetList);
	cscan->custom_private = list_make2(makeString(pg_get_querydef(remote, false)),
									   makeBoolean(replicated));
	cscan->methods = &gp_slice_plan_methods;

	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
}

/*
 * Install the node on the coordinator; called from _PG_init()
 */
void
gp_slice_init(void)
{
	RegisterCustomScanMethods(&gp_slice_plan_methods);
}
