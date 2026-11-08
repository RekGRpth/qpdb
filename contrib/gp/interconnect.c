/* -------------------------------------------------------------------------
 *
 * interconnect.c
 *		Pull rows between segments over a small TCP protocol
 *
 * A request has a database, a user, the segment the rows are for and SQL.
 * The segment executes it as that user and streams rows in the binary format
 * of their types, followed by EOS.  An error goes back as text.  The receiver
 * materializes the answer, so the writer never waits for a reader of another
 * motion while the reader is blocked on this one.  Closing the connection
 * cancels the query on either side.
 *
 * The protocol uses the same trust model as the cluster's peer connections:
 * the server named by the request checks the user's privileges itself.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/interconnect.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <sys/un.h>

#include "access/xact.h"
#include "common/ip.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/readfuncs.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "tcop/dest.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "gp.h"

#define GP_IC_MAGIC "GPIC0001"
#define GP_IC_MAGIC_LEN 8
#define GP_IC_MAX_STRING (16 * 1024 * 1024)
#define GP_IC_MAX_FIELD (1024 * 1024 * 1024)
#define GP_IC_PORT_OFFSET 10000
#define GP_IC_LISTEN_BACKLOG 16

#define GP_IC_ROW 'R'
#define GP_IC_EOS 'E'
#define GP_IC_ERROR 'X'
#define GP_IC_RETRY 'I'
#define GP_IC_READY 'Y'
#define GP_IC_GO 'G'

pg_noreturn PGDLLEXPORT void gp_interconnect_main(Datum main_arg);

/* Read the shared secret installed by the registration worker */
static char *
gp_interconnect_token(void)
{
	char		path[MAXPGPATH];
	FILE	   *file;
	char		token[65];

	snprintf(path, sizeof(path), "%s/gp_interconnect.token", DataDir);
	file = AllocateFile(path, "r");
	if (file == NULL)
		return NULL;
	if (fgets(token, sizeof(token), file) == NULL)
	{
		FreeFile(file);
		return NULL;
	}
	FreeFile(file);
	if (strlen(token) != 64)
		return NULL;
	for (int i = 0; i < 64; i++)
		if (!isxdigit((unsigned char) token[i]))
			return NULL;
	return pstrdup(token);
}

static bool
gp_ic_read(pgsocket fd, void *buf, int len)
{
	char	   *p = buf;

	while (len > 0)
	{
		ssize_t		n = recv(fd, p, len, 0);

		if (n < 0 && errno == EINTR)
		{
			CHECK_FOR_INTERRUPTS();
			continue;
		}
		if (n <= 0)
			return false;
		p += n;
		len -= n;
	}
	return true;
}

static bool
gp_ic_write(pgsocket fd, const void *buf, int len)
{
	const char *p = buf;

	while (len > 0)
	{
		ssize_t		n = send(fd, p, len, 0);

		if (n < 0 && errno == EINTR)
		{
			CHECK_FOR_INTERRUPTS();
			continue;
		}
		if (n <= 0)
			return false;
		p += n;
		len -= n;
	}
	return true;
}

static bool
gp_ic_read_i32(pgsocket fd, int32 *value)
{
	uint32		net;

	if (!gp_ic_read(fd, &net, sizeof(net)))
		return false;
	*value = (int32) ntohl(net);
	return true;
}

static bool
gp_ic_write_i32(pgsocket fd, int32 value)
{
	uint32		net = htonl((uint32) value);

	return gp_ic_write(fd, &net, sizeof(net));
}

static char *
gp_ic_read_string(pgsocket fd)
{
	int32		len;
	char	   *str;

	if (!gp_ic_read_i32(fd, &len) || len < 0 || len > GP_IC_MAX_STRING)
		return NULL;
	str = palloc(len + 1);
	if (!gp_ic_read(fd, str, len))
		return NULL;
	str[len] = '\0';
	return str;
}

static bool
gp_ic_write_message(pgsocket fd, char type, const void *buf, int32 len)
{
	return gp_ic_write(fd, &type, 1) && gp_ic_write_i32(fd, len) &&
		gp_ic_write(fd, buf, len);
}

