/* -------------------------------------------------------------------------
 *
 * ddl.c
 *		DDL of the coordinator on the segments
 *
 * A DDL statement on the coordinator runs there first, recording the OIDs
 * of its new catalog rows; then the segments run the same statement, its
 * own text cut out of the query string, with these OIDs in
 * gp.dispatch_oids.  Statements nested in a dispatched one, and those of
 * extension scripts, are not sent on their own: the segments run them as
 * part of the outer statement.
 *
 * Only the statements listed in gp_ddl_dispatched() go to the segments.
 * Temporary objects and objects of extensions stay on the coordinator.
 * Statements that compute a column with a non-immutable expression are
 * refused, as each segment would compute its own values.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/ddl.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "commands/extension.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "gp.h"

/* GUC variables */
bool		gp_dispatch_ddl = true;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* Depth of dispatched statements being run on the coordinator */
static int	dispatch_depth = 0;

static void
gp_ddl_unsupported(const char *what)
{
	ereport(ERROR,
			errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("%s is not supported on a cluster yet", what));
}

/*
 * Whether a relation stays on the coordinator: a temporary one, or one of
 * an extension
 */
static bool
gp_relation_is_local(const RangeVar *rv)
{
	Oid			relid;

	if (rv->relpersistence == RELPERSISTENCE_TEMP ||
		(rv->schemaname != NULL && strncmp(rv->schemaname, "pg_temp", 7) == 0))
		return true;

	relid = RangeVarGetRelid(rv, NoLock, true);
	return OidIsValid(relid) &&
		(get_rel_persistence(relid) == RELPERSISTENCE_TEMP ||
		 OidIsValid(getExtensionOfObject(RelationRelationId, relid)));
}

/*
 * Refuse an expression that is not immutable, as each segment would
 * compute it on its own
 */
static void
gp_check_immutable(Relation rel, Node *raw, ParseExprKind kind,
				   const char *column)
{
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	Node	   *expr;

	if (raw == NULL)
		return;

	pstate = make_parsestate(NULL);
	nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
										   NULL, false, true);
	addNSItemToQuery(pstate, nsitem, false, true, true);
	expr = transformExpr(pstate, copyObject(raw), kind);
	if (contain_mutable_functions(expr))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot compute column \"%s\" with a non-immutable expression on a cluster",
					   column),
				errdetail("Each segment would compute the expression on its own."),
				errhint("Use a constant, or add the column without a default and fill it with UPDATE."));
	free_parsestate(pstate);
}

/* Whether a type name is one of the serial types */
static bool
gp_is_serial(const TypeName *typname)
{
	const char *name;

	if (typname == NULL || list_length(typname->names) != 1 ||
		typname->pct_type)
		return false;
	name = strVal(linitial(typname->names));
	return strcmp(name, "smallserial") == 0 || strcmp(name, "serial2") == 0 ||
		strcmp(name, "serial") == 0 || strcmp(name, "serial4") == 0 ||
		strcmp(name, "bigserial") == 0 || strcmp(name, "serial8") == 0;
}

/*
 * Refuse ALTER TABLE that computes a column with a non-immutable
 * expression: ADD COLUMN with such a default, a serial or an identity, and
 * ALTER COLUMN TYPE with such a USING
 */
static void
gp_check_alter_table(AlterTableStmt *stmt)
{
	bool		computes = false;
	Oid			relid;
	Relation	rel;
	ListCell   *lc;

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		if (cmd->subtype == AT_AddColumn || cmd->subtype == AT_AlterColumnType)
			computes = true;
	}
	if (!computes)
		return;

	/* The lock of ALTER TABLE itself, lest it be upgraded later */
	relid = RangeVarGetRelid(stmt->relation,
							 AlterTableGetLockLevel(stmt->cmds),
							 stmt->missing_ok);
	if (!OidIsValid(relid))
		return;
	rel = relation_open(relid, NoLock);

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);
		ColumnDef  *def = (ColumnDef *) cmd->def;
		ListCell   *lc2;

		if (cmd->subtype == AT_AddColumn)
		{
			if (gp_is_serial(def->typeName))
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot add a serial column on a cluster"),
						errdetail("Each segment would number its rows on its own."));
			gp_check_immutable(rel, def->raw_default,
							   EXPR_KIND_COLUMN_DEFAULT, def->colname);
			foreach(lc2, def->constraints)
			{
				Constraint *con = lfirst_node(Constraint, lc2);

				if (con->contype == CONSTR_DEFAULT)
					gp_check_immutable(rel, con->raw_expr,
									   EXPR_KIND_COLUMN_DEFAULT, def->colname);
				else if (con->contype == CONSTR_IDENTITY)
					ereport(ERROR,
							errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot add an identity column on a cluster"),
							errdetail("Each segment would number its rows on its own."));
			}
		}
		else if (cmd->subtype == AT_AlterColumnType)
			gp_check_immutable(rel, def->raw_default,
							   EXPR_KIND_ALTER_COL_TRANSFORM, cmd->name);
	}

	relation_close(rel, NoLock);
}

/*
 * Whether DROP goes to the segments
 */
