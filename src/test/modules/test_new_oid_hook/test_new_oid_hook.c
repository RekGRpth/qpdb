/*--------------------------------------------------------------------------
 *
 * test_new_oid_hook.c
 *		Code for testing new_oid_hook.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_new_oid_hook/test_new_oid_hook.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/catalog.h"
#include "fmgr.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

/* The OID to propose once, for a new row of this catalog */
static Oid	proposed_catalog = InvalidOid;
static Oid	proposed_oid = InvalidOid;

static new_oid_hook_type prev_new_oid_hook = NULL;

static Oid
test_new_oid(Relation relation, Oid indexId, AttrNumber oidcolumn)
{
	if (OidIsValid(proposed_oid) &&
		RelationGetRelid(relation) == proposed_catalog)
	{
		Oid			result = proposed_oid;

		proposed_oid = InvalidOid;
		return result;
	}

	if (prev_new_oid_hook)
		return prev_new_oid_hook(relation, indexId, oidcolumn);

	return InvalidOid;
}

PG_FUNCTION_INFO_V1(test_new_oid_hook_propose);
Datum
test_new_oid_hook_propose(PG_FUNCTION_ARGS)
{
	proposed_catalog = PG_GETARG_OID(0);
	proposed_oid = PG_GETARG_OID(1);

	PG_RETURN_VOID();
}

void
_PG_init(void)
{
	prev_new_oid_hook = new_oid_hook;
	new_oid_hook = test_new_oid;
}
