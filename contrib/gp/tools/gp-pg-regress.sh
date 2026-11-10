#!/bin/sh
# contrib/gp/tools/gp-pg-regress.sh
#
# Run tests of PostgreSQL on the cluster of gp-cluster.sh, to see where it
# differs from a server on its own.  Tables that the tests create are
# distributed by their first column (gp.default_distribution), and each test
# runs in a database "regression", which pg_regress makes anew.  The answers
# are those of PostgreSQL, so a test that differs has a diff in the
# output directory to be read; not every difference is a mistake.
#
# usage: gp-pg-regress.sh test...
#   gp-pg-regress.sh test_setup select join
#
# Environment:
#   PG_REGRESS      pg_regress of the build, which has regress.so next to it
#                   unless DLPATH is set
#   PG_SRC          src/test/regress of the source tree, by git by default
#   DLPATH          directory of regress.so, that of PG_REGRESS by default
#   OUTDIR          where the results and the diffs go,
#                   $GP_CLUSTER_DIR/regress by default
#   GP_PORT, GP_CLUSTER_DIR, PGBIN
#                   as for gp-cluster.sh; the cluster must be running
#
# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

set -eu

die()
{
	echo "gp-pg-regress.sh: $*" >&2
	exit 1
}

[ $# -gt 0 ] || die "no tests given"
[ -n "${PG_REGRESS:-}" ] || die "set PG_REGRESS to the pg_regress of the build"
[ -x "$PG_REGRESS" ] || die "$PG_REGRESS is not there"

if [ -z "${PGBIN:-}" ]; then
	command -v pg_config >/dev/null || die "pg_config not found, set PGBIN"
	PGBIN=$(pg_config --bindir)
fi

if [ -z "${PG_SRC:-}" ]; then
	top=$(git -C "$(dirname "$0")" rev-parse --show-toplevel) ||
		die "not in a git tree, set PG_SRC"
	PG_SRC=$top/src/test/regress
fi
[ -d "$PG_SRC/sql" ] || die "$PG_SRC has no sql directory"

DLPATH=${DLPATH:-$(dirname "$PG_REGRESS")}
OUTDIR=${OUTDIR:-${GP_CLUSTER_DIR:-$HOME/gp-cluster}/regress}
mkdir -p "$OUTDIR"

# The coordinator must be there: a database of tests is made on the cluster
"$PGBIN/psql" -X -A -t -q -h localhost -p "${GP_PORT:-9000}" -d postgres \
	-c "SELECT count(*) FROM gp.cluster_segments()" >/dev/null ||
	die "the cluster is not running, start it with gp-cluster.sh start"

# The answers of our own must be those of the PostgreSQL of this tree: a
# merge that changed a base answer leaves them stale, which shows here
# before the tests turn red with diffs no one asked for
"$(dirname "$0")"/gp-gp-manifest.sh check ||
	die "the answers of our own are stale; refresh them and run tools/gp-gp-manifest.sh update"

# test_setup makes a tablespace, which outlives the database that pg_regress
# drops and remakes; without this a second run on the same cluster fails there
"$PGBIN/psql" -X -A -t -q -v ON_ERROR_STOP=1 -h localhost \
	-p "${GP_PORT:-9000}" -d postgres \
	-c "DROP TABLESPACE IF EXISTS regress_tblspace" ||
	die "could not drop the tablespace of the previous run"

# Every session of the tests takes new tables to the segments
PGOPTIONS="${PGOPTIONS:-} -c gp.default_distribution=first_column"
export PGOPTIONS

# Answers of our own: a test that differs on purpose keeps a copy of its
# output as pgtests/expected/foo.gp.out, which takes the place of the answer
# of PostgreSQL.  The numbered answers of such a test go away, so a cluster
# answer cannot hide behind one of them, and neither can an answer of
# PostgreSQL show up as the cluster one by mistake.
ANSWERS=$(dirname "$0")/../pgtests/expected
EXPECTED="$OUTDIR/expected"
found=false
for answer in "$ANSWERS"/*.gp.out; do
	[ -e "$answer" ] || continue
	if ! $found; then
		found=true
		rm -rf "$EXPECTED"
		mkdir -p "$EXPECTED/expected"
		cp "$PG_SRC"/expected/*.out "$EXPECTED/expected/"
	fi
	test=$(basename "$answer" .gp.out)
	cp "$answer" "$EXPECTED/expected/$test.out"
	rm -f "$EXPECTED/expected/${test}"_[0-9]*.out
done
if $found; then
	set -- --expecteddir="$EXPECTED" "$@"
fi

exec "$PG_REGRESS" --host=localhost --port="${GP_PORT:-9000}" \
	--bindir="$PGBIN" --inputdir="$PG_SRC" --outputdir="$OUTDIR" \
	--dlpath="$DLPATH" --dbname=regression "$@"
