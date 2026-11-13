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
 * When the source is one hashed table and every target column comes directly
 * from it, GpDml runs the whole INSERT ... SELECT on the segments instead:
 * the source rows move by the distribution key of the target, so every
 * segment inserts only its own rows.  Expressions and defaults stay on the
 * coordinator.
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
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "optimizer/cost.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "access/sysattr.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/tuplestore.h"

#include "gp.h"

static planner_hook_type prev_planner = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze = NULL;
static const CustomScanMethods gp_dml_plan_methods;

static void gp_dml_lock(Oid relid);

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
gp_insert_plan(Query *parse, Query *original, PlannedStmt *stmt)
{
	ModifyTable *mt = (ModifyTable *) stmt->planTree;
	RangeTblEntry *rte = rt_fetch(parse->resultRelation, parse->rtable);
	GpPolicy	policy;
	Plan	   *source;
	CustomScan *cscan;
	List	   *buckets = NIL;
	int			segment;
	bool		motion;

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
	if (gp_slice_insert_motion(original, &motion))
	{
		cscan = makeNode(CustomScan);
		cscan->scan.plan.startup_cost = mt->plan.startup_cost;
		cscan->scan.plan.total_cost = mt->plan.total_cost;
		cscan->scan.plan.plan_rows = mt->plan.plan_rows;
		cscan->scan.plan.plan_width = mt->plan.plan_width;
		cscan->scan.scanrelid = 0;
		cscan->custom_private = list_make3(makeString(pg_get_querydef(original, false)),
										   makeBoolean(mt->canSetTag),
										   makeBoolean(motion));
		cscan->methods = &gp_dml_plan_methods;
		stmt->planTree = &cscan->scan.plan;
		return;
	}
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
 * The rows of UPDATE and DELETE are on the segments, and the statement runs
 * there, over its own rows, in the transaction of the coordinator.  The
 * plan of the coordinator goes away; a custom scan sends the statement to
 * the segments on the first call, and takes their counts of rows for the
 * answer of the statement.  The coordinator holds an exclusive lock on the
 * table for the whole statement, which orders the writers between the
 * sessions and spares the segments a deadlock of their own.
 */
typedef struct GpDmlState
{
	CustomScanState css;
	char	   *sql;
	bool		motion;
	bool		done;
} GpDmlState;

static Node *gp_dml_create_state(CustomScan *cscan);
static const CustomExecMethods gp_dml_exec_methods;

static const CustomScanMethods gp_dml_plan_methods = {
	.CustomName = "GpDml",
	.CreateCustomScanState = gp_dml_create_state,
};

static Node *
gp_dml_create_state(CustomScan *cscan)
{
	GpDmlState *state = (GpDmlState *) newNode(sizeof(GpDmlState),
											   T_CustomScanState);

	state->css.flags = cscan->flags;
	state->css.methods = &gp_dml_exec_methods;
	return (Node *) state;
}

static void
gp_dml_begin(CustomScanState *node, EState *estate, int eflags)
{
	GpDmlState *state = (GpDmlState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	state->sql = strVal(linitial(cscan->custom_private));
	state->motion = boolVal(lthird(cscan->custom_private));
}

static TupleTableSlot *
gp_dml_exec(CustomScanState *node)
{
	GpDmlState *state = (GpDmlState *) node;

	if (!state->done)
	{
		uint64		rows = 0;

		if (state->motion && gp_dispatch_in_xact())
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("a motion query cannot see writes made earlier in the same transaction"),
					errhint("Run the query before writing, or prepare it after the write."));
		state->done = true;
		gp_dispatch_xact_rows(state->sql, &rows);
		node->ss.ps.state->es_processed = rows;
	}
	return NULL;
}

static void
gp_dml_end(CustomScanState *node)
{
}

static void
gp_dml_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GpDmlState *state = (GpDmlState *) node;

	ExplainPropertyText("Remote SQL", state->sql, es);
	ExplainPropertyBool("Motion", state->motion, es);
}

static const CustomExecMethods gp_dml_exec_methods = {
	.CustomName = "GpDml",
	.BeginCustomScan = gp_dml_begin,
	.ExecCustomScan = gp_dml_exec,
	.EndCustomScan = gp_dml_end,
	.ExplainCustomScan = gp_dml_explain,
};

/*
 * Turn the plan of an UPDATE or a DELETE of a distributed table into the
 * custom scan that runs the statement on the segments.  The statement may
 * use its own table only: the segments see their own rows of it, and no
 * other rows at all.
 */
