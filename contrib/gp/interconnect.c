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
#include <time.h>
#include <sys/un.h>

#include "access/xact.h"
#include "common/ip.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "nodes/readfuncs.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "port/atomics.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "tcop/dest.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
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
#define GP_IC_ASSIGN 'A'
#define GP_IC_CANCEL 'C'

#define GP_IC_SLOTS 64

typedef enum GpIcSlotState
{
	GP_IC_SLOT_FREE = 0,
	GP_IC_SLOT_STARTING,
	GP_IC_SLOT_READY,
	GP_IC_SLOT_ASSIGNED,
	GP_IC_SLOT_BUSY,
} GpIcSlotState;

typedef struct GpIcSlot
{
	slock_t		lock;
	pg_atomic_uint32 state;
	pg_atomic_uint32 port;
	uint64		generation;
	pid_t		pid;
	time_t		started;
	Latch	   *latch;
	char		db[NAMEDATALEN];
	char		user[NAMEDATALEN];
} GpIcSlot;

static GpIcSlot *gp_ic_slots = NULL;
static BackgroundWorkerHandle *gp_ic_handles[GP_IC_SLOTS];

pg_noreturn PGDLLEXPORT void gp_interconnect_main(Datum main_arg);
pg_noreturn PGDLLEXPORT void gp_interconnect_request_main(Datum main_arg);

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

/* The wait event of the reads of a client of the interconnect */
static uint32
gp_ic_client_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("GpInterconnectRead");
	return event;
}

