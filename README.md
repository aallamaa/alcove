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
standard microbenchmarks alcove beats CPython 3.13 by **30–400×** on
the shapes the JIT recognizes (`fib`, `fact`, `tak`, `ackermann`,
sieve-style tight loops):

| benchmark    | alcove | python3 | speedup |
|--------------|-------:|--------:|--------:|
| `fib 33`     | 10 ms  | 260 ms  | **242× alcove** |
| `countdown`  | 13 ms  |  819 ms | **400× alcove** |
| `ackermann`  | 23 ms  | 1157 ms |  95× alcove |
| `tak`        | 13 ms  |   78 ms |  30× alcove |
| `fact`       | 12 ms  |   80 ms |  62× alcove |
| `forsum`     | 15 ms  |  198 ms |  45× alcove |
| `nqueens`    | 105 ms |   79 ms |   1.5× python (shape not yet JIT'd) |

Reproduce with `make benchmark-jit`. The JIT works by shape-matching
the bytecode — see `alcove.c` around `try_jit_*` and the design notes
inline.

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

`./alcove -r 6379` boots a RESP2 server that answers `redis-cli`
clients at 80–99% of `redis-server` throughput with ~5× lower memory
on a fresh dataset. SET / GET / INCR / DEL / LPUSH / RPUSH / LRANGE /
HSET / HGETALL / TTL with lazy expiry / SAVE / BGSAVE / CONFIG /
PING / SELECT all work.

Exposes a Lisp hook for custom commands:

```lisp
(redis-defcmd "MIRROR" (fn (args) (reverse args)))
```

Then from any redis-cli: `> MIRROR 1 2 3` → `(3 2 1)`.

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

---

## Examples

| path | what |
|---|---|
| [`examples/mario/`](examples/mario/) | Side-scrolling platformer with SDL2 (native) and Canvas/WASM (web). See the [live demo](https://aallamaa.github.io/alcove/mario.html). |
| [`examples/mlp/`](examples/mlp/) | MLP digit classifier on UCI optdigits — full pipeline with `make data && make train`. |
| [`examples/arkanoid.alc`](examples/arkanoid.alc) | Auto-playing arkanoid on the terminal — mutable-string framebuffer, ANSI rendering. |
| [`ffi-examples/`](ffi-examples/) | libm, libc strings, sleeping via usleep, a custom .so for everything FFI can call. |

---

## Build

Requires `cc`, `make`. Optional: `libreadline` (REPL line-editing,
history, paren-match, color), `libffi` (the `(ffi-fn …)` builtin).
`make deps` prints what was auto-detected.

```sh
make              # → ./alcove, JIT enabled (default goal)
make nojit        # JIT off; bytecode only
make jit-mono     # JIT + single-threaded refcounts (fastest)
make parser       # debug build with -g3
make test         # run test.alc (currently 366 asserts) + ffi-examples
make benchmark    # alcove vs python3 microbenchmarks
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
