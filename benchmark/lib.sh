# Shared benchmark helpers. Sourced by run.sh, compare.sh, run-resp.sh,
# and test-reuseport.sh. Expects PYTHON to be set (or defaults to python3).

: "${PYTHON:=python3}"

# Kill any process holding $1 (port). One pipeline, no TOCTOU race.
reap_port() {
  lsof -ti :"$1" 2>/dev/null | xargs kill -9 2>/dev/null || true
}

# Poll $1 (log file) for N "listening on" lines. The optional second
# arg is the expected listener count, and the optional third arg is the
# retry count. Multi-reactor RESP benchmarks need this because
# SO_REUSEPORT only balances connections across listeners that already
# exist when clients connect.
wait_listening() {
  local log="$1" expected="${2:-1}" tries="${3:-50}"
  for _ in $(seq 1 "$tries"); do
    [ "$(grep -c "listening on" "$log" 2>/dev/null || true)" -ge "$expected" ] && return 0
    sleep 0.05
  done
  return 1
}

# wall-clock in microseconds since epoch.
#
# IMPORTANT: the per-iteration timers (best_of/median_of) do NOT call this —
# they read bash's $EPOCHREALTIME directly into a variable, with no subshell
# and no `date` fork. That matters: a `$(date)` / `$(now_us)` command
# substitution forks a subshell (~0.6–2 ms here), which is LARGER than the
# JIT'd micro-benchmarks themselves and was swamping their net work as noise.
# now_us is kept only as a fallback for shells without $EPOCHREALTIME (bash<5)
# and for other sourcing scripts.
now_us() {
  if [ -n "${EPOCHREALTIME:-}" ]; then
    echo "${EPOCHREALTIME/./}"   # "S.UUUUUU" -> integer microseconds
  else
    local ns; ns=$(date +%s%N); echo $(( ns / 1000 ))
  fi
}

# Read the current time in microseconds into the named variable, in-process
# (no subshell/fork) when $EPOCHREALTIME is available. Usage: read_us s
read_us() {
  if [ -n "${EPOCHREALTIME:-}" ]; then
    printf -v "$1" '%s' "${EPOCHREALTIME/./}"
  else
    local ns; ns=$(date +%s%N); printf -v "$1" '%s' "$(( ns / 1000 ))"
  fi
}

format_ms() {
  awk -v us="$1" 'BEGIN { printf "%8.3f ms", us / 1000.0 }'
}

# best-of-N wall-clock in us: best_of N <cmd...>
best_of() {
  local n="$1"; shift
  local best=999999999 s e t rc
  for _ in $(seq 1 "$n"); do
    read_us s; "$@" >/dev/null 2>&1; rc=$?; read_us e
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
    read_us s; "$@" >/dev/null 2>&1; rc=$?; read_us e
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
