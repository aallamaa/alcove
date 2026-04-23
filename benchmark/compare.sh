#!/bin/bash
# Build both variants (multithreaded-atomic and single-threaded) and
# run the benchmark suite against each, printing a side-by-side table.
set -u
cd "$(dirname "$0")/.."

CC=${CC:-cc}
PYTHON=${PYTHON:-python3}

mkdir -p benchmark/bin

echo "Building MT  (default: __sync atomics) -> benchmark/bin/alcove-mt ..."
$CC -Wall -W -O3 -o benchmark/bin/alcove-mt alcove.c -lm
echo "Building ST  (ALCOVE_SINGLE_THREADED=1) -> benchmark/bin/alcove-st ..."
$CC -Wall -W -O3 -DALCOVE_SINGLE_THREADED=1 -o benchmark/bin/alcove-st alcove.c -lm

cd benchmark

now_ms() { "$PYTHON" -c 'import time; print(int(time.time()*1000))'; }
best_of() {
  local n="$1"; shift
  local best=999999999 s e t
  for _ in $(seq 1 "$n"); do
    s=$(now_ms); "$@" >/dev/null 2>&1; e=$(now_ms)
    t=$((e-s)); [ "$t" -lt "$best" ] && best=$t
  done
  echo "$best"
}

printf "%-22s %10s %10s %10s %10s\n" benchmark "mt(ms)" "st(ms)" "st/mt" "Δ"
printf -- '-%.0s' {1..68}; echo
for prog in fib fact forsum countdown; do
  MT=$(best_of 5 "$(pwd)/bin/alcove-mt" "$prog.alc")
  ST=$(best_of 5 "$(pwd)/bin/alcove-st" "$prog.alc")
  RATIO=$("$PYTHON" -c "print(f'{$ST/$MT:.2f}x')" 2>/dev/null || echo "-")
  DELTA_MS=$((ST - MT))
  DELTA_PCT=$("$PYTHON" -c "print(f'{($ST-$MT)*100/$MT:+.1f}%')" 2>/dev/null || echo "-")
  printf "%-22s %8d %8d %10s %7s / %s\n" \
    "$prog" "$MT" "$ST" "$RATIO" "${DELTA_MS}ms" "$DELTA_PCT"
done
echo
echo "Lower 'st/mt' = atomics-free is faster. 1.00x = no difference."
