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
else
  echo "RESP WATCH FAILED — driver log:"
  sed 's/^/  /' "$LOG" | tail -40
  exit 1
fi

# --- phase 2: (redis-wait-event!) in a live -R server ---------------------
# Combined REPL+RESP (-R) is single-reactor, so a wait can't be woken by a
# network client mid-block (the C wake shim's cross-thread signal→poll cycle
# is unit-tested in mpsc_test.c / mpsc-test-tsan). What CAN be verified live:
# a network SET's emit is consumable via wait-event (fast path through the
# real RESP handler), and an empty wait genuinely blocks for its timeout.
# The REPL needs a real terminal, hence tmux.
if ! command -v tmux >/dev/null 2>&1; then
  echo "  (tmux absent — live wait-event check skipped)"
  echo "==> RESP WATCH PASSED (phase 2 skipped)"
  exit 0
fi
PORT2=$((WATCH_PORT + 1))
SES="alcwatch$$"
tmux kill-session -t "$SES" 2>/dev/null || true
tmux new-session -d -s "$SES" -x 200 -y 50 \
  "./alcove --noload --no-init -R $PORT2"
trap 'rm -f "$LOG"; tmux kill-session -t "$SES" 2>/dev/null || true' EXIT INT TERM

i=0
until redis-cli -p "$PORT2" PING >/dev/null 2>&1; do
  i=$((i + 1))
  [ "$i" -gt 100 ] && { echo "RESP WATCH FAILED: -R server never came up"; exit 1; }
  sleep 0.1
done

tmux send-keys -t "$SES" '(redis-watch! t)' Enter
# Wait until the REPL has actually EVALUATED the enable before the SET —
# a fixed sleep races on slow runners. The marker is split in the input so
# the pty's input echo can't satisfy the grep; only prn output can.
tmux send-keys -t "$SES" '(prn (str "ARMED" "-OK"))' Enter
i=0
until tmux capture-pane -t "$SES" -p | grep -q 'ARMED-OK'; do
  i=$((i + 1))
  [ "$i" -gt 100 ] && { echo "RESP WATCH FAILED: REPL never evaluated watch-enable"; exit 1; }
  sleep 0.1
done
redis-cli -p "$PORT2" SET wakekey wakeval >/dev/null 2>&1
sleep 0.3
tmux send-keys -t "$SES" '(prn "GOT" (nth (redis-wait-event! 5000) 3))' Enter
sleep 1
tmux send-keys -t "$SES" '(prn "SLEPT" (>= (- (do (= t0 (now-ms)) (redis-wait-event! 500) (now-ms)) t0) 450))' Enter
sleep 2

PANE=$(tmux capture-pane -t "$SES" -p -S -200 | sed 's/\x1b\[[0-9;]*m//g')
if echo "$PANE" | grep -q 'GOT.*wakekey' && echo "$PANE" | grep -q 'SLEPT *t'; then
  echo "  live wait-event: OK (network SET consumed; empty wait blocked ~500ms)"
  echo "==> RESP WATCH PASSED"
else
  echo "RESP WATCH FAILED — live wait-event pane:"
  echo "$PANE" | grep -v '^$' | sed 's/^/  /' | tail -20
  exit 1
fi
