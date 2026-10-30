/* -------------------------------------------------------------------------
 *
 * route.c
 *		Rows of a distributed table to the segments
 *
 * A route is a COPY ... FROM STDIN of the binary format, open on all
 * segments in the transactions that follow the transaction of the
 * coordinator.  A row, as the values of the columns of the table, goes to
 * the segment of its bucket, or to all segments for a replicated table;
 * the binary format takes every type as it is.  The columns the segments
 * compute themselves, the generated ones, are not sent.  The rows wait in
 * a buffer of each segment, and go in chunks.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/route.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "gp.h"

/* A buffer of a segment is sent when it grows to this size */
#define GP_ROUTE_FLUSH 65536

/* Header of the binary format: the signature, then no flags and no extension */
static const char binary_signature[11] = "PGCOPY\n\377\r\n\0";

struct GpRoute
{
	GpPolicy	policy;
	int			nsend;			/* columns that go to the segments */
	int		   *sendatt;		/* their positions in the row */
	FmgrInfo   *sendfn;			/* their binary output functions */
	Oid		   *keytypes;
	Datum	   *keyvalues;
	bool	   *keynulls;
	GpSegment  *segments;
	int			nsegments;
	int		   *bucket_pos;		/* segment of each bucket, as a position */
	bool		direct;			/* all rows go to the one segment of the route */
	StringInfoData *buffers;	/* one for each segment */
	StringInfoData tuple;		/* the row that is being sent */
};

/*
 * Start a route to the segments for rows of a table; the memory of the
 * route is that of the caller until gp_route_end().  With a segment other
 * than 0, the route goes to it alone, and all rows to it, whatever their
 * buckets: the caller knows that they belong there.
 */
GpRoute *
gp_route_begin(Relation rel, const GpPolicy *policy, int only)
{
	GpRoute    *route = palloc0_object(GpRoute);
	TupleDesc	tupDesc = RelationGetDescr(rel);
	int			natts = tupDesc->natts;
	StringInfoData collist;
	char	   *copysql;

	route->policy = *policy;

	/* The columns that go to the segments; they compute the generated ones */
	route->sendatt = palloc_array(int, natts);
	route->sendfn = palloc_array(FmgrInfo, natts);
	initStringInfo(&collist);
	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupDesc, i);
		Oid			sendoid;
		bool		isvarlena;

		if (att->attisdropped || att->attgenerated)
			continue;
		getTypeBinaryOutputInfo(att->atttypid, &sendoid, &isvarlena);
		fmgr_info(sendoid, &route->sendfn[route->nsend]);
		route->sendatt[route->nsend++] = i;
		appendStringInfo(&collist, "%s%s", collist.len > 0 ? ", " : "",
						 quote_identifier(NameStr(att->attname)));
	}
	copysql = psprintf("COPY %s (%s) FROM STDIN (FORMAT binary)",
					   quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
												  RelationGetRelationName(rel)),
					   collist.data);

	route->keytypes = palloc_array(Oid, Max(policy->nkeys, 1));
	route->keyvalues = palloc_array(Datum, Max(policy->nkeys, 1));
	route->keynulls = palloc_array(bool, Max(policy->nkeys, 1));
	for (int k = 0; k < policy->nkeys; k++)
		route->keytypes[k] = TupleDescAttr(tupDesc, policy->keys[k] - 1)->atttypid;

	route->direct = only != 0;
	route->nsegments = gp_copy_open(copysql, only, &route->segments);
	if (route->nsegments == 0)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("the cluster has no segments"));

	/* The segment of each bucket, as a position in the segments */
	if (!policy->replicated && !route->direct)
	{
		route->bucket_pos = palloc_array(int, policy->nbuckets);
		for (int b = 0; b < policy->nbuckets; b++)
		{
			int			pos = -1;

			for (int i = 0; i < route->nsegments && pos < 0; i++)
				if (route->segments[i].segment_id == policy->map[b])
					pos = i;
			if (pos < 0)
				ereport(ERROR,
						errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("bucket %d of table \"%s\" is on segment %d, which is not registered",
							   b, RelationGetRelationName(rel), policy->map[b]));
			route->bucket_pos[b] = pos;
		}
	}

	route->buffers = palloc_array(StringInfoData, route->nsegments);
	for (int i = 0; i < route->nsegments; i++)
	{
		initStringInfo(&route->buffers[i]);
		appendBinaryStringInfo(&route->buffers[i], binary_signature,
							   sizeof(binary_signature));
		pq_sendint32(&route->buffers[i], 0);	/* flags */
		pq_sendint32(&route->buffers[i], 0);	/* length of the header extension */
	}
	initStringInfo(&route->tuple);

	return route;
}

/*
 * Send a row to its segment, or to all of them.  The values are those of
 * the columns of the table by their positions; what the function allocates
 * for the row is of the current memory context.
 */
void
gp_route_row(GpRoute *route, const Datum *values, const bool *nulls)
{
	const GpPolicy *policy = &route->policy;
	int			first;
	int			last;

	resetStringInfo(&route->tuple);
	pq_sendint16(&route->tuple, route->nsend);
	for (int k = 0; k < route->nsend; k++)
	{
		int			i = route->sendatt[k];

		if (nulls[i])
			pq_sendint32(&route->tuple, (uint32) -1);
		else
		{
			bytea	   *data = SendFunctionCall(&route->sendfn[k], values[i]);

			pq_sendint32(&route->tuple, VARSIZE_ANY_EXHDR(data));
			pq_sendbytes(&route->tuple, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data));
		}
	}

	if (policy->replicated)
	{
		first = 0;
		last = route->nsegments - 1;
	}
	else if (route->direct)
		first = last = 0;
	else
	{
		for (int k = 0; k < policy->nkeys; k++)
		{
			route->keyvalues[k] = values[policy->keys[k] - 1];
			route->keynulls[k] = nulls[policy->keys[k] - 1];
		}
		first = last = route->bucket_pos[gp_key_bucket(policy->nbuckets,
													   policy->nkeys,
													   route->keyvalues,
													   route->keynulls,
													   route->keytypes,
													   InvalidOid)];
	}

	for (int i = first; i <= last; i++)
	{
		StringInfo	buffer = &route->buffers[i];

		appendBinaryStringInfo(buffer, route->tuple.data, route->tuple.len);
		if (buffer->len >= GP_ROUTE_FLUSH)
		{
			gp_copy_send(route->segments[i].segment_id, buffer->data,
						 buffer->len);
			resetStringInfo(buffer);
		}
	}
}

/*
 * Send what waits in the buffers, end the COPY on the segments, and wait
 * for them to apply the rows
 */
void
gp_route_end(GpRoute *route)
{
	for (int i = 0; i < route->nsegments; i++)
	{
		pq_sendint16(&route->buffers[i], (uint16) -1);	/* the trailer */
		gp_copy_send(route->segments[i].segment_id, route->buffers[i].data,
					 route->buffers[i].len);
	}
	gp_copy_close();
}
