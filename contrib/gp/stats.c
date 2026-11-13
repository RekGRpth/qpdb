/* -------------------------------------------------------------------------
 *
 * stats.c
 *		ANALYZE on the segments and cluster statistics on the coordinator
 *
 * A distributed table is empty on the coordinator.  ANALYZE therefore runs
 * on every segment first.  Relation sizes are summed over the segments,
 * while attribute statistics come from the first segment, as an initial
 * sample of the distribution of the whole table.  A replicated table is
 * counted once.  The ordinary restore functions put the result into the
 * catalogs of the coordinator, whose planner then sees the cluster statistics.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/stats.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "nodes/value.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "gp.h"

static const char *const gp_stat_names[] = {
	"null_frac",
	"avg_width",
	"n_distinct",
	"most_common_vals",
	"most_common_freqs",
	"histogram_bounds",
	"correlation",
	"most_common_elems",
	"most_common_elem_freqs",
	"elem_count_histogram",
	"range_length_histogram",
	"range_empty_frac",
	"range_bounds_histogram",
};

static const char *const gp_stat_types[] = {
	"real",
	"integer",
	"real",
	"text",
	"real[]",
	"text",
	"real",
	"text",
	"real[]",
	"real[]",
	"text",
	"real",
	"text",
};

StaticAssertDecl(lengthof(gp_stat_names) == lengthof(gp_stat_types),
				 "statistic names and types must match");

/* Keep the ANALYZE options when the statement is rebuilt for one relation */
static void
gp_analyze_options(StringInfo sql, List *options)
{
	ListCell   *lc;

	if (options == NIL)
		return;
	appendStringInfoChar(sql, '(');
	foreach(lc, options)
	{
		DefElem    *opt = lfirst_node(DefElem, lc);

		appendStringInfo(sql, "%s%s", lc == list_head(options) ? "" : ", ",
						 quote_identifier(opt->defname));
		if (opt->arg == NULL)
			continue;
		switch (nodeTag(opt->arg))
		{
			case T_Boolean:
				appendStringInfo(sql, " %s", boolVal(opt->arg) ? "true" : "false");
				break;
			case T_Integer:
				appendStringInfo(sql, " %d", intVal(opt->arg));
				break;
			case T_Float:
			case T_String:
				appendStringInfo(sql, " %s",
								 quote_literal_cstr(strVal(opt->arg)));
				break;
			default:
				elog(ERROR, "unexpected ANALYZE option value");
		}
	}
	appendStringInfoString(sql, ") ");
}

/* Append one relation, rather than a local relation of a mixed statement */
static void
gp_analyze_relation(StringInfo sql, List *options, Oid relid, bool inherit,
					List *columns)
{
	const char *schema = get_namespace_name(get_rel_namespace(relid));
	const char *relname = get_rel_name(relid);
	ListCell   *lc;

	if (schema == NULL || relname == NULL)
		return;
	if (sql->len > 0)
		appendStringInfoString(sql, "; ");
	appendStringInfoString(sql, "ANALYZE ");
	gp_analyze_options(sql, options);
	if (!inherit)
		appendStringInfoString(sql, "ONLY ");
	appendStringInfoString(sql, quote_qualified_identifier(schema, relname));
	if (columns == NIL)
		return;
	appendStringInfoChar(sql, '(');
	foreach(lc, columns)
	{
		appendStringInfo(sql, "%s%s", lc == list_head(columns) ? "" : ", ",
						 quote_identifier(strVal(lfirst(lc))));
	}
	appendStringInfoChar(sql, ')');
}

/* The distributed relations of an explicit ANALYZE, or all of them */
static List *
gp_analyze_targets(VacuumStmt *stmt, StringInfo sql)
{
	List	   *relids = NIL;
	MemoryContext oldcontext = CurrentMemoryContext;

	if (stmt->rels != NIL)
	{
		ListCell   *lc;

		foreach(lc, stmt->rels)
		{
			VacuumRelation *vrel = lfirst_node(VacuumRelation, lc);
			GpPolicy	policy;
			Oid			relid = RangeVarGetRelid(vrel->relation, NoLock, true);

			if (!OidIsValid(relid) || !gp_policy_get(relid, &policy))
				continue;
			relids = lappend_oid(relids, relid);
			gp_analyze_relation(sql, stmt->options, relid,
								vrel->relation->inh, vrel->va_cols);
		}
		return relids;
	}

	SPI_connect();
	if (SPI_execute("SELECT relid FROM gp.distribution_policy ORDER BY relid",
					true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not list distributed relations for ANALYZE");
	MemoryContextSwitchTo(oldcontext);
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;
		Oid			relid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i],
												 SPI_tuptable->tupdesc, 1, &isnull));

		if (isnull)
			continue;
		relids = lappend_oid(relids, relid);
		gp_analyze_relation(sql, stmt->options, relid, true, NIL);
	}
	SPI_finish();
	return relids;
}

/* A restore function returns false for invalid arguments, without an ERROR */
static void
gp_stats_restore(const char *sql)
{
	bool		isnull;
	Datum		result;

	if (SPI_execute(sql, false, 1) != SPI_OK_SELECT || SPI_processed != 1)
		elog(ERROR, "could not restore cluster statistics");
	result = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
						   1, &isnull);
	if (isnull || !DatumGetBool(result))
		elog(ERROR, "could not restore cluster statistics");
}

