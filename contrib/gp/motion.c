/* -------------------------------------------------------------------------
 *
 * motion.c
 *		Rows of a query from every segment of the cluster
 *
 * A query that joins tables of different colocation groups needs the rows
 * of some of them to move, so that every row meets its partners on one
 * segment.  The movement goes the pull way: every segment, as it runs the
 * query, asks every segment — itself among them — for rows.  A fragment is
 * sent as a serialized PlannedStmt, never as SQL text, and the answer is a
 * stream of binary tuples.  An inner join
 * either asks for the rows that the hash of the join key places on it, however
 * many tables move, or broadcasts a whole table when there is no such key.  An
 * outer join of two tables broadcasts a whole side instead, keeping rows with
 * null keys and rows without a partner.
 *
 * An aggregate whose grouping columns do not cover the distribution key moves
 * its rows the same way, but hashes those grouping columns: every group lands
 * on one segment, which computes the whole aggregate.  An INSERT ... SELECT
 * hashes its source rows by the distribution key of the target table, so
 * every segment inserts only the rows that belong to it.
 *
 * The coordinator leaves gp.peer_query in the query as the description of
 * the rows to move.  Its planner hook replaces that function by GpMotion,
 * a CustomScan with the same serialized fragment and answer columns.  The
 * function remains as a wrapper for compatibility, but a fragment contains
 * the motion node itself rather than a FunctionScan.
 *
 * The addresses of the segments are not kept here: they are asked of the
 * coordinator at every call, so a cluster that changed stays correct.  The
 * segment the rows are for is told to the others by a setting of the
 * connection, for the query itself is one for all of them.  Every segment
 * answers as the user that runs the query, in the transaction of its
 * connection: the lock of the distributed snapshot, held by the
 * coordinator from before its question to after the last row, keeps the
 * answers of all the segments on one state of the cluster.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/motion.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
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
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "gp.h"

/* The cost of the trip to the segments, before the first row */
#define GP_MOTION_STARTUP_COST 100.0

/* A row from the segments costs more than one of a table */
#define GP_MOTION_ROW_COST_FACTOR 10.0

/* The wait event of the connections of the motion */
static uint32 gp_motion_wait_event = 0;
static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;

typedef struct GpMotionState
{
	CustomScanState css;
	char	   *serialized;
	Tuplestorestate *store;		/* all rows, once read */
} GpMotionState;

static Node *gp_motion_create_state(CustomScan *cscan);
static Plan *gp_motion_plan(PlannerInfo *root, RelOptInfo *rel,
							CustomPath *best_path, List *tlist,
							List *clauses, List *custom_plans);

static const CustomScanMethods gp_motion_plan_methods = {
	.CustomName = "GpMotion",
	.CreateCustomScanState = gp_motion_create_state,
};

static uint32
gp_motion_event(void)
{
	if (gp_motion_wait_event == 0)
		gp_motion_wait_event = WaitEventExtensionNew("GpMotion");
	return gp_motion_wait_event;
}

