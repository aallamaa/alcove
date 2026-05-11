# The alcove language — a working overview

This document is a practical guide to the alcove language as it actually
runs today. It is meant for someone who wants to *write programs* in
alcove (games, scripts, embedded callbacks for the RESP server) rather
than hack on the interpreter itself. Everything below is sourced from
`alcove.c`, `alcove.h`, `.init.alc`, and the regression suite in
`test.alc`. When something is unique to alcove (vs Arc / Common Lisp /
Scheme), it is called out explicitly.

---

## 1. What alcove is

alcove is a Lisp-1 modeled after **Arc** (Paul Graham) with influences
from Clojure. It ships as a single C binary (`./alcove`) that combines:

- a **REPL / script runner**,
- a **persistent key-value store** (`db.dump` is auto-loaded on
  startup; values marked `(persist 'name)` survive process restarts),
- a **RESP2 server** mode for embedding the interpreter behind a
  Redis-compatible wire protocol (`-r PORT`),
- an **FFI** to any shared library via libffi.

Internally there are three execution layers, all transparent to the
user:

1. **Tree-walking evaluator** — fallback for forms the compiler does
   not yet understand.
2. **Bytecode VM** — runs compiled function bodies (`def`, `fn`).
3. **JIT** — arm64 / amd64 native code for hot shapes (leaf arithmetic,
   self-tail loops, two-call recursion). Gated behind `make jit`.

The compiler picks the best layer per function silently. You write Lisp;
you get JIT speed when the shape allows. On the four canonical
microbenchmarks (fib, fact, forsum, countdown) alcove with JIT beats
CPython 3.13 by 1.6×–60×.

### Running it

```sh
make                  # build ./alcove (JIT by default; `make nojit` opts out)
./alcove              # REPL (use rlwrap for line editing)
./alcove file.alc     # run a script and exit
./alcove --noload     # skip loading db.dump at startup
./alcove --db foo.dump file.alc      # load persistent vars from foo.dump
./alcove --no-init    # skip running .init.alc on startup
./alcove -e '(+ 1 2)' # evaluate expression and exit
./alcove -r 6379      # RESP2 server on port 6379
```

Script execution prints the value of each top-level form unless it is
`nil`. Errors do **not** abort a script — alcove logs the error and
keeps going. There is no `try` / `catch`.

---

## 2. Lexical syntax

### Literals

| literal      | example                        | type            |
|--------------|--------------------------------|-----------------|
| fixnum       | `42`, `-7`, `1000000`          | tagged 61-bit   |
| float        | `3.14`, `1.0e9`                | IEEE 754        |
| string       | `"hello\n"`                    | mutable bytes   |
| character    | `#\a`, `#\Z`, `#\ ` (space)    | byte            |
| symbol       | `foo`, `+`, `mario-x`          | interned        |
| nil          | `nil`, `()`, `'()`             | the empty list  |
| true         | `t`                            | symbol `t`      |
| vector       | `#[1 2 3]`                     | fixed-size      |

Strings are **mutable** — `(= (s i) ch)` writes a byte. There is **no
string concatenation builtin** (no `concat`, `str`, `format`). Build
strings either by mutating a pre-sized buffer or by streaming output
with multiple `(pr …)` calls.

### Reader

- `;` starts a line comment.
- `'expr` reads as `(quote expr)`.
- `` `expr `` is the quasiquote macro form (used in `defmacro`).
- `,expr` is unquote inside a quasiquote (test.alc:595 has the canonical
  example: `(defmacro my-when (cond body) `(if ,cond ,body nil))`).

### Truthiness — heads-up

alcove follows Arc-ish rules with **one deviation**: `(no 0)` is `t`,
i.e. `0` is **falsey**. This bites you when checking remaining lives or
remaining bricks: prefer `(< lives 1)` or `(is bricks 0)` over `(no
bricks)` if you actually want to test "is it zero".

The empty list, the empty string `""`, and `nil` are all falsey. Any
non-empty list, any symbol other than `nil`, and any non-empty string
are truthy.

Container types follow the same emptiness rule: an empty `vec`,
`blob`, hash-map, or deque is falsey; non-empty is truthy. So `(if v
…)` and `(no v)` Just Work on any container.

---

