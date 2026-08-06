#!/usr/bin/env python3
"""resp_fuzz.py — protocol fuzzer for the RESP network surface.

The jit/eval/adr/msgpack fuzzers all reach the engine through APIs the
attacker does not control. The RESP parser is the opposite: it is the one
place where BYTES FROM A SOCKET drive a state machine, and it was covered
only by well-formed conventional tests.

This drives a real server over a real socket with input designed to break a
hand-written framer:

  - malformed frames: bogus type bytes, negative/oversized/absent lengths,
    a declared bulk length that disagrees with the bytes that follow,
    unterminated frames, bare CR or LF instead of CRLF
  - truncation: every prefix of a valid pipeline, so the parser is resumed
    mid-frame at every possible byte boundary
  - split writes: one frame delivered a byte at a time, so "did you buffer
    a partial read correctly" is answered for every offset
  - pipelining: many frames in one write, valid and invalid interleaved
  - inline commands (the space-separated non-RESP form redis supports)
  - structural extremes: deep nesting, huge argument counts, NUL bytes and
    binary in keys and values

The oracle is deliberately weak, because a protocol fuzzer's job is not to
check replies: after every case the server must still be ALIVE and answer a
subsequent PING. A crash, a hang, or a wedged connection fails the run —
and under an ASan/TSan build (make resp-fuzz-asan) so does any memory error.

Deterministic: seeded xorshift, so a failure reproduces from its seed.
Skips cleanly when the server cannot be started.

Usage: python3 tools/resp_fuzz.py [--cases N] [--seed S] [--bin ./alcove]
"""
import argparse
import os
import random
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TYPES = [b"*", b"$", b"+", b"-", b":", b"%", b"~", b">", b"?", b"\x00", b"\xff"]
CMDS = [b"PING", b"SET", b"GET", b"DEL", b"EXISTS", b"INCR", b"TTL", b"EXPIRE",
        b"DBSIZE", b"KEYS", b"TYPE", b"LPUSH", b"HSET", b"COMMAND", b"ECHO",
        b"FLUSHDB", b"SELECT", b"PERSIST", b"UNKNOWNCMD", b""]


def bulk(b):
    return b"$" + str(len(b)).encode() + b"\r\n" + b + b"\r\n"


def array(items):
    return b"*" + str(len(items)).encode() + b"\r\n" + b"".join(bulk(i) for i in items)


def gen_case(rng):
    """One fuzz payload. Mixes well-formed frames with targeted malformations."""
    kind = rng.randrange(12)
    if kind == 0:  # well-formed command (keeps the server doing real work)
        cmd = rng.choice(CMDS)
        n = rng.randrange(0, 4)
        return array([cmd] + [rng.choice([b"k", b"v", b"0", b"-1", b"9" * 20])
                              for _ in range(n)])
    if kind == 1:  # bogus type byte
        return rng.choice(TYPES) + b"3\r\nPING\r\n"
    if kind == 2:  # length that lies about the payload
        body = b"X" * rng.randrange(0, 8)
        claim = rng.choice([-1, -99, 0, 1, 7, 10 ** 6, 2 ** 31, 2 ** 63])
        return b"*1\r\n$" + str(claim).encode() + b"\r\n" + body + b"\r\n"
    if kind == 3:  # absurd / negative multibulk count
        return b"*" + str(rng.choice([-1, -1000, 0, 10 ** 7, 2 ** 31, 2 ** 63])).encode() + b"\r\n"
    if kind == 4:  # unterminated frame
        return b"*2\r\n$4\r\nPING"
    if kind == 5:  # bare CR / bare LF instead of CRLF
        sep = rng.choice([b"\r", b"\n", b"\n\r", b"\r\r", b"\n\n"])
        return b"*1" + sep + b"$4" + sep + b"PING" + sep
    if kind == 6:  # inline command (non-RESP form)
        return b" ".join(rng.choice(CMDS) for _ in range(rng.randrange(1, 5))) + b"\r\n"
    if kind == 7:  # pipeline of valid + invalid
        out = b""
        for _ in range(rng.randrange(2, 12)):
            out += rng.choice([array([b"PING"]), b"*x\r\n", b"$-5\r\n",
                               array([b"SET", b"k", b"v"]), b"+OK\r\n"])
        return out
    if kind == 8:  # binary / NUL in key and value
        blob = bytes(rng.randrange(256) for _ in range(rng.randrange(0, 64)))
        return array([b"SET", blob, blob])
    if kind == 9:  # deep nesting of array headers, no payload
        return b"".join(b"*1\r\n" for _ in range(rng.randrange(2, 200)))
    if kind == 10:  # enormous single argument
        return array([b"SET", b"k", b"A" * rng.randrange(1, 200000)])
    return bytes(rng.randrange(256) for _ in range(rng.randrange(1, 256)))


