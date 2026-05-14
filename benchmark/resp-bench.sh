#!/bin/bash
# Randomised-key RESP benchmark sweep against Redis and alcove.
#
# Spawns each server on its own port, runs `redis-benchmark -t SET,GET`
# with `-r $KEYSPACE` (uniformly random key suffix in [0, KEYSPACE))
# across a range of client counts, and prints a table. Reaps everything
# on exit.
#
# Usage:
#   ./benchmark/resp-bench.sh                 # default sweep
#   KEYSPACE=1000000 ./benchmark/resp-bench.sh
#   CLIENTS="50 100 200" ./benchmark/resp-bench.sh
#
# Why `-r KEYSPACE`? Without it `redis-benchmark` hammers a single hot
# key, so the L1 cache absorbs every read/write and the numbers measure
# the server's request loop rather than its data-structure overhead.

set -u
cd "$(dirname "$0")"

KEYSPACE="${KEYSPACE:-100000}"
N="${N:-100000}"
PIPELINE="${PIPELINE:-64}"
CLIENTS="${CLIENTS:-50 100 200 500}"

ALCOVE="${ALCOVE:-../alcove}"
ALCOVE_THREADS="${ALCOVE_THREADS:-1 4}"

if ! command -v redis-server >/dev/null || ! command -v redis-benchmark >/dev/null; then
  echo "error: redis-server / redis-benchmark not on PATH" >&2
  exit 1
fi
if [ ! -x "$ALCOVE" ]; then
  echo "error: $ALCOVE not built (run: make jit)" >&2
  exit 1
fi

PIDS=()
LOGS=()
cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null; done
  for l in "${LOGS[@]}"; do rm -f "$l"; done
}
trap cleanup EXIT

wait_port() {
  for _ in $(seq 1 100); do
    redis-cli -p "$1" PING >/dev/null 2>&1 && return 0
    sleep 0.05
  done
  echo "error: $1 didn't come up" >&2
  return 1
}

extract() {
  echo "$1" | sed -nE "s/^$2: ([0-9.]+) requests per second.*p50=([0-9.]+) msec.*/\1 \2/p"
}

sweep() {
  local label="$1" port="$2"
  echo "$label  port=$port  N=$N  pipeline=$PIPELINE  -r $KEYSPACE"
  printf "%-10s %14s %14s %10s %10s\n" "clients" "SET rps" "GET rps" "SET p50ms" "GET p50ms"
  printf -- '-%.0s' {1..64}; echo
  for c in $CLIENTS; do
    out=$(redis-benchmark -p "$port" -t SET,GET -n "$N" -c "$c" \
                          -P "$PIPELINE" -r "$KEYSPACE" -q 2>/dev/null | tr '\r' '\n')
    read s_rps s_p50 < <(extract "$out" SET)
    read g_rps g_p50 < <(extract "$out" GET)
    printf "%-10s %14s %14s %10s %10s\n" "$c" \
           "${s_rps:-?}" "${g_rps:-?}" "${s_p50:-?}" "${g_p50:-?}"
  done
  echo
}

start_redis() {
  local port="$1"
  local log
  log=$(mktemp -t resp-bench-redis.XXXXXX.log)
  LOGS+=("$log")
  redis-server --save '' --appendonly no --bind 127.0.0.1 --port "$port" \
               --daemonize no >"$log" 2>&1 &
  PIDS+=($!)
  wait_port "$port"
}

start_alcove() {
  local port="$1" threads="$2"
  local log
  log=$(mktemp -t resp-bench-alcove.XXXXXX.log)
  LOGS+=("$log")
  "$ALCOVE" -r "$port" --noload --threads "$threads" >"$log" 2>&1 &
  PIDS+=($!)
  wait_port "$port"
}

port=17160
start_redis "$port"
sweep "redis $(redis-server --version 2>&1 | grep -oE 'v=[^ ]+' | head -1)" "$port"
kill "${PIDS[-1]}" 2>/dev/null; wait "${PIDS[-1]}" 2>/dev/null
unset 'PIDS[${#PIDS[@]}-1]'

for t in $ALCOVE_THREADS; do
  port=$((port + 1))
  start_alcove "$port" "$t"
  sweep "alcove --threads $t" "$port"
  kill "${PIDS[-1]}" 2>/dev/null; wait "${PIDS[-1]}" 2>/dev/null
  unset 'PIDS[${#PIDS[@]}-1]'
done