/* Ask every segment, this one included, for the rows of the fragment */
static void
gp_motion_fetch(Tuplestorestate *store, TupleDesc tupdesc,
				const char *serialized)
{
	const char *coord_keywords[] = {"dbname", "dbname", "application_name",
	NULL};
	const char *coord_values[3];
	char	   *db;
	PGconn	   *conn;
	PGresult   *segments;

	/* The segments of the cluster, asked of the coordinator */
	db = get_database_name(MyDatabaseId);
	coord_values[0] = gp_coordinator_conninfo();
	coord_values[1] = gp_database;
	coord_values[2] = "gp motion";
	conn = libpqsrv_connect_params(coord_keywords, coord_values, true,
								   gp_motion_event());
	if (PQstatus(conn) != CONNECTION_OK)
	{
		char	   *err = pchomp(PQerrorMessage(conn));

		libpqsrv_disconnect(conn);
		ereport(ERROR,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("could not ask the coordinator for the segments"),
				errdetail("%s", err));
	}
	/*
	 * The registry is the only address of a broker: there is no rule to
	 * fall back to.  A segment registers itself a moment before its broker
	 * reports its port, so that moment is waited out the way an
	 * unreachable broker is, not failed at once
	 */
	segments = NULL;
	for (int attempt = 0;; attempt++)
	{
		bool		reported = true;

		if (segments != NULL)
			PQclear(segments);
		segments = libpqsrv_exec_params(conn,
										"SELECT segment_id, host, port, interconnect_port FROM gp.cluster_segments()",
										0, NULL, NULL, NULL, NULL, 0,
										gp_motion_event());
		if (PQresultStatus(segments) != PGRES_TUPLES_OK)
		{
			char	   *err = pchomp(PQresultErrorMessage(segments));

			PQclear(segments);
			libpqsrv_disconnect(conn);
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not read the segments of the cluster"),
					errdetail("%s", err));
		}
		for (int i = 0; i < PQntuples(segments); i++)
			if (PQgetisnull(segments, i, 3))
				reported = false;
		if (reported || attempt >= 59)
			break;
		CHECK_FOR_INTERRUPTS();
		pg_usleep(100000);
	}

	for (int i = 0; i < PQntuples(segments); i++)
	{
		const char *host = PQgetvalue(segments, i, 1);
		int			segment_id = atoi(PQgetvalue(segments, i, 0));
		int			broker = PQgetisnull(segments, i, 3) ? 0 :
			atoi(PQgetvalue(segments, i, 3));
		int			requester = gp_segment_id;

		if (broker == 0)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("segment %d has not reported its interconnect port yet",
						   segment_id));
		gp_interconnect_query(store, tupdesc, db,
							  GetUserNameFromId(GetUserId(), false), serialized,
							  host, broker, requester);
	}

	PQclear(segments);
	libpqsrv_disconnect(conn);
}

static TupleTableSlot *
gp_motion_next(ScanState *ss)
{
	GpMotionState *state = (GpMotionState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (state->store == NULL)
	{
		MemoryContext oldcontext;

		if (gp_role != GP_ROLE_SEGMENT)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("GpMotion runs on a segment only"));

		oldcontext = MemoryContextSwitchTo(ss->ps.state->es_query_cxt);
		state->store = tuplestore_begin_heap(true, false, work_mem);
		gp_motion_fetch(state->store, ss->ss_ScanTupleSlot->tts_tupleDescriptor,
						state->serialized);
		MemoryContextSwitchTo(oldcontext);
	}

	/* At the end the slot is empty, which is what ExecScan() looks for */
	(void) tuplestore_gettupleslot(state->store, true, false, slot);
	return slot;
}

static bool
gp_motion_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
gp_motion_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, gp_motion_next, gp_motion_recheck);
}

static void
gp_motion_begin(CustomScanState *node, EState *estate, int eflags)
{
	GpMotionState *state = (GpMotionState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	state->serialized = strVal(linitial(cscan->custom_private));
}

static void
gp_motion_end(CustomScanState *node)
{
	GpMotionState *state = (GpMotionState *) node;

	if (state->store != NULL)
		tuplestore_end(state->store);
	state->store = NULL;
}

/* The rows are all here, so a rescan reads them again from the store */
static void
gp_motion_rescan(CustomScanState *node)
{
	GpMotionState *state = (GpMotionState *) node;

	if (state->store != NULL)
		tuplestore_rescan(state->store);
	ExecScanReScan(&node->ss);
}

static const CustomExecMethods gp_motion_exec_methods = {
	.CustomName = "GpMotion",
	.BeginCustomScan = gp_motion_begin,
	.ExecCustomScan = gp_motion_exec,
	.EndCustomScan = gp_motion_end,
	.ReScanCustomScan = gp_motion_rescan,
};

static Node *
gp_motion_create_state(CustomScan *cscan)
{
	GpMotionState *state = (GpMotionState *) newNode(sizeof(GpMotionState),
													 T_CustomScanState);

	state->css.flags = cscan->flags;
	state->css.methods = &gp_motion_exec_methods;
	/* A tuplestore hands out minimal tuples */
	state->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) state;
}

