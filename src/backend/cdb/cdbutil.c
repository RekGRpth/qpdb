/*-------------------------------------------------------------------------
 *
 * cdbutil.c
 *	  Internal utility support functions for Greenplum Database/PostgreSQL.
 *
 * Portions Copyright (c) 2005-2011, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/backend/cdb/cdbutil.c
 *
 * NOTES
 *
 *	- According to src/backend/executor/execHeapScan.c
 *		"tuples returned by heap_getnext() are pointers onto disk
 *		pages and were not created with palloc() and so should not
 *		be pfree()'d"
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include <sys/param.h>			/* for MAXHOSTNAMELEN */

#include "access/genam.h"
#include "access/table.h"
#include "catalog/gp_segment_configuration.h"
#include "common/ip.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/memutils.h"
#include "catalog/gp_id.h"
#include "catalog/indexing.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "postmaster/postmaster.h"
#include "catalog/namespace.h"
#include "access/xact.h"

/*
 * Obtain the dbid of a of a segment at a given segment index (i.e., content id)
 * currently fulfilling the role specified. This means that the segment is
 * really performing the role of primary or mirror, irrespective of their
 * preferred role.
 */
int16
contentid_get_dbid(int16 contentid, char role, bool getPreferredRoleNotCurrentRole)
{
	int16		dbid = 0;
	Relation	rel;
	ScanKeyData scankey[2];
	SysScanDesc scan;
	HeapTuple	tup;

	/*
	 * Can only run on a coordinator node, this restriction is due to the reliance
	 * on the gp_segment_configuration table.  This may be able to be relaxed
	 * by switching to a different method of checking.
	 */
	if (!IS_QUERY_DISPATCHER())
		elog(ERROR, "contentid_get_dbid() executed on execution segment");

	rel = table_open(GpSegmentConfigRelationId, AccessShareLock);

	/* XXX XXX: CHECK THIS  XXX jic 2011/12/09 */
	if (getPreferredRoleNotCurrentRole)
	{
		/*
		 * SELECT * FROM gp_segment_configuration WHERE content = :1 AND
		 * preferred_role = :2
		 */
		ScanKeyInit(&scankey[0],
					Anum_gp_segment_configuration_content,
					BTEqualStrategyNumber, F_INT2EQ,
					Int16GetDatum(contentid));
		ScanKeyInit(&scankey[1],
					Anum_gp_segment_configuration_preferred_role,
					BTEqualStrategyNumber, F_CHAREQ,
					CharGetDatum(role));
		scan = systable_beginscan(rel, GpSegmentConfigContentPreferred_roleIndexId, true,
								  NULL, 2, scankey);
	}
	else
	{
		/*
		 * SELECT * FROM gp_segment_configuration WHERE content = :1 AND role
		 * = :2
		 */
		ScanKeyInit(&scankey[0],
					Anum_gp_segment_configuration_content,
					BTEqualStrategyNumber, F_INT2EQ,
					Int16GetDatum(contentid));
		ScanKeyInit(&scankey[1],
					Anum_gp_segment_configuration_role,
					BTEqualStrategyNumber, F_CHAREQ,
					CharGetDatum(role));
		/* no index */
		scan = systable_beginscan(rel, InvalidOid, false,
								  NULL, 2, scankey);
	}

	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
	{
		dbid = ((Form_gp_segment_configuration) GETSTRUCT(tup))->dbid;
		/* We expect a single result, assert this */
		Assert(systable_getnext(scan) == NULL); /* should be only 1 */
	}

	/* no need to hold the lock, it's a catalog */
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return dbid;
}