static bool
gp_ic_write_error(pgsocket fd, const char *message)
{
	return gp_ic_write_message(fd, GP_IC_ERROR, message, strlen(message));
}

/* Do not reset a response that the client has not read yet */
static void
gp_ic_shutdown_write(pgsocket fd)
{
#ifdef WIN32
	(void) shutdown(fd, SD_SEND);
#else
	(void) shutdown(fd, SHUT_WR);
#endif
}

typedef struct GpFragmentDest
{
	DestReceiver pub;
	pgsocket fd;
} GpFragmentDest;

/* One tuple of the executor, as a row of the protocol */
static bool
gp_ic_receive_tuple(TupleTableSlot *slot, DestReceiver *self)
{
	GpFragmentDest *dest = (GpFragmentDest *) self;
	TupleDesc	desc = slot->tts_tupleDescriptor;
	StringInfoData message;

	initStringInfo(&message);
	slot_getallattrs(slot);
	for (int i = 0; i < desc->natts; i++)
	{
		Oid			sendfn;
		bool		typisvarlena;
		FmgrInfo	send;
		bytea	   *value;
		int32		net_len;

		if (slot->tts_isnull[i])
		{
			net_len = htonl((uint32) -1);
			appendBinaryStringInfo(&message, (char *) &net_len, sizeof(net_len));
			continue;
		}
		getTypeBinaryOutputInfo(TupleDescAttr(desc, i)->atttypid,
								&sendfn, &typisvarlena);
		fmgr_info(sendfn, &send);
		value = SendFunctionCall(&send, slot->tts_values[i]);
		net_len = htonl((uint32) VARSIZE_ANY_EXHDR(value));
		appendBinaryStringInfo(&message, (char *) &net_len, sizeof(net_len));
		appendBinaryStringInfo(&message, VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value));
		pfree(value);
	}
	{
		bool		ok = gp_ic_write_message(dest->fd, GP_IC_ROW, message.data, message.len);

		pfree(message.data);
		return ok;
	}
}

static void
gp_ic_receive_noop(DestReceiver *self)
{
}

static void
gp_ic_receive_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
}

/* Deserialize and execute one fragment, streaming its tuples */
static void
gp_ic_execute(pgsocket fd, const char *serialized)
{
	PlannedStmt *stmt = (PlannedStmt *) stringToNode(serialized);
	QueryDesc   *query;
	GpFragmentDest *dest;

	if (!IsA(stmt, PlannedStmt) || stmt->commandType != CMD_SELECT)
		ereport(ERROR,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("an invalid plan fragment"));
	foreach_node(RangeTblEntry, rte, stmt->rtable)
	{
		if (rte->rtekind == RTE_RELATION)
			LockRelationOid(rte->relid,
							rte->rellockmode != NoLock ? rte->rellockmode : AccessShareLock);
	}

	dest = palloc0(sizeof(GpFragmentDest));
	dest->pub.receiveSlot = gp_ic_receive_tuple;
	dest->pub.rStartup = gp_ic_receive_startup;
	dest->pub.rShutdown = gp_ic_receive_noop;
	dest->pub.rDestroy = gp_ic_receive_noop;
	dest->pub.mydest = DestTuplestore;
	dest->fd = fd;
	query = CreateQueryDesc(stmt, serialized, GetActiveSnapshot(), NULL,
							(DestReceiver *) dest, NULL, NULL, 0);
	ExecutorStart(query, 0);
	ExecutorRun(query, ForwardScanDirection, 0L);
	ExecutorFinish(query);
	ExecutorEnd(query);
	FreeQueryDesc(query);
	pfree(dest);
}

