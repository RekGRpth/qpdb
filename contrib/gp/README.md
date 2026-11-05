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
session as the user that logged in, without a password: segments trust the
coordinator by `pg_hba.conf`, certificates or a password file.  `SET
SESSION AUTHORIZATION` may take the session to a role that cannot log in,
so the connection always takes the login user.

Before a command, the segments get the settings of the session: those a
user may change, set for the database, the role, the client or the session
rather than in the configuration of the server, and the role of the
commands, which follows `SET ROLE`, `SET SESSION AUTHORIZATION` and
`SECURITY DEFINER` functions alike, so a command runs on the segments as
the same user as on the coordinator and the objects it creates get the
same owner.  A connection remembers what it has got and gets only the
difference.

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

A unique index of a distributed table must include the whole key of its
distribution: a segment sees its own rows only, so an index without the
key cannot tell two equal rows apart when they sit on different segments.
A table distributed by hand learns this from `gp.distribute()`, and a
statement that makes such an index on a distributed table fails.

What goes to the segments: `CREATE`, `ALTER` and `DROP` of tables, indexes,
sequences and views; `CREATE SCHEMA` and `DROP SCHEMA`; composite, enum and
range types, domains, `ALTER TYPE ... ADD VALUE`; base and shell types,
operators, aggregates, casts, collations, conversions, operator classes and
families, text search objects, transforms and access methods; extended
statistics, which hold no rows of their own; `CREATE` and
`ALTER` of functions and procedures, whose bodies the segments do not check
(`check_function_bodies = off`), as a body may use a temporary table;
renaming and moving to another schema; roles,
from any database, with `GRANT role`, `ALTER ROLE ... SET`, `REASSIGN
OWNED` and `DROP OWNED`; `GRANT` and `REVOKE` on objects and databases,
`ALTER DEFAULT PRIVILEGES`; `COMMENT`; `OWNER TO`; `CREATE`, `ALTER` and
`DROP EXTENSION`; `TRUNCATE`.  Commands that cannot run
in a transaction block are sent after the coordinator (see below); other
statements run on the coordinator only.

An extension goes as a whole: the segments run its script themselves, with
the OIDs of the coordinator, so it must be installed on them in the same
version, or the statement fails everywhere.  Its objects are then on the
segments, and DDL on them (`GRANT`, `COMMENT`, `ALTER EXTENSION ... SET
SCHEMA`) goes there as well.

What stays on the coordinator: temporary objects (functions and types of
`pg_temp` too), extension `gp` with its schema and objects, and an object
that depends on a temporary one, which stays with it — a function of a
temporary sequence or type, a domain of a temporary type; one statement may
not mix them with other objects.  A table `LIKE` a temporary one takes its
columns on the coordinator, and the segments make it as the coordinator
sees it.  Materialized views and foreign tables never go to the segments,
and the commands on them — an index, `ALTER`, `GRANT`, `COMMENT`,
`REFRESH` — stay on the coordinator with them.  Not sent yet: triggers,
policies, event triggers, tablespaces and `ALTER DATABASE`.  What is
refused, as it would stay on the coordinator while the commands around
it go to the segments: rules on tables of the cluster.  The segments need no SQL part of `gp`: its tables and functions
are for the configuration of the cluster, and its settings come with the
library.  What is refused: `ALTER TABLE` that computes a column with a
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

The commit of a transaction that the segments take part in keeps one lock
of the coordinator, which a query that reads the segments takes as well:
from the moment the segments prepare to the moment they all hear `COMMIT
PREPARED`, the query either waits or sees its snapshots taken with no
transaction in between, so every query sees a distributed transaction in
full or not at all.  This makes the commits run one at a time, and a
query wait out the one that is running; a faster scheme can come later.

A transaction that wrote one segment alone ends with a plain `COMMIT`
there, with no prepared transaction: a segment lost in that moment rolls
back rather than stay prepared, and the coordinator warns that its rows
may be lost.