static void
gp_dml_plan(Query *parse, PlannedStmt *stmt, ParamListInfo boundParams,
			const char *queryString)
{
	ModifyTable *mt = (ModifyTable *) stmt->planTree;
	RangeTblEntry *rte = rt_fetch(parse->resultRelation, parse->rtable);
	const char *verb = parse->commandType == CMD_UPDATE ? "UPDATE" : "DELETE";
	GpPolicy	policy;

	if (!gp_policy_get(rte->relid, &policy))
		return;

	if (mt->returningLists != NIL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("RETURNING is not supported on a distributed table"));
	if (parse->hasRowSecurity)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("row-level security is not supported on a distributed table"));
	if (boundParams != NULL && boundParams->numParams > 0)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("parameters are not supported in %s of a distributed table yet",
					   verb));
	if (parse->hasSubLinks)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("subqueries are not supported in %s of a distributed table yet",
					   verb),
				errdetail("A segment sees its own rows of a distributed table, and no rows of the others."));
	{
		ListCell   *lc;

		foreach(lc, parse->rtable)
	{
		RangeTblEntry *r = lfirst_node(RangeTblEntry, lc);

		if (r->rtekind == RTE_RELATION && r->relid == rte->relid)
			continue;
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("%s of a distributed table can only use its own table",
					   verb),
				errdetail("A segment sees its own rows of it, and no rows of the others."));
		}
	}
	if (parse->commandType == CMD_UPDATE)
	{
		ListCell   *lc;

		/*
		 * The columns the statement sets are the updatedCols of its
		 * permission info: the target list of the query is not to be
		 * trusted, for the planner rebuilds it.  A key that anything
		 * else sets is a change of the distribution.
		 */
		foreach(lc, parse->rteperminfos)
		{
			RTEPermissionInfo *perminfo = lfirst(lc);
			Bitmapset  *keys = NULL;
			int			i;

			if (perminfo->relid != rte->relid)
				continue;
			for (i = 0; i < policy.nkeys; i++)
				keys = bms_add_member(keys,
									  policy.keys[i] -
									  FirstLowInvalidHeapAttributeNumber);
			if (bms_overlap(perminfo->updatedCols, keys))
			{
				bms_free(keys);
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("changing the distribution key of a distributed table is not supported yet"),
						errdetail("Rows with the new key would belong to another segment."));
				return;
			}
			bms_free(keys);
		}
	}

	/* The writers of the table run one at a time */
	gp_dml_lock(rte->relid);

	{
		/*
		 * The segments run the statement as it was written: the target
		 * list of the query is not to be trusted here, for the planner
		 * rebuilds it and renumbers the columns it leaves out.
		 */
		CustomScan *cscan = makeNode(CustomScan);
		int			location = stmt->stmt_location;
		int			len = stmt->stmt_len > 0 ? stmt->stmt_len :
			strlen(queryString);
		char	   *sql = pnstrdup(queryString + location, len);

		cscan->scan.plan.startup_cost = mt->plan.startup_cost;
		cscan->scan.plan.total_cost = mt->plan.total_cost;
		cscan->scan.plan.plan_rows = mt->plan.plan_rows;
		cscan->scan.plan.plan_width = mt->plan.plan_width;
		cscan->scan.scanrelid = 0;
		cscan->custom_private = list_make3(makeString(sql),
										   makeBoolean(stmt->canSetTag),
										   makeBoolean(false));
		cscan->methods = &gp_dml_plan_methods;
		stmt->planTree = &cscan->scan.plan;
	}
}

/*
 * Refuse what would change a distributed table, and is not sent to the
 * segments: MERGE, which would find no rows on the coordinator, and a
 * data-modifying statement in WITH, whose rows would stay there.  Only the
 * INSERT of the statement itself is routed, and UPDATE and DELETE go to the
 * segments whole.
 */
