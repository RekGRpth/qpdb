/* -------------------------------------------------------------------------
 *
 * insert.c
 *		INSERT into a distributed table
 *
 * The table on the coordinator holds no rows, so the plan of an INSERT into
 * it must not store them there.  The planner hook takes the plan that
 * the standard planner has made, and puts a custom scan, GpInsert, in
 * place of the ModifyTable on top: the source of the rows stays as it is
 * (VALUES, a SELECT, defaults and functions evaluated on the coordinator),
 * and the new node sends each row to the segment of its bucket as COPY
 * does, see route.c.  The segments check the constraints and compute the
 * generated columns.  The command tag counts the rows.
 *
 * If the rows are VALUES whose keys are all constants and fall in one
 * segment, the plan knows that segment, and the rows go to it alone, the
 * other segments not being asked at all (direct dispatch).  The buckets of
 * the keys go in the plan too, to check against the map at execution.
 *
 * Triggers and constraints of the table do not run on the coordinator;
 * RETURNING, ON CONFLICT, row-level security and WITH CHECK OPTION are
 * refused rather than lost.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/insert.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "gp.h"

static planner_hook_type prev_planner = NULL;

typedef struct GpInsertState
{
	CustomScanState css;
	Oid			relid;
	bool		can_set_tag;	/* the rows are counted in the command tag */
	int			direct_segment; /* all rows go to it, 0 if not known */
	List	   *direct_buckets; /* the buckets of the rows, for the check */
	bool		done;
} GpInsertState;

static Node *gp_insert_create_state(CustomScan *cscan);

static const CustomScanMethods gp_insert_plan_methods = {
	.CustomName = "GpInsert",
	.CreateCustomScanState = gp_insert_create_state,
};

static void
gp_insert_begin(CustomScanState *node, EState *estate, int eflags)
{
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	node->custom_ps = list_make1(ExecInitNode(linitial(cscan->custom_plans),
											  estate, eflags));
}

/*
 * Send all rows of the source to the segments, on the first call, and
 * return no row, as ModifyTable does without RETURNING
 */
static TupleTableSlot *
gp_insert_exec(CustomScanState *node)
{
	GpInsertState *state = (GpInsertState *) node;
	EState	   *estate = node->ss.ps.state;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	PlanState  *child = linitial(node->custom_ps);
	Relation	rel;
	GpPolicy	policy;
	GpRoute    *route;
	Tuplestorestate *buffer;
	TupleTableSlot *bslot;
	int			nsource = 0;
	int			only = 0;

	if (state->done)
		return NULL;
	state->done = true;

	rel = table_open(state->relid, RowExclusiveLock);
	if (!gp_policy_get(state->relid, &policy))
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("table \"%s\" is not distributed any more",
					   RelationGetRelationName(rel)),
				errhint("Plan the statement again."));

	/* The columns of the source are those of the table, in order */
	foreach_node(TargetEntry, tle, child->plan->targetlist)
	{
		if (!tle->resjunk)
			nsource++;
	}
	if (nsource != RelationGetDescr(rel)->natts)
		elog(ERROR, "the source of INSERT has %d columns, but table \"%s\" has %d",
			 nsource, RelationGetRelationName(rel),
			 RelationGetDescr(rel)->natts);

	/* The segment of the plan, unless the map is not what the plan saw */
	if (state->direct_segment != 0 && !policy.replicated)
	{
		only = state->direct_segment;
		foreach_int(bucket, state->direct_buckets)
		{
			if (bucket >= policy.nbuckets || policy.map[bucket] != only)
			{
				only = 0;
				break;
			}
		}
	}

	/*
	 * The rows of the source first: it may read the segments itself, on the
	 * connections that the rows are going to be sent on
	 */
	buffer = tuplestore_begin_heap(false, false, work_mem);
	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(child);

		if (TupIsNull(slot))
			break;
		tuplestore_puttupleslot(buffer, slot);
	}
	bslot = MakeSingleTupleTableSlot(ExecGetResultType(child), &TTSOpsMinimalTuple);

	route = gp_route_begin(rel, &policy, only);
	while (tuplestore_gettupleslot(buffer, true, false, bslot))
	{
		MemoryContext oldcontext;

		slot_getallattrs(bslot);

		ResetExprContext(econtext);
		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
		gp_route_row(route, bslot->tts_values, bslot->tts_isnull);
		MemoryContextSwitchTo(oldcontext);

		if (state->can_set_tag)
			estate->es_processed++;
	}
	gp_route_end(route);
	ExecDropSingleTupleTableSlot(bslot);
	tuplestore_end(buffer);

	table_close(rel, NoLock);
	return NULL;
}