A segment lost between the commit of the coordinator and `COMMIT PREPARED`
keeps its transaction prepared, holding its locks, and the coordinator
warns.  A background worker of the coordinator, `gp recovery`, resolves
what is left: every `gp.recovery_interval` seconds it asks every segment
for the transactions it keeps prepared under a name of ours, looks into
the clog of the coordinator for the fate of each, and runs `COMMIT
PREPARED` or `ROLLBACK PREPARED` on the segment as the clog says — a
coordinator lost before it told is rolled back, as the safest of the two.
A pass runs under the lock of the distributed snapshot above, so it never
touches a transaction whose commit is running.  `gp.recovery_pass_now()`
runs a pass at once.

#### Commands outside a transaction

`CREATE DATABASE`, `DROP DATABASE`, `VACUUM`, `REINDEX`, `CREATE INDEX
CONCURRENTLY` and `DROP INDEX CONCURRENTLY` cannot run in a transaction
block, so the coordinator runs them first and then all segments at once,
each in a transaction of its own.  Two-phase commit does not cover them.  The
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
list of tables that mixes temporary ones or those of extension `gp` with
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

By default a table stays on the coordinator, empty or not.  `SET
gp.default_distribution = first_column` (also for a role or a database)
distributes each new permanent table by its first column that has a hash
function, a deterministic collation and is not generated, as
`gp.distribute()` does; a table with no such column stays.  A partition
goes with its table.  `CREATE TABLE AS` and `SELECT INTO` take three steps
to make such a table: the table without rows on the coordinator, as `WITH NO
DATA` makes it; the same table on the segments, with the same OIDs; and the
rows by `INSERT ... SELECT`.  `CREATE TABLE AS EXECUTE`, a temporary table
and a materialized view stay on the coordinator, and a statement with
parameters is refused.

### COPY into distributed tables (`copy.c`)

The table on the coordinator holds no rows, so `COPY ... FROM` into a
distributed table does not run there as usual.  The hook reads and parses
the input with the code of `COPY` itself (files, programs, `STDIN`, every
format), evaluates the defaults of columns the input lacks, finds the
segment of each row by its bucket, and sends the rows to the segments as
`COPY ... FROM STDIN (FORMAT binary)`, which takes every type as it is.  A
row of a replicated table goes to all segments.  The segments run the
`COPY` in the transactions that follow the transaction of the coordinator,
so the two-phase commit covers it; they check the constraints and compute
the generated columns themselves.  The command tag counts the rows of the
input.

The privileges are checked as `COPY` does, on the coordinator for the user,
and again on the segments, where the connection is of the same user.
Triggers and constraints of the table do not run on the coordinator, and
`WHERE`, row-level security and `ON_ERROR` other than `stop` are refused.
A failure of a row on a segment, or in the input, fails the statement
everywhere.  The hook does not call the hooks that were set before it for
such a `COPY`.

### INSERT into distributed tables (`insert.c`, `route.c`)

The planner hook takes the plan of an `INSERT` into a distributed table
that the standard planner has made, and puts a custom scan, `GpInsert`, in
place of the `ModifyTable` on top.  The source of the rows stays as it is:
`VALUES`, a `SELECT`, `DEFAULT VALUES`, the defaults of the columns and the
functions in them run on the coordinator, so `now()` and `nextval()` give
one value for all segments.  `GpInsert` takes each row, finds the segment
of its bucket, and sends it as `COPY` does (`route.c` is what the two share).
The segments check the constraints and compute the generated columns.  The
command tag counts the rows, and the transaction of the coordinator covers
them by two-phase commit.

Triggers and constraints of the table do not run on the coordinator.
`RETURNING`, `ON CONFLICT`, row-level security and `WITH CHECK OPTION` are
refused, rather than the rows lost.  An `INSERT ... SELECT` takes the rows of
its `SELECT`, which reads distributed tables as described below.  As the
source may read the segments on the very connections that the rows go on,
`GpInsert` first reads all its rows into a store, and sends them after.

