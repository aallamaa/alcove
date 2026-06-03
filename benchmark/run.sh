#!/bin/bash
# alcove vs C vs python3 micro-benchmarks
# Usage: ./benchmark/run.sh   (or: make benchmark)
#
# Reports best-of-15 wall-clock for each benchmark across three implementations
# — alcove, native C (cc -O2), and python3 — plus median-of-20 startup, and a
# summary table with startup-adjusted "net" times and slowdown ratios vs the
# native-C baseline (alcove÷C and python÷C). Each C program lives next to its
# .alc/.py twin (fib.c, tak.c, …) and computes the identical answer; mlp has no
# C twin (stochastic training) so its C column reads "n/a".

set -u
cd "$(dirname "$0")"

ALCOVE="${ALCOVE:-../alcove}"
PYTHON="${PYTHON:-python3}"
CC="${CC:-cc}"
CFLAGS_BENCH="${CFLAGS_BENCH:--O2}"

if [ ! -x "$ALCOVE" ]; then
  echo "error: $ALCOVE not found or not executable. Build first: make speed" >&2
  exit 1
fi
if ! command -v "$PYTHON" >/dev/null 2>&1; then
  echo "error: $PYTHON not on PATH" >&2
  exit 1
fi
HAVE_CC=1
if ! command -v "$CC" >/dev/null 2>&1; then
  echo "warning: C compiler '$CC' not found — skipping the C column" >&2
  HAVE_CC=0
fi

. ./lib.sh

# All deterministic micro-benchmarks have a C twin; mlp does not.
DETERMINISTIC="fib fact forsum countdown ackermann listsum sieve sieve-fast nqueens nqueens-vec tak"
STOCHASTIC="mlp"

# Compile the C twins (and the empty.c startup baseline) into a temp dir.
CBIN_DIR=""
has_c() { [ "$HAVE_CC" -eq 1 ] && [ -n "${CBIN_DIR:-}" ] && [ -x "$CBIN_DIR/$1" ]; }
if [ "$HAVE_CC" -eq 1 ]; then
  CBIN_DIR=$(mktemp -d)
  trap 'rm -rf "$CBIN_DIR"' EXIT
  echo "=== compiling C twins ($CC $CFLAGS_BENCH) ==="
  for prog in empty $DETERMINISTIC; do
    [ -f "$prog.c" ] || continue
    if "$CC" $CFLAGS_BENCH -o "$CBIN_DIR/$prog" "$prog.c" -lm 2>/tmp/alcbench_cc.$$; then
      printf "  %-12s compiled\n" "$prog"
    else
      echo "  $prog: C BUILD FAILED:" >&2; sed 's/^/    /' /tmp/alcbench_cc.$$ >&2
    fi
  done
  rm -f /tmp/alcbench_cc.$$
  echo ""
fi

printf "alcove  : %s\n" "$("$ALCOVE" <<<'(prn "ok")' 2>/dev/null | head -1 || echo ok)"
[ "$HAVE_CC" -eq 1 ] && printf "C       : %s (%s)\n" "$("$CC" --version 2>/dev/null | head -1)" "$CFLAGS_BENCH"
printf "python3 : %s\n\n" "$("$PYTHON" --version 2>&1)"

# Verify each benchmark actually runs and produces the right answer before
# we time it. alcove can exit 0 on an internal runtime error (the REPL
# prints the error and keeps going, so a file-mode run that hits an error
# still terminates cleanly with empty stdout) — without this gate the
# timer would happily measure "how fast can we crash silently".
strip_ansi() { sed $'s/\x1b\\[[0-9;]*m//g'; }

