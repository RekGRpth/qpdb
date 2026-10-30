/* -------------------------------------------------------------------------
 *
 * distribution.c
 *		Buckets of distribution keys
 *
 * A row of a table distributed by hash goes to a bucket by the hash of its
 * key, and the bucket to a segment by the map of the colocation group of
 * the table.  The hash of a column is the extended hash function of the
 * default hash operator class of its type; the hashes of the columns of a
 * key are combined as hash partitioning does, skipping NULLs.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/distribution.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "common/hashfn.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "gp.h"

/*
 * Seed of the hashes of keys.  It differs from the seed of hash
 * partitioning, so that a table partitioned by hash on its distribution key
 * has each partition on all segments, not on some of them.
 */
#define GP_HASH_SEED UINT64CONST(0x7A5B9C3D1E2F4A6B)

PG_FUNCTION_INFO_V1(gp_bucket);
PG_FUNCTION_INFO_V1(gp_check_distribution_key);

/*
 * The extended hash function of the default hash operator class of a type
 */
static Oid
gp_hash_proc(Oid typid)
{
	Oid			opclass = GetDefaultOpClass(typid, HASH_AM_OID);
	Oid			opcintype;
	Oid			proc;

	if (!OidIsValid(opclass))
		ereport(ERROR,
				errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg("type %s has no default hash operator class",
					   format_type_be(typid)),
				errhint("A distribution key needs types with hash functions."));

	opcintype = get_opclass_input_type(opclass);
	proc = get_opfamily_proc(get_opclass_family(opclass), opcintype, opcintype,
							 HASHEXTENDED_PROC);
	if (!OidIsValid(proc))
		ereport(ERROR,
				errcode(ERRCODE_UNDEFINED_FUNCTION),
				errmsg("type %s has no extended hash function",
					   format_type_be(typid)));
	return proc;
}

/*
 * The bucket of a key among nbuckets: the hashes of its columns combined,
 * skipping NULLs
 */
int
gp_key_bucket(int nbuckets, int nkeys, const Datum *values, const bool *nulls,
			  const Oid *types, Oid collation)
{
	uint64		hash = 0;

	if (!OidIsValid(collation))
		collation = DEFAULT_COLLATION_OID;

	for (int i = 0; i < nkeys; i++)
	{
		uint64		h;

		if (nulls[i])
			continue;
		h = DatumGetUInt64(OidFunctionCall2Coll(gp_hash_proc(types[i]),
												collation, values[i],
												UInt64GetDatum(GP_HASH_SEED)));
		hash = hash_combine64(hash, h);
	}

	return (int) (hash % (uint64) nbuckets);
}

/*
 * gp.bucket(nbuckets, VARIADIC key): the bucket of a key among nbuckets
 */
Datum
gp_bucket(PG_FUNCTION_ARGS)
{
	int32		nbuckets;
	Datum	   *args;
	Oid		   *types;
	bool	   *nulls;
	int			nargs;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	nbuckets = PG_GETARG_INT32(0);
	if (nbuckets <= 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("number of buckets must be positive"));

	nargs = extract_variadic_args(fcinfo, 1, true, &args, &types, &nulls);
	if (nargs < 1)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("a key has at least one column"));

	PG_RETURN_INT32(gp_key_bucket(nbuckets, nargs, args, nulls, types,
								  PG_GET_COLLATION()));
}

/*
 * The policy of a distributed table, in the current memory context; false
 * for a table that is not distributed, or without the extension in the
 * database.  It reads the policy as the superuser, as a user of the table
 * need not read the tables of the extension.
 */
bool
gp_policy_get(Oid relid, GpPolicy *policy)
{
	const char *sql =
		"SELECT p.kind, p.key_columns, g.nbuckets, g.map "
		"FROM gp.distribution_policy p "
		"LEFT JOIN gp.colocation_groups g "
		"ON g.colocation_id OPERATOR(pg_catalog.=) p.colocation_id "
		"WHERE p.relid OPERATOR(pg_catalog.=) $1";
	Oid			argtypes[1] = {REGCLASSOID};
	Datum		args[1] = {ObjectIdGetDatum(relid)};
	MemoryContext callercxt = CurrentMemoryContext;
	Oid			saved_userid;
	int			saved_seccontext;
	bool		found = false;

	if (creating_extension || !OidIsValid(get_extension_oid("gp", true)))
		return false;

	GetUserIdAndSecContext(&saved_userid, &saved_seccontext);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
						   saved_seccontext | SECURITY_LOCAL_USERID_CHANGE);
	SPI_connect();
	if (SPI_execute_with_args(sql, 1, argtypes, args, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not read the distribution policy");

	if (SPI_processed == 1)
	{
		HeapTuple	tuple = SPI_tuptable->vals[0];
		TupleDesc	desc = SPI_tuptable->tupdesc;
		bool		isnull;
		char	   *kind = TextDatumGetCString(SPI_getbinval(tuple, desc, 1, &isnull));

		found = true;
		policy->replicated = strcmp(kind, "replicated") == 0;
		policy->nkeys = 0;
		policy->keys = NULL;
		policy->nbuckets = 0;
		policy->map = NULL;

		if (!policy->replicated)
		{
			Datum	   *elems;
			int			n;

			deconstruct_array_builtin(DatumGetArrayTypeP(SPI_getbinval(tuple, desc, 2, &isnull)),
									  INT2OID, &elems, NULL, &n);
			policy->nkeys = n;
			policy->keys = MemoryContextAlloc(callercxt, sizeof(AttrNumber) * n);
			for (int i = 0; i < n; i++)
				policy->keys[i] = DatumGetInt16(elems[i]);

			policy->nbuckets = DatumGetInt32(SPI_getbinval(tuple, desc, 3, &isnull));
			deconstruct_array_builtin(DatumGetArrayTypeP(SPI_getbinval(tuple, desc, 4, &isnull)),
									  INT4OID, &elems, NULL, &n);
			policy->map = MemoryContextAlloc(callercxt, sizeof(int) * n);
			for (int i = 0; i < n; i++)
				policy->map[i] = DatumGetInt32(elems[i]);
		}
	}

	SPI_finish();
	SetUserIdAndSecContext(saved_userid, saved_seccontext);
	return found;
}

/*
 * gp.check_distribution_key(relation, attnums): fail unless the columns
 * may be a distribution key
 */
Datum
gp_check_distribution_key(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ArrayType  *attnums = PG_GETARG_ARRAYTYPE_P(1);
	Datum	   *elems;
	int			nelems;

	deconstruct_array_builtin(attnums, INT2OID, &elems, NULL, &nelems);
	for (int i = 0; i < nelems; i++)
	{
		AttrNumber	attnum = DatumGetInt16(elems[i]);
		Oid			typid;
		int32		typmod;
		Oid			collation;

		get_atttypetypmodcoll(relid, attnum, &typid, &typmod, &collation);
		(void) gp_hash_proc(typid);
		if (OidIsValid(collation) && !get_collation_isdeterministic(collation))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("column \"%s\" has a nondeterministic collation",
						   get_attname(relid, attnum, false)),
					errdetail("Its hash depends on the collation."));
	}

	PG_RETURN_VOID();
}
