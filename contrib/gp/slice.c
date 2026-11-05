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
#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/value.h"
#include "optimizer/optimizer.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
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

static CustomScan *gp_slice_make(Plan *src, Query *original, bool replicated,
								 List *sortClause);

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
bool
gp_slice_agg_plan(Query *original, PlannedStmt *stmt)
{
	/* Returns whether the plan was rewritten */
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	Agg		   *agg;
	Plan	   *below;
	CustomScan *cscan;
	ListCell   *lc;
	bool		replicated;

	/* Find the aggregate, below whatever sorts and limits sit on top */
	while (plan != NULL && !IsA(plan, Agg))
	{
		if (plan->righttree != NULL)
			return false;
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL)
		return false;
	agg = (Agg *) plan;

	/*
	 * A sorted aggregate answers the ORDER BY of the query by itself, with
	 * no sort on top: its replacement would not answer it.  A sort on top
	 * sorts the groups of the segments as it sorted the groups of the gather
	 */
	if (original->sortClause != NIL && !sort_above)
		return false;

	/* Below the aggregate there is the gather of the table and nothing else */
	below = agg->plan.lefttree;
	while (below != NULL && IsA(below, Sort))
		below = below->lefttree;
	if (below == NULL || !gp_is_gather(below))
		return false;

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
						return false;
					var = (Var *) tle->expr;
					if (var->varattno == policy.keys[k])
					{
						grouped = true;
						break;
					}
				}
				if (!grouped)
					return false;
			}
		}
	}

	/* The columns the segments return are the target list of the aggregate */
	if (list_length(agg->plan.targetlist) !=
		list_length(original->targetList))
		return false;

	cscan = gp_slice_make(&agg->plan, original, replicated, original->sortClause);

	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;

	return true;

}

/*
 * A scan that runs a query on the segments: its target list is that of the
 * plan it replaces, and its SQL is the query without its sort and limit
 */
static CustomScan *
gp_slice_make(Plan *src, Query *original, bool replicated, List *sortClause)
{
	Query	   *remote = copyObject(original);
	CustomScan *cscan = makeNode(CustomScan);
	List	   *srctlist = src->targetlist;
	List	   *origtlist = original->targetList;
	List	   *tlist = NIL;
	ListCell   *lc;
	int			pos = 0;

	/*
	 * A junk column is a grouping or a sort key that is not selected.  With
	 * no ORDER BY it is not needed by anything above, so it is dropped from
	 * both lists.  With an ORDER BY it is needed by the sort, so it goes to
	 * the segments as a column of its own.
	 */
	if (sortClause == NIL)
	{
		List	   *srcnonjunk = NIL;
		List	   *orignojunk = NIL;

		foreach(lc, srctlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			if (!tle->resjunk)
				srcnonjunk = lappend(srcnonjunk, tle);
		}
		foreach(lc, origtlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			if (!tle->resjunk)
				orignojunk = lappend(orignojunk, tle);
		}
		srctlist = srcnonjunk;
		origtlist = orignojunk;
	}
	if (sortClause != NIL)
	{
		foreach(lc, remote->targetList)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			tle->resjunk = false;
		}
	}
	remote->sortClause = NIL;
	remote->limitCount = NULL;
	remote->limitOffset = NULL;

	foreach(lc, srctlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		TargetEntry *out;
		Var		   *var;

		pos++;
		var = makeVar(INDEX_VAR, pos,
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
	cscan->scan.plan.startup_cost = src->startup_cost;
	cscan->scan.plan.total_cost = src->total_cost;
	cscan->scan.plan.plan_rows = src->plan_rows;
	cscan->scan.plan.plan_width = src->plan_width;
	cscan->custom_scan_tlist = copyObject(origtlist);
	cscan->custom_private = list_make2(makeString(pg_get_querydef(remote, false)),
									   makeBoolean(replicated));
	cscan->methods = &gp_slice_plan_methods;
	return cscan;
}

/*
 * Whether the condition equates the two given columns of the two tables,
 * at the top of an AND or below it
 */
static bool
gp_slice_join_pair(Node *qual, Index rti1, Index rti2,
				   AttrNumber k1, AttrNumber k2)
{
	OpExpr	   *op;
	Var		   *var1;
	Var		   *var2;

	if (qual == NULL)
		return false;
	if (IsA(qual, BoolExpr) && ((BoolExpr *) qual)->boolop == AND_EXPR)
	{
		ListCell   *lc;

		foreach(lc, ((BoolExpr *) qual)->args)
		{
			if (gp_slice_join_pair(lfirst(lc), rti1, rti2, k1, k2))
				return true;
		}
		return false;
	}
	if (!IsA(qual, OpExpr))
		return false;
	op = (OpExpr *) qual;
	if (list_length(op->args) != 2 ||
		get_opname(op->opno) == NULL ||
		strcmp(get_opname(op->opno), "=") != 0)
		return false;
	if (!IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Var))
		return false;
	var1 = (Var *) linitial(op->args);
	var2 = (Var *) lsecond(op->args);

	return (var1->varno == rti1 && var1->varattno == k1 &&
			var2->varno == rti2 && var2->varattno == k2) ||
		(var1->varno == rti2 && var1->varattno == k2 &&
		 var2->varno == rti1 && var2->varattno == k1);
}

