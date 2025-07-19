/*-------------------------------------------------------------------------
 *
 * segadmin.c
 *	  Functions to support administrative tasks with GPDB segments.
 *
 * Portions Copyright (c) 2010 Greenplum
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/backend/utils/gp/segadmin.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq-fe.h"
#include "miscadmin.h"
#include "pqexpbuffer.h"

#include "access/genam.h"
#include "access/table.h"
#include "catalog/gp_segment_configuration.h"
#include "catalog/pg_proc.h"
#include "catalog/indexing.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "common/hashfn.h"
#include "postmaster/startup.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"

#define COORDINATOR_ONLY 0x1
#define UTILITY_MODE 0x2
#define SUPERUSER 0x4
#define READ_ONLY 0x8
#define SEGMENT_ONLY 0x10
#define STANDBY_ONLY 0x20
#define SINGLE_USER_MODE 0x40

/* Convenience routine to look up the mirror for a given segment index */
static int16
content_get_mirror_dbid(int16 contentid)
{
	return contentid_get_dbid(contentid, GP_SEGMENT_CONFIGURATION_ROLE_MIRROR, false /* false == current, not
							    * preferred, role */ );
}

/* Tell the caller whether a mirror exists at a given segment index */
static bool
segment_has_mirror(int16 contentid)
{
	return content_get_mirror_dbid(contentid) != 0;
}

/*
 * As the function name says, test whether a given dbid is the dbid of the
 * standby coordinator.
 */
static bool
dbid_is_coordinator_standby(int16 dbid)
{
	int16		standbydbid = content_get_mirror_dbid(COORDINATOR_CONTENT_ID);

	return (standbydbid == dbid);
}

/**
 * Get an available dbid value. We AccessExclusiveLock
 * gp_segment_configuration to prevent races but no one should be calling
 * this code concurrently if we've done our job right.
 */
static int16
get_availableDbId()
{
	/*
	 * Set up hash of used dbids.  We use int32 here because int16 doesn't
	 * have a convenient hash and we can use casting below to check for
	 * overflow of int16
	 */
	HASHCTL		hash_ctl;
	HTAB	   *htab;
	Relation	rel;
	HeapTuple	tuple;
	SysScanDesc sscan;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(int32);
	hash_ctl.entrysize = sizeof(int32);
	hash_ctl.hash = int32_hash;
	htab = hash_create("Temporary table of dbids",
								   1024,
								   &hash_ctl,
								   HASH_ELEM | HASH_FUNCTION);

	/* scan GpSegmentConfigRelationId */
	rel = table_open(GpSegmentConfigRelationId, AccessExclusiveLock);

	sscan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while ((tuple = systable_getnext(sscan)) != NULL)
	{
		int32		dbid = (int32) ((Form_gp_segment_configuration) GETSTRUCT(tuple))->dbid;

		(void) hash_search(htab, (void *) &dbid, HASH_ENTER, NULL);
	}
	systable_endscan(sscan);

	table_close(rel, NoLock);

	/* search for available dbid */
	for (int32 dbid = 1;; dbid++)
	{
		if (dbid != (int16) dbid)
			elog(ERROR, "unable to find available dbid");

		if (hash_search(htab, (void *) &dbid, HASH_FIND, NULL) == NULL)
		{
			hash_destroy(htab);
			return dbid;
		}
	}
}

/*
 * Get the highest contentid defined in the system. As above, we
 * AccessExclusiveLock gp_segment_configuration to prevent races but no
 * one should be calling this code concurrently if we've done our job right.
 */
static int16
get_maxcontentid()
{
	Relation	rel = table_open(GpSegmentConfigRelationId, AccessExclusiveLock);
	int16		contentid = 0;
	HeapTuple	tuple;
	SysScanDesc sscan;

	sscan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while ((tuple = systable_getnext(sscan)) != NULL)
	{
		contentid = Max(contentid,
						((Form_gp_segment_configuration) GETSTRUCT(tuple))->content);
	}
	systable_endscan(sscan);
	table_close(rel, NoLock);

	return contentid;
}

/*
 * Check that the code is being called in right context.
 */