/* Whether the last socket call found a nonblocking socket busy */
static bool
gp_ic_wouldblock(void)
{
#ifdef WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/*
 * Read from a nonblocking socket, waiting on the latch between attempts.
 * A client that waits this way is reached by a cancel of its own query:
 * no signal handler of pqsignal interrupts a blocking recv, and neither
 * does the timer of a statement timeout.
 */
static bool
gp_ic_read_latched(pgsocket fd, void *buf, int len)
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
		if (n < 0 && gp_ic_wouldblock())
		{
			int			wake;
			int			holdoff = InterruptHoldoffCount;

			/*
			 * A statement that reads the interconnect holds the lock of
			 * the distributed snapshot, which holds its interrupts back:
			 * a wait must let them through, or a timeout of the query
			 * never reaches it
			 */
			InterruptHoldoffCount = 0;
			wake = WaitLatchOrSocket(MyLatch,
									 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH |
									 WL_SOCKET_READABLE,
									 fd, -1L, gp_ic_client_event());
			if (wake & WL_LATCH_SET)
				ResetLatch(MyLatch);
			/* An interrupt is answered here, while nothing holds it back */
			CHECK_FOR_INTERRUPTS();
			InterruptHoldoffCount = holdoff;
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
gp_ic_read_i32_latched(pgsocket fd, int32 *value)
{
	uint32		net;

	if (!gp_ic_read_latched(fd, &net, sizeof(net)))
		return false;
	*value = (int32) ntohl(net);
	return true;
}

/* Write to a nonblocking socket, waiting on the latch between attempts */
static bool
gp_ic_write_latched(pgsocket fd, const void *buf, int len)
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
		if (n < 0 && gp_ic_wouldblock())
		{
			int			wake;
			int			holdoff = InterruptHoldoffCount;

			/* As in the read: a wait must let the interrupts through */
			InterruptHoldoffCount = 0;
			wake = WaitLatchOrSocket(MyLatch,
									 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH |
									 WL_SOCKET_WRITEABLE,
									 fd, -1L, gp_ic_client_event());
			if (wake & WL_LATCH_SET)
				ResetLatch(MyLatch);
			/* An interrupt is answered here, while nothing holds it back */
			CHECK_FOR_INTERRUPTS();
			InterruptHoldoffCount = holdoff;
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
	ResetLatch(MyLatch);
	(void) WaitLatchOrSocket(MyLatch,
							 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH |
							 WL_SOCKET_READABLE | WL_TIMEOUT,
							 fd, 1000,
							 WaitEventExtensionNew("GpInterconnectClose"));
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
			/* Tests stop a fragment here to see its cancellation */
			INJECTION_POINT("gp-interconnect-fragment", NULL);
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
		/*
		 * A catch block runs in ErrorContext, which CopyErrorData refuses;
		 * work in a context of this worker, as the recovery worker does
		 */
		MemoryContext worker_context =
			AllocSetContextCreate(TopMemoryContext, "gp interconnect worker",
								  ALLOCSET_DEFAULT_SIZES);
		MemoryContext oldcontext = MemoryContextSwitchTo(worker_context);
		ErrorData  *edata = CopyErrorData();

		AbortCurrentTransaction();
		(void) gp_ic_write_error(fd, edata->message);
		FreeErrorData(edata);
		EmitErrorReport();
		FlushErrorState();
		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(worker_context);
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

/* Bind a listener on gp.host; TCP port 0 asks the OS for a free port */
static pgsocket
gp_ic_listen(const char *port)
{
	struct addrinfo *addrs = NULL;
	struct addrinfo hints = {0};
	pgsocket	listener = PGINVALID_SOCKET;

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (is_absolute_path(gp_host_address()))
	{
		struct sockaddr_un addr;

		gp_ic_unix_path(gp_host_address(), atoi(port), &addr);
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
		return PGINVALID_SOCKET;
	for (struct addrinfo *ai = addrs; ai != NULL; ai = ai->ai_next)
	{
		listener = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (listener == PGINVALID_SOCKET)
			continue;
#ifdef IPV6_V6ONLY
		/*
		 * One listener serves both families, for a client may name either
		 * address; Windows keeps an IPv6 listener apart from IPv4 unless
		 * this says otherwise.  A system that cannot mix the families may
		 * still bind the IPv4 address below.
		 */
		if (ai->ai_family == AF_INET6)
		{
			int			off = 0;

			(void) setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY,
							  (const char *) &off, sizeof(off));
		}
#endif
		if (bind(listener, ai->ai_addr, ai->ai_addrlen) == 0 &&
			listen(listener, GP_IC_LISTEN_BACKLOG) == 0)
			break;
		closesocket(listener);
		listener = PGINVALID_SOCKET;
	}
	pg_freeaddrinfo_all(hints.ai_family, addrs);
	return listener;
}

/*
 * Bind the listener of a segment.  A Unix-domain socket keeps its usual
 * name, in the directory of its own segment, where no other process
 * competes for it.  Over TCP the port of the server says nothing about
 * what a bind may take: on Windows two sockets can hold the same port
 * unless one of them refuses, so the broker asks the operating system
 * for any free port and reports it, and the registry is the only address
 * there is.
 */
static pgsocket
gp_interconnect_listen(int *port)
{
	pgsocket	listener;
	char		service[12];

	if (is_absolute_path(gp_host_address()))
	{
		snprintf(service, sizeof(service), "%d", *port);
		return gp_ic_listen(service);
	}

	listener = gp_ic_listen("0");
	if (listener != PGINVALID_SOCKET)
	{
		struct sockaddr_storage addr;
		socklen_t	len = sizeof(addr);

		if (getsockname(listener, (struct sockaddr *) &addr, &len) == 0)
		{
			if (addr.ss_family == AF_INET)
				*port = ntohs(((struct sockaddr_in *) &addr)->sin_port);
			else
				*port = ntohs(((struct sockaddr_in6 *) &addr)->sin6_port);
			return listener;
		}
		closesocket(listener);
	}
	return PGINVALID_SOCKET;
}

/*
 * Tell the coordinator the port the broker listens on, so that clients need
 * no rule to find it.  A failure restarts the broker, which binds again and
 * reports again; nothing is served before the report is through.
 */
static void
gp_interconnect_register(int port)
{
	const char *keywords[] = {"dbname", "dbname", "application_name", NULL};
	const char *values[3];
	uint32		wait_event = WaitEventExtensionNew("GpInterconnectRegister");
	PGconn	   *conn;
	PGresult   *res;
	char		sql[64];

	values[0] = gp_coordinator_conninfo();
	values[1] = gp_database;
	values[2] = "gp interconnect";
	conn = libpqsrv_connect_params(keywords, values, true, wait_event);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		char	   *err = pchomp(PQerrorMessage(conn));

		libpqsrv_disconnect(conn);
		ereport(LOG,
				errmsg("could not register the interconnect port on the coordinator"),
				errdetail("%s", err));
		proc_exit(1);
	}
	snprintf(sql, sizeof(sql), "SELECT gp.register_interconnect(%d, %d)",
			 gp_segment_id, port);
	res = libpqsrv_exec_params(conn, sql, 0, NULL, NULL, NULL, NULL, 0,
							   wait_event);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		char	   *err = pchomp(PQresultErrorMessage(res));

		PQclear(res);
		libpqsrv_disconnect(conn);
		ereport(LOG,
				errmsg("could not register the interconnect port on the coordinator"),
				errdetail("%s", err));
		proc_exit(1);
	}
	PQclear(res);
	libpqsrv_disconnect(conn);
}

/* Invalidate a stale worker before freeing its slot for another generation */
static void
gp_ic_slot_invalidate(GpIcSlot *slot)
{
	SpinLockAcquire(&slot->lock);
	slot->generation++;
	slot->latch = NULL;
	pg_atomic_write_u32(&slot->state, GP_IC_SLOT_FREE);
	SpinLockRelease(&slot->lock);
}

/* Reap workers whose request has finished or whose startup has failed */
static void
gp_ic_reap_slots(void)
{
	for (int i = 0; i < GP_IC_SLOTS; i++)
	{
		GpIcSlot   *slot = &gp_ic_slots[i];
		GpIcSlotState state = pg_atomic_read_u32(&slot->state);
		pid_t		pid;

		if (gp_ic_handles[i] != NULL)
		{
			if (GetBackgroundWorkerPid(gp_ic_handles[i], &pid) == BGWH_STOPPED)
			{
				gp_ic_slot_invalidate(slot);
				pfree(gp_ic_handles[i]);
				gp_ic_handles[i] = NULL;
			}
			continue;
		}
		if (state == GP_IC_SLOT_FREE)
			continue;

		/* A stale STARTING generation must not block a replacement broker */
		if (state == GP_IC_SLOT_STARTING &&
			time(NULL) - slot->started >= 60)
		{
			gp_ic_slot_invalidate(slot);
			continue;
		}
		if (slot->pid != 0 &&
			GetBackgroundWorkerTypeByPid(slot->pid) != NULL)
		{
			/* Its old latch belongs to the broker that died */
			SpinLockAcquire(&slot->lock);
			slot->latch = NULL;
			SpinLockRelease(&slot->lock);
			continue;
		}
		gp_ic_slot_invalidate(slot);
	}
}

/*
 * The worker of a slot of this generation, if one is there: the
 * generation tells a worker of an earlier request from the worker of
 * this one.
 */
static pid_t
gp_ic_slot_worker(GpIcSlot *slot, uint64 generation)
{
	pid_t		pid = 0;

	SpinLockAcquire(&slot->lock);
	if (slot->generation == generation && slot->pid != 0)
		pid = slot->pid;
	SpinLockRelease(&slot->lock);
	return pid;
}

/*
 * Stop the worker of a request whose client went away.  The worker dies
 * on the signal, from the middle of a long fragment too, not only
 * between two rows.
 */
static void
gp_ic_cancel_slot(pgsocket fd)
{
	int32		slot_id;
	uint32		gen_hi;
	uint32		gen_lo;
	uint64		generation;
	pid_t		pid;

	if (!gp_ic_read_i32(fd, &slot_id) ||
		!gp_ic_read(fd, &gen_hi, sizeof(gen_hi)) ||
		!gp_ic_read(fd, &gen_lo, sizeof(gen_lo)))
		return;
	generation = ((uint64) ntohl(gen_hi) << 32) | ntohl(gen_lo);
	if (slot_id < 0 || slot_id >= GP_IC_SLOTS)
		return;

	pid = gp_ic_slot_worker(&gp_ic_slots[slot_id], generation);
	if (pid == 0)
		ereport(LOG,
				errmsg("no interconnect worker to stop in slot %d", slot_id));
	else if (kill(pid, SIGINT) != 0)
		ereport(LOG,
				errmsg("could not stop the interconnect worker of slot %d: %m",
					   slot_id));
	else
		ereport(LOG,
				errmsg("stopped the interconnect worker of slot %d", slot_id));
}

/* Assign one request a dynamic worker and its own ephemeral listener */
static bool gp_ic_slot_assignment(GpIcSlot *slot, uint64 generation,
								 uint32 *port);

static bool
gp_ic_assign(pgsocket fd, const char *token)
{
	char		magic[GP_IC_MAGIC_LEN];
	char	   *provided;
	char	   *db = NULL;
	char	   *user = NULL;
	char	   *sql = NULL;
	int32		requester;
	int32		len;
	int			slot_id = -1;
	BackgroundWorker worker = {0};
	GpIcSlot   *slot = NULL;
	pid_t		pid;
	uint64		generation = 0;
	bool		assigned = false;

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
	pfree(provided);

	/*
	 * A request begins with the length of its database; a cancel, with a
	 * negative one followed by the worker it is for
	 */
	if (!gp_ic_read_i32(fd, &len))
		return false;
	if (len == -1)
	{
		gp_ic_cancel_slot(fd);
		gp_ic_shutdown_write(fd);
		return true;
	}
	if (len < 0 || len > GP_IC_MAX_STRING)
		return false;
	db = palloc(len + 1);
	if (!gp_ic_read(fd, db, len))
		return false;
	db[len] = '\0';
	user = gp_ic_read_string(fd);
	if (user == NULL ||
		!gp_ic_read_i32(fd, &requester))
		return false;
	sql = gp_ic_read_string(fd);
	if (sql == NULL)
		return false;
	/* The plan is sent again to the assigned worker, not kept in shmem */
	pfree(sql);

	gp_ic_reap_slots();
	for (int i = 0; i < GP_IC_SLOTS; i++)
		if (pg_atomic_read_u32(&gp_ic_slots[i].state) == GP_IC_SLOT_FREE &&
			gp_ic_handles[i] == NULL)
		{
			slot_id = i;
			break;
		}
	if (slot_id >= 0)
	{
		slot = &gp_ic_slots[slot_id];
		SpinLockAcquire(&slot->lock);
		strlcpy(slot->db, db, sizeof(slot->db));
		strlcpy(slot->user, user, sizeof(slot->user));
		slot->latch = MyLatch;
		slot->pid = 0;
		slot->started = time(NULL);
		slot->generation++;
		generation = slot->generation;
		pg_atomic_write_u32(&slot->port, 0);
		pg_atomic_write_u32(&slot->state, GP_IC_SLOT_STARTING);
		SpinLockRelease(&slot->lock);

		worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
			BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = BGW_NEVER_RESTART;
		worker.bgw_main_arg = UInt64GetDatum((generation << 16) |
										   (uint64) slot_id);
		strlcpy(worker.bgw_library_name, "gp", BGW_MAXLEN);
		strlcpy(worker.bgw_function_name, "gp_interconnect_request_main", BGW_MAXLEN);
		strlcpy(worker.bgw_name, "gp interconnect request", BGW_MAXLEN);
		strlcpy(worker.bgw_type, "gp interconnect request", BGW_MAXLEN);
		if (!RegisterDynamicBackgroundWorker(&worker, &gp_ic_handles[slot_id]))
		{
			gp_ic_slot_invalidate(slot);
			gp_ic_handles[slot_id] = NULL;
			slot = NULL;
		}
	}

	for (int i = 0; slot != NULL && i < 600; i++)
	{
		uint32		port;

		if (gp_ic_slot_assignment(slot, generation, &port))
		{
			char		answer[16];
			int32		net_slot = htonl(slot_id);
			uint32		gen_hi = htonl((uint32) (generation >> 32));
			uint32		gen_lo = htonl((uint32) generation);
			uint32		net_port = htonl(port);

			/*
			 * The port of the worker, and its slot and generation, which
			 * the client names back to stop the worker when it goes away
			 */
			memcpy(answer + 0, &net_port, sizeof(net_port));
			memcpy(answer + 4, &net_slot, sizeof(net_slot));
			memcpy(answer + 8, &gen_hi, sizeof(gen_hi));
			memcpy(answer + 12, &gen_lo, sizeof(gen_lo));
			assigned = gp_ic_write_message(fd, GP_IC_ASSIGN,
										   answer, sizeof(answer));
			break;
		}
		if (GetBackgroundWorkerPid(gp_ic_handles[slot_id], &pid) == BGWH_STOPPED)
			break;
		ResetLatch(MyLatch);
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH |
						 WL_TIMEOUT, 100, WaitEventExtensionNew("GpInterconnectAssign"));
	}
	pfree(db);
	pfree(user);
	if (!assigned)
		(void) gp_ic_write_message(fd, GP_IC_RETRY, NULL, 0);
	if (!assigned)
		gp_ic_shutdown_write(fd);

	/*
	 * A client that went away while its worker was starting left no one
	 * to connect to it and no one to name it in a cancel: the worker
	 * would wait out its whole minute before it goes by itself
	 */
	if (!assigned && slot != NULL)
	{
		pid_t		orphan = gp_ic_slot_worker(slot, generation);

		if (orphan != 0)
			(void) kill(orphan, SIGINT);
	}
	return assigned;
}

