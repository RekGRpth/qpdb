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
#include "commands/defrem.h"
#include "access/hash.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_proc.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/value.h"
#include "optimizer/optimizer.h"
#include "optimizer/planmain.h"
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
	bool		motion;
	bool		autonomous;
	Tuplestorestate *store;		/* all rows, once read */
} GpSliceState;

static Node *gp_slice_create_state(CustomScan *cscan);

static const CustomScanMethods gp_slice_plan_methods = {
	.CustomName = "GpSlice",
	.CreateCustomScanState = gp_slice_create_state,
};

/* Where the rows of a plan subtree are computed, and by which columns they hash */
typedef enum GpLocusKind
{
	GP_LOCUS_COORDINATOR,		/* no distributed locus */
	GP_LOCUS_SEGMENTS,			/* a part of the answer on every segment */
	GP_LOCUS_ONE_SEGMENT,		/* the whole answer on one segment */
} GpLocusKind;

typedef struct GpLocus
{
	GpLocusKind kind;
	bool		replicated;		/* a copy is available on every segment */
	int			colocation_id;
	int			nkeys;
	AttrNumber *keys;			/* positions in the output of the subtree */
	Oid		   *keytypes;
	Oid		   *keycollations;
} GpLocus;

static GpLocus
gp_locus_make(GpLocusKind kind)
{
	GpLocus	locus = { .kind = kind };

	return locus;
}

static GpLocus
gp_locus_policy(const GpPolicy *policy)
{
	GpLocus	locus;

	if (policy->replicated)
	{
		locus = gp_locus_make(GP_LOCUS_ONE_SEGMENT);
		locus.replicated = true;
		return locus;
	}
	locus = gp_locus_make(GP_LOCUS_SEGMENTS);
	locus.colocation_id = policy->colocation_id;
	locus.nkeys = policy->nkeys;
	locus.keys = palloc_array(AttrNumber, policy->nkeys);
	memcpy(locus.keys, policy->keys, policy->nkeys * sizeof(AttrNumber));
	return locus;
}

/* Unknown hashing, but the rows are still split over the segments */
static GpLocus
gp_locus_unhash(GpLocus locus)
{
	locus.colocation_id = 0;
	locus.nkeys = 0;
	locus.keys = NULL;
	locus.keytypes = NULL;
	locus.keycollations = NULL;
	return locus;
}

/*
 * Map all the hash keys through a projection.  Losing one key of a compound
 * key, or computing an expression over it, makes the hashing unknown: hashing
 * by the remaining columns would not prove colocation.
 */
static GpLocus
gp_locus_project(Plan *plan, GpLocus child)
{
	AttrNumber *keys;
	Oid		   *types;
	Oid		   *collations;

	if (child.kind != GP_LOCUS_SEGMENTS || child.nkeys == 0)
		return child;
	keys = palloc_array(AttrNumber, child.nkeys);
	types = palloc_array(Oid, child.nkeys);
	collations = palloc_array(Oid, child.nkeys);
	for (int k = 0; k < child.nkeys; k++)
	{
		bool		found = false;
		ListCell   *lc;

		foreach(lc, plan->targetlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);
			Var		   *var;

			if (!IsA(tle->expr, Var))
				continue;
			var = (Var *) tle->expr;
			if (var->varattno == child.keys[k])
			{
				keys[k] = tle->resno;
				types[k] = var->vartype;
				collations[k] = var->varcollid;
				found = true;
				break;
			}
		}
		if (!found)
		{
			pfree(keys);
			pfree(types);
			pfree(collations);
			return gp_locus_unhash(child);
		}
	}
	child.keys = keys;
	child.keytypes = types;
	child.keycollations = collations;
	return child;
}

/* Sort and limit keep the output; Result and SubqueryScan may project it */
static GpLocus
gp_locus_unary(Plan *plan, GpLocus child)
{
	if (IsA(plan, Sort) || IsA(plan, Limit) || IsA(plan, IncrementalSort))
		return child;
	if (IsA(plan, Result) || IsA(plan, SubqueryScan))
		return gp_locus_project(plan, child);
	return gp_locus_make(GP_LOCUS_COORDINATOR);
}

/* Whether every hash key is present in a grouping/partition column list */
static bool
gp_locus_columns(GpLocus locus, Plan *below, int numCols,
				 AttrNumber *colIdx)
{
	if (locus.kind != GP_LOCUS_SEGMENTS || locus.nkeys == 0)
		return false;
	while (below != NULL && (IsA(below, Sort) || IsA(below, IncrementalSort)))
		below = below->lefttree;
	for (int k = 0; k < locus.nkeys; k++)
	{
		bool		grouped = false;

		for (int i = 0; i < numCols; i++)
		{
			TargetEntry *tle = list_nth(below->targetlist, colIdx[i] - 1);
			Var		   *var;

			if (!IsA(tle->expr, Var))
				return false;
			var = (Var *) tle->expr;
			if (var->varattno == locus.keys[k])
			{
				grouped = true;
				break;
			}
		}
		if (!grouped)
			return false;
	}
	return true;
}

/* Whether every hash key is a grouping column of the aggregate */
static bool
gp_locus_grouped(GpLocus locus, Agg *agg, Plan *below)
{
	return gp_locus_columns(locus, below, agg->numCols, agg->grpColIdx);
}

static GpLocus gp_locus_plan(Plan *plan, List *rtable, List *subplans);

static bool
gp_locus_join_var(Node *node, Index varno, AttrNumber attno)
{
	Var		   *var;

	if (!IsA(node, Var))
		return false;
	var = (Var *) node;
	return var->varno == varno && var->varattno == attno;
}

/* Whether a join clause equates this pair of distribution columns */
static bool
gp_locus_join_pair(Node *qual, AttrNumber outer, AttrNumber inner)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;

	if (!IsA(qual, OpExpr))
		return false;
	op = (OpExpr *) qual;
	if (list_length(op->args) != 2 || get_opname(op->opno) == NULL ||
		strcmp(get_opname(op->opno), "=") != 0)
		return false;
	left = linitial(op->args);
	right = lsecond(op->args);
	return (gp_locus_join_var(left, OUTER_VAR, outer) &&
			gp_locus_join_var(right, INNER_VAR, inner)) ||
		(gp_locus_join_var(right, OUTER_VAR, outer) &&
		 gp_locus_join_var(left, INNER_VAR, inner));
}

/* All key columns of two colocated inputs must meet by equality */
static bool
gp_locus_join_keys(Join *join, GpLocus outer, GpLocus inner)
{
	bool	   *used;

	if (outer.kind != GP_LOCUS_SEGMENTS || inner.kind != GP_LOCUS_SEGMENTS ||
		outer.nkeys == 0 || outer.nkeys != inner.nkeys ||
		outer.colocation_id == 0 ||
		outer.colocation_id != inner.colocation_id)
		return false;
	used = palloc0_array(bool, inner.nkeys);
	for (int k = 0; k < outer.nkeys; k++)
	{
		bool		found = false;
		ListCell   *lc;

		foreach(lc, join->joinqual)
		{
			Node	   *qual = lfirst(lc);

			for (int j = 0; j < inner.nkeys; j++)
				if (!used[j] && gp_locus_join_pair(qual, outer.keys[k],
												 inner.keys[j]))
				{
					used[j] = true;
					found = true;
					break;
				}
			if (found)
				break;
		}
		if (!found)
		{
			pfree(used);
			return false;
		}
	}
	pfree(used);
	return true;
}

/* The locus of a binary join of two already computed inputs */
static GpLocus
gp_locus_join(Join *join, GpLocus outer, GpLocus inner)
{
	GpLocus	result;

	if (outer.kind == GP_LOCUS_COORDINATOR || inner.kind == GP_LOCUS_COORDINATOR)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	if (outer.kind == GP_LOCUS_ONE_SEGMENT && inner.kind == GP_LOCUS_ONE_SEGMENT)
	{
		result = gp_locus_make(GP_LOCUS_ONE_SEGMENT);
		result.replicated = outer.replicated && inner.replicated;
		return result;
	}
	if (outer.kind == GP_LOCUS_SEGMENTS && inner.kind == GP_LOCUS_SEGMENTS &&
		gp_locus_join_keys(join, outer, inner))
		/* A colocated outer join keeps every partner on its own segment */
		return gp_locus_project(&join->plan, outer);
	if (join->jointype == JOIN_FULL)
		/* Without colocation both inputs must meet on one chosen segment */
		return gp_locus_make(GP_LOCUS_ONE_SEGMENT);
	if (outer.kind == GP_LOCUS_ONE_SEGMENT)
	{
		if (!outer.replicated)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		return gp_locus_project(&join->plan, inner);
	}
	if (inner.kind == GP_LOCUS_ONE_SEGMENT)
	{
		if (!inner.replicated)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		return gp_locus_project(&join->plan, outer);
	}
	return gp_locus_make(GP_LOCUS_COORDINATOR);
}

/* All branches of an Append must have the same distribution to keep it */
static GpLocus
gp_locus_append(Append *append, List *rtable, List *subplans)
{
	GpLocus	result = gp_locus_make(GP_LOCUS_COORDINATOR);
	bool		first = true;

	foreach_ptr(Plan, child_plan, append->appendplans)
	{
		GpLocus	child = gp_locus_plan(child_plan, rtable, subplans);

		if (child.kind == GP_LOCUS_COORDINATOR)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		if (first)
		{
			result = child;
			first = false;
			continue;
		}
		if (result.kind != child.kind || result.replicated != child.replicated ||
			result.colocation_id != child.colocation_id ||
			result.nkeys != child.nkeys)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		for (int i = 0; i < result.nkeys; i++)
		{
			if (result.keys[i] != child.keys[i] ||
				(result.keytypes != NULL && child.keytypes != NULL &&
				 result.keytypes[i] != child.keytypes[i]) ||
				(result.keycollations != NULL && child.keycollations != NULL &&
				 result.keycollations[i] != child.keycollations[i]))
				return gp_locus_make(GP_LOCUS_COORDINATOR);
		}
	}
	if (first)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	return gp_locus_project(&append->plan, result);
}

/* Set operations require every equal output row on the same segment */
static GpLocus
gp_locus_setop(SetOp *setop, List *rtable, List *subplans)
{
	GpLocus	outer = gp_locus_plan(setop->plan.lefttree, rtable, subplans);
	GpLocus	inner = gp_locus_plan(setop->plan.righttree, rtable, subplans);
	GpLocus	result;

	if (outer.kind != inner.kind)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	if (outer.kind == GP_LOCUS_ONE_SEGMENT)
	{
		if (!outer.replicated || !inner.replicated)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		result = gp_locus_make(GP_LOCUS_ONE_SEGMENT);
		result.replicated = true;
		return result;
	}
	if (outer.kind != GP_LOCUS_SEGMENTS || outer.nkeys == 0 ||
		outer.nkeys != inner.nkeys ||
		outer.colocation_id != inner.colocation_id)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	for (int i = 0; i < outer.nkeys; i++)
		if (outer.keys[i] != inner.keys[i] ||
			(outer.keytypes != NULL && inner.keytypes != NULL &&
			 outer.keytypes[i] != inner.keytypes[i]) ||
			(outer.keycollations != NULL && inner.keycollations != NULL &&
			 outer.keycollations[i] != inner.keycollations[i]))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
	return gp_locus_project(&setop->plan, outer);
}

