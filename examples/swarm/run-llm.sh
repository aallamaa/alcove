#!/bin/sh
# examples/swarm/run-llm.sh — the GENERATIVE swarm (M0+M2+M3 combined).
#
# Starts the blackboard, sets a generation budget, and runs N worker processes
# that each ASK the model for candidates (lib/llm), score them (lib/evolve), and
# publish to the board (lib/swarm). The server-side BEST command reports the
# winner. Real candidates need a key:
#
#   export ANTHROPIC_API_KEY=sk-ant-...
#   sh examples/swarm/run-llm.sh
#
# With no key the workers still run end to end (placeholder candidates) — proving
# the plumbing — but the winning candidate will only be good with a real model.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd); cd "$ROOT"
PORT=${SWARM_PORT:-$((7000 + $$ % 2000))}
BUDGET=${SWARM_BUDGET:-8}   # total generations split across the workers
WORKERS=${SWARM_WORKERS:-3}
[ -x ./alcove ] && [ -x ./adder ] || { echo "build first: make jit adder"; exit 1; }
D=$(mktemp -d); SRV=
cleanup() { [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$D"; }
trap cleanup EXIT

cp examples/swarm/server-init.alc "$D/.init.alc"
( cd "$D" && exec "$ROOT/alcove" -r "$PORT" --noload >/dev/null 2>"$D/srv.err" ) & SRV=$!

echo "== setting generation budget = $BUDGET on :$PORT =="
i=0
while [ $i -lt 100 ]; do
  printf 'require("lib/swarm")\nfd = (swarm/connect "127.0.0.1" %s)\nswarm/set(fd "count" "%s")\nswarm/close(fd)\n' "$PORT" "$BUDGET" > "$D/seed.adr"
  ./adder "$D/seed.adr" >/dev/null 2>&1 && break
  i=$((i + 1)); sleep 0.1
done

echo "== launching $WORKERS generative workers =="
pids=
j=0
while [ $j -lt "$WORKERS" ]; do
  ./adder examples/swarm/worker-llm.adr "$PORT" >/dev/null 2>>"$D/w.err" & pids="$pids $!"
  j=$((j + 1))
done
# shellcheck disable=SC2086
wait $pids

echo "== leaderboard =="
./adder examples/swarm/check.adr "$PORT" 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g'
