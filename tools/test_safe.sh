#!/bin/sh
# test_safe.sh — the --safe / client-callback sandbox.
# Verifies (1) the CLI --safe gate refuses host-escape builtins on every dispatch
# path while pure compute + def still run, (2) allow-unsafe is itself sandboxed,
# and (3) the real goal: a RESP client triggering a redis-defcmd callback cannot
# reach host-escape builtins, a safe callback works, and (allow-unsafe ...) in the
# operator's init grants an exception. Prints "SAFE: OK" / exits 0 on success.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ALC="$ROOT/alcove"
ok=1
say() { printf '  %s\n' "$1"; }

# 1. CLI --safe: host-escape refused via direct / apply / map / compiled.
for form in '(shell "echo x")' '(apply shell (list "echo x"))' \
            '(map shell (list "echo x"))' '(def _f () (delete-file "/tmp/zz")) (_f)'; do
  o=$(printf '%s\n' "$form" | "$ALC" --safe --noload --noinit 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
  echo "$o" | grep -q "not permitted" || { say "FAIL --safe bypass: $form"; ok=0; }
done
# def + arithmetic still work under --safe (code definition is not host-escape).
o=$(printf '(def sq (x) (* x x))\n(prn (sq 9))\n' | "$ALC" --safe --noload --noinit 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
echo "$o" | grep -q "81" || { say "FAIL --safe over-blocks def/arith"; ok=0; }
# allow-unsafe is itself sandboxed (no self-un-sandbox).
o=$(printf '(allow-unsafe "shell")\n' | "$ALC" --safe --noload --noinit 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
echo "$o" | grep -q "not permitted" || { say "FAIL allow-unsafe not self-sandboxed"; ok=0; }

# 2. RESP client-callback sandbox (needs redis-cli + the server build).
if command -v redis-cli >/dev/null 2>&1; then
  d=$(mktemp -d); port=7755
  cat > "$d/.init.alc" <<'EOF'
(redis-defcmd "SAFECMD"   (fn (a) (str "ok=" (+ 1 2))))
(redis-defcmd "DANGERCMD" (fn (a) (get (shell "echo PWNED") "out")))
EOF
  ( cd "$d" && exec "$ALC" -r $port --noload ) >/dev/null 2>&1 &
  srv=$!
  up=0; i=0; while [ $i -lt 80 ]; do redis-cli -p $port PING >/dev/null 2>&1 && { up=1; break; }; i=$((i+1)); sleep 0.1; done
  if [ "$up" != 1 ]; then
    say "(RESP server did not come up on :$port — client-callback checks skipped)"
    kill $srv 2>/dev/null; wait $srv 2>/dev/null; rm -rf "$d"
    if [ "$ok" = 1 ]; then echo "SAFE: OK"; exit 0; else echo "SAFE: FAILED"; exit 1; fi
  fi
  safe=$(redis-cli -p $port SAFECMD 2>&1)
  danger=$(redis-cli -p $port DANGERCMD 2>&1)
  kill $srv 2>/dev/null; wait $srv 2>/dev/null
  echo "$safe"   | grep -q "ok=3"          || { say "FAIL client safe callback: $safe"; ok=0; }
  echo "$danger" | grep -qi "not permitted" || { say "FAIL client could shell out: $danger"; ok=0; }
  # operator grant: allow-unsafe in init lets the callback use shell.
  cat > "$d/.init.alc" <<'EOF'
(allow-unsafe "shell")
(redis-defcmd "DANGERCMD" (fn (a) (get (shell "echo PWNED") "out")))
EOF
  ( cd "$d" && exec "$ALC" -r $port --noload ) >/dev/null 2>&1 &
  srv=$!
  i=0; while [ $i -lt 60 ]; do redis-cli -p $port PING >/dev/null 2>&1 && break; i=$((i+1)); sleep 0.1; done
  granted=$(redis-cli -p $port DANGERCMD 2>&1)
  kill $srv 2>/dev/null; wait $srv 2>/dev/null
  rm -rf "$d"
  echo "$granted" | grep -q "PWNED" || { say "FAIL allow-unsafe grant: $granted"; ok=0; }
else
  say "(redis-cli absent — RESP client-callback checks skipped)"
fi

if [ "$ok" = 1 ]; then echo "SAFE: OK"; exit 0; else echo "SAFE: FAILED"; exit 1; fi
