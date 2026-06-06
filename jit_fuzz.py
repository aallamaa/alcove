#!/usr/bin/env python3
"""jit_fuzz.py — differential fuzzer for the alcove JIT.

Generates a large batch of randomized function bodies whose shapes the JIT is
*supposed* to recognize (counter loops, leaf arithmetic, float accumulators),
then runs each one under both a JIT build and a non-JIT build and checks two
things:

  1. CORRECTNESS  — every result is byte-identical between the JIT and the
     bytecode VM (compared via `(msgpack-encode v)`, which is type- and
     bit-exact: an int and a float that print the same still differ here).
     A mismatch means the emitted machine code diverges from the VM — a JIT
     miscompile. This is the hard failure.

  2. COVERAGE     — for each generated shape we record `(jit? f)`. Shapes that
     are meant to JIT but don't are reported, so coverage gaps (like the
     count-up-to-a-wide-limit loop, or the for_loop_inc/forsum regression)
     surface as data instead of a silent benchmark slowdown.

This explores far more of the input space than the fixed benchmark set: all
four comparison directions, varied steps/limits/seeds (including negative,
zero, wide, and 0-iteration cases), and add/sub/mul float accumulators.

Usage:
    python3 jit_fuzz.py [--seed N] [--count N] [--keep]
    make jit-fuzz

Exit code 0 iff every result matched AND every expected-to-JIT shape JIT'd.
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))


def build(cc, src, out, jit):
    flags = ["-O2", "-w"]
    if jit:
        flags.append("-DALCOVE_JIT=1")
    cmd = [cc, *flags, "-o", out, src, "-lm"]
    subprocess.run(cmd, check=True, cwd=ROOT)


# ---- shape generators -------------------------------------------------------
# Each returns (name, definition_str, [(args_str, expect_jit_bool)], category).
# Generators only emit terminating calls (the counter always moves toward the
# limit, iteration counts are capped).

CMPS = ["<", "<=", ">", ">="]
FLOAT_CONSTS = ["0.5", "0.25", "2.0", "1.0", "0.1", "3.5", "1.5"]


def gen_counter_loop(rng, idx):
    """(def f (n) (if (CMP n LIM) (f (OP n STEP)) n)) — self-tail counter."""
    name = f"cl{idx}"
    cmp = rng.choice(CMPS)
    countup = cmp in ("<", "<=")
    step = rng.choice([1, 2, 3, 5])
    # limit: mix small (fused SLOT_*_FIX), mid (fused + materialized-reg cmp),
    # and >i16 wide (the generic-triple try_jit_wide_counter_loop shape).
    limit = rng.choice([10, 100, 511, 512, 1000, 9999, 20000, 40000, 5000000])
    if not countup:
        limit = rng.choice([-1, 0, 1, 50, 500, 1000, 40000, 3000000])
    op = "+" if countup else "-"
    body = f"(if ({cmp} n {limit}) ({name} ({op} n {step})) n)"
    defn = f"(def {name} (n) {body})"
    # args: a normal run + a 0-iteration run (start already past the limit).
    if countup:
        normal = limit - rng.randint(1, 30) * step  # ends near limit
        zero = limit + rng.randint(1, 50)
    else:
        normal = limit + rng.randint(1, 30) * step
        zero = limit - rng.randint(1, 50)
    args = [(str(normal), True), (str(zero), True)]
    # int16 limits fuse to SLOT_*_FIX (a JIT shape); only |limit|>32767 would
    # fall to the generic triple. All our limits fit, so expect JIT.
    return name, defn, args, "counter"


def gen_leaf(rng, idx):
    """(def f (s) (OP s K)) — leaf arithmetic, and (def f () K)."""
    name = f"lf{idx}"
    kind = rng.choice(["const", "add", "sub", "mul"])
    if kind == "const":
        k = rng.randint(-1000, 1000)
        defn = f"(def {name} () {k})"
        return name, defn, [("", True)], "leaf"
    op = {"add": "+", "sub": "-", "mul": "*"}[kind]
    k = rng.randint(1, 200)
    defn = f"(def {name} (s) ({op} s {k}))"
    args = [(str(rng.randint(-10000, 10000)), True) for _ in range(2)]
    return name, defn, args, "leaf"


def gen_float_acc(rng, idx):
    """(def f (n acc) (if (< n LIM) (f (+ n 1) (FOP acc FC)) acc)) — float acc.

    LIM must exceed int16 so the counter compare is the generic triple (the
    float_acc shape); start near the limit so only a few float ops run (mul
    would otherwise overflow to inf — still equivalent, but we want exact
    finite checks too)."""
    name = f"fa{idx}"
    fop = rng.choice(["+", "-", "*"])
    fc = rng.choice(FLOAT_CONSTS)
    limit = rng.choice([40000, 50000, 100000, 200000])
    iters = rng.randint(0, 6)            # incl. 0-iteration passthrough
    start = limit - iters
    defn = (f"(def {name} (n acc) "
            f"(if (< n {limit}) ({name} (+ n 1) ({fop} acc {fc})) acc))")
    seeds = ["0.0", "1.0", "-3.5", "0", "7", "-2"]  # float + int seeds
    args = [(f"{start} {rng.choice(seeds)}", True) for _ in range(3)]
    return name, defn, args, "float-acc"


def gen_eq_countdown(rng, idx):
    """(def f (n) (if (is n K) n (f (OP n STEP)))) — equality-base tail loop.

    The swapped-polarity twin of gen_counter_loop: the `is`-base if puts the
    BASE arm (return the slot) in THEN and the recurse arm in ELSE, so the
    bytecode is mirrored vs the gt/lt counter loop. JITs via the
    simple_tail_loop_eq matcher. Semantics: while (n != K) { n OP= STEP; }
    return n — so STEP must drive n toward K and K must be reachable from the
    start by an exact multiple of STEP (else it would run forever)."""
    name = f"eq{idx}"
    target = rng.choice([0, 1, -1, 5, 50, 100, -50])
    step = rng.choice([1, 2, 3, 5])
    countdown = rng.choice([True, False])
    op = "-" if countdown else "+"
    body = f"(if (is n {target}) n ({name} ({op} n {step})))"
    defn = f"(def {name} (n) {body})"
    # Pick starts that are exact multiples of STEP away from the target on the
    # correct side, plus a 0-iteration case (start == target).
    if countdown:
        normal = target + rng.randint(1, 30) * step
    else:
        normal = target - rng.randint(1, 30) * step
    args = [(str(normal), True), (str(target), True)]
    return name, defn, args, "eq-countdown"


def gen_float_series(rng, idx):
    """(def f (k acc) (if (< k LIM)
                          (f (+ k STEP) (+ acc (- (/ N1 k) (/ N2 (+ k OFF2)))))
                          acc)) — telescoping reciprocal float series.

    The Leibniz/Nilakantha π shape (float_series_loop): two divisions, the second
    by a counter-derived divisor. k stays strictly positive across the run (start
    >= 1, positive step, positive OFF2) so no divisor ever hits 0 — the /0 deopt
    path is covered separately by a unit test, since an error result can't be
    msgpack-compared. Start near the limit so only a few iterations run."""
    name = f"fs{idx}"
    n1 = rng.choice(FLOAT_CONSTS)
    n2 = rng.choice(FLOAT_CONSTS)
    step = rng.choice([2, 4, 6])
    off2 = rng.choice([1, 2, 3])
    limit = rng.choice([40000, 50000, 100000, 200000])
    iters = rng.randint(0, 6)            # incl. 0-iteration passthrough
    start = limit - iters * step
    if start < 1:
        start = 1
    defn = (f"(def {name} (k acc) "
            f"(if (< k {limit}) "
            f"({name} (+ k {step}) (+ acc (- (/ {n1} k) (/ {n2} (+ k {off2}))))) "
            f"acc))")
    seeds = ["0.0", "1.0", "-3.5", "0", "7", "-2"]  # float + int seeds
    args = [(f"{start} {rng.choice(seeds)}", True) for _ in range(3)]
    return name, defn, args, "float-series"


def gen_numloop(rng, idx):
    """Random numeric self-tail loop for the general numeric-loop compiler:
    int counter + 1-3 float locals, each updated by a small random float
    expression (+ - *, and sometimes / by a nonzero const), with an int-counter
    exit. Every float local's update is wrapped so it contains a float const
    (→ the analyzer types the slot FLOAT and the strict-EXP_FLOAT entry guard
    matches the float seeds → the loop JITs). Start near the limit so only a few
    iterations run (keeps results finite and exact-comparable). This is the
    differential safety net for the numloop codegen — the ONLY thing that catches
    a wrong-but-finite miscompile."""
    name = f"nl{idx}"
    nf = rng.randint(1, 2)            # 1-2 float locals (+ counter): fits amd64 xmm budget
    fs = [f"a{j}" for j in range(nf)]
    limit = rng.choice([40000, 50000, 100000])
    iters = rng.randint(0, 5)
    start = limit - iters

    def fexpr(depth):
        if depth <= 0 or rng.random() < 0.5:
            return (rng.choice(fs) if rng.random() < 0.6
                    else rng.choice(FLOAT_CONSTS))
        op = rng.choice(["+", "-", "*", "*", "/"])
        a, b = fexpr(depth - 1), fexpr(depth - 1)
        if op == "/":
            b = rng.choice(["2.0", "0.5", "3.0", "1.5"])  # nonzero const divisor
        return f"({op} {a} {b})"

    # force a float const into each update so the slot types FLOAT; shallow so the
    # operand stack stays inside the amd64 register budget (else it bails to VM)
    newvals = [f"(+ {rng.choice(FLOAT_CONSTS)} {fexpr(1)})" for _ in range(nf)]
    ret = rng.choice(fs)
    defn = (f"(def {name} (n {' '.join(fs)}) "
            f"(if (< n {limit}) ({name} (+ n 1) {' '.join(newvals)}) {ret}))")
    seeds = ["0.5", "1.5", "-0.5", "2.0", "0.25", "-1.5"]
    args = [(f"{start} " + " ".join(rng.choice(seeds) for _ in fs), True)
            for _ in range(3)]
    return name, defn, args, "numloop"


GENERATORS = [gen_counter_loop, gen_leaf, gen_float_acc, gen_eq_countdown,
              gen_float_series, gen_numloop]


def generate(rng, count):
    defs, probes, checks = [], [], []
    for i in range(count):
        g = rng.choice(GENERATORS)
        name, defn, args, cat = g(rng, i)
        defs.append(defn)
        probes.append((name, cat))
        for a, _exp in args:
            call = f"({name} {a})" if a else f"({name})"
            checks.append(call)
    lines = list(defs)
    # coverage probes: print "JIT <cat> <name>" / "VM  <cat> <name>"
    for name, cat in probes:
        lines.append(f'(prn (str "JITQ {cat} {name} " (jit? {name})))')
    # result checks
    for call in checks:
        lines.append(f"(prn (msgpack-encode {call}))")
    lines.append("(quit)")
    return "\n".join(lines) + "\n"


def run(binary, program):
    p = subprocess.run([binary, "--noload"], input=program,
                       capture_output=True, text=True, timeout=120)
    out = []
    for ln in p.stdout.splitlines():
        # strip ANSI + the "In [n]:"/"Out[n]:" REPL prefixes
        ln = ln.replace("\x1b", "")
        import re
        ln = re.sub(r"\x1b?\[[0-9;]*m", "", ln)
        ln = re.sub(r"^(In|Out)\s*\[\d+\]:", "", ln)
        out.append(ln)
    return "\n".join(out)


def extract(out):
    """Split a run's output into (jitq_map, result_list)."""
    jitq, results = {}, []
    for ln in out.splitlines():
        s = ln.strip()
        if s.startswith("JITQ "):
            parts = s.split(None, 3)
            # JITQ <cat> <name> <t|nil>
            if len(parts) >= 4:
                jitq[parts[2]] = (parts[1], parts[3].strip())
        elif "blob" in s and "|" in s:
            results.append(s)
    return jitq, results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--count", type=int, default=400)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    tmp = tempfile.mkdtemp(prefix="jitfuzz.")
    jit_bin = os.path.join(tmp, "alcove_jit")
    nojit_bin = os.path.join(tmp, "alcove_nojit")
    src = os.path.join(ROOT, "alcove.c")
    print(f"[jit-fuzz] seed={args.seed} count={args.count} tmp={tmp}")
    print("[jit-fuzz] building jit + nojit ...")
    build(args.cc, src, jit_bin, jit=True)
    build(args.cc, src, nojit_bin, jit=False)

    program = generate(rng, args.count)
    with open(os.path.join(tmp, "fuzz.alc"), "w") as f:
        f.write(program)

    jit_out = run(jit_bin, program)
    nojit_out = run(nojit_bin, program)
    jq, jr = extract(jit_out)
    nq, nr = extract(nojit_out)

    ok = True

    # 1) correctness: results must be byte-identical (jit == VM).
    if jr != nr:
        ok = False
        print(f"[FAIL] result mismatch: {len(jr)} jit vs {len(nr)} nojit lines")
        for i, (a, b) in enumerate(zip(jr, nr)):
            if a != b:
                print(f"  line {i}: jit={a!r}  nojit={b!r}")
                if i > 20:
                    break
    else:
        print(f"[OK]   {len(jr)} results byte-identical jit == VM")

    # 2) coverage: every probed shape should JIT in the jit build.
    by_cat = {}
    missed = []
    for name, (cat, status) in jq.items():
        d = by_cat.setdefault(cat, [0, 0])
        d[1] += 1
        if status == "t":
            d[0] += 1
        else:
            missed.append((cat, name))
    print("[jit-fuzz] coverage by category (jit'd / total):")
    for cat in sorted(by_cat):
        hit, tot = by_cat[cat]
        print(f"    {cat:12s} {hit}/{tot}")
    if missed:
        ok = False
        print(f"[FAIL] {len(missed)} shape(s) expected to JIT did not:")
        for cat, name in missed[:20]:
            print(f"    {cat} {name}")

    if not args.keep:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("==> JIT FUZZ PASSED" if ok else "==> JIT FUZZ FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