## 3. Special forms

```
(def name (params...) body...)         ; named function, top-level binding
(defmacro name (params...) body)       ; macro; body returns the expansion
(fn (params...) body...)               ; anonymous lambda; closes over env
(let var val body...)                  ; one binding
(with (v1 e1 v2 e2 ...) body...)       ; multiple bindings
(if c1 t1 c2 t2 ... else)              ; cond-style ladder; no `cond` keyword
(case key v1 r1 v2 r2 ... default)     ; flat pairs, Arc-style
(when c body...)                       ; (if c (do body...) nil)
(and ...) (or ...)                     ; short-circuit; both tail-aware
(do body...)                           ; sequence; value of last form
(for v start end body...)              ; inclusive range, integer counter
(while cond body...)                   ; loop while cond truthy
(quote x)                              ; same as 'x
(= place val)                          ; assignment — see below
```

### `(= place val)` — the only assignment form

There is no `set!`, no `inc!`, no `setq`. Everything mutates through
`=`. The left-hand side may be:

- a bare symbol — `(= x 5)` binds or rebinds the symbol in the
  innermost scope where it is bound; if it is unbound, it becomes a
  global.
- `(s i)` where `s` is a string — writes the byte at index `i`.
- `(v i)` where `v` is a vector — writes that slot.
- `(car xs)` / `(cdr xs)` — replaces the cell of a pair.

### `if` is cond-style

`(if c1 t1 c2 t2 ... else)`. The two-arg form `(if c t)` returns `nil`
when `c` is false. There is **no separate `cond`**.

### `case` is flat-pairs

```
(case (mod n 2) 0 'even 1 'odd 'unknown)
```

The default is the last bare expression — there is no `else` keyword.
Comparison is `iso` (structural equality).

### `for` is integer-only and inclusive

`(for i 1 5 body)` binds `i` to 1, 2, 3, 4, 5 in turn. Empty range
(start > end) returns `nil`. The body's value of the **last** iteration
is the value of the form.

### Closures

`fn` closes over the enclosing environment. Both pure (read-only)
captures and mutable captured state work:

```
; pure: partial application
(def make-adder (n) (fn (x) (+ x n)))
(= add5 (make-adder 5))
(add5 7)                ; 12

; mutable: stateful counter via captured var
(def make-counter ()
  (let n 0
    (fn () (do (= n (+ n 1)) n))))
(= c (make-counter))
(c) (c) (c)             ; 1, 2, 3 — captured n persists per closure
```

Mutating a captured var via `(= var …)` walks up the scope chain and
updates the originating slot — separate calls to `make-counter`
produce independent counters because each call has its own `let`
binding. Globals captured by `fn` also work but live in the single
global env, so they're shared across every closure that refers to
them.

`def` is sugar for `(= name (fn (params…) body…))` — `def` and `fn`
produce the same kind of object. There is no separate `lambda` form;
just use `fn`. `(fn? x)` tests for a function (including builtins).

The `NO CLOSURES` invariant in older internal notes refers to the env
arena's storage layout, not the user-visible language.

---

## 4. Builtin functions

### Arithmetic

`+ - * /` are variadic. `(+)` is `0`, `(*)` is `1`, `(- 5)` negates,
`(/ a b c)` left-folds. Integer division uses `/`; mixing a float
promotes the result to float.

`mod abs min max expt sqrt-int` work as expected. `**` is an alias for
`expt`. Integers exceeding 2^60 silently widen to floats — there is no
bignum.

Comparisons `< > <= >=` are **variadic and chained** in the SQL/Python
sense: `(< 1 2 3 4)` is `t` iff each adjacent pair is ordered. With
zero or one argument they are vacuously `t`.

Equality:
- `is` — identity / EQ for atoms (symbols, fixnums, chars). Cheap.
- `iso` — structural equality, recurses into lists/strings.
- `in` — `(in x a b c)` is `(or (iso x a) (iso x b) (iso x c))`.

Type predicates (all return `t` / `nil`): `number?`, `string?`,
`symbol?`, `pair?`, `fn?`. There is no `vec?`, `blob?`, `dict?`,
`deque?` user-facing — check via `(is x nil)` for nil, or just access
and handle errors.

### Bitwise

