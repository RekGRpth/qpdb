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
#include "catalog/pg_collation_d.h"
#include "commands/defrem.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "funcapi.h"
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
	Oid			collation = PG_GET_COLLATION();
	uint64		hash = 0;

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
	if (!OidIsValid(collation))
		collation = DEFAULT_COLLATION_OID;

	for (int i = 0; i < nargs; i++)
	{
		uint64		h;

		if (nulls[i])
			continue;
		h = DatumGetUInt64(OidFunctionCall2Coll(gp_hash_proc(types[i]),
												collation, args[i],
												UInt64GetDatum(GP_HASH_SEED)));
		hash = hash_combine64(hash, h);
	}

	PG_RETURN_INT32((int32) (hash % (uint64) nbuckets));
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
