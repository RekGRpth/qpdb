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
 * Temporary objects and objects of extension gp stay on the coordinator,
 * where the configuration of the cluster is; the other extensions, which
 * must be installed on the segments, go as a whole: the segments run the
 * script of CREATE EXTENSION themselves, with the OIDs of the coordinator.
 * Roles, which all databases share, go from any database.
 * Statements that compute a column with a non-immutable expression are
 * refused, as each segment would compute its own values.
 *
 * CREATE DATABASE, DROP DATABASE, VACUUM and the CONCURRENTLY commands
 * cannot run in a transaction block, so they run on the segments one by one
 * after the coordinator, each segment in a transaction of its own, and 2PC
 * does not cover them.  If CREATE DATABASE fails on a segment, it is dropped
 * on the others and the coordinator rolls back.  DROP DATABASE has removed
 * the files on the coordinator by then, so a failure on a segment is a
 * WARNING.
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
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "gp.h"

/* GUC variables */
bool		gp_dispatch_ddl = true;

/* How a statement goes to the segments */
typedef enum GpDispatch
{
	GP_DISPATCH_NONE,
	GP_DISPATCH_XACT,			/* in the transaction of the coordinator */
	GP_DISPATCH_NONXACT,		/* after it, in a transaction of its own */
} GpDispatch;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* Depth of dispatched statements being run on the coordinator */
static int	dispatch_depth = 0;

/*
 * Whether an object is a member of extension gp, which is on the coordinator
 * only, unlike the other extensions
 */
static bool
gp_is_member_of_gp(Oid classid, Oid objid)
{
	Oid			ext = getExtensionOfObject(classid, objid);

	return OidIsValid(ext) && ext == get_extension_oid("gp", true);
}

/*
 * Whether a relation stays on the coordinator: a temporary one, or one of
 * extension gp
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
		 gp_is_member_of_gp(RelationRelationId, relid));
}

/*
 * Whether a schema stays on the coordinator: a temporary one, one of
 * extension gp, or the schema of its control file, which CREATE EXTENSION
 * makes depend on the extension, but not a member of it.
 */
static bool
gp_schema_is_local(const char *name)
{
	Oid			nspid = get_namespace_oid(name, true);
	Oid			gp = get_extension_oid("gp", true);

	return OidIsValid(nspid) &&
		(isAnyTempNamespace(nspid) ||
		 gp_is_member_of_gp(NamespaceRelationId, nspid) ||
		 (OidIsValid(gp) && get_extension_schema(gp) == nspid));
}

/*
 * Whether an object, as a statement names it, stays on the coordinator.
 * Objects that do not exist do not: the statement fails anyway.
 */
static bool
gp_object_is_local(ObjectType type, Node *object)
{
	if (object == NULL)
		return false;
	if (IsA(object, RangeVar))
		return gp_relation_is_local((RangeVar *) object);

	switch (type)
	{
		case OBJECT_TABLE:
		case OBJECT_INDEX:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
			return gp_relation_is_local(makeRangeVarFromNameList(castNode(List, object)));
		case OBJECT_COLUMN:
		case OBJECT_TABCONSTRAINT:
			{
				/* The name of the relation, then of the column */
				List	   *names = castNode(List, object);
				List	   *relname = list_copy_head(names, list_length(names) - 1);

				return gp_relation_is_local(makeRangeVarFromNameList(relname));
			}
		case OBJECT_SCHEMA:
			return gp_schema_is_local(strVal(object));
		case OBJECT_EXTENSION:
			return strcmp(strVal(object), "gp") == 0;
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
			{
				TypeName   *typname = IsA(object, TypeName) ? (TypeName *) object :
					makeTypeNameFromNameList(castNode(List, object));
				Oid			typid = LookupTypeNameOid(NULL, typname, true);

				return OidIsValid(typid) &&
					gp_is_member_of_gp(TypeRelationId, typid);
			}
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
			{
				Oid			funcid = LookupFuncWithArgs(type,
														castNode(ObjectWithArgs, object),
														true);

				return OidIsValid(funcid) &&
					gp_is_member_of_gp(ProcedureRelationId, funcid);
			}
		default:
			return false;
	}
}

/*
 * Whether a statement on several objects goes to the segments: none of
 * them stays on the coordinator.  Objects of both kinds are an error.
 */
static bool
gp_objects_dispatched(ObjectType type, List *objects)
{
	int			nlocal = 0;
	ListCell   *lc;

	foreach(lc, objects)
	{
		if (gp_object_is_local(type, lfirst(lc)))
			nlocal++;
	}

	if (nlocal > 0 && nlocal < list_length(objects))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot operate on objects of the coordinator and of the cluster at once"),
				errdetail("Temporary objects and objects of extension gp are on the coordinator only."),
				errhint("Use separate commands."));
	return nlocal == 0;
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
	switch (stmt->removeType)
	{
		case OBJECT_TABLE:
		case OBJECT_INDEX:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_SCHEMA:
		case OBJECT_EXTENSION:
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
			return gp_objects_dispatched(stmt->removeType, stmt->objects);
		default:
			return false;
	}
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
		case OBJECT_EXTENSION:
		case OBJECT_ROLE:
			return true;
		default:
			return false;
	}
}

