#!/bin/sh
#
# Check that the branch changes the core of PostgreSQL only by the patch
# that adds new_oid_hook.  Any other file that differs from BASE, outside
# contrib/gp, fails the check.
#
# Usage: contrib/gp/tools/check-core-diff.sh [BASE]
#
# BASE is the mirror of upstream, origin/master by default.  The diff is
# taken from its merge base with HEAD, so the commits of upstream that are
# not merged yet do not count.
#
# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

set -eu

base=${1:-origin/master}

allowed='^('
allowed="$allowed"'contrib/gp/'
allowed="$allowed"'|contrib/Makefile$|contrib/meson\.build$'
allowed="$allowed"'|\.github/workflows/gp\.yml$'
allowed="$allowed"'|src/backend/catalog/(catalog|heap|index)\.c$'
allowed="$allowed"'|src/include/catalog/catalog\.h$'
allowed="$allowed"'|src/test/modules/(Makefile|meson\.build)$'
allowed="$allowed"'|src/test/modules/test_new_oid_hook/'
allowed="$allowed"'|src/tools/pgindent/typedefs\.list$'
allowed="$allowed"')'

unexpected=$(git diff --name-only "$base...HEAD" | grep -Ev "$allowed" || true)

if [ -n "$unexpected" ]; then
	echo "These files differ from $base, but are neither in contrib/gp nor in the new_oid_hook patch:" >&2
	echo "$unexpected" >&2
	exit 1
fi