`bit-and bit-or bit-xor` plus aliases `& | ^`, `~` for bitwise NOT,
`<<` and `>>` for shifts. `>>` is sign-preserving.

### Lists

`cons car cdr list length nth reverse append`. All accept the obvious
arguments and return errors (not segfaults) on type mismatch — the
returned error is an `exp_t*` of error type that prints visibly but is
truthy under `(no (no e))`, which is how the regression suite traps it.

### Higher-order

`map filter reduce apply any? all?`. `reduce` is left-fold:
`(reduce + 0 xs)`. `apply` flattens its last list arg into a call.

### Strings & chars

Strings index with `(s i)` returning `#\c`. Mutate with `(= (s i) ch)`.
That is the entire surface. There is no `substring`, no `concat`. To
build a string up character by character, allocate the full buffer as a
literal of spaces and overwrite slots.

### Vectors

```
(= v (vec 5 0))           ; length 5, all 0
(vec-len v)               ; 5
(vec-ref v 0)             ; 0
(vec-set! v 2 'x)
```

Vectors hold any `exp_t`. `(vec n init)` errors instead of segfaulting
on absurdly large `n`. The literal form `#[1 2 3]` is shorthand for
`(vector 1 2 3)`.

**Tensor bulk ops** — for ML / numerical work, nine builtins walk a
vec's underlying storage in C and do the math in raw `double`s instead
of allocating an `EXP_FLOAT` per element. They accept vec elements
that are either float or fixnum (coerced):

| primitive               | semantics                          | mutates? |
|-------------------------|------------------------------------|----------|
| `(vec-dot a b)`         | `Σ a[i]*b[i]` → float              | no       |
| `(vec-max v)`           | largest element → float            | no       |
| `(vec-argmax v)`        | index of largest element → int     | no       |
| `(vec-axpy! y a x)`     | `y[i] += a*x[i]`                   | y        |
| `(vec-scale! v a)`      | `v[i] *= a`                        | v        |
| `(vec-add! y x)`        | `y[i] += x[i]`                     | y        |
| `(vec-copy! dst src)`   | `dst[i] = src[i]`                  | dst      |
| `(vec-fill! v a)`       | `v[i] = a` for all i               | v        |
| `(vec-relu! v)`         | `v[i] = max(0, v[i])`              | v        |

The mutating ops write output slots in place when the existing slot is
a uniquely-owned `EXP_FLOAT` (`nref == 1`, `!SHARED`); otherwise they
fall back to allocating a fresh boxed float. On a steady-state MLP
inner loop the fast path takes 100% of writes — typically ~35× faster
than the hand-rolled per-element interpreter version. See
`examples/mlp/` for a worked example.

The `(vec-dot W[k] x)` pattern wants `W` stored as a vec-of-rows
(outer vec of inner vecs) rather than a flat row-major vec, so each
row is a contiguous walk. The mlp demo follows that layout.

### Hash-maps (dicts)

Clojure-style mutable dicts with string keys (anything is `pr`-printed
to derive the key, so `:foo`, `"foo"`, and `'foo` all hash the same):

```
(= d (hash-map))
(assoc! d "k" 1)
(assoc! d "k2" "v")
(get d "k")           ; 1
(contains? d "k")     ; t
(dissoc! d "k")
(keys d) (vals d)     ; lists
(count d)             ; 1
```

`(hash-map "k1" v1 "k2" v2 …)` builds a populated dict. Iteration
order is hash-bucket order, not insertion order.

### Deques

Doubly-linked deque with O(1) ends:

```
(= q (deque))
(push-right! q 1) (push-right! q 2)
(push-left!  q 0)         ; q is now (0 1 2)
(peek-left q)             ; 0  — no mutation
(peek-right q)            ; 2
(pop-left! q)             ; 0  — mutates
(pop-right! q)            ; 2
(count q)                 ; 1
```

`(deque x y z)` builds a populated deque. Push/pop returns the value
moved; peek returns nil on empty.

### Blobs

Binary-safe byte buffer, distinct from `string` only in that strings
use C `strlen` everywhere (so NUL bytes terminate them). Use blob for
files, RESP values, or any byte-array workload.

