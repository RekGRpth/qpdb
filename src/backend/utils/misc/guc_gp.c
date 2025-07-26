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

#include <limits.h>

#include "cdb/cdbvars.h"
#include "utils/guc_tables.h"

static bool check_dispatch_log_stats(bool *newval, void **extra, GucSource source);

bool		Debug_print_full_dtm = false;
bool		Debug_cancel_print = false;

bool		log_dispatch_stats = false;

/* Optimizer related gucs */
bool		optimizer;

static bool
check_optimizer(bool *newval, void **extra, GucSource source)
{
	GUC_check_errmsg("ORCA is not supported by this build");
	return true;
}

struct config_bool ConfigureNamesBool_gp[] =
{
	{
		{"log_dispatch_stats", PGC_SUSET, STATS_MONITORING,
			gettext_noop("Writes dispatcher performance statistics to the server log."),
			NULL
		},
		&log_dispatch_stats,
		false,
		check_dispatch_log_stats, NULL, NULL
	},

	{
		{"debug_print_full_dtm", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Prints full DTM information to server log."),
			NULL,
			GUC_SUPERUSER_ONLY | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE
		},
		&Debug_print_full_dtm,
		false,
		NULL, NULL, NULL
	},

	{
		{"debug_cancel_print", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Print cancel detail information."),
			NULL,
			GUC_SUPERUSER_ONLY | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE
		},
		&Debug_cancel_print,
		false,
		NULL, NULL, NULL
	},

	{
		{"optimizer", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable GPORCA."),
			NULL
		},
		&optimizer,
		false,
		check_optimizer, NULL, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, false, NULL, NULL
	}
};

struct config_int ConfigureNamesInt_gp[] =
{
	{
		{"gp_dbid", PGC_POSTMASTER, PRESET_OPTIONS,
			gettext_noop("The dbid used by this server."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&GpIdentity.dbid,
		UNINITIALIZED_GP_IDENTITY_VALUE, INT_MIN, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"gp_contentid", PGC_POSTMASTER, PRESET_OPTIONS,
			gettext_noop("The contentid used by this server."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&GpIdentity.segindex,
		UNINITIALIZED_GP_IDENTITY_VALUE, INT_MIN, INT_MAX,
		NULL, NULL, NULL
	},

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
	{
		{"gp_role", PGC_BACKEND, GP_WORKER_IDENTITY,
			gettext_noop("Sets the role for the session."),
			gettext_noop("Valid values are DISPATCH, EXECUTE, and UTILITY."),
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&gp_role_string,
		"undefined",
		check_gp_role, assign_gp_role, show_gp_role
	},

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

static bool
check_dispatch_log_stats(bool *newval, void **extra, GucSource source)
{
	if (*newval &&
		(log_parser_stats || log_planner_stats || log_executor_stats || log_statement_stats))
	{
		if (source >= PGC_S_INTERACTIVE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot enable \"log_dispatch_stats\" when "
							"\"log_statement_stats\", "
							"\"log_parser_stats\", \"log_planner_stats\", "
							"or \"log_executor_stats\" is true")));
		/* source == PGC_S_OVERRIDE means do it anyway, eg at xact abort */
		else if (source != PGC_S_OVERRIDE)
			return false;
	}
	return true;
}

