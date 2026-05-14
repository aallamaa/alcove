#!/usr/bin/env python3
"""
Randomised-key SET/GET benchmark against any RESP server (Redis, alcove -r).

Unlike `redis-benchmark` (which by default hammers a single hot key), this
script pre-generates a large keyspace, shuffles it, then drives SET / GET
across the shuffled order via N parallel pipelined connections. Output
mirrors the redis-benchmark table format.

Usage:
  ./benchmark/resp-bench.py --port 6379 --clients 50 --pipeline 64 \
                            --keyspace 100000 --value-size 16 --ops 100000

Each (clients × ops) run is timed; reported rps is ops / wall-time across
all clients.
"""
import argparse
import os
import random
import socket
import string
import sys
import threading
import time


def make_keys(n: int, width: int = 16) -> list[bytes]:
    """N distinct keys of fixed length, in a deterministic order. Caller
    shuffles for access randomness."""
    return [f"key:{i:0{width-4}d}".encode() for i in range(n)]


def make_value(size: int) -> bytes:
    """A fixed-content value of `size` bytes. Content doesn't matter for
    GET/SET cost; size does."""
    return (b"x" * size)


def encode_set(key: bytes, val: bytes) -> bytes:
    return (b"*3\r\n$3\r\nSET\r\n"
            b"$" + str(len(key)).encode() + b"\r\n" + key + b"\r\n"
            b"$" + str(len(val)).encode() + b"\r\n" + val + b"\r\n")


def encode_get(key: bytes) -> bytes:
    return (b"*2\r\n$3\r\nGET\r\n"
            b"$" + str(len(key)).encode() + b"\r\n" + key + b"\r\n")


class RespReader:
    """Minimal RESP parser — just enough to consume +OK / $N<bytes> / $-1
    replies in a pipelined stream. State machine over a growing buffer."""
    def __init__(self):
        self.buf = bytearray()
        self.pos = 0

    def feed(self, data: bytes):
        self.buf.extend(data)

    def _readline(self):
        """Returns (line, new_pos) or (None, pos) if no full line yet."""
        nl = self.buf.find(b"\r\n", self.pos)
        if nl == -1:
            return None, self.pos
        return bytes(self.buf[self.pos:nl]), nl + 2

    def try_pop(self) -> bool:
        """Try to consume one reply. Returns True if popped, False if
        need more bytes. Discards the parsed value — we don't care."""
        line, after = self._readline()
        if line is None:
            return False
        if not line:
            return False
        t = line[0:1]
        if t in (b"+", b"-", b":"):
            self.pos = after
            return True
        if t == b"$":
            length = int(line[1:])
            if length < 0:
                # nil bulk string (e.g., GET miss)
                self.pos = after
                return True
            # Need length + 2 more bytes
            end = after + length + 2
            if end > len(self.buf):
                return False
            self.pos = end
            return True
        # arrays etc. — not used by SET/GET, but punt
        raise ValueError(f"unsupported RESP type: {t!r}")

    def compact(self):
        """Drop already-consumed bytes."""
        if self.pos:
            del self.buf[:self.pos]
            self.pos = 0