When the source is `VALUES` or a `SELECT` without a table, and the key
columns of every row are constants (a default, a folded expression and a
`NULL` count), the planner computes their buckets, and if all fall in one
segment, the plan names it: the rows go to that segment alone, and the
others are not asked, nor opened (*direct dispatch*, as in Greengage).  The
buckets stay in the plan, and the executor checks them against the map
before it relies on the segment.  A key that is a parameter of a prepared
statement, a function or a column of a source table takes the way to all
segments.

### UPDATE and DELETE of distributed tables (`insert.c`)

The rows of UPDATE and DELETE are on the segments, and the statement runs
there, over its own rows, in the transaction of the coordinator, under the
two-phase commit; one segment that wrote alone commits in one phase.  The
plan of the coordinator goes away, and a custom scan, `GpDml`, sends the
statement to the segments on the first call, and answers with the sum of
their counts of rows.  The coordinator takes an exclusive lock on the table
for the whole statement, which runs the writers one at a time and spares
the segments a deadlock of their own.  A replicated table takes its UPDATE
on every segment, over every copy.

What is refused: a change of the distribution key, which would move rows
between segments; `RETURNING`; row-level security; parameters, subqueries,
and anything beyond the target table in the statement, for a segment sees
its own rows of the target table and no rows of the others; `MERGE`, and a
data-modifying statement in WITH.  A statement that matches no row changes
nothing, and answers 0.

### SELECT from distributed tables (`gather.c`)

The rows of a distributed table are on the segments, so a query cannot read
the table on the coordinator.  The hook of the paths of a relation makes
the only path to a distributed table a custom scan, `GpGather`, and the
planner plans the rest, the joins, the aggregates, the sorts and the
windows, on the coordinator as for any table.  `GpGather` sends `SELECT
columns FROM table` to all segments at once and reads their rows in the
binary format, which takes every type as it is, into a tuplestore, from which
the rows go up the plan; a rescan, as in a nested loop, reads them from the
store again.  The conditions of the query and the projection run on the
coordinator, so the result is always correct, if not always cheap: all rows
of the table come, none is chosen on the segments, and the planner does not
yet know how many that is.  A replicated table has all its rows on every
segment, so only one of them is asked, by the process number.  A partitioned
table is read as a whole, the partitions of the coordinator being empty.
`EXPLAIN (VERBOSE)` shows the query that the segments run.

A segment that the transaction has written to answers in that transaction,
so it shows what the transaction has written.  The segments answer at their
own moments, as there is no distributed snapshot yet: a transaction that is
committing may show on some segments and not on others.  `FOR UPDATE` and
`TABLESAMPLE` on a distributed table are refused, and so are `UPDATE`,
`DELETE` and `MERGE` of one, and a data-modifying statement of `WITH` on
one, which would otherwise change nothing on the coordinator and say
nothing: they wait for the distributed `DML`.  `TRUNCATE` goes to the
segments with the other DDL.

The policy of a table is read once, by a session, from `gp.distribution_policy`,
and kept; `gp.distribute()` and the drop of a table invalidate it through
the relcache entry of the table, and a change of an extension drops it.

### Aggregates and joins on the segments (`slice.c`)

When a query groups by the whole key of the distribution, every row of a
group is on one segment, so the segment can compute the group to the end:
`SELECT key, count(*) FROM t GROUP BY key` needs no rows on the
coordinator, only the finished groups of every segment.  Such an
aggregate is replaced after the planner by a custom scan, `GpSlice`,
which runs the aggregate of the query on the segments and reads their
rows; the sort and the limit of the query stay on the coordinator.  A
replicated table is aggregated by one segment as a whole, even without a
grouping.  Joins, subqueries, windows, grouping sets, a grouping that
does not cover the whole key, a global aggregate of a hashed table, and
a sorted aggregate that answers the `ORDER BY` of the query by itself
are left to the coordinator.  A junk column of the target list — a
grouping or a sort key that is not selected — is not a stopper: it is
dropped when there is no `ORDER BY`, and sent to the segments as a
column of its own when there is one.

