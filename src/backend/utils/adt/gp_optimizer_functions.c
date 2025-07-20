/*
 * gp_optimizer_functions.c
 *    Defines builtin transformation functions for the optimizer.
 *
 * enable_xform: This function wraps EnableXform.
 *
 * disable_xform: This function wraps DisableXform.
 *
 * gp_opt_version: This function wraps LibraryVersion. 
 *
 * Copyright(c) 2012 - present, EMC/Greenplum
 */

#include "postgres.h"

#include "funcapi.h"
#include "utils/builtins.h"

/*
* Returns the optimizer and gpos library versions.
*/
Datum
gp_opt_version(PG_FUNCTION_ARGS pg_attribute_unused())
{
	return CStringGetTextDatum("Server has been compiled without ORCA");
}