/* Make the plan of the path; setrefs maps its answer onto INDEX_VAR */
static Plan *
gp_motion_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			   List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	RangeTblEntry *rte = root->simple_rte_array[rel->relid];
	RangeTblFunction *rtf = linitial(rte->functions);
	List	   *scan_tlist = NIL;

	/* The source returns every column of the function, not only the columns
	 * that the plan above this one needs */
	for (int i = 0; i < rtf->funccolcount; i++)
	{
		Var		   *var = makeVar(rel->relid, i + 1,
								 list_nth_oid(rtf->funccoltypes, i),
								 list_nth_int(rtf->funccoltypmods, i),
								 list_nth_oid(rtf->funccolcollations, i), 0);
		const char *name = strVal(list_nth(rtf->funccolnames, i));

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) var, i + 1,
											 pstrdup(name), false));
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = best_path->custom_private;
	cscan->methods = &gp_motion_plan_methods;
	return &cscan->scan.plan;
}

static const CustomPathMethods gp_motion_path_methods = {
	.CustomName = "GpMotion",
	.PlanCustomPath = gp_motion_plan,
};

/* The serialized argument of the planner's gp.peer_query marker */
static char *
gp_motion_serialized(RangeTblEntry *rte)
{
	RangeTblFunction *rtf;
	FuncExpr   *func;
	Const	   *arg;
	Oid			argtype = TEXTOID;
	Oid			peer_query_oid;

	if (rte->rtekind != RTE_FUNCTION || rte->funcordinality ||
		list_length(rte->functions) != 1)
		return NULL;
	rtf = linitial(rte->functions);
	if (!IsA(rtf->funcexpr, FuncExpr))
		return NULL;
	func = (FuncExpr *) rtf->funcexpr;
	peer_query_oid = LookupFuncName(list_make2(makeString("gp"),
											 makeString("peer_query")),
									1, &argtype, true);
	if (!OidIsValid(peer_query_oid) || func->funcid != peer_query_oid ||
		list_length(func->args) != 1)
		return NULL;
	arg = (Const *) linitial(func->args);
	if (!IsA(arg, Const) || arg->consttype != TEXTOID || arg->constisnull)
		return NULL;
	return TextDatumGetCString(arg->constvalue);
}

/* Replace the function marker by a real motion path in a fragment */
static void
gp_motion_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
						   RangeTblEntry *rte)
{
	CustomPath *cpath;
	char	   *serialized;

	if (prev_set_rel_pathlist)
		prev_set_rel_pathlist(root, rel, rti, rte);

	if (!IsUnderPostmaster || rel->reloptkind != RELOPT_BASEREL)
		return;
	serialized = gp_motion_serialized(rte);
	if (serialized == NULL)
		return;

	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = rel->rows;
	cpath->path.startup_cost = GP_MOTION_STARTUP_COST +
		rel->baserestrictcost.startup;
	cpath->path.total_cost = cpath->path.startup_cost +
		rel->tuples * cpu_tuple_cost * GP_MOTION_ROW_COST_FACTOR +
		rel->baserestrictcost.per_tuple * rel->tuples;
	cpath->path.pathkeys = NIL;
	cpath->flags = 0;
	cpath->custom_paths = NIL;
	cpath->custom_private = list_make1(makeString(serialized));
	cpath->methods = &gp_motion_path_methods;

	add_path(rel, &cpath->path);
}

/*
 * The SQL wrapper remains available, but the planner turns its calls in a
 * fragment into GpMotion instead of executing a FunctionScan
 */
PG_FUNCTION_INFO_V1(gp_peer_query);

Datum
gp_peer_query(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *store;
	TupleDesc	tupdesc;
	char	   *sql;
	MemoryContext oldcontext;

	/* The planner of the coordinator writes the calls; none runs by hand */
	if (gp_role != GP_ROLE_SEGMENT)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("gp.peer_query runs on a segment only"));

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("set-valued function called in context that cannot accept a set"));
	if (rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("gp.peer_query needs the columns of its answer"));
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("the query of the rows is required"));

	/* The result must survive the per-tuple context of the function call */
	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	store = tuplestore_begin_heap(true, false, work_mem);
	MemoryContextSwitchTo(oldcontext);
	sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	gp_motion_fetch(store, tupdesc, sql);

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = store;
	rsinfo->setDesc = tupdesc;

	return (Datum) 0;
}

/* Install the executor everywhere and the planner hook on the coordinator */
void
gp_motion_init(void)
{
	RegisterCustomScanMethods(&gp_motion_plan_methods);
	if (gp_role == GP_ROLE_COORDINATOR)
	{
		prev_set_rel_pathlist = set_rel_pathlist_hook;
		set_rel_pathlist_hook = gp_motion_set_rel_pathlist;
	}
}
