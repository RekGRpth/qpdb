/* -------------------------------------------------------------------------
 *
 * gp.c
 *		Cluster of PostgreSQL servers: a coordinator and segments
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/gp.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "catalog/catalog.h"
#include "miscadmin.h"
#include "utils/guc.h"

#if !defined(NEW_OID_HOOK_VERSION) || NEW_OID_HOOK_VERSION < 1
#error "gp requires a server with new_oid_hook"
#endif

PG_MODULE_MAGIC_EXT(
					.name = "gp",
					.version = PG_VERSION
);

/* Role of the server in the cluster */
enum
{
	GP_ROLE_NONE,				/* not in a cluster */
	GP_ROLE_COORDINATOR,
	GP_ROLE_SEGMENT,
};

static const struct config_enum_entry gp_role_options[] = {
	{"none", GP_ROLE_NONE, false},
	{"coordinator", GP_ROLE_COORDINATOR, false},
	{"segment", GP_ROLE_SEGMENT, false},
	{NULL, 0, false}
};

/* GUC Variables */
static int	gp_role = GP_ROLE_NONE;
static int	gp_segment_id = 0;
static char *gp_database = NULL;

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* The role is set at server start, so the hooks come only from there */
	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomEnumVariable("gp.role",
							 "Role of the server in the cluster.",
							 "With none, the server works on its own.",
							 &gp_role,
							 GP_ROLE_NONE,
							 gp_role_options,
							 PGC_POSTMASTER,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("gp.segment_id",
							"Number of the server in the cluster.",
							"0 on the coordinator, from 1 on segments.",
							&gp_segment_id,
							0,
							0,
							INT_MAX,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("gp.database",
							   "Database that holds the configuration of the cluster.",
							   "Segments register in this database of the coordinator.",
							   &gp_database,
							   "postgres",
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);

	MarkGUCPrefixReserved("gp");

	/* A misconfigured node does not start */
	if (gp_role == GP_ROLE_SEGMENT && gp_segment_id == 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("\"gp.segment_id\" must be set on a segment"));
	if (gp_role != GP_ROLE_SEGMENT && gp_segment_id != 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("\"gp.segment_id\" is set, but \"gp.role\" is not \"segment\""));
}
