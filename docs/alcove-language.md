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
from Clojure. It ships as a single C binary (`alcove`) that combines:

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
you get JIT speed when the shape allows. On the canonical microbenchmarks
(fib, fact, forsum, countdown, …) alcove with JIT beats CPython 3.13 by
roughly 50×–700× on the shapes the JIT recognizes — see the README
benchmark table or `make benchmark-jit` for current figures.

### Running it

```sh
make                  # build alcove (JIT by default; `make nojit` opts out)
make install          # install alcove and adder into ~/.local/bin by default
alcove              # REPL (use rlwrap for line editing)
alcove file.alc     # run a script and exit
alcove --noload     # skip loading db.dump at startup
alcove --db foo.dump file.alc      # load persistent vars from foo.dump
alcove --no-init    # skip running .init.alc on startup
alcove -e '(+ 1 2)' # evaluate expression and exit
alcove -r 6379      # RESP2 server on port 6379
```

Script execution prints the value of each top-level form unless it is
`nil`. Errors do **not** abort a script — alcove logs the error (with a
caret and a call backtrace) and keeps going, then exits non-zero. Use
`(try body handler)` to catch errors in-language.

The error location is **precise**: it points at the exact failing expression,
not just the top-level form. A division-by-zero inside a function reports the
function's line with a caret under the offending form —

```
div.alc:2: Illegal division by 0
  (/ 1 x))
  ^
  backtrace (most recent call first):
    toto
```

even though the call that triggered it is on a different line. This comes from a
per-bytecode `pc → source line` table built at compile time and read **only when
an error is raised** — so it costs nothing at runtime (the VM/JIT loop never
touches it). `--no-line-info` disables source-position tracking entirely (errors
fall back to top-level-form granularity); it's a minimalism switch, not a speed
one — tracking is parse-time only and never on the hot path.

### Editor intelligence

`tools/lsp.alc` is an LSP server written in Alcove (see
[`editor/README.md`](../editor/README.md) for Neovim/Emacs/VS Code setup):
live syntax diagnostics, completion for every builtin with its docstring,
and hover docs — serving `.alc` and `.adr` from one process.

### Debugging (`--debug`)

`alcove --debug script.alc` (works for `.adr` too: `adder --debug`) runs the
program under an interactive, gdb-style debugger. Debug mode runs on the AST
tree-walker (like `--interpret`), so every call frame is a live environment you
can inspect, and **tail-call optimisation is disabled** so every call is a real,
named frame (deep tail loops can overflow the C stack while debugging — that's
the trade for honest backtraces).

Like gdb, it opens at a **setup prompt** before running anything: set
breakpoints, then `c` to run (or `s` to step from the start). A breakpoint
stops with the callee's parameters already in scope. From the `(dbg)` prompt:

| command | does |
|---|---|
| `step` / `s` | run the next form, descending into calls |
| `next` / `n` | run the next form, stepping *over* calls |
| `continue` / `c` | resume until a breakpoint |
| `break <fn>` / `break <line>` | stop on entry to a function, or at a source line |
| `bt` / `where` | full backtrace (innermost first), with each frame's line |
| `frame N` / `up` / `down` | select a frame for `locals` / `p` |
| `locals` | the selected frame's bindings |
| `p <expr>` | evaluate any expression **in the selected frame's scope** |
| `quit` / `q` | detach and run to completion |

```
$ alcove --debug div.alc
-- debugger ready. set breakpoints (break <fn|line>), then 'c' to run.
(dbg) break toto
(dbg) c
-- break in toto, line 2:
   (/ 1 x)
(dbg) p x
  0
(dbg) bt
 *#0 toto   line 2   (/ 1 x)
  #1 test   line 4   (toto (* x 2))
```

`p` evaluating in a frame's environment is nearly free here — a frame *is* a
first-class env, so `p` is just `eval` against it. For an `.adr` file the lines
and caret map back to the Adder source (run it with `adder --debug`).

On a terminal the `(dbg)` prompt has **TAB completion** and color: TAB on an
empty line lists every command; on a partial word it completes the command; and
after `p`/`break` it completes symbols from the selected frame's scope (its
locals, then globals and builtins). Output is plain when stderr isn't a tty.

`(break)` is the in-code breakpoint: drop it anywhere and execution stops at
that point with the debugger attached (full backtraces and source lines need
`--debug`; `locals`/`p` work from the live scope regardless).

**Break-on-raise.** Under `--debug`, an error raised *outside* any `(try ...)`
drops straight into the debugger at the raise site — with every frame and its
locals still live, so you land exactly where it broke and can `bt` / `locals` /
`p <expr>`. `c` lets the error propagate as usual; `q` detaches. This is like
gdb's `catch throw`: wrapping an expression in `(try ...)` suppresses the break
(it's being handled), and because errors are first-class values, an `(error? …)`
probe will also break at the raise — `c` past it, or use `try`.

**Recover with a value.** At a break-on-error, `return <expr>` evaluates `<expr>`
in the failing frame's scope and makes the failing expression yield *that* value
instead of the error — the computation then continues as if it had succeeded
(Common-Lisp `use-value` semantics):

```
** error raised: Illegal division by 0
-- break in toto, line 1:
   (/ 1 x)
(dbg) return 999      ; (/ 1 x) now yields 999; the program runs on, no error
```

### Scripts: arguments, environment, shebang

Everything after the script path lands in `*args*` as a list of strings
(`nil` when there are none — REPL, plain `-e`, embedded):

```sh
alcove deploy.alc prod --fast     # *args* = ("prod" "--fast")
alcove -e '(prn *args*)' a b      # ("a" "b")
```

`#!` starts a comment in both readers, so scripts run directly:

```sh
#!/usr/bin/env alcove             # or: #!/usr/bin/env adder
```

The process environment and standard streams:

```lisp
(getenv "HOME")             ; → "/home/you" | nil
(getenv "PORT" "6379")      ; default when unset
(setenv "MODE" "fast")      ; visible to (shell ...) children
(eprn "to stderr, no colour")
(read-line)                 ; one stdin line | nil at EOF
```

In Adder the same script reads:

```python
#!/usr/bin/env adder
if (no *args*):
  eprn "usage: greet.adr NAME"
  prn "hello, " (first *args*)
```

See [`examples/adder/dirstat.adr`](../examples/adder/dirstat.adr) for a
complete utility script exercising the whole scripting floor.

### Sandboxing (RESP client callbacks, `--safe`)

When alcove serves the Redis-compatible protocol (`alcove -r`), a connected
client can only invoke the operator's `redis-defcmd` callbacks (there is no
client `EVAL`). Those callbacks are **sandboxed**: while one runs on behalf of a
client, the *privileged* builtins — anything that escapes into the host (`shell`,
the file ops, `setenv`, `ffi-*`, `load`/`require`/`eval`, `savedb`/`loaddb`,
`redis-defcmd`, the debugger, `exit`…) — are refused. So code a network client
triggers cannot reach the OS, filesystem, FFI, or load code. Pure computation,
local `let`/`fn`, string/JSON/regex, and the keyspace ops (`redis-get`/`set`/…)
stay available — and global mutation (`def`/`=`/`persist`) is separately refused
in callbacks by the existing concurrency guard.

The operator's own setup runs **unsandboxed**, so `def`/`defn`/`defmacro` and the
rest work normally when you build the server. To let a specific callback use a
privileged builtin, grant it from trusted init code:

```lisp
(allow-unsafe "shell")   ; now callbacks may (shell …); allow-unsafe is itself sandboxed
```

`--safe` applies the same restriction **process-wide** (for running an untrusted
script via the CLI). Enforcement is one chokepoint, `invoke_internal`, that every
builtin invocation flows through (no bypass via `apply`/`map`/a compiled fn);
privileged builtins carry the `FLAG_UNSAFE` bit. It costs nothing when neither a
callback nor `--safe` is active (one never-taken branch).

### The web build (wasm)

