#!/bin/sh
# --dev class-redefinition gate (make dev-redefine-test).
#
# defclass rejects redefinition by design: two incompatible shapes for one
# name make is-a? identity meaningless. That is right for programs and
# miserable at a REPL, so `--dev` re-registers over the existing entry while
# KEEPING its type id.
#
# Both halves matter and both are asserted here:
#   - WITHOUT --dev a redefinition still errors (a normal build is unchanged,
#     so nothing shipped can come to depend on the flag)
#   - WITH --dev the redefinition takes, instances made BEFORE it stay
#     is-a?/type-of correct, and the NEW schema's validator is the one
#     enforced afterwards (reattachment, not just re-registration)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT INT TERM

cat > "$D/redef.alc" <<'EOF'
;; Standalone: `assert` lives in test.alc, not the language — check by hand
;; and exit non-zero on the first surprise.
(= fails 0)
(def ck (name got want)
  (if (iso got want)
      (prn (str "  ok   " name))
      (do (= fails (+ fails 1))
          (prn (str "  FAIL " name ": got " (str got) " want " (str want))))))
(defclass P (n Int))
(= before (P 1))
(ck "v1 validator rejects the wrong type" (error? (try (P "x") (fn (e) e))) t)
(defclass P (n String))
(ck "redefined: new validator rejects the old type" (error? (try (P 5) (fn (e) e))) t)
(ck "redefined: new schema accepts" (P-n (P "ok")) "ok")
(ck "pre-existing instance keeps is-a?" (is-a? before P) t)
(ck "pre-existing instance keeps type-of" (str (type-of before)) "P")
(if (> fails 0) (exit 1) (prn "DEV REDEFINE DRIVER OK"))
EOF

echo "== without --dev: redefinition must still be refused =="
if ./alcove --noload --no-init "$D/redef.alc" >"$D/out.plain" 2>&1; then
  echo "  FAILED — a normal build accepted the redefinition"
  sed 's/^/  /' "$D/out.plain" | head -20
  echo "==> DEV REDEFINE FAILED"; exit 1
fi
if ! grep -q "defclass" "$D/out.plain"; then
  echo "  FAILED — refused, but not by defclass:"
  sed 's/^/  /' "$D/out.plain" | head -20
  echo "==> DEV REDEFINE FAILED"; exit 1
fi
echo "  OK — normal build unchanged (defclass still refuses)"

echo "== with --dev: redefinition takes, identity and validator survive =="
if ! ./alcove --dev --noload --no-init "$D/redef.alc" >"$D/out.dev" 2>&1; then
  echo "  FAILED — --dev run errored:"
  sed 's/^/  /' "$D/out.dev" | head -30
  echo "==> DEV REDEFINE FAILED"; exit 1
fi
if ! grep -q 'DEV REDEFINE DRIVER OK' "$D/out.dev"; then
  echo "  FAILED — driver did not report OK:"
  sed 's/\x1b\[[0-9;]*m//g' "$D/out.dev" | head -20
  echo "==> DEV REDEFINE FAILED"; exit 1
fi
sed 's/\x1b\[[0-9;]*m//g' "$D/out.dev" | grep -E '^  (ok|FAIL)' | sed 's/^/  /' 

echo "==> DEV REDEFINE PASSED"