```
(= b (make-blob 16))            ; 16 zero bytes
(= b2 (make-blob "abc"))         ; 3 bytes from string
(blob-len b)
(blob-ref b 0)                   ; → fixnum (byte value 0..255)
(blob->string b)                 ; copy bytes into a string
(string->blob "xyz")             ; opposite direction
(read-bytes "path/to/file")      ; slurp a file → blob (or nil)
```

`blob-ref` is read-only; to mutate, convert to string and back, or
use FFI.

### I/O

`pr x y z` — write each arg to stdout, no separator, no newline.
`prn x y z` — same but trailing newline. `print` and `println` exist as
aliases.

File reading: `(read-bytes "path")` returns a blob (see above) or
`nil` on missing/unreadable. There is **no `read-line` or
`read-char`** — keyboard input requires the FFI route described
below, and writing to files goes through FFI too.

### Time, randomness, persistence

- `(time expr)` evaluates `expr` and prints elapsed wall time. Useful
  for spot benchmarks, **not** as a wall-clock query.
- `(random n)` returns a fixnum in `[0, n)`.
- `(persist 'sym)` marks a top-level binding as persistent. `savedb
  "path"` writes them; auto-loads on startup. `forget 'sym` unbinds.
  `unpersist 'sym` clears the flag without unbinding. Round-trip
  works for fixnum, float, char, string, symbol, pair, lambda
  (source-form), blob, and vec (including nested vec-of-vec with
  heterogeneous element types). Dict and deque do not yet round-
  trip — `savedb` prints `skipping <name> — type X has no dump fn`.
- `(eval form)` evaluates a quoted form in the global environment.

---

## 5. FFI — calling C

```
(= sleep_us (ffi-fn "libc.so.6" "usleep" "int" "int"))
(sleep_us 100000)
```

`(ffi-fn LIB FN RET-TYPE ARG-TYPES…)` returns a callable. Up to **8
arguments** are supported. Type strings:

| string                | C type           | alcove type                    |
|-----------------------|------------------|--------------------------------|
| `"void"`              | `void`           | nil (return only)              |
| `"int"`               | `int32_t`        | fixnum or char (codepoint)     |
| `"long"`, `"int64"`   | `int64_t`        | fixnum or char (codepoint)     |
| `"double"`            | `double`         | float or fixnum or char        |
| `"string"`, `"char*"` | `const char *`   | string                         |
| `"ptr"`, `"void*"`    | `void *`         | fixnum (the address)           |

Numeric arg slots (`int` / `long` / `double`) accept **chars**
directly — alcove passes the codepoint as the integer value. So
`(gfx-text-set i (s i))` works without an explicit `(int->char)`
coercion (where `(s i)` returns a char).

Library names are passed straight to `dlopen`. Use `libc.so.6` on Linux
and `libc.dylib` on macOS — alcove does no name munging. Use a path
with `/` for your own libraries.

### What FFI cannot do

- **No struct-by-value, no varargs, no callbacks.** If you need any of
  these, write a small C shim that exposes flat-int / flat-pointer
  entry points and `dlopen` it.
- **Returned `char*` is copied** into a new alcove string. C-side
  `free()` is your problem — wrap it via `(ffi-fn "libc..." "free"
  "void" "ptr")`.
- **Reserved alcove names cannot be shadowed** at top level. `time` is
  taken; bind FFI wrappers under different names (`ctime`, `c_time`).
- **`NULL` is `0`** — pass the fixnum `0` for any pointer arg.

The Mario example in `examples/mario/` ships an SDL2 C shim
(`mario_gfx.c`) that exposes window/renderer lifecycle, key state,
sprite drawing primitives, and millisecond timing under flat-int
entry points alcove can dlopen on either OS.

---

## 6. Macros

`defmacro` defines a macro whose body returns the expansion. Quasiquote
+ unquote work:

```
(defmacro my-when (cond body)
  `(if ,cond ,body nil))