static void
gp_ic_slot_signal(GpIcSlot *slot)
{
	if (slot->latch != NULL)
		SetLatch(slot->latch);
}

static void
gp_ic_slot_publish(GpIcSlot *slot, uint64 generation, GpIcSlotState state)
{
	SpinLockAcquire(&slot->lock);
	if (slot->generation == generation)
	{
		pg_atomic_write_u32(&slot->state, state);
		gp_ic_slot_signal(slot);
	}
	SpinLockRelease(&slot->lock);
}

static void
gp_ic_slot_set_port(GpIcSlot *slot, uint64 generation, uint32 port)
{
	SpinLockAcquire(&slot->lock);
	if (slot->generation == generation)
		pg_atomic_write_u32(&slot->port, port);
	SpinLockRelease(&slot->lock);
}

static bool
gp_ic_slot_start(GpIcSlot *slot, uint64 generation)
{
	bool		current;

	SpinLockAcquire(&slot->lock);
	current = slot->generation == generation;
	if (current)
		slot->pid = MyProcPid;
	SpinLockRelease(&slot->lock);
	return current;
}

static bool
gp_ic_slot_assignment(GpIcSlot *slot, uint64 generation, uint32 *port)
{
	bool		ready;

	SpinLockAcquire(&slot->lock);
	ready = slot->generation == generation &&
		pg_atomic_read_u32(&slot->state) == GP_IC_SLOT_READY;
	if (ready)
	{
		*port = pg_atomic_read_u32(&slot->port);
		pg_atomic_write_u32(&slot->state, GP_IC_SLOT_ASSIGNED);
	}
	SpinLockRelease(&slot->lock);
	return ready;
}

