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
#include "utils/rel.h"

#include "gp.h"

static planner_hook_type prev_planner = NULL;

typedef struct GpInsertState
{
	CustomScanState css;
	Oid			relid;
	bool		can_set_tag;	/* the rows are counted in the command tag */
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
	int			nsource = 0;

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

	route = gp_route_begin(rel, &policy);
	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(child);
		MemoryContext oldcontext;

		if (TupIsNull(slot))
			break;
		slot_getallattrs(slot);

		ResetExprContext(econtext);
		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
		gp_route_row(route, slot->tts_values, slot->tts_isnull);
		MemoryContextSwitchTo(oldcontext);

		if (state->can_set_tag)
			estate->es_processed++;
	}
	gp_route_end(route);

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
	return (Node *) state;
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
	cscan->custom_private = list_make2(makeInteger((int) rte->relid),
									   makeBoolean(mt->canSetTag));
	cscan->methods = &gp_insert_plan_methods;

	stmt->planTree = &cscan->scan.plan;
}

static PlannedStmt *
gp_planner(Query *parse, const char *query_string, int cursorOptions,
		   ParamListInfo boundParams, ExplainState *es)
{
	PlannedStmt *stmt;

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
