#!/bin/sh
# resp_tsan.sh — the multi-reactor RESP server under ThreadSanitizer.
#
# Starts `alcove -r --threads 4` (a TSan-instrumented build), drives concurrent
# clients against a SHARED key (max contention on the lock-free keyspace), plus
# distinct-key writers and a (redis-defcmd) callback, then asserts the reactor
# pool is data-race-clean. This is the end-to-end concurrency gate; mpsc-test-tsan
# only covers the MPSC queue primitive in isolation.
#
# Skips cleanly (exit 0) when redis-cli or a TSan-capable toolchain is absent.
# Every client is timeout-guarded so a TSan-slowed run can never hang the gate.
set -u
CC=${CC:-cc}
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
PORT=${RESP_TSAN_PORT:-7771}

command -v redis-cli >/dev/null 2>&1 || {
  echo "  (redis-cli absent — RESP TSan check skipped)"; echo "==> RESP TSAN SKIPPED"; exit 0; }

BIN=$(mktemp /tmp/alcove_tsan.XXXXXX)
D=$(mktemp -d)
SRV=
cleanup() { [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -f "$BIN"; rm -rf "$D"; }
trap cleanup EXIT

echo "== building TSan reactor binary =="
if ! $CC -fsanitize=thread -O1 -g -fno-strict-aliasing -pthread -o "$BIN" alcove.c -lm 2>"$D/build.err"; then
  if grep -qiE "thread.*saniti|tsan|unrecognized|libtsan" "$D/build.err"; then
    echo "  (ThreadSanitizer unavailable on this toolchain — skipped)"; echo "==> RESP TSAN SKIPPED"; exit 0
  fi
  echo "  build failed:"; cat "$D/build.err"; echo "==> RESP TSAN FAILED"; exit 1
fi

echo '(redis-defcmd "BUMP" (fn (a) (str "ok=" (+ 1 2))))' > "$D/.init.alc"
( cd "$D" && exec "$BIN" -r "$PORT" --threads 4 --noload ) >/dev/null 2>"$D/srv.err" &
SRV=$!
up=0; i=0
while [ $i -lt 150 ]; do redis-cli -p "$PORT" PING >/dev/null 2>&1 && { up=1; break; }; i=$((i+1)); sleep 0.1; done
[ "$up" = 1 ] || { echo "  server did not come up:"; sed 's/\x1b\[[0-9;]*m//g' "$D/srv.err" | tail -20; echo "==> RESP TSAN FAILED"; exit 1; }

echo "== driving concurrent clients (4 reactors, shared-key contention) =="
# Light but contended: 4 clients on the SAME key + distinct-key writers + a few
# callback hits. A few dozen contended ops surface a startup/keyspace race; more
# only makes the TSan-slowed run drag. timeout-guarded so nothing can hang.
# Collect ONLY the client PIDs — a bare `wait` would also wait on $SRV, which
# serves forever (SIGINT comes after), deadlocking the gate.
cpids=
for j in 1 2 3 4; do timeout 60 redis-cli -p "$PORT" -r 40 INCR shared >/dev/null 2>&1 & cpids="$cpids $!"; done
for j in 1 2 3;  do timeout 60 redis-cli -p "$PORT" -r 25 SET "k$j" "v$j" >/dev/null 2>&1 & cpids="$cpids $!"; done
timeout 60 redis-cli -p "$PORT" -r 5 BUMP >/dev/null 2>&1 & cpids="$cpids $!"
# shellcheck disable=SC2086
wait $cpids
sleep 0.3

echo "== stopping server =="
kill -INT "$SRV" 2>/dev/null
k=0; while kill -0 "$SRV" 2>/dev/null && [ $k -lt 50 ]; do sleep 0.2; k=$((k+1)); done
kill -9 "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV=

n=$(grep -c "WARNING: ThreadSanitizer" "$D/srv.err" 2>/dev/null)
if [ "${n:-0}" -gt 0 ]; then
  echo "  $n ThreadSanitizer report(s):"
  grep -A12 "WARNING: ThreadSanitizer" "$D/srv.err" | sed 's/\x1b\[[0-9;]*m//g' | head -48
  echo "==> RESP TSAN FAILED"; exit 1
fi
echo "  OK — 4-reactor server data-race-clean under concurrent load"
echo "==> RESP TSAN PASSED"