static void
gp_check_modifications(Query *parse, bool toplevel)
{
	if (parse->resultRelation > 0 &&
		(parse->commandType == CMD_MERGE || !toplevel))
	{
		RangeTblEntry *rte = rt_fetch(parse->resultRelation, parse->rtable);
		GpPolicy	policy;

		if (gp_policy_get(rte->relid, &policy))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("%s of a distributed table is not supported yet",
						   toplevel ? "MERGE" :
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
	Query	   *agg_original = NULL;
	Query	   *join_original = NULL;
	Query	   *union_original = NULL;
	Query	   *window_original = NULL;
	Query	   *cte_original = NULL;
	Query	   *distinct_original = NULL;
	Query	   *insert_original = NULL;
	bool		replaced = false;
	int			saved_parallel_workers = max_parallel_workers_per_gather;

	if (IsUnderPostmaster)
		gp_check_modifications(parse, true);

	agg_original = gp_slice_agg_query(parse);
	if (agg_original == NULL)
		join_original = gp_slice_join_query(parse);
	if (agg_original == NULL && join_original == NULL)
		union_original = gp_slice_union_query(parse);
	if (union_original == NULL)
		window_original = gp_slice_window_query(parse);
	if (window_original == NULL)
		cte_original = gp_slice_cte_query(parse);
	if (cte_original == NULL)
		distinct_original = gp_slice_distinct_query(parse);
	if (parse->commandType == CMD_INSERT)
		insert_original = copyObject(parse);

	/* A GpSlice is distributed itself, not a branch of a parallel Gather */
	if (union_original != NULL || window_original != NULL ||
		cte_original != NULL || distinct_original != NULL)
		max_parallel_workers_per_gather = 0;
	PG_TRY();
	{
		if (prev_planner)
			stmt = prev_planner(parse, query_string, cursorOptions, boundParams, es);
		else
			stmt = standard_planner(parse, query_string, cursorOptions, boundParams, es);
	}
	PG_FINALLY();
	{
		max_parallel_workers_per_gather = saved_parallel_workers;
	}
	PG_END_TRY();

	if (parse->commandType == CMD_INSERT && IsUnderPostmaster &&
		stmt->planTree != NULL && IsA(stmt->planTree, ModifyTable))
		gp_insert_plan(parse, insert_original, stmt);
	if ((parse->commandType == CMD_UPDATE || parse->commandType == CMD_DELETE) &&
		IsUnderPostmaster && stmt->planTree != NULL &&
		IsA(stmt->planTree, ModifyTable))
		gp_dml_plan(parse, stmt, boundParams, query_string);

	if (agg_original != NULL)
		replaced = gp_slice_agg_plan(agg_original, stmt);
	else if (join_original != NULL)
		replaced = gp_slice_join_plan(join_original, stmt);

	if (!replaced && union_original != NULL)
		replaced = gp_slice_union_plan(union_original, stmt);
	if (!replaced && window_original != NULL)
		replaced = gp_slice_window_plan(window_original, stmt);
	if (!replaced && cte_original != NULL)
		replaced = gp_slice_cte_plan(cte_original, stmt);
	if (!replaced && distinct_original != NULL)
		replaced = gp_slice_distinct_plan(distinct_original, stmt);

	if (!replaced)
	{
		/*
		 * A subquery in FROM may have been flattened into the query by the
		 * planner, which rewrites the query in place: what was a select
		 * over a subquery is an aggregate or a colocated join now, and the
		 * detection above, run before the planner, saw none of it
		 */
		Query	   *flattened = gp_slice_agg_query(parse);

		if (flattened != NULL)
			replaced = gp_slice_agg_plan(flattened, stmt);
		if (!replaced)
		{
			flattened = gp_slice_join_query(parse);
			if (flattened != NULL)
				replaced = gp_slice_join_plan(flattened, stmt);
		}
		if (!replaced)
		{
			/*
			 * An aggregate whose grouping does not cover the key of the
			 * distribution is answered in two steps: partly on the
			 * segments, finished here
			 */
			gp_slice_partial_plan(parse, stmt);

			/* The subqueries of FROM have loci of their own */
			gp_slice_subquery_plans(parse, stmt);
		}
	}

	return stmt;
}

/*
 * The writers of a distributed table run one at a time.  Their lock is
 * on the policy of the table, not on the table itself: the parser holds
 * the row exclusive lock of the target already when any hook gets to
 * run, and upgrading it to an exclusive lock would deadlock two writers
 * upgrading at the same time.  A lock on the policy upgrades nothing.
 */
static void
gp_dml_lock(Oid relid)
{
	Oid			policy_relid;

	policy_relid = get_relname_relid("distribution_policy",
									 get_namespace_oid("gp", false));
	LockDatabaseObject(policy_relid, relid, 0, ExclusiveLock);
}

/*
 * The lock of a distributed UPDATE or DELETE, taken as early as the
 * hooks allow, so that a second writer waits before it plans
 */
static void
gp_post_parse_analyze(ParseState *pstate, Query *query,
					  const JumbleState *jstate)
{
	if (prev_post_parse_analyze)
		prev_post_parse_analyze(pstate, query, jstate);

	if ((query->commandType == CMD_UPDATE || query->commandType == CMD_DELETE) &&
		query->resultRelation > 0 && IsUnderPostmaster)
	{
		RangeTblEntry *rte = rt_fetch(query->resultRelation, query->rtable);
		GpPolicy	policy;

		if (rte->rtekind == RTE_RELATION && gp_policy_get(rte->relid, &policy))
			gp_dml_lock(rte->relid);
	}
}

/*
 * Install the hooks on the coordinator; called from _PG_init()
 */
void
gp_insert_init(void)
{
	RegisterCustomScanMethods(&gp_insert_plan_methods);
	RegisterCustomScanMethods(&gp_dml_plan_methods);
	prev_planner = planner_hook;
	planner_hook = gp_planner;
	prev_post_parse_analyze = post_parse_analyze_hook;
	post_parse_analyze_hook = gp_post_parse_analyze;
}