def run_client(host: str, port: int, ops: list[bytes], pipeline: int,
               result_slot: list, idx: int):
    """Send `ops` over one connection in batches of `pipeline`. Wait for
    each batch's replies before sending the next. Records elapsed seconds
    in result_slot[idx]."""
    sock = socket.create_connection((host, port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    reader = RespReader()
    n = len(ops)
    start = time.perf_counter()
    i = 0
    while i < n:
        batch_size = min(pipeline, n - i)
        batch = b"".join(ops[i:i + batch_size])
        sock.sendall(batch)
        # Read until we've consumed `batch_size` replies
        consumed = 0
        while consumed < batch_size:
            chunk = sock.recv(65536)
            if not chunk:
                raise IOError("connection closed mid-batch")
            reader.feed(chunk)
            while consumed < batch_size and reader.try_pop():
                consumed += 1
            reader.compact()
        i += batch_size
    elapsed = time.perf_counter() - start
    sock.close()
    result_slot[idx] = elapsed


def time_phase(label: str, host: str, port: int, clients: int,
               pipeline: int, all_ops: list[bytes]) -> tuple[float, int]:
    """Split `all_ops` evenly across `clients` workers, time wall-clock.
    Returns (rps, total_ops)."""
    n = len(all_ops)
    # Slice ops per client. Workers don't share state — they each get a
    # disjoint subset of the shuffled stream.
    per = n // clients
    slices = [all_ops[i * per:(i + 1) * per] for i in range(clients)]
    # Trailing remainder appended to the last worker.
    if per * clients < n:
        slices[-1].extend(all_ops[per * clients:])

    results = [0.0] * clients
    threads = []
    start = time.perf_counter()
    for i, sl in enumerate(slices):
        t = threading.Thread(target=run_client,
                             args=(host, port, sl, pipeline, results, i))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    wall = time.perf_counter() - start
    rps = n / wall if wall > 0 else 0
    return rps, n, wall


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6379)
    ap.add_argument("--clients", type=int, default=50,
                    help="parallel connections")
    ap.add_argument("--pipeline", type=int, default=64,
                    help="commands per pipeline batch per connection")
    ap.add_argument("--keyspace", type=int, default=100_000,
                    help="number of distinct keys")
    ap.add_argument("--value-size", type=int, default=16,
                    help="bytes per value")
    ap.add_argument("--ops", type=int, default=100_000,
                    help="number of SETs (and number of GETs) to issue")
    ap.add_argument("--seed", type=int, default=0,
                    help="random seed for reproducibility")
    ap.add_argument("--quiet", action="store_true",
                    help="machine-readable: 'SET_rps GET_rps SET_p50_ms GET_p50_ms'")
    args = ap.parse_args()

    random.seed(args.seed)
    keys = make_keys(args.keyspace)
    val = make_value(args.value_size)

    # Build the SET stream: pick `ops` random keys with replacement, shuffle.
    set_keys = [keys[random.randrange(args.keyspace)] for _ in range(args.ops)]
    random.shuffle(set_keys)
    set_ops = [encode_set(k, val) for k in set_keys]

    # Build the GET stream: independent random shuffle of the keyspace.
    get_keys = [keys[random.randrange(args.keyspace)] for _ in range(args.ops)]
    random.shuffle(get_keys)
    get_ops = [encode_get(k) for k in get_keys]

    if not args.quiet:
        print(f"resp-bench  host={args.host}  port={args.port}  "
              f"clients={args.clients}  pipeline={args.pipeline}  "
              f"keyspace={args.keyspace}  value={args.value_size}B  "
              f"ops={args.ops}")

    set_rps, _, set_wall = time_phase("SET", args.host, args.port,
                                       args.clients, args.pipeline, set_ops)
    get_rps, _, get_wall = time_phase("GET", args.host, args.port,
                                       args.clients, args.pipeline, get_ops)

    # Approximate p50 latency = batch round-trip / pipeline. Without a
    # per-op timing instrument this is a coarse proxy; the rps figure is
    # the load-bearing number.
    set_p50_ms = (set_wall / (args.ops / args.pipeline)) * 1000.0
    get_p50_ms = (get_wall / (args.ops / args.pipeline)) * 1000.0

    if args.quiet:
        print(f"{set_rps:.1f} {get_rps:.1f} {set_p50_ms:.3f} {get_p50_ms:.3f}")
    else:
        print(f"  SET   {set_rps:14.0f} rps   p50≈{set_p50_ms:7.3f} ms/batch")
        print(f"  GET   {get_rps:14.0f} rps   p50≈{get_p50_ms:7.3f} ms/batch")


if __name__ == "__main__":
    main()