The browser/wasm build runs the same language with three differences:
`(shell ...)` returns an error (no subprocesses in a browser), FFI and the
RESP server are compiled out, and the filesystem is emscripten's in-memory
MEMFS (file ops work, but against a sandbox). Everything else — JSON,
regex, tensors, the JIT-less VM — behaves identically; the wasm smoke
battery (`make test-web`) pins that.

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

Strings are **mutable** — `(= (s i) ch)` writes a byte. Build strings
with `str` (concatenate any values), `string-append` (concatenate
strings), or `fmt` (printf-style templating); see [Strings &
chars](#strings--chars). You can also mutate a pre-sized buffer in
place or stream output with multiple `(pr …)` calls.

### Reader

- `;` starts a line comment. `#` followed by a space (`# …`) is also a
  line comment, to end of line. A `#` glued to a token is a reader literal
  instead: `#[1 2 3]` vector, `#{1 2 3}` set, `#b"bytes"` blob, `#\c` char —
  all of which print back in the same form and re-read.
- `'expr` reads as `(quote expr)`.
- `` `expr `` reads as `(quasiquote expr)` — a template that evaluates
  the unquoted holes and quotes everything else.
- `,expr` is `(unquote expr)` — evaluate `expr` and drop its value into
  the template.
- `,@expr` is `(unquote-splicing expr)` — splice the elements of the
  list `expr` into the surrounding list.

```
(= xs (list 2 3))
`(1 ,(+ 1 1) 3)        ; (1 2 3)
`(1 ,@xs 4)            ; (1 2 3 4)
```

These power `defmacro`; the canonical example is
`(defmacro my-when (cond body) `(if ,cond ,body nil))`.

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

### Infix operators

A 3-element form whose **head is a non-callable value** and whose middle
is a binary operator is evaluated infix: `(a op b)` means `(op a b)`.
This is the same head-dispatch that makes `("abc" 1)` index a string —
it only fires when the head is *not* a function, so ordinary calls and
higher-order uses are untouched:

```
(1 + 2)                 ; 3
(let (n 0) (n < 1))     ; t
(1/2 + 1/3)             ; 5/6   — works across the numeric tower
(apply + (list 1 2 3))  ; 6     — head is a function, so a normal call
(map + xs)              ; + stays a first-class argument, NOT infix
```

Operators: `+ - * / < > <= >= is iso isnt mod`. It is **binary only with
no precedence** — group multiple operators with parens:
`((fib (n - 1)) + (fib (n - 2)))`. In Adder the parens are usually
implicit at the line level: `a + b`, `n < 1`. Calling a genuinely
non-callable head that *isn't* an infix form (e.g. `(1 2 3)`) is an
error — quote it (`'(1 2 3)`) if you mean a literal list.

---

## 3. Special forms

```
(def name (params...) body...)         ; named function, top-level binding
(defmacro name (params...) body)       ; macro; body returns the expansion
(fn (params...) body...)               ; anonymous lambda; closes over env
(let var val body...)                  ; one binding; (let (a b) xs ...) destructures
(let* (v1 e1 v2 e2 ...) body...)       ; sequential bindings; each ei sees earlier vars
(with (v1 e1 v2 e2 ...) body...)       ; multiple bindings, evaluated in parallel
(if c1 t1 c2 t2 ... else)             ; multi-arm conditional (see below)
(cond t1 e1 t2 e2 ... default)        ; flat multi-arm: eval tests left-to-right
(case key v1 r1 v2 r2 ... default)    ; flat pairs, Arc-style; match via iso
(match expr pat1 r1 pat2 r2 ... dflt) ; structural pattern matching (see below)
(when c body...)                       ; (if c (do body...) nil)
(unless c body...)                     ; runs body when c is falsey; else nil
(and ...) (or ...)                     ; short-circuit; both tail-aware
(do body...)                           ; sequence; value of last form
(for v start end body...)              ; inclusive range, integer counter
(while cond body...)                   ; loop while cond truthy
(for-each! var gen body...)            ; iterate a generator binding each value
(quote x)                              ; same as 'x
(= place val)                          ; assignment — see below
(setf place val)                       ; exact synonym of = (readable head)
(try body handler)                     ; catch errors; nil handler propagates
(try body handler finally-expr)        ; catch + always run finally-expr
(try body nil finally-expr)            ; no catch; always run finally-expr
```

### `(= place val)` / `(setf place val)` and `(setq sym val)` — assignment

There is no `set!` or `inc!`. Most mutation uses `=`. `setf` is an
**exact synonym** of `=` (same place handling, same scope rules) — it
exists purely as a more readable head, especially in indented Alcove
Script where `setf total 0` reads better than `= total 0`. (`set` is
*not* an alias — it is the set-constructor; see [Sets](#sets).) The
left-hand side may be:

- a bare symbol — `(= x 5)` binds or rebinds the symbol in the
  innermost scope where it is bound. At the REPL or file top level this
  creates a top-level session binding. Inside a function, unbound names
  are local to that call unless a surrounding binding is captured.
- `(s i)` where `s` is a string — writes the byte at index `i`.
- `(v i)` where `v` is a vector — writes that slot.
- `(car xs)` / `(cdr xs)` — replaces the cell of a pair.

`(setq sym val [sym val ...])` is the Emacs-style variable-only form:
it updates the nearest existing lexical binding, but if the name is
unbound it creates/updates a top-level session binding even when called
inside a function. Adder can call it directly:

```text
setq x 10
```

Use `=`/`set` when you want function-local unbound assignment; use
`setq` when you explicitly want top-level fallback.

### `if` and `cond`

`(if c1 t1 c2 t2 ... else)` — multi-arm conditional; the two-arg form
`(if c t)` returns `nil` when `c` is false.

`cond` is the same flat-pair walk but evaluates each test freshly
instead of matching against a discriminant. The lone trailing element is
the default:

```
(cond
  (> x 100) "big"
  (> x 10)  "medium"
  "small")
```

Unlike `if`, all tests are independent expressions — useful when each
branch checks a different condition. Both `if` and `cond` are
tail-call aware.

### `case` is flat-pairs (value dispatch)

```
(case (mod n 2) 0 'even 1 'odd 'unknown)
```

Comparison is `iso` (structural equality). The trailing odd element is
the default. Use `cond` for predicate-based dispatch, `case` for
value dispatch.

### `match` — structural pattern matching

`match` evaluates `expr` once, then tries each pattern left-to-right
until one matches. The first matching arm's result is returned; a lone
trailing element is the unconditional default.

```
(match expr
  pattern1 result1
  pattern2 result2
  default)
```

**Pattern language:**

| Pattern | Matches | Binds? |
|---|---|---|
| `_` | anything | no |
| `nil` / `t` | literal nil or t | no |
| `42`, `"hi"`, `3.14` | exact value | no |
| `x` (any other symbol) | anything | yes — x gets the value |
| `(quote foo)` | the symbol `foo` specifically | no |
| `(? pred)` | if `(pred val)` is truthy | no |
| `(list p1 p2 ...)` | list of exactly that length | sub-patterns bind |
| `(cons ph pt)` | any non-empty pair | head and tail |
| `(vec p1 p2 ...)` | vector of exactly that length | sub-patterns bind |

```
; literal match
(match x  42 "forty-two"  "other")

; capture binding
(match (list 1 2 3)
  (list h . _) (str "head=" h)   ; use (cons h t) for pair
  _            "not a list")

; structural — nested
(match point
  (list 0 0)   "origin"
  (list 0 y)   (str "y-axis y=" y)
  (list x 0)   (str "x-axis x=" x)
  (list x y)   (str "general (" x "," y ")")
  _            "not a point")

; guard predicate
(match n
  (? even?)  "even"
  (? odd?)   "odd"
  _          "non-integer")

; mixed nesting
(match expr
  (list 'if _ _ _)  "if-form"
  (list 'def name _ . _) (str "def of " name)
  (cons h _)        "other call"
  _                 "atom")
```

Variables bound by a pattern are in scope only inside that arm's result
expression. Unmatched arms whose patterns partially bound variables
silently discard those bindings.

### Destructuring in function parameters

A parameter slot may be a list pattern — it destructures the
corresponding argument:

```
(def swap ((x y)) (list y x))
(swap (list 10 20))             ; (20 10)

(def add-pair ((a b) (c d)) (list (+ a c) (+ b d)))
(add-pair (list 1 2) (list 3 4)) ; (4 6)
```

Missing elements get `nil`. Works with rest params:

```
(def first-two ((a b) . rest) (list a b rest))
(first-two (list 10 20) 30 40) ; (10 20 (30 40))
```

### Error handling with `try` / `finally`

```
; catch an error
(try (/ 1 0) (fn (e) (str "caught: " (error-message e))))
; → "caught: Illegal division by 0"

; body succeeds — handler not called
(try (+ 1 2) (fn (e) "err"))   ; → 3

; nil handler = no catch; propagate the error
(try (/ 1 0) nil)              ; → error value

; finally — always runs; value discarded
(try (/ 1 0)
     (fn (e) (str "caught: " (error-message e)))
     (prn "cleanup"))          ; prints "cleanup", returns "caught: …"

; finally-only (no catch)
(try (open-file "f")
     nil
     (close-resources))       ; cleanup always runs; error propagates

; nesting for type dispatch via match
(try risky-call
     (fn (e)
       (match (error-message e)
         (? (fn (s) (string-contains? s "not found")))  'missing
         (? (fn (s) (string-contains? s "timeout")))    'retry
         e)))                  ; re-raise unknown errors
```

When the handler evaluation itself raises an error, the handler's error
surfaces (not the original body error). This lets you distinguish "body
failed" from "cleanup failed".

### `call/cc` — escape continuations

`(call/cc f)` calls `f` with one argument, a continuation `k`. Invoking
`(k v)` abandons the work in progress and makes the whole `call/cc` form
return `v`. If `k` is never called, `call/cc` returns `f`'s normal result.

```
(call/cc (fn (k) (+ 1 2)))          ; → 3    (k unused)
(call/cc (fn (k) (k 42) 999))       ; → 42   (999 never runs)
(call/cc (fn (k) (+ 1 (k 100) 2)))  ; → 100  (the + never completes)

; early return / guard
(def clamp (x lo hi)
  (call/cc (fn (ret)
    (do (if (< x lo) (ret lo)) (if (> x hi) (ret hi)) x))))

; break out of nested loops at once
(call/cc (fn (found)
  (do (= i 1)
      (while (<= i 5)
        (do (= j 1)
            (while (<= j 5)
              (do (if (is (+ i j) 7) (found (list i j))) (= j (+ j 1))))
            (= i (+ i 1)))) nil)))   ; → (2 5)
```

This is the **escape-only** form of `call/cc`: `k` is one-shot and valid
only during `call/cc`'s dynamic extent (you can escape *outward*, not resume
later) — equivalent to Scheme's `call/ec`. Full re-entrant continuations
would need C-stack capture and are not provided. The escape rides the same
value-propagation path as `try`, so it threads cleanly through `if`/`do`/
`while`/`for`/recursion/`map`. See `docs/call_cc.md` for the full guide.

### `defc` — define with an early-return continuation

`(defc name (params…) body…)` defines a function whose body is wrapped in
`(call/cc (fn (return) …))`, binding the escape continuation to `return`. It's
exact sugar for `(def name (params) (call/cc (fn (return) body…)))`, so
`(return v)` exits the function immediately with `v` — an imperative-style
early return without the `call/cc`/`fn` boilerplate. The `clamp` above becomes:

```lisp
(defc clamp (x lo hi)
  (if (< x lo) (return lo))
  (if (> x hi) (return hi))
  x)                           ; falls through to x if neither guard fires
```

`return` is intentionally bound in the body (an anaphoric capture). Like `def`,
the param list may be a list pattern or a bare symbol for rest args
(`(defc f xs …)`). `(return v)` works with any `v`, including the falsy `0` /
`nil`. If the body never calls `return`, the function returns its last form.

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

Mutating a captured var via `(= var …)` walks up the lexical scope
chain and updates the originating slot — separate calls to
`make-counter` produce independent counters because each call has its
own `let` binding. Top-level session bindings can also be referenced by
functions, but parameters and function-local names do not leak back to
the REPL.

`def` is sugar for `(= name (fn (params…) body…))` — `def` and `fn`
produce the same kind of object. There is no separate `lambda` form;
just use `fn`. `(fn? x)` tests for a function (including builtins).

The `NO CLOSURES` invariant in older internal notes refers to the env
arena's storage layout, not the user-visible language.

### `let`, `let*`, and destructuring

`let` takes one binding and a body that may hold several expressions
(the last is the value):

```
(let x 5 (+ x 1) (+ x 10))      ; 15 — multi-expression body
(let (a b) (list 1 2) (+ a b))  ; 3  — destructure a list; missing slots are nil
```

`let*` binds a flat list of pairs **sequentially**, so each value sees
the bindings before it. The legacy flat form `(let* v1 e1 ... body)` is
also accepted for a single body expression:

```
(let* (a 1 b (* a 10)) (+ a b)) ; 11 — b sees a
```

`let`, `let*`, `with`, `when`, and `unless` are all tail-call aware:
the final body expression keeps the caller's tail position, so a
self-call there recurses without growing the C stack.

### Rest parameters

A bare symbol in place of the parameter list collects **all** arguments
into a list; a dotted tail collects the **remaining** ones:

```
(def all-args xs xs)            ; (all-args 1 2 3) → (1 2 3)
(def head-rest (a . rest) rest) ; (head-rest 1 2 3) → (2 3)
((fn xs xs) 7 8)                ; (7 8) — works for anonymous fns too
```

### Default parameter values

A `(name default)` pair in the parameter list makes the parameter
optional; the default expression is evaluated when the caller omits it
(and may reference earlier work — it's an ordinary expression):

```
(def greet (name (greeting "hello"))
  (string-concat greeting ", " name))
(greet "ada")            ; → "hello, ada"
(greet "ada" "salut")    ; → "salut, ada"

(def pad (s (width (+ 8 2))) (string-pad-left s width))   ; computed default
```

There are **no keyword arguments**; for many optional knobs, the idiom
is passing a dict: `(def run (cmd (opts (hash-map))) ...)`.

### Error handling

Errors are first-class values that propagate up the call chain. Instead
of crashing, builtins return an error `exp_t`. You can inspect or catch
them:

```
(error? (/ 1 0))                ; t
(error-message (/ 1 0))         ; "Illegal division by 0"
(try (/ 1 0) (fn (e) 'caught))  ; 'caught — handler runs on error
(try (+ 1 2) (fn (e) 'caught))  ; 3 — body succeeded, handler ignored
```

`(try body handler)` evaluates `body`; if it signals an error, it calls
`(handler error-value)` instead of propagating, returning the handler's
result. Otherwise it returns `body`'s value.

---

## 4. Builtin functions

### Arithmetic

`+ - * /` are variadic. `(+)` is `0`, `(*)` is `1`, `(- 5)` negates,
`(/ a b c)` left-folds. Integer division uses `/`; mixing a float
promotes the result to float.

`mod abs min max expt sqrt-int` work as expected. `**` is an alias for
`expt`. Integers exceeding 2^60 silently widen to floats — there is no
bignum.

Floating-point helpers (all return a float): `round` (nearest integer),
`floor` (largest integer ≤ x), `ceil` (smallest integer ≥ x),
`truncate` (toward zero), `log` (natural log), and `sin` `cos` `tan`
(radians). Coercion: `(float x)` widens an integer; `(int x)` truncates
a float to an integer.

```
(round 2.6)     ; 3.0
(floor 2.9)     ; 2.0
(ceil 2.1)      ; 3.0
(truncate -2.7) ; -2.0
(int 3.9)       ; 3
(float 5)       ; 5.0
```

Comparisons `< > <= >=` are **variadic and chained** in the SQL/Python
sense: `(< 1 2 3 4)` is `t` iff each adjacent pair is ordered. With
zero or one argument they are vacuously `t`.

### Rationals (exact fractions)

`(rational n d)` builds the exact fraction `n/d`, sign-normalized
(denominator always positive) and reduced to lowest terms. The literal
`n/d` reads the same thing — `1/3`, `-7/2`, `22/7`. A fraction that
reduces to a whole number collapses to a plain integer, so `(rational 4 2)`
is `2` and `rational?` is `nil` for it.

```
1/3                 ; 1/3
(rational 2 4)      ; 1/2   (reduced)
(rational 1 -3)     ; -1/3  (sign normalized)
(+ 1/3 1/6)         ; 1/2
(- 7/2 1/2)         ; 3
(* 2/3 3)           ; 2
(/ 1/3 2)           ; 1/6
(numerator 3/4)     ; 3
(denominator 3/4)   ; 4
```

The exact tower mixes with the existing numbers by a **Python-style
contagion lattice**:

- `int` + `rational` → `rational` (exact stays exact);
- `rational` + `float` → `float` (a binary float is inexact, so it wins
  and the result is inexact);
- comparisons between exact values (`int`/`rational`) are done **without
  precision loss** (no float round-trip).

Rationals are **bounded** — numerator and denominator are `int64`. There
is no bignum, so an operation whose exact result would not fit `int64`
**raises an error** rather than wrapping or silently losing precision;
reach for a `float` when you want an inexact large magnitude. `rational?`
tests for a true (non-integer) fraction; `numerator`/`denominator` also
accept a plain integer (returning it and `1`).

### Decimals (exact base-10)

`(decimal "1.50")` builds an exact base-10 number; the literal `1.5m` (note
the `m` suffix — `1.5` is a binary float, `1.5m` is a decimal) reads the same.
Construct from a string (the normal, exact path), an integer, or another
decimal. A **float** argument is refused — a binary float can't represent
`0.1` exactly, so pass a string to say which decimal you mean.

```
0.1m + 0.2m         ; 0.3m   (exact — vs 0.1 + 0.2 = 0.30000000000000004)
(- 10.00m 0.01m)    ; 9.99m
(* 0.2m 0.3m)       ; 0.06m
(/ 1m 8m)           ; 0.125m
(/ 10m 3m)          ; 3.3333333333333333333333333333m  (28 digits, rounded)
```

Decimals are normalized (trailing fractional zeros trimmed), so `1.50m` and
`1.5m` are the same value and `is`/`iso`-equal, and a set keeps just one of
them. The stored form is a **128-bit coefficient + scale**, bounded to **28
significant digits** (rust_decimal-class); a product or quotient beyond that,
and `÷0`, **raise** — there is no bignum.

The contagion rules are **strict** (Python's): a decimal combines with
integers and other decimals (the result is a decimal), but mixing a decimal
with a **float** or a **rational** is an **error** — converting between an
exact decimal, an exact fraction, and a binary float should be explicit, not
silent. (Ordered comparison across these is still allowed; only arithmetic is
strict.) `decimal?` tests for the type.

Equality:
- `is` — identity / EQ for atoms (symbols, fixnums, chars). Cheap.
- `iso` — structural equality, recurses into lists/strings.
- `in` — `(in x a b c)` is `(or (iso x a) (iso x b) (iso x c))`.

Type predicates (all return `t` / `nil`): `number?` (any number —
fixnum, float, rational, or decimal), `string?`, `symbol?`, `char?`,
`pair?`, `list?`, `null?` (alias `nil?`), `fn?`, `vec?`, `blob?`,
`dict?`, `deque?`. All accept zero or one argument; the no-arg form
returns `nil`. `pair?` is true only for *non-empty* pairs — the empty
list (nil) is excluded. `list?` is true for nil **or** any proper list;
`null?` is true only for nil.

Value predicates: `zero?` (the number 0), `yes` (truthy — the complement
of `no`), `not` (alias of `no`), `isnt` (the complement of `is`). So
`(not nil)`, `(yes 5)`, `(zero? 0)`, `(isnt 1 2)` are all `t`. Note `0`
is falsey, so `(zero? 0)` is `t` but `(yes 0)` is `nil`.

### First-class types and defclass

The predicates above answer "what shape is this" one type at a time. `Int`,
`Float`, `String`, `Number`, `List`, `Bool`, … `Type` itself are also **runtime
values** — 30 immediate type objects, one per `alc_type_id` — so you can hold a
type, compare it, and query with it:

```
(type 42)              ; Int
(type 3.14)             ; Float
(type "hi")             ; String
(type? Int)             ; t     — Int is itself a value of type Type
(type-name Int)         ; "Int"
(is-a? 42 Int)          ; t
(is-a? 42 Number)       ; t     — Number, List, Fn, etc. are supertypes
(is-a? 1.5m Number)     ; t     — decimal conforms to Number too
```

Two conformance rules are worth calling out because they follow the language's
own truthiness rules rather than a naive tag check: `nil` **is** the false
boolean here (there is no separate false object), so `(is-a? nil Bool)` is `t`
(and so is `(is-a? t Bool)`, but `(is-a? 0 Bool)` is `nil` — `0` is falsey but
not boolean-typed); and `nil` **is** the empty list, so `(is-a? nil List)` is
`t` alongside any proper list. Binding any of the 30 names as an ordinary
identifier is now rejected — `(= Int 5)`, `(let Int 5 ...)`, `(defmacro Int
...)` all error; this is the one breaking change of this feature.

**Annotations are the same vocabulary as type objects.** The trailing-keyword
JIT hints from before (`(x :int)`, return `:f64`) and a bare type name are
interchangeable — a hint code *is* a type id:

```
(def f (x Int) Float (+ x 1.0))    ; identical compiled shape to:
(def f (x :int) :f64 (+ x 1.0))
```

The JIT only special-cases `Int`/`Float` (`:int`/`:f64`); every other
annotation — `String`, `Bool`, a user class, `Any` — is parsed, validated by
name, and recorded on the lambda, but **inert**: nothing checks a `String`
param against its argument at the call boundary (a wrong-typed argument fails
later, inside whatever builtin first assumes the shape, not at the annotation).

**`defclass`** builds a typed, dict-backed record on top of this:

```
(defclass Account (owner String) (balance Decimal) (active Bool))
(= a (Account "Ada" 100.0m t))
(Account? a)               ; t
(type a)                    ; Account       — the class name IS the type object
(Account-owner a)           ; "Ada"
(Account-balance! a 125.0m) ; checked setter — mutates in place, returns a
(Account-balance! a "oops") ; error — "oops" isn't a Decimal
```

`Account` the constructor **is** the callable type object returned by
`defclass` — `(Account? a)`, `(type-name Account)`, and `(is-a? a Account)` all
work on the same value. Dispatch to the constructor is wired uniformly through
the AST evaluator, `vm_invoke_values`, and compiled calls, so `(Account ...)`,
`(apply Account (list ...))`, and a call inside a compiled function all reach
it. The schema is enforced on **every** write to an instance, not just the
generated setter — a raw `(assoc! a "nope" 1)` on an unknown field errors, and
so does `(dissoc! a "balance")` (schema fields can't be deleted); only
`(assoc! a "balance" 130.0m)` — a known field with a conforming value —
succeeds. Field types may name another class (`(defclass Transfer (from
Account) (to Account) (amount Decimal))`), checked the same way. `(defclass
Marker)` with no field list is legal — a zero-field tag type whose only job is
`(Marker? (Marker))`.

User classes work as annotations exactly like the builtin types — parsed,
recorded, JIT-inert:

```
(def describe (a Account) String (Account-owner a))
(describe 42)   ; NOT rejected at the boundary — errors later, inside
                ; Account-owner ("Account-owner: expected Account")
```

Honest limitations, by design for this first cut: a class **cannot be
redefined** in a session (`(defclass Account ...)` again errors — is-a?
identity would go stale); `defclass` **refuses to clobber** an already-bound
name, and rolls registration back on any failure so the name stays available
for a corrected retry; `savedb`/`loaddb` round-trips an instance's **data and
type tag** but not the validator, so a reloaded instance answers `(type
...)`/`(is-a? ...)` correctly but has lost schema enforcement — reassigning
the class definition before `loaddb` restores the tag mapping but not
re-validation of old writes. Class-body forms are position-based fields
(`(name Type)`) or keyword-headed directives parsed by clause position: at
most one `(:extends Parent)` **first**, then any number of `(name Type)`
fields, then any number of `(:method name (params) body...)` clauses last.
Inheritance is **single** (one parent, no mixins), and there is no `super` —
an overriding method has no way to call the implementation it replaces.

**vs. `defstruct`.** The older `defstruct` remains the untyped/legacy record:
its instances are plain dicts that classify as `Dict` under `type`/`is-a?`
(there is no `Point` type object to check against), whereas a `defclass`
instance classifies as its own class. Reach for `defclass` when you want the
schema and the type identity; `defstruct` is still there for a quick
unchecked bag of fields.

### Inheritance — `(:extends Parent)`

A keyword-headed clause, first in the class body (at most one), inherits a
fully-defined parent's fields:

```
(defclass Animal (name String))
(defclass Dog (:extends Animal) (breed String))
(defclass Puppy (:extends Dog) (weeks Int))
(= d (Dog "Rex" "Lab"))
(Dog-breed d)          ; "Lab"   — own field
(Dog-name d)           ; "Rex"   — inherited field, child-prefixed accessor
(Animal-name d)        ; "Rex"   — PARENT's accessor also works on a child
(is-a? d Dog)          ; t
(is-a? d Animal)       ; t       — subsumption falls out of the is-a? chain-walk
(Animal? d)            ; t       — a predicate is-a? underneath
(type-of d)            ; Dog     — stays most-derived, not Animal
```

The constructor takes **inherited-then-own** fields positionally (`(Dog name
breed)`); the child generates accessors and setters for **every** effective
field (inherited + own) under its own prefix, and a child field **cannot
shadow** an inherited name — `(defclass Bad (:extends Animal) (name String))`
errors before any instance exists. Grandchildren compose transitively —
`Puppy` inherits from `Dog` which inherits from `Animal`, so `(is-a? puppy
Animal)` is `t` and `(Animal-name puppy)` works. A class-typed field accepts
any subclass instance, for the same reason subsumption works everywhere else:

```
(defclass Shelter (pet Animal))
(Shelter (Dog "Rex" "Lab"))   ; fine — Dog is-a Animal
```

`Parent` must already be a **fully-defined** class — `(defclass X (:extends
NoSuchClass) ...)` and `(defclass X (:extends Int) ...)` (a builtin type as
parent) both error — and `:extends` must be the **first** clause
(`(defclass X (a Int) (:extends Animal) ...)` errors). The parent's type
object is recorded on the schema itself: `(get Dog__class "parent")` is
`Animal`; a base class has no `"parent"` key.

### Methods are generic functions — `(:method name (self args...) body...)`

Method clauses come after all field clauses. `(:method speak (self)
body...)` does **not** stash a closure inside the instance — it registers
`speak` as a plain multimethod keyed by `(type self)`, built on the same
`defmulti`/`defmethod` machinery `type-of`-dispatch already had. Calling it is
an ordinary call, `(speak x)` — there is no method-lookup syntax, so it costs
nothing the compiler/JIT doesn't already know how to handle:

```
(defclass Animal (name String)
  (:method speak (self) "..."))
(defclass Dog (:extends Animal) (breed String)
  (:method speak (self) "woof"))
(defclass Puppy (:extends Dog) (weeks Int))   ; no speak of its own

(speak (Dog "Rex" "Lab"))      ; "woof"  — Dog's own method
(speak (Puppy "Bit" "Lab" 6))  ; "woof"  — inherits Dog's, not Animal's
(speak (Animal "Gen"))         ; "..."   — Animal's own
```

Overriding is just defining the method again on the more specific class — the
resolver (`__mm-lookup`) tries the instance's exact type, then walks the
`:extends` parent chain outward, and finally falls back to a method
registered for the `Any` type object if one exists:

```
(defmethod speak Any (x) "???")     ; defmethod OUTSIDE a class body, by type key
(speak 99)                          ; "???"             — Any is the fallback
(defmethod speak Int (n) (str "the integer " n))
(speak 99)                          ; "the integer 99"  — a specific key beats Any
(speak (Dog "Rex" "Lab"))           ; "woof"  — still Dog's; Any never shadows a hit
```

`defmethod`'s second argument is the same type-object vocabulary as
everything else in this section — a user class (`Dog`), a builtin type
(`Int`), or `Any` — so class methods and ad hoc `defmethod`s on builtin types
share one dispatch table. `defmulti` is idempotent (redeclaring an existing
generic keeps its method table), which is what lets several sibling
`defclass` bodies extend one shared generic name. There is no `super` yet — an
override has no way to call the implementation it replaces.

Adder writes the same feature with a block form, and note that a
**multi-line** `:method` body needs a trailing colon on the clause line
(`:method speak (self):` then an indented body), while a single-line body
doesn't (`:method speak (self) "woof"`). Adder also layers **dot syntax** on
top of all of this (`d.speak(x)`, `d.breed`, `d.breed = "Lab"`, all Adder-only
sugar over the forms shown above) — see the Adder chapter of the book for the
full desugaring.

### Bitwise

`bit-and bit-or bit-xor` plus aliases `& | ^`, `~` for bitwise NOT,
`<<` and `>>` for shifts. `>>` is sign-preserving.

### Lists

`cons car cdr list length nth reverse append`. All accept the obvious
arguments and return errors (not segfaults) on type mismatch — the
returned error is an `exp_t*` of error type that prints visibly but is
truthy under `(no (no e))`, which is how the regression suite traps it.

Sequence utilities:

```
(take 2 (list 1 2 3 4))   ; (1 2)
(drop 2 (list 1 2 3 4))   ; (3 4)
(range 0 4)               ; (0 1 2 3) — end-exclusive
(range 0 6 2)             ; (0 2 4)   — optional step
(zip (list 1 2) (list 3 4)) ; ((1 3) (2 4)) — stops at the shorter
(flatten (list 1 (list 2 (list 3)))) ; (1 2 3)
(sort (list 3 1 2))       ; (1 2 3) — numbers by value, strings lexicographically
(sort-by (fn (x) (- 0 x)) (list 1 2 3)) ; (3 2 1) — sort by (key-fn element)
(partition 2 (list 1 2 3 4 5))   ; ((1 2) (3 4)) — trailing partial dropped
(partition 2 1 (list 1 2 3))     ; ((1 2) (2 3)) — sliding window via step
(interleave (list 1 2) (list "a" "b")) ; (1 "a" 2 "b") — stops at shortest
(max-by length (list "aa" "b" "ccc"))  ; "ccc"   (min-by for the smallest)
(group-by odd (range 0 5))       ; {"t" (1 3), "nil" (0 2 4)} — keys are
                                 ; STRINGIFIED (dicts are string-keyed)
(frequencies (list "a" "b" "a")) ; {"a" 2, "b" 1} — same stringified keys
```

### Higher-order

`map filter reduce apply any? all?`. `reduce` is left-fold:
`(reduce + 0 xs)`. `apply` flattens its last list arg into a call.

### Symbols & macro hygiene

`(gensym)` returns a fresh, session-unique symbol (`G0`, `G1`, …).
`(with-gensyms (a b) body...)` binds each name to a fresh gensym for the
duration of `body` — the standard way to avoid variable capture inside
`defmacro`:

```
(defmacro swap (x y)
  (with-gensyms (tmp)
    `(let ,tmp ,x (= ,x ,y) (= ,y ,tmp))))
```

### Strings & chars

Strings index with `(s i)` returning `#\c`. Mutate with `(= (s i) ch)`.

Common whole-string helpers:

```
(str "x=" 42)
(substr "abcdef" 1 4)            ; "bcd"
(string-append "alc" "ove")
(string-split "a,b,c" ",")
(string-join (list "a" "b") "/")
(string-trim "  hi  ")
(string-upcase "fast")
(string-downcase "LOUD")
(string-contains? "hello world" "world")  ; t
(string-index "hello" "ll")     ; 2 — 0-based, or nil if absent
(string-replace "a-b-c" "-" "+") ; "a+b+c" — replaces all occurrences
(starts-with? "foobar" "foo")    ; t       (ends-with? for suffixes)
(string-repeat "ab" 3)           ; "ababab"
(string-pad-left "7" 3 "0")      ; "007" — width counts CODEPOINTS;
(string-pad-right "ab" 4)        ; "ab  " — pad: char or 1-codepoint string
```

`fmt` is the formatting function: `{}` interpolates the next argument
with default rendering, and `{:<spec>}` applies a printf-style spec
(`{:.2f}`, `{:x}`, `{:05d}`, `{:s}`). The spec accepts only printf
flags/width/precision plus a type char; anything else (e.g. an embedded
`%` or `*`) is left as literal text rather than risking malformed
output.

```
(fmt "{} + {} = {:.1f}" 1 2 3.0) ; "1 + 2 = 3.0"
(fmt "{:x}" 255)                 ; "ff"
(fmt "{:05d}" 42)                ; "00042"
```

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

**Element-kind inference (typed storage)** — `make_vector` and the
`#[...]` reader pick the tightest cell representation based on the
elements supplied: all-fixnum → `int64_t[]`, all-numeric (any float
present) → `double[]`, otherwise a generic `exp_t*[]` fallback. The
typed kinds skip per-cell `exp_t` boxing, which is what gives the MLP
example its 30%-ish step-time gain over the pre-refactor build.
Writing a value whose type doesn't match the cell kind transparently
promotes the whole vec to the generic kind; mixing fixnums into an
f64 vec quietly stores them as doubles.

**Deque ops** — `(vec-push! v x)` appends at the back, `(vec-pop! v)`
removes and returns the last element, `(vec-unshift! v x)` prepends at
the front, `(vec-shift! v)` removes and returns the first. All
amortised O(1) via a cap/start/end window; no mid-shift on pop, and
grow is 1.5x with recentering on front-grow.

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

### Sets

Hash-backed sets store unique scalar values by typed identity. Fixnums,
floats, chars, strings, symbols, blobs, `nil`, and `t` are supported.
Different types do not collide, so `1`, `"1"`, and `'1` are distinct set
elements, and `set->list` preserves the original element types.

```
(= s (set 1 2 2 "a" :kw))
(set? s)                         ; t
(set-has? s :kw)                 ; t
(set-add! s 3)                   ; mutates, returns s
(set-del! s 1)                   ; mutates, returns s
(set->list s)                    ; order undefined
(set-union s (set 3 4))
(set-intersection s (set :kw 9))
(set-difference s (set "a"))
```

Adder reserves `set` for assignment, so use `hash-set` there:
`set s (hash-set 1 2 3)`.

### Persistent maps (HAMT)

Unlike `hash-map` (mutable), a HAMT is **immutable**: `assoc`/`dissoc` return
a new map that shares structure with the old one (O(log32 n) time and space),
so prior versions stay valid. Keys are compared by value (`isequal`): fixnum,
char, float, string, symbol, blob.

```
(= m (hamt "a" 1 "b" 2 "c" 3))
(hamt-count m)                   ; 3
(hamt-get m "b")                 ; 2
(hamt-get m "z" 99)              ; 99   (default for a missing key)
(hamt-contains? m "a")           ; t
(= m2 (hamt-assoc m "d" 4))      ; new map; m unchanged
(hamt-count m)                   ; still 3
(= m3 (hamt-dissoc m "b"))       ; new map without "b"
(hamt-keys m)                    ; list of keys (unordered)
(hamt-vals m)                    ; list of values
(hamt->list m)                   ; flat (k1 v1 k2 v2 …)
(hamt-merge a b)                 ; union; b's value wins on shared keys
(hamt? m)                        ; t
```

### Regular expressions (POSIX ERE)

```lisp
(re-match "([a-z]+)-([0-9]+)" "see abc-42")  ; ("abc-42" "abc" "42") | nil
(re-find "[0-9]+" "ab 123")        ; (3 6) — BYTE offsets, for substr
(re-find "a" "banana" 2)           ; (3 4) — optional start offset
(re-find-all "[0-9]+" "1 22 333")  ; ("1" "22" "333")
(re-replace "[0-9]+" "a1b22c" "#") ; "a#b#c" — replacement is LITERAL
(re-split "[ ,]+" "a, b  c")       ; ("a" "b" "c")
```

The engine is libc's POSIX **Extended** regex (`regcomp`/`regexec`), so it
works identically in the native build and the browser/wasm build with zero
dependencies. Mind the dialect: character classes are `[[:digit:]]` /
`[[:alpha:]]` (there is **no `\d` / `\w`**), quantifiers are greedy only
(no `*?`), and there is no lookaround and no `$1` backrefs in
`re-replace`. Offsets are bytes, not codepoints. Compiled patterns are
cached (8 entries), so a regex in a loop doesn't recompile. An unmatched
optional group comes back as `nil`; a pattern that matches the empty
string makes `re-split` error (it would split between every byte).

### JSON

```lisp
(json-encode (hash-map "name" "alcove" "tags" (list 1 2.5 t)))
;; → "{\"name\":\"alcove\",\"tags\":[1,2.5,true]}"
(json-encode v 2)                ; pretty-print, 2-space indent
(json-decode "{\"k\": [1, null]}")  ; → {"k" (1 nil)}
```

Mapping: dicts ↔ objects, lists **and vectors** → arrays (arrays decode to
lists), `t` ↔ `true`, `nil` → `null`. Decode is lossy the same way
msgpack-decode is: `false` and `null` both come back as `nil` — and since
the empty list IS `nil`, `()` encodes as `null`. Symbols encode as strings.
Integral numbers decode to fixnums, the rest to floats. NaN/Infinity and
non-JSON types (blob, fn) make encode error; malformed or trailing input
makes decode error. The decoder is depth-capped and fuzz-hardened
(`make json-fuzz`).

### base64 / hex

```lisp
(base64-encode "binary or string")   ; → padded RFC 4648 string
(base64-decode "Zm9v")               ; → blob (blob->string for text)
(hex-encode #b"\xab")                ; → "ab" (lowercase)
(hex-decode "41427e")                ; → blob; either case accepted
```

### MessagePack

`(msgpack-encode v)` serializes a value to a MessagePack blob;
`(msgpack-decode blob)` parses it back. Supported value types: nil, `t`,
fixnums, floats, strings (and symbols), blobs (→ bin), lists (→ array), and
string-keyed dicts (→ map). Chars encode as their integer codepoint.

```
(= b (msgpack-encode (list 1 "two" (hash-map "k" 3.5))))
(msgpack-decode b)               ; → (1 "two" {"k" 3.5})
```

Unsupported types (lambda, ffi, …) make `encode` error; malformed or
truncated input (or a non-string map key) makes `decode` error — both
catchable with `try`.

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
aliases. `epr` / `eprn` are the stderr twins (uncoloured — diagnostics
stay grep-able when redirected). `(read-line)` reads one line from stdin
(`nil` at EOF).

Whole-file helpers:

```
(read-bytes "path")              ; blob
(write-bytes "path" blob)
(read-string "path")             ; string
(write-string "path" "text")
(append-string "path" "more")
(read-lines "path")              ; list of strings
(file-exists? "path")
(load "lib.alc")                 ; evaluate Alcove forms from a file
```

Ports / streaming file IO:

For processing large files or handling streams, Alcove supports buffered streaming using ports:

```lisp
(open path mode)   ; open a file for buffered streaming. mode is "r", "w", or "a"
(close port)       ; close an open port (idempotent)
(write port str)   ; write string to a writable port
(read-line [port]) ; read one line (without trailing newline) from port or stdin
(eof? port)        ; t if the readable port is at EOF, nil otherwise
(port? x)          ; t if x is a port, nil otherwise
```

To ensure files are properly closed in case of errors, use the manual cleanup idiom with `try/finally`:

```lisp
(let (f (open path "r"))
  (try
    (do ... (read-line f) ...)
    (finally (close f))))
```

Note: ports will also auto-close upon being garbage collected if they are no longer referenced, but explicit cleanup is recommended.

Filesystem management:

```
(list-dir "src")                 ; sorted entry names ("." ".." excluded)
(file-info "a.txt")              ; {"size" n, "mtime" unix-secs, "dir?" t|nil}
                                 ; ... or nil if the path doesn't exist
(make-dir "build/out")           ; mkdir -p; idempotent
(rename-file "a" "b")
(delete-file "a")                ; files and empty directories
```

Subprocesses:

```
(shell "git status --short")     ; → {"out" "...captured stdout...", "exit" 0}
(shell "ls nope 2>&1")           ; append 2>&1 to capture stderr too
```

`shell` runs the command through `/bin/sh`; a signal-terminated child
reports `128+signal` as its exit code (the shell convention). It is not
available in the web build (the call returns an error).

There is **no `read-char` or stream/port object**. Character-level
keyboard input (raw tty) still uses the FFI route described below.

### Modules

`(require "mod")` loads `mod.alc` exactly once per session — searched
relative to the requiring file, then `$ALCOVE_PATH`, then cwd. A module
can open with `(ns mymod)` so its `def`s become `mymod/name`;
`(require "mod" :refer parse emit)` imports chosen names unqualified
(`:refer` alone imports them all). `(require "mod.so")` dlopens a native
module (see the embedding guide). `load` remains the no-bookkeeping
textual include.

### Time, randomness, persistence

- `(time expr)` evaluates `expr` and prints elapsed wall time. Useful
  for spot benchmarks, **not** as a wall-clock query.
- `(random n)` returns a fixnum in `[0, n)`.
- `(persist 'sym)` marks a top-level binding as persistent. `savedb
  "path"` writes them; auto-loads on startup. `forget 'sym` unbinds.
  `unpersist 'sym` clears the flag without unbinding. Round-trip
  works for fixnum, float, char, string, symbol, pair, lambda
  (source-form), blob, vec, hash-map, set, and deque — arbitrarily
  nested (a vec of dicts, a dict of deques, etc.). A value whose type
  (or any nested element, e.g. a builtin or ffi handle) has no
  serializer is skipped with a warning — `savedb` prints
  `skipping <name> — type X (or a nested element) has no dump fn` —
  and the rest of the dump is written intact.
- `(eval form)` evaluates a quoted form in the current evaluator
  environment.

### Observability

For operating a server or embedded deployment: machine-readable error
classes, leveled logging, and (opt-in) metrics.

**Error codes.** `(error-code e)` returns an error's class as a stable,
prose-independent symbol — or `nil` if `e` is not an error. The code is
fixed even if the message wording changes, so handlers can dispatch on it
rather than string-matching:

```
(let ((e (try (/ 1 0) (fn (e) e))))
  (error-code e))                       ; 'div-by-zero
(error-code (try (foo) (fn (e) e)))     ; 'unbound-variable
(error-code 42)                         ; nil — not an error
(if (is (error-code e) 'div-by-zero) … …)
```

Classes include `'div-by-zero`, `'unbound-variable`, `'illegal-value`,
`'number-expected`, `'missing-parameter`, `'index-out-of-range`,
`'missing-name`, `'parse-error`, and a catch-all `'error`. Like
`error-message`, `error-code` does **not** re-raise the error it inspects.

**Logging.** `(log! LEVEL MSG key val …)` emits one [logfmt](https://brandur.org/logfmt)
line to stderr when `LEVEL` is at or above the current threshold:

```
(log! :warn "cache miss" :key "users/42" :ms 12)
; → ts=2026-06-18T13:51:47.407Z level=warn msg="cache miss" key=users/42 ms=12
```

Values containing a space, `=`, or `"` are quoted. The level is one of
`:debug :info :warn :error`. Below the threshold, `log!` writes nothing and
returns `nil`; otherwise it returns the emitted line as a string (so it is
testable without capturing stderr). `(log-debug …)`, `(log-info …)`,
`(log-warn …)`, `(log-error …)` are level-fixed wrappers. `(log-level)`
returns the current minimum level (default `:info`); `(set-log-level :warn)`
changes it. The threshold is atomic and shared across RESP reactors. (It is
`log!`, not `log`, because `log` is the natural logarithm.)

**Metrics (opt-in).** Counters and gauges live in a small fixed registry,
compiled in **only** when you build with `-DALCOVE_METRICS` (`make
alcove-with-metrics` or `adder-with-metrics`). The default binary ships none
of it, so the RESP command path carries no shared-counter contention:

```
(counter! "jobs.done")        ; → 1   (adds 1, creating the counter)
(counter! "jobs.done" 5)      ; → 6
(gauge!   "queue.depth" 12)   ; → 12  (sets an absolute value)
(metric   "jobs.done")        ; → 6   (or nil if it doesn't exist)
(metrics)                     ; → ("jobs.done" 6 "queue.depth" 12 …)
```

A metrics build also auto-instruments the RESP server, surfacing
`resp.connections`, `resp.commands`, and `resp.errors` in `(metrics)`.
Increments are atomic (thread-safe across reactors); only slot *creation*
takes a brief lock.

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
with `/` for your own libraries. An **empty lib name `""`** resolves
symbols already linked into the alcove process (libc/libm), portably:
`(ffi-fn "" "strlen" "long" "string")`. `(ffi?)` reports whether the
build has libffi.

### Callbacks — pass an alcove fn to C

`(ffi-callback RET (ARG-TYPES…) FN)` wraps an alcove function in a libffi
closure and returns a value you pass wherever a `ptr` arg is expected — so
C can call back into alcove (e.g. a comparator, an event handler).

```
(= cb (ffi-callback "long" (list "long" "long") (fn (a b) (+ a b))))
(some-c-fn cb 17 25)          ; C invokes cb -> 42
```

Ret/arg types: `void int long double ptr` (no string return — the buffer's
lifetime can't outlive the call). Closures work as callbacks. See
`ffi-examples/05-callbacks.alc`.

### Struct-by-value

`(ffi-struct FIELD-TYPES…)` defines a by-value aggregate (fields:
`int long double ptr`, or a nested `ffi-struct` descriptor). The descriptor
is usable as an `ffi-fn` arg/return type and with `(ffi-pack DESC vals…)` →
a blob, and `(ffi-unpack DESC blob)` → a list of field values.

```
(= Point (ffi-struct "double" "double"))
(= norm2 (ffi-fn LIB "pt_norm2" "double" Point))
(norm2 (ffi-pack Point 3.0 4.0))     ; → 25.0
(ffi-unpack Point (ffi-pack Point 7.5 8.5))   ; → (7.5 8.5)
```

See `ffi-examples/06-structs.alc`.

### Varargs

`(ffi-vfn LIB FN RET FIXED-ARG-TYPES…)` binds a variadic C function. The
given types are the fixed prefix; extra call args have their type inferred
(fixnum→`long`, so use `%ld`; float→`double`; char→`int`; string/nil→
pointer). The cif is built per call, so each call site can pass a different
variadic tail.

```
(= printf (ffi-vfn "" "printf" "int" "string"))
(printf "%ld and %.1f\n" 42 3.5)
```

See `ffi-examples/07-varargs.alc`.

### Other notes

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
   exposes a Lisp function as a Redis command; `args` is a list of
   binary-safe blobs. `(redis-set k v)` stores existing alcove values
   into the RESP keyspace: strings/blobs/numbers/chars become Redis
   strings, deques become Redis lists, and hash-maps become Redis
   hashes. Vectors stay `EXP_VECTOR` and can be read back with
   `(redis-val k)` for tensor/vector builtins. The full command set
   (SET / GET / INCR / LPUSH / HSET / LRANGE / TTL sweep, etc.) runs
   at 80–99 % of redis-server's throughput with ~5× lower memory.

If you only want to write scripts you can ignore both. They become
relevant when alcove is used as an embedded server.

---

## 11. Patterns used by the example games

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

## 9. Generators — lazy sequences

Generators are stateful iterators. Unlike `map`/`filter` which
materialise a full list, generator operations are lazy: each element is
computed only when requested. The convention: `!`-suffixed names operate
on generators; `?`-suffixed names are predicates; no suffix = eager list
operations.

### The sentinel

`*done*` is the exhaustion sentinel — a unique immortal object. Test
with `done?`:

```
(done? (*done*))                ; t
(done? 42)                      ; nil
```

### Constructors

```
(iter! list)               ; generator over an existing list
(range! end)               ; 0..end-1
(range! start end)         ; start..end-1
(range! start end step)    ; step may be negative: (range! 10 0 -1)
(map! fn gen)              ; lazy map — wraps gen, applies fn on each next!
(filter! pred gen)         ; lazy filter — skips elements where pred returns nil
```

### Consuming

```
(next! gen)         ; advance gen, return next value or *done*
(collect! gen)      ; drain gen to a list
(for-each! x gen body...)  ; iterate, binding each value to x; returns nil
```

### Examples

```
; basic range
(collect! (range! 5))             ; (0 1 2 3 4)
(collect! (range! 2 8 2))         ; (2 4 6)
(collect! (range! 10 0 -3))       ; (10 7 4 1)

; manual stepping
(= g (iter! (list "a" "b" "c")))
(next! g)                         ; "a"
(next! g)                         ; "b"
(done? (next! (range! 0 0)))      ; t — empty range immediately done

; lazy pipeline — no intermediate list allocated
(collect!
  (filter! odd
    (map! (fn (x) (* x x))
      (range! 10))))               ; (1 9 25 49 81)

; for-each! with side effects
(for-each! x (range! 1 4)
  (pr x) (pr " "))                 ; prints "1 2 3 "

; generators compose lazily
(= positives (filter! (fn (x) (> x 0))
               (iter! (list -3 1 -1 4 -2 5 0 9))))
(collect! positives)               ; (1 4 5 9)

; manual iteration loop
(= g (range! 100))
(= found nil)
(while (no found)
  (let v (next! g)
    (if (done? v) (= found 'none)
        (iso v 42) (= found v))))
found                              ; 42

; generator as infinite stream (use next! manually, don't collect!)
(def naturals () (range! 0 9999999999))
(= n (naturals))
(next! n)   ; 0
(next! n)   ; 1
(next! n)   ; 2 ...
```

### `map!` vs `map`

| | `map` | `map!` |
|---|---|---|
| Input | a list | a generator |
| Output | a fully materialised list | a new generator |
| When computed | immediately, all elements | on demand, one at a time |
| Memory | O(n) | O(1) for the pipeline |
| Use when | you need a list result | chaining transformations |

```
(map (fn (x) (* x x)) (list 1 2 3))        ; (1 4 9)  — full list now
(collect! (map! (fn (x) (* x x)) (range! 3))) ; (0 1 4) — same result, lazy
```

The `gen-` prefixed forms (`gen-list`, `gen-range`, `gen-map`,
`gen-filter`, `gen-next!`, `gen-collect`, `gen-done?`, `*gen-done*`)
are kept as aliases for backwards compatibility.

---

## 10. Things alcove does **not** have

So you do not waste time looking:

- No `progn` (use `do`). No `setf` as mutation (use `=` or the `setf`
  alias for `=`). `cond`, `match`, `try/finally`, and generators now exist.
- No multiple return values. Errors are reified as values and can be caught
  with `(try body handler)` rather than via unwinding exceptions.
- Continuations are **escape-only** (`call/cc` = Scheme's `call/ec`): `k`
  escapes outward within its dynamic extent. No full re-entrant/resumable
  continuations (no generators-via-call/cc, no coroutines). See
  `docs/call_cc.md`.
- No bignums; integers wider than 60 bits silently become floats.
- No threads visible from user Lisp. The RESP server runs sharded
  reactors internally but those shards do not expose user-facing
  primitives. There is no `future`, `agent`, `go`, `spawn`.
- No HTTP client. `(shell "curl -s ...")`, FFI, or a native module.
- No keyword arguments — default parameter values + dict-passing cover it
  (see §3). A keyword-arg design that desugars purely in the reader may
  happen someday; nothing that touches the calling convention will.
- No `with-open-file` macro (use `open`/`close` explicitly in a `try/finally` block).
- No PCRE syntax: regex is POSIX **ERE** (see §4) — `[[:digit:]]`, not
  `\d`; no lazy quantifiers, lookaround, or `$1` backrefs. A PCRE binding
  belongs in a native module, not the core.

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

## 11. Semantics (pinned) — the trust-critical surface

These rules are part of the **stable surface** (see
[`docs/stability.md`](stability.md)). Each is pinned by a conformance assert in
`test.alc` (search "Trust-critical semantics conformance"), so it cannot drift
silently — changing one is a **breaking change** and must be recorded in
[`CHANGELOG.md`](../CHANGELOG.md).

**Truthiness.** `nil` is false, and so are the *empty/zero* values: `0`, `0.0`,
the empty string `""`, the empty list, and the empty vector. Every other value —
including non-zero numbers, non-empty strings/lists/vectors, symbols, keywords,
and procedures — is true. `(if X then else)` and `and`/`or`/`no`/`when`/`while`
all use this single predicate.

**Evaluation order.** A call `(f a b c)` evaluates `f`, then `a`, `b`, `c`
**left-to-right**, then applies. `let`/`let*` bindings evaluate top-to-bottom
(`let*` sees earlier bindings; parallel `let` does not). `do` returns its last
form's value.

**Errors are values.** An error is a first-class value that **propagates**
through ordinary calls (passing one as an operand re-raises it), but is inert
once bound to a parameter. The only operations guaranteed safe on a
possibly-error value are `error?`, `error-message`, and `try`. A stored error
**re-arms** when it is surfaced again (e.g. returned by `nth` from a list), so
inspect with `error?` before using a value that might be an error. `(try body
handler)` runs `handler` with the error value on a raise.

**Numeric tower & contagion.** Four kinds: fixnum (`int`), exact rational
(`rational`), bounded decimal (`decimal`), and float (`float`). Combining:
`int` promotes to whichever other kind it meets; `rational` + `float` →
`float` (the exact value is converted, lossy); but a `decimal` combines **only**
with integers — `decimal` + `float` and `decimal` + `rational` are **type
errors** (two exact systems, or exact-vs-inexact, never mix silently). **Integer
overflow is an error**, never a silent wrap or implicit float — use a float,
rational, or decimal explicitly.

**Infix.** A 3-element form whose head evaluates to a **non-callable** value,
`(A op B)`, is read as `(op A B)` when `op` is a binary operator/function; this
is value-based and collision-free, so a higher-order call like `(apply + xs)` —
whose head *is* a function — is never reinterpreted. Generalized: `(a f b)` →
`(f a b)` for any function `f`. The AST interpreter and the bytecode VM agree
byte-for-byte (enforced by `tools/equiv_sweep.alc`).

**Macro expansion.** Macros expand at compile time in a fresh environment; they
see builtins and quasiquote but not user-defined runtime bindings at expansion
time. Expansion is applied in both interpreted and compiled bodies.