/* Execute the request of one connection and answer it */
static bool
gp_ic_serve(pgsocket fd, const char *token)
{
	char		magic[GP_IC_MAGIC_LEN];
	char	   *provided;
	char	   *db = NULL;
	char	   *user = NULL;
	char	   *sql = NULL;
	int32		requester;
	bool		restart = false;
	static char *connected_db = NULL;
	static char *connected_user = NULL;

	if (!gp_ic_read(fd, magic, sizeof(magic)) ||
		memcmp(magic, GP_IC_MAGIC, sizeof(magic)) != 0)
		return false;
	provided = gp_ic_read_string(fd);
	if (provided == NULL || strlen(provided) != 64 ||
		timingsafe_bcmp(provided, token, 64) != 0)
	{
		(void) gp_ic_write_error(fd, "invalid interconnect token");
		gp_ic_shutdown_write(fd);
		return false;
	}
	db = gp_ic_read_string(fd);
	user = gp_ic_read_string(fd);
	if (db == NULL || user == NULL ||
		!gp_ic_read_i32(fd, &requester))
		return false;
	sql = gp_ic_read_string(fd);
	if (sql == NULL)
		return false;

	PG_TRY();
	{
		char		set[64];
		char		go;

		if (connected_db == NULL)
		{
			BackgroundWorkerInitializeConnection(db, user,
										 BGWORKER_BYPASS_ROLELOGINCHECK);
			connected_db = MemoryContextStrdup(TopMemoryContext, db);
			connected_user = MemoryContextStrdup(TopMemoryContext, user);
		}
		else if (strcmp(connected_db, db) != 0 ||
				 strcmp(connected_user, user) != 0)
		{
			/* Do not execute under the identity of an earlier request */
			(void) gp_ic_write_message(fd, GP_IC_RETRY, NULL, 0);
			/* Let the client read the retry before the worker goes away */
			gp_ic_shutdown_write(fd);
			restart = true;
		}
		/* No executor or snapshot runs before the client confirms GO */
		if (!restart && gp_ic_write_message(fd, GP_IC_READY, NULL, 0) &&
			gp_ic_read(fd, &go, 1) && go == GP_IC_GO)
		{
			snprintf(set, sizeof(set), "SET gp.requester_segment = %d", requester);
			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			SPI_connect();
			PushActiveSnapshot(GetTransactionSnapshot());
			if (SPI_execute(set, false, 0) != SPI_OK_UTILITY)
				ereport(ERROR,
						errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("could not set the receiving segment"));
			gp_ic_execute(fd, sql);

			if (!gp_ic_write_message(fd, GP_IC_EOS, NULL, 0))
				ereport(ERROR,
						errcode(ERRCODE_CONNECTION_FAILURE),
						errmsg("could not finish the motion answer"));
			PopActiveSnapshot();
			SPI_finish();
			CommitTransactionCommand();
		}
	}
	PG_CATCH();
	{
		ErrorData  *edata = CopyErrorData();

		AbortCurrentTransaction();
		(void) gp_ic_write_error(fd, edata->message);
		FreeErrorData(edata);
		EmitErrorReport();
		FlushErrorState();
	}
	PG_END_TRY();
	return restart;
}

/* A Unix socket in gp.host's directory, used by the TAP clusters */
static void
gp_ic_unix_path(const char *host, int port, struct sockaddr_un *addr)
{
	MemSet(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
	addr->sun_len = sizeof(*addr);
#endif
	if (snprintf(addr->sun_path, sizeof(addr->sun_path),
				 "%s/gp_interconnect_%d.sock", host, port) >= sizeof(addr->sun_path))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("the interconnect socket path is too long"));
}

