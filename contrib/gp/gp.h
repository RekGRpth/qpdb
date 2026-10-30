/* -------------------------------------------------------------------------
 *
 * gp.h
 *		Cluster of PostgreSQL servers: a coordinator and segments
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/gp.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef GP_H
#define GP_H

#include "libpq-fe.h"
#include "access/attnum.h"
#include "nodes/plannodes.h"
#include "tcop/cmdtag.h"
#include "utils/guc.h"

/* Role of the server in the cluster */
enum
{
	GP_ROLE_NONE,				/* not in a cluster */
	GP_ROLE_COORDINATOR,
	GP_ROLE_SEGMENT,
};

/* Longest host of a segment, with the terminating zero */
#define GP_HOST_LEN 256

/* A segment as gp.segments has it */
typedef struct GpSegment
{
	int			segment_id;
	int			port;
	char		host[GP_HOST_LEN];
} GpSegment;

/* GUC variables */
extern int	gp_role;
extern int	gp_segment_id;
extern char *gp_database;
extern int	gp_max_segments;

/* segments.c */
extern void gp_segments_init(void);
extern int	gp_segments_get(GpSegment **segments);

/* How the rows of a distributed table are placed */
typedef struct GpPolicy
{
	bool		replicated;
	int			nkeys;
	AttrNumber *keys;			/* key columns */
	int			nbuckets;
	int		   *map;			/* segment of each bucket */
} GpPolicy;

/* distribution.c */
extern bool gp_policy_get(Oid relid, GpPolicy *policy);
extern int	gp_key_bucket(int nbuckets, int nkeys, const Datum *values,
						  const bool *nulls, const Oid *types, Oid collation);

/* copy.c */
extern bool gp_copy_from(PlannedStmt *pstmt, const char *queryString,
						 QueryCompletion *qc);

/* dispatch.c */
extern void gp_dispatch_init(void);
extern PGconn *gp_connection_get(const GpSegment *segment);
extern void gp_dispatch_xact(const char *sql);
extern int	gp_copy_open(const char *sql, GpSegment **segments);
extern void gp_copy_send(int segment_id, const char *data, int len);
extern void gp_copy_close(void);

/* Segments may create objects of their own for a command out of a transaction */
#define GP_NONXACT_LOCAL_DDL 0x01
/* A failure on a segment is a WARNING: the coordinator cannot undo the command */
#define GP_NONXACT_WARN 0x02
extern void gp_dispatch_nonxact(const char *sql, const char *oids,
								const char *undo, int flags);

/* oids.c */
extern char *gp_dispatch_oids;
extern bool gp_dispatch_oids_session;
extern bool gp_allow_local_ddl;
extern void gp_oids_init(void);
extern bool gp_check_dispatch_oids(char **newval, void **extra,
								   GucSource source);
extern void gp_assign_dispatch_oids(const char *newval, void *extra);
extern int	gp_oids_mark(void);
extern void gp_oids_keep(bool keep);
extern char *gp_oids_since(int mark);

/* ddl.c */
extern bool gp_dispatch_ddl;
extern void gp_ddl_init(void);

#endif							/* GP_H */