/* Whether any of the join conditions equates the two columns */
static bool
gp_slice_join_pair_list(List *conds, Index rti1, Index rti2,
						AttrNumber k1, AttrNumber k2)
{
	ListCell   *lc;

	foreach(lc, conds)
	{
		if (gp_slice_join_pair(lfirst(lc), rti1, rti2, k1, k2))
			return true;
	}
	return false;
}

/* A table of a join, with its place in the distribution of the rows */
typedef struct GpJoinTable
{
	Index		rti;
	GpPolicy	policy;
} GpJoinTable;

/*
 * Collect the base tables of a join tree and all its join conditions;
 * false when a leaf is not a base table or a join type is not supported
 */
static bool
gp_slice_join_collect(Node *node, List **tables, List **conds)
{
	JoinExpr   *join;

	if (IsA(node, RangeTblRef))
	{
		GpJoinTable *table = palloc0(sizeof(GpJoinTable));

		table->rti = ((RangeTblRef *) node)->rtindex;
		*tables = lappend(*tables, table);
		return true;
	}
	if (!IsA(node, JoinExpr))
		return false;
	join = (JoinExpr *) node;
	if (join->jointype != JOIN_INNER && join->jointype != JOIN_LEFT &&
		join->jointype != JOIN_RIGHT && join->jointype != JOIN_FULL)
		return false;
	*conds = lappend(*conds, join->quals);
	return gp_slice_join_collect(join->larg, tables, conds) &&
		gp_slice_join_collect(join->rarg, tables, conds);
}

/*
 * Load the policies of the tables of a join and check that every row of
 * the join is on one segment: all hashed tables are of one colocation
 * group, and every one of them is joined to the others by the whole key,
 * while a replicated table is on every segment already
 */
static bool
gp_slice_join_colocated(Query *parse, List *tables, List *conds)
{
	ListCell   *lc;
	int			colocation_id = 0;
	int			nkeys = 0;
	bool		any_hash = false;
	List	   *connected = NIL;
	List	   *pending = NIL;

	/* Policies, and one colocation group for all hashed tables */
	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);
		RangeTblEntry *rte = rt_fetch(table->rti, parse->rtable);

		if (rte->rtekind != RTE_RELATION ||
			!gp_policy_get(rte->relid, &table->policy))
			return false;
		if (table->policy.replicated)
			continue;
		any_hash = true;
		if (colocation_id == 0)
		{
			colocation_id = table->policy.colocation_id;
			nkeys = table->policy.nkeys;
		}
		if (table->policy.colocation_id != colocation_id ||
			table->policy.nkeys != nkeys || colocation_id == 0)
			return false;
	}
	if (!any_hash)
		return true;

	/* Grow the connected set of hashed tables by key pairs of the joins */
	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (!table->policy.replicated)
			pending = lappend(pending, table);
	}
	if (pending != NIL)
	{
		GpJoinTable *first = linitial(pending);

		connected = list_make1(first);
		pending = list_delete(pending, first);
	}
	for (;;)
	{
		bool		grew = false;
		ListCell   *lp;

		foreach(lp, pending)
		{
			GpJoinTable *table = lfirst(lp);
			ListCell   *lu;

			foreach(lu, connected)
			{
				GpJoinTable *other = lfirst(lu);
				bool		all_keys = true;

				for (int i = 0; i < nkeys; i++)
				{
					if (!gp_slice_join_pair_list(conds, table->rti, other->rti,
												 table->policy.keys[i],
												 other->policy.keys[i]))
					{
						all_keys = false;
						break;
					}
				}
				if (all_keys)
				{
					connected = lappend(connected, table);
					pending = list_delete(pending, table);
					grew = true;
					break;
				}
			}
			if (grew)
				break;
		}
		if (!grew)
			return pending == NIL;
	}
}