/* The listener of a segment, one request at a time */
void
gp_interconnect_main(Datum main_arg)
{
	struct addrinfo *addrs = NULL;
	struct addrinfo hints = {0};
	pgsocket listener = PGINVALID_SOCKET;
	char		port[12];
	char	   *token;
	uint32		wait_event;

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	snprintf(port, sizeof(port), "%d", PostPortNumber + GP_IC_PORT_OFFSET);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (is_absolute_path(gp_host_address()))
	{
		struct sockaddr_un addr;

		gp_ic_unix_path(gp_host_address(), PostPortNumber + GP_IC_PORT_OFFSET,
						&addr);
		listener = socket(AF_UNIX, SOCK_STREAM, 0);
		if (listener != PGINVALID_SOCKET)
		{
			unlink(addr.sun_path);
			if (bind(listener, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
				listen(listener, GP_IC_LISTEN_BACKLOG) != 0)
			{
				closesocket(listener);
				listener = PGINVALID_SOCKET;
			}
		}
	}
	else if (pg_getaddrinfo_all(NULL, port, &hints, &addrs) != 0)
	{
		ereport(LOG,
				errmsg("could not resolve the interconnect address"));
		proc_exit(1);
	}
	for (struct addrinfo *ai = addrs; ai != NULL; ai = ai->ai_next)
	{
		int			on = 1;

		listener = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (listener == PGINVALID_SOCKET)
			continue;
		(void) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
						  (const char *) &on, sizeof(on));
		if (bind(listener, ai->ai_addr, ai->ai_addrlen) == 0 &&
			listen(listener, GP_IC_LISTEN_BACKLOG) == 0)
			break;
		closesocket(listener);
		listener = PGINVALID_SOCKET;
	}
	pg_freeaddrinfo_all(hints.ai_family, addrs);
	if (listener == PGINVALID_SOCKET)
	{
		ereport(LOG,
				errmsg("could not listen on interconnect port %d",
					   PostPortNumber + GP_IC_PORT_OFFSET));
		proc_exit(1);
	}

	if (!pg_set_noblock(listener))
	{
		ereport(LOG,
				errmsg("could not make the interconnect listener nonblocking"));
		proc_exit(1);
	}
	/* Accept registrations that finish while their first motion connects */
	token = NULL;
	for (int i = 0; i < 60 && token == NULL; i++)
	{
		token = gp_interconnect_token();
		if (token == NULL)
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep(1000000);
		}
	}
	if (token == NULL)
	{
		ereport(LOG,
				errmsg("the interconnect token is not installed yet"));
		proc_exit(1);
	}

	wait_event = WaitEventExtensionNew("GpInterconnect");

	for (;;)
	{
		long		rc;
		pgsocket	fd;
		bool		restart;

		ResetLatch(MyLatch);
		rc = WaitLatchOrSocket(MyLatch,
								   WL_LATCH_SET | WL_EXIT_ON_PM_DEATH |
								   WL_SOCKET_READABLE | WL_SOCKET_ACCEPT |
								   WL_TIMEOUT,
								   listener, 1000, wait_event);
		if (rc & WL_EXIT_ON_PM_DEATH)
			proc_exit(0);
		CHECK_FOR_INTERRUPTS();
		fd = accept(listener, NULL, NULL);

		if (fd == PGINVALID_SOCKET)
		{
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			ereport(LOG,
					errmsg("could not accept an interconnect connection"));
			proc_exit(1);
		}
		if (!pg_set_block(fd))
		{
			closesocket(fd);
			ereport(LOG,
					errmsg("could not make an interconnect connection blocking"));
			continue;
		}
		restart = gp_ic_serve(fd, token);
		closesocket(fd);
		if (restart)
			proc_exit(1);
	}
}

/* Called from _PG_init() on a segment */
void
gp_interconnect_init(void)
{
	BackgroundWorker worker = {0};

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 1;
	strlcpy(worker.bgw_library_name, "gp", BGW_MAXLEN);
	strlcpy(worker.bgw_function_name, "gp_interconnect_main", BGW_MAXLEN);
	strlcpy(worker.bgw_name, "gp interconnect", BGW_MAXLEN);
	strlcpy(worker.bgw_type, "gp interconnect", BGW_MAXLEN);
	RegisterBackgroundWorker(&worker);
}

/* Plan a fragment locally; only its serialized plan goes over the protocol */
char *
gp_interconnect_plan(const char *sql)
{
	PlannedStmt *stmt;
	RawStmt    *raw = linitial(pg_parse_query(sql));
	Query	   *query = parse_analyze_fixedparams(raw, sql, NULL, 0, NULL);
	bool		old = gp_gather_fragment(true);

	PG_TRY();
	{
		stmt = standard_planner(query, sql, 0, NULL, NULL);
	}
	PG_FINALLY();
	{
		gp_gather_fragment(old);
	}
	PG_END_TRY();
	return nodeToString(stmt);
}

