#!/usr/bin/env python3
"""test_repl_pty.py — end-to-end check of lib/repl.adr over a REAL pty.

The programmable-REPL hooks (*prompt-in*, *output-hook*, bind-key + repl-*)
fire only on the interactive readline path — gated on isatty(stdin) — so a
pty is required (piped stdin does NOT exercise them). This drives ./adder and
./alcove with a planted .init that does `require("repl"); repl/install-all()`,
then asserts on the (ANSI-stripped) terminal output and the transcript/errlog
files the output hook writes.

Self-gating: prints "REPL: OK" and exits 0 only if every check passes.
Run from the repo root:  ALCOVE_PATH=lib python3 tools/test_repl_pty.py
"""
import os
import pty
import re
import select
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, "lib")
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
passed = 0
failed = 0


def check(name, ok):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print(f"FAIL: {name}")


def drive(binary, init_text, commands, read_t=0.7):
    """Spawn `binary` under a pty in a temp cwd holding `init_text`; send each
    command; return (raw_output, tempdir)."""
    d = tempfile.mkdtemp(prefix="replpty.")
    # init filename matches the dialect's preferred init (.adr for adder).
    init_name = ".init.adr" if binary.endswith("adder") else ".init.alc"
    with open(os.path.join(d, init_name), "w") as f:
        f.write(init_text)
    pid, fd = pty.fork()
    if pid == 0:  # child
        os.chdir(d)
        env = dict(os.environ)
        env["ALCOVE_PATH"] = LIB
        env["TERM"] = "dumb"
        os.execve(binary, [binary, "--noload"], env)
    out = b""

    def rd(t):
        nonlocal out
        while True:
            r, _, _ = select.select([fd], [], [], t)
            if not r:
                break
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            out += chunk

    rd(1.0)  # startup + init
    for c in commands:
        os.write(fd, c.encode())
        rd(read_t)
    os.write(fd, b"quit\n")
    rd(0.4)
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass
    return out.decode("utf8", "replace"), d


def run_dialect(binary, dialect):
    d_tmp = tempfile.mkdtemp(prefix="repllog.")
    tlog = os.path.join(d_tmp, "t.log")
    elog = os.path.join(d_tmp, "e.log")
    nb = os.path.join(d_tmp, "nb.md")
    # require by bare name (resolved via ALCOVE_PATH=lib); install everything;
    # point transcript/errlog at temp files; then a quick form sequence.
    init = (
        'require("repl")\n'
        "repl/install-all()\n"
        'repl/transcript-to("%s")\n'
        'repl/errlog-to("%s")\n'
    ) % (tlog, elog)
    if binary.endswith("alcove"):
        init = (
            '(require "repl")\n'
            "(repl/install-all)\n"
            '(repl/transcript-to "%s")\n'
            '(repl/errlog-to "%s")\n'
        ) % (tlog, elog)
    cmds = [
        "(+ 1 2)\n",     # cell 1 -> 3
        "(/ 1 0)\n",     # cell 2 -> error (status flips red, errlog)
        "(repl/last 2)\n",        # cell 3 -> 3 (ring works across the error)
        "(repl/hud)\n",           # cell 4 -> "evals=.. errors=1 .."
        '(repl/export-md "%s")\n' % nb,  # cell 5 -> writes notebook
        "\x0f",          # C-o -> bound show-hud handler edits/prints (bind-key + repl-*)
    ]
    raw, _ = drive(binary, init, cmds)
    clean = ANSI.sub("", raw)

    check(f"{dialect}: custom prompt-in fires", f"{dialect} [1]>" in clean)
    check(f"{dialect}: result shown via prompt-out", "= 3" in clean)
    check(f"{dialect}: prompt advances", f"{dialect} [2]>" in clean)
    check(f"{dialect}: error surfaced", "Illegal division by 0" in clean)
    # status flip: prompt-in renders red (91) once an error has happened
    check(f"{dialect}: status color flips to red", "\x1b[91m" in raw)
    check(f"{dialect}: hud counts the error", re.search(r"errors=1", clean) is not None)
    check(f"{dialect}: C-o bound handler ran (hud printed)",
          clean.count("evals=") >= 2)  # (repl/hud) + the C-o handler

    # output-hook side effects: transcript + errlog files
    tcontent = open(tlog).read() if os.path.exists(tlog) else ""
    econtent = open(elog).read() if os.path.exists(elog) else ""
    check(f"{dialect}: transcript records ok cell", "In[1]:" in tcontent and "Out[1]: 3" in tcontent)
    check(f"{dialect}: transcript records error cell",
          "In[2]:" in tcontent and "Error: Illegal division by 0" in tcontent)
    check(f"{dialect}: errlog records only the error", "In[2]:" in econtent and "In[1]:" not in econtent)
    # notebook export, including the error cell, never re-raises
    nbc = open(nb).read() if os.path.exists(nb) else ""
    check(f"{dialect}: notebook exported all cells",
          "In[1]: (+ 1 2)" in nbc and "Error: Illegal division by 0" in nbc)


def run_graceful(binary, dialect):
    """A nil / erroring hook must never brick the REPL."""
    init = (
        'require("repl")\n'
        "setf *output-hook* nil\n"
        'setf *input-hook* (fn (s) (car 5))\n'  # deliberately-erroring input hook
    )
    if binary.endswith("alcove"):
        init = (
            '(require "repl")\n'
            "(setf *output-hook* nil)\n"
            "(setf *input-hook* (fn (s) (car 5)))\n"
        )
    raw, _ = drive(binary, init, ["(+ 40 2)\n"])
    clean = ANSI.sub("", raw)
    check(f"{dialect}: survives nil out-hook + erroring in-hook", "42" in clean)


def main():
    for binary, dialect in ((os.path.join(ROOT, "adder"), "adder"),
                            (os.path.join(ROOT, "alcove"), "alcove")):
        if not os.path.exists(binary):
            print(f"FAIL: {binary} not built")
            global failed
            failed += 1
            continue
        run_dialect(binary, dialect)
        run_graceful(binary, dialect)
    print(f"checks: {passed} passed, {failed} failed")
    if failed == 0:
        print("REPL: OK")
        sys.exit(0)
    sys.exit(1)


if __name__ == "__main__":
    main()
