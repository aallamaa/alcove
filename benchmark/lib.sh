# Shared benchmark helpers. Sourced by run.sh and compare.sh.
# Expects PYTHON to be set (or defaults to python3).

: "${PYTHON:=python3}"

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
