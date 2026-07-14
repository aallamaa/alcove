#!/bin/bash
# Perf-regression gate (make bench-gate): fails when HEAD is >15% slower
# than a baseline ref on any gate kernel.
#
# Methodology (designed for noisy shared CI runners):
#   - Build BOTH the baseline ref and HEAD in the same job, on the same
#     runner (the baseline in a temporary git worktree — the working tree
#     is never touched), and run them INTERLEAVED (A B A B ...) so noisy
#     neighbors and frequency scaling hit both binaries alike. Absolute
#     times are never compared across runs or machines.
#   - min-of-15 with 2 unrecorded warmups per kernel: cloud noise only
#     ever ADDS time, so the minimum is the closest estimate of the
#     uncontended execution path.
#   - Both binaries run the SAME kernel files (from the current tree), and
#     their checksummed outputs must AGREE — a wrong answer fails the gate
#     independently of timing.
#
# Usage: tools/bench_gate.sh [BASE_REF]
#   BASE_REF default: merge-base of origin/master and HEAD; if that IS
#   HEAD (a push run on master), HEAD^ is used so every commit is gated
#   against its parent.

set -eu

cd "$(dirname "$0")/.."

KERNELS="gate-fix gate-float gate-mat gate-cons gate-dict gate-nlc"
RUNS=15
WARMUPS=2
THRESHOLD=115 # percent: HEAD_min*100/BASE_min above this fails

BASE_REF="${1:-}"
if [ -z "$BASE_REF" ]; then
  BASE_REF=$(git merge-base origin/master HEAD 2>/dev/null || git rev-parse HEAD^)
  if [ "$(git rev-parse "$BASE_REF")" = "$(git rev-parse HEAD)" ]; then
    BASE_REF=$(git rev-parse HEAD^)
  fi
fi
echo "== bench gate: HEAD vs $(git rev-parse --short "$BASE_REF") =="

WT=$(mktemp -d -t alcove-bench-base.XXXXXX)
cleanup() {
  git worktree remove --force "$WT" 2>/dev/null || true
  rm -rf "$WT"
  rm -f alcove_gate_head
}
trap cleanup EXIT INT TERM

echo "-- building baseline ($BASE_REF) in a worktree --"
git worktree add --force --detach "$WT" "$BASE_REF" >/dev/null
make -C "$WT" >/dev/null 2>&1
BASE_BIN="$WT/alcove"

echo "-- building HEAD --"
make >/dev/null 2>&1
cp alcove alcove_gate_head
HEAD_BIN=./alcove_gate_head

# measure_ms BIN KERNEL -> echoes "elapsed_ms output_checksum"
measure_ms() {
  local t0 t1 out
  t0=${EPOCHREALTIME/./}
  out=$("$1" --noload --no-init "benchmark/$2.alc" 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g')
  t1=${EPOCHREALTIME/./}
  echo "$(((t1 - t0) / 1000)) $out"
}

SUMMARY="${GITHUB_STEP_SUMMARY:-/dev/null}"
TABLE="| kernel | base min (ms) | HEAD min (ms) | ratio | status |
|---|---|---|---|---|"
FAIL=0

for k in $KERNELS; do
  for _ in $(seq 1 $WARMUPS); do
    "$BASE_BIN" --noload --no-init "benchmark/$k.alc" >/dev/null 2>&1
    "$HEAD_BIN" --noload --no-init "benchmark/$k.alc" >/dev/null 2>&1
  done
  base_min=99999999 head_min=99999999 base_out="" head_out=""
  for _ in $(seq 1 $RUNS); do
    read -r t o <<<"$(measure_ms "$BASE_BIN" "$k")"
    ((t < base_min)) && base_min=$t
    base_out=$o
    read -r t o <<<"$(measure_ms "$HEAD_BIN" "$k")"
    ((t < head_min)) && head_min=$t
    head_out=$o
  done
  status="PASS"
  if [ "$base_out" != "$head_out" ]; then
    status="FAIL (output: base='$base_out' head='$head_out')"
    FAIL=1
  fi
  ratio_pct=$((head_min * 100 / (base_min > 0 ? base_min : 1)))
  if [ "$ratio_pct" -gt "$THRESHOLD" ]; then
    status="FAIL (+$((ratio_pct - 100))% > $((THRESHOLD - 100))%)"
    FAIL=1
  fi
  printf '%-11s base=%5dms head=%5dms ratio=%d%% %s\n' \
    "$k" "$base_min" "$head_min" "$ratio_pct" "$status"
  TABLE="$TABLE
| $k | $base_min | $head_min | ${ratio_pct}% | $status |"
done

echo "$TABLE" >>"$SUMMARY"
if [ "$FAIL" -ne 0 ]; then
  echo "== BENCH GATE FAILED: a kernel regressed past ${THRESHOLD}% or diverged =="
  echo "**❌ bench gate failed — see table above**" >>"$SUMMARY"
  exit 1
fi
echo "== BENCH GATE PASSED =="
