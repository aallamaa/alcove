#!/usr/bin/env python3
"""eval_fuzz.py — generative whole-program differential fuzzer for the evaluator.

The reader, the Adder transpiler, and the msgpack decoder all have libFuzzer
targets; the *evaluator / bytecode VM / JIT* — the code that matters most — have
only the fixed equiv_sweep form-set and jit_fuzz's narrow loop shapes. This tool
closes that gap: it generates a large batch of random, *terminating*, *pure*
programs across the full breadth of constructs (arithmetic, the numeric tower,
conditionals, let/let*, lists, vectors, strings, higher-order fns, bounded
recursion, quasiquote, …) and, for each one, checks that the AST interpreter and
the bytecode compiler/VM agree BYTE-FOR-BYTE.

For each generated expression E the harness compares
    (eval 'E)              [the AST tree-walker]
against
    ((fn () E))            [E's body compiled to bytecode → VM (+ JIT on shapes)]
via `(msgpack-encode …)`, which is type- and bit-exact (an int and a float that
print the same still differ here, so an int↔float miscompile can't hide). Any
divergence — different value, or one errors and the other doesn't — is a real
compile-vs-interpret bug. Errors are captured (so a generated type error doesn't
abort the run) and compared the same way: both tiers must error identically.

The SAME generated program is run under three builds:
  * nojit  — exercises AST≡VM.
  * jit    — exercises AST≡(VM+JIT); recursive shapes warm up and JIT.
  * asan   — the jit build under ASan+UBSan: catches any memory-safety / UB bug
             the random program shapes trip in eval→compile→VM→JIT (no crash,
             clean sanitizer is required to pass).

Generators only emit terminating, side-effect-free expressions (bounded
recursion depth, capped iteration counts, no global mutation, no I/O, no
randomness) because the harness double-evaluates each E (once via eval, once via
the thunk).

Usage:
    python3 tools/eval_fuzz.py [--seed N] [--count N] [--keep] [--no-asan]
    make eval-fuzz

Exit code 0 iff every program reported 0 MISMATCH under every build AND the asan
build was sanitizer-clean.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from random import Random

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "alcove.c")


def build(cc, out, jit=False, asan=False):
    flags = ["-w", "-fno-strict-aliasing"]
    if asan:
        flags += ["-g", "-O1", "-fsanitize=address,undefined",
                  "-fno-sanitize-recover=all"]
    else:
        flags += ["-O2"]
    if jit:
        flags.append("-DALCOVE_JIT=1")
    subprocess.run([cc, *flags, "-o", out, SRC, "-lm"], check=True, cwd=ROOT)


# ---- type-directed program generation ---------------------------------------
# gen(t, env, depth) returns a string that evaluates to a value of "type" t.
# t in {int, float, bool, str, list, any}. `env` maps in-scope var names to
# their type (int/float only — the names we can safely reference as leaves).
# Everything is pure and terminating: recursion is a bounded counting IIFE,
# iteration limits are literal and small, divisors are nonzero literals, and no
# generated form mutates a global.

INT_LIT = lambda r: r.choice([0, 1, 2, 3, 7, -1, -5, 10, 42, 100, -100,
                              32767, 32768, 1000000, -1000000])
FLOAT_LIT = lambda r: r.choice(["0.0", "1.0", "-1.0", "0.5", "2.5", "3.14",
                                "-0.25", "10.0", "100.0"])
NZ_FLOAT = lambda r: r.choice(["2.0", "0.5", "-2.0", "4.0", "0.25", "10.0"])
POS_INT = lambda r: r.choice([1, 2, 3, 5, 7, 10])
STR_LIT = lambda r: '"' + r.choice(["", "a", "hi", "abc", "x y", "ALC", "42"]) + '"'
KW_LIT = lambda r: r.choice([":a", ":b", ":k", ":yes", ":no"])


def _vars_of(env, t):
    return [n for n, vt in env.items() if vt == t]


def g_int(r, env, d):
    choices = ["lit", "lit"]
    iv = _vars_of(env, "int")
    if iv:
        choices.append("var")
    if d > 0:
        choices += ["arith", "arith", "abs", "maxmin", "mod", "if", "let",
                    "length", "case", "rec"]
    k = r.choice(choices)
    if k == "lit":
        return str(INT_LIT(r))
    if k == "var":
        return r.choice(iv)
    if k == "arith":
        op = r.choice(["+", "-", "*"])
        return f"({op} {g_int(r, env, d-1)} {g_int(r, env, d-1)})"
    if k == "abs":
        return f"(abs {g_int(r, env, d-1)})"
    if k == "maxmin":
        return f"({r.choice(['max','min'])} {g_int(r, env, d-1)} {g_int(r, env, d-1)})"
    if k == "mod":
        return f"(mod {g_int(r, env, d-1)} {POS_INT(r)})"
    if k == "if":
        return f"(if {g_bool(r, env, d-1)} {g_int(r, env, d-1)} {g_int(r, env, d-1)})"
    if k == "let":
        v = f"v{d}{r.randint(0,9)}"
        e2 = dict(env); e2[v] = "int"
        return f"(let ({v} {g_int(r, env, d-1)}) {g_int(r, e2, d-1)})"
    if k == "length":
        return f"(length {g_list(r, env, d-1)})"
    if k == "case":
        return (f"(case {g_int(r, env, d-1)} "
                f"1 {INT_LIT(r)} 2 {INT_LIT(r)} {INT_LIT(r)})")
    if k == "rec":
        # bounded sum: ((fn f (n acc) (if (< n 1) acc (f (- n 1) (+ acc n)))) K 0)
        cap = r.choice([0, 1, 3, 5, 12, 30])
        return (f"((fn f (n acc) (if (< n 1) acc (f (- n 1) (+ acc n)))) {cap} 0)")
    return str(INT_LIT(r))


def g_float(r, env, d):
    choices = ["lit", "lit"]
    fv = _vars_of(env, "float")
    if fv:
        choices.append("var")
    if d > 0:
        choices += ["arith", "div", "sqrt", "coerce", "if", "let", "round"]
    k = r.choice(choices)
    if k == "lit":
        return FLOAT_LIT(r)
    if k == "var":
        return r.choice(fv)
    if k == "arith":
        op = r.choice(["+", "-", "*"])
        return f"({op} {g_float(r, env, d-1)} {g_float(r, env, d-1)})"
    if k == "div":
        return f"(/ {g_float(r, env, d-1)} {NZ_FLOAT(r)})"
    if k == "sqrt":
        return f"(sqrt (abs {g_float(r, env, d-1)}))"
    if k == "coerce":
        return f"(float {g_int(r, env, d-1)})"
    if k == "round":
        fn = r.choice(["round", "floor", "ceil", "truncate"])
        return f"({fn} {g_float(r, env, d-1)})"
    if k == "if":
        return f"(if {g_bool(r, env, d-1)} {g_float(r, env, d-1)} {g_float(r, env, d-1)})"
    if k == "let":
        v = f"f{d}{r.randint(0,9)}"
        e2 = dict(env); e2[v] = "float"
        return f"(let ({v} {g_float(r, env, d-1)}) {g_float(r, e2, d-1)})"
    return FLOAT_LIT(r)


def g_bool(r, env, d):
    if d <= 0:
        return r.choice(["t", "nil", "t", "nil"])
    k = r.choice(["cmp", "cmp", "eq", "and", "or", "no", "pred", "zero", "lit"])
    if k == "lit":
        return r.choice(["t", "nil"])
    if k == "cmp":
        op = r.choice(["<", "<=", ">", ">="])
        return f"({op} {g_int(r, env, d-1)} {g_int(r, env, d-1)})"
    if k == "eq":
        return f"(is {g_int(r, env, d-1)} {g_int(r, env, d-1)})"
    if k == "and":
        return f"(and {g_bool(r, env, d-1)} {g_bool(r, env, d-1)})"
    if k == "or":
        return f"(or {g_bool(r, env, d-1)} {g_bool(r, env, d-1)})"
    if k == "no":
        return f"(no {g_bool(r, env, d-1)})"
    if k == "zero":
        return f"(zero? {g_int(r, env, d-1)})"
    if k == "pred":
        p = r.choice(["null?", "list?", "pair?"])
        return f"({p} {g_list(r, env, d-1)})"
    return "t"


def g_list(r, env, d):
    if d <= 0:
        return r.choice(["nil", "(list)", "(list 1 2 3)", "'(1 2 3)"])
    k = r.choice(["list", "cons", "cdr", "reverse", "cons", "map", "quote", "filter"])
    if k == "list":
        n = r.randint(0, 4)
        return "(list " + " ".join(g_int(r, env, 0) for _ in range(n)) + ")"
    if k == "cons":
        return f"(cons {g_int(r, env, d-1)} {g_list(r, env, d-1)})"
    if k == "cdr":
        return f"(cdr {g_list(r, env, d-1)})"
    if k == "reverse":
        return f"(reverse {g_list(r, env, d-1)})"
    if k == "quote":
        return r.choice(["'(1 2 3)", "'(a b c)", "'(1 (2 3) 4)", "'()"])
    if k == "map":
        return f"(map (fn (x) (* x x)) {g_list(r, env, d-1)})"
    if k == "filter":
        return f"(filter (fn (x) (> x 0)) {g_list(r, env, d-1)})"
    return "(list 1 2 3)"


def g_str(r, env, d):
    if d <= 0:
        return STR_LIT(r)
    k = r.choice(["lit", "str", "concat", "upcase", "substr"])
    if k == "lit":
        return STR_LIT(r)
    if k == "str":
        return f"(str {g_any(r, env, d-1)} {g_any(r, env, d-1)})"
    if k == "concat":
        return f"(string-concat {g_str(r, env, d-1)} {g_str(r, env, d-1)})"
    if k == "upcase":
        return f"(string-upcase {g_str(r, env, d-1)})"
    if k == "substr":
        return f"(substr {g_str(r, env, d-1)} 0 {r.randint(0,3)})"
    return STR_LIT(r)


def g_tower(r, env, d):
    """Mixed exact-numeric-tower expression: int / float / rational / decimal
    combined under +,-,*. Exercises the contagion rules (the recently-added
    exact rationals + decimals) on both tiers."""
    def atom():
        return r.choice([
            str(INT_LIT(r)), FLOAT_LIT(r),
            f"(rational {r.choice([1,2,3,5,7])} {POS_INT(r)})",
            f"{r.choice([1,2,3])}/{POS_INT(r)}",
            f'(decimal "{r.choice(["0.5","1.5","2.25","10.0","-1.5"])}")',
        ])
    if d <= 0:
        return atom()
    op = r.choice(["+", "-", "*"])
    return f"({op} {g_tower(r, env, d-1)} {g_tower(r, env, d-1)})"


def g_vec(r, env, d):
    n = r.randint(0, 4)
    elems = " ".join(g_int(r, env, 0) for _ in range(n))
    return f"(vector {elems})"


def g_any(r, env, d):
    t = r.choice(["int", "int", "float", "bool", "str", "list", "kw", "qq",
                  "tower", "vec"])
    if t == "int":
        return g_int(r, env, d)
    if t == "tower":
        return g_tower(r, env, d)
    if t == "vec":
        return g_vec(r, env, d)
    if t == "float":
        return g_float(r, env, d)
    if t == "bool":
        return g_bool(r, env, d)
    if t == "str":
        return g_str(r, env, d)
    if t == "list":
        return g_list(r, env, d)
    if t == "kw":
        return KW_LIT(r)
    if t == "qq" and d > 0:
        return f"`(1 ,{g_int(r, env, d-1)} ,@{g_list(r, env, d-1)} z)"
    return g_int(r, env, d)


def gen_program(r, count, depth):
    head = r"""(= _tested 0)(= _mismatch 0)(= _astonly 0)
