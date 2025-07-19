/*-------------------------------------------------------------------------
 *
 * cdbvars.h
 *	  definitions for Greenplum-specific global variables
 *
 * Portions Copyright (c) 2003-2010, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/include/cdb/cdbvars.h
 *
 * NOTES
 *	  See src/backend/utils/misc/guc_gp.c for variable external specification.
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBVARS_H
#define CDBVARS_H

/*
 * Parameters gp_role
 *
 * The run-time parameters (GUC variables) gp_role
 * reports and provides control over the role assumed by a
 * postgres process.
 *
 * Valid  roles are the following:
 *
 *	dispatch	The process acts as a parallel SQL dispatcher.
 *	execute		The process acts as a parallel SQL executor.
 *	utility		The process acts as a simple SQL engine.
 *
 * For postmaster, the parameter is initialized by '-c' (required).
 *
 * For normal connections to cluster, you can connect to QD directly,
 * but can not connect to QE directly unless specifying the utility role.
 *
 * For utility role connection to either QD or QE, PGOPTIONS could be used.
 * Alternatively, libpq-based applications can be modified to set
 * the value directly via an argument to one of the libpq functions
 * PQconnectdb, PQsetdbLogin or PQconnectStart.
 *
 * For single backend connection, utility role is enforced in code.
 */

typedef enum
{
	GP_ROLE_UNDEFINED = 0,		/* Should never see this role in use */
	GP_ROLE_UTILITY,			/* Operating as a simple database engine */
	GP_ROLE_DISPATCH,			/* Operating as the parallel query dispatcher */
	GP_ROLE_EXECUTE,			/* Operating as a parallel query executor */
} GpRoleValue;

extern GpRoleValue Gp_role;	/* GUC var - server operating mode.  */
extern char *gp_role_string;	/* Use by guc.c as staging area for value. */

extern const char *role_to_string(GpRoleValue role);

#endif   /* CDBVARS_H */