static void
mirroring_sanity_check(int flags, const char *func)
{
	if ((flags & COORDINATOR_ONLY) == COORDINATOR_ONLY)
	{
		if (GpIdentity.dbid == UNINITIALIZED_GP_IDENTITY_VALUE)
			elog(ERROR, "%s requires valid GpIdentity dbid", func);

		if (!IS_QUERY_DISPATCHER())
			elog(ERROR, "%s must be run on the coordinator", func);
	}

	if ((flags & UTILITY_MODE) == UTILITY_MODE)
	{
		if (Gp_role != GP_ROLE_UTILITY)
			elog(ERROR, "%s must be run in utility mode", func);
	}

	if ((flags & SINGLE_USER_MODE) == SINGLE_USER_MODE)
	{
		if (IsUnderPostmaster)
			elog(ERROR, "%s must be run in single-user mode", func);
	}

	if ((flags & SUPERUSER) == SUPERUSER)
	{
		if (!superuser())
			elog(ERROR, "%s can only be run by a superuser", func);
	}

	if ((flags & SEGMENT_ONLY) == SEGMENT_ONLY)
	{
		if (GpIdentity.dbid == UNINITIALIZED_GP_IDENTITY_VALUE)
			elog(ERROR, "%s requires valid GpIdentity dbid", func);

		if (IS_QUERY_DISPATCHER())
			elog(ERROR, "%s cannot be run on the coordinator", func);
	}

	if ((flags & STANDBY_ONLY) == STANDBY_ONLY)
	{
		if (GpIdentity.dbid == UNINITIALIZED_GP_IDENTITY_VALUE)
			elog(ERROR, "%s requires valid GpIdentity dbid", func);

		if (!dbid_is_coordinator_standby(GpIdentity.dbid))
			elog(ERROR, "%s can only be run on the standby coordinator", func);
	}
}

/*
 * Add a new row to gp_segment_configuration.
 */
static void
add_segment_config(GpSegConfigEntry *i)
{
	Relation	rel = table_open(GpSegmentConfigRelationId, AccessExclusiveLock);
	Datum		values[Natts_gp_segment_configuration];
	bool		nulls[Natts_gp_segment_configuration];
	HeapTuple	tuple;

	MemSet(nulls, false, sizeof(nulls));

	values[Anum_gp_segment_configuration_dbid - 1] = Int16GetDatum(i->dbid);
	values[Anum_gp_segment_configuration_content - 1] = Int16GetDatum(i->segindex);
	values[Anum_gp_segment_configuration_role - 1] = CharGetDatum(i->role);
	values[Anum_gp_segment_configuration_preferred_role - 1] =
		CharGetDatum(i->preferred_role);
	values[Anum_gp_segment_configuration_mode - 1] =
		CharGetDatum(i->mode);
	values[Anum_gp_segment_configuration_status - 1] =
		CharGetDatum(i->status);
	values[Anum_gp_segment_configuration_port - 1] =
		Int32GetDatum(i->port);
	values[Anum_gp_segment_configuration_hostname - 1] =
		CStringGetTextDatum(i->hostname);
	values[Anum_gp_segment_configuration_address - 1] =
		CStringGetTextDatum(i->address);
	values[Anum_gp_segment_configuration_datadir - 1] =
		CStringGetTextDatum(i->datadir);

	tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* insert a new tuple */
	CatalogTupleInsert(rel, tuple);

	table_close(rel, NoLock);
}

/*
 * Coordinator function for adding a new segment
 */
static void
add_segment_config_entry(GpSegConfigEntry *i)
{
	/* Add gp_segment_configuration entry */
	add_segment_config(i);
}

static void
add_segment(GpSegConfigEntry *new_segment_information)
{
	int16		primary_dbid = new_segment_information->dbid;

	if (new_segment_information->role == GP_SEGMENT_CONFIGURATION_ROLE_MIRROR)
	{
		int			preferredPrimaryDbId;

		primary_dbid = contentid_get_dbid(new_segment_information->segindex, GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY, false	/* false == current, not
										    * preferred, role */ );
		if (!primary_dbid)
			elog(ERROR, "contentid %i does not point to an existing segment",
				 new_segment_information->segindex);

		/*
		 * no mirrors should be defined
		 */
		if (segment_has_mirror(new_segment_information->segindex))
			elog(ERROR, "segment already has a mirror defined");

		/*
		 * figure out if the preferred role of this mirror needs to be primary
		 * or mirror (no preferred primary -- make this one the preferred
		 * primary)
		 */
		preferredPrimaryDbId = contentid_get_dbid(new_segment_information->segindex, GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY, true /* preferred role */ );

		if (preferredPrimaryDbId == 0 && new_segment_information->preferred_role == GP_SEGMENT_CONFIGURATION_ROLE_MIRROR)
		{
			elog(NOTICE, "override preferred_role of this mirror as primary to support rebalance operation.");
			new_segment_information->preferred_role = GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY;
		}
	}

	add_segment_config_entry(new_segment_information);
}