/*
 * Resolve a column of a plan to the column of a base table it comes from,
 * through the OUTER_VAR and INNER_VAR references of the plans above
 */
static bool
gp_slice_base_column(Plan *plan, TargetEntry *tle, Index *rti,
					 AttrNumber *attno)
{
	Var		   *var;
	Plan	   *side;
	TargetEntry *stle;

	if (!IsA(tle->expr, Var))
		return false;
	var = (Var *) tle->expr;
	if (var->varno != OUTER_VAR && var->varno != INNER_VAR)
	{
		*rti = var->varno;
		*attno = var->varattno;
		return true;
	}

	side = var->varno == OUTER_VAR ? plan->lefttree : plan->righttree;
	if (side == NULL)
		return false;
	stle = list_nth(side->targetlist, var->varattno - 1);
	return gp_slice_base_column(side, stle, rti, attno);
}

/*
 * Whether the grouping of the aggregate covers the distribution key of
 * some hashed table of the join, so that every group is on one segment.
 * All hashed tables are colocated by their keys, so grouping by the whole
 * key of any one of them is enough.  The grouping columns are read from
 * the plan
 */
static bool
gp_slice_join_agg_keys(Agg *agg, List *tables)
{
	ListCell   *lc;

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);
		bool		all_keys = true;

		if (table->policy.replicated)
			continue;
		for (int i = 0; i < table->policy.nkeys; i++)
		{
			bool		grouped = false;

			for (int j = 0; j < agg->numCols; j++)
			{
				TargetEntry *tle = list_nth(agg->plan.lefttree->targetlist,
											agg->grpColIdx[j] - 1);
				Index		grti;
				AttrNumber	gattno;

				if (!gp_slice_base_column(agg->plan.lefttree, tle, &grti, &gattno))
					return false;
				if (grti == table->rti && gattno == table->policy.keys[i])
				{
					grouped = true;
					break;
				}
			}
			if (!grouped)
			{
				all_keys = false;
				break;
			}
		}
		if (all_keys)
			return true;
	}
	return false;
}

/*
 * Whether the query is a join the segments can compute: distributed tables
 * joined so that every row of the join is on one segment — hashed tables
 * of one colocation group connected by the whole key, with replicated
 * tables anywhere.  An aggregate over the join is allowed when its
 * grouping covers the key of a hashed table, which is checked on the plan.
 * A copy of the query is returned, or NULL when it is not such a join.
 */
Query *
gp_slice_join_query(Query *parse)
{
	List	   *tables = NIL;
	List	   *conds = NIL;
	ListCell   *lc;
	bool		any_hash = false;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT)
		return NULL;
	if (parse->hasWindowFuncs || parse->hasSubLinks ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupingSets != NIL || parse->rowMarks != NIL ||
		parse->distinctClause != NIL)
		return NULL;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1)
		return NULL;
	if (!gp_slice_join_collect(linitial(parse->jointree->fromlist),
							   &tables, &conds))
		return NULL;
	if (list_length(tables) < 2)
		return NULL;
	conds = lappend(conds, parse->jointree->quals);
	if (!gp_slice_join_colocated(parse, tables, conds))
		return NULL;

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (!table->policy.replicated)
			any_hash = true;
	}
	if (parse->hasAggs && any_hash && parse->groupClause == NIL)
		return NULL;

	return copyObject(parse);
}

/*
 * Replace the whole plan of a colocated join with a scan that runs it on
 * the segments, keeping the sort and the limit of the query here
 */
