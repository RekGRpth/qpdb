/*-------------------------------------------------------------------------
 *
 * cdbvars.c
 *	  Provides storage areas and processing routines for Greenplum Database variables
 *	  managed by GUC.
 *
 * Portions Copyright (c) 2003-2010, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/backend/cdb/cdbvars.c
 *
 *
 * NOTES
 *	  See src/backend/utils/misc/guc.c for variable external specification.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/guc.h"

/* GPDB */
#include "cdb/cdbvars.h"

/*
 * ----------------
 *		GUC/global variables
 *
 *	Initial values are set by guc.c function "InitializeGUCOptions" called
 *	*very* early during postmaster, postgres, or bootstrap initialization.
 * ----------------
 */



GpRoleValue Gp_role;			/* Role paid by this Greenplum Database
								 * backend */
char	   *gp_role_string;		/* Staging area for guc.c */

/*
 * Convert a Greenplum Database role string (as for gp_role) to an
 * enum value of type GpRoleValue. Return GP_ROLE_UNDEFINED in case the
 * string is unrecognized.
 */
static GpRoleValue
string_to_role(const char *string)
{
	GpRoleValue role = GP_ROLE_UNDEFINED;

	if (pg_strcasecmp(string, "dispatch") == 0)
		role = GP_ROLE_DISPATCH;
	else if (pg_strcasecmp(string, "execute") == 0)
		role = GP_ROLE_EXECUTE;
	else if (pg_strcasecmp(string, "utility") == 0)
		role = GP_ROLE_UTILITY;

	return role;
}

/*
 * Convert a GpRoleValue to a role string (as for gp_role).  Return eyecatcher
 * in the unexpected event that the value is unknown or undefined.
 */
const char *
role_to_string(GpRoleValue role)
{
	switch (role)
	{
		case GP_ROLE_DISPATCH:
			return "dispatch";
		case GP_ROLE_EXECUTE:
			return "execute";
		case GP_ROLE_UTILITY:
			return "utility";
		case GP_ROLE_UNDEFINED:
		default:
			return "undefined";
	}
}

bool
check_gp_role(char **newval, void **extra, GucSource source)
{
	GpRoleValue newrole = string_to_role(*newval);

	/* Force utility mode in a stand-alone backend. */
	if (!IsPostmasterEnvironment && newrole != GP_ROLE_UTILITY)
	{
		elog(LOG, "gp_role forced to 'utility' in single-user mode");
		*newval = strdup("utility");
		return true;
	}

	if (source == PGC_S_DEFAULT)
		return true;
	else if (Gp_role == GP_ROLE_UNDEFINED)
		return (newrole != GP_ROLE_UNDEFINED);
	else /* can only downgrade to utility. */
		return (newrole == GP_ROLE_UTILITY);
}

void
assign_gp_role(const char *newval, void *extra)
{
	Gp_role = string_to_role(newval);

	// if (Gp_role == GP_ROLE_UTILITY && MyProc != NULL)
	// 	MyProc->mppIsWriter = false;

	// if (Gp_role == GP_ROLE_UTILITY)
	// 	should_reject_connection = false;
}

/*
 * Show hook routine for "gp_role" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_gp_role(void)
{
	return role_to_string(Gp_role);
}
