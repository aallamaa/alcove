#!/usr/bin/env bash
# benchmark/test-reuseport.sh — sanity check that SO_REUSEPORT lets
# two ./alcove -r processes share the same port. Step 2.4 will have
# N reactors each call bind() in parallel; without REUSEPORT the
# second bind would fail with EADDRINUSE.
#
# Test plan:
#   1. Start two alcove instances on the same port.
#   2. Verify both processes are alive and listening.
#   3. Send a PING via redis-cli; expect PONG.
#   4. Tear down both.

set -e
cd "$(dirname "$0")/.."
. ./benchmark/lib.sh

if [ ! -x ./alcove ]; then
  echo "./alcove missing — run \`make\` first"
  exit 1
fi
if ! command -v redis-cli >/dev/null; then
  echo "redis-cli not found (brew install redis / apt install redis-tools)"
  exit 1
fi

PORT="${REUSEPORT_PORT:-17132}"
reap_port "$PORT"

LOG1="$(mktemp -t alcove-rp1.XXXXXX.log)"
LOG2="$(mktemp -t alcove-rp2.XXXXXX.log)"
P1=
P2=
trap 'kill -INT ${P1:-} ${P2:-} 2>/dev/null; wait 2>/dev/null; rm -f "$LOG1" "$LOG2"' EXIT

./alcove -r "$PORT" >"$LOG1" 2>&1 &
P1=$!
./alcove -r "$PORT" >"$LOG2" 2>&1 &
P2=$!

if ! wait_listening "$LOG1" || ! wait_listening "$LOG2"; then
  echo "FAIL: at least one alcove failed to bind"
  echo "--- log 1 ---"; cat "$LOG1"
  echo "--- log 2 ---"; cat "$LOG2"
  exit 1
fi

if ! kill -0 $P1 2>/dev/null || ! kill -0 $P2 2>/dev/null; then
  echo "FAIL: a process died after bind"
  exit 1
fi

# `|| true` keeps `set -e` from aborting before our reply check below
# can produce a useful FAIL message on transient connect failure.
reply=$(redis-cli -p "$PORT" PING 2>/dev/null || true)
if [ "$reply" != "PONG" ]; then
  echo "FAIL: expected PONG, got '$reply'"
  exit 1
fi

echo "ok: two reactors bound port $PORT via SO_REUSEPORT, PING -> PONG"
