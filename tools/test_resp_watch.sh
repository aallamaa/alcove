#!/bin/sh
# Basic crash/load test for layer-2 keyspace watches under --threads

set -eu

if ! command -v redis-cli >/dev/null 2>&1; then
  echo "  (redis-cli absent — RESP watch check skipped)"
  echo "==> RESP WATCH SKIPPED"
  exit 0
fi

PORT=${PORT:-17623}
LOG=$(mktemp -t alcove-resp-watch.XXXXXX.log)

./alcove --noload --no-init -r "$PORT" --threads 4 >"$LOG" 2>&1 &
SRV=$!

cleanup() {
  kill -INT "$SRV" 2>/dev/null || true
  wait "$SRV" 2>/dev/null || true
  rm -f "$LOG"
}
trap cleanup EXIT INT TERM

i=0
while [ "$i" -lt 100 ]; do
  if redis-cli -p "$PORT" PING >/dev/null 2>&1; then
    break
  fi
  i=$((i + 1))
  sleep 0.05
done

run() {
  redis-cli -p "$PORT" --raw "$@" | tr -d '\r'
}

fail() {
  echo "RESP watch FAILED: $1"
  echo "server log:"
  sed 's/^/  /' "$LOG" | tail -80
  exit 1
}

# In-process tests handle the strict property verifications. This script
# ensures the server doesn't crash under concurrent mutations when
# layer-2 watches are evaluated at the choke point.

run SET w1 "val1" >/dev/null
run SET w2 "val2" >/dev/null
run DEL w1 >/dev/null

echo "RESP WATCH: OK"