bool
gp_slice_join_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	bool		replicated = true;
	List	   *tables = NIL;
	List	   *conds = NIL;
	ListCell   *lc;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit)))
	{
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL)
		return false;

	/* A merge join answers the ORDER BY by itself, with no sort on top */
	if (original->sortClause != NIL && !sort_above)
		return false;

	if (!gp_slice_join_collect(linitial(original->jointree->fromlist),
							   &tables, &conds))
		return false;
	conds = lappend(conds, original->jointree->quals);
	if (!gp_slice_join_colocated(original, tables, conds))
		return false;
	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (!table->policy.replicated)
			replicated = false;
	}

	/* The columns the segments return are the target list of the plan */
	if (list_length(plan->targetlist) != list_length(original->targetList))
		return false;

	/* An aggregate over the join must group by the key of a hashed table */
	if (original->hasAggs && !replicated)
	{
		Plan	   *agg = plan;

		while (agg != NULL && !IsA(agg, Agg))
		{
			if (agg->righttree != NULL)
				return false;
			agg = agg->lefttree;
		}
		if (agg == NULL || !gp_slice_join_agg_keys((Agg *) agg, tables))
			return false;
	}

	cscan = gp_slice_make(plan, original, replicated, original->sortClause);

	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;

	return true;

}

/*
 * Whether a subquery is answered by one segment: a replicated table for a
 * single-table aggregate, or all replicated tables for a join
 */
static bool
gp_slice_subquery_replicated(Query *subquery, bool is_join)
{
	ListCell   *lc;

	if (!is_join)
	{
		foreach(lc, subquery->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
			GpPolicy	policy;

			if (rte->rtekind == RTE_RELATION &&
				gp_policy_get(rte->relid, &policy) && policy.replicated)
				return true;
		}
		return false;
	}
	else
	{
		List	   *tables = NIL;
		List	   *conds = NIL;

		if (!gp_slice_join_collect(linitial(subquery->jointree->fromlist),
								   &tables, &conds))
			return false;
		foreach(lc, tables)
		{
			GpJoinTable *table = lfirst(lc);
			RangeTblEntry *rte = rt_fetch(table->rti, subquery->rtable);
			GpPolicy	policy;

			if (!gp_policy_get(rte->relid, &policy) || !policy.replicated)
				return false;
		}
		return true;
	}
}

/*
 * Whether the query is a plain select over one subquery, with nothing of
 * its own but a projection, a sort and a limit: whatever the planner put
 * below the sort and the limit is the plan of the subquery alone — an
 * aggregate, a join, or a SubqueryScan the planner chose to keep
 */
static bool
gp_slice_subquery_outer(Query *parse, RangeTblEntry **rte_out)
{
	RangeTblRef *ref;
	RangeTblEntry *rte;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		parse->hasAggs || parse->hasWindowFuncs || parse->hasSubLinks ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->distinctClause != NIL || parse->rowMarks != NIL)
		return false;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef) ||
		parse->jointree->quals != NULL)
		return false;
	ref = linitial_node(RangeTblRef, parse->jointree->fromlist);
	rte = rt_fetch(ref->rtindex, parse->rtable);
	if (rte->rtekind != RTE_SUBQUERY)
		return false;

	*rte_out = rte;
	return true;
}

/*
 * A subquery in FROM has a locus of its own: if it is a colocated join or
 * a single-table aggregate by the key, it runs on the segments even when
 * the query above it is not colocated at all.  The subquery keeps no
 * limit of its own: a limit taken on every segment is not the limit of
 * the whole.  The planner often inlines the plan of a simple subquery
 * with no SubqueryScan of its own, so the plan below the sort and the
 * limit of the outer query is replaced, whatever node it is.  A plan that
 * answers the ORDER BY by its own order, with no sort on top, is left
 * alone: the replacement would not answer it
 */