/* A dynamic worker accepts one request on a listener of its own */
void
gp_interconnect_request_main(Datum main_arg)
{
	uint64		request = DatumGetUInt64(main_arg);
	int			slot_id = request & 0xffff;
	uint64		generation = request >> 16;
	GpIcSlot   *slot = &gp_ic_slots[slot_id];
	pgsocket	listener;
	pgsocket	fd;
	char		service[12];
	char	   *token;
	uint32		wait_event;

	/* The broker stops a worker this way when its client is gone */
	pqsignal(SIGTERM, die);
	pqsignal(SIGINT, die);
	BackgroundWorkerUnblockSignals();
	if (!gp_ic_slot_start(slot, generation))
		proc_exit(0);
	if (is_absolute_path(gp_host_address()))
		snprintf(service, sizeof(service), "%u",
				 (unsigned) (PostPortNumber + 20000 + (generation % 20000)));
	else
		strlcpy(service, "0", sizeof(service));
	listener = gp_ic_listen(service);
	if (listener == PGINVALID_SOCKET)
	{
		gp_ic_slot_publish(slot, generation, GP_IC_SLOT_FREE);
		proc_exit(1);
	}
	if (!is_absolute_path(gp_host_address()))
	{
		struct sockaddr_storage addr;
		socklen_t	len = sizeof(addr);

		if (getsockname(listener, (struct sockaddr *) &addr, &len) != 0)
		{
			closesocket(listener);
			gp_ic_slot_publish(slot, generation, GP_IC_SLOT_FREE);
			proc_exit(1);
		}
		if (addr.ss_family == AF_INET)
			gp_ic_slot_set_port(slot, generation,
								ntohs(((struct sockaddr_in *) &addr)->sin_port));
		else
			gp_ic_slot_set_port(slot, generation,
								ntohs(((struct sockaddr_in6 *) &addr)->sin6_port));
	}
	else
		gp_ic_slot_set_port(slot, generation,
							PostPortNumber + 20000 + (generation % 20000));
	pg_write_barrier();
	gp_ic_slot_publish(slot, generation, GP_IC_SLOT_READY);

	if (!pg_set_noblock(listener))
	{
		closesocket(listener);
		gp_ic_slot_publish(slot, generation, GP_IC_SLOT_FREE);
		proc_exit(1);
	}
	wait_event = WaitEventExtensionNew("GpInterconnectRequest");
	ResetLatch(MyLatch);
	(void) WaitLatchOrSocket(MyLatch,
							 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH |
							 WL_SOCKET_READABLE | WL_SOCKET_ACCEPT | WL_TIMEOUT,
							 listener, 60000, wait_event);
	CHECK_FOR_INTERRUPTS();
	fd = accept(listener, NULL, NULL);
	closesocket(listener);
	if (fd == PGINVALID_SOCKET || !pg_set_block(fd))
	{
		if (fd != PGINVALID_SOCKET)
			closesocket(fd);
		gp_ic_slot_publish(slot, generation, GP_IC_SLOT_FREE);
		proc_exit(1);
	}
	gp_ic_slot_publish(slot, generation, GP_IC_SLOT_BUSY);
	token = gp_interconnect_token();
	if (token != NULL)
	{
		(void) gp_ic_serve(fd, token);
		closesocket(fd);
	}
	gp_ic_slot_publish(slot, generation, GP_IC_SLOT_FREE);
	proc_exit(0);
}