/* Read a row into the store of the caller */
static void
gp_interconnect_row(Tuplestorestate *store, TupleDesc tupdesc,
					FmgrInfo *recv, Oid *ioparam,
					const char *data, int32 len)
{
	TupleTableSlot *slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	const char *p = data;
	const char *end = data + len;

	for (int i = 0; i < tupdesc->natts; i++)
	{
		int32		value_len;
		int32		net_len;
		StringInfoData buf;

		if (end - p < sizeof(net_len))
			ereport(ERROR,
					errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("a short row in the interconnect answer"));
		memcpy(&net_len, p, sizeof(net_len));
		value_len = (int32) ntohl(net_len);
		p += sizeof(net_len);
		if (value_len == -1)
		{
			slot->tts_isnull[i] = true;
			continue;
		}
		if (value_len < 0 || value_len > GP_IC_MAX_FIELD || end - p < value_len)
			ereport(ERROR,
					errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("a short value in the interconnect answer"));
		buf.data = (char *) p;
		buf.len = value_len;
		buf.maxlen = value_len;
		buf.cursor = 0;
		slot->tts_values[i] = ReceiveFunctionCall(&recv[i], &buf, ioparam[i],
												TupleDescAttr(tupdesc, i)->atttypmod);
		slot->tts_isnull[i] = false;
		p += value_len;
	}
	if (p != end)
		ereport(ERROR,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("an overlong row in the interconnect answer"));
	ExecStoreVirtualTuple(slot);
	tuplestore_puttupleslot(store, slot);
	ExecDropSingleTupleTableSlot(slot);
}

