#!/bin/sh
# scripts/swarm-smoke.sh — end-to-end swarm convergence check (manual, not gated).
#
# Starts an alcove RESP keyspace as a shared blackboard, seeds candidate programs,
# runs two worker PROCESSES that claim + score them via atomic INCR tickets, then
# asserts the swarm converged on the optimal candidate with every candidate
# evaluated exactly once. Pure alcove — no redis-cli needed.
#
#   make swarm-smoke           # or:  sh scripts/swarm-smoke.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
# Unique-per-invocation by default so back-to-back runs never collide on a port
# still held by a prior (shutting-down) server.
PORT=${SWARM_PORT:-$((7000 + $$ % 2000))}
[ -x ./alcove ] && [ -x ./adder ] || { echo "build first: make jit adder"; echo "==> SWARM SMOKE FAILED"; exit 1; }
D=$(mktemp -d); SRV=
cleanup() { [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$D"; }
trap cleanup EXIT

echo "== starting blackboard server on :$PORT =="
# Run the server from $D with server-init.alc as its .init.alc, so it registers
# the server-side BEST aggregation command at startup. exec so $! is the server.
cp examples/swarm/server-init.alc "$D/.init.alc"
ALC="$ROOT/alcove"
( cd "$D" && exec "$ALC" -r "$PORT" --noload >/dev/null 2>"$D/srv.err" ) & SRV=$!

echo "== seeding candidates (retry until the server is up) =="
up=0; i=0
while [ $i -lt 100 ]; do
  ./adder examples/swarm/seed.adr "$PORT" >/dev/null 2>"$D/seed.err" && { up=1; break; }
  i=$((i + 1)); sleep 0.1
done
[ "$up" = 1 ] || { echo "  server did not come up:"; sed 's/\x1b\[[0-9;]*m//g' "$D/srv.err" "$D/seed.err" | tail -20; echo "==> SWARM SMOKE FAILED"; exit 1; }

echo "== running 2 workers (parallel claim via atomic INCR) =="
./adder examples/swarm/worker.adr "$PORT" >/dev/null 2>"$D/w1.err" & W1=$!
./adder examples/swarm/worker.adr "$PORT" >/dev/null 2>"$D/w2.err" & W2=$!
wait "$W1" "$W2"

echo "== leaderboard =="
OUT=$(./adder examples/swarm/check.adr "$PORT" 2>"$D/chk.err")
echo "  $OUT"
CLEAN=$(echo "$OUT" | sed 's/\x1b\[[0-9;]*m//g')   # prn colorizes numbers; strip ANSI before matching

if echo "$CLEAN" | grep -qF 'best=(fn (x) (* x x)) score=0 filled=6/6' \
   && echo "$CLEAN" | grep -qF 'SWARM SERVER BEST: 2 0'; then
  echo "==> SWARM SMOKE PASSED"; exit 0
fi
echo "  worker / check stderr:"; sed 's/\x1b\[[0-9;]*m//g' "$D/w1.err" "$D/w2.err" "$D/chk.err" 2>/dev/null | tail -20
echo "==> SWARM SMOKE FAILED"; exit 1
