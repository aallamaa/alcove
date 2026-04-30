#!/usr/bin/env bash
# benchmark/run-resp.sh — parallel-clients RESP throughput benchmark.
#
# Spins up ./alcove -r on a free port, sweeps redis-benchmark across
# concurrency levels, prints a tidy summary, then tears down the
# server. Use this to gauge how well the reactor handles many
# concurrent clients (Step 2.3 ships acceptor-on-its-own-thread; Step
# 2.4 will add multiple reactors).
#
# Requires: redis-benchmark (brew install redis / apt install redis-tools).
# Build first: `make jit` (or `make jit-mono`); this script picks up
# whatever ./alcove was last built with.

set -e
cd "$(dirname "$0")/.."
. ./benchmark/lib.sh

if ! command -v redis-benchmark >/dev/null; then
  echo "redis-benchmark not found (brew install redis / apt install redis-tools)"
  exit 1
fi
if [ ! -x ./alcove ]; then
  echo "./alcove missing — run \`make jit\` first"
  exit 1
fi

PORT="${RESP_PORT:-17131}"
N="${RESP_N:-100000}"
PIPELINE="${RESP_PIPELINE:-1}"
CLIENTS_LIST="${RESP_CLIENTS:-1 10 50 100 200 500}"

reap_port "$PORT"

LOG="$(mktemp -t alcove-resp.XXXXXX.log)"
./alcove -r "$PORT" >"$LOG" 2>&1 &
SRV=$!
trap 'kill -INT $SRV 2>/dev/null; wait $SRV 2>/dev/null; rm -f "$LOG"' EXIT

if ! wait_listening "$LOG"; then
  echo "alcove failed to start; log:"; cat "$LOG"; exit 1
fi

build="$(./alcove -e '(print "")' 2>/dev/null | head -1)"
echo "alcove RESP benchmark — port=$PORT  N=$N  pipeline=$PIPELINE"
echo

printf "%-10s %12s %12s %10s %10s\n" "clients" "SET rps" "GET rps" "SET p50ms" "GET p50ms"
printf '%s\n' "----------------------------------------------------------"

for c in $CLIENTS_LIST; do
  # redis-benchmark -q uses \r-overwrite for live progress; the final
  # summary "SET: NNN.NN requests per second, p50=X.XX msec" sits at
  # the tail of that same line. tr -d '\r' splits into discrete lines,
  # then we grep for the summary line specifically.
  out=$(redis-benchmark -p "$PORT" -t SET,GET -n "$N" -c "$c" -P "$PIPELINE" -q 2>/dev/null \
        | tr '\r' '\n')
  set_line=$(printf '%s\n' "$out" | grep "SET: .* requests per second" | tail -1)
  get_line=$(printf '%s\n' "$out" | grep "GET: .* requests per second" | tail -1)
  set_rps=$(printf '%s\n' "$set_line" | awk '{print $2}')
  get_rps=$(printf '%s\n' "$get_line" | awk '{print $2}')
  set_p50=$(printf '%s\n' "$set_line" | sed -n 's/.*p50=\([^ ]*\) msec.*/\1/p')
  get_p50=$(printf '%s\n' "$get_line" | sed -n 's/.*p50=\([^ ]*\) msec.*/\1/p')
  printf "%-10s %12s %12s %10s %10s\n" "$c" "${set_rps:-?}" "${get_rps:-?}" "${set_p50:-?}" "${get_p50:-?}"
done
