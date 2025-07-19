/*-------------------------------------------------------------------------
 *
 * cdbutil.h
 *	  Header file for routines in cdbutil.c and results returned by
 *	  those routines.
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/include/cdb/cdbutil.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBUTIL_H
#define CDBUTIL_H

/* --------------------------------------------------------------------------------------------------
 * Structure for MPP 2.0 database information
 *
 * The information contained in this structure represents logically a row from
 * gp_configuration.  It is used to describe either an entry
 * database or a segment database.
 *
 * Storage for instances of this structure are palloc'd.  Storage for the values
 * pointed to by non-NULL char*'s are also palloc'd.
 *
 */
#define COMPONENT_DBS_MAX_ADDRS (8)
typedef struct GpSegConfigEntry 
{
	/* copy of entry in gp_segment_configuration */
	int16		dbid;			/* the dbid of this database */
	int16		segindex;		/* content indicator: -1 for entry database,
								 * 0, ..., n-1 for segment database */

	char		role;			/* primary, coordinator, mirror, coordinator-standby */
	char		preferred_role; /* what role would we "like" to have this segment in ? */
	char		mode;
	char		status;
	int32		port;			/* port that instance is listening on */
	char		*hostname;		/* name or ip address of host machine */
	char		*address;		/* ip address of host machine */
	char		*datadir;		/* absolute path to data directory on the host. */

	/* additional cached info */
	char		*hostip;	/* cached lookup of name */
	char		*hostaddrs[COMPONENT_DBS_MAX_ADDRS];	/* cached lookup of names */	
} GpSegConfigEntry;

extern int16 contentid_get_dbid(int16 contentid, char role, bool getPreferredRoleNotCurrentRole);

#endif   /* CDBUTIL_H */