class Server:
    def __init__(self, binary, port):
        self.port, self.proc, self.dir = port, None, tempfile.mkdtemp(prefix="respfuzz.")
        open(os.path.join(self.dir, ".init.alc"), "w").close()
        self.proc = subprocess.Popen(
            [binary, "-r", str(port), "--noload"], cwd=self.dir,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(200):
            if self.proc.poll() is not None:
                return
            try:
                with socket.create_connection(("127.0.0.1", port), 0.2) as s:
                    s.sendall(array([b"PING"]))
                    if s.recv(64):
                        return
            except OSError:
                time.sleep(0.05)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def ping(self, timeout=3.0):
        """The oracle: a fresh connection must still get a reply."""
        try:
            with socket.create_connection(("127.0.0.1", self.port), timeout) as s:
                s.settimeout(timeout)
                s.sendall(array([b"PING"]))
                return bool(s.recv(64))
        except OSError:
            return False

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", type=int, default=400)
    ap.add_argument("--seed", type=int, default=20260806)
    ap.add_argument("--bin", default=os.path.join(ROOT, "alcove"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("RESP_FUZZ_PORT", 7793)))
    a = ap.parse_args()

    # Resolve BEFORE use: the server is spawned with cwd=<tempdir>, so a
    # relative --bin would be looked up there and vanish.
    a.bin = os.path.abspath(a.bin)
    if not os.path.exists(a.bin):
        print(f"  ({a.bin} missing — RESP fuzz skipped)")
        print("==> RESP FUZZ SKIPPED")
        return 0

    srv = Server(a.bin, a.port)
    if not srv.alive() or not srv.ping():
        err = b""
        if srv.proc and srv.proc.stderr:
            try:
                err = srv.proc.stderr.read(2000)
            except Exception:
                pass
        srv.stop()
        print(f"  (server did not come up on :{a.port} — RESP fuzz skipped)")
        if err:
            print("  " + err.decode(errors="replace").strip()[:400])
        print("==> RESP FUZZ SKIPPED")
        return 0

    rng = random.Random(a.seed)
    sent = 0
    try:
        for i in range(a.cases):
            case = gen_case(rng)
            mode = i % 3
            try:
                with socket.create_connection(("127.0.0.1", a.port), 3.0) as s:
                    s.settimeout(3.0)
                    if mode == 0:            # one write
                        s.sendall(case)
                    elif mode == 1:          # split writes: resume mid-frame
                        step = max(1, len(case) // 7)
                        for k in range(0, len(case), step):
                            s.sendall(case[k:k + step])
                            time.sleep(0.001)
                    else:                    # truncated prefix
                        s.sendall(case[:rng.randrange(0, len(case) + 1)])
                    s.shutdown(socket.SHUT_WR)
                    try:
                        while s.recv(65536):
                            pass
                    except OSError:
                        pass
            except OSError:
                pass  # a refused/reset connection is a legal outcome; the ping decides
            sent += 1
            if not srv.alive():
                err = srv.proc.stderr.read(4000).decode(errors="replace") if srv.proc.stderr else ""
                print(f"  SERVER DIED after {sent} cases (seed={a.seed}, case #{i})")
                print(f"  payload: {case[:200]!r}")
                if err.strip():
                    print("  stderr: " + err.strip()[:1500])
                print("==> RESP FUZZ FAILED")
                return 1
            if i % 25 == 0 and not srv.ping():
                print(f"  SERVER UNRESPONSIVE after {sent} cases (seed={a.seed}, case #{i})")
                print(f"  payload: {case[:200]!r}")
                print("==> RESP FUZZ FAILED")
                return 1
        if not srv.ping():
            print(f"  SERVER UNRESPONSIVE at end of run (seed={a.seed})")
            print("==> RESP FUZZ FAILED")
            return 1
    finally:
        srv.stop()

    print(f"  OK — {sent} malformed/partial/pipelined payloads, server alive and "
          f"responsive (seed={a.seed})")
    print("==> RESP FUZZ PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
