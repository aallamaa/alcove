#!/bin/sh
# Layer-2 keyspace watch gate (make resp-watch-test).
#
# Drives tools/test_resp_watch.alc: the script enables the watch, serves
# --threads 4, a redis-cli client SETs then DELs a key, SIGINTs the server
# (by explicit pid), and the main thread drains and verifies the event
# stream (set wk, del wk, oldest first).

set -eu

if ! command -v redis-cli >/dev/null 2>&1; then
  echo "  (redis-cli absent — RESP watch check skipped)"
  echo "==> RESP WATCH SKIPPED"
  exit 0
fi

# Unique port per run — back-to-back runs on a fixed port collide while a
# prior server is still shutting down.
WATCH_PORT=${WATCH_PORT:-$((17000 + $$ % 2000))}
export WATCH_PORT

LOG=$(mktemp -t alcove-resp-watch.XXXXXX.log)
trap 'rm -f "$LOG"' EXIT INT TERM

# timeout: if the server never receives its SIGINT (deadlock, blocked
# redis-cli), fail the gate instead of hanging the CI job.
if timeout 60 ./alcove --noload --no-init tools/test_resp_watch.alc >"$LOG" 2>&1 \
   && grep -q 'RESP WATCH DRIVER OK' "$LOG"; then
  grep 'RESP WATCH DRIVER OK' "$LOG"
  echo "==> RESP WATCH PASSED"
else
  echo "RESP WATCH FAILED — driver log:"
  sed 's/^/  /' "$LOG" | tail -40
  exit 1
fi