/* Compute the locus bottom-up without changing the plan */
static GpLocus
gp_locus_plan(Plan *plan, List *rtable, List *subplans)
{
	GpLocus	child;

	if (plan == NULL)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	if (gp_is_gather(plan) || IsA(plan, SeqScan))
	{
		RangeTblEntry *rte;
		GpPolicy	policy;

		if (((Scan *) plan)->scanrelid == 0 ||
			((Scan *) plan)->scanrelid > list_length(rtable))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		rte = rt_fetch(((Scan *) plan)->scanrelid, rtable);

		if (rte->rtekind != RTE_RELATION || !gp_policy_get(rte->relid, &policy))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		return gp_locus_policy(&policy);
	}
	if (IsA(plan, Gather))
		/* GpSlice is distributed itself; its full answer must not multiply here */
		return gp_locus_plan(plan->lefttree, rtable, subplans);
	if (IsA(plan, Sort) || IsA(plan, Limit) || IsA(plan, IncrementalSort) ||
		IsA(plan, Result))
	{
		child = gp_locus_plan(plan->lefttree, rtable, subplans);
		return gp_locus_unary(plan, child);
	}
	if (IsA(plan, SubqueryScan))
	{
		child = gp_locus_plan(((SubqueryScan *) plan)->subplan, rtable, subplans);
		return gp_locus_unary(plan, child);
	}
	if (IsA(plan, CteScan))
	{
		CteScan   *cte = (CteScan *) plan;

		if (subplans == NULL)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		child = gp_locus_plan(list_nth(subplans, cte->ctePlanId - 1),
							  rtable, subplans);
		return gp_locus_project(&cte->scan.plan, child);
	}
	if (IsA(plan, Append))
		return gp_locus_append((Append *) plan, rtable, subplans);
	if (IsA(plan, SetOp))
		return gp_locus_setop((SetOp *) plan, rtable, subplans);
	if (IsA(plan, Unique))
	{
		Unique	   *unique = (Unique *) plan;

		child = gp_locus_plan(unique->plan.lefttree, rtable, subplans);
		if (child.kind == GP_LOCUS_ONE_SEGMENT ||
			gp_locus_columns(child, unique->plan.lefttree,
							 unique->numCols, unique->uniqColIdx))
			return gp_locus_project(&unique->plan, child);
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	}
	if (IsA(plan, WindowAgg))
	{
		WindowAgg  *window = (WindowAgg *) plan;

		child = gp_locus_plan(window->plan.lefttree, rtable, subplans);
		if (child.kind == GP_LOCUS_ONE_SEGMENT ||
			gp_locus_columns(child, window->plan.lefttree,
							 window->partNumCols, window->partColIdx))
			return gp_locus_project(&window->plan, child);
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	}
	if (IsA(plan, Agg))
	{
		Agg		   *agg = (Agg *) plan;
		Plan	   *below = agg->plan.lefttree;

		while (below != NULL && IsA(below, Sort))
			below = below->lefttree;
		child = gp_locus_plan(agg->plan.lefttree, rtable, subplans);
		if (child.kind == GP_LOCUS_ONE_SEGMENT ||
			gp_locus_grouped(child, agg, below))
			return gp_locus_project(&agg->plan, child);
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	}
	if (IsA(plan, NestLoop) || IsA(plan, HashJoin) || IsA(plan, MergeJoin))
	{
		Join	   *join = (Join *) plan;
		GpLocus	outer = gp_locus_plan(join->plan.lefttree, rtable, subplans);
		GpLocus	inner = gp_locus_plan(join->plan.righttree, rtable, subplans);

		return gp_locus_join(join, outer, inner);
	}
	return gp_locus_make(GP_LOCUS_COORDINATOR);
}

/*
 * Copy a query after the planner, or refuse it when it cannot be deparsed.
 * A pulled-up or removed subquery keeps its slot with subquery = NULL, and the
 * join aliases of the flattened tree belong to the planner, not to ruleutils.
 * Such a tree is good for analysis, but not for pg_get_querydef: keep the
 * coordinator's plan rather than descend into a stale subquery.
 */
static Query *
gp_slice_query_copy(Query *parse)
{
	foreach_node(RangeTblEntry, rte, parse->rtable)
	{
		if (rte->rtekind == RTE_SUBQUERY && rte->subquery == NULL)
			return NULL;
		if (rte->rtekind == RTE_JOIN &&
			list_length(rte->joinaliasvars) != list_length(rte->eref->colnames))
			return NULL;
	}
	return copyObject(parse);
}

static bool gp_slice_union_leaf(Query *query);
static CustomScan *gp_slice_make(Plan *src, Query *original, bool replicated,
								 bool motion, List *sortClause);
typedef struct GpJoinTable GpJoinTable;
static bool gp_slice_join_rows(Query *parse, GpJoinTable *table, double *rows);
static bool gp_slice_agg_locus(Query *original, Agg *agg, Plan *below,
							   bool *replicated, bool *motion, GpLocus *locus);
static bool gp_slice_join_partial(Query *original, PlannedStmt *stmt,
								  Plan *plan, Plan *parent, bool sort_above,
								  bool motion);
static bool gp_slice_join_redistributed_agg(Query *original, PlannedStmt *stmt,
											Plan *plan, Plan *parent,
											bool sort_above, List *tables);

/* A Sort added after setrefs refers to its child as OUTER_VAR */
static Node *
gp_slice_sort_vars(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == INDEX_VAR)
	{
		Var		   *var = (Var *) node;

		var->varno = OUTER_VAR;
		var->varnosyn = OUTER_VAR;
	}
	return expression_tree_mutator(node, gp_slice_sort_vars, context);
}

/* Keep the original visibility of junk columns in an added Sort */
static void
gp_slice_sort_target(Query *original, Sort *sort)
{
	ListCell   *lc;

	sort->plan.targetlist = (List *) gp_slice_sort_vars(
		(Node *) copyObject(sort->plan.targetlist), NULL);
	foreach(lc, sort->plan.targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		TargetEntry *orig = get_tle_by_resno(original->targetList, tle->resno);

		if (orig != NULL)
			tle->resjunk = orig->resjunk;
	}
}

/*
 * The request worker does not inherit the settings of the session.  Only an
 * immutable fragment can run there without changing the answer.  The motion
 * function itself is the exception: its serialized argument was planned by
 * the coordinator, and the function is just the transport of that fragment.
 */
static bool
gp_slice_mutable_checker(Oid func_id, void *context)
{
	Oid			peer_query_oid = *((Oid *) context);

	return func_id != peer_query_oid &&
		func_volatile(func_id) != PROVOLATILE_IMMUTABLE;
}

static bool
gp_slice_mutable_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (check_functions_in_node(node, gp_slice_mutable_checker, context))
		return true;

	/* These nodes need the rules of contain_mutable_functions() too.  Be
	 * conservative rather than duplicate its detailed JSON and XML rules. */
	if (IsA(node, SQLValueFunction) || IsA(node, NextValueExpr) ||
		IsA(node, JsonConstructorExpr) || IsA(node, JsonExpr) ||
		IsA(node, XmlExpr))
		return true;

	if (IsA(node, Query))
		return query_tree_walker((Query *) node, gp_slice_mutable_walker,
							 context, 0);
	return expression_tree_walker(node, gp_slice_mutable_walker, context);
}

/* Whether an autonomous request preserves the semantics of this session */
static bool
gp_slice_autonomous(Query *query)
{
	Oid			peer_query_oid;

	if (GetUserId() != GetSessionUserId())
		return false;

	foreach_node(RangeTblEntry, rte, query->rtable)
	{
		HeapTuple	tuple;
		Form_pg_class rel;

		if (rte->rtekind != RTE_RELATION)
			continue;
		/* Children can have their own RLS policies; keep this case on libpq */
		if (rte->inh)
			return false;
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(rte->relid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", rte->relid);
		rel = (Form_pg_class) GETSTRUCT(tuple);
		if (rel->relrowsecurity || rel->relforcerowsecurity)
		{
			ReleaseSysCache(tuple);
			return false;
		}
		ReleaseSysCache(tuple);
	}

	peer_query_oid = DatumGetObjectId(DirectFunctionCall1(regprocedurein,
								  CStringGetDatum("gp.peer_query(text)")));
	return !gp_slice_mutable_walker((Node *) query, &peer_query_oid);
}

/*
 * Read all rows of the aggregate from the segments into the tuplestore.
 * The columns of the result are those of the target list of the node.
 */
static void
gp_slice_load_libpq(GpSliceState *state)
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

	if (state->motion && gp_dispatch_in_xact())
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("a motion query cannot see writes made earlier in the same transaction"),
				errhint("Run the query before writing, or prepare it after the write."));

	for (int i = 0; i < natts; i++)
	{
		Oid			recvfn;

		getTypeBinaryInputInfo(TupleDescAttr(tupdesc, i)->atttypid,
							   &recvfn, &ioparam[i]);
		fmgr_info(recvfn, &recv[i]);
	}

	state->store = tuplestore_begin_heap(true, false, work_mem);
	gp_dtx_statement_start(ss->ps.state);
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

/* Run a clean immutable fragment in autonomous interconnect workers */
static bool
gp_slice_load_interconnect(GpSliceState *state)
{
	ScanState  *ss = &state->css.ss;
	TupleDesc	tupdesc = ss->ss_ScanTupleSlot->tts_tupleDescriptor;
	GpSegment  *segments;
	char	   *plan;
	char	   *db;
	GpSnapshots *snapshots;
	const char *user;
	int			nsegments;
	int			only = 0;

	if (!state->autonomous || GetUserId() != GetSessionUserId())
		return false;

	plan = gp_interconnect_plan(state->sql);
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
			/* Earlier workers may have returned rows already */
			tuplestore_end(state->store);
			state->store = NULL;
			return false;
		}
	}
	return true;
}

static void
gp_slice_load(GpSliceState *state)
{
	if (XactIsoLevel == XACT_READ_COMMITTED && !gp_dispatch_in_xact() &&
		gp_slice_load_interconnect(state))
		return;
	gp_slice_load_libpq(state);
}

static void
gp_slice_begin(CustomScanState *node, EState *estate, int eflags)
{
	GpSliceState *state = (GpSliceState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	state->sql = strVal(linitial(cscan->custom_private));
	state->replicated = boolVal(lsecond(cscan->custom_private));
	state->motion = boolVal(lthird(cscan->custom_private));
	state->autonomous = boolVal(lfourth(cscan->custom_private));
}

static TupleTableSlot *
gp_slice_next(ScanState *ss)
{
	GpSliceState *state = (GpSliceState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	gp_dtx_statement_start(ss->ps.state);
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
		return gp_slice_query_copy(parse);

	/* A table by hash without a grouping cannot be answered by one segment */
	if (parse->groupClause == NIL)
		return NULL;

	return gp_slice_query_copy(parse);
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
	bool		replicated = false;
	bool		motion = false;
	GpLocus	locus;

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
	while (below != NULL && (IsA(below, Sort) || IsA(below, IncrementalSort)))
		below = below->lefttree;
	if (below == NULL || !gp_is_gather(below))
		return false;

	locus = gp_locus_plan(&agg->plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR)
	{
		/* A grouping that is not colocated can still move by its columns */
		if (!gp_slice_agg_locus(original, agg, below, &replicated, &motion, &locus))
			return false;
	}
	else
		replicated = locus.kind == GP_LOCUS_ONE_SEGMENT;

	/* The columns the segments return are the target list of the aggregate */
	if (list_length(agg->plan.targetlist) !=
		list_length(original->targetList))
		return false;

	cscan = gp_slice_make(&agg->plan, original, replicated, motion,
						  original->sortClause);

	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;

	return true;

}

/* One nonrecursive CTE over one distributed table */
Query *
gp_slice_cte_query(Query *parse)
{
	CommonTableExpr *cte;
	RangeTblEntry *rte;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		parse->hasAggs || parse->hasWindowFuncs || parse->hasSubLinks ||
		parse->hasTargetSRFs || parse->hasRowSecurity ||
		parse->setOperations != NULL || parse->groupClause != NIL ||
		parse->groupingSets != NIL || parse->distinctClause != NIL ||
		parse->rowMarks != NIL || list_length(parse->cteList) != 1)
		return NULL;
	cte = linitial_node(CommonTableExpr, parse->cteList);
	if (cte->cterecursive || !gp_slice_union_leaf((Query *) cte->ctequery))
		return NULL;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef))
		return NULL;
	rte = rt_fetch(linitial_node(RangeTblRef, parse->jointree->fromlist)->rtindex,
					 parse->rtable);
	if (rte->rtekind != RTE_CTE)
		return NULL;
	return gp_slice_query_copy(parse);
}

/* Replace a CteScan by the whole query, including its CTE */
bool
gp_slice_cte_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	GpLocus	locus;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit)))
	{
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL || !IsA(plan, CteScan))
		return false;
	if (original->sortClause != NIL && !sort_above)
		return false;

	locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR)
		return false;
	if (list_length(plan->targetlist) != list_length(original->targetList))
		return false;
	cscan = gp_slice_make(plan, original,
						  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
						  original->sortClause);
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	stmt->parallelModeNeeded = false;
	return true;
}