# verify_exact: alcove AND (if present) C output must equal python's. Used for
# the deterministic benchmarks where all impls compute the same integer answer.
verify_exact() {
  local prog="$1" alc_out py_out c_out
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
  if has_c "$prog"; then
    c_out=$("$CBIN_DIR/$prog" 2>/dev/null | tr -d '[:space:]') \
      || { echo "verify: C crashed running $prog" >&2; exit 1; }
    if [ "$c_out" != "$py_out" ]; then
      echo "verify: C output mismatch for $prog" >&2
      echo "  C      : $c_out" >&2
      echo "  python : $py_out" >&2
      exit 1
    fi
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

echo "=== verify (all impls compute the same answer) ==="
for prog in $DETERMINISTIC; do
  verify_exact "$prog"
  if has_c "$prog"; then printf "  %-12s ok (alcove=C=python)\n" "$prog"
  else printf "  %-12s ok (alcove=python; no C twin)\n" "$prog"; fi
done
for prog in $STOCHASTIC; do
  verify_nonempty "$prog"
  printf "  %-12s ok (stochastic — non-empty check)\n" "$prog"
done
echo ""

# startup (empty script / empty binary)
echo "=== startup (median of 20) ==="
STARTUP_ALC=$(median_of 20 "$ALCOVE" --noload empty.alc)
STARTUP_PY=$(median_of 20 "$PYTHON"   empty.py)
STARTUP_C=0
printf "  alcove   %s\n"   "$(format_ms "$STARTUP_ALC")"
if has_c empty; then
  STARTUP_C=$(median_of 20 "$CBIN_DIR/empty")
  printf "  C        %s\n" "$(format_ms "$STARTUP_C")"
fi
printf "  python3  %s\n\n" "$(format_ms "$STARTUP_PY")"

# the micro-benchmarks (best of 15) + heavy ones (best of 3)
declare -a ROWS
run_one() { # run_one <prog> <reps>
  local prog="$1" reps="$2" A C P
  echo "=== $prog (best of $reps) ==="
  A=$(best_of "$reps" "$ALCOVE" --noload "$prog.alc")
  printf "  alcove   %s\n" "$(format_ms "$A")"
  if has_c "$prog"; then
    C=$(best_of "$reps" "$CBIN_DIR/$prog")
    printf "  C        %s\n" "$(format_ms "$C")"
  else
    C="-"
    printf "  C        %s\n" "n/a"
  fi
  P=$(best_of "$reps" "$PYTHON" "$prog.py")
  printf "  python3  %s\n\n" "$(format_ms "$P")"
  ROWS+=( "$prog $A $C $P" )
}
for prog in $DETERMINISTIC; do run_one "$prog" 15; done
for prog in $STOCHASTIC;   do run_one "$prog" 3;  done

# summary table
#
# "net" subtracts the median startup from the best-of-N total. When the
# workload is faster than startup jitter (best_total < median_startup),
# the subtraction underflows; we mark those rows "<noise" rather than
# clamp and print a meaningless ratio. Ratios are computed from net times
# against the native-C baseline: alcove÷C and python÷C ( >1 = slower than
# C, <1 = faster than C — e.g. a JIT'd shape that beats -O2 ).
NOISE_FLOOR_US=500  # any net <500µs is dominated by startup variance
echo "=== summary (per-benchmark; C is the speed ceiling at equal work) ==="
printf "%-14s %12s %12s %12s %16s %14s\n" benchmark alcove C python "alcove/C" "vs python"
printf -- '-%.0s' {1..84}; echo
for r in "${ROWS[@]}"; do
  set -- $r
  name=$1; a=$2; c=$3; p=$4
  net_a=$(( a - STARTUP_ALC )); [ "$net_a" -lt 0 ] && net_a=0
  net_p=$(( p - STARTUP_PY  )); [ "$net_p" -lt 0 ] && net_p=0
  a_str=$(format_ms "$a"); p_str=$(format_ms "$p")
  if [ "$c" = "-" ]; then c_str="n/a"; net_c=-1; else net_c=$(( c - STARTUP_C )); [ "$net_c" -lt 0 ] && net_c=0; c_str=$(format_ms "$c"); fi
  # Category (from the JIT-shape investigation — see the footnotes / README §1):
  #   kernel = the JIT substitutes a hand-written native kernel running a BETTER
  #            algorithm than the source/C twin, so "alcove faster" is doing
  #            fewer ops, not faster ones — NOT like-for-like.
  #   cfold  = cc -O2 constant-folds the fixed-argument inner call, so C
  #            measures ~nothing; no honest ratio.
  #   fair   = same algorithm both sides; C is the ceiling, ratio is meaningful.
  case "$name" in
    fib|nqueens|nqueens-vec|listsum) kind=kernel ;;
    fact|countdown)                  kind=cfold  ;;
    mlp)                             kind=none   ;;
    *)                               kind=fair   ;;
  esac
  if [ "$kind" = kernel ]; then ac="JIT kernel †"
  elif [ "$kind" = cfold ]; then ac="C folds ‡"
  elif [ "$net_c" -lt 0 ]; then ac="n/a"
  elif [ "$net_a" -lt "$NOISE_FLOOR_US" ] || [ "$net_c" -lt "$NOISE_FLOOR_US" ]; then
    ac="~startup ‡"   # work below the spawn floor — read the absolute columns
  else
    ac=$("$PYTHON" -c "r=$net_a/$net_c; print(f'{r:.2f}x slower' if r>=1 else f'{1/r:.2f}x faster')")
  fi
  # alcove vs python: robust raw-total ratio (alcove is always far ahead).
  if [ "$name" = mlp ]; then vp=$("$PYTHON" -c "print(f'{$p/$a:.1f}x')"); else vp=$("$PYTHON" -c "print(f'{$p/$a:.0f}x')"); fi
  printf "%-14s %12s %12s %12s %16s %14s\n" "$name" "$a_str" "$c_str" "$p_str" "$ac" "$vp"
done
echo ""
echo "note: best-of-15 total wall-clock (mlp best-of-3); times include process"
echo "      startup (alcove $(format_ms "$STARTUP_ALC" | xargs), C $(format_ms "${STARTUP_C:-0}" | xargs), python $(format_ms "$STARTUP_PY" | xargs)). C is cc $CFLAGS_BENCH."
echo "      'vs python' = python/alcove (alcove's favour). On FAIR rows alcove is"
echo "      ~1-3x of -O2 C; C is the ceiling at equal work."
echo ""
echo "† NOT like-for-like — the JIT recognized the shape and ran a hand-written"
echo "  native kernel with a BETTER algorithm (fewer ops, not faster ones):"
echo "    fib           folds f(n)=f(n-1)+f(n-2) to an O(n) loop (recurse_add_two)"
echo "    nqueens(-vec) bitmask O(1)-conflict solver (nq_count_bits) vs O(row) safe?"
echo "    listsum       bump-arena cons alloc vs the C twin's malloc()/node"
echo "  A C program using the same trick would match or beat alcove."
echo "‡ C runs sub-millisecond here (cc -O2 constant-folds the fixed-arg call, or"
echo "  the work is below the startup floor) — no honest ratio; read absolute cols."