static void
gp_slice_subquery_walk(Query *parse, Plan **planp, bool sort_above)
{
	Plan	   *plan = *planp;
	RangeTblEntry *rte;
	Query	   *subquery;
	Query	   *candidate;
	bool		is_agg;
	bool		replicated;
	CustomScan *cscan;

	if (plan == NULL)
		return;

	/* The sort, the limit and the projection of the outer query */
	if (IsA(plan, Sort) || IsA(plan, Limit) || IsA(plan, Result))
	{
		gp_slice_subquery_walk(parse, &plan->lefttree,
							   sort_above || IsA(plan, Sort));
		return;
	}

	if (parse->sortClause != NIL && !sort_above)
		return;
	if (!gp_slice_subquery_outer(parse, &rte))
		return;
	subquery = rte->subquery;
	if (subquery->limitCount != NULL || subquery->limitOffset != NULL)
		return;

	candidate = gp_slice_agg_query(subquery);
	is_agg = candidate != NULL;
	if (candidate == NULL)
		candidate = gp_slice_join_query(subquery);
	if (candidate == NULL)
		return;

	/* The columns the segments return are the outputs of the subquery */
	if (list_length(plan->targetlist) != list_length(subquery->targetList))
		return;

	replicated = gp_slice_subquery_replicated(subquery, !is_agg);
	cscan = gp_slice_make(plan, subquery, replicated, parse->sortClause);
	*planp = &cscan->scan.plan;
}

void
gp_slice_subquery_plans(Query *parse, PlannedStmt *stmt)
{
	gp_slice_subquery_walk(parse, &stmt->planTree, false);
}

/* An aggregate of the query, answered by the segments as a column */
typedef struct GpPartialAgg
{
	Aggref	   *aggref;			/* the aggregate of the query */
	Index		pos;			/* its column in the answer of the segments */
} GpPartialAgg;

/* A grouping column of the segments' answer */
typedef struct GpPartialKey
{
	Expr	   *expr;			/* the expression grouped by */
	Index		pos;
	Index		ressortgroupref;	/* its mark in the grouping */
} GpPartialKey;

/* What is collected from a query on the way to its partial form */
typedef struct GpPartialState
{
	List	   *aggs;			/* GpPartialAgg */
	List	   *keys;			/* GpPartialKey */
	Index		npos;			/* columns of the answer so far */
	int			nextaggno;		/* aggno for the next combining aggregate */
	bool		ok;				/* false once anything unsplittable is met */
} GpPartialState;

/*
 * Whether the segments can answer an aggregate with a column that the
 * coordinator finishes by the same aggregate again: min and max of any
 * type, and sum of a type it adds up in, whose argument, result and
 * transition are all of one type.  count counts the partial columns
 * instead of adding them, and the average divides, so neither can be
 * combined by itself
 */
static bool
gp_partial_splittable(Aggref *aggref)
{
	HeapTuple	agg_tup;
	HeapTuple	proc_tup;
	Form_pg_aggregate aggform;
	Form_pg_proc procform;
	const char *name;
	bool		ok;

	if (aggref->aggkind != AGGKIND_NORMAL || aggref->aggstar ||
		aggref->aggvariadic || aggref->aggdirectargs != NIL ||
		aggref->aggdistinct != NIL || aggref->aggorder != NIL ||
		aggref->aggfilter != NULL ||
		list_length(aggref->args) != 1 ||
		list_length(aggref->aggargtypes) != 1 ||
		linitial_oid(aggref->aggargtypes) != aggref->aggtype)
		return false;

	agg_tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggref->aggfnoid));
	if (!HeapTupleIsValid(agg_tup))
		return false;
	aggform = (Form_pg_aggregate) GETSTRUCT(agg_tup);
	ok = OidIsValid(aggform->aggcombinefn);
	ReleaseSysCache(agg_tup);
	if (!ok)
		return false;

	proc_tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(aggref->aggfnoid));
	if (!HeapTupleIsValid(proc_tup))
		return false;
	procform = (Form_pg_proc) GETSTRUCT(proc_tup);
	name = NameStr(procform->proname);
	ok = (strcmp(name, "min") == 0 || strcmp(name, "max") == 0 ||
		  strcmp(name, "sum") == 0);
	ReleaseSysCache(proc_tup);
	return ok;
}