(def cap-eval (form) (try (eval form) (fn (er) (list :ERR (error-message er)))))
(def cap-call (th)   (try (th)        (fn (er) (list :ERR (error-message er)))))
(def _enc (v) (try (msgpack-encode v) (fn (er) (list :ENCERR (error-message er)))))
(def chk (lbl astv th)
  (= _tested (+ _tested 1))
  (if (compiled? th)
      (if (iso (_enc astv) (_enc (cap-call th)))
          nil
          (do (= _mismatch (+ _mismatch 1))
              (pr "MISMATCH [")(pr lbl)(pr "] ast=")(pr astv)
              (pr " vm=")(prn (cap-call th))))
      (= _astonly (+ _astonly 1))))
(defmacro equiv (lbl e) `(chk ,lbl (cap-eval (quote ,e)) (fn () ,e)))
"""
    lines = [head]
    for i in range(count):
        e = g_any(r, {}, depth)
        # one-line, no embedded newlines (generators never emit any)
        lines.append(f'(equiv "p{i}" {e})')
    tail = r"""
(pr "EVAL FUZZ: ")(pr _tested)(pr " tested, ")(pr _mismatch)(pr " MISMATCH, ")
(pr _astonly)(prn " ast-only")
(if (is _mismatch 0) (prn "EVAL FUZZ: OK") (prn "EVAL FUZZ: FAIL"))
(quit)
"""
    lines.append(tail)
    return "\n".join(lines) + "\n"


ANSI = re.compile(r"\x1b\[[0-9;]*m")


def run(binary, path, asan=False):
    env = dict(os.environ)
    if asan:
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        env["UBSAN_OPTIONS"] = "print_stacktrace=1"
    p = subprocess.run([binary, "--noload", path], capture_output=True,
                       text=True, timeout=300, env=env)
    out = ANSI.sub("", p.stdout + "\n" + p.stderr)
    return p.returncode, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=20260617)
    ap.add_argument("--count", type=int, default=1500)
    ap.add_argument("--depth", type=int, default=4)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-asan", action="store_true")
    args = ap.parse_args()

    r = Random(args.seed)
    tmp = tempfile.mkdtemp(prefix="evalfuzz.")
    prog = os.path.join(tmp, "fuzz.alc")
    with open(prog, "w") as f:
        f.write(gen_program(r, args.count, args.depth))

    print(f"[eval-fuzz] seed={args.seed} count={args.count} depth={args.depth} tmp={tmp}")
    builds = [("nojit", dict(jit=False)), ("jit", dict(jit=True))]
    if not args.no_asan:
        builds.append(("asan", dict(jit=True, asan=True)))

    ok = True
    for name, kw in builds:
        binary = os.path.join(tmp, f"alcove_{name}")
        print(f"[eval-fuzz] building {name} ...", flush=True)
        build(args.cc, binary, **kw)
        rc, out = run(binary, prog, asan=(name == "asan"))
        san = re.search(r"AddressSanitizer|runtime error:|LeakSanitizer", out)
        verdict = next((l for l in out.splitlines()
                        if l.startswith("EVAL FUZZ:") and ("OK" in l or "FAIL" in l)), "")
        summary = next((l for l in out.splitlines()
                        if l.startswith("EVAL FUZZ:") and "tested" in l), "")
        if san:
            ok = False
            print(f"  [{name}] SANITIZER REPORT:")
            print("\n".join("    " + l for l in out.splitlines()
                            if l.strip())[:4000])
        if rc != 0:
            ok = False
            print(f"  [{name}] NON-ZERO EXIT ({rc})")
        if "OK" not in verdict:
            ok = False
            print(f"  [{name}] {summary or 'no verdict'}")
            for l in out.splitlines():
                if l.startswith("MISMATCH"):
                    print("    " + l)
        else:
            tag = " (sanitizer-clean)" if name == "asan" else ""
            print(f"  [{name}] OK — {summary}{tag}")

    if not args.keep:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    print("==> EVAL FUZZ PASSED" if ok else "==> EVAL FUZZ FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