/* Sum relation sizes, except that a replicated table is already whole */
static void
gp_stats_relation(Oid relid, bool replicated)
{
	GpQuery    *query;
	PGresult   *res;
	StringInfoData sql;
	uint64		pages = 0;
	uint64		allvisible = 0;
	uint64		allfrozen = 0;
	double		tuples = 0;
	int			rows = 0;
	GpSegment  *segments;
	int			nsegments;
	const char *schema = get_namespace_name(get_rel_namespace(relid));
	const char *relname = get_rel_name(relid);

	if (schema == NULL || relname == NULL)
		return;
	query = gp_query_start(psprintf(
		"SELECT relpages::text, reltuples::text, "
		"relallvisible::text, relallfrozen::text "
		"FROM pg_catalog.pg_class WHERE oid = %u", relid), 0);
	while ((res = gp_query_next(query)) != NULL)
	{
		if (PQnfields(res) != 4 || PQntuples(res) != 1)
			elog(ERROR, "unexpected relation statistics from a segment");
		if (!replicated || rows == 0)
		{
			pages += strtoull(PQgetvalue(res, 0, 0), NULL, 10);
			tuples += strtod(PQgetvalue(res, 0, 1), NULL);
			allvisible += strtoull(PQgetvalue(res, 0, 2), NULL, 10);
			allfrozen += strtoull(PQgetvalue(res, 0, 3), NULL, 10);
		}
		rows++;
		PQclear(res);
	}
	nsegments = gp_segments_get(&segments);
	if (rows != nsegments)
		elog(ERROR, "relation statistics are missing on a segment");

	initStringInfo(&sql);
	appendStringInfo(&sql,
		"SELECT pg_catalog.pg_restore_relation_stats("
		"'version', '%d'::integer, 'schemaname', %s::text, "
		"'relname', %s::text, 'relpages', '%llu'::integer, "
		"'reltuples', '%.17g'::real, 'relallvisible', '%llu'::integer, "
		"'relallfrozen', '%llu'::integer)",
		PG_VERSION_NUM, quote_literal_cstr(schema), quote_literal_cstr(relname),
		(unsigned long long) pages, tuples,
		(unsigned long long) allvisible, (unsigned long long) allfrozen);
	gp_stats_restore(sql.data);
}

/* Copy the sample of the first segment, using attnum instead of attname */
static void
gp_stats_attributes(Oid relid, int only)
{
	GpQuery    *query;
	PGresult   *res;
	const char *schema = get_namespace_name(get_rel_namespace(relid));
	const char *relname = get_rel_name(relid);

	if (schema == NULL || relname == NULL)
		return;
	query = gp_query_start(psprintf(
		"SELECT a.attnum::text, s.inherited::text, "
		"s.null_frac::text, s.avg_width::text, s.n_distinct::text, "
		"s.most_common_vals::text, s.most_common_freqs::text, "
		"s.histogram_bounds::text, s.correlation::text, "
		"s.most_common_elems::text, s.most_common_elem_freqs::text, "
		"s.elem_count_histogram::text, s.range_length_histogram::text, "
		"s.range_empty_frac::text, s.range_bounds_histogram::text "
		"FROM pg_catalog.pg_stats s "
		"JOIN pg_catalog.pg_attribute a "
		"ON a.attrelid = s.tableid AND a.attname = s.attname "
		"WHERE s.tableid = %u ORDER BY a.attnum, s.inherited", relid), only);
	while ((res = gp_query_next(query)) != NULL)
	{
		StringInfoData sql;

		if (PQnfields(res) != lengthof(gp_stat_names) + 2 ||
			PQntuples(res) != 1)
			elog(ERROR, "unexpected attribute statistics from a segment");
		initStringInfo(&sql);
		appendStringInfo(&sql,
			"SELECT pg_catalog.pg_restore_attribute_stats("
			"'version', '%d'::integer, 'schemaname', %s::text, "
			"'relname', %s::text, 'attnum', '%s'::smallint, "
			"'inherited', '%s'::boolean",
			PG_VERSION_NUM, quote_literal_cstr(schema),
			quote_literal_cstr(relname), PQgetvalue(res, 0, 0),
			PQgetvalue(res, 0, 1));
		for (int i = 0; i < lengthof(gp_stat_names); i++)
		{
			if (PQgetisnull(res, 0, i + 2))
				continue;
			appendStringInfo(&sql, ", '%s', %s::%s", gp_stat_names[i],
							 quote_literal_cstr(PQgetvalue(res, 0, i + 2)),
							 gp_stat_types[i]);
		}
		appendStringInfoChar(&sql, ')');
		gp_stats_restore(sql.data);
		PQclear(res);
	}
}

/* Run ANALYZE on the segments and put cluster statistics on the coordinator */
void
gp_analyze(VacuumStmt *stmt)
{
	StringInfoData sql;
	List	   *relids;
	ListCell   *lc;
	GpSegment  *segments;
	int			nsegments;

	initStringInfo(&sql);
	relids = gp_analyze_targets(stmt, &sql);
	if (relids == NIL)
		return;

	gp_dispatch_xact(sql.data);
	nsegments = gp_segments_get(&segments);
	if (nsegments == 0)
		return;

	SPI_connect();
	foreach(lc, relids)
	{
		Oid			relid = lfirst_oid(lc);
		GpPolicy	policy;

		if (!gp_policy_get(relid, &policy))
			continue;
		gp_stats_relation(relid, policy.replicated);
		gp_stats_attributes(relid, segments[0].segment_id);
	}
	SPI_finish();
	CommandCounterIncrement();
}