/* Collect the aggregates of an expression tree, in the order they appear */
static bool
gp_partial_walker(Node *node, GpPartialState *state)
{
	ListCell   *lc;

	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		GpPartialAgg *partial;

		if (!gp_partial_splittable(aggref))
		{
			state->ok = false;
			return true;
		}
		foreach(lc, state->aggs)
		{
			partial = lfirst(lc);
			if (equal(partial->aggref, aggref))
				return false;
		}
		partial = palloc0(sizeof(GpPartialAgg));
		partial->aggref = aggref;
		state->npos++;
		partial->pos = state->npos;
		state->aggs = lappend(state->aggs, partial);
		return false;
	}
	return expression_tree_walker(node, gp_partial_walker, state);
}

/* Replace a group key with its column, an aggregate with its combining one */
static Node *
gp_partial_mutator(Node *node, GpPartialState *state)
{
	ListCell   *lc;

	if (node == NULL)
		return NULL;
	foreach(lc, state->keys)
	{
		GpPartialKey *key = lfirst(lc);

		if (equal(key->expr, node))
			return (Node *) makeVar(OUTER_VAR, key->pos,
									exprType(node), exprTypmod(node),
									exprCollation(node), 0);
	}
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		Aggref	   *combine;
		Var		   *var;

		foreach(lc, state->aggs)
		{
			GpPartialAgg *partial = lfirst(lc);

			if (!equal(partial->aggref, aggref))
				continue;
			combine = copyObject(aggref);
			var = makeVar(OUTER_VAR, partial->pos, aggref->aggtype,
						  -1, aggref->aggcollid, 0);
			combine->args = list_make1(makeTargetEntry((Expr *) var, 1,
													   NULL, false));
			combine->aggargtypes = list_make1_oid(aggref->aggtype);
			combine->aggsplit = AGGSPLIT_SIMPLE;
			combine->aggno = state->nextaggno;
			combine->aggtransno = state->nextaggno;
			state->nextaggno++;
			return (Node *) combine;
		}
		state->ok = false;
		return node;
	}
	return expression_tree_mutator(node, gp_partial_mutator, state);
}

/*
 * Answer an aggregate query in two steps, when the whole of it cannot run
 * on the segments: the segments aggregate their own rows — grouped by the
 * grouping of the query, or all rows together when there is none — and the
 * coordinator finishes the aggregate over the columns of every segment.
 * The final aggregate is the same aggregate again, which only min, max and
 * sum of a closing type answer truly; the grouping columns go to the
 * segments as columns of their own, and HAVING stays on the coordinator,
 * for a group may pass it only as a whole.
 *
 * The plan is the aggregate with its gather replaced by the slice: the
 * target list and the qual of the aggregate are rebuilt over the columns
 * of the answer, and the grouping columns are renumbered to them.
 */
