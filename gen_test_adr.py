#!/usr/bin/env python3
"""Generate test.adr from test.alc so the adder front end runs the SAME test
corpus as alcove instead of a hand-maintained subset that silently drifts.

    test.adr  =  alc2adr(test.alc)            # the shared engine suite
                 with test_adder_extra.adr     # adder-syntax-only tests
                 spliced in just before the "TEST RESULT" summary so the
                 extra assertions are counted.

adder is only a surface syntax over the same Lisp engine (adder.c #includes
alcove.c), and alc2adr.py round-trips test.alc behaviourally, so the
transpiled assertions hold identically. Run via `make gen-test-adr`.

Usage: python3 gen_test_adr.py [-o test.adr]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))

HEADER = (
    "# ====================================================================\n"
    "# GENERATED FILE - do not edit by hand.\n"
    "#   regenerate with:  make gen-test-adr   (or python3 gen_test_adr.py)\n"
    "# Source of truth:\n"
    "#   test.alc             - shared engine tests (transpiled via alc2adr.py)\n"
    "#   test_adder_extra.adr - adder-syntax-only tests (spliced before the\n"
    "#                          TEST RESULT summary)\n"
    "# Editing this file directly will be overwritten on the next regen.\n"
    "# ====================================================================\n\n"
)


def main():
    out = "test.adr"
    if len(sys.argv) >= 3 and sys.argv[1] == "-o":
        out = sys.argv[2]
    out = os.path.join(ROOT, out)

    transpiled = subprocess.run(
        [sys.executable, os.path.join(ROOT, "alc2adr.py"),
         os.path.join(ROOT, "test.alc")],
        capture_output=True, text=True, check=True).stdout
    with open(os.path.join(ROOT, "test_adder_extra.adr")) as f:
        extra = f.read()

    lines = transpiled.splitlines(keepends=True)
    summary = [i for i, l in enumerate(lines) if "TEST RESULT" in l]
    if len(summary) != 1:
        sys.exit(f"gen_test_adr: expected exactly one 'TEST RESULT' line in the "
                 f"transpiled output, found {len(summary)}")
    idx = summary[0]

    merged = (HEADER
              + "".join(lines[:idx])
              + "\n" + extra + "\n"
              + "".join(lines[idx:]))
    with open(out, "w") as f:
        f.write(merged)
    print(f"gen_test_adr: wrote {out} ({merged.count(chr(10))} lines)")


if __name__ == "__main__":
    main()
