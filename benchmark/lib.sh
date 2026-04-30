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

# wall-clock in milliseconds since epoch
now_ms() { "$PYTHON" -c 'import time; print(int(time.time()*1000))'; }

# best-of-N wall-clock in ms: best_of N <cmd...>
best_of() {
  local n="$1"; shift
  local best=999999999 s e t
  for _ in $(seq 1 "$n"); do
    s=$(now_ms); "$@" >/dev/null 2>&1; e=$(now_ms)
    t=$(( e - s ))
    [ "$t" -lt "$best" ] && best=$t
  done
  echo "$best"
}

# median-of-N wall-clock in ms: median_of N <cmd...>
median_of() {
  local n="$1"; shift
  local s e
  {
    for _ in $(seq 1 "$n"); do
      s=$(now_ms); "$@" >/dev/null 2>&1; e=$(now_ms)
      echo $(( e - s ))
    done
  } | sort -n | awk -v n="$n" 'NR==int((n+1)/2)'
}