/* A window query over one distributed table, with every partition local */
Query *
gp_slice_window_query(Query *parse)
{
	RangeTblEntry *rte;
	GpPolicy	policy;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		!parse->hasWindowFuncs || parse->hasAggs || parse->hasSubLinks ||
		parse->hasTargetSRFs || parse->hasRowSecurity ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->distinctClause != NIL || parse->rowMarks != NIL)
		return NULL;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef) ||
		parse->jointree->quals != NULL)
		return NULL;
	rte = rt_fetch(linitial_node(RangeTblRef, parse->jointree->fromlist)->rtindex,
					 parse->rtable);
	if (rte->rtekind != RTE_RELATION || !gp_policy_get(rte->relid, &policy))
		return NULL;
	return gp_slice_query_copy(parse);
}

/* Replace a WindowAgg whose partitions all fit their distribution key */
bool
gp_slice_window_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	GpLocus	locus;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit) ||
							IsA(plan, IncrementalSort)))
	{
		if (IsA(plan, IncrementalSort))
		{
			Sort	   *full = makeNode(Sort);

			*full = ((IncrementalSort *) plan)->sort;
			full->plan.type = T_Sort;

			/* A distributed answer has no presorted input for IncrementalSort */
			full->plan.lefttree = plan->lefttree;
			if (parent != NULL)
				parent->lefttree = (Plan *) full;
			parent = (Plan *) full;
			plan = plan->lefttree;
			sort_above = true;
			continue;
		}
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL || !IsA(plan, WindowAgg))
		return false;
	if (original->sortClause != NIL && !sort_above)
		return false;

	locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR)
		return false;
	if (list_length(plan->targetlist) != list_length(original->targetList))
		return false;
	cscan = gp_slice_make(plan, original,
						  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
						  original->sortClause);
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	stmt->parallelModeNeeded = false;
	return true;
}

/* A DISTINCT query is local when its output rows share one distribution key */
Query *
gp_slice_distinct_query(Query *parse)
{
	RangeTblEntry *rte;
	GpPolicy	policy;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		parse->distinctClause == NIL || parse->hasWindowFuncs ||
		parse->hasSubLinks || parse->hasTargetSRFs || parse->hasRowSecurity ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupingSets != NIL || parse->rowMarks != NIL)
		return NULL;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef) ||
		parse->jointree->quals != NULL)
		return NULL;
	rte = rt_fetch(linitial_node(RangeTblRef, parse->jointree->fromlist)->rtindex,
					 parse->rtable);
	if (rte->rtekind != RTE_RELATION || !gp_policy_get(rte->relid, &policy))
		return NULL;
	return gp_slice_query_copy(parse);
}

/* Replace a Unique or a distinct aggregate whose keys are all local */
bool
gp_slice_distinct_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	GpLocus	locus;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit) ||
							IsA(plan, Unique)))
	{
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL)
		return false;
	if (original->sortClause != NIL && !sort_above)
		return false;

	locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR)
		return false;
	if (list_length(plan->targetlist) != list_length(original->targetList))
		return false;
	cscan = gp_slice_make(plan, original,
						  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
						  original->sortClause);
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	stmt->parallelModeNeeded = false;
	return true;
}

static bool
gp_slice_grouping_key(Query *parse, const GpPolicy *policy, GroupingSet *gs)
{
	ListCell   *lc;

	if (gs->kind == GROUPING_SET_SETS)
	{
		/* GROUPING SETS ((a), (a)) has one extra list wrapper */
		foreach(lc, gs->content)
		{
			if (!gp_slice_grouping_key(parse, policy,
									   lfirst_node(GroupingSet, lc)))
				return false;
		}
		return true;
	}
	if (gs->kind != GROUPING_SET_SIMPLE || gs->content == NIL)
		return false;
	for (int k = 0; k < policy->nkeys; k++)
	{
		bool		found = false;

		foreach(lc, gs->content)
		{
			Index		ref = lfirst_int(lc);
			TargetEntry *tle = get_sortgroupref_tle(ref, parse->targetList);
			Var		   *var;

			if (!IsA(tle->expr, Var))
				return false;
			var = (Var *) tle->expr;
			if (var->varattno == policy->keys[k])
			{
				found = true;
				break;
			}
		}
		if (!found)
			return false;
	}
	return true;
}

/* Every grouping set must contain the whole distribution key and be nonempty */
static bool
gp_slice_grouping_keys(Query *parse, const GpPolicy *policy)
{
	ListCell   *lc;

	foreach(lc, parse->groupingSets)
	{
		if (!gp_slice_grouping_key(parse, policy,
								   lfirst_node(GroupingSet, lc)))
			return false;
	}
	return true;
}

/* A GROUPING SETS query whose every set keeps its groups on one segment */
Query *
gp_slice_grouping_query(Query *parse)
{
	RangeTblEntry *rte;
	GpPolicy	policy;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		!parse->hasAggs || parse->groupingSets == NIL ||
		parse->hasWindowFuncs || parse->hasSubLinks || parse->hasTargetSRFs ||
		parse->hasRowSecurity || parse->setOperations != NULL ||
		parse->cteList != NIL || parse->distinctClause != NIL ||
		parse->rowMarks != NIL)
		return NULL;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef) ||
		parse->jointree->quals != NULL)
		return NULL;
	rte = rt_fetch(linitial_node(RangeTblRef, parse->jointree->fromlist)->rtindex,
					 parse->rtable);
	if (rte->rtekind != RTE_RELATION || !gp_policy_get(rte->relid, &policy) ||
		!gp_slice_grouping_keys(parse, &policy))
		return NULL;
	return gp_slice_query_copy(parse);
}

/* Replace an aggregate whose every grouping set covers the distribution key */
bool
gp_slice_grouping_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	Agg		   *agg;
	GpLocus	locus;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit)))
	{
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL || !IsA(plan, Agg))
		return false;
	if (original->sortClause != NIL && !sort_above)
		return false;
	agg = (Agg *) plan;
	locus = gp_locus_plan(&agg->plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR)
		return false;
	if (list_length(agg->plan.targetlist) != list_length(original->targetList))
		return false;
	cscan = gp_slice_make(&agg->plan, original,
						  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
						  original->sortClause);
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	stmt->parallelModeNeeded = false;
	return true;
}

/* A UNION ALL leaf reads one distributed table and nothing else */
static bool
gp_slice_union_leaf(Query *query)
{
	RangeTblEntry *rte;
	GpPolicy	policy;

	if (query->commandType != CMD_SELECT || query->setOperations != NULL ||
		query->hasAggs || query->hasWindowFuncs || query->hasSubLinks ||
		query->hasTargetSRFs || query->hasRowSecurity || query->cteList != NIL ||
		query->groupClause != NIL || query->groupingSets != NIL ||
		query->distinctClause != NIL || query->rowMarks != NIL ||
		query->sortClause != NIL || query->limitCount != NULL ||
		query->limitOffset != NULL)
		return false;
	if (!IsA(query->jointree, FromExpr) ||
		list_length(query->jointree->fromlist) != 1 ||
		!IsA(linitial(query->jointree->fromlist), RangeTblRef) ||
		query->jointree->quals != NULL)
		return false;
	rte = rt_fetch(linitial_node(RangeTblRef, query->jointree->fromlist)->rtindex,
					 query->rtable);
	return rte->rtekind == RTE_RELATION && gp_policy_get(rte->relid, &policy);
}

/* Every operation of the tree must be UNION ALL over such a leaf */
static bool
gp_slice_union_tree(Query *parse, Node *node)
{
	SetOperationStmt *setop;
	RangeTblEntry *rte;

	if (IsA(node, RangeTblRef))
	{
		rte = rt_fetch(((RangeTblRef *) node)->rtindex, parse->rtable);
		return rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL &&
			gp_slice_union_leaf(rte->subquery);
	}
	if (!IsA(node, SetOperationStmt))
		return false;
	setop = (SetOperationStmt *) node;
	return (setop->op == SETOP_UNION || setop->op == SETOP_INTERSECT ||
			setop->op == SETOP_EXCEPT) &&
		gp_slice_union_tree(parse, setop->larg) &&
		gp_slice_union_tree(parse, setop->rarg);
}

Query *
gp_slice_union_query(Query *parse)
{
	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		parse->setOperations == NULL || parse->hasAggs ||
		parse->hasWindowFuncs || parse->hasSubLinks || parse->hasTargetSRFs ||
		parse->hasRowSecurity || parse->cteList != NIL ||
		parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->distinctClause != NIL || parse->rowMarks != NIL)
		return NULL;
	if (!gp_slice_union_tree(parse, parse->setOperations))
		return NULL;
	return gp_slice_query_copy(parse);
}

/* Collect whether the tree mixes replicated and hashed leaves */
static void
gp_slice_union_mixed(Query *parse, Node *node, bool *saw_replicated,
					 bool *saw_hashed)
{
	SetOperationStmt *setop;
	RangeTblEntry *rte;
	GpPolicy	policy;

	if (IsA(node, RangeTblRef))
	{
		rte = rt_fetch(((RangeTblRef *) node)->rtindex, parse->rtable);
		if (rte->rtekind != RTE_SUBQUERY || rte->subquery == NULL)
			return;
		rte = rt_fetch(linitial_node(RangeTblRef,
									 rte->subquery->jointree->fromlist)->rtindex,
						 rte->subquery->rtable);
		if (!gp_policy_get(rte->relid, &policy))
			return;
		if (policy.replicated)
			*saw_replicated = true;
		else
			*saw_hashed = true;
		return;
	}
	if (!IsA(node, SetOperationStmt))
		return;
	setop = (SetOperationStmt *) node;
	gp_slice_union_mixed(parse, setop->larg, saw_replicated, saw_hashed);
	gp_slice_union_mixed(parse, setop->rarg, saw_replicated, saw_hashed);
}

/* Replace a compatible Append by one scan of its whole UNION ALL */
bool
gp_slice_union_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	bool		distinct = !((SetOperationStmt *) original->setOperations)->all;
	GpLocus	locus;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit) ||
							IsA(plan, Result) || IsA(plan, Gather) ||
							(distinct && (IsA(plan, Unique) || IsA(plan, Agg)))))
	{
		if (IsA(plan, Sort))
			sort_above = true;
		/* A parallel Gather is not kept above a distributed union */
		if (!IsA(plan, Gather))
			parent = plan;
		plan = plan->lefttree;
	}
	if (IsA(plan, SetOp))
	{
		/* HashSetOp has two inputs, not an Append: replace the whole node */
		locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
		if (locus.kind == GP_LOCUS_COORDINATOR)
			return false;
		cscan = gp_slice_make(plan, original,
							  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
							  original->sortClause);
		if (parent == NULL)
			stmt->planTree = &cscan->scan.plan;
		else
			parent->lefttree = &cscan->scan.plan;
		stmt->parallelModeNeeded = false;
		return true;
	}
	if (plan == NULL || !IsA(plan, Append))
		return false;
	if (original->sortClause != NIL && !sort_above)
		return false;

	locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR && !distinct)
	{
		bool		saw_replicated = false;
		bool		saw_hashed = false;

		/* Different hash maps have no common locus, but no duplicates either */
		gp_slice_union_mixed(original, original->setOperations,
							 &saw_replicated, &saw_hashed);
		if (saw_replicated && saw_hashed)
			return false;
	}
	if (locus.kind == GP_LOCUS_COORDINATOR && distinct)
		/* DISTINCT needs one hashed place for every equal output row */
		return false;
	if (list_length(plan->targetlist) != list_length(original->targetList))
		return false;
	cscan = gp_slice_make(plan, original,
						  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
						  original->sortClause);
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	/* No PostgreSQL parallel workers remain above the distributed union */
	stmt->parallelModeNeeded = false;
	return true;
}

/*
 * A scan that runs a query on the segments: its target list is that of the
 * plan it replaces, and its SQL is the query without its sort and limit
 */
