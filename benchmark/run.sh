#!/bin/bash
# alcove vs python3 micro-benchmarks
# Usage: ./benchmark/run.sh   (or: make benchmark)
#
# Reports best-of-5 wall-clock for each benchmark, plus median-of-20 startup,
# plus a summary table with computed "net" (total minus startup) and slowdown.

set -u
cd "$(dirname "$0")"

ALCOVE="${ALCOVE:-../alcove}"
PYTHON="${PYTHON:-python3}"

if [ ! -x "$ALCOVE" ]; then
  echo "error: $ALCOVE not found or not executable. Build first: make speed" >&2
  exit 1
fi
if ! command -v "$PYTHON" >/dev/null 2>&1; then
  echo "error: $PYTHON not on PATH" >&2
  exit 1
fi

# milliseconds-since-epoch (portable enough for macOS /opt/homebrew python3)
now_ms() { "$PYTHON" -c 'import time; print(int(time.time()*1000))'; }

# best-of-N wall-clock in ms
best_of() {
  local n="$1"; shift
  local best=999999999 t s e
  for _ in $(seq 1 "$n"); do
    s=$(now_ms)
    "$@" >/dev/null 2>&1
    e=$(now_ms)
    t=$(( e - s ))
    if [ "$t" -lt "$best" ]; then best=$t; fi
  done
  echo "$best"
}

# median-of-N wall-clock in ms
median_of() {
  local n="$1"; shift
  local s e
  {
    for _ in $(seq 1 "$n"); do
      s=$(now_ms)
      "$@" >/dev/null 2>&1
      e=$(now_ms)
      echo $(( e - s ))
    done
  } | sort -n | awk -v n="$n" 'NR==int((n+1)/2)'
}

printf "alcove  : %s\n" "$("$ALCOVE" <<<'(prn "ok")' 2>/dev/null | head -1 || echo ok)"
printf "python3 : %s\n\n" "$("$PYTHON" --version 2>&1)"

# startup (empty script)
echo "=== startup (median of 20) ==="
STARTUP_ALC=$(median_of 20 "$ALCOVE"  empty.alc)
STARTUP_PY=$(median_of 20 "$PYTHON"   empty.py)
printf "  alcove   %5d ms\n"   "$STARTUP_ALC"
printf "  python3  %5d ms\n\n" "$STARTUP_PY"

# the four benchmarks (best of 5)
declare -a ROWS
for prog in fib fact forsum countdown; do
  echo "=== $prog (best of 5) ==="
  A=$(best_of 5 "$ALCOVE"  "$prog.alc")
  P=$(best_of 5 "$PYTHON"  "$prog.py")
  printf "  alcove   %5d ms\n"   "$A"
  printf "  python3  %5d ms\n"   "$P"
  ROWS+=( "$prog $A $P" )
  echo ""
done

# summary table
echo "=== summary ==="
printf "%-22s %10s %10s %10s %10s %10s\n" benchmark alcove python net_alc net_py "alc/py"
printf -- '-%.0s' {1..78}; echo
for r in "${ROWS[@]}"; do
  set -- $r
  name=$1; a=$2; p=$3
  net_a=$(( a - STARTUP_ALC )); [ "$net_a" -lt 1 ] && net_a=1
  net_p=$(( p - STARTUP_PY  )); [ "$net_p" -lt 1 ] && net_p=1
  ratio=$("$PYTHON" -c "print(f'{$net_a/$net_p:.1f}x')")
  printf "%-22s %7d ms %7d ms %7d ms %7d ms %10s\n" \
    "$name" "$a" "$p" "$net_a" "$net_p" "$ratio"
done