static void
gp_insert_end(CustomScanState *node)
{
	ExecEndNode(linitial(node->custom_ps));
}

static void
gp_insert_rescan(CustomScanState *node)
{
	elog(ERROR, "GpInsert cannot be rescanned");
}

static const CustomExecMethods gp_insert_exec_methods = {
	.CustomName = "GpInsert",
	.BeginCustomScan = gp_insert_begin,
	.ExecCustomScan = gp_insert_exec,
	.EndCustomScan = gp_insert_end,
	.ReScanCustomScan = gp_insert_rescan,
};

static Node *
gp_insert_create_state(CustomScan *cscan)
{
	GpInsertState *state = (GpInsertState *) newNode(sizeof(GpInsertState),
													 T_CustomScanState);

	state->css.methods = &gp_insert_exec_methods;
	state->relid = (Oid) intVal(linitial(cscan->custom_private));
	state->can_set_tag = boolVal(lsecond(cscan->custom_private));
	state->direct_buckets = (List *) lthird(cscan->custom_private);
	state->direct_segment = intVal(lfourth(cscan->custom_private));
	return (Node *) state;
}

/*
 * The constant that a row of the source has for a column of the table, or
 * NULL if it is not one.  A single row is a Result, of the target list of
 * the table; the rows of a multi-row VALUES are lists of a ValuesScan, which
 * the target list refers to.
 */
static Const *
gp_key_const(Plan *source, AttrNumber attno, int row)
{
	TargetEntry *tle = get_tle_by_resno(source->targetlist, attno);

	if (tle == NULL)
		return NULL;
	if (IsA(tle->expr, Const))
		return (Const *) tle->expr;
	if (IsA(source, ValuesScan) && IsA(tle->expr, Var))
	{
		Var		   *var = (Var *) tle->expr;
		ValuesScan *scan = (ValuesScan *) source;

		if (var->varno == scan->scan.scanrelid && var->varlevelsup == 0 &&
			var->varattno > 0)
		{
			Node	   *expr = list_nth(list_nth_node(List, scan->values_lists, row),
										var->varattno - 1);

			if (IsA(expr, Const))
				return (Const *) expr;
		}
	}
	return NULL;
}

/*
 * The segment that all rows of the source go to, if their keys are all
 * constants and in the same segment, and else 0; the buckets of the rows
 * go to *buckets.
 */
static int
gp_insert_direct_segment(Oid relid, const GpPolicy *policy, Plan *source,
						 List **buckets)
{
	int			nrows;
	int			segment = 0;
	Oid		   *types;
	Datum	   *values;
	bool	   *nulls;

	if (policy->replicated || source->qual != NIL)
		return 0;
	if (IsA(source, Result))
	{
		if (outerPlan(source) != NULL || ((Result *) source)->resconstantqual != NULL)
			return 0;
		nrows = 1;
	}
	else if (IsA(source, ValuesScan))
		nrows = list_length(((ValuesScan *) source)->values_lists);
	else
		return 0;

	types = palloc_array(Oid, policy->nkeys);
	values = palloc_array(Datum, policy->nkeys);
	nulls = palloc_array(bool, policy->nkeys);
	for (int k = 0; k < policy->nkeys; k++)
		types[k] = get_atttype(relid, policy->keys[k]);

	for (int row = 0; row < nrows; row++)
	{
		int			bucket;

		for (int k = 0; k < policy->nkeys; k++)
		{
			Const	   *key = gp_key_const(source, policy->keys[k], row);

			if (key == NULL || key->consttype != types[k])
				return 0;
			values[k] = key->constvalue;
			nulls[k] = key->constisnull;
		}

		bucket = gp_key_bucket(policy->nbuckets, policy->nkeys, values, nulls,
							   types, InvalidOid);
		if (row > 0 && policy->map[bucket] != segment)
			return 0;
		segment = policy->map[bucket];
		*buckets = list_append_unique_int(*buckets, bucket);
	}

	return segment;
}

