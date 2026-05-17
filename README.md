# Alcove

A Lisp-1 dialect in one C file. Arc + Clojure flavour, bytecode VM,
native JIT (arm64 + amd64), persistent key-value store, RESP2 server
mode, libffi-based C interop, and a small tensor toolkit fast enough
to train an MLP digit classifier in pure Lisp.

🎮 **Play the in-browser Mario demo** — the game is one alcove file
running under WebAssembly:
**[aallamaa.github.io/alcove/mario.html](https://aallamaa.github.io/alcove/mario.html)**

> This README is the working overview as of 2026. The original 2014
> project intro — back when alcove was an alpha experiment about a
> code-as-data in-memory database — is preserved verbatim at the
> bottom of this file under [Original README](#original-readme).

---

## Taste

```lisp
;; Hello world
(prn "hello" "world")                ; → hello world

;; Closures
(def make-counter ()
  (let n 0 (fn () (do (= n (+ n 1)) n))))
(= c (make-counter))
(c) (c) (c)                          ; → 1, 2, 3

;; Macros (with quasiquote + unquote)
(defmacro when (cond body) `(if ,cond ,body nil))

;; Persistent variables (durable across restarts)
(= score 1280)
(persist 'score)
(savedb)                             ; later: ./alcove auto-loads it

;; Vectors with tensor-style bulk ops
(= a #[1 2 3 4]) (= b #[5 6 7 8])
(vec-dot a b)                        ; → 70.0
(vec-axpy! a 2.5 b)                  ; a += 2.5 * b, in place

;; FFI: any C library at runtime
(= sqrt (ffi-fn "libm.so.6" "sqrt" "double" "double"))
(sqrt 2.0)                           ; → 1.41421...
```

---

## Highlights

### 1. JIT performance

`make jit` builds with the native backend on by default. On the
standard microbenchmarks alcove beats CPython 3.13 by **17–700×** on
the shapes the JIT recognizes (`fib`, `fact`, `tak`, `ackermann`,
sieve-style tight loops, mlp tensor inner loops):

| benchmark      | alcove   | python3   | speedup            |
|----------------|---------:|----------:|-------------------:|
| `fib 33`       |   2.2 ms |  249.0 ms | **~700× alcove** ¹ |
| `nqueens 10`   |   2.5 ms |   72.9 ms | **~700× alcove** ¹ |
| `countdown`    |   4.6 ms |  765.2 ms |  **342× alcove**   |
| `ackermann 3 9`|  13.3 ms | 1093.4 ms |   99× alcove       |
| `forsum 1e7`   |   6.4 ms |  182.1 ms |   43× alcove       |
| `fact 19`      |   3.9 ms |   68.8 ms |   38× alcove       |
| `sieve-fast`   |   2.8 ms |   19.5 ms |   21× alcove       |
| `tak 24 16 8`  |   5.7 ms |   71.1 ms |   18× alcove       |
| `listsum`      |   3.6 ms |   31.2 ms |   17× alcove       |
| `mlp` (5 ep.)  | 670.0 ms | 2642.0 ms |  3.9× alcove ²     |

¹ Net of startup, fib & nqueens land in the noise floor — sub-100 µs
of arithmetic after the JIT shape catches. Treat as "the JIT erased
the work."
² mlp is heavy float math through `vec-dot` / `vec-axpy!` / etc. The
Python comparison is pure stdlib (no numpy) — matches the rest of
the suite. Numbers from `make benchmark-jit`.

Reproduce with `make benchmark` (speed build) or `make benchmark-jit`
(JIT + single-threaded refcount; the numbers above). The JIT works
by shape-matching the bytecode — see `alcove.c` around `try_jit_*`
and the design notes inline.

### 2. ML in Lisp — digit classifier in 130 lines

`examples/mlp/` ships a 64→32→10 MLP that trains from scratch on the
UCI optdigits dataset (3823 train / 1797 test, the same data behind
`sklearn.datasets.load_digits`). Pure alcove — no BLAS, no numpy
dependency. **95% test accuracy, 880 ms for 5 epochs** on a laptop.

```
$ cd examples/mlp
$ make           # downloads dataset, packs, trains
…
final test  acc: 1713/1797   (95.3%)
real    0m0.881s
```

The speedup comes from nine tensor-bulk-ops baked into the language —
`vec-dot`, `vec-axpy!`, `vec-relu!`, … — that operate on `vec`
storage in raw `double`s. See [`examples/mlp/README.md`](examples/mlp/README.md)
for the full story (and a side-by-side `make benchmark` against the
per-element interpreter baseline: 35× speedup).

### 3. Embedded Redis-compatible server

`./alcove -r 6379` boots a RESP2 server. `redis-cli` and any redis
client library talk to it unchanged. SET / GET / INCR / DEL / LPUSH /
RPUSH / LRANGE / HSET / HGETALL / TTL with lazy expiry / SAVE / BGSAVE
/ CONFIG / PING / SELECT all work.

**Throughput vs Redis 8.0.2**, randomised-key SET/GET workload over
a 100 k key keyspace, pipeline=64, best of 3 (`./benchmark/resp-bench-c`,
a self-contained C client checked into the repo):

| server                   | peak SET rps | peak GET rps | vs Redis  |
|--------------------------|-------------:|-------------:|----------:|
| redis 8.0.2 (1 thread)   |   2.19 M     |   2.75 M     | baseline  |
| alcove `--threads 1`     |   3.81 M     |   4.70 M     | **1.7×**  |
| alcove `--threads 4`     |  16.52 M     |  16.11 M     | **~7×**   |
| alcove `--threads 8`     |  22.06 M     |  23.46 M     | **~10×**  |

The multithreaded gap is the real story — alcove's per-shard reactor
architecture lets each thread own its own keyspace partition with no
lock contention, while Redis is single-threaded at the data plane.
Reproduce with `./benchmark/resp-bench.sh` (uses `redis-benchmark -r
KEYSPACE`) or `./benchmark/resp-bench-c` directly.

Exposes a Lisp hook for custom commands:

```lisp
(redis-defcmd "MIRROR" (fn (args) (reverse args)))
```

Command arguments arrive as binary-safe `EXP_BLOB` values, and Lisp
can write the RESP keyspace directly with existing containers:
`(redis-set "queue" (deque "a" "b"))` stores a Redis list, while
`(redis-set "h" (hash-map "field" "value"))` stores a Redis hash, and
`(redis-set "v" (vector 1 2 3))` keeps an Alcove `EXP_VECTOR` available
to Lisp through `(redis-val "v")`.

### 4. FFI to any C library

`(ffi-fn LIB FN-NAME RET-TYPE ARG-TYPES…)` returns a callable that
acts like any Lisp function. Up to 8 args; types `void`, `int`,
`long`, `double`, `string`, `ptr` (struct-by-value and varargs are
not supported — write a flat-int shim).

See `ffi-examples/` for libm, libc strings, a custom .so, plus the
Mario demo's SDL2 shim for a working cross-platform game.

### 5. Persistent variables

Mark any top-level binding as durable:

```lisp
(= board (vec 100 0))
(persist 'board)
(savedb "game.dump")     ; writes the binary dump
```

Restart `./alcove --db game.dump` and `board` is back. The savedb
format round-trips fixnum, float, char, string, symbol, pair, blob,
vec (including nested vec-of-vec with heterogeneous element types),
and lambda (re-compiled on load).

### 6. Three execution layers, picked automatically

1. **Tree-walking evaluator** — fallback for forms the compiler does
   not yet understand.
2. **Bytecode VM** — runs every compiled `def`/`fn` body.
3. **JIT** — arm64 / amd64 native code for ~12 hot shapes (leaf
   arithmetic, self-tail loops, fib / fact / tak / ackermann /
   nqueens-safe? / sieve, …). Falls back to the bytecode VM when a
   shape doesn't match.

Inspect with `(disasm f)` — it prints the bytecode plus the JIT
install status.

### 7. Alcove Script — a Python-like surface syntax

`make als` builds `./alcoves`: the full runtime plus a whitespace /
`:`-block reader. It is *not* a new language — the reader turns
indentation into ordinary Lisp forms *before* macro-expansion, so it
stays fully homoiconic. A line is a list; a trailing `:` opens a
block; `name(args)` is sugar for `name (args)`; `'x` is `(quote x)`.

**Idiomatic — recursion, loops, locals:**

```python
def fact (n):
  if (< n 2):
    1
    * n (fact (- n 1))

def sum-to (n):
  with (s 0):
    for i 1 n:
      = s (+ s i)
    s
```

**Macros, without the parens** (`macro`/`set` map to
`defmacro`/`=`; quasiquote is built with `list` + `'`):

```python
defmacro inc (v):
  list '= v (list '+ v 1)

def demo ():
  = x 10
  inc x
  inc x          # x is now 12
  prn x
```

**Everything is a block.** Because `:` just "appends the indented
forms to this list", any parenthesised argument can be unfolded into
a deeper block. All three of these read into the *identical* Lisp
form `(def fib (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))`:

```python
def fib (n):                 # inline
  if (< n 2):
    n
    + (fib (- n 1)) (fib (- n 2))

def fib (n):                 # ladder the + call
  if (< n 2):
    n
    +:
      fib (- n 1)
      fib (- n 2)

def fib (n):                 # ladder all the way down
  if (< n 2):
    n
    +:
      fib:
        - n 1
      fib:
        - n 2
```

**The REPL is block-aware** — continuation prompt, auto-indent after
`:`, blank line submits, live syntax highlighting + history. And
`(source f)` prints the definition *back as Alcove Script*, normalised
to the idiomatic form (homoiconicity, round-tripped):

```
In [4]: def fib(n):
   ...:   if (< n 2):
   ...:     n
   ...:     +:
   ...:       fib:
   ...:         - n 1
   ...:       fib:
   ...:         - n 2
   ...:
Out[4]: #<procedure:fib>
In [5]: fib 10
Out[5]: 55
In [6]: source fib
def fib (n):
  if (< n 2):
    n
    + (fib (- n 1)) (fib (- n 2))
```

Alcove Script is accepted at the prompt, in `.als` files, piped
stdin, and `-e`; `./alcove` itself is unchanged. `als.py` (forward)
and `alc2als.py` (`.alc` → `.als`, with builder laddering) are
offline tools. See [`examples/alcove-script/`](examples/alcove-script/)
and [`alcove-script-spec.md`](alcove-script-spec.md).

---

## Examples

| path | what |
|---|---|
| [`examples/mario/`](examples/mario/) | Side-scrolling platformer with SDL2 (native) and Canvas/WASM (web). See the [live demo](https://aallamaa.github.io/alcove/mario.html). |
| [`examples/mlp/`](examples/mlp/) | MLP digit classifier on UCI optdigits — full pipeline with `make data && make train`. |
| [`examples/arkanoid.alc`](examples/arkanoid.alc) | Auto-playing arkanoid on the terminal — mutable-string framebuffer, ANSI rendering. |
| [`ffi-examples/`](ffi-examples/) | libm, libc strings, sleeping via usleep, a custom .so for everything FFI can call. |
| [`examples/alcove-script/`](examples/alcove-script/) | Alcove Script (`.als`) — Python-like indentation syntax over the same Lisp forms; `make als` → `./alcoves`. |

---

## Build

Requires `cc`, `make`. Optional: `libreadline` (REPL line-editing,
history, paren-match, color), `libffi` (the `(ffi-fn …)` builtin).
`make deps` prints what was auto-detected.

```sh
make              # → ./alcove, JIT enabled (default goal)
make nojit        # JIT off; bytecode only
make jit-mono     # JIT + single-threaded refcounts (fastest)
make als          # → ./alcoves (Alcove Script front end)
make parser       # debug build with -g3
make test         # run test.alc (currently 400+ asserts) + ffi-examples
make benchmark    # alcove vs python3 microbenchmarks (incl. mlp)
make web          # → web/alcove-core.{js,wasm} via Emscripten
```

The default `make` writes `./alcove`. Use it like:

```sh
./alcove                          # REPL (rlwrap recommended)
./alcove file.alc                 # run script and exit
./alcove --noload                 # skip auto-load of ./db.dump
./alcove --db saved.dump file.alc # load vars from saved.dump first
./alcove -e '(+ 1 2)'             # evaluate one expression
./alcove -r 6379                  # RESP2 server mode
```

---

## Documentation

- **Language guide**: [`docs/alcove-language.md`](docs/alcove-language.md)
  — practical reference: literals, special forms, builtins, FFI,
  macros, persistence, closures, footguns. Generated from `alcove.c`
  + tested against `test.alc`.
- **Multithreading design**: [`docs/multithreading.md`](docs/multithreading.md)
  — how the RESP server's sharded reactors work, MPSC inboxes,
  refcount duality.
- **Feature proposals**: [`docs/specs/proposals.md`](docs/specs/proposals.md)
  — open language ideas (string-buf, try/catch, unquote-splicing).
- **Alcove Script**: [`alcove-script-spec.md`](alcove-script-spec.md)
  — the indentation reader spec, plus
  [`examples/alcove-script/README.md`](examples/alcove-script/README.md)
  for the `./alcoves` REPL, `.als` files, and the offline tools.
- **Editor support**: [`editor/`](editor/README.md) — syntax
  highlighting for vim and emacs (drop-in files + install steps).

---

## License

AGPL-3.0. See [`agpl-3.0.txt`](agpl-3.0.txt).

---

## Original README

The text below is from the original 2014 commit. The project has
grown since (JIT, RESP server, FFI, WASM build, ML tensor ops, vec
persistence, …) but the original framing still says it best:

```
===================================================
ALCOVE A YET TO DEFINE NEW KIND OF DATABASE/LANGAGE
===================================================

(PS: The following text will become a blog post and leave the place
to a real README when this project will be advanced enough, this is
really an alpha alpha status version of the project... )
```

### History

More than 20 years ago, when I was still a teenage coding on 8 bits
and 16 bits processors using assembly languages, I came up with the
idea of designing a new breed of processor. And this idea primarely
came while working with the blitter and copper co-processors of the
Amiga. The idea was the following, building processors that would be
made of stacked alveolus, each hexagonal prism constituting this
alveolus structure being able to exchange streams of data and code
with its neighboring cells. I called this technology, X.One
Technologies (from Hexagon and Axone, X.One => Multiple One).

Today, the world is being shaped by the Internet and this same
principle applies. Each entity is becoming part of the Internet, and
is exchanging with the rest of the Internet. Massive amounts of data
are being processed, and the more we move forward, the more we need
to be able to process quickly and efficiently those massive amounts.

Code and data are converging in a highly concurrent world, both at
the processor level (increasing number of cores) and at the network
level (distributed over network nodes).

### What will ALCOVE be?

When programming we always try to get as close as possible to the
data through various abstraction layers. More and more
database/storage technologies are greatly improving the developping
experience (schema-less databases like mongodb, in memory nosql
databases such as Redis).

But I feel we need code to be even closer to the data, so that's what
this project is about. Getting something similar to Redis (In-memory
key-value store) with some concurrency features (agent, generators,
..). Basicaly data can be code, and code can be data, so when
fetching data from a key for instance, you could potentially be
triggering a piece of code which would return data without even
knowing it.

The first part of this project is creating an interpretor for the
langage which will be the core of the database. The homoiconicity of
LISP made it the default choice, plus I never had the chance when i
was young, student, or even when working to code in LISP so LISP (and
also SmallTalk) is for me like the lost atlantide of computer
science.

The main inspirations for this LISP are ARC (from Paul Graham) and
Clojure. I will try to stick as much as possible to the ARC specs.

### Install and Run

```
make
./alcove
```

I would advice to use rlwrap

```
rlwrap ./alcove
```
