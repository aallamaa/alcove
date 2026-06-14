#!/usr/bin/env python3
"""test_debug.py — end-to-end check of the gdb-style debugger (--debug / (break)).

Spawns `alcove --debug <script>`, feeds debug commands on stdin, and asserts on
the (ANSI-stripped) output: function + line breakpoints, full backtrace (TCO is
disabled under --debug so tail-called frames are real), frame/locals/p, step and
next, and the (break) builtin. Self-gating: prints "DEBUG: OK" and exits 0 only
if every check passes.

Run from the repo root:  python3 tools/test_debug.py
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALCOVE = os.path.join(ROOT, "alcove")
ANSI = re.compile(r"\x1b\[[0-9;]*m")
passed = 0
failed = 0


def check(name, ok):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print(f"FAIL: {name}")


def run(script_text, commands, extra_args=(), binary=ALCOVE, suffix=".alc"):
    with tempfile.NamedTemporaryFile("w", suffix=suffix, delete=False) as f:
        f.write(script_text)
        path = f.name
    try:
        p = subprocess.run(
            [binary, *extra_args, "--noload", "--noinit", path],
            input="".join(c + "\n" for c in commands),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            cwd=ROOT, text=True, timeout=20)
        return ANSI.sub("", p.stdout)
    finally:
        os.unlink(path)


DIV = "(def toto (x)\n  (/ 1 x))\n(def test (x)\n  (toto (* x 2)))\n(test 0.)\n"

# 1. function breakpoint + full backtrace (toto is TAIL-called from test, yet
#    both frames must appear because --debug disables TCO).
out = run(DIV, ["break toto", "c", "bt", "locals", "p x", "p (* x 100)", "c"],
          extra_args=["--debug"])
check("function breakpoint hit", "break in toto" in out)
check("bt shows callee toto", re.search(r"#\d+ toto", out) is not None)
check("bt shows caller test (no TCO collapse)",
      re.search(r"#\d+ test", out) is not None)
check("locals shows the param", "x = 0" in out)
check("p evaluates in the frame", "\n  0\n" in out or " 0\n" in out)
check("division error still surfaces", "Illegal division by 0" in out)

# 1b. break-on-raise: an uncaught error drops into the debugger at the raise
#     site with live frames — no breakpoint set, just `c` then the error.
out = run(DIV, ["c", "bt", "locals", "p x", "c"], extra_args=["--debug"])
check("break-on-raise drops in", "error raised" in out)
check("break-on-raise: live locals at raise", "x = 0" in out)
check("break-on-raise: bt has both frames",
      re.search(r"#\d+ toto", out) is not None and
      re.search(r"#\d+ test", out) is not None)

# 1c. a try-wrapped error must NOT break (it's handled).
TRY = "(def boom (x) (/ 1 x))\n(try (boom 0.) (fn (e) 42))\n"
out = run(TRY, ["c", "c"], extra_args=["--debug"])
check("try suppresses break-on-raise", "error raised" not in out)

# 1d. `return <expr>` recovers — the failing expression yields the value (the
#     expr is evaluated in the failing frame, where x is in scope), no error.
REC = ('(def toto (x) (/ 1 x))\n(def test (x) (toto (* x 2)))\n'
       '(prn (str "R=" (test 0.)))\n')
out = run(REC, ["c", "return (+ x 700)"], extra_args=["--debug"])
check("return recovers with an in-frame value", "R=700" in out)

# 2. stepping: step descends into a callee, next steps over it.
STEP = ("(def add (a b)\n  (+ a b))\n(def main ()\n  (let (x 5)\n"
        "    (add x 10)\n    (prn x)))\n(main)\n")
out = run(STEP, ["break main", "c", "step", "step", "bt", "next", "bt", "c"],
          extra_args=["--debug"])
check("step into callee (add)", "break in add" in out)
check("next returns to caller (main)",
      out.count("break in main") >= 2)
check("line numbers tracked", "line 5" in out or "line 6" in out)

# 3. (break) builtin works without --debug (locals/p from the live env).
BRK = "(def f (n)\n  (break)\n  (* n n))\n(prn (f 7))\n"
out = run(BRK, ["locals", "p n", "p (* n 2)", "c"])
check("(break) drops into the debugger", "(dbg)" in out)
check("(break) locals from live env", "n = 7" in out)
check("(break) p computes", "14" in out)
check("(break) continue resumes program", "49" in out)

# 4. Adder (.adr) debugging — same debugger, lines mapped back to Adder source.
adder = os.path.join(ROOT, "adder")
if os.path.exists(adder):
    ADR = ("def toto (x):\n  / 1 x\ndef test (x):\n  toto (* x 2)\ntest 0.\n")
    out = run(ADR, ["break toto", "c", "bt", "locals", "p x", "c"],
              extra_args=["--debug"], binary=adder, suffix=".adr")
    check("adder: breakpoint hit", "break in toto" in out)
    check("adder: bt shows both frames",
          re.search(r"#\d+ toto", out) is not None and
          re.search(r"#\d+ test", out) is not None)
    check("adder: locals from live frame", "x = 0" in out)
    check("adder: error caret maps to .adr source",
          re.search(r"\.adr:\d", out) is not None)
else:
    print("note: no ./adder binary — Adder debug checks skipped")

print(f"debug tests: {passed} passed, {failed} failed")
print("DEBUG: OK" if failed == 0 else "DEBUG: FAILED")
sys.exit(1 if failed else 0)