static CustomScan *
gp_slice_make(Plan *src, Query *original, bool replicated, bool motion,
			  List *sortClause)
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
		out->ressortgroupref = tle->ressortgroupref;
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
	cscan->custom_private = list_make4(makeString(pg_get_querydef(remote, false)),
									   makeBoolean(replicated),
									   makeBoolean(motion),
									   makeBoolean(gp_slice_autonomous(remote)));
	cscan->methods = &gp_slice_plan_methods;
	return cscan;
}

typedef struct GpJoinTable
{
	Index		rti;
	Query	   *query;
	GpPolicy	policy;
} GpJoinTable;

/*
 * Whether the condition equates the two given columns of the two tables,
 * at the top of an AND or below it, with each Var at its own query level
 */
static bool
gp_slice_join_var_owned(Var *var, Query *condquery, GpJoinTable *table,
						AttrNumber key)
{
	if (var->varno != table->rti || var->varattno != key)
		return false;
	return table->query == condquery ? var->varlevelsup == 0 :
		var->varlevelsup > 0;
}

static bool
gp_slice_join_pair(Node *qual, Query *condquery, GpJoinTable *table1,
				   GpJoinTable *table2, AttrNumber k1, AttrNumber k2)
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
			if (gp_slice_join_pair(lfirst(lc), condquery, table1, table2, k1, k2))
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

	return (gp_slice_join_var_owned(var1, condquery, table1, k1) &&
			gp_slice_join_var_owned(var2, condquery, table2, k2)) ||
		(gp_slice_join_var_owned(var2, condquery, table1, k1) &&
		 gp_slice_join_var_owned(var1, condquery, table2, k2));
}

/* A join condition and the query level that owns its RT indexes */
typedef struct GpJoinCond
{
	Query	   *query;
	Node	   *qual;
} GpJoinCond;

/* Whether any of the join conditions equates the two columns */
static bool
gp_slice_join_pair_list(List *conds, GpJoinTable *table1,
						GpJoinTable *table2, AttrNumber k1, AttrNumber k2)
{
	ListCell   *lc;

	foreach(lc, conds)
	{
		GpJoinCond *cond = lfirst(lc);

		/* Do not confuse equal RT indexes of unrelated query levels */
		if (cond->query != table1->query && cond->query != table2->query)
			continue;
		if (gp_slice_join_pair(cond->qual, cond->query, table1, table2, k1, k2))
			return true;
	}
	return false;
}

/*
 * Collect the base tables of a join tree and all its join conditions;
 * false when a leaf is not a base table or a join type is not supported
 */
static bool
gp_slice_join_collect(Node *node, Query *query, List **tables, List **conds)
{
	JoinExpr   *join;

	if (IsA(node, RangeTblRef))
	{
		GpJoinTable *table = palloc0(sizeof(GpJoinTable));

		table->rti = ((RangeTblRef *) node)->rtindex;
		table->query = query;
		*tables = lappend(*tables, table);
		return true;
	}
	if (!IsA(node, JoinExpr))
		return false;
	join = (JoinExpr *) node;
	if (join->jointype != JOIN_INNER && join->jointype != JOIN_LEFT &&
		join->jointype != JOIN_RIGHT && join->jointype != JOIN_FULL)
		return false;
	if (join->quals != NULL)
	{
		GpJoinCond *cond = palloc0(sizeof(GpJoinCond));

		cond->query = query;
		cond->qual = join->quals;
		*conds = lappend(*conds, cond);
	}
	return gp_slice_join_collect(join->larg, query, tables, conds) &&
		gp_slice_join_collect(join->rarg, query, tables, conds);
}

/* A comma-separated FROM list is a set of inner joins too */
static bool
gp_slice_join_from(Query *parse, List **tables, List **conds)
{
	ListCell   *lc;

	if (!IsA(parse->jointree, FromExpr))
		return false;
	foreach(lc, parse->jointree->fromlist)
	{
		if (!gp_slice_join_collect(lfirst(lc), parse, tables, conds))
			return false;
	}
	if (parse->jointree->quals != NULL)
	{
		GpJoinCond *cond = palloc0(sizeof(GpJoinCond));

		cond->query = parse;
		cond->qual = parse->jointree->quals;
		*conds = lappend(*conds, cond);
	}
	return true;
}

typedef struct GpJoinSubqueries
{
	List	  **tables;
	List	  **conds;
	int			depth;
	bool		ok;
} GpJoinSubqueries;

/* Collect every correlated level, keeping the owner of each RT index */
static bool
gp_slice_join_sublink_walker(Node *node, GpJoinSubqueries *state)
{
	SubLink    *sublink;
	Query	   *subquery;

	if (node == NULL)
		return false;
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, gp_slice_join_sublink_walker,
							 state, 0);
	if (!IsA(node, SubLink))
		return expression_tree_walker(node, gp_slice_join_sublink_walker, state);
	sublink = (SubLink *) node;
	if (!IsA(sublink->subselect, Query))
	{
		state->ok = false;
		return true;
	}
	subquery = (Query *) sublink->subselect;
	/* Multiple outer levels need full varlevelsup-to-parent resolution later */
	if (state->depth > 0)
	{
		state->ok = false;
		return true;
	}
	state->depth++;
	if (!gp_slice_join_from(subquery, state->tables, state->conds))
	{
		state->ok = false;
		state->depth--;
		return true;
	}
	{
		bool		failed = query_tree_walker(subquery,
										   gp_slice_join_sublink_walker, state, 0);

		state->depth--;
		return failed;
	}
}

static bool
gp_slice_join_sublinks(Query *parse, List **tables, List **conds)
{
	GpJoinSubqueries state = { .tables = tables, .conds = conds, .ok = true };

	(void) query_tree_walker(parse, gp_slice_join_sublink_walker, &state, 0);
	return state.ok;
}

/* Load the policies needed to build a motion, independently of its locus */
static bool
gp_slice_join_load_policies(Query *parse, List *tables)
{
	ListCell   *lc;

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);
		RangeTblEntry *rte = rt_fetch(table->rti, table->query->rtable);

		if (rte->rtekind != RTE_RELATION ||
			!gp_policy_get(rte->relid, &table->policy))
			return false;
	}
	return true;
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

	if (!gp_slice_join_load_policies(parse, tables))
		return false;

	/* One colocation group for all hashed tables, if they are colocated */
	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

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
					if (!gp_slice_join_pair_list(conds, table, other,
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
 * Whether an inner join of hashed tables of different colocation groups is
 * connected by the whole keys of all of them.  The first table stays; every
 * table of another group moves by the hash of its key under its map.  The
 * connected join graph proves that every row meets all its partners on one
 * segment, even when the key equalities form a chain rather than name the
 * first table directly.  The table that stays and the tables that move are
 * returned
 */
static bool
gp_slice_join_redistributable(Query *parse, List *tables, List *conds,
							  GpJoinTable **stay, List **moves)
{
	GpJoinTable *first;
	Relation	rel1;
	List	   *connected;
	List	   *pending = NIL;
	ListCell   *lc;
	int			nkeys;
	bool		any_move = false;

	*moves = NIL;
	if (list_length(tables) < 2)
		return false;
	first = linitial(tables);
	if (first->policy.replicated || first->policy.colocation_id == 0 ||
		first->policy.nkeys == 0)
		return false;
	nkeys = first->policy.nkeys;

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (table->policy.replicated || table->policy.colocation_id == 0 ||
			table->policy.nkeys != nkeys)
			return false;
		if (table != first)
		{
			pending = lappend(pending, table);
			if (table->policy.colocation_id != first->policy.colocation_id)
				any_move = true;
		}
	}
	if (!any_move)
		return false;

	/* The key columns must hash to one place, so they are of one type */
	rel1 = table_open(rt_fetch(first->rti, parse->rtable)->relid,
					  AccessShareLock);
	foreach(lc, pending)
	{
		GpJoinTable *table = lfirst(lc);
		Relation	rel2 = table_open(rt_fetch(table->rti, parse->rtable)->relid,
									AccessShareLock);
		bool		ok = true;

		for (int i = 0; i < nkeys; i++)
		{
			if (TupleDescAttr(RelationGetDescr(rel1), first->policy.keys[i] - 1)->atttypid !=
				TupleDescAttr(RelationGetDescr(rel2), table->policy.keys[i] - 1)->atttypid)
			{
				ok = false;
				break;
			}
		}
		table_close(rel2, AccessShareLock);
		if (!ok)
		{
			table_close(rel1, AccessShareLock);
			return false;
		}
	}
	table_close(rel1, AccessShareLock);

	/* Grow the connected set by key pairs, regardless of colocation group */
	connected = list_make1(first);
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
					if (!gp_slice_join_pair_list(conds, table, other,
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
		{
			if (pending != NIL)
				return false;
			break;
		}
	}

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (table->policy.colocation_id != first->policy.colocation_id)
			*moves = lappend(*moves, table);
	}
	*stay = first;
	return true;
}

/* A colocated component of a join graph, ignoring all other tables */
static List *
gp_slice_join_component(List *tables, List *conds, GpJoinTable *root)
{
	List	   *connected = list_make1(root);
	List	   *pending = NIL;
	ListCell   *lc;

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (table != root && !table->policy.replicated)
			pending = lappend(pending, table);
	}
	for (;;)
	{
		bool		grew = false;

		foreach(lc, pending)
		{
			GpJoinTable *table = lfirst(lc);
			ListCell   *other_lc;

			foreach(other_lc, connected)
			{
				GpJoinTable *other = lfirst(other_lc);
				bool		all_keys = table->policy.nkeys == other->policy.nkeys &&
					table->policy.colocation_id == other->policy.colocation_id;

				for (int i = 0; all_keys && i < table->policy.nkeys; i++)
				{
					if (!gp_slice_join_pair_list(conds, table, other,
												 table->policy.keys[i],
												 other->policy.keys[i]))
						all_keys = false;
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
			break;
	}
	return connected;
}

/* Choose a stationary component and broadcast every other hashed table */
static bool
gp_slice_join_broadcast_component(Query *original, List *tables, List *conds,
								  GpJoinTable **stay, List **moves)
{
	List	   *best = NIL;
	List	   *best_moves = NIL;
	double		best_rows = 0;
	int			best_count = 0;
	bool		best_known = false;
	ListCell   *lc;

	*moves = NIL;
	foreach(lc, tables)
	{
		GpJoinTable *root = lfirst(lc);
		List	   *component;
		List	   *other_moves = NIL;
		ListCell   *it;
		double		rows = 0;
		bool		known = true;
		int			count;

		if (root->policy.replicated)
			continue;
		component = gp_slice_join_component(tables, conds, root);
		foreach(it, tables)
		{
			GpJoinTable *table = lfirst(it);
			double		table_rows;

			if (table->policy.replicated || list_member_ptr(component, table))
				continue;
			other_moves = lappend(other_moves, table);
			if (known && gp_slice_join_rows(original, table, &table_rows))
				rows += table_rows;
			else
				known = false;
		}
		if (other_moves == NIL)
			continue;
		count = list_length(other_moves);
		if (best == NIL ||
			(known && (!best_known || rows < best_rows)) ||
			(!known && !best_known && count < best_count))
		{
			best = component;
			best_moves = other_moves;
			best_rows = rows;
			best_count = count;
			best_known = known;
		}
	}
	if (best == NIL)
		return false;
	*stay = linitial(best);
	*moves = best_moves;
	return true;
}

/* Whether two hashed tables can join by broadcasting one of them */
static bool
gp_slice_join_broadcastable(List *tables)
{
	GpJoinTable *t1;
	GpJoinTable *t2;

	if (list_length(tables) != 2)
		return false;
	t1 = linitial(tables);
	t2 = lsecond(tables);
	return !t1->policy.replicated && !t2->policy.replicated &&
		t1->policy.colocation_id != 0 && t2->policy.colocation_id != 0;
}

/* The imported row count of a table, if ANALYZE has provided one */
static bool
gp_slice_join_rows(Query *parse, GpJoinTable *table, double *rows)
{
	Relation	rel = table_open(rt_fetch(table->rti, table->query->rtable)->relid,
							 NoLock);

	if (rel->rd_rel->reltuples < 0)
	{
		table_close(rel, NoLock);
		return false;
	}
	*rows = rel->rd_rel->reltuples;
	table_close(rel, NoLock);
	return true;
}

/*
 * The kind of the first join of the query: an inner join, or which side of
 * it is kept even when it has no partner
 */
static JoinType
gp_slice_join_type_of(Query *parse)
{
	Node	   *first;

	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1)
		return JOIN_INNER;
	first = linitial(parse->jointree->fromlist);
	if (!IsA(first, JoinExpr))
		return JOIN_INNER;
	return ((JoinExpr *) first)->jointype;
}

/*
 * Rewrite the table that moves into a pull of its rows from every segment:
 * the segment runs the query, and the function asks every segment — itself
 * among them — for the rows.  An inner join needs only the rows that the
 * hash of the join key places on the asking segment.  An outer join broadcasts
 * the moving side instead, so that rows with null keys and rows without a
 * partner are kept.  The map of the table that stays goes into the query as
 * an array, and the segment the rows are for comes in a setting of the
 * connection, so the serialized plan is one for all the segments
 */
static bool
gp_slice_redistribute_rte_keys(Query *remote, GpJoinTable *stay, GpJoinTable *move,
						 bool broadcast, int nkeys, const AttrNumber *keyatts,
							 const Bitmapset *needed,
							 bool default_collation)
{
	RangeTblEntry *rte = rt_fetch(move->rti, remote->rtable);
	Relation	rel;
	TupleDesc	tupdesc;
	StringInfoData sql;
	StringInfoData cols;
	StringInfoData map;
	StringInfoData keys;
	List	   *coltypes = NIL;
	List	   *coltypmods = NIL;
	List	   *colnames = NIL;
	List	   *colcolls = NIL;
	RangeTblFunction *rtf;
	FuncExpr   *func;
	Const	   *arg;
	Oid			peer_query_oid;

	rel = table_open(rte->relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	initStringInfo(&sql);
	initStringInfo(&cols);
	initStringInfo(&keys);
	initStringInfo(&map);
	appendStringInfoString(&map, "ARRAY[");
	for (int i = 0; i < stay->policy.nbuckets; i++)
		appendStringInfo(&map, "%s%d", i > 0 ? "," : "",
						 stay->policy.map[i]);
	appendStringInfoString(&map, "]");
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
		{
			table_close(rel, AccessShareLock);
			return false;
		}
		if (needed != NULL && !bms_is_member(att->attnum, needed))
			appendStringInfo(&cols, "%sNULL AS %s", cols.len > 0 ? ", " : "",
							 quote_identifier(NameStr(att->attname)));
		else
			appendStringInfo(&cols, "%s%s", cols.len > 0 ? ", " : "",
							 quote_identifier(NameStr(att->attname)));
		coltypes = lappend_oid(coltypes, att->atttypid);
		coltypmods = lappend_int(coltypmods, att->atttypmod);
		colnames = lappend(colnames, makeString(pstrdup(NameStr(att->attname))));
		colcolls = lappend_oid(colcolls, att->attcollation);
	}
	for (int i = 0; i < nkeys; i++)
		appendStringInfo(&keys, "%s%s%s", keys.len > 0 ? ", " : "",
						 quote_identifier(NameStr(TupleDescAttr(tupdesc,
																keyatts[i] - 1)->attname)),
						 default_collation &&
						 TupleDescAttr(tupdesc, keyatts[i] - 1)->attcollation != InvalidOid ?
						 " COLLATE pg_catalog.default" : "");

	if (broadcast)
	{
		appendStringInfo(&sql, "SELECT %s FROM %s%s",
						 cols.data,
						 rte->inh ? "" : "ONLY ",
						 quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
													RelationGetRelationName(rel)));
	}
	else
	{
		appendStringInfo(&sql, "SELECT %s FROM %s%s WHERE (%s)[gp.bucket(%d%s%s) + 1] OPERATOR(pg_catalog.=) current_setting('gp.requester_segment')::int4",
						 cols.data,
						 rte->inh ? "" : "ONLY ",
						 quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
													RelationGetRelationName(rel)),
						 map.data, stay->policy.nbuckets,
						 keys.len > 0 ? ", " : "", keys.data);
	}
	table_close(rel, AccessShareLock);

	peer_query_oid = DatumGetObjectId(DirectFunctionCall1(regprocedurein,
														  CStringGetDatum("gp.peer_query(text)")));
	arg = makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
					PointerGetDatum(cstring_to_text(gp_interconnect_plan(sql.data))), false, false);
	func = makeNode(FuncExpr);
	func->funcid = peer_query_oid;
	func->funcresulttype = RECORDOID;
	func->funcretset = true;
	func->funcvariadic = false;
	func->funcformat = COERCE_EXPLICIT_CALL;
	func->funccollid = InvalidOid;
	func->inputcollid = DEFAULT_COLLATION_OID;
	func->args = list_make1(arg);

	rtf = makeNode(RangeTblFunction);
	rtf->funccolcount = list_length(coltypes);
	rtf->funccoltypes = coltypes;
	rtf->funccoltypmods = coltypmods;
	rtf->funccolnames = colnames;
	rtf->funccolcollations = colcolls;
	rtf->funcexpr = (Node *) func;

	rte->rtekind = RTE_FUNCTION;
	rte->functions = list_make1(rtf);
	return true;
}

