#!/bin/bash
# alcove vs python3 micro-benchmarks
# Usage: ./benchmark/run.sh   (or: make benchmark)
#
# Reports best-of-15 wall-clock for each benchmark, plus median-of-20 startup,
# plus a summary table with computed "net" (total minus startup) and speedup.

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

. ./lib.sh

printf "alcove  : %s\n" "$("$ALCOVE" <<<'(prn "ok")' 2>/dev/null | head -1 || echo ok)"
printf "python3 : %s\n\n" "$("$PYTHON" --version 2>&1)"

# Verify each benchmark actually runs and produces the right answer before
# we time it. alcove can exit 0 on an internal runtime error (the REPL
# prints the error and keeps going, so a file-mode run that hits an error
# still terminates cleanly with empty stdout) — without this gate the
# timer would happily measure "how fast can we crash silently".
#
# Strategy: run both impls once with stdout captured, strip ANSI colour
# escapes from the alcove output, and require the two trimmed strings to
# match. Any mismatch (including empty alcove output) aborts the suite.
strip_ansi() { sed $'s/\x1b\\[[0-9;]*m//g'; }

# verify_exact: alcove output (after stripping ANSI) must equal python's.
# Used for the deterministic benchmarks where both impls compute the same
# integer answer.
verify_exact() {
  local prog="$1" alc_out py_out
  alc_out=$("$ALCOVE" --noload "$prog.alc" 2>/dev/null | strip_ansi | tr -d '[:space:]') \
    || { echo "verify: alcove crashed running $prog.alc" >&2; exit 1; }
  py_out=$("$PYTHON" "$prog.py" 2>/dev/null | tr -d '[:space:]') \
    || { echo "verify: python crashed running $prog.py" >&2; exit 1; }
  if [ -z "$alc_out" ]; then
    echo "verify: alcove produced no output for $prog.alc (silent failure)" >&2
    exit 1
  fi
  if [ "$alc_out" != "$py_out" ]; then
    echo "verify: output mismatch for $prog" >&2
    echo "  alcove : $alc_out" >&2
    echo "  python : $py_out" >&2
    exit 1
  fi
}

# verify_nonempty: just confirm alcove produces SOME numeric output. For
# stochastic benchmarks (mlp uses different RNGs in each impl, so the
# trained-model accuracy drifts a few tenths of a percent between runs)
# the exact-match check is too strict, but we still want to catch the
# silent-failure case where alcove exits 0 with no output at all.
verify_nonempty() {
  local prog="$1" alc_out
  alc_out=$("$ALCOVE" --noload "$prog.alc" 2>/dev/null | strip_ansi | tr -d '[:space:]') \
    || { echo "verify: alcove crashed running $prog.alc" >&2; exit 1; }
  if [ -z "$alc_out" ]; then
    echo "verify: alcove produced no output for $prog.alc (silent failure)" >&2
    exit 1
  fi
  if ! [[ "$alc_out" =~ ^-?[0-9]+(\.[0-9]+)?$ ]]; then
    echo "verify: alcove output for $prog is not a number: $alc_out" >&2
    exit 1
  fi
}

echo "=== verify (alcove actually runs and computes the right answer) ==="
for prog in fib fact forsum countdown ackermann listsum sieve sieve-fast nqueens nqueens-vec tak; do
  verify_exact "$prog"
  printf "  %-12s ok\n" "$prog"
done
for prog in mlp; do
  verify_nonempty "$prog"
  printf "  %-12s ok (stochastic — non-empty check)\n" "$prog"
done
echo ""

# startup (empty script)
echo "=== startup (median of 20) ==="
STARTUP_ALC=$(median_of 20 "$ALCOVE" --noload empty.alc)
STARTUP_PY=$(median_of 20 "$PYTHON"   empty.py)
printf "  alcove   %s\n"   "$(format_ms "$STARTUP_ALC")"
printf "  python3  %s\n\n" "$(format_ms "$STARTUP_PY")"

# the micro-benchmarks (best of 15)
declare -a ROWS
for prog in fib fact forsum countdown ackermann listsum sieve sieve-fast nqueens nqueens-vec tak; do
  echo "=== $prog (best of 15) ==="
  A=$(best_of 15 "$ALCOVE" --noload "$prog.alc")
  P=$(best_of 15 "$PYTHON"  "$prog.py")
  printf "  alcove   %s\n"   "$(format_ms "$A")"
  printf "  python3  %s\n"   "$(format_ms "$P")"
  ROWS+=( "$prog $A $P" )
  echo ""
done

# heavy benchmarks (best of 3 — each run is hundreds of ms to seconds)
for prog in mlp; do
  echo "=== $prog (best of 3) ==="
  A=$(best_of 3 "$ALCOVE" --noload "$prog.alc")
  P=$(best_of 3 "$PYTHON"  "$prog.py")
  printf "  alcove   %s\n"   "$(format_ms "$A")"
  printf "  python3  %s\n"   "$(format_ms "$P")"
  ROWS+=( "$prog $A $P" )
  echo ""
done

# summary table
#
# "net" subtracts the median startup from the best-of-N total. When the
# workload is faster than startup jitter (best_total < median_startup),
# the subtraction underflows; we mark those rows "≤noise" rather than
# clamp to 1 µs and print a meaningless 100000x speedup ratio.
NOISE_FLOOR_US=500  # any net <500µs is dominated by startup variance
echo "=== summary ==="
printf "%-22s %14s %14s %14s %14s %14s\n" benchmark alcove python net_alc net_py "speedup"
printf -- '-%.0s' {1..98}; echo
for r in "${ROWS[@]}"; do
  set -- $r
  name=$1; a=$2; p=$3
  net_a=$(( a - STARTUP_ALC ))
  net_p=$(( p - STARTUP_PY  ))
  if [ "$net_a" -lt "$NOISE_FLOOR_US" ] || [ "$net_p" -lt "$NOISE_FLOOR_US" ]; then
    net_a_str="<noise"; net_p_str="<noise"; speedup="startup-bound"
    [ "$net_a" -ge "$NOISE_FLOOR_US" ] && net_a_str=$(format_ms "$net_a")
    [ "$net_p" -ge "$NOISE_FLOOR_US" ] && net_p_str=$(format_ms "$net_p")
  else
    net_a_str=$(format_ms "$net_a")
    net_p_str=$(format_ms "$net_p")
    # speedup = py/alc ; > 1 means alcove faster, < 1 means python faster.
    speedup=$("$PYTHON" -c "
r = $net_p / $net_a
if r >= 1: print(f'{r:.1f}x alcove')
else:      print(f'{1/r:.1f}x python')")
  fi
  printf "%-22s %14s %14s %14s %14s %14s\n" \
    "$name" "$(format_ms "$a")" "$(format_ms "$p")" \
    "$net_a_str" "$net_p_str" "$speedup"
done
