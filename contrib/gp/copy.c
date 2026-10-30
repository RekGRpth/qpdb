/* -------------------------------------------------------------------------
 *
 * copy.c
 *		COPY FROM into a distributed table
 *
 * The table on the coordinator holds no rows, so COPY FROM into it does
 * not run on the coordinator as usual.  The coordinator reads and parses
 * the input, evaluates the defaults of columns that the input lacks,
 * finds the segment of each row by its bucket, and sends the rows to
 * the segments in COPY ... FROM STDIN of the binary format, which takes
 * the types as they are.  The segments run the COPY in the transactions
 * that follow the transaction of the coordinator, and check the
 * constraints and compute the generated columns themselves.  A row of a
 * replicated table goes to all segments.
 *
 * Triggers and constraints of the table are not run on the coordinator;
 * WHERE, row-level security and ON_ERROR other than stop are refused.
 * The hook stands in place of the previous ProcessUtility hooks for such a
 * COPY.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/copy.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "commands/copy.h"
#include "executor/executor.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"

#include "gp.h"

/*
 * The checks that DoCopy() makes of a COPY FROM: files and programs,
 * privileges on the columns, row-level security
 */
static void
gp_copy_check(ParseState *pstate, CopyStmt *stmt, Relation rel)
{
	TupleDesc	tupDesc = RelationGetDescr(rel);
	ParseNamespaceItem *nsitem;
	RTEPermissionInfo *perminfo;
	CopyFormatOptions opts = {0};
	ListCell   *lc;

	if (stmt->filename != NULL)
	{
		if (stmt->is_program)
		{
			if (!has_privs_of_role(GetUserId(), ROLE_PG_EXECUTE_SERVER_PROGRAM))
				ereport(ERROR,
						errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						errmsg("permission denied to COPY to or from an external program"),
						errdetail("Only roles with privileges of the \"%s\" role may COPY to or from an external program.",
								  "pg_execute_server_program"),
						errhint("Anyone can COPY to stdout or from stdin. "
								"psql's \\copy command also works for anyone."));
		}
		else if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
			ereport(ERROR,
					errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					errmsg("permission denied to COPY from a file"),
					errdetail("Only roles with privileges of the \"%s\" role may COPY from a file.",
							  "pg_read_server_files"),
					errhint("Anyone can COPY to stdout or from stdin. "
							"psql's \\copy command also works for anyone."));
	}

	if (stmt->whereClause != NULL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("COPY FROM with WHERE is not supported on a distributed table"));

	ProcessCopyOptions(pstate, &opts, true, stmt->options);
	if (opts.on_error != COPY_ON_ERROR_STOP)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("COPY FROM with ON_ERROR other than stop is not supported on a distributed table"));

	nsitem = addRangeTableEntryForRelation(pstate, rel, RowExclusiveLock,
										   NULL, false, false);
	perminfo = nsitem->p_perminfo;
	perminfo->requiredPerms = ACL_INSERT;
	foreach(lc, CopyGetAttnums(tupDesc, rel, stmt->attlist))
		perminfo->insertedCols =
			bms_add_member(perminfo->insertedCols,
						   lfirst_int(lc) - FirstLowInvalidHeapAttributeNumber);
	ExecCheckPermissions(pstate->p_rtable, list_make1(perminfo), true);

	if (check_enable_rls(RelationGetRelid(rel), InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("COPY FROM not supported with row-level security"),
				errhint("Use INSERT statements instead."));

	if (XactReadOnly && !rel->rd_islocaltemp)
		PreventCommandIfReadOnly("COPY FROM");
}

/*
 * COPY FROM into a distributed table, as the segments take it.  Returns
 * false for any other COPY, which runs as usual.
 */
bool
gp_copy_from(PlannedStmt *pstmt, const char *queryString, QueryCompletion *qc)
{
	CopyStmt   *stmt = (CopyStmt *) pstmt->utilityStmt;
	Relation	rel;
	GpPolicy	policy;
	ParseState *pstate;
	int			natts;
	CopyFromState cstate;
	EState	   *estate;
	ExprContext *econtext;
	GpRoute    *route;
	Datum	   *values;
	bool	   *nulls;
	uint64		processed = 0;
	ErrorContextCallback errcallback;

	if (!stmt->is_from || stmt->relation == NULL)
		return false;

	rel = table_openrv(stmt->relation, RowExclusiveLock);
	if (!gp_policy_get(RelationGetRelid(rel), &policy))
	{
		table_close(rel, NoLock);
		return false;
	}

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	gp_copy_check(pstate, stmt, rel);

	natts = RelationGetDescr(rel)->natts;

	cstate = BeginCopyFrom(pstate, rel, NULL, stmt->filename, stmt->is_program,
						   NULL, stmt->attlist, stmt->options);
	route = gp_route_begin(rel, &policy, 0);

	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	values = palloc_array(Datum, natts);
	nulls = palloc_array(bool, natts);

	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	for (;;)
	{
		MemoryContext oldcontext;

		CHECK_FOR_INTERRUPTS();
		ResetPerTupleExprContext(estate);
		oldcontext = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		if (!NextCopyFrom(cstate, econtext, values, nulls))
		{
			MemoryContextSwitchTo(oldcontext);
			break;
		}

		gp_route_row(route, values, nulls);

		MemoryContextSwitchTo(oldcontext);
		processed++;
	}

	error_context_stack = errcallback.previous;

	gp_route_end(route);

	EndCopyFrom(cstate);
	FreeExecutorState(estate);
	free_parsestate(pstate);
	table_close(rel, NoLock);

	if (qc != NULL)
		SetQueryCompletion(qc, CMDTAG_COPY, processed);
	return true;
}
