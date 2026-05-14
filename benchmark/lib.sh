# Shared benchmark helpers. Sourced by run.sh, compare.sh, run-resp.sh,
# and test-reuseport.sh. Expects PYTHON to be set (or defaults to python3).

: "${PYTHON:=python3}"

# Kill any process holding $1 (port). One pipeline, no TOCTOU race.
reap_port() {
  lsof -ti :"$1" 2>/dev/null | xargs kill -9 2>/dev/null || true
}

# Poll $2 (log file) for "listening on" up to ~2.5s. Returns 0 if seen.
wait_listening() {
  local log="$1" tries="${2:-50}"
  for _ in $(seq 1 "$tries"); do
    grep -q "listening on" "$log" && return 0
    sleep 0.05
  done
  return 1
}

# wall-clock in microseconds since epoch. Use date rather than spawning
# Python for the timer itself; the old millisecond timer quantized very
# fast benchmarks into a 1 ms floor and distorted the startup-adjusted
# speedup table.
now_us() {
  local ns
  ns=$(date +%s%N)
  echo $(( ns / 1000 ))
}

format_ms() {
  awk -v us="$1" 'BEGIN { printf "%8.3f ms", us / 1000.0 }'
}

# best-of-N wall-clock in us: best_of N <cmd...>
best_of() {
  local n="$1"; shift
  local best=999999999 s e t rc
  for _ in $(seq 1 "$n"); do
    s=$(now_us); "$@" >/dev/null 2>&1; rc=$?; e=$(now_us)
    if [ "$rc" -ne 0 ]; then
      echo "benchmark command failed ($rc): $*" >&2
      return "$rc"
    fi
    t=$(( e - s ))
    [ "$t" -lt "$best" ] && best=$t
  done
  echo "$best"
}

# median-of-N wall-clock in us: median_of N <cmd...>
median_of() {
  local n="$1"; shift
  local s e rc vals
  vals=$(mktemp)
  for _ in $(seq 1 "$n"); do
    s=$(now_us); "$@" >/dev/null 2>&1; rc=$?; e=$(now_us)
    if [ "$rc" -ne 0 ]; then
      rm -f "$vals"
      echo "benchmark command failed ($rc): $*" >&2
      return "$rc"
    fi
    echo $(( e - s )) >>"$vals"
  done
  sort -n "$vals" | awk -v n="$n" 'NR==int((n+1)/2)'
  rm -f "$vals"
}