/*
 * Put GpInsert in place of the ModifyTable of an INSERT into a distributed
 * table
 */
static void
gp_insert_plan(Query *parse, PlannedStmt *stmt)
{
	ModifyTable *mt = (ModifyTable *) stmt->planTree;
	RangeTblEntry *rte = rt_fetch(parse->resultRelation, parse->rtable);
	GpPolicy	policy;
	Plan	   *source;
	CustomScan *cscan;
	List	   *buckets = NIL;
	int			segment;

	if (!gp_policy_get(rte->relid, &policy))
		return;

	if (mt->returningLists != NIL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("INSERT ... RETURNING is not supported on a distributed table"));
	if (mt->onConflictAction != ONCONFLICT_NONE)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("INSERT ... ON CONFLICT is not supported on a distributed table"));
	if (mt->withCheckOptionLists != NIL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("row-level security and WITH CHECK OPTION are not supported on a distributed table"));

	source = outerPlan(mt);
	segment = gp_insert_direct_segment(rte->relid, &policy, source, &buckets);
	cscan = makeNode(CustomScan);
	cscan->scan.plan.startup_cost = mt->plan.startup_cost;
	cscan->scan.plan.total_cost = mt->plan.total_cost;
	cscan->scan.plan.plan_rows = mt->plan.plan_rows;
	cscan->scan.plan.plan_width = mt->plan.plan_width;
	cscan->scan.plan.initPlan = mt->plan.initPlan;
	cscan->scan.plan.extParam = mt->plan.extParam;
	cscan->scan.plan.allParam = mt->plan.allParam;
	cscan->scan.scanrelid = 0;
	cscan->custom_plans = list_make1(source);
	cscan->custom_scan_tlist = source->targetlist;
	cscan->custom_private = list_make4(makeInteger((int) rte->relid),
									   makeBoolean(mt->canSetTag),
									   buckets,
									   makeInteger(segment));
	cscan->methods = &gp_insert_plan_methods;

	stmt->planTree = &cscan->scan.plan;
}

/*
 * Refuse what would change a distributed table, and is not sent to the
 * segments: UPDATE, DELETE and MERGE, which would find no rows on the
 * coordinator, and a data-modifying statement in WITH, whose rows would stay
 * there.  Only the INSERT of the statement itself is routed.
 */
static void
gp_check_modifications(Query *parse, bool toplevel)
{
	if (parse->resultRelation > 0 &&
		(parse->commandType == CMD_UPDATE ||
		 parse->commandType == CMD_DELETE ||
		 parse->commandType == CMD_MERGE ||
		 (parse->commandType == CMD_INSERT && !toplevel)))
	{
		RangeTblEntry *rte = rt_fetch(parse->resultRelation, parse->rtable);
		GpPolicy	policy;

		if (gp_policy_get(rte->relid, &policy))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("%s of a distributed table is not supported yet",
						   toplevel ? (parse->commandType == CMD_UPDATE ? "UPDATE" :
									   parse->commandType == CMD_DELETE ? "DELETE" : "MERGE") :
						   "a data-modifying statement in WITH"));
	}

	foreach_node(CommonTableExpr, cte, parse->cteList)
	{
		Query	   *query = castNode(Query, cte->ctequery);

		gp_check_modifications(query, false);
	}
}

static PlannedStmt *
gp_planner(Query *parse, const char *query_string, int cursorOptions,
		   ParamListInfo boundParams, ExplainState *es)
{
	PlannedStmt *stmt;

	if (IsUnderPostmaster)
		gp_check_modifications(parse, true);

	if (prev_planner)
		stmt = prev_planner(parse, query_string, cursorOptions, boundParams, es);
	else
		stmt = standard_planner(parse, query_string, cursorOptions, boundParams, es);

	if (parse->commandType == CMD_INSERT && IsUnderPostmaster &&
		stmt->planTree != NULL && IsA(stmt->planTree, ModifyTable))
		gp_insert_plan(parse, stmt);

	return stmt;
}

/*
 * Install the hook on the coordinator; called from _PG_init()
 */
void
gp_insert_init(void)
{
	RegisterCustomScanMethods(&gp_insert_plan_methods);
	prev_planner = planner_hook;
	planner_hook = gp_planner;
}