/*
 * Whether GRANT or REVOKE on objects goes to the segments
 */
static bool
gp_grant_dispatched(GrantStmt *stmt)
{
	switch (stmt->objtype)
	{
		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_SCHEMA:
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
			break;
		case OBJECT_DATABASE:
		case OBJECT_LANGUAGE:
		case OBJECT_PARAMETER_ACL:
			return true;
		default:
			return false;
	}

	/* ALL ... IN SCHEMA names schemas */
	if (stmt->targtype == ACL_TARGET_ALL_IN_SCHEMA)
		return gp_objects_dispatched(OBJECT_SCHEMA, stmt->objects);
	return gp_objects_dispatched(stmt->objtype, stmt->objects);
}

/* Object types that COMMENT and OWNER TO go to the segments for */
static bool
gp_comment_type_dispatched(ObjectType type)
{
	return gp_object_type_dispatched(type) || type == OBJECT_DATABASE;
}

/*
 * Whether VACUUM goes to the segments: not ANALYZE, which is for the
 * statistics of the cluster, and not when all tables stay on the coordinator
 */
static bool
gp_vacuum_dispatched(VacuumStmt *stmt)
{
	List	   *relations = NIL;
	ListCell   *lc;

	if (!stmt->is_vacuumcmd)
		return false;
	if (stmt->rels == NIL)
		return true;

	foreach(lc, stmt->rels)
		relations = lappend(relations, lfirst_node(VacuumRelation, lc)->relation);
	return gp_objects_dispatched(OBJECT_TABLE, relations);
}

/* Whether the options of VACUUM have FULL, which creates relations */
static bool
gp_vacuum_is_full(VacuumStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->options)
	{
		DefElem    *opt = lfirst_node(DefElem, lc);

		if (strcmp(opt->defname, "full") == 0 && defGetBoolean(opt))
			return true;
	}
	return false;
}

/* Whether a statement that runs in a transaction goes to the segments */
static bool
gp_ddl_dispatched_xact(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CreateSeqStmt:
			return !gp_relation_is_local(((CreateSeqStmt *) parsetree)->sequence);
		case T_AlterSeqStmt:
			return !gp_relation_is_local(((AlterSeqStmt *) parsetree)->sequence);
		case T_ViewStmt:
			return !gp_relation_is_local(((ViewStmt *) parsetree)->view);
		case T_RenameStmt:
			{
				RenameStmt *stmt = (RenameStmt *) parsetree;

				if (!gp_object_type_dispatched(stmt->renameType))
					return false;
				if (stmt->relation != NULL)
					return !gp_relation_is_local(stmt->relation);
				if (stmt->renameType == OBJECT_SCHEMA)
					return !gp_schema_is_local(stmt->subname);
				return !gp_object_is_local(stmt->renameType, stmt->object);
			}
		case T_AlterObjectSchemaStmt:
			{
				AlterObjectSchemaStmt *stmt = (AlterObjectSchemaStmt *) parsetree;

				if (!gp_object_type_dispatched(stmt->objectType))
					return false;
				if (stmt->relation != NULL)
					return !gp_relation_is_local(stmt->relation);
				return !gp_object_is_local(stmt->objectType, stmt->object);
			}
		case T_AlterOwnerStmt:
			{
				AlterOwnerStmt *stmt = (AlterOwnerStmt *) parsetree;

				if (!gp_comment_type_dispatched(stmt->objectType))
					return false;
				if (stmt->relation != NULL)
					return !gp_relation_is_local(stmt->relation);
				return !gp_object_is_local(stmt->objectType, stmt->object);
			}
		case T_CommentStmt:
			{
				CommentStmt *stmt = (CommentStmt *) parsetree;

				return gp_comment_type_dispatched(stmt->objtype) &&
					!gp_object_is_local(stmt->objtype, stmt->object);
			}
		case T_GrantStmt:
			return gp_grant_dispatched((GrantStmt *) parsetree);
		case T_CreateExtensionStmt:
			return strcmp(((CreateExtensionStmt *) parsetree)->extname, "gp") != 0;
		case T_AlterExtensionStmt:
			return strcmp(((AlterExtensionStmt *) parsetree)->extname, "gp") != 0;
		case T_AlterExtensionContentsStmt:
			return strcmp(((AlterExtensionContentsStmt *) parsetree)->extname, "gp") != 0;
		case T_CreateSchemaStmt:
		case T_CompositeTypeStmt:
		case T_CreateEnumStmt:
		case T_AlterEnumStmt:
		case T_CreateRangeStmt:
		case T_CreateDomainStmt:
		case T_CreateFunctionStmt:
		case T_AlterFunctionStmt:
		case T_CreateRoleStmt:
		case T_AlterRoleStmt:
		case T_AlterRoleSetStmt:
		case T_DropRoleStmt:
		case T_GrantRoleStmt:
		case T_ReassignOwnedStmt:
		case T_DropOwnedStmt:
		case T_AlterDefaultPrivilegesStmt:
			return true;
		default:
			return false;
	}
}