void
gp_slice_partial_plan(Query *parse, PlannedStmt *stmt)
{
	Plan	   *plan = stmt->planTree;
	RangeTblEntry *rte;
	GpPolicy	policy;
	GpPartialState state;
	RangeTblRef *ref;
	ListCell   *lc;
	Agg		   *agg;
	Query	   *remote;
	CustomScan *cscan;
	List	   *tlist = NIL;
	AttrNumber *grpColIdx;
	int			nkeys;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		!parse->hasAggs)
		return;
	if (parse->hasWindowFuncs || parse->hasSubLinks ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupingSets != NIL || parse->rowMarks != NIL ||
		parse->distinctClause != NIL)
		return;

	/* One hashed table, whose rows are spread over the segments */
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef))
		return;
	ref = linitial_node(RangeTblRef, parse->jointree->fromlist);
	rte = rt_fetch(ref->rtindex, parse->rtable);
	if (rte->rtekind != RTE_RELATION ||
		!gp_policy_get(rte->relid, &policy) || policy.replicated)
		return;

	/* The grouping columns lead the answer of the segments */
	state.aggs = NIL;
	state.keys = NIL;
	state.npos = 0;
	state.nextaggno = 0;
	state.ok = true;
	foreach(lc, parse->groupClause)
	{
		SortGroupClause *sc = lfirst_node(SortGroupClause, lc);
		TargetEntry *tle = get_sortgroupclause_tle(sc, parse->targetList);
		GpPartialKey *key = palloc0(sizeof(GpPartialKey));

		key->expr = tle->expr;
		state.npos++;
		key->pos = state.npos;
		key->ressortgroupref = tle->ressortgroupref;
		state.keys = lappend(state.keys, key);
	}

	/* The aggregates must be answerable by the segments */
	gp_partial_walker((Node *) parse->targetList, &state);
	if (state.ok)
		gp_partial_walker((Node *) parse->havingQual, &state);
	if (!state.ok || state.aggs == NIL)
	return;

	/* Find the aggregate of the plan, below the sort and the limit of it */
	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit) ||
							IsA(plan, Result)))
		plan = plan->lefttree;
	if (plan == NULL || !IsA(plan, Agg))
		return;
	agg = (Agg *) plan;
	if (!gp_is_gather(agg->plan.lefttree))
		return;
	if (agg->numCols != list_length(parse->groupClause))
		return;

	/*
	 * The query of the segments: the grouping columns and the aggregates
	 * as its target list, with HAVING left out, for it sees whole groups
	 * only on the coordinator
	 */
	remote = copyObject(parse);
	remote->targetList = NIL;
	foreach(lc, state.keys)
	{
		GpPartialKey *key = lfirst(lc);
		TargetEntry *tle = makeTargetEntry(copyObject(key->expr),
										   key->pos, NULL, false);

		tle->ressortgroupref = key->ressortgroupref;
		remote->targetList = lappend(remote->targetList, tle);
	}
	foreach(lc, state.aggs)
	{
		GpPartialAgg *partial = lfirst(lc);
		TargetEntry *tle = makeTargetEntry((Expr *) copyObject(partial->aggref),
										   partial->pos, NULL, false);

		remote->targetList = lappend(remote->targetList, tle);
	}
	remote->havingQual = NULL;
	remote->sortClause = NIL;
	remote->limitCount = NULL;
	remote->limitOffset = NULL;

	/* The slice that reads the answer of the segments */
	cscan = makeNode(CustomScan);
	{
		int			pos = 0;

		foreach(lc, remote->targetList)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);
			TargetEntry *out;
			Var		   *var;

			pos++;
			var = makeVar(INDEX_VAR, pos, exprType((Node *) tle->expr),
						  exprTypmod((Node *) tle->expr),
						  exprCollation((Node *) tle->expr), 0);
			out = makeTargetEntry((Expr *) var, tle->resno, NULL, false);
			tlist = lappend(tlist, out);
		}
	}
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->scan.plan.startup_cost = agg->plan.lefttree->startup_cost;
	cscan->scan.plan.total_cost = agg->plan.lefttree->total_cost;
	cscan->scan.plan.plan_rows = agg->plan.lefttree->plan_rows;
	cscan->scan.plan.plan_width = agg->plan.lefttree->plan_width;
	cscan->custom_scan_tlist = copyObject(remote->targetList);
	cscan->custom_private = list_make2(makeString(pg_get_querydef(remote, false)),
									   makeBoolean(false));
	cscan->methods = &gp_slice_plan_methods;

	/* The final aggregate over the columns of the answer */
	state.ok = true;
	tlist = NIL;
	foreach(lc, parse->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Node	   *expr = gp_partial_mutator((Node *) tle->expr, &state);

		if (!state.ok)
			return;
		tlist = lappend(tlist, makeTargetEntry((Expr *) expr, tle->resno,
											   tle->resname ?
											   pstrdup(tle->resname) : NULL,
											   tle->resjunk));
	}
	agg->plan.qual = (List *) gp_partial_mutator((Node *) parse->havingQual,
												 &state);
	if (!state.ok)
		return;

	nkeys = list_length(state.keys);
	grpColIdx = palloc_array(AttrNumber, nkeys);
	{
		int			i = 0;

		foreach(lc, state.keys)
		{
			GpPartialKey *key = lfirst(lc);

			grpColIdx[i++] = key->pos;
		}
	}
	agg->grpColIdx = grpColIdx;
	agg->aggsplit = AGGSPLIT_SIMPLE;
	agg->plan.targetlist = tlist;
	agg->plan.lefttree = &cscan->scan.plan;
}

/*
 * Install the node on the coordinator; called from _PG_init()
 */
void
gp_slice_init(void)
{
	RegisterCustomScanMethods(&gp_slice_plan_methods);
}