/*
 * Tell the coordinator about a new primary segment.
 *
 * gp_add_segment_primary(hostname, address, port)
 *
 * Args:
 *   hostname - host name string
 *   address - either hostname or something else
 *   port - port number
 *   datadir - absolute path to primary data directory.
 *
 * Returns the dbid of the new segment.
 */
Datum
gp_add_segment_primary(PG_FUNCTION_ARGS)
{
	GpSegConfigEntry	new;

	MemSet(&new, 0, sizeof(GpSegConfigEntry));

	if (PG_ARGISNULL(0))
		elog(ERROR, "hostname cannot be NULL");
	new.hostname = TextDatumGetCString(PG_GETARG_DATUM(0));

	if (PG_ARGISNULL(1))
		elog(ERROR, "address cannot be NULL");
	new.address = TextDatumGetCString(PG_GETARG_DATUM(1));

	if (PG_ARGISNULL(2))
		elog(ERROR, "port cannot be NULL");
	new.port = PG_GETARG_INT32(2);

	if (PG_ARGISNULL(3))
		elog(ERROR, "datadir cannot be NULL");
	new.datadir = TextDatumGetCString(PG_GETARG_DATUM(3));

	mirroring_sanity_check(COORDINATOR_ONLY | SUPERUSER, "gp_add_segment_primary");

	new.segindex = get_maxcontentid() + 1;
	new.dbid = get_availableDbId();
	new.role = GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY;
	new.preferred_role = GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY;
	new.mode = GP_SEGMENT_CONFIGURATION_MODE_NOTINSYNC;
	new.status = GP_SEGMENT_CONFIGURATION_STATUS_UP;

	add_segment(&new);

	PG_RETURN_INT16(new.dbid);
}

/*
 * Currently this is called by `gpinitsystem`.
 *
 * This method shouldn't be called at all. `gp_add_segment_primary()` and
 * `gp_add_segment_mirror()` should be used instead. This is to avoid setting
 * character values of role, preferred_role, mode, status, etc. outside database.
 */
Datum
gp_add_segment(PG_FUNCTION_ARGS)
{
	GpSegConfigEntry new;

	MemSet(&new, 0, sizeof(GpSegConfigEntry));

	if (PG_ARGISNULL(0))
		elog(ERROR, "dbid cannot be NULL");
	new.dbid = PG_GETARG_INT16(0);

	if (PG_ARGISNULL(1))
		elog(ERROR, "content cannot be NULL");
	new.segindex = PG_GETARG_INT16(1);

	if (PG_ARGISNULL(2))
		elog(ERROR, "role cannot be NULL");
	new.role = PG_GETARG_CHAR(2);

	if (PG_ARGISNULL(3))
		elog(ERROR, "preferred_role cannot be NULL");
	new.preferred_role = PG_GETARG_CHAR(3);

	if (PG_ARGISNULL(4))
		elog(ERROR, "mode cannot be NULL");
	new.mode = PG_GETARG_CHAR(4);

	if (PG_ARGISNULL(5))
		elog(ERROR, "status cannot be NULL");
	new.status = PG_GETARG_CHAR(5);

	if (PG_ARGISNULL(6))
		elog(ERROR, "port cannot be NULL");
	new.port = PG_GETARG_INT32(6);

	if (PG_ARGISNULL(7))
		elog(ERROR, "hostname cannot be NULL");
	new.hostname = TextDatumGetCString(PG_GETARG_DATUM(7));

	if (PG_ARGISNULL(8))
		elog(ERROR, "address cannot be NULL");
	new.address = TextDatumGetCString(PG_GETARG_DATUM(8));

	if (PG_ARGISNULL(9))
		elog(ERROR, "datadir cannot be NULL");
	new.datadir = TextDatumGetCString(PG_GETARG_DATUM(9));

	mirroring_sanity_check(COORDINATOR_ONLY | SUPERUSER, "gp_add_segment");

	new.mode = GP_SEGMENT_CONFIGURATION_MODE_NOTINSYNC;
	elog(NOTICE, "mode is changed to GP_SEGMENT_CONFIGURATION_MODE_NOTINSYNC under walrep.");

	add_segment(&new);

	PG_RETURN_INT16(new.dbid);
}