/* Redistribute a join side by its distribution key */
static bool
gp_slice_redistribute_rte(Query *remote, GpJoinTable *stay, GpJoinTable *move,
						 bool broadcast)
{
	return gp_slice_redistribute_rte_keys(remote, stay, move, broadcast,
										 move->policy.nkeys, move->policy.keys, NULL,
										 false);
}

/* Whether gp.bucket() can hash a grouping column of this type */
static bool
gp_slice_hashable(Oid typid)
{
	Oid			opclass = GetDefaultOpClass(typid, HASH_AM_OID);
	Oid			opcintype;
	Oid			proc;

	if (!OidIsValid(opclass))
		return false;
	opcintype = get_opclass_input_type(opclass);
	proc = get_opfamily_proc(get_opclass_family(opclass), opcintype, opcintype,
							 HASHEXTENDED_PROC);
	return OidIsValid(proc);
}

/*
 * Whether an aggregate of one table can run on the segments.  A replicated
 * table is answered by one of them.  A hash table answers locally when the
 * grouping covers its distribution key.  Otherwise its rows move by the
 * grouping columns, provided they are plain columns of the table: every group
 * then lands on one segment, which computes the whole aggregate
 */
static bool
gp_slice_agg_locus(Query *original, Agg *agg, Plan *below,
				   bool *replicated, bool *motion, GpLocus *locus)
{
	RangeTblRef *ref;
	RangeTblEntry *rte;
	GpJoinTable *table;
	AttrNumber *keyatts;

	*locus = gp_locus_make(GP_LOCUS_COORDINATOR);
	*replicated = false;
	*motion = false;
	if (!IsA(original->jointree, FromExpr) ||
		list_length(original->jointree->fromlist) != 1 ||
		!IsA(linitial(original->jointree->fromlist), RangeTblRef))
		return false;
	ref = linitial_node(RangeTblRef, original->jointree->fromlist);
	rte = rt_fetch(ref->rtindex, original->rtable);
	if (rte->rtekind != RTE_RELATION)
		return false;

	table = palloc0(sizeof(GpJoinTable));
	table->rti = ref->rtindex;
	if (!gp_policy_get(rte->relid, &table->policy))
		return false;
	*locus = gp_locus_policy(&table->policy);
	if (table->policy.replicated)
	{
		*replicated = true;
		return true;
	}

	if (gp_locus_grouped(*locus, agg, below))
	{
		*locus = gp_locus_project(&agg->plan, *locus);
		return true;
	}
	if (agg->numCols == 0 || gp_dispatch_in_xact())
		return false;

	/* Move every row by all the grouping columns instead */
	keyatts = palloc_array(AttrNumber, agg->numCols);
	for (int i = 0; i < agg->numCols; i++)
	{
		TargetEntry *tle = list_nth(below->targetlist, agg->grpColIdx[i] - 1);
		Var		   *var;

		if (!IsA(tle->expr, Var))
			return false;
		var = (Var *) tle->expr;
		if (var->varattno <= 0 || !gp_slice_hashable(var->vartype))
			return false;
		keyatts[i] = var->varattno;
	}
	if (!gp_slice_redistribute_rte_keys(original, table, table, false,
										agg->numCols, keyatts, NULL, false))
		return false;
	locus->colocation_id = table->policy.colocation_id;
	locus->nkeys = agg->numCols;
	locus->keys = keyatts;
	*locus = gp_locus_project(&agg->plan, *locus);
	*motion = true;
	return true;
}

/*
 * Rewrite the source of INSERT INTO a hashed table SELECT direct columns of
 * one hashed table into a pull by the target's distribution key.  Every
 * segment then inserts only the rows that belong to it.  Expressions and
 * defaults stay on the coordinator: functions and sequences must keep their
 * current meaning, and peer connections cannot see earlier writes
 */
bool
gp_slice_insert_motion(Query *parse, bool *motion)
{
	RangeTblEntry *target;
	RangeTblEntry *source;
	RangeTblRef *ref;
	Relation	target_rel;
	Relation	source_rel;
	Query	   *motion_query = parse;
	Index		outer_rti;
	GpJoinTable stay = {0};
	GpJoinTable move = {0};
	GpLocus	source_locus;
	GpLocus	target_locus;
	AttrNumber *keyatts;
	AttrNumber *sourceatts;
	Bitmapset  *needed = NULL;
	int			natts;

	if (!IsUnderPostmaster || parse->commandType != CMD_INSERT ||
		gp_dispatch_in_xact())
		return false;
	if (parse->resultRelation <= 0 || parse->hasSubLinks ||
		parse->hasAggs || parse->hasWindowFuncs || parse->hasTargetSRFs ||
		parse->hasRowSecurity || parse->setOperations != NULL ||
		parse->cteList != NIL || parse->groupClause != NIL ||
		parse->groupingSets != NIL || parse->distinctClause != NIL ||
		parse->sortClause != NIL || parse->limitCount != NULL ||
		parse->limitOffset != NULL || parse->rowMarks != NIL)
		return false;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef) ||
		parse->jointree->quals != NULL)
		return false;

	target = rt_fetch(parse->resultRelation, parse->rtable);
	if (target->rtekind != RTE_RELATION ||
		!gp_policy_get(target->relid, &stay.policy))
		return false;
	if (stay.policy.replicated)
		return false;
	stay.rti = parse->resultRelation;
	ref = linitial_node(RangeTblRef, parse->jointree->fromlist);
	outer_rti = ref->rtindex;
	source = rt_fetch(outer_rti, parse->rtable);
	if (source->rtekind == RTE_SUBQUERY)
	{
		motion_query = source->subquery;
		if (motion_query == NULL || motion_query->hasSubLinks ||
			motion_query->hasAggs || motion_query->hasWindowFuncs ||
			motion_query->hasTargetSRFs || motion_query->hasRowSecurity ||
			motion_query->setOperations != NULL || motion_query->cteList != NIL ||
			motion_query->groupClause != NIL || motion_query->groupingSets != NIL ||
			motion_query->distinctClause != NIL || motion_query->sortClause != NIL ||
			motion_query->limitCount != NULL || motion_query->limitOffset != NULL ||
			motion_query->rowMarks != NIL)
			return false;
		if (!IsA(motion_query->jointree, FromExpr) ||
			list_length(motion_query->jointree->fromlist) != 1 ||
			!IsA(linitial(motion_query->jointree->fromlist), RangeTblRef) ||
			motion_query->jointree->quals != NULL)
			return false;
		ref = linitial_node(RangeTblRef, motion_query->jointree->fromlist);
		source = rt_fetch(ref->rtindex, motion_query->rtable);
	}
	if (source->rtekind != RTE_RELATION ||
		!gp_policy_get(source->relid, &move.policy) || move.policy.replicated)
		return false;
	move.rti = ref->rtindex;
	source_locus = gp_locus_policy(&move.policy);
	target_locus = gp_locus_policy(&stay.policy);

	target_rel = table_open(target->relid, AccessShareLock);
	source_rel = table_open(source->relid, AccessShareLock);
	natts = RelationGetDescr(target_rel)->natts;
	if (list_length(parse->targetList) != natts)
		goto unsupported;
	keyatts = palloc_array(AttrNumber, stay.policy.nkeys);
	sourceatts = palloc_array(AttrNumber, natts);
	if (motion_query != parse)
	{
		foreach_node(TargetEntry, inner_tle, motion_query->targetList)
		{
			Var		   *inner_var;

			if (inner_tle->resjunk || !IsA(inner_tle->expr, Var))
				goto unsupported;
			inner_var = (Var *) inner_tle->expr;
			if (inner_var->varno != move.rti || inner_var->varattno <= 0)
				goto unsupported;
		}
	}
	for (int attno = 1; attno <= natts; attno++)
	{
		TargetEntry *tle = get_tle_by_resno(parse->targetList, attno);
		Var		   *var;

		if (tle == NULL || tle->resjunk || !IsA(tle->expr, Var))
			goto unsupported;
		var = (Var *) tle->expr;
		if (var->varno != outer_rti || var->varattno <= 0)
			goto unsupported;
		sourceatts[attno - 1] = var->varattno;
		if (motion_query != parse)
		{
			TargetEntry *inner_tle = get_tle_by_resno(motion_query->targetList,
													  var->varattno);
			Var		   *inner_var;

			if (inner_tle == NULL || inner_tle->resjunk ||
				!IsA(inner_tle->expr, Var))
				goto unsupported;
			inner_var = (Var *) inner_tle->expr;
			if (inner_var->varno != move.rti || inner_var->varattno <= 0)
				goto unsupported;
			sourceatts[attno - 1] = inner_var->varattno;
		}
		needed = bms_add_member(needed, sourceatts[attno - 1]);
	}
	for (int i = 0; i < stay.policy.nkeys; i++)
	{
		if (get_atttype(target->relid, stay.policy.keys[i]) !=
			get_atttype(source->relid, sourceatts[stay.policy.keys[i] - 1]))
			goto unsupported;
		keyatts[i] = sourceatts[stay.policy.keys[i] - 1];
	}
	/* The source is already colocated with the target only by every key */
	*motion = false;
	if (source_locus.colocation_id != target_locus.colocation_id ||
		source_locus.nkeys != target_locus.nkeys)
		*motion = true;
	for (int i = 0; i < stay.policy.nkeys && !*motion; i++)
		if (sourceatts[stay.policy.keys[i] - 1] != source_locus.keys[i])
			*motion = true;
	table_close(target_rel, AccessShareLock);
	table_close(source_rel, AccessShareLock);

	if (!*motion)
		return true;
	return gp_slice_redistribute_rte_keys(motion_query, &stay, &move, false,
										  stay.policy.nkeys, keyatts, needed, true);

