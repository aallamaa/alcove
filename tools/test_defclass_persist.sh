#!/bin/sh
# defclass instance persistence (dump v5): instances persist by class NAME, so
# a load in a DIFFERENT class-definition order keeps the right identity (beating
# the old session-ordinal type id), and loading BEFORE the class is defined
# pre-registers a claimable type that a later defclass claims.
#
# Three separate processes share one db file:
#   P1  define A (id 100000) then B (id 100001); save a B instance.
#   P2  define ONLY B (id 100000 now — the ordering trap); load; the instance
#       must still be a B (name-based remap), and its validator must fire.
#   P3  load FIRST (B undefined → claimable pre-registration keeps identity),
#       THEN defclass B claims it; a newly built instance validates.

set -eu

ALCOVE=${ALCOVE:-./alcove}
DB=$(mktemp -t alcove-defclass-persist.XXXXXX.db)
OUT=$(mktemp -t alcove-defclass-persist.XXXXXX.out)

cleanup() { rm -f "$DB" "$OUT"; }
trap cleanup EXIT INT TERM

# Shared assertion helper prepended to each process script. A failed check
# raises, which exits the process non-zero; `set -e` aborts the gate.
HELPER='
(def _fail (msg) (do (pr (str "ASSERT FAILED: " msg "\n")) (raise (quote test-fail) msg)))
(def _ck (name pass) (if pass (pr (str "  ok " name "\n")) (_fail name)))
'

run_proc() {
  # $1 = label, $2 = alcove source (already includes HELPER)
  printf '%s\n' "$2" | "$ALCOVE" --no-init --noload /dev/stdin >"$OUT" 2>&1 || {
    echo "defclass-persist FAILED in $1 (exit $?):"
    sed 's/^/  /' "$OUT"
    exit 1
  }
  sed 's/^/  /' "$OUT"
}

echo "==> P1: define A then B, save a B instance"
run_proc P1 "$HELPER
(defclass A (x Int))
(defclass B (label String) (n Int))
(_ck \"A registered\"   (iso (type-name A) \"A\"))
(_ck \"B registered\"   (iso (type-name B) \"B\"))
(= inst (B \"hello\" 5))
(_ck \"instance is B\"  (B? inst))
(persist (quote inst))
(savedb \"$DB\")
(pr \"P1 done\n\")"

echo "==> P2: define ONLY B (ordinal id shifts), load — name remap must win"
run_proc P2 "$HELPER
(defclass B (label String) (n Int))
(loaddb \"$DB\")
(_ck \"B? inst after name remap\"      (B? inst))
(_ck \"type-of inst is B\"             (iso (type-name (type-of inst)) \"B\"))
(_ck \"B-label getter\"                (iso (B-label inst) \"hello\"))
(_ck \"B-n getter\"                    (iso (B-n inst) 5))
(_ck \"validator restored: bad type\"  (error? (try (assoc! inst \"n\" \"bad\") (fn (e) e))))
(_ck \"validator restored: unknown\"   (error? (try (assoc! inst \"extra\" 1) (fn (e) e))))
(pr \"P2 done\n\")"

echo "==> P3: load BEFORE defining B (claimable pre-registration), then claim"
run_proc P3 "$HELPER
(loaddb \"$DB\")
(_ck \"identity survives pre-registration\" (iso (type-name (type-of inst)) \"B\"))
(defclass B (label String) (n Int))
(_ck \"defclass claims pre-registration\"   (B? inst))
(= fresh (B \"world\" 9))
(_ck \"new instance getter\"                (iso (B-label fresh) \"world\"))
(_ck \"new instance validates\"             (error? (try (assoc! fresh \"n\" \"bad\") (fn (e) e))))
(pr \"P3 done\n\")"

echo "==> DEFCLASS PERSIST OK"
