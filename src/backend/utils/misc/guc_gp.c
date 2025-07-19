/*--------------------------------------------------------------------
 * guc_gp.c
 *
 * Additional Greenplum-specific GUCs are defined in this file, to
 * avoid adding so much stuff to guc.c. This makes it easier to diff
 * and merge with upstream.
 *
 * Portions Copyright (c) 2005-2010, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Copyright (c) 2000-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/guc_gp.c
 *
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/guc_tables.h"

struct config_bool ConfigureNamesBool_gp[] =
{

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, false, NULL, NULL
	}
};

struct config_int ConfigureNamesInt_gp[] =
{

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0, 0, 0, NULL, NULL
	}
};

struct config_real ConfigureNamesReal_gp[] =
{

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0.0, 0.0, 0.0, NULL, NULL
	}
};

struct config_string ConfigureNamesString_gp[] =
{

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, NULL, NULL, NULL
	}
};

struct config_enum ConfigureNamesEnum_gp[] =
{

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0, NULL, NULL, NULL
	}
};