unsupported:
	table_close(target_rel, AccessShareLock);
	table_close(source_rel, AccessShareLock);
	return false;
}

/*
 * Whether every join of the tree is inner.  An outer join below an inner
 * root also makes an N-way motion unsafe
 */
static bool
gp_slice_join_all_inner(Node *node)
{
	JoinExpr   *join;

	if (IsA(node, RangeTblRef))
		return true;
	if (!IsA(node, JoinExpr))
		return false;
	join = (JoinExpr *) node;
	return join->jointype == JOIN_INNER &&
		gp_slice_join_all_inner(join->larg) &&
		gp_slice_join_all_inner(join->rarg);
}

static bool
gp_slice_join_all_inner_from(Query *parse)
{
	ListCell   *lc;

	foreach(lc, parse->jointree->fromlist)
	{
		if (!gp_slice_join_all_inner(lfirst(lc)))
			return false;
	}
	return true;
}

/*
 * Move the sides of a join of colocation groups.  An inner join moves every
 * table of another group by the hash of its key under the map of the first.
 * An outer join of two tables broadcasts its null-supplying side instead.  A
 * full join broadcasts both sides and runs on one segment, as either side can
 * have an unmatched row
 */
static bool
gp_slice_join_motion(Query *original, List *tables, List *conds,
					 bool *single_segment, GpJoinTable **agg_stay)
{
	GpJoinTable *stay = NULL;
	GpJoinTable *move = NULL;
	List	   *moves = NIL;
	ListCell   *lc;
	JoinType	jointype;
	bool		redistributing;
	bool		multiway = false;

	*single_segment = false;
	*agg_stay = NULL;
	if (!gp_slice_join_load_policies(original, tables))
		return false;
	redistributing = gp_slice_join_redistributable(original, tables, conds,
												   &stay, &moves);
	if (!redistributing && list_length(tables) > 2)
		multiway = gp_slice_join_broadcast_component(original, tables, conds,
													&stay, &moves);
	else if (!redistributing && !gp_slice_join_broadcastable(tables))
		return false;
	if (!redistributing && list_length(tables) > 2 && !multiway)
		return false;
	jointype = gp_slice_join_type_of(original);
	if (list_length(tables) != 2 &&
		!gp_slice_join_all_inner_from(original))
	{
		return false;
	}
	if (redistributing)
	{
		move = linitial(moves);

		/* Either side of a two-way inner join can stay: move the smaller */
		if (jointype == JOIN_INNER && list_length(tables) == 2)
		{
			GpJoinTable *other_stay;
			List	   *other_moves;
			List	   *reverse = list_make2(lsecond(tables), linitial(tables));
			double		rows;
			double		other_rows;

			if (gp_slice_join_redistributable(original, reverse, conds,
											  &other_stay, &other_moves) &&
				gp_slice_join_rows(original, move, &rows) &&
				gp_slice_join_rows(original, linitial(other_moves), &other_rows) &&
				other_rows < rows)
			{
				stay = other_stay;
				moves = other_moves;
				move = linitial(moves);
			}
		}
	}
	else if (multiway)
		*agg_stay = stay;
	else
	{
		stay = linitial(tables);
		move = lsecond(tables);

		/* Broadcast the smaller side when there is no useful join key */
		if (jointype == JOIN_INNER)
		{
			double		rows;
			double		other_rows;

			if (gp_slice_join_rows(original, stay, &rows) &&
				gp_slice_join_rows(original, move, &other_rows) && rows < other_rows)
			{
				stay = lsecond(tables);
				move = linitial(tables);
			}
		}
		*agg_stay = stay;
	}
	switch (jointype)
	{
		case JOIN_INNER:
			if (redistributing)
			{
				foreach(lc, moves)
				{
					move = lfirst(lc);
					if (!gp_slice_redistribute_rte(original, stay, move, false))
						return false;
				}
			}
			else if (multiway)
			{
				foreach(lc, moves)
				{
					move = lfirst(lc);
					if (!gp_slice_redistribute_rte(original, stay, move, true))
						return false;
				}
			}
			else if (!gp_slice_redistribute_rte(original, stay, move, true))
				return false;
			break;
		case JOIN_LEFT:
			if (!gp_slice_redistribute_rte(original, stay, move, true))
				return false;
			break;
		case JOIN_RIGHT:
			if (!gp_slice_redistribute_rte(original, move, stay, true))
				return false;
			break;
		case JOIN_FULL:
			if (!gp_slice_redistribute_rte(original, stay, move, true) ||
				!gp_slice_redistribute_rte(original, move, stay, true))
				return false;
			*single_segment = true;
			break;
		default:
			return false;
	}
	return true;
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
 * Whether an outer join can supply nulls for the columns of a base table.
 * A group by a key of such a table is not local to a segment: unmatched rows
 * on different segments all belong to its null group, which must be finished
 * on the coordinator rather than returned as several groups
 */
static bool
gp_slice_join_nullable(Node *node, Index rti, bool nullable)
{
	JoinExpr   *join;
	bool		left_nullable = nullable;
	bool		right_nullable = nullable;

	if (IsA(node, RangeTblRef))
		return nullable && ((RangeTblRef *) node)->rtindex == rti;
	if (!IsA(node, JoinExpr))
		return false;
	join = (JoinExpr *) node;
	if (join->jointype == JOIN_LEFT || join->jointype == JOIN_FULL)
		right_nullable = true;
	if (join->jointype == JOIN_RIGHT || join->jointype == JOIN_FULL)
		left_nullable = true;

	return gp_slice_join_nullable(join->larg, rti, left_nullable) ||
		gp_slice_join_nullable(join->rarg, rti, right_nullable);
}

/*
 * Whether the grouping of the aggregate covers the distribution key of
 * some hashed table of the join, so that every group is on one segment.
 * All hashed tables are colocated by their keys, so grouping by the whole
 * key of any one of them that is not null-supplying is enough.  A broadcast
 * join is the exception: only the key of its stationary table is local.
 * The grouping columns are read from the plan
 */
static bool
gp_slice_join_agg_keys(Agg *agg, List *tables, Query *original,
					   GpJoinTable *only)
{
	ListCell   *lc;

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);
		bool		all_keys = true;

		if (table->policy.replicated || (only != NULL && table != only) ||
			gp_slice_join_nullable(linitial(original->jointree->fromlist),
								  table->rti, false))
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

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT)
		return NULL;
	if (parse->hasWindowFuncs ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupingSets != NIL || parse->rowMarks != NIL ||
		parse->distinctClause != NIL)
		return NULL;
	if (!IsA(parse->jointree, FromExpr) || parse->jointree->fromlist == NIL)
		return NULL;
	if (!gp_slice_join_from(parse, &tables, &conds))
	{
			return NULL;
	}
	if (parse->hasSubLinks &&
		!gp_slice_join_sublinks(parse, &tables, &conds))
		return NULL;
	if (list_length(tables) < 2)
		return NULL;
	if (!gp_slice_join_colocated(parse, tables, conds))
	{
		GpJoinTable *stay;
		List	   *moves;

		/* Nested RTEs cannot be rewritten into Motion by the simple join path */
		if (parse->hasSubLinks)
			return NULL;
		if (gp_dispatch_in_xact() ||
			(list_length(tables) > 2 &&
			 !gp_slice_join_broadcast_component(parse, tables, conds,
											 &stay, &moves) &&
			 !gp_slice_join_redistributable(parse, tables, conds,
											&stay, &moves) &&
			 !gp_slice_join_broadcastable(tables)))
			return NULL;
	}

	return gp_slice_query_copy(parse);
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
	bool		single_segment = false;
	bool		motion = false;
	GpJoinTable *agg_stay = NULL;
	GpLocus	locus;
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

	/* Non-aggregate plans without a final Sort keep their ordering here */
	if (original->sortClause != NIL && !sort_above && !original->hasAggs)
		return false;

	locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
	if (!gp_slice_join_from(original, &tables, &conds))
		return false;
	if (!gp_slice_join_load_policies(original, tables))
		return false;
	if (locus.kind == GP_LOCUS_COORDINATOR &&
		!gp_slice_join_colocated(original, tables, conds))
	{
		if (original->hasSubLinks)
			return false;
		{
			bool		motion_ok = gp_slice_join_motion(original, tables, conds,
													 &single_segment, &agg_stay);

						if (!motion_ok)
				return false;
		}

		motion = true;
	}
	else if (gp_slice_join_type_of(original) == JOIN_FULL &&
			 locus.kind == GP_LOCUS_ONE_SEGMENT && !locus.replicated)
	{
		if (original->hasSubLinks)
			return false;
		if (!gp_slice_join_motion(original, tables, conds, &single_segment,
								  &agg_stay))
			return false;
		motion = true;
	}
	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (!table->policy.replicated)
			replicated = false;
	}
	if (single_segment)
		replicated = true;

	/* The columns the segments return are the target list of the plan */
	if (list_length(plan->targetlist) != list_length(original->targetList))
		return false;

	/* An aggregate over the join can be partial even when it is not by a key */
	if (original->hasAggs && !replicated)
	{
		Plan	   *agg = plan;

		if (gp_slice_join_partial(original, stmt, plan, parent, sort_above, motion))
			return true;
		if (gp_slice_join_redistributed_agg(original, stmt, plan, parent,
										 sort_above, tables))
			return true;
		if (original->groupClause == NIL)
			return false;

		while (agg != NULL && !IsA(agg, Agg))
		{
			if (agg->righttree != NULL)
				return false;
			agg = agg->lefttree;
		}
		if (agg == NULL ||
			!gp_slice_join_agg_keys((Agg *) agg, tables, original, agg_stay))
			return false;
	}

	/* A failed partial aggregate must keep the original ordering here */
	if (original->sortClause != NIL && !sort_above)
		return false;

	cscan = gp_slice_make(plan, original, replicated, motion, original->sortClause);

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

		if (!gp_slice_join_from(subquery, &tables, &conds))
			return false;
		if (!gp_slice_join_colocated(subquery, tables, conds))
		{
			GpJoinTable *stay;
			List	   *moves;

			/* Both sides of a full join are broadcast to one segment */
			return list_length(tables) == 2 &&
				gp_slice_join_type_of(subquery) == JOIN_FULL &&
				(gp_slice_join_redistributable(subquery, tables, conds,
											   &stay, &moves) ||
				 gp_slice_join_broadcastable(tables));
		}
		foreach(lc, tables)
		{
			GpJoinTable *table = lfirst(lc);

			if (!table->policy.replicated)
				return false;
		}
		return true;
	}
}

