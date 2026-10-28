#!/bin/sh
#
# Demo cluster on one host: a coordinator and segments, each a separate
# PostgreSQL server with gp preloaded, on neighbouring ports.
#
# Usage: contrib/gp/tools/gp-cluster.sh COMMAND
#
# Commands:
#   init     create the nodes; the cluster directory must not exist
#   start    start the segments, then the coordinator; create extension gp
#            in its database postgres and wait for the segments to register
#   stop     stop the coordinator, then the segments
#   status   show the nodes, their ports and roles
#   destroy  stop the nodes and remove the cluster directory
#
# Environment:
#   GP_CLUSTER_DIR  directory of the cluster, $HOME/gp-cluster by default
#   GP_PORT         port of the coordinator, 9000 by default; segment N,
#                   numbered from 1, listens on GP_PORT + N (init only)
#   GP_SEGMENTS     number of segments, 3 by default (init only)
#   PGBIN           directory of initdb, pg_ctl and psql of a server built
#                   with contrib/gp; taken from pg_config in PATH by default
#
# The other commands take the nodes from the file "nodes", which init
# writes to the cluster directory.
#
# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

set -eu

dir=${GP_CLUSTER_DIR:-$HOME/gp-cluster}
nodes=$dir/nodes

die()
{
	echo "$0: $*" >&2
	exit 1
}

if [ -z "${PGBIN:-}" ]; then
	command -v pg_config >/dev/null || die "pg_config not found, set PGBIN"
	PGBIN=$(pg_config --bindir)
fi
[ -x "$PGBIN/initdb" ] || die "$PGBIN/initdb not found, set PGBIN"
[ -f "$("$PGBIN/pg_config" --sharedir)/extension/gp.control" ] ||
	die "extension gp is not installed in $PGBIN/.., set PGBIN"

# Print "name port role" of each node, segments first
list()
{
	cat "$nodes"
}

# Run psql in the database postgres of the coordinator
psql_coordinator()
{
	"$PGBIN/psql" -X -A -t -q -v ON_ERROR_STOP=1 -h localhost \
		-p "$(sed -n 's/^coordinator \([0-9]*\) .*/\1/p' "$nodes")" \
		-d postgres "$@"
}

running()
{
	"$PGBIN/pg_ctl" status -D "$dir/$1" >/dev/null 2>&1
}

cmd_init()
{
	[ ! -e "$dir" ] || die "$dir already exists"
	port=${GP_PORT:-9000}
	segments=${GP_SEGMENTS:-3}

	mkdir -p "$dir"
	i=1
	while [ "$i" -le "$segments" ]; do
		echo "seg$i $((port + i)) segment" >>"$nodes.new"
		i=$((i + 1))
	done
	echo "coordinator $port coordinator" >>"$nodes.new"

	while read -r name nport role; do
		echo "initdb $name"
		# A segment has a number, registers on the coordinator and prepares
		# transactions of DDL
		case $role in
			segment) set -- -c "gp.segment_id=${name#seg}" \
				-c "gp.coordinator=host=localhost port=$port" \
				-c gp.register_retry_interval=1 \
				-c max_prepared_transactions=64 ;;
			*) set -- ;;
		esac
		"$PGBIN/initdb" --pgdata "$dir/$name" --auth trust --no-instructions \
			-c shared_preload_libraries=gp \
			-c "gp.role=$role" \
			"$@" \
			-c "port=$nport" \
			-c "cluster_name=gp-$name" \
			>"$dir/$name.initdb.log" 2>&1 ||
			die "initdb $name failed, see $dir/$name.initdb.log"
	done <"$nodes.new"
	mv "$nodes.new" "$nodes"
}

cmd_start()
{
	list | while read -r name port role; do
		if running "$name"; then
			echo "$name is already running"
		else
			"$PGBIN/pg_ctl" start --silent --wait -D "$dir/$name" \
				-l "$dir/$name.log" || die "start $name failed, see $dir/$name.log"
			echo "$name started on port $port"
		fi
	done

	psql_coordinator -c 'SET client_min_messages = warning' \
		-c 'CREATE EXTENSION IF NOT EXISTS gp' ||
		die "could not create extension gp on the coordinator"

	# Segments retry every few seconds, so the first ones may have failed
	segments=$(grep -c ' segment$' "$nodes")
	i=0
	while [ "$(psql_coordinator -c 'SELECT count(*) FROM gp.segments')" -lt "$segments" ]; do
		i=$((i + 1))
		[ "$i" -le 60 ] ||
			die "segments did not register in 60 seconds, see the logs in $dir"
		sleep 1
	done
	echo "$segments segments registered on the coordinator"
}

cmd_stop()
{
	# Reverse order: the coordinator first
	list | sed '1!G;h;$!d' | while read -r name port role; do
		if running "$name"; then
			"$PGBIN/pg_ctl" stop --silent --wait -D "$dir/$name" -m fast
			echo "$name stopped"
		fi
	done
}

cmd_status()
{
	list | while read -r name port role; do
		if running "$name"; then
			actual=$("$PGBIN/psql" -X -A -t -h localhost -p "$port" -d postgres \
				-c 'SHOW gp.role' 2>/dev/null) || actual='?'
			echo "$name	port $port	running	gp.role = $actual"
		else
			echo "$name	port $port	stopped"
		fi
	done
}

cmd_destroy()
{
	cmd_stop
	rm -rf "$dir"
	echo "removed $dir"
}

case ${1:-} in
	init)
		cmd_init
		;;
	start | stop | status | destroy)
		# Only a directory made by init is touched, destroy included
		[ -f "$nodes" ] || die "no cluster in $dir, run init first"
		"cmd_$1"
		;;
	*)
		echo "Usage: $0 init|start|stop|status|destroy" >&2
		exit 2
		;;
esac
