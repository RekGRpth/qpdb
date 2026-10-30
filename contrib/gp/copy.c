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

/* A buffer of a segment is sent when it grows to this size */
#define GP_COPY_FLUSH 65536

/* Header of the binary format: the signature, then no flags and no extension */
static const char binary_signature[11] = "PGCOPY\n\377\r\n\0";

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
	TupleDesc	tupDesc;
	int			natts;
	int			nsend = 0;
	int		   *sendatt;
	FmgrInfo   *sendfn;
	StringInfoData collist;
	Oid		   *keytypes;
	Datum	   *keyvalues;
	bool	   *keynulls;
	char	   *copysql;
	CopyFromState cstate;
	EState	   *estate;
	ExprContext *econtext;
	GpSegment  *segments;
	int			nsegments;
	int		   *bucket_pos = NULL;
	StringInfoData *buffers;
	StringInfoData tuple;
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

	tupDesc = RelationGetDescr(rel);
	natts = tupDesc->natts;

	/* The columns that go to the segments; they compute the generated ones */
	sendatt = palloc_array(int, natts);
	sendfn = palloc_array(FmgrInfo, natts);
	initStringInfo(&collist);
	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupDesc, i);
		Oid			sendoid;
		bool		isvarlena;

		if (att->attisdropped || att->attgenerated)
			continue;
		getTypeBinaryOutputInfo(att->atttypid, &sendoid, &isvarlena);
		fmgr_info(sendoid, &sendfn[nsend]);
		sendatt[nsend++] = i;
		appendStringInfo(&collist, "%s%s", collist.len > 0 ? ", " : "",
						 quote_identifier(NameStr(att->attname)));
	}
	copysql = psprintf("COPY %s (%s) FROM STDIN (FORMAT binary)",
					   quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
												  RelationGetRelationName(rel)),
					   collist.data);

	keytypes = palloc_array(Oid, Max(policy.nkeys, 1));
	keyvalues = palloc_array(Datum, Max(policy.nkeys, 1));
	keynulls = palloc_array(bool, Max(policy.nkeys, 1));
	for (int k = 0; k < policy.nkeys; k++)
		keytypes[k] = TupleDescAttr(tupDesc, policy.keys[k] - 1)->atttypid;

	cstate = BeginCopyFrom(pstate, rel, NULL, stmt->filename, stmt->is_program,
						   NULL, stmt->attlist, stmt->options);

	nsegments = gp_copy_open(copysql, &segments);
	if (nsegments == 0)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("the cluster has no segments"));

	/* The segment of each bucket, as a position in segments */
	if (!policy.replicated)
	{
		bucket_pos = palloc_array(int, policy.nbuckets);
		for (int b = 0; b < policy.nbuckets; b++)
		{
			int			pos = -1;

			for (int i = 0; i < nsegments && pos < 0; i++)
				if (segments[i].segment_id == policy.map[b])
					pos = i;
			if (pos < 0)
				ereport(ERROR,
						errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("bucket %d of table \"%s\" is on segment %d, which is not registered",
							   b, RelationGetRelationName(rel), policy.map[b]));
			bucket_pos[b] = pos;
		}
	}

	buffers = palloc_array(StringInfoData, nsegments);
	for (int i = 0; i < nsegments; i++)
	{
		initStringInfo(&buffers[i]);
		appendBinaryStringInfo(&buffers[i], binary_signature,
							   sizeof(binary_signature));
		pq_sendint32(&buffers[i], 0);	/* flags */
		pq_sendint32(&buffers[i], 0);	/* length of the header extension */
	}
	initStringInfo(&tuple);

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
		int			first;
		int			last;

		CHECK_FOR_INTERRUPTS();
		ResetPerTupleExprContext(estate);
		oldcontext = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		if (!NextCopyFrom(cstate, econtext, values, nulls))
		{
			MemoryContextSwitchTo(oldcontext);
			break;
		}

		/* The row in the binary format */
		resetStringInfo(&tuple);
		pq_sendint16(&tuple, nsend);
		for (int k = 0; k < nsend; k++)
		{
			int			i = sendatt[k];

			if (nulls[i])
				pq_sendint32(&tuple, (uint32) -1);
			else
			{
				bytea	   *data = SendFunctionCall(&sendfn[k], values[i]);

				pq_sendint32(&tuple, VARSIZE_ANY_EXHDR(data));
				pq_sendbytes(&tuple, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data));
			}
		}

		if (policy.replicated)
		{
			first = 0;
			last = nsegments - 1;
		}
		else
		{
			for (int k = 0; k < policy.nkeys; k++)
			{
				keyvalues[k] = values[policy.keys[k] - 1];
				keynulls[k] = nulls[policy.keys[k] - 1];
			}
			first = last = bucket_pos[gp_key_bucket(policy.nbuckets, policy.nkeys,
													keyvalues, keynulls, keytypes,
													InvalidOid)];
		}

		for (int i = first; i <= last; i++)
		{
			appendBinaryStringInfo(&buffers[i], tuple.data, tuple.len);
			if (buffers[i].len >= GP_COPY_FLUSH)
			{
				gp_copy_send(segments[i].segment_id, buffers[i].data,
							 buffers[i].len);
				resetStringInfo(&buffers[i]);
			}
		}

		MemoryContextSwitchTo(oldcontext);
		processed++;
	}

	error_context_stack = errcallback.previous;

	for (int i = 0; i < nsegments; i++)
	{
		pq_sendint16(&buffers[i], (uint16) -1);	/* the trailer */
		gp_copy_send(segments[i].segment_id, buffers[i].data, buffers[i].len);
	}
	gp_copy_close();

	EndCopyFrom(cstate);
	FreeExecutorState(estate);
	free_parsestate(pstate);
	table_close(rel, NoLock);

	if (qc != NULL)
		SetQueryCompletion(qc, CMDTAG_COPY, processed);
	return true;
}
