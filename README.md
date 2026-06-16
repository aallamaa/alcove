# Alcove & Adder

[![CI](https://github.com/aallamaa/alcove/actions/workflows/ci.yml/badge.svg)](https://github.com/aallamaa/alcove/actions/workflows/ci.yml)

**One tiny runtime, two languages.** **Alcove** is a Lisp-1 dialect
(Arc + Clojure flavour); **Adder** is the *same language* wearing
Python's indentation syntax — the most readable compromise between
Lisp's homoiconic core and Python's surface. Both share everything:
one C file, a bytecode VM, a native JIT (arm64 + amd64), tensors fast
enough to train an MNIST classifier, JSON/regex/shell scripting,
persistence, FFI, and a WebAssembly build.

```python
#!/usr/bin/env adder                 # ← Adder: Python-shaped
def fib(n):
  if (n < 2):
    n
  else:
    (fib (n - 1)) + (fib (n - 2))    # infix: (a op b) == (op a b)

prn "fib 30 = " (fib 30)
```

```lisp
;; ← Alcove: the same program, as Lisp
(def fib (n)
  (if (< n 2) n
    (+ (fib (- n 1)) (fib (- n 2)))))

(prn "fib 30 = " (fib 30))
```

Adder is not a transpile-to target or a skin: the indentation reader
produces ordinary Lisp forms *before* macro-expansion, so it is fully
homoiconic — macros, `(source f)` round-tripping, the REPL, scripts,
and the browser runtime all speak both. `make` builds `alcove`,
`make als` builds `adder`; every release ships both binaries.

🎮 **Play the in-browser Mario demo** — the game is one alcove file
running under WebAssembly:
**[aallamaa.github.io/alcove/mario.html](https://aallamaa.github.io/alcove/mario.html)**

✎ **Draw a digit** — a neural net trained *by* the language classifies
your handwriting live:
**[aallamaa.github.io/alcove/digits.html](https://aallamaa.github.io/alcove/digits.html)**

📚 **Learn both syntaxes in the browser** — the playground's guided
tour has an Alcove/Adder switch on every lesson:
**[aallamaa.github.io/alcove/playground.html](https://aallamaa.github.io/alcove/playground.html)**

🧭 **Lisp comparison table** — Common Lisp, Racket, Clojure, Emacs Lisp,
Alcove, and Adder side by side:
**[aallamaa.github.io/alcove/docs/lisp-hyperpolyglot-alcove.html](https://aallamaa.github.io/alcove/docs/lisp-hyperpolyglot-alcove.html)**

> This README is the working overview as of 2026. The original 2014
> project intro — back when alcove was an alpha experiment about a
> code-as-data in-memory database — is preserved verbatim at the
> bottom of this file under [Original README](#original-readme).

---

## Taste

The same features, both surfaces — pick whichever reads better to you:

```lisp
;; Alcove ----------------------------------------------------------
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
(savedb)                             ; later: alcove auto-loads it

;; Vectors with tensor-style bulk ops
(= a #[1 2 3 4]) (= b #[5 6 7 8])
(vec-dot a b)                        ; → 70.0
(vec-axpy! a 2.5 b)                  ; a += 2.5 * b, in place

;; FFI: any C library at runtime
(= sqrt (ffi-fn "libm.so.6" "sqrt" "double" "double"))
(sqrt 2.0)                           ; → 1.41421...
```

```python
# Adder ------------------------------------------------------------
# Closures
def make-counter():
  with (n 0):
    fn():
      n = (+ n 1)                    # `=` returns the new value

c = make-counter()
prn (c) (c) (c)                      # → 1 2 3

# Infix: (a op b) means (op a b) when the head is a value, not a function
def fib(n):
  if (n < 2):
    n
  else:
    (fib (n - 1)) + (fib (n - 2))
prn (map fib (range 0 10))           # → (0 1 1 2 3 5 8 13 21 34)

# Scripting floor: JSON, regex, shell — same builtins, no parens tax
= cfg (json-decode (read-string "config.json"))
prn (get cfg "port")
each m (re-find-all "[0-9]+" (get (shell "uname -r") "out")):
  prn "version part: " m
```

---

## Highlights

### 1. JIT performance

`make benchmark` runs each program three ways — **alcove, the same
program in C (compiled with `gcc -O2`), and CPython 3.13** — so you can
see how alcove stacks up against both the fastest practical baseline (C)
and the most common scripting language (Python).

Two honest takeaways:

1. **alcove is *not* faster than C when both do the same work — C is the
   fastest.** On the benchmarks where alcove and C run the identical
   algorithm, alcove is about **1.2–3.5× slower than C**. For a dynamic
   Lisp that's an excellent result — and on the float-heavy `pi` / `logistic`
   loops, where alcove's numeric tail-loop JIT compiles the loop to native,
   it lands within **~1.1–1.3× of C** (the ◆ rows).
2. **alcove is roughly 10–300× faster than Python.**

Full results (one machine, x86-64; each cell is best-of-15 wall-clock
including the time to launch the program):

| benchmark      |   alcove |  C (gcc -O2) |   python3 | alcove vs C     | alcove vs python |
|----------------|---------:|-------------:|----------:|-----------------|-----------------:|
| `fib`          |  0.92 ms |     4.48 ms  |  252.2 ms | 4.9× faster *   |      274× faster |
| `nqueens`      |  1.05 ms |     2.39 ms  |   70.0 ms | 2.3× faster *   |       67× faster |
| `nqueens-vec`  |  1.05 ms |     2.77 ms  |  110.4 ms | 2.6× faster *   |      105× faster |
| `sieve-fast`   |  1.78 ms |     0.70 ms  |   17.3 ms | 2.6× slower **  |       10× faster |
| `listsum`      |  2.63 ms |     2.68 ms  |   27.7 ms | 1.4× faster *   |       11× faster |
| `fact`         |  2.60 ms |     1.12 ms  |   66.0 ms | 2.3× slower **  |       25× faster |
| `countdown`    |  3.46 ms |     0.51 ms  |  750.0 ms | 6.8× slower **  |      217× faster |
| `sieve`        |  3.73 ms |     2.89 ms  |   55.9 ms | 1.2× slower     |       15× faster |
| `tak`          |  3.93 ms |     2.94 ms  |   68.0 ms | 1.2× slower     |       17× faster |
| `forsum`       |  5.26 ms |     2.64 ms  |  178.1 ms | 2.1× slower     |       34× faster |
| `ackermann`    | 11.7 ms  |     3.67 ms  | 1056.0 ms | 3.5× slower     |       90× faster |
| `pi`           | 20.0 ms  |    20.7 ms   |  947.9 ms | 1.07× faster ◆  |       47× faster |
| `logistic`     |  116 ms  |     92.1 ms  | 2000.8 ms | 1.26× slower ◆  |       17× faster |
| `mlp` (5 ep.)  |  134 ms  |     n/a  *** | 2574.3 ms | —               |     19.2× faster |

(Ratios are just the two time columns divided — e.g. `fib` 4.48 / 0.92 ≈
4.9. They're end-to-end, so they include the launch time noted below.)

**\* The "× faster" rows are real numbers but not a fair race — alcove
isn't running faster than C, it's doing *less work*.** When alcove's
just-in-time compiler
recognizes one of a few specific code patterns, it quietly swaps in a
hand-tuned routine that reaches the same answer a smarter way. The plain
C program does the work the long way:

- `fib` — the source computes Fibonacci with the slow doubling recursion
  (~11 million calls for `fib 33`). alcove turns it into a simple
  counting loop; C actually makes all the calls. (Tell-tale: `fib 42`
  takes 0.37 s in C but ~0 ms in alcove — alcove never makes the ~700
  million calls.)
- `nqueens` — alcove checks queen conflicts with an instant bit-trick
  instead of scanning the board each step.
- `listsum` — alcove hands out memory from a fast internal pool; the C
  version calls the slower system `malloc()` once per list cell.

Write the C version using the *same* trick (a counting-loop `fib`, a
bit-trick `nqueens`, a memory pool) and **C wins again.** So these rows
show off what alcove's JIT does, not that the language outruns C.

**\*\* The C time barely reflects real work, so its "× slower" is
overstated.** On `fact` and `countdown` the C compiler is clever enough
to notice the answer is a fixed constant and work it out *while
compiling*, so the run-time C program does almost nothing. On
`sieve-fast` C simply finishes in well under a millisecond — too fast to
time reliably at this scale. In all three the C column is so small that
the ratio mostly measures alcove's own launch cost, not a fair race.

**◆ The fair fight.** `pi` and `logistic` are float-heavy scalar loops with no
"trick" — alcove and C run the same arithmetic. alcove's numeric tail-loop JIT
compiles the loop body to native code (unboxed doubles in registers), so it lands
at **1.07× faster to 1.26× slower than `gcc -O2`** — genuine near-parity with C on
exactly the numeric kernels alcove is built for.

**\*\*\*** `mlp` (the §2 demo) has no C version (it trains a neural net with
randomness, so the result differs run to run); it's **~19× faster than plain
Python** now that each layer's forward and backward pass is a single fused
SIMD call (`mat-vec!` / `mat-vec-t!` / `vec-ger!`).

Note on launch time: every number above includes starting the program —
about **1 ms for alcove** and **9 ms for Python** — which is a large
share of the very fast rows. Reproduce with `make benchmark`.

### 2. ML in Lisp — digit classifier in 130 lines

`examples/mlp/` ships a 64→32→10 MLP that trains from scratch on the
UCI optdigits dataset (3823 train / 1797 test, the same data behind
`sklearn.datasets.load_digits`). Pure alcove — no BLAS, no numpy
dependency. **95% test accuracy, ~135 ms for 5 epochs** on a laptop
(~19× faster than the same algorithm in plain Python).

```
$ cd examples/mlp
$ make           # downloads dataset, packs, trains
…
final test  acc: 1709/1797   (95.1%)
real    0m0.14s
```

The speed comes from fusing each dense layer into a single SIMD C call over
the underlying `double` storage: forward is `mat-vec!` (W·x + bias), the input
gradient is `mat-vec-t!` (Wᵀ·g), and the weight update is `vec-ger!` (rank-1) —
no per-element interpreter loop. See [`examples/mlp/README.md`](examples/mlp/README.md)
for the full story (~280× over the per-element baseline).

**Automatic differentiation — real MNIST.** [`examples/autograd/`](examples/autograd/)
is a small reverse-mode autograd built on those same kernels: you write only the
forward pass and `(backward loss)` fills every gradient. Its flagship,
`mnist.alc`, trains a 784-128-10 MLP on the full 60k-image MNIST dataset to
**97.5% test accuracy in ~28 s** — ~3× faster than the identical per-sample
training loop in Python+NumPy (`mnist_baseline.py`, ~92 s on the same machine).

### 3. Embedded Redis-compatible server

`alcove -r 6379` boots a RESP2 server. `redis-cli` and any redis
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

Restart `alcove --db game.dump` and `board` is back. The savedb
format round-trips fixnum, float, char, string, symbol, pair, blob,
vec, hash-map, set, deque (arbitrarily nested — a vec of dicts, a dict
of deques, etc.), and lambda (re-compiled on load). A value whose type
(or any nested element) has no serializer is skipped with a warning,
never corrupting the rest of the dump.

### 6. Three execution layers, picked automatically

1. **Tree-walking evaluator** — runs top-level forms and anything the
   bytecode compiler hands back. A `def`/`fn` body still compiles even
   when it calls a builtin the VM has no native opcode for: that one
   sub-call is emitted as `OP_EVAL_AST` (evaluated by the tree-walker)
   while the rest of the lambda stays compiled and keeps its
   tail-call elimination. Only a few constructs send the *whole* body
   back to the tree-walker — a nested `fn`/`def` (its closure must
   capture a stable env) and the `match` / `for-gen` tail-aware forms
   (they rely on the tree-walker's own tail-marker trampoline).
2. **Bytecode VM** — runs every compiled `def`/`fn` body. Tail calls
   (self and cross-function) run in O(1) C stack, so deep recursion
   through `if`/`and`/`or`/`cond`/`case`/`let`/`when`/`for` and around
   builtin sub-calls does not overflow.
3. **JIT** — arm64 / amd64 native code for ~12 hot shapes (leaf
   arithmetic, self-tail loops, fib / fact / tak / ackermann /
   nqueens-safe? / sieve, …). Falls back to the bytecode VM when a
   shape doesn't match.

Inspect with `(disasm f)` — it prints the bytecode plus the JIT
install status.

### 7. Pattern matching, generators, and structured error handling

Recent additions to the language:

**`match`** — structural pattern matching. Patterns include literals,
capture bindings, `(list …)`, `(cons h t)`, `(vec …)`, `(quote sym)`,
and `(? pred)` guards:

```lisp
(match shape
  (list 0 0)       "origin"
  (list x 0)       (str "x-axis x=" x)
  (list x y)       (str "point " x "," y)
  _                "not a 2d point")

(match (/ 10 x)
  (? number?)      "ok"
  _                "error")
```

**`cond`** — flat multi-arm conditional (like `case` but evaluates each
test instead of matching a key):

```lisp
(cond (< n 0) "neg"  (is n 0) "zero"  "pos")
```

**`try`/`finally`** — catch errors and guarantee cleanup:

```lisp
(try (risky-call)
     (fn (e) (str "caught: " (error-message e)))
     (cleanup))          ; always runs, even on success
```

An **uncaught** error prints a call backtrace (the chain of functions that led
to it), and `(backtrace)` returns the live call stack as a list for custom
handlers:

```
err.alc:3: Illegal division by 0
  (run)
  ^
  backtrace (most recent call first):
    divide
    compute
    run
```

**`call/cc`** — escape continuations for early return / non-local exit
(escape-only: `k` escapes outward within its `call/cc`'s extent). See
[`docs/call_cc.md`](docs/call_cc.md):

```lisp
(def clamp (x lo hi)
  (call/cc (fn (ret)
    (do (if (< x lo) (ret lo)) (if (> x hi) (ret hi)) x))))
(clamp 99 0 10)                    ; → 10  (returns early, skips the rest)
```

**`defc`** — sugar for that idiom: it defines a function whose body is wrapped
in `(call/cc (fn (return) …))`, so `return` is an escape continuation — an
imperative-style early exit without the boilerplate:

```lisp
(defc clamp (x lo hi)
  (if (< x lo) (return lo))
  (if (> x hi) (return hi))
  x)
(clamp 99 0 10)                    ; → 10
```

**Destructuring params** — any parameter slot may be a list pattern:

```lisp
(def add-vec2 ((ax ay) (bx by)) (list (+ ax bx) (+ ay by)))
(add-vec2 (list 1 2) (list 3 4))   ; → (4 6)
```

**Generators** — lazy sequences with `!`-suffix operators. Unlike
`map`/`filter` which materialise full lists, these compute on demand:

```lisp
; all squares of odd numbers up to 100, without intermediate list
(collect!
  (filter! odd
    (map! (fn (x) (* x x))
      (range! 10))))                 ; → (1 9 25 49 81)

; for-each! iterates a generator
(for-each! x (range! 1 6) (pr (* x x)) (pr " "))  ; 1 4 9 16 25

; manual stepping
(= g (iter! (list "a" "b" "c")))
(next! g)                           ; "a"
(done? (next! (range! 0 0)))        ; t
```

See [`examples/adder/new-features.adr`](examples/adder/new-features.adr)
for comprehensive examples of all five features.

### 9. Adder — a Pythonic Lisp

**Adder is an attempt to make a Pythonic Lisp** — Lisp's
homoiconic core dressed in Python's indentation-based, paren-light
surface syntax. The goal is code that reads like Python while staying
fully a Lisp underneath, with no loss of macros or code-as-data.

`make als` builds `adder`: the full runtime plus a whitespace /
`:`-block reader. It is *not* a new language — the reader turns
indentation into ordinary Lisp forms *before* macro-expansion, so it
stays fully homoiconic. A line is a list; a trailing `:` opens a
block (or `head: body` inline on one line, Python-style); `name(args)`
is sugar for `name (args)`; `'x` is `(quote x)`.

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

**`if` / `elif` / `else`.** Python-style multi-way branching desugars to
Alcove's flat Arc-style `cond` — each `elif`/`else` is appended to the *same*
`if` node as another test/expr pair, so `if t1 / elif t2 / else` becomes
`(if t1 e1 t2 (do e2) (do e3))`:

```python
def grade (n):
  if (>= n 90):
    'A
  elif (>= n 80):
    'B
  else:
    'F
```

The one constraint: **`elif`/`else` must be indented at the same column as the
`if` they extend** — they attach to the nearest preceding `if` at their own
indent level. Nesting an `elif` *under* the `if`'s body (deeper indent) detaches
it, and the chain silently won't behave as written. (Same rule as Python: the
`elif`/`else` keyword lines up with `if`.) An `if` with no `elif`/`else` is just
the two-armed `(if cond then [else])`.

**The REPL is block-aware** — continuation prompt, auto-indent after
`:`, blank line submits, live syntax highlighting + history. And
`(source f)` prints the definition *back as Adder*, normalised
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

Adder is accepted at the prompt, in `.adr` files, piped
stdin, and `-e`; `alcove` itself is unchanged. `adr.py` (forward)
and `alc2adr.py` (`.alc` → `.adr`, with builder laddering) are
offline tools. See [`examples/adder/`](examples/adder/)
and [`adder-spec.md`](adder-spec.md).

### 10. Shell scripts — the scripting floor (v0.2)

Scripts are real programs: `#!` is a comment in both readers, `*args*`
carries the command line, and the stdlib covers env, filesystem,
subprocesses, JSON, base64/hex, and POSIX-ERE regex — no FFI needed:

```python
#!/usr/bin/env adder
# greet.adr — chmod +x and run: ./greet.adr world
if (no *args*):
  do:
    eprn "usage: greet.adr NAME"        # stderr
    exit 1
prn "hello, " (first *args*) " from " (getenv "USER" "?")
prn (get (shell "uname -s") "out")      # → {"out" ..., "exit" 0}
prn (json-encode (frequencies (re-find-all "[a-z]" "banana")))
```

The same calls work identically in Alcove syntax. The full kit:
`*args*` · `getenv`/`setenv` · `epr`/`eprn` · `read-line` ·
`list-dir`/`file-info`/`make-dir`/`rename-file`/`delete-file` ·
`(shell cmd)` · `json-encode`/`json-decode` · `base64-*`/`hex-*` ·
`re-match`/`re-find`/`re-find-all`/`re-replace`/`re-split` ·
`group-by`/`frequencies`/`partition`/`interleave`/`max-by`/`min-by` ·
`string-pad-*`/`string-repeat`/`starts-with?`/`ends-with?` — see
[`examples/adder/dirstat.adr`](examples/adder/dirstat.adr) for a complete
utility script in ~50 lines.

---

## Examples

| path | what |
|---|---|
| [`examples/mario/`](examples/mario/) | Side-scrolling platformer with SDL2 (native) and Canvas/WASM (web). See the [live demo](https://aallamaa.github.io/alcove/mario.html). |
| [`web/learn.html`](web/learn.html) | Editable Rosetta-style examples for Alcove and Adder. See the [live page](https://aallamaa.github.io/alcove/learn.html). |
| [`docs/lisp-hyperpolyglot-alcove.html`](docs/lisp-hyperpolyglot-alcove.html) | Lisp comparison table extended with Alcove and Adder. See the [live table](https://aallamaa.github.io/alcove/docs/lisp-hyperpolyglot-alcove.html). |
| [`web/playground.html`](web/playground.html) | In-browser playground — editable code, shareable links, and an 8-step guided tour in BOTH syntaxes (an Alcove/Adder picker). See the [live page](https://aallamaa.github.io/alcove/playground.html). |
| [`examples/mlp/`](examples/mlp/) | MLP digit classifier on UCI optdigits — full pipeline with `make data && make train`. |
| [`examples/autograd/`](examples/autograd/) | Reverse-mode autograd in pure alcove: gradient check, automatic-backprop MLP, and real MNIST to 97.5% in ~28 s (3× NumPy). |
| [`examples/arkanoid.alc`](examples/arkanoid.alc) | Auto-playing arkanoid on the terminal — mutable-string framebuffer, ANSI rendering. |
| [`ffi-examples/`](ffi-examples/) | libm, libc strings, sleeping via usleep, a custom .so for everything FFI can call. |
| [`examples/adder/`](examples/adder/) | Adder (`.adr`) — Python-like indentation syntax over the same Lisp forms; `make als` → `adder`. `dirstat.adr` is the scripting-floor showcase. |

---

## Install

Prebuilt binaries (`alcove` + `adder` in one tarball) are on the
[releases page](https://github.com/aallamaa/alcove/releases):
**linux-x86_64** (full: JIT, readline, FFI), **linux-aarch64**
(static — runs anywhere incl. Raspberry Pi; JIT, no readline/FFI), and a
**wasm** bundle (both cores + the playground pages, self-hostable).

```sh
tar xzf alcove-0.2.0-linux-x86_64.tar.gz
./alcove-0.2.0-linux-x86_64/alcove          # REPL
```

On macOS (or for the full feature set anywhere), build from source — it's
one C file and takes a few seconds:

```sh
git clone https://github.com/aallamaa/alcove && cd alcove
make && make als && make install            # → ~/.local/bin/{alcove,adder}
```

## Build

Requires `cc`, `make`. Optional: `libreadline` (REPL line-editing,
history, paren-match, color), `libffi` (the `(ffi-fn …)` builtin).
`make deps` prints what was auto-detected.

```sh
make              # build alcove, JIT enabled (default goal)
make nojit        # JIT off; bytecode only
make jit-mono     # JIT + single-threaded refcounts (fastest)
make als          # → adder (Adder front end)
make install      # install alcove and adder into ~/.local/bin by default
make parser       # debug build with -g3
make test         # run test.alc (1600+ asserts) + ffi-examples
make test-all     # every build variant + equiv-sweep + jit-fuzz + web smoke
make benchmark    # alcove vs python3 microbenchmarks (incl. mlp)
make web          # → web/alcove-core.{js,wasm} via Emscripten
sh tools/release.sh  # release tarballs into dist/ (x86_64 + aarch64 + wasm)
```

After `make install`, use it like this. Make sure `~/.local/bin` is in `PATH`.

```sh
alcove                          # REPL (rlwrap recommended)
alcove file.alc                 # run script and exit
alcove --noload                 # skip auto-load of ./db.dump
alcove --db saved.dump file.alc # load vars from saved.dump first
alcove -e '(+ 1 2)'             # evaluate one expression
alcove -r 6379                  # RESP2 server mode
```

---

## Documentation

- **Language guide**: [`docs/alcove-language.md`](docs/alcove-language.md)
  — practical reference: literals, special forms, builtins, FFI,
  macros, persistence, closures, footguns. Generated from `alcove.c`
  + tested against `test.alc`.
- **Escape continuations**: [`docs/call_cc.md`](docs/call_cc.md) — the
  `call/cc` guide (early return, non-local exit, breaking nested loops,
  exception-style bailout) with idioms and the escape-only semantics.
- **Multithreading design**: [`docs/multithreading.md`](docs/multithreading.md)
  — how the RESP server's sharded reactors work, MPSC inboxes,
  refcount duality.
- **Feature proposals**: [`docs/specs/proposals.md`](docs/specs/proposals.md)
  — open language ideas (string-buf, try/catch, unquote-splicing).
- **Adder**: [`adder-spec.md`](adder-spec.md)
  — the indentation reader spec, plus
  [`examples/adder/README.md`](examples/adder/README.md)
  for the `adder` REPL, `.adr` files, and the offline tools.
- **Editor support**: [`editor/`](editor/README.md) — syntax
  highlighting for vim and emacs (drop-in files + install steps), and a
  **language server** (`tools/lsp.alc`, written in Alcove itself):
  live diagnostics, completion with docs, and hover for BOTH dialects.
- **Debugger**: `alcove --debug script.alc` is an interactive, gdb-style
  debugger — `break`/`step`/`next`/`continue`, `bt`, `frame`, `locals`, and
  `p <expr>` (evaluate in any frame's scope), with break-on-raise, TAB
  completion (commands + in-scope variables) and color. `(break)` is the
  in-code breakpoint. See the language guide's "Debugging" section.

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
make install
alcove
```

I would advice to use rlwrap

```
rlwrap alcove
```