static bool
gp_drop_dispatched(DropStmt *stmt)
{
	int			nlocal = 0;
	ListCell   *lc;

	switch (stmt->removeType)
	{
		case OBJECT_TABLE:
		case OBJECT_INDEX:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
			if (stmt->concurrent)
				gp_ddl_unsupported("DROP INDEX CONCURRENTLY");
			foreach(lc, stmt->objects)
			{
				if (gp_relation_is_local(makeRangeVarFromNameList(lfirst(lc))))
					nlocal++;
			}
			break;
		case OBJECT_SCHEMA:
			foreach(lc, stmt->objects)
			{
				Oid			nspid = get_namespace_oid(strVal(lfirst(lc)), true);

				if (OidIsValid(nspid) &&
					OidIsValid(getExtensionOfObject(NamespaceRelationId, nspid)))
					nlocal++;
			}
			break;
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
			return true;
		default:
			return false;
	}

	if (nlocal > 0 && nlocal < list_length(stmt->objects))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot drop objects of the coordinator and of the cluster at once"),
				errdetail("Temporary objects and objects of extensions are on the coordinator only."),
				errhint("Drop them in separate commands."));
	return nlocal == 0;
}

/* Object types that rename and set schema go to the segments for */
static bool
gp_object_type_dispatched(ObjectType type)
{
	switch (type)
	{
		case OBJECT_TABLE:
		case OBJECT_INDEX:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_COLUMN:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
		case OBJECT_ATTRIBUTE:
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
		case OBJECT_SCHEMA:
			return true;
		default:
			return false;
	}
}

/*
 * Whether a statement goes to the segments.  It errors out for statements
 * that should, but cannot.
 */
static bool
gp_ddl_dispatched(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CreateStmt:
			return !gp_relation_is_local(((CreateStmt *) parsetree)->relation);
		case T_IndexStmt:
			{
				IndexStmt  *stmt = (IndexStmt *) parsetree;

				if (gp_relation_is_local(stmt->relation))
					return false;
				if (stmt->concurrent)
					gp_ddl_unsupported("CREATE INDEX CONCURRENTLY");
				return true;
			}
		case T_AlterTableStmt:
			{
				AlterTableStmt *stmt = (AlterTableStmt *) parsetree;

				if (gp_relation_is_local(stmt->relation))
					return false;
				gp_check_alter_table(stmt);
				return true;
			}
		case T_DropStmt:
			return gp_drop_dispatched((DropStmt *) parsetree);
		case T_CreateSeqStmt:
			return !gp_relation_is_local(((CreateSeqStmt *) parsetree)->sequence);
		case T_AlterSeqStmt:
			return !gp_relation_is_local(((AlterSeqStmt *) parsetree)->sequence);
		case T_ViewStmt:
			return !gp_relation_is_local(((ViewStmt *) parsetree)->view);
		case T_RenameStmt:
			{
				RenameStmt *stmt = (RenameStmt *) parsetree;

				return gp_object_type_dispatched(stmt->renameType) &&
					(stmt->relation == NULL ||
					 !gp_relation_is_local(stmt->relation));
			}
		case T_AlterObjectSchemaStmt:
			{
				AlterObjectSchemaStmt *stmt = (AlterObjectSchemaStmt *) parsetree;

				return gp_object_type_dispatched(stmt->objectType) &&
					(stmt->relation == NULL ||
					 !gp_relation_is_local(stmt->relation));
			}
		case T_CreateSchemaStmt:
		case T_CompositeTypeStmt:
		case T_CreateEnumStmt:
		case T_AlterEnumStmt:
		case T_CreateRangeStmt:
		case T_CreateDomainStmt:
		case T_CreateFunctionStmt:
		case T_AlterFunctionStmt:
			return true;
		default:
			return false;
	}
}

/* The text of the statement, cut out of the query string */
static char *
gp_statement_text(PlannedStmt *pstmt, const char *queryString)
{
	int			location = pstmt->stmt_location;
	int			len = pstmt->stmt_len;

	if (location < 0)
	{
		/* Unknown: the whole string */
		location = 0;
		len = 0;
	}
	if (len <= 0)
		len = strlen(queryString + location);
	return pnstrdup(queryString + location, len);
}

static void
gp_process_utility(PlannedStmt *pstmt, const char *queryString,
				   bool readOnlyTree, ProcessUtilityContext context,
				   ParamListInfo params, QueryEnvironment *queryEnv,
				   DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	bool		dispatch = false;
	int			mark = 0;

	if (gp_dispatch_ddl && dispatch_depth == 0 && !creating_extension &&
		IsUnderPostmaster)
		dispatch = gp_ddl_dispatched(parsetree);
	if (dispatch)
	{
		mark = gp_oids_mark();
		dispatch_depth++;
	}

	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		if (dispatch)
			dispatch_depth--;
	}
	PG_END_TRY();

	/* A view of a temporary table is temporary itself */
	if (dispatch && IsA(parsetree, ViewStmt) &&
		gp_relation_is_local(((ViewStmt *) parsetree)->view))
		dispatch = false;

	if (dispatch)
		gp_dispatch_xact(psprintf("SET LOCAL gp.dispatch_oids = %s; %s",
								  quote_literal_cstr(gp_oids_since(mark)),
								  gp_statement_text(pstmt, queryString)));
}

/*
 * Install the hook on the coordinator; called from _PG_init()
 */
void
gp_ddl_init(void)
{
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_process_utility;
}