A join runs on the segments too, when any row that joins another is on
the same segment with it: tables of one colocation group joined by the
whole key of the distribution, or a replicated table joined to anything,
or two replicated tables.  The whole plan of such a join is replaced by
`GpSlice`, which runs the join of the query on the segments; the sort and
the limit stay on the coordinator, and a merge join that answers the
`ORDER BY` by itself is left alone.  Inner, left, right and full joins
are all colocated by the same argument: a row without a partner on its
segment has no partner anywhere.  A join by another column, and an
aggregate whose grouping does not cover the key, are left to the
coordinator.  An aggregate over a colocated join runs on the segments as
well, when its grouping covers the key of a hashed table.

A subquery in FROM has a locus of its own: an aggregate of a distributed
table grouped by the key, or a colocated join, runs on the segments even
inside a subquery of a query that is not colocated itself.  The planner
often inlines the plan of a simple subquery with no `SubqueryScan` node,
so the plan below the sort and the limit of the outer query is replaced,
whatever node it is; a plan that answers the `ORDER BY` by its own order
is left alone, and so is a subquery with a limit of its own, a condition
of the outer query on its results, and a subquery joined to a table.

`EXPLAIN (VERBOSE)` shows the query of the segments.

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

To see where the cluster differs from a server on its own, run tests of
PostgreSQL on the running cluster, with tables distributed by their first
column:

```sh
export PG_REGRESS=/path/to/build/src/test/regress/pg_regress
contrib/gp/tools/gp-pg-regress.sh test_setup select join aggregates
```

The diffs are in `$GP_CLUSTER_DIR/regress`.  A test needs the tables of those
it depends on, as in the schedule.  The script drops the tablespace of the
previous run, so a rerun on the same cluster works when the previous run
succeeded; roles of a failed run linger, and call for `gp-cluster.sh destroy`,
`init` and `start` again.  Not every difference is a mistake: tablespaces,
materialized views and the statements listed above as not sent are known.

A test that differs on purpose keeps a copy of its output as
`pgtests/expected/foo.gp.out`, which the script puts in place of the answer
of PostgreSQL; the numbered answers of such a test are dropped, so a cluster
answer cannot hide behind one of them.  The refusals of non-immutable
expressions, `CREATE TABLE AS EXECUTE`, the artifacts of a failed
`CREATE INDEX CONCURRENTLY` and a command on a temporary table together
with others have answers of their own this way.