/*
 * Whether the query is a select over one subquery, with nothing of its own
 * but a projection, a condition, a sort and a limit: whatever the planner put
 * below the sort and the limit is the plan of the subquery alone — an
 * aggregate, a join, or a SubqueryScan the planner chose to keep
 */
static bool
gp_slice_subquery_outer(Query *parse, RangeTblEntry **rte_out)
{
	RangeTblRef *ref;
	RangeTblEntry *rte;

	/* A condition pushed into the subquery may leave hasAggs set here.  A real
	 * aggregate of the outer query is not a unary node and never reaches this
	 * predicate through the walker's unary chain */
	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		parse->hasWindowFuncs || parse->hasSubLinks ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->distinctClause != NIL || parse->rowMarks != NIL)
		return false;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef))
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
static GpLocus
gp_slice_subquery_walk(Query *parse, Plan **planp, bool sort_above)
{
	Plan	   *plan = *planp;
	RangeTblEntry *rte;
	Query	   *subquery;
	Query	   *candidate;
	bool		is_agg;
	bool		replicated;
	bool		motion = false;
	bool		copies = false;
	CustomScan *cscan;
	GpLocus	locus = gp_locus_make(GP_LOCUS_COORDINATOR);

	if (plan == NULL)
		return gp_locus_make(GP_LOCUS_COORDINATOR);

	/* A condition or projection above the subquery changes rows, not their locus */
	if (IsA(plan, Sort) || IsA(plan, Limit) || IsA(plan, Result) ||
		IsA(plan, SubqueryScan))
	{
		Plan	  **childp = IsA(plan, SubqueryScan) ?
			&((SubqueryScan *) plan)->subplan : &plan->lefttree;
		GpLocus	child = gp_slice_subquery_walk(parse, childp,
							   sort_above || IsA(plan, Sort));
		return gp_locus_unary(plan, child);
	}

	if (parse->sortClause != NIL && !sort_above)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	if (!gp_slice_subquery_outer(parse, &rte))
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	subquery = rte->subquery;
	if (subquery->limitCount != NULL || subquery->limitOffset != NULL)
		return gp_locus_make(GP_LOCUS_COORDINATOR);

	candidate = gp_slice_agg_query(subquery);
	is_agg = candidate != NULL;
	if (candidate == NULL)
		candidate = gp_slice_join_query(subquery);
	if (candidate == NULL)
		return gp_locus_make(GP_LOCUS_COORDINATOR);

	/* The columns the segments return are the non-junk outputs of the subquery */
	if (parse->sortClause != NIL)
	{
		if (list_length(plan->targetlist) != list_length(subquery->targetList))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
	}
	else
	{
		int			planned = 0;
		int			needed = 0;
		ListCell   *lc;

		/* The planner may prune a subquery output the outer query never reads */
		foreach(lc, candidate->targetList)
		{
			TargetEntry *subtle = lfirst_node(TargetEntry, lc);
			ListCell   *plc;
			bool		returned = false;

			foreach(plc, plan->targetlist)
			{
				TargetEntry *ptle = lfirst_node(TargetEntry, plc);

				if (ptle->resno == subtle->resno && !ptle->resjunk)
				{
					returned = true;
					break;
				}
			}
			if (!returned)
				subtle->resjunk = true;
		}
		foreach(lc, plan->targetlist)
			if (!lfirst_node(TargetEntry, lc)->resjunk)
				planned++;
		foreach(lc, candidate->targetList)
			if (!lfirst_node(TargetEntry, lc)->resjunk)
				needed++;
		if (planned != needed)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
	}

	if (is_agg)
	{
		Plan	   *agg = plan;
		Plan	   *below;

		while (agg != NULL && !IsA(agg, Agg))
		{
			if (agg->righttree != NULL)
				return gp_locus_make(GP_LOCUS_COORDINATOR);
			agg = agg->lefttree;
		}
		if (agg == NULL)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		below = agg->lefttree;
		while (below != NULL && IsA(below, Sort))
			below = below->lefttree;
		if (below == NULL || !gp_is_gather(below) ||
			!gp_slice_agg_locus(candidate, (Agg *) agg, below,
								&replicated, &motion, &locus))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
	}
	else
	{
		List	   *tables = NIL;
		List	   *conds = NIL;
		bool		single_segment = false;
		GpJoinTable *agg_stay = NULL;

		locus = gp_locus_plan(plan, candidate->rtable, NULL);
		replicated = gp_slice_subquery_replicated(subquery, true);
		copies = replicated;

		if (!gp_slice_join_from(candidate, &tables, &conds))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		if (locus.kind == GP_LOCUS_COORDINATOR &&
			!gp_slice_join_colocated(candidate, tables, conds))
		{
			if (!gp_slice_join_motion(candidate, tables, conds, &single_segment,
									  &agg_stay))
				return gp_locus_make(GP_LOCUS_COORDINATOR);
			motion = true;
		}
		else if (gp_slice_join_type_of(candidate) == JOIN_FULL &&
				 locus.kind == GP_LOCUS_ONE_SEGMENT && !locus.replicated)
		{
			/* A full join that is not colocated still broadcasts both sides */
			if (!gp_slice_join_motion(candidate, tables, conds, &single_segment,
									  &agg_stay))
				return gp_locus_make(GP_LOCUS_COORDINATOR);
			motion = true;
		}
		replicated = replicated || single_segment;
		if (subquery->hasAggs && !replicated)
		{
			Plan	   *agg = plan;

			while (agg != NULL && !IsA(agg, Agg))
			{
				if (agg->righttree != NULL)
					return gp_locus_make(GP_LOCUS_COORDINATOR);
				agg = agg->lefttree;
			}
			if (agg == NULL ||
				!gp_slice_join_agg_keys((Agg *) agg, tables, subquery, agg_stay))
				return gp_locus_make(GP_LOCUS_COORDINATOR);
		}
	}
	if (is_agg)
		/* The planner puts both the original HAVING and a pushed outer condition
		 * in the aggregate's qual, not necessarily in the subquery's havingQual */
		candidate->havingQual = (Node *) copyObject((Node *) plan->qual);
	cscan = gp_slice_make(plan, candidate, replicated, motion, parse->sortClause);
	*planp = &cscan->scan.plan;
	if (is_agg)
		return locus;
	if (locus.kind != GP_LOCUS_COORDINATOR)
		return locus;
	locus = gp_locus_make(replicated ? GP_LOCUS_ONE_SEGMENT : GP_LOCUS_SEGMENTS);
	locus.replicated = copies;
	return locus;
}

void
gp_slice_subquery_plans(Query *parse, PlannedStmt *stmt)
{
	(void) gp_slice_subquery_walk(parse, &stmt->planTree, false);
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
	Plan	   *below;			/* old input, for resolving planned join Vars */
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
	if (IsA(node, Var) && state->below != NULL)
	{
		Plan	   *source = state->below;

		while (IsA(node, Var) &&
			   (((Var *) node)->varno == OUTER_VAR ||
				((Var *) node)->varno == INNER_VAR))
		{
			Var		   *var = (Var *) node;
			TargetEntry *tle;

			if (var->varattno <= 0)
				break;
			tle = list_nth(source->targetlist, var->varattno - 1);
			node = (Node *) tle->expr;
			if (IsA(node, Var) &&
				((Var *) node)->varno == INNER_VAR && source->righttree != NULL)
				source = source->righttree;
			else if (IsA(node, Var) &&
					 ((Var *) node)->varno == OUTER_VAR && source->lefttree != NULL)
				source = source->lefttree;
		}
	}
	foreach(lc, state->keys)
	{
		GpPartialKey *key = lfirst(lc);

		if (equal(key->expr, node) ||
			(IsA(key->expr, Var) && IsA(node, Var) &&
			 ((Var *) key->expr)->varno == ((Var *) node)->varno &&
			 ((Var *) key->expr)->varattno == ((Var *) node)->varattno))
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

/* Finish a partial aggregate over the slice built from its remote query */
static bool
gp_slice_partial_finish(Agg *agg, GpPartialState *state,
						Query *remote, bool motion,
						List *final_target, List *final_qual)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *below = agg->plan.lefttree;
	List	   *tlist = NIL;
	ListCell   *lc;
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
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->scan.plan.startup_cost = agg->plan.lefttree->startup_cost;
	cscan->scan.plan.total_cost = agg->plan.lefttree->total_cost;
	cscan->scan.plan.plan_rows = agg->plan.lefttree->plan_rows;
	cscan->scan.plan.plan_width = agg->plan.lefttree->plan_width;
	cscan->custom_scan_tlist = copyObject(remote->targetList);
	cscan->custom_private = list_make4(makeString(pg_get_querydef(remote, false)),
								   makeBoolean(false), makeBoolean(motion),
								   makeBoolean(gp_slice_autonomous(remote)));
	cscan->methods = &gp_slice_plan_methods;

	/* The final aggregate over the columns of the answer */
	state->ok = true;
	state->below = below;
	tlist = NIL;
	foreach(lc, final_target)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		TargetEntry *out;
		Node	   *expr = gp_partial_mutator((Node *) tle->expr, state);

		if (!state->ok)
			return false;
		out = makeTargetEntry((Expr *) expr, tle->resno,
							  tle->resname ? pstrdup(tle->resname) : NULL,
							  tle->resjunk);
		out->ressortgroupref = tle->ressortgroupref;
		tlist = lappend(tlist, out);
	}
	agg->plan.qual = (List *) gp_partial_mutator((Node *) final_qual, state);
	if (!state->ok)
		return false;

	{
		AttrNumber *grpColIdx = palloc_array(AttrNumber, list_length(state->keys));
		int			i = 0;

		foreach(lc, state->keys)
		{
			GpPartialKey *key = lfirst(lc);

			grpColIdx[i++] = key->pos;
		}
		agg->grpColIdx = grpColIdx;
	}
	agg->aggsplit = AGGSPLIT_SIMPLE;
	agg->plan.targetlist = tlist;
	agg->plan.lefttree = &cscan->scan.plan;
	return true;
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
	state.below = NULL;
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
	/* The planner leaves a list of implicit-AND conditions in the jointree */
	if (remote->jointree->quals != NULL &&
		IsA(remote->jointree->quals, List))
		remote->jointree->quals =
			(Node *) makeBoolExpr(AND_EXPR, (List *) remote->jointree->quals, -1);
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

	(void) gp_slice_partial_finish(agg, &state, remote, false,
								  parse->targetList, (List *) parse->havingQual);
}