/* The listener of a segment, one request at a time */
void
gp_interconnect_main(Datum main_arg)
{
	pgsocket listener = PGINVALID_SOCKET;
	int			listen_port;
	char	   *token;
	uint32		wait_event;

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	listen_port = PostPortNumber + GP_IC_PORT_OFFSET;
	listener = gp_interconnect_listen(&listen_port);
	if (listener == PGINVALID_SOCKET)
	{
		ereport(LOG,
				errmsg("could not listen on interconnect port %d",
					   listen_port));
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

	/* The row of this segment exists once its registration is through */
	gp_interconnect_register(listen_port);

	gp_ic_reap_slots();
	wait_event = WaitEventExtensionNew("GpInterconnect");

	for (;;)
	{
		long		rc;
		pgsocket	fd;

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
		(void) gp_ic_assign(fd, token);
		closesocket(fd);
	}
}

static void
gp_ic_shmem_request(void *arg)
{
	ShmemRequestStruct(.name = "gp interconnect requests",
					   .size = mul_size(sizeof(GpIcSlot), GP_IC_SLOTS),
					   .ptr = (void **) &gp_ic_slots);
}

static void
gp_ic_shmem_init(void *arg)
{
	MemSet(gp_ic_slots, 0, sizeof(GpIcSlot) * GP_IC_SLOTS);
	for (int i = 0; i < GP_IC_SLOTS; i++)
	{
		SpinLockInit(&gp_ic_slots[i].lock);
		pg_atomic_init_u32(&gp_ic_slots[i].state, GP_IC_SLOT_FREE);
		pg_atomic_init_u32(&gp_ic_slots[i].port, 0);
	}
}

static const ShmemCallbacks gp_ic_shmem_callbacks = {
	.request_fn = gp_ic_shmem_request,
	.init_fn = gp_ic_shmem_init,
};

/* Called from _PG_init() on a segment */
void
gp_interconnect_init(void)
{
	BackgroundWorker worker = {0};

	RegisterShmemCallbacks(&gp_ic_shmem_callbacks);

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

/* Connect to the broker or to an assigned worker */
static pgsocket
gp_ic_connect(const char *host, int port)
{
	struct addrinfo *addrs = NULL;
	struct addrinfo hints = {0};
	pgsocket	fd = PGINVALID_SOCKET;
	char		portstr[12];

	snprintf(portstr, sizeof(portstr), "%d", port);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (is_absolute_path(host))
	{
		struct sockaddr_un addr;

		gp_ic_unix_path(host, port, &addr);
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
	return fd;
}

/*
 * Ask the broker to stop the worker of this request.  The client sends
 * this when it dies or gives up, so that a fragment long between two
 * rows does not run on.  Nothing waits for an answer: the client is on
 * its way out, and the broker stops the worker or it is already gone.
 */
static void
gp_ic_cancel_request(const char *host, int port, const char *token,
					 int32 slot_id, uint64 generation)
{
	char		message[GP_IC_MAGIC_LEN + 4 + 64 + 4 + 4 + 8];
	char	   *p = message;
	int32		net_len = htonl(64);
	int32		net_cancel = htonl(-1);
	int32		net_slot = htonl(slot_id);
	uint32		gen_hi = htonl((uint32) (generation >> 32));
	uint32		gen_lo = htonl((uint32) generation);
	pgsocket	fd;

	memcpy(p, GP_IC_MAGIC, GP_IC_MAGIC_LEN);
	p += GP_IC_MAGIC_LEN;
	memcpy(p, &net_len, sizeof(net_len));
	p += sizeof(net_len);
	memcpy(p, token, 64);
	p += 64;
	memcpy(p, &net_cancel, sizeof(net_cancel));
	p += sizeof(net_cancel);
	memcpy(p, &net_slot, sizeof(net_slot));
	p += sizeof(net_slot);
	memcpy(p, &gen_hi, sizeof(gen_hi));
	p += sizeof(gen_hi);
	memcpy(p, &gen_lo, sizeof(gen_lo));

	/* The cancel must not fail the failure it answers */
	PG_TRY();
	{
		fd = gp_ic_connect(host, port);
		if (fd == PGINVALID_SOCKET ||
			!gp_ic_write(fd, message, sizeof(message)))
			ereport(LOG,
					errmsg("could not reach the interconnect broker of %s to stop slot %d",
						   host, slot_id));
		if (fd != PGINVALID_SOCKET)
			closesocket(fd);
	}
	PG_CATCH();
	{
		ereport(LOG,
				errmsg("could not ask %s to stop interconnect slot %d",
					   host, slot_id));
		FlushErrorState();
	}
	PG_END_TRY();
}

static bool
gp_ic_write_i32_latched(pgsocket fd, int32 value)
{
	uint32		net = htonl((uint32) value);

	return gp_ic_write_latched(fd, &net, sizeof(net));
}

/* Send the full request over a nonblocking socket */
static bool
gp_ic_send_query_latched(pgsocket fd, const char *token,
						 const char *db, const char *user, int requester,
						 const char *sql)
{
	return gp_ic_write_latched(fd, GP_IC_MAGIC, GP_IC_MAGIC_LEN) &&
		gp_ic_write_i32_latched(fd, strlen(token)) &&
		gp_ic_write_latched(fd, token, strlen(token)) &&
		gp_ic_write_i32_latched(fd, strlen(db)) &&
		gp_ic_write_latched(fd, db, strlen(db)) &&
		gp_ic_write_i32_latched(fd, strlen(user)) &&
		gp_ic_write_latched(fd, user, strlen(user)) &&
		gp_ic_write_i32_latched(fd, requester) &&
		gp_ic_write_i32_latched(fd, strlen(sql)) &&
		gp_ic_write_latched(fd, sql, strlen(sql));
}

/*
 * Ask one peer for its rows; false asks for a dispatched-connection
 * fallback.  The port is that of the peer's broker, not of its server
 */
bool
gp_interconnect_query_try(Tuplestorestate *store, TupleDesc tupdesc,
					  const char *db, const char *user, const char *sql,
					  const char *host, int port, int requester)
{
	pgsocket	fd = PGINVALID_SOCKET;
	FmgrInfo   *recv;
	Oid		   *ioparam;
	MemoryContext rowcxt;
	char	   *token;
	bool		retry = false;
	bool		have_target = false;
	int32		slot_id = 0;
	uint64		generation = 0;

	token = gp_interconnect_token();
	if (token == NULL)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("the interconnect token is not installed yet"));

	fd = gp_ic_connect(host, port);
	if (fd == PGINVALID_SOCKET)
		return false;
	if (!pg_set_noblock(fd))
	{
		closesocket(fd);
		return false;
	}

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
		if (!gp_ic_send_query_latched(fd, token, db, user, requester,
									  sql))
			retry = true;

		/* The broker answers with the worker of the request */
		if (!retry)
		{
			char		type;
			int32		len;
			char		answer[16];
			int			worker_port = 0;

			if (!gp_ic_read_latched(fd, &type, 1) ||
				!gp_ic_read_i32_latched(fd, &len) ||
				len < 0 || len > GP_IC_MAX_STRING)
				retry = true;
			else if (type == GP_IC_ASSIGN && len == sizeof(answer))
			{
				uint32		net_port;
				uint32		gen_hi;
				uint32		gen_lo;

				if (!gp_ic_read_latched(fd, answer, sizeof(answer)))
					retry = true;
				else
				{
					memcpy(&net_port, answer + 0, sizeof(net_port));
					memcpy(&slot_id, answer + 4, sizeof(slot_id));
					memcpy(&gen_hi, answer + 8, sizeof(gen_hi));
					memcpy(&gen_lo, answer + 12, sizeof(gen_lo));
					worker_port = (int) ntohl(net_port);
					slot_id = ntohl(slot_id);
					generation = ((uint64) ntohl(gen_hi) << 32) | ntohl(gen_lo);
					have_target = true;
				}
			}
			else if (type == GP_IC_RETRY && len == 0)
				retry = true;
			else if (type == GP_IC_ERROR)
			{
				char	   *data = palloc(len);

				if (!gp_ic_read_latched(fd, data, len))
					retry = true;
				else
					ereport(ERROR,
							errcode(ERRCODE_INTERNAL_ERROR),
							errmsg("the segment returned an error: %.*s", len, data));
			}
			else
				ereport(ERROR,
						errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("an unknown assignment of the interconnect"));
			closesocket(fd);
			fd = PGINVALID_SOCKET;
			if (!retry)
			{
				fd = gp_ic_connect(host, worker_port);
				if (fd == PGINVALID_SOCKET)
					retry = true;
				else if (!pg_set_noblock(fd))
				{
					closesocket(fd);
					fd = PGINVALID_SOCKET;
					retry = true;
				}
				else
				{
					if (!gp_ic_send_query_latched(fd, token, db, user,
												  requester, sql))
						retry = true;
				}
			}
		}

		/* Until GO is sent, the peer has not executed the fragment */
		if (!retry)
		{
			char		type;
			int32		len;
			char	   *data;
			char		go = GP_IC_GO;

			if (!gp_ic_read_latched(fd, &type, 1) ||
				!gp_ic_read_i32_latched(fd, &len) ||
				len < 0 || len > GP_IC_MAX_STRING)
				retry = true;
			else
			{
				data = palloc(len);
				if (!gp_ic_read_latched(fd, data, len))
					retry = true;
				else if (type == GP_IC_RETRY && len == 0)
					retry = true;
				else if (type == GP_IC_READY && len == 0)
				{
					if (!gp_ic_write_latched(fd, &go, 1))
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
			if (!gp_ic_read_latched(fd, &type, 1) ||
				!gp_ic_read_i32_latched(fd, &len) ||
				len < 0 || len > GP_IC_MAX_STRING)
				ereport(ERROR,
						errcode(ERRCODE_CONNECTION_FAILURE),
						errmsg("could not read the interconnect answer of %s", host));
			data = palloc(len);
			if (!gp_ic_read_latched(fd, data, len))
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

		/* A request that never ran to its end does not run on */
		if (retry && have_target)
			gp_ic_cancel_request(host, port, token, slot_id, generation);
	}
	PG_CATCH();
	{
		if (have_target)
			gp_ic_cancel_request(host, port, token, slot_id, generation);
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
