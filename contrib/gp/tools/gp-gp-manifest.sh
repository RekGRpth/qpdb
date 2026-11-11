#!/bin/sh
# contrib/gp/tools/gp-gp-manifest.sh
#
# The answers of our own, pgtests/expected/foo.gp.out, are made from the
# expected/foo.out of PostgreSQL.  The manifest pgtests/MANIFEST keeps the
# SHA-256 of that base answer as it was when each answer was written, so a
# merge of upstream that changes a base answer is caught before it turns the
# tests of the cluster red with diffs no one asked for.
#
# usage: gp-gp-manifest.sh check|update [test...]
#
# Commands:
#   check   compare the hashes of the manifest with the base answers; fail
#           with the list of stale answers, and name answers that have grown
#           equal to their base again for removal
#   update  rewrite the manifest from the current base answers; run after
#           refreshing the answers
#
# Environment:
#   PG_SRC  src/test/regress of the source tree, by git by default
#
# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

set -eu

die()
{
	echo "$0: $*" >&2
	exit 1
}

[ $# -ge 1 ] || die "no command given, check or update"
command=$1
[ "$command" = check ] || [ "$command" = update ] ||
	die "unknown command $command, check or update"
shift

root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel) ||
	die "not in a git tree, set PG_SRC"
: "${PG_SRC:=$root/src/test/regress}"
[ -d "$PG_SRC/expected" ] || die "$PG_SRC has no expected directory"

answers=$root/contrib/gp/pgtests/expected
manifest=$root/contrib/gp/pgtests/MANIFEST

# The tests to work on: those given, or every answer of our own
if [ $# -gt 0 ]; then
	tests=$*
else
	tests=$(cd "$answers" && ls -- *.gp.out 2>/dev/null | sed 's/\.gp\.out$//' || true)
	[ -n "$tests" ] || die "no answers of our own in $answers"
fi

# check: one line per answer that needs attention, and failure if any
if [ "$command" = check ]; then
	status=0
	for test in $tests; do
		base=$PG_SRC/expected/$test.out
		answer=$answers/$test.gp.out
		[ -f "$answer" ] || continue
		recorded=$(sed -n "s/^$test \([0-9a-f]\{64\}\)$/\1/p" "$manifest")
		if [ -z "$recorded" ]; then
			echo "no manifest entry for $test; run update"
			status=1
			continue
		fi
		if [ ! -f "$base" ]; then
			echo "$test: base answer $test.out is gone from upstream"
			status=1
			continue
		fi
		actual=$(sha256sum "$base" | cut -d' ' -f1)
		if [ "$actual" != "$recorded" ]; then
			echo "$test: base answer changed, refresh the answer of our own"
			status=1
		fi
		if cmp -s "$base" "$answer"; then
			echo "$test: answer equals its base, remove it"
			status=1
		fi
	done
	exit $status
fi

# update: rewrite the whole manifest, tests given or not
{
	echo "# Copyright (c) 2026, Georgy Shelkovy"
	echo "# Licensed under the PostgreSQL License, see LICENSE"
	echo "#"
	echo "# The base answer each answer of our own was made from, by test name:"
	echo "# the SHA-256 of the expected/foo.out of PostgreSQL at the moment the"
	echo "# answer was written.  When upstream changes that file, check reports"
	echo "# the answer as stale; when an answer grows equal to its base again,"
	echo "# check asks for it to be removed.  Regenerate the hashes with"
	echo "# tools/gp-gp-manifest.sh update after refreshing the answers."
	for test in $tests; do
		base=$PG_SRC/expected/$test.out
		answer=$answers/$test.gp.out
		[ -f "$answer" ] || die "no answer $answer"
		[ -f "$base" ] || die "no base answer $base"
		echo "$test $(sha256sum "$base" | cut -d' ' -f1)"
	done
} >$manifest.new
mv $manifest.new $manifest
