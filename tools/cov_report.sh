#!/bin/sh
# cov_report.sh — summarize gcov line-coverage for the trust-critical fragments.
#
# Run after a `--coverage` build of alcove.c has executed (so the .gcda exists).
# alcove.c is a single translation unit that #includes every fragment, so one
# instrumented build + one gcov pass yields per-fragment coverage. Prints a table
# for the hot path (evaluator / VM / JIT / core data structures) plus a weighted
# aggregate. SIGNAL ONLY — it answers "are we exercising the evaluator enough?";
# it never fails the build (no threshold gate, which would just be flaky).
#
# Usage: sh tools/cov_report.sh [obj-basename]   (default: alcove_cov-alcove)
OBJ=${1:-alcove_cov-alcove}
HOT="alcove.c reader.c env.h numeric.h print.h vector.h dict.h hamt.h set.h \
blob.h msgpack.h json.h deque.h persist.h \
builtins.h builtins_stdlib.h builtins_control.h builtins_dict.h \
builtins_os.h builtins_regex.h jit_common.h jit_amd64.h jit_arm64.h"

gcov -n -o . "$OBJ" 2>/dev/null | awk -v hot="$HOT" '
  BEGIN { n = split(hot, H, " "); for (i = 1; i <= n; i++) want[H[i]] = 1 }
  /^File / { f = $2; gsub(/'\''/, "", f); cur = f }
  /^Lines executed:/ {
    if (cur in want) {
      # line form: "Lines executed:69.26% of 6889"
      split($0, a, ":"); split(a[2], b, "%"); pct = b[1] + 0;
      ntot = $NF + 0;
      hit[cur] = pct; tot[cur] = ntot;
      execlines += pct * ntot / 100; sumlines += ntot;
    }
  }
  END {
    printf "    %-24s %8s %9s\n", "FRAGMENT", "LINES", "COVERED";
    nn = split(hot, H, " ");
    for (i = 1; i <= nn; i++) {
      k = H[i];
      if (k in hit) printf "    %-24s %8d %8.1f%%\n", k, tot[k], hit[k];
    }
    if (sumlines > 0)
      printf "    %-24s %8d %8.1f%%\n", "-- hot-path total", sumlines,
             100 * execlines / sumlines;
  }'
