#!/bin/sh
# obs_test.sh — observability gate: leveled logfmt logging (stderr capture) +
# RESP server auto-instrumentation metrics.
#
# Part A (always): builds the default binary and asserts a real (log-warn …)
# emits exactly one logfmt line to stderr with the right level/msg/kv fields,
# and that a below-threshold message writes NOTHING (the suppression contract
# the in-suite test.alc asserts can only check via return value).
#
# Part B (metrics build): metrics are OPT-IN (-DALCOVE_METRICS), so this builds
# `alcove-with-metrics`, starts a RESP server whose .init.alc exposes a MET
# command returning (metric NAME), drives a few commands + one bad command, and
# asserts resp.connections / resp.commands / resp.errors are all > 0.
#
# Skips the server half cleanly (still passing) when redis-cli is absent.
set -u
CC=${CC:-cc}
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
PORT=${OBS_TEST_PORT:-7773}
FAIL=0
D=$(mktemp -d)
SRV=
BIN=
cleanup() { [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; [ -n "$BIN" ] && rm -f "$BIN"; rm -rf "$D"; }
trap cleanup EXIT

# strip ANSI so greps are robust against colored output
strip() { sed 's/\x1b\[[0-9;]*m//g'; }

# ---- Part A: logfmt logging to stderr -------------------------------------
echo "== building default binary (metrics OFF) =="
if ! $CC -O2 -o "$D/alcove" alcove.c -lm 2>"$D/build.err"; then
  echo "  build failed:"; cat "$D/build.err"; echo "==> OBS TEST FAILED"; exit 1
fi

echo "== log: one logfmt line on a real emit =="
"$D/alcove" -e '(log-warn "served" :n 3 :who "cli")' >/dev/null 2>"$D/log.err"
LOGLINES=$(grep -c . "$D/log.err")
LINE=$(strip < "$D/log.err")
echo "  line: $LINE"
case "$LINE" in
  ts=*\ level=warn\ msg=served\ n=3\ who=cli) : ;;
  *) echo "  FAIL: logfmt line did not match expected shape"; FAIL=1 ;;
esac
[ "$LOGLINES" = 1 ] || { echo "  FAIL: expected exactly 1 log line, got $LOGLINES"; FAIL=1; }

echo "== log: below-threshold writes nothing =="
"$D/alcove" -e '(do (set-log-level :error) (log-info "hidden") (log-debug "hidden2"))' \
  >/dev/null 2>"$D/quiet.err"
if [ -s "$D/quiet.err" ]; then
  echo "  FAIL: suppressed levels still wrote to stderr:"; strip < "$D/quiet.err"; FAIL=1
else
  echo "  OK — suppressed levels are silent"
fi

# ---- Part B: RESP auto-instrumentation metrics ----------------------------
if ! command -v redis-cli >/dev/null 2>&1; then
  echo "  (redis-cli absent — RESP auto-metrics check skipped)"
  [ "$FAIL" = 0 ] && { echo "==> OBS TEST PASSED (logging only)"; exit 0; }
  echo "==> OBS TEST FAILED"; exit 1
fi

echo "== building metrics binary (-DALCOVE_METRICS) =="
BIN=$(mktemp /tmp/alcove_metrics.XXXXXX)
if ! $CC -O2 -DALCOVE_METRICS -pthread -o "$BIN" alcove.c -lm 2>"$D/mbuild.err"; then
  echo "  metrics build failed:"; cat "$D/mbuild.err"; echo "==> OBS TEST FAILED"; exit 1
fi

# Expose (metric NAME) over RESP. redis-defcmd at startup (before serving) is
# allowed even under --threads; we use a single reactor here for simplicity.
# the callback receives its RESP args as a list; coerce the first to a string.
echo '(redis-defcmd "MET" (fn (args) (str (metric (str (first args))))))' > "$D/.init.alc"
( cd "$D" && exec "$BIN" -r "$PORT" --noload ) >/dev/null 2>"$D/srv.err" &
SRV=$!
up=0; i=0
while [ $i -lt 150 ]; do redis-cli -p "$PORT" PING >/dev/null 2>&1 && { up=1; break; }; i=$((i+1)); sleep 0.1; done
[ "$up" = 1 ] || { echo "  server did not come up:"; strip < "$D/srv.err" | tail -20; echo "==> OBS TEST FAILED"; exit 1; }

echo "== driving traffic (good commands + one bad command) =="
redis-cli -p "$PORT" SET a 1   >/dev/null 2>&1
redis-cli -p "$PORT" GET a     >/dev/null 2>&1
redis-cli -p "$PORT" INCR ctr  >/dev/null 2>&1
redis-cli -p "$PORT" NOSUCHCMD >/dev/null 2>&1   # one error reply
sleep 0.2

check_metric() {  # name -> assert value > 0
  v=$(redis-cli -p "$PORT" MET "$1" 2>/dev/null | strip | tr -d '[:space:]')
  if [ -z "$v" ] || [ "$v" = "nil" ] || ! [ "$v" -gt 0 ] 2>/dev/null; then
    echo "  FAIL: metric $1 = '${v:-<empty>}' (expected > 0)"; FAIL=1
  else
    echo "  OK — $1 = $v"
  fi
}
check_metric resp.connections
check_metric resp.commands
check_metric resp.errors

echo "== stopping server =="
kill -INT "$SRV" 2>/dev/null
k=0; while kill -0 "$SRV" 2>/dev/null && [ $k -lt 50 ]; do sleep 0.2; k=$((k+1)); done
kill -9 "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV=

if [ "$FAIL" = 0 ]; then echo "==> OBS TEST PASSED"; exit 0; fi
echo "==> OBS TEST FAILED"; exit 1
