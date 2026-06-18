#!/bin/sh
# oom_test.sh — verify OOM recovery (Tier 1.3 follow-up).
#
# A failed allocation mid-computation must abort the current top-level form with
# a surfaced out-of-memory error and let the PROCESS SURVIVE — the next form runs
# and the engine stays fully usable — instead of exit()'ing or segfaulting.
# Driven by the unsafe (alloc-fail-after N) fault-injection builtin. Run under a
# normal build AND an ASan/UBSan build, since the longjmp-based recovery must not
# corrupt state (subsequent evaluation must be clean).
#
# Usage:  sh tools/oom_test.sh        (or: make oom-test)
set -e
CC=${CC:-cc}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

SCRIPT=$(mktemp /tmp/oomtest.XXXXXX.alc)
trap 'rm -f "$SCRIPT" /tmp/oom_err.$$ /tmp/alcove_oom.$$ /tmp/alcove_oom_asan.$$' EXIT
cat > "$SCRIPT" <<'ALC'
(alloc-fail-after 1)                          ; next allocation fails
(vector 1 2 3 4 5)                            ; OOMs → form aborted, process lives
(prn (str "R1 " (+ 40 2)))                    ; engine usable again
(alloc-fail-after 2)
(hash-map "a" 1 "b" 2 "c" 3)                  ; OOM again (or completes — either ok)
(prn (str "R2 " (reduce + 0 (range 1 11))))   ; usable again (1..10 sum = 55)
(prn "DONE")
(quit)
ALC

run_check() { # run_check BINPATH LABEL ASAN?
  bin=$1; label=$2; asan=$3
  out=$(ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
        "$bin" --noload "$SCRIPT" 2>/tmp/oom_err.$$); rc=$?
  err=$(sed 's/\x1b\[[0-9;]*m//g' /tmp/oom_err.$$)
  out=$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g')
  fail=0
  printf '%s\n' "$out" | grep -q "R1 42" || { echo "  FAIL [$label]: engine dead after 1st OOM (no R1)"; fail=1; }
  printf '%s\n' "$out" | grep -q "R2 55" || { echo "  FAIL [$label]: engine dead after 2nd OOM (no R2)"; fail=1; }
  printf '%s\n' "$out" | grep -q "DONE"  || { echo "  FAIL [$label]: never reached DONE"; fail=1; }
  printf '%s\n' "$err" | grep -qi "out of memory" || { echo "  FAIL [$label]: OOM not surfaced on stderr"; fail=1; }
  if [ "$rc" = "139" ] || [ "$rc" = "134" ] || [ "$rc" -ge 128 ] 2>/dev/null; then
    echo "  FAIL [$label]: crashed (rc=$rc)"; fail=1
  fi
  if [ -n "$asan" ]; then
    printf '%s\n' "$err" | grep -qiE "AddressSanitizer|runtime error:|LeakSanitizer" \
      && { echo "  FAIL [$label]: sanitizer report"; printf '%s\n' "$err" | head -20; fail=1; }
  fi
  [ "$fail" = 0 ] && echo "  OK [$label] — OOM recovered, engine usable, no crash${asan:+ (ASan+UBSan clean)}"
  return "$fail"
}

ok=1
echo "== oom-test: normal build =="
$CC -Wall -W -fno-strict-aliasing -O2 -DALCOVE_JIT=1 -o /tmp/alcove_oom.$$ alcove.c -lm
run_check /tmp/alcove_oom.$$ normal || ok=0

echo "== oom-test: ASan+UBSan build (recovery must not corrupt) =="
$CC -Wall -W -fno-strict-aliasing -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -DALCOVE_JIT=1 -o /tmp/alcove_oom_asan.$$ alcove.c -lm
run_check /tmp/alcove_oom_asan.$$ asan asan || ok=0

[ "$ok" = 1 ] && { echo "==> OOM TEST PASSED"; exit 0; } || { echo "==> OOM TEST FAILED"; exit 1; }