```

There is no `unquote-splicing` (`,@`). Variadic macros work via rest
arguments only inside the macro body — you can splice manually using
`cons` / `list` to build the expansion.

---

## 7. Persistence and the RESP server

Two features unique to alcove that are easy to overlook:

1. `(persist 'sym)` + `db.dump` — the running session is durable
   across restarts. Skip with `--noload`.
2. RESP2 server mode (`-r 6379`) — `redis-cli` clients can talk to
   the live alcove process. `(redis-defcmd "MYCMD" (fn (args) …))`
   exposes a Lisp function as a Redis command. The full command set
   (SET / GET / INCR / LPUSH / HSET / LRANGE / TTL sweep, etc.) runs
   at 80–99 % of redis-server's throughput with ~5× lower memory.

If you only want to write scripts you can ignore both. They become
relevant when alcove is used as an embedded server.

---

## 8. Patterns used by the example games

### Mutable string as a 2D framebuffer (arkanoid, mario)

A single string of `H * (W+1)` bytes — each row is `W` chars plus a
newline — is the level. `(= (board (+ x (* y STRIDE))) ch)` writes one
cell. `(board idx)` reads. Indices may be variables and arbitrary
expressions; the `(s i)` form evaluates `i` correctly in every
position (literal, var, computed, in compiled bodies).

### ANSI rendering

```
(= CLEAR  "\x1B[2J\x1B[H")            ; clear + home cursor
(pr CLEAR)
```

Color glyphs embed the escape inline:

```
(= MARIO "\x1B[1;91mM\x1B[0m")         ; bright red M
(pr MARIO)
```

Each escape sequence is zero-width on a real terminal so the layout
stays aligned.

### Frame pacing

```
(= sleep_us (ffi-fn "libc.dylib" "usleep" "int" "int"))  ; macOS
(= sleep_us (ffi-fn "libc.so.6"  "usleep" "int" "int"))  ; linux
(sleep_us 33000)                       ; ~30 fps
```

For a cross-platform game, build a tiny `libgame.so`/`libgame.dylib`
that exposes `game_sleep_ms` so the alcove file is OS-agnostic.

### Keyboard input

There is no built-in. Either:

- Auto-play (the arkanoid approach — paddle tracks the ball) — no
  input needed, runs anywhere.
- C shim — call `tcsetattr` + `fcntl(O_NONBLOCK)` + `read`, expose a
  `term_getch` that returns the byte or `-1`. The Mario example uses
  this route.

---

## 9. Things alcove does **not** have

So you do not waste time looking:

- No `cond`, `progn` (use `do`), `let*` (use `with`), `setq`, `setf`,
  `destructuring-bind`.
- No multiple return values, no continuations, no exceptions.
- No `string-append`, `format`, `printf`-style formatting.
- No floating-point formatting controls — `(prn 1.0)` prints
  `1.000000`.
- No bignums; integers wider than 60 bits silently become floats.
- No threads visible from user Lisp. The RESP server runs sharded
  reactors internally but those shards do not expose user-facing
  primitives. There is no `future`, `agent`, `go`, `spawn`.
- No regex, no JSON, no HTTP. Use FFI.
- No module system. Top-level `def`s land in the single global env.
  `(load "file.alc")` does not exist either — use the shell:
  `cat lib.alc main.alc | ./alcove`.

When in doubt, search `alcove.c` for the symbol; if it is not in the
reserved-symbol table it is not a builtin.

## Footguns to know about

- **Don't name a parameter `t` or `nil`.** `lookup` resolves reserved
  symbols *before* it walks function-param slots, and caches the result
  on the symbol's `meta` field. A function `(def f (t) ...)` will see
  `t` resolve to the truth singleton inside its body regardless of
  what the caller passed in — and the symbol stays mis-resolved for
  future calls too. The same applies to any builtin name (`+`, `if`,
  `vec-ref`, etc.) used as a param. Pick unambiguous names like
  `tile`, `val`, `x`.
- **Vector accessor is `vec-ref`, not `vec-get`.** alcove follows
  Scheme/Racket naming. `(vec-get ...)` is an unbound symbol and
  silently produces an error each call — particularly bad inside a
  render loop where the error is swallowed.
- **FFI args of type `"int"` accept fixnums *and* chars** (auto-
  coerced via codepoint). String indexing returns a char, so
  `(my-c-fn (s i))` works for ASCII-byte APIs without manual
  conversion.
- **Hex literals** (`0xFF`, `0X10`, optional leading `+`/`-`) parse as
  fixnums. Out-of-fixnum-range hex falls through to symbol — there is
  no hex float syntax.