/* A partitioned join feeds the same partial aggregate as a single table */
static bool
gp_slice_join_partial(Query *original, PlannedStmt *stmt, Plan *plan,
					  Plan *parent, bool sort_above, bool motion)
{
	GpPartialState state;
	GpPartialState finalstate;
	ListCell   *lc;
	Agg		   *agg;
	Query	   *remote;

	if (!IsA(plan, Agg))
		return false;
	agg = (Agg *) plan;
	if (agg->numCols != list_length(original->groupClause))
		return false;

	state.aggs = NIL;
	state.keys = NIL;
	state.npos = 0;
	state.nextaggno = 0;
	state.below = NULL;
	state.ok = true;
	foreach(lc, original->groupClause)
	{
		SortGroupClause *sc = lfirst_node(SortGroupClause, lc);
		TargetEntry *tle = get_sortgroupclause_tle(sc, original->targetList);
		GpPartialKey *key = palloc0(sizeof(GpPartialKey));

		key->expr = tle->expr;
		if (IsA(key->expr, Var))
		{
			Var		   *var = (Var *) key->expr;
			RangeTblEntry *group = rt_fetch(var->varno, original->rtable);

			if (group->rtekind == RTE_GROUP && var->varattno > 0)
				key->expr = list_nth(group->groupexprs, var->varattno - 1);
		}
		state.npos++;
		key->pos = state.npos;
		key->ressortgroupref = tle->ressortgroupref;
		state.keys = lappend(state.keys, key);
	}
	gp_partial_walker((Node *) original->targetList, &state);
	if (state.ok)
		gp_partial_walker((Node *) original->havingQual, &state);
	if (!state.ok || state.aggs == NIL)
		return false;

	remote = copyObject(original);
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

	/* Remote SQL needs original Vars; the final Agg needs planned Aggrefs */
	finalstate = state;
	finalstate.aggs = NIL;
	finalstate.npos = list_length(state.keys);
	finalstate.nextaggno = 0;
	finalstate.ok = true;
	gp_partial_walker((Node *) agg->plan.targetlist, &finalstate);
	if (finalstate.ok)
		gp_partial_walker((Node *) agg->plan.qual, &finalstate);
	if (!finalstate.ok || finalstate.aggs == NIL)
		return false;

	if (!gp_slice_partial_finish(agg, &finalstate, remote, motion,
								 agg->plan.targetlist, agg->plan.qual))
		return false;
	/* Partial groups from different segments are not sorted by group key */
	agg->aggstrategy = AGG_HASHED;
	if (original->sortClause != NIL && !sort_above)
	{
		Sort	   *sort = make_sort_from_sortclauses(original->sortClause,
												  &agg->plan);

		gp_slice_sort_target(original, sort);

		if (parent == NULL)
			stmt->planTree = &sort->plan;
		else
			parent->lefttree = &sort->plan;
	}
	return true;
}

/* Resolve a grouping key through the parser's GROUP and join RTEs */
static Node *
gp_slice_join_agg_key(Query *original, SortGroupClause *sc)
{
	TargetEntry *tle = get_sortgroupclause_tle(sc, original->targetList);
	Node	   *expr = (Node *) tle->expr;

	if (IsA(expr, Var))
	{
		Var		   *var = (Var *) expr;
		RangeTblEntry *rte = rt_fetch(var->varno, original->rtable);

		if (rte->rtekind == RTE_GROUP && var->varattno > 0)
			expr = list_nth(rte->groupexprs, var->varattno - 1);
	}
	return flatten_join_alias_for_parser(original, expr, 0);
}

static bool
gp_slice_join_agg_equal(Node *a, Node *b)
{
	return equal(a, b) ||
		(IsA(a, Var) && IsA(b, Var) &&
		 ((Var *) a)->varno == ((Var *) b)->varno &&
		 ((Var *) a)->varattno == ((Var *) b)->varattno);
}

/* Mapping of Vars of the original join to columns of its serialized answer */
typedef struct GpJoinAggMap
{
	Query	   *original;
	Query	   *inner;
	bool		ok;
} GpJoinAggMap;

static Node *
gp_slice_join_agg_var(Node *node, GpJoinAggMap *map)
{
	Node	   *expr = node;
	Var		   *var;
	RangeTblEntry *rte;
	ListCell   *lc;

	if (node == NULL)
		return NULL;
	if (!IsA(node, Var))
		return expression_tree_mutator(node, gp_slice_join_agg_var, map);
	var = (Var *) node;
	if (var->varlevelsup != 0 || var->varattno <= 0)
	{
		map->ok = false;
		return node;
	}
	rte = rt_fetch(var->varno, map->original->rtable);
	if (rte->rtekind == RTE_GROUP)
		expr = list_nth(rte->groupexprs, var->varattno - 1);
	else if (rte->rtekind == RTE_JOIN)
		expr = flatten_join_alias_for_parser(map->original, expr, 0);

	foreach(lc, map->inner->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (gp_slice_join_agg_equal((Node *) tle->expr, expr))
			return (Node *) makeVar(1, tle->resno, exprType((Node *) tle->expr),
								  exprTypmod((Node *) tle->expr),
								  exprCollation((Node *) tle->expr), 0);
	}
	map->ok = false;
	return node;
}

/* A function RTE that reads a serialized join with all of its output columns */
static RangeTblEntry *
gp_slice_join_agg_rte(Query *inner, const char *serialized)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RangeTblFunction *rtf = makeNode(RangeTblFunction);
	FuncExpr   *func = makeNode(FuncExpr);
	Const	   *arg;
	List	   *types = NIL;
	List	   *typmods = NIL;
	List	   *names = NIL;
	List	   *collations = NIL;
	ListCell   *lc;
	Oid			peer_query_oid = DatumGetObjectId(DirectFunctionCall1(
		regprocedurein, CStringGetDatum("gp.peer_query(text)")));

	foreach(lc, inner->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		types = lappend_oid(types, exprType((Node *) tle->expr));
		typmods = lappend_int(typmods, exprTypmod((Node *) tle->expr));
		names = lappend(names, makeString(pstrdup(tle->resname)));
		collations = lappend_oid(collations, exprCollation((Node *) tle->expr));
	}
	arg = makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
					PointerGetDatum(cstring_to_text(serialized)), false, false);
	func->funcid = peer_query_oid;
	func->funcresulttype = RECORDOID;
	func->funcretset = true;
	func->funcformat = COERCE_EXPLICIT_CALL;
	func->inputcollid = DEFAULT_COLLATION_OID;
	func->args = list_make1(arg);
	rtf->funccolcount = list_length(types);
	rtf->funccoltypes = types;
	rtf->funccoltypmods = typmods;
	rtf->funccolnames = names;
	rtf->funccolcollations = collations;
	rtf->funcexpr = (Node *) func;

	rte->rtekind = RTE_FUNCTION;
	rte->functions = list_make1(rtf);
	rte->eref = makeAlias("moved", names);
	rte->inFromCl = true;
	return rte;
}

/*
 * Compute a full aggregate after redistributing the join itself by all of its
 * grouping columns.  This is the general fallback when a partial aggregate is
 * not available: every group lands on one segment, whatever its aggregate does.
 */
static bool
gp_slice_join_redistributed_agg(Query *original, PlannedStmt *stmt, Plan *plan,
								Plan *parent, bool sort_above, List *tables)
{
	Agg		   *agg;
	Query	   *inner;
	Query	   *remote;
	GpJoinAggMap map;
	GpJoinTable *maptable = NULL;
	StringInfoData sql;
	AttrNumber *keypos;
	ListCell   *lc;
	CustomScan *cscan;
	RangeTblEntry *rte;
	FromExpr   *from;
	int			pos = 0;
	int			keyidx = 0;
	int			nkeys = list_length(original->groupClause);
	char	   *serialized;

	if (nkeys == 0 || !IsA(plan, Agg))
		return false;
	agg = (Agg *) plan;
	if (agg->numCols != nkeys)
		return false;

	/* The inner query computes the join, before any aggregation */
	inner = copyObject(original);
	inner->targetList = NIL;
	inner->hasAggs = false;
	inner->hasGroupRTE = false;
	inner->groupClause = NIL;
	inner->groupingSets = NIL;
	inner->havingQual = NULL;
	inner->distinctClause = NIL;
	inner->sortClause = NIL;
	inner->limitCount = NULL;
	inner->limitOffset = NULL;
	foreach(lc, original->rtable)
	{
		RangeTblEntry *input = lfirst_node(RangeTblEntry, lc);
		Relation	rel;
		TupleDesc	tupdesc;
		int			rti = foreach_current_index(lc) + 1;

		if (input->rtekind != RTE_RELATION &&
			(input->rtekind != RTE_FUNCTION || !OidIsValid(input->relid)))
			continue;
		rel = table_open(input->relid, NoLock);
		tupdesc = RelationGetDescr(rel);
		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			Var		   *var;
			TargetEntry *tle;

			if (att->attisdropped)
				continue;
			var = makeVar(rti, att->attnum, att->atttypid,
						  att->atttypmod, att->attcollation, 0);
			pos++;
			tle = makeTargetEntry((Expr *) var, pos, psprintf("c%d", pos), false);
			inner->targetList = lappend(inner->targetList, tle);
		}
		table_close(rel, NoLock);
	}
	if (inner->targetList == NIL)
		return false;

	/* Grouping expressions can be computed columns of the join answer */
	keypos = palloc_array(AttrNumber, nkeys);
	foreach(lc, original->groupClause)
	{
		SortGroupClause *sc = lfirst_node(SortGroupClause, lc);
		Node	   *expr = gp_slice_join_agg_key(original, sc);
		TargetEntry *found = NULL;
		ListCell   *it;

		if (!gp_slice_hashable(exprType(expr)))
			return false;
		foreach(it, inner->targetList)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, it);

			if (gp_slice_join_agg_equal((Node *) tle->expr, expr))
			{
				found = tle;
				break;
			}
		}
		if (found == NULL)
		{
			pos++;
			found = makeTargetEntry((Expr *) copyObject(expr), pos,
									psprintf("c%d", pos), false);
			inner->targetList = lappend(inner->targetList, found);
		}
		keypos[keyidx++] = found->resno;
	}

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (!table->policy.replicated)
		{
			maptable = table;
			break;
		}
	}
	if (maptable == NULL)
		return false;

	/* Keep on the asking segment only rows whose grouping key belongs to it */
	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT * FROM (%s) AS joined WHERE (ARRAY[",
					 pg_get_querydef(inner, false));
	for (int i = 0; i < maptable->policy.nbuckets; i++)
		appendStringInfo(&sql, "%s%d", i > 0 ? "," : "",
						 maptable->policy.map[i]);
	appendStringInfo(&sql, "])[gp.bucket(%d", maptable->policy.nbuckets);
	for (int i = 0; i < nkeys; i++)
	{
		TargetEntry *tle = list_nth(inner->targetList, keypos[i] - 1);

		appendStringInfo(&sql, ", %s%s", quote_identifier(tle->resname),
						 exprCollation((Node *) tle->expr) != InvalidOid ?
						 " COLLATE pg_catalog.default" : "");
	}
	appendStringInfoString(&sql,
		") + 1] OPERATOR(pg_catalog.=) current_setting('gp.requester_segment')::int4");
	serialized = gp_interconnect_plan(sql.data);

	/* The original aggregate now reads the moved columns from one function */
	remote = copyObject(original);
	rte = gp_slice_join_agg_rte(inner, serialized);
	remote->rtable = list_make1(rte);
	remote->rteperminfos = NIL;
	remote->hasGroupRTE = false;
	from = makeNode(FromExpr);
	{
		RangeTblRef *ref = makeNode(RangeTblRef);

		ref->rtindex = 1;
		from->fromlist = list_make1(ref);
	}
	remote->jointree = from;
	map.original = original;
	map.inner = inner;
	map.ok = true;
	remote = query_tree_mutator(remote, gp_slice_join_agg_var, &map, 0);
	if (!map.ok)
		return false;

	cscan = gp_slice_make(plan, remote, false, true, original->sortClause);
	/* All Vars above the replacement refer to the function's output */
	stmt->rtable = copyObject(remote->rtable);
	stmt->permInfos = NIL;
	stmt->unprunableRelids = NULL;
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	if (original->sortClause != NIL && !sort_above)
	{
		Sort	   *sort = make_sort_from_sortclauses(original->sortClause,
												  &cscan->scan.plan);

		gp_slice_sort_target(original, sort);

		if (parent == NULL)
			stmt->planTree = &sort->plan;
		else
			parent->lefttree = &sort->plan;
	}
	return true;
}

/*
 * Install the node on the coordinator; called from _PG_init()
 */
void
gp_slice_init(void)
{
	RegisterCustomScanMethods(&gp_slice_plan_methods);
}