CI runs the tests of `tools/gp-pg-regress.tests` this way on every push,
weekly and on demand (`gp-pg-regress.yml`); a failing test fails the job,
which keeps the diffs as an artifact.

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
| `gp.recovery_interval` | 5s | Time between the passes of the recovery worker, on the coordinator |
| `gp.dispatch_oids` | | OIDs of a command from the coordinator; set per command by the coordinator, not in the configuration of a server |
| `gp.dispatch_oids_session` | off | Keeps `gp.dispatch_oids` for the next transaction, for a command out of a transaction block |
| `gp.allow_local_ddl` | off | Lets a segment create objects of its own; superuser, for maintenance |
| `gp.dispatch_ddl` | on | Sends DDL of the coordinator to the segments; superuser, for maintenance |
| `gp.default_distribution` | `coordinator` | `first_column` distributes new tables by their first column; settable by a user |

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
| `gp.policy_changed(relation)` | Make every session read the policy of a table again; `gp.distribute()` calls it |
| `gp.segment_id()` | The segment this server is: 0 on the coordinator, and its number on a segment.  A row of a distributed table keeps its segment, but no query tells it yet: that comes with the pushdown of the MPP planner |

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
| `014_extension.pl` | Extensions on the segments with the same OIDs: create, update, cascade, move, drop, a failing segment; extension `gp` stays |
| `015_copy.pl` | `COPY FROM` into distributed and replicated tables: placement by bucket, types, transactions, errors, privileges |
| `016_insert.pl` | `INSERT` into distributed and replicated tables: `VALUES`, `SELECT`, defaults, types, transactions, prepared statements, what is refused, privileges |
| `017_direct_dispatch.pl` | `INSERT` with constant keys goes to one segment alone: which segments are asked, folded constants, `NULL` keys, what is not direct |
| `018_gather.pl` | `SELECT` from distributed tables against the same rows in ordinary tables: aggregates, joins, windows, rescans, replicated and partitioned tables, types, own writes, privileges, what is refused |
| `019_default_distribution.pl` | `gp.default_distribution`: the key of a new table, partitions, temporary tables, `CREATE TABLE AS`, `SELECT INTO`, a database's setting |
| `020_dtx_snapshot.pl` | The lock of the distributed snapshot: a reader waits out a commit stopped between its prepare and `COMMIT PREPARED`; writers and readers together keep every transaction whole |
| `021_one_phase.pl` | A transaction that wrote one segment commits there in one phase; a segment lost in the window rolls back and its rows are lost with a warning |
| `022_unique_key.pl` | Unique indexes of a distributed table must include the whole key of the distribution; equal keys stay on one segment, and its index tells them apart |
| `023_update_delete.pl` | `UPDATE` and `DELETE` run on the segments: distributed and replicated tables, rollbacks, a second writer waiting out the first, what is refused |
| `024_recovery.pl` | The recovery worker of the coordinator: a segment lost during `COMMIT PREPARED` has its transaction committed, a coordinator lost between the prepare and its commit has it rolled back on the segments |
| `025_agg_pushdown.pl` | Aggregates on the segments: grouping by the key of the distribution, replicated tables, what is not pushed, the same answers as on the coordinator |
| `026_join_pushdown.pl` | Joins and aggregates over them on the segments: one colocation group joined by the key, replicated tables, outer joins, joins of three and more tables, what is not pushed, the same answers as on the coordinator |
| `027_subquery_pushdown.pl` | Aggregates and colocated joins inside a subquery of a non-colocated query run on the segments; a limit inside, a condition or a join of the outer query do not |

Besides, CI runs the whole test suite of PostgreSQL twice: on the patched
server without `gp`, and with `gp` preloaded on a single server, to show
that the patch and the extension change nothing while not in use.
`tools/check-core-diff.sh` checks that the tree differs from upstream only
by the patch and `contrib/gp`.

## Roadmap

1. **Cluster skeleton** — done: roles, registration, the copy of segments,
   connections, settings of sessions, distribution policies.
2. **DDL and the same OIDs** — done: the same OIDs, propagating DDL
   with roles, privileges, comments and owners, its two-phase commit,
   commands that run outside a transaction and extensions.
3. **Placing data and correct SELECT** — in progress: `COPY`, `INSERT`,
   direct dispatch of `INSERT` and `SELECT` through a gather are done, and
   the 159 tests of the schedule of PostgreSQL all pass on the cluster,
   twelve of them against answers of our own in `pgtests/expected/`: the
   refusals of non-immutable expressions, `CREATE TABLE AS EXECUTE`, the
   artifacts of a failed `CREATE INDEX CONCURRENTLY`, a command on a
   temporary table together with others; next are `gp_segment_id()`,
   stable functions of time on the coordinator and a comparison with a
   vanilla server.
4. **Distributed transactions and DML** — done: two-phase commit of all
   writes, and one phase for a transaction that wrote one segment;
   distributed snapshots; `UPDATE` and `DELETE` on the segments; unique
   indexes made to include the key of the distribution; and the recovery
   worker that resolves the transactions lost segments keep prepared.
5. **MPP planner without interconnect** — in progress: aggregates whose
   grouping covers the key of the distribution, and joins whose partners
   are on the same segment run on the segments, with aggregates over
   colocated joins of any number of tables, junk columns of the target
   list allowed, and the same computation inside a subquery in FROM;
   next is the locus model for the rest.
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