/* Ask one peer for its rows; false asks for a dispatched-connection fallback */
bool
gp_interconnect_query_try(Tuplestorestate *store, TupleDesc tupdesc,
					  const char *db, const char *user, const char *sql,
					  const char *host, int port, int requester)
{
	struct addrinfo *addrs = NULL;
	struct addrinfo hints = {0};
	pgsocket fd = PGINVALID_SOCKET;
	char		portstr[12];
	FmgrInfo   *recv;
	Oid		   *ioparam;
	MemoryContext rowcxt;
	char	   *token;
	bool		retry = false;

	token = gp_interconnect_token();
	if (token == NULL)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("the interconnect token is not installed yet"));

	snprintf(portstr, sizeof(portstr), "%d", port + GP_IC_PORT_OFFSET);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (is_absolute_path(host))
	{
		struct sockaddr_un addr;

		gp_ic_unix_path(host, port + GP_IC_PORT_OFFSET, &addr);
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd != PGINVALID_SOCKET && connect(fd, (struct sockaddr *) &addr,
											sizeof(addr)) != 0)
		{
			closesocket(fd);
			fd = PGINVALID_SOCKET;
		}
	}
	else
	{
		if (pg_getaddrinfo_all(host, portstr, &hints, &addrs) != 0)
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not resolve the interconnect address of %s", host));
		for (struct addrinfo *ai = addrs; ai != NULL; ai = ai->ai_next)
		{
			fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
			if (fd == PGINVALID_SOCKET)
				continue;
			if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
				break;
			closesocket(fd);
			fd = PGINVALID_SOCKET;
		}
	}
	pg_freeaddrinfo_all(hints.ai_family, addrs);
	if (fd == PGINVALID_SOCKET)
		return false;

	recv = palloc0_array(FmgrInfo, tupdesc->natts);
	ioparam = palloc_array(Oid, tupdesc->natts);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Oid			recvfn;

		if (TupleDescAttr(tupdesc, i)->attisdropped)
			continue;
		getTypeBinaryInputInfo(TupleDescAttr(tupdesc, i)->atttypid,
							   &recvfn, &ioparam[i]);
		fmgr_info(recvfn, &recv[i]);
	}
	rowcxt = AllocSetContextCreate(CurrentMemoryContext, "interconnect row",
								   ALLOCSET_DEFAULT_SIZES);

	PG_TRY();
	{
		if (!gp_ic_write(fd, GP_IC_MAGIC, GP_IC_MAGIC_LEN) ||
			!gp_ic_write_i32(fd, strlen(token)) ||
			!gp_ic_write(fd, token, strlen(token)) ||
			!gp_ic_write_i32(fd, strlen(db)) ||
			!gp_ic_write(fd, db, strlen(db)) ||
			!gp_ic_write_i32(fd, strlen(user)) ||
			!gp_ic_write(fd, user, strlen(user)) ||
			!gp_ic_write_i32(fd, requester) ||
			!gp_ic_write_i32(fd, strlen(sql)) ||
			!gp_ic_write(fd, sql, strlen(sql)))
			retry = true;

		/* Until GO is sent, the peer has not executed the fragment */
		if (!retry)
		{
			char		type;
			int32		len;
			char	   *data;
			char		go = GP_IC_GO;

			if (!gp_ic_read(fd, &type, 1) || !gp_ic_read_i32(fd, &len) ||
				len < 0 || len > GP_IC_MAX_STRING)
				retry = true;
			else
			{
				data = palloc(len);
				if (!gp_ic_read(fd, data, len))
					retry = true;
				else if (type == GP_IC_RETRY && len == 0)
					retry = true;
				else if (type == GP_IC_READY && len == 0)
				{
					if (!gp_ic_write(fd, &go, 1))
						ereport(ERROR,
								errcode(ERRCODE_CONNECTION_FAILURE),
								errmsg("could not start the motion query on %s", host));
				}
				else if (type == GP_IC_ERROR)
					ereport(ERROR,
							errcode(ERRCODE_INTERNAL_ERROR),
							errmsg("the segment returned an error: %.*s", len, data));
				else
					ereport(ERROR,
							errcode(ERRCODE_DATA_CORRUPTED),
							errmsg("an unknown start of the interconnect answer"));
				pfree(data);
			}
		}

		for (; !retry;)
		{
			char		type;
			int32		len;
			char	   *data;

			CHECK_FOR_INTERRUPTS();
			if (!gp_ic_read(fd, &type, 1) || !gp_ic_read_i32(fd, &len) ||
				len < 0 || len > GP_IC_MAX_STRING)
				ereport(ERROR,
						errcode(ERRCODE_CONNECTION_FAILURE),
						errmsg("could not read the interconnect answer of %s", host));
			data = palloc(len);
			if (!gp_ic_read(fd, data, len))
				ereport(ERROR,
						errcode(ERRCODE_CONNECTION_FAILURE),
						errmsg("could not read the interconnect answer of %s", host));
			if (type == GP_IC_RETRY)
				ereport(ERROR,
						errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("a retry after the start of the interconnect query"));
			if (type == GP_IC_EOS)
			{
				if (len != 0)
					ereport(ERROR,
							errcode(ERRCODE_DATA_CORRUPTED),
							errmsg("a nonempty end of the interconnect answer"));
				break;
			}
			if (type == GP_IC_ERROR)
				ereport(ERROR,
						errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("the segment returned an error: %.*s", len, data));
			if (type != GP_IC_ROW)
				ereport(ERROR,
						errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("an unknown message in the interconnect answer"));
			{
				MemoryContext oldcontext = MemoryContextSwitchTo(rowcxt);

				gp_interconnect_row(store, tupdesc, recv, ioparam, data, len);
				MemoryContextSwitchTo(oldcontext);
				MemoryContextReset(rowcxt);
			}
			pfree(data);
		}
		closesocket(fd);
	}
	PG_CATCH();
	{
		closesocket(fd);
		PG_RE_THROW();
	}
	PG_END_TRY();
	MemoryContextDelete(rowcxt);
	return !retry;
}

void
gp_interconnect_query(Tuplestorestate *store, TupleDesc tupdesc,
					  const char *db, const char *user, const char *sql,
					  const char *host, int port, int requester)
{
	for (int i = 0; i < 60; i++)
	{
		if (gp_interconnect_query_try(store, tupdesc, db, user, sql,
									  host, port, requester))
			return;
		CHECK_FOR_INTERRUPTS();
		pg_usleep(100000);
	}
	ereport(ERROR,
			errcode(ERRCODE_CONNECTION_FAILURE),
			errmsg("could not connect to the interconnect of %s with the requested database and role",
				   host));
}