/*
 * How a statement goes to the segments.  It errors out for statements that
 * should, but cannot.
 */
static GpDispatch
gp_ddl_dispatched(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CreateStmt:
			return gp_relation_is_local(((CreateStmt *) parsetree)->relation) ?
				GP_DISPATCH_NONE : GP_DISPATCH_XACT;
		case T_IndexStmt:
			{
				IndexStmt  *stmt = (IndexStmt *) parsetree;

				if (gp_relation_is_local(stmt->relation))
					return GP_DISPATCH_NONE;
				return stmt->concurrent ? GP_DISPATCH_NONXACT : GP_DISPATCH_XACT;
			}
		case T_AlterTableStmt:
			{
				AlterTableStmt *stmt = (AlterTableStmt *) parsetree;

				if (gp_relation_is_local(stmt->relation))
					return GP_DISPATCH_NONE;
				gp_check_alter_table(stmt);
				return GP_DISPATCH_XACT;
			}
		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;

				if (!gp_drop_dispatched(stmt))
					return GP_DISPATCH_NONE;
				return stmt->concurrent ? GP_DISPATCH_NONXACT : GP_DISPATCH_XACT;
			}
		case T_CreatedbStmt:
		case T_DropdbStmt:
			return GP_DISPATCH_NONXACT;
		case T_VacuumStmt:
			return gp_vacuum_dispatched((VacuumStmt *) parsetree) ?
				GP_DISPATCH_NONXACT : GP_DISPATCH_NONE;
		default:
			return gp_ddl_dispatched_xact(parsetree) ?
				GP_DISPATCH_XACT : GP_DISPATCH_NONE;
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

/*
 * Run a statement that cannot run in a transaction block on the segments,
 * with the OIDs of its objects on the coordinator
 */
static void
gp_dispatch_nonxact_stmt(Node *parsetree, const char *sql, const char *oids)
{
	const char *undo = NULL;
	int			flags = 0;

	switch (nodeTag(parsetree))
	{
		case T_CreatedbStmt:
			undo = psprintf("DROP DATABASE %s",
							quote_identifier(((CreatedbStmt *) parsetree)->dbname));
			break;
		case T_DropdbStmt:
			flags = GP_NONXACT_WARN;
			break;
		case T_VacuumStmt:
			/* The relations of VACUUM FULL are transient: no OIDs to share */
			oids = NULL;
			if (gp_vacuum_is_full((VacuumStmt *) parsetree))
				flags = GP_NONXACT_LOCAL_DDL;
			break;
		default:
			break;
	}

	gp_dispatch_nonxact(sql, oids, undo, flags);
}

static void
gp_process_utility(PlannedStmt *pstmt, const char *queryString,
				   bool readOnlyTree, ProcessUtilityContext context,
				   ParamListInfo params, QueryEnvironment *queryEnv,
				   DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	GpDispatch	mode = GP_DISPATCH_NONE;
	MemoryContext oldcontext = CurrentMemoryContext;
	char	   *oids = NULL;
	int			mark = 0;

	if (gp_dispatch_ddl && dispatch_depth == 0 && !creating_extension &&
		IsUnderPostmaster)
		mode = gp_ddl_dispatched(parsetree);
	if (mode != GP_DISPATCH_NONE)
	{
		mark = gp_oids_mark();
		dispatch_depth++;
	}

	/* The command commits on its own, maybe several times */
	if (mode == GP_DISPATCH_NONXACT)
		gp_oids_keep(true);

	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);

		if (mode == GP_DISPATCH_NONXACT)
		{
			MemoryContextSwitchTo(oldcontext);
			oids = gp_oids_since(mark);
		}
	}
	PG_FINALLY();
	{
		if (mode != GP_DISPATCH_NONE)
			dispatch_depth--;
		if (mode == GP_DISPATCH_NONXACT)
			gp_oids_keep(false);
	}
	PG_END_TRY();

	/* A view of a temporary table is temporary itself */
	if (mode == GP_DISPATCH_XACT && IsA(parsetree, ViewStmt) &&
		gp_relation_is_local(((ViewStmt *) parsetree)->view))
		mode = GP_DISPATCH_NONE;

	if (mode == GP_DISPATCH_XACT)
		gp_dispatch_xact(psprintf("SET LOCAL gp.dispatch_oids = %s; %s",
								  quote_literal_cstr(gp_oids_since(mark)),
								  gp_statement_text(pstmt, queryString)));
	else if (mode == GP_DISPATCH_NONXACT)
		gp_dispatch_nonxact_stmt(parsetree,
								 gp_statement_text(pstmt, queryString), oids);
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
