#!/bin/sh
# adfmt_test.sh — gate the Adder formatter (`adder fmt`).
#
# For every checked-in .adr corpus file it asserts the two properties that make
# the formatter trustworthy:
#   1. MEANING-PRESERVING — transpiling the original and the formatted source
#      through adr.h (via the `adfmt_sexpr` probe) yields IDENTICAL s-exprs, so
#      formatting only ever changes layout, never behavior.
#   2. IDEMPOTENT — fmt(fmt(x)) == fmt(x): a formatted file is a fixed point.
#
# Also checks that an ALCOVE s-expr file round-trips to Adder and back with the
# same meaning (the `adder fmt file.alc` -> indented Adder path).
#
# Skips cleanly if `adder` isn't built.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
CC=${CC:-cc}
ADDER=./adder
[ -x "$ADDER" ] || { echo "  (./adder not built — adfmt gate skipped)"; echo "==> ADFMT SKIPPED"; exit 0; }

D=$(mktemp -d); trap 'rm -rf "$D"' EXIT

# a tiny probe that prints adr.h's s-expr for a file (the semantic oracle)
cat > "$D/sx.c" <<'EOF'
#include "adr.h"
int main(int c, char**v){ if(c<2) return 1; FILE*f=fopen(v[1],"rb"); if(!f) return 2;
  char*b=malloc(1<<22); size_t n=fread(b,1,(1<<22)-1,f); b[n]=0; fclose(f);
  fputs(als_to_sexpr(b), stdout); return 0; }
EOF
$CC -O2 -I. -o "$D/sx" "$D/sx.c" 2>"$D/err" || { echo "  probe build failed:"; cat "$D/err"; echo "==> ADFMT FAILED"; exit 1; }

fail=0
check() { # $1 = file
  f=$1
  "$ADDER" fmt "$f" > "$D/f1" 2>/dev/null
  "$ADDER" fmt "$D/f1" > "$D/f2" 2>/dev/null
  "$D/sx" "$f"     > "$D/a" 2>/dev/null
  "$D/sx" "$D/f1"  > "$D/b" 2>/dev/null
  if ! diff -q "$D/a" "$D/b" >/dev/null 2>&1; then
    echo "  MEANING CHANGED: $f"; diff "$D/a" "$D/b" | head -6 | sed 's/^/    /'; fail=1; return
  fi
  if ! diff -q "$D/f1" "$D/f2" >/dev/null 2>&1; then
    echo "  NOT IDEMPOTENT: $f"; diff "$D/f1" "$D/f2" | head -6 | sed 's/^/    /'; fail=1; return
  fi
  echo "  OK  $f"
}

echo "== formatter: meaning-preserving + idempotent over the .adr corpus =="
for f in examples/adder/*.adr lib/repl.adr test.adr; do
  [ -f "$f" ] && check "$f"
done

echo "== alcove s-expr -> indented Adder round-trips =="
printf '(def fib (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))\n(def m () (prn (fib 10)))\n' > "$D/in.alc"
check "$D/in.alc"

if [ "$fail" = 0 ]; then echo "==> ADFMT PASSED"; exit 0; fi
echo "==> ADFMT FAILED"; exit 1
