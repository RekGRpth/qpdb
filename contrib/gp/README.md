# gp: a cluster of PostgreSQL servers

`gp` turns several PostgreSQL servers into one MPP cluster in the manner of
Greenplum: a **coordinator** that clients connect to, and **segments** that
hold the data and run parts of queries.  It is an extension plus a single
patch of the server, `new_oid_hook`, rather than a fork.

**Status: under development, not for production.**  The cluster forms,
segments register themselves, and DDL of the coordinator runs on the
segments with the same OIDs.  Distributed queries and distributed
transactions are yet to come; see [Roadmap](#roadmap).

## Requirements

- PostgreSQL master (the development of PostgreSQL 20) with the
  `new_oid_hook` patch.  The extension refuses to build without it
  (`NEW_OID_HOOK_VERSION`).  The patch is proposed to pgsql-hackers; until
  it is in, the server is built from this tree.
- `gp` in `shared_preload_libraries` on every server of the cluster.
- `max_prepared_transactions` above zero on the segments, for two-phase
  commit of DDL.

## Architecture

### Servers and their roles

Every server of a cluster loads `gp` and has a role, `gp.role`:

| `gp.role` | Number, `gp.segment_id` | What it does |
|---|---|---|
| `none` (default) | 0 | Works on its own; `gp` does nothing |
| `coordinator` | 0 | Takes clients, keeps the configuration of the cluster, sends commands to the segments |
| `segment` | 1, 2, ... | Holds data, registers on the coordinator, runs what the coordinator sends |

A segment number is the name of a server, not its position: numbers may
have gaps, which keeps the cluster open to growing and shrinking by moving
buckets (below).  A misconfigured server does not start: a segment needs
`gp.segment_id` and `gp.coordinator`, and other roles may not have them.

### Segments of the cluster (`segments.c`, `gp.c`)

The coordinator keeps the segments in the table `gp.segments` of one
database, `gp.database` (`postgres` by default).  Sessions in any database
need them, so a background worker of the coordinator, the *segments
loader*, keeps a copy in shared memory of `gp.max_segments` entries.  It
reads the table at start and again whenever a transaction that changed it
commits: a statement trigger on the table asks for that, and an object
access hook when the table is dropped.  `gp.cluster_segments()` shows the
copy.

A segment registers itself: at start, its background worker connects to
`gp.coordinator`, calls `gp.register_segment(segment_id, gp.host, port)` in
`gp.database` and exits.  While the coordinator is down or has no
extension, the worker retries every `gp.register_retry_interval`, so
segments may start before the coordinator.  A number registered at another
address, or an address under another number, is refused.

### Connections and commands (`dispatch.c`)

A session of the coordinator opens a connection to a segment when it needs
it first and keeps it until it ends.  It connects to the database of the
session as the session user, without a password: segments trust the
coordinator by `pg_hba.conf`, certificates or a password file.

Before a command, the segments get the settings of the session: those a
user may change, set for the database, the role, the client or the session
rather than in the configuration of the server, and `SET ROLE`.  A
connection remembers what it has got and gets only the difference.

`gp.exec_on_all(sql)` sends a command to all segments at once and returns
the command status of each.  Each segment runs it in a transaction of its
own, unless the transaction of the coordinator has sent DDL to the segment
already: then the command runs in that transaction and sees the DDL.  A
cancel or an abort on the coordinator cancels it on the segments.  It is a
tool for maintenance and tests.

### The same OIDs everywhere (`oids.c`)

A plan refers to tables, types and functions by OID, so an object must have
the same OID on every server.  With `new_oid_hook`, an extension may
propose the OID of a new catalog row; the server checks it and refuses one
in use.

- On the coordinator, the hook takes each OID from `GetNewOidWithIndex()`
  itself and records it with its catalog; `gp.recorded_oids()` shows the
  OIDs of the transaction.
- A segment gets the OIDs of a command in `gp.dispatch_oids` and proposes
  them in turn, failing if one is for another catalog, if it needs more, or
  if some are left over at commit: then it has run the command differently.
- A segment creates no objects of its own unless `gp.allow_local_ddl` is
  on, as they would take OIDs that the coordinator may assign later.

Relfilenumbers stay local and may differ.

### DDL on the segments (`ddl.c`)

A DDL statement on the coordinator runs there first; then the segments run
the same statement, its own text cut out of the query string, with the
OIDs it got on the coordinator.  Statements nested in a sent one (elements
of `CREATE SCHEMA`) and statements of extension scripts are not sent on
their own.  DDL in a function is sent when the function runs on the
coordinator.

What goes to the segments: `CREATE`, `ALTER` and `DROP` of tables, indexes,
sequences and views; `CREATE SCHEMA` and `DROP SCHEMA`; composite, enum and
range types, domains, `ALTER TYPE ... ADD VALUE`; `CREATE` and `ALTER` of
functions and procedures; renaming and moving to another schema; roles,
from any database, with `GRANT role`, `ALTER ROLE ... SET`, `REASSIGN
OWNED` and `DROP OWNED`; `GRANT` and `REVOKE` on objects and databases,
`ALTER DEFAULT PRIVILEGES`; `COMMENT`; `OWNER TO`.  Commands that cannot run
in a transaction block are sent after the coordinator (see below); other
statements run on the coordinator only.

What stays on the coordinator: temporary objects, objects of extensions
and the schema of `gp`; one statement may not mix them with other
objects.  What is refused: `ALTER TABLE` that computes a column with a
non-immutable expression (`ADD COLUMN ... DEFAULT now()`, `serial`,
identity, `ALTER COLUMN TYPE ... USING random()`), as each segment would
compute its own values.

The segments run DDL in transactions that follow the transaction of the
coordinator: the first statement opens one, and the end of the transaction
of the coordinator ends them by two-phase commit.  Before the coordinator
commits, each segment runs `PREPARE TRANSACTION 'gp_<xid>'`, `<xid>` being
the transaction of the coordinator; a segment that cannot fails the commit,
and the others roll back.  After the coordinator commits, the segments run
`COMMIT PREPARED`.  An error on a segment at any statement fails the
transaction everywhere.  A subtransaction cannot be rolled back on the
segments yet, so a transaction that rolled back one after sending DDL
cannot commit.  `gp.dispatch_ddl = off` keeps DDL on the coordinator, for
maintenance.

A segment lost between the commit of the coordinator and `COMMIT PREPARED`
keeps its transaction prepared, holding its locks, and the coordinator
warns.  Until recovery comes, resolve it by hand: `pg_xact_status('<xid>')`
on the coordinator tells whether to run `COMMIT PREPARED 'gp_<xid>'` or
`ROLLBACK PREPARED 'gp_<xid>'` on the segment; `pg_prepared_xacts` there
lists such transactions.

#### Commands outside a transaction

`CREATE DATABASE`, `DROP DATABASE`, `VACUUM`, `CREATE INDEX CONCURRENTLY`
and `DROP INDEX CONCURRENTLY` cannot run in a transaction block, so the
coordinator runs them first and then all segments at once, each in a
transaction of its own.  Two-phase commit does not cover them.  The
coordinator is the first to run them, as it gives the OIDs of `CREATE
DATABASE` and `CREATE INDEX CONCURRENTLY`.  A segment gets them by `SET
gp.dispatch_oids` in the session, with `gp.dispatch_oids_session` on, in a
command of its own, and both are reset after.

When a segment fails, the connections to the segments are closed, which
ends what they keep of the command, and:

- `CREATE DATABASE` is dropped on the segments where it succeeded, and the
  coordinator rolls back; its directory of files may stay there.
- `DROP DATABASE` has removed the files on the coordinator already, so a
  failure on a segment is a `WARNING`.  Run `DROP DATABASE IF EXISTS` again
  to finish.
- `CREATE INDEX CONCURRENTLY` leaves the index on the coordinator and the
  segments that built it, and an invalid one where it failed.  Drop it with
  `DROP INDEX`.

`VACUUM` goes without OIDs, and `VACUUM FULL` lets the segments create
relations for the rewrite (`gp.allow_local_ddl`, so only for superusers).  A
list of tables that mixes temporary ones or those of extensions with
others is refused; `ANALYZE` is not sent, as the statistics of the cluster
are for a later step.

### Distribution (`distribution.c`)

`gp.distribute(table, key, colocate_with)` distributes an empty table by
hash of its key columns, or replicates it to all segments if the key is
`NULL`.  A row goes to a bucket by `gp.bucket()`, the extended hash
functions of the key types combined as hash partitioning does, with a seed
of its own; the bucket goes to a segment by a map.

The map belongs to a *colocation group*, not to a table: tables of a group
have keys of the same types and the same number of buckets (1024), so rows
with equal keys are on the same segment and a join by the key is local.  A
new group spreads its buckets over the segments in turn;
`gp.bucket_segment()` reads the map.  Growing or shrinking the cluster will
move buckets, about 1/N of the data, rather than rehash everything.

Policies live in each database (`gp.distribution_policy`,
`gp.colocation_groups`).  An event trigger drops the policy of a dropped
table.

## Trying it

Build the tree with Meson or make; `contrib/gp` is built with the rest.
Then, with the server installed:

```sh
export PGBIN=/path/to/install/bin     # or have its pg_config in PATH
contrib/gp/tools/gp-cluster.sh init   # coordinator on 9000, segments on 9001-9003
                                      # with max_prepared_transactions = 64
contrib/gp/tools/gp-cluster.sh start  # creates extension gp, waits for segments
contrib/gp/tools/gp-cluster.sh status
psql -h localhost -p 9000 postgres -c 'SELECT * FROM gp.cluster_segments()'
contrib/gp/tools/gp-cluster.sh destroy
```

`GP_CLUSTER_DIR`, `GP_PORT` and `GP_SEGMENTS` change the directory
(`~/gp-cluster`), the first port and the number of segments.

## Reference

### Settings

All are set at server start unless noted.

| Setting | Default | Meaning |
|---|---|---|
| `gp.role` | `none` | `none`, `coordinator` or `segment` |
| `gp.segment_id` | 0 | Number of the server: 0 on the coordinator, from 1 on segments |
| `gp.database` | `postgres` | Database with the configuration of the cluster |
| `gp.coordinator` | | Connection string of the coordinator; on segments only |
| `gp.host` | `localhost` | Host of a segment for the other servers |
| `gp.register_retry_interval` | 5s | Time between attempts of a segment to register |
| `gp.max_segments` | 64 | Size of the copy of `gp.segments` on the coordinator |
| `gp.dispatch_oids` | | OIDs of a command from the coordinator; superuser, set per command |
| `gp.dispatch_oids_session` | off | Keeps `gp.dispatch_oids` for the next transaction, for a command out of a transaction block; superuser |
| `gp.allow_local_ddl` | off | Lets a segment create objects of its own; superuser, for maintenance |
| `gp.dispatch_ddl` | on | Sends DDL of the coordinator to the segments; superuser, for maintenance |

### Functions

| Function | What it does |
|---|---|
| `gp.register_segment(segment_id, host, port)` | Register a segment; called by the segment itself |
| `gp.cluster_segments()` | Segments as the coordinator knows them, from any database |
| `gp.exec_on_all(sql)` | Run a command on all segments, return the status of each |
| `gp.recorded_oids()` | OIDs of the new catalog rows of the transaction on the coordinator |
| `gp.distribute(table, key, colocate_with)` | Distribute an empty table by hash of `key`, or replicate it |
| `gp.bucket(nbuckets, VARIADIC key)` | Bucket of a key |
| `gp.bucket_segment(colocation_id, bucket)` | Segment of a bucket of a colocation group |

`gp.register_segment()`, `gp.exec_on_all()` and `gp.distribute()` are for
superusers only.

## Tests

`sql/gp.sql` runs on a single server with `gp` preloaded.  TAP tests in `t/`
start their own clusters:

| Test | What it checks |
|---|---|
| `001_cluster.pl` | A coordinator and segments start with their roles |
| `002_gp_cluster.pl` | `tools/gp-cluster.sh` through its whole cycle |
| `003_register.pl` | Registration rules; misconfigured servers do not start |
| `004_register_worker.pl` | Segments register themselves, before or after the coordinator starts |
| `005_cluster_segments.pl` | The copy of `gp.segments` follows commits, rollbacks and drops |
| `006_exec_on_all.pl` | Commands on all segments, errors, broken connections, cancels |
| `007_session_settings.pl` | Settings of the session reach the segments |
| `008_distribution.pl` | Distribution policies and colocation groups |
| `009_same_oids.pl` | Objects get the OIDs of the coordinator on a segment |
| `010_ddl.pl` | DDL of the coordinator on the segments: statements, transactions, what stays, what is refused |
| `011_two_phase.pl` | Two-phase commit of DDL, a segment that cannot prepare, a segment lost before `COMMIT PREPARED` |
| `012_roles.pl` | Roles, privileges, comments and owners on the segments |
| `013_nonxact.pl` | `CREATE DATABASE`, `DROP DATABASE`, `VACUUM` and the `CONCURRENTLY` commands on the segments, and their failures |

Besides, CI runs the whole test suite of PostgreSQL twice: on the patched
server without `gp`, and with `gp` preloaded on a single server, to show
that the patch and the extension change nothing while not in use.
`tools/check-core-diff.sh` checks that the tree differs from upstream only
by the patch and `contrib/gp`.

## Roadmap

1. **Cluster skeleton** — done: roles, registration, the copy of segments,
   connections, settings of sessions, distribution policies.
2. **DDL and the same OIDs** — in progress: the same OIDs, propagating DDL
   with roles, privileges, comments and owners, its two-phase commit and
   commands that run outside a transaction are done; next is `CREATE
   EXTENSION` on the segments.
3. **Placing data and correct SELECT** — routing of `INSERT` and `COPY`,
   plan fragments on segments, every query correct through a gather on the
   coordinator.
4. **Distributed transactions and DML** — two-phase commit of all writes,
   distributed snapshots, `UPDATE` and `DELETE`.
5. **MPP planner without interconnect** — locus, slices; colocated joins
   and aggregates on the segments.
6. **Interconnect** — Redistribute and Broadcast motions between segments.
7. **Statistics** — `ANALYZE` on the segments, statistics on the
   coordinator.
8. **More kinds of distribution** — replicated with the coordinator, on the
   coordinator only, on one segment.
9. **Management without ssh** — systemd units, cluster state views,
   settings on all servers.

Later: mirrors, growing and shrinking the cluster by moving buckets,
two-stage aggregates, range and list distribution.

## License

PostgreSQL License, see [LICENSE](LICENSE).
