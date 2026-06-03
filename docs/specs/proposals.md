# Language feature proposals — discovered while building examples/mario

These are concrete language/runtime additions whose absence forced
either a workaround in `mario.alc`, a new C shim entry point, or
verbose alcove-side scaffolding. Each spec includes motivation, the
proposed surface, sketch of behavior, and rough implementation notes.

The bug ticket `docs/bugs/alcove-001-for-body-tail-call.md` covers a
correctness issue separately. This document covers *capability gaps*.

---

## Status at a glance (updated 2026-06-03)

| Spec | Feature | Status |
|------|---------|--------|
| 6 | `try` / handler / `finally` | ✅ **shipped** (surface differs — see below) |
| 7 | `,@` unquote-splicing | ✅ **shipped** |
| 4 | `string-concat` / `string-append` / `format` | ✅ **shipped** (`format` = `str`) |
| 3 | `(platform)` / `(arch)` / `(dylib-suffix)` | ✅ **shipped** |
| 2 | `sleep-ms` / `now-ms` / timing | ✅ **shipped** (`time-of-day` = `(/ (time) 1000)`) |
| 5 | input-state pattern / `bit-and` | ✅ **shipped** — `bit-and`/`band`; pattern is doc-only |
| 1 | `string-buf` & friends | ✅ **shipped** (`string-len` = `length`/`count`) |

All seven specs are now satisfied (some via aliases / equivalent existing
builtins, noted per-spec). Per-spec details inline, each headed by a
**Status:** line.

---

## Spec 1 — `string-buf` / `string-set!` / `string-len` (mutable string buffers)

**Status: ✅ shipped (2026-06-03).** `string-buf` / `string-set!` /
`string-fill!` / `string-copy!` are bound (in `alcove.c`, near `substr`).
Indices are **codepoint-based** — consistent with `length`, `substr`,
and `(= (s i) ch)` — and each op rebuilds the byte buffer so a
replacement codepoint may change width safely (`dst==src` aliasing in
`string-copy!` is safe; copy clamps at dst's end, never grows it). For
ASCII buffers codepoint == byte, so an FFI `char*` round-trips directly.
`string-len` was **not** added: `(length s)` already gives codepoint
count and `(count s)` gives byte count — together they cover it.

```lisp
(= s (string-buf 8))           ; → "        " (8 spaces)
(string-set! s 0 #\H)          ; → "H       "
(string-copy! s 1 "i!")        ; → "Hi!     "
```

**Motivation:** alcove has mutable strings already, but no portable way
to *create* a fresh fixed-size buffer of a given length. Writing
`(make-string 64 #\space)` doesn't work; the workaround in the Mario
shim was to expose a per-text-render API (`gfx_text_set` + `gfx_text`)
because pushing a freshly-built alcove string through libffi was
non-obvious. A first-class string buffer would simplify any code path
that needs to format text, build a JSON payload, render a HUD, etc.

**Proposed surface:**

```lisp
(string-buf len)             ; → fresh mutable string of `len` 0x20 (' ') bytes
(string-buf len init)        ; → fresh mutable string of `len` `init` bytes
(string-len s)               ; → length of s in bytes (alias of (len s))
(string-set! s i ch)         ; mutate s[i] := ch (char). Errors out of range.
(string-fill! s ch)          ; fill all bytes of s with ch
(string-copy! dst dst-i src) ; copy src into dst starting at dst-i; clamps at dst end
```

**Why not just use `vec`?** Vectors hold `exp_t*` per cell; strings
are byte-packed. For long buffers, the size difference is 8x on
64-bit. More importantly, FFI shared libraries take `char*`, not
arrays of tagged values — passing a string through `(ffi-fn ...
"string" ...)` works today; passing a vector does not.

**Implementation notes:**

- Mutable strings already exist (used by `arkanoid.alc`). The
  underlying `EXP_STRING` payload is a `char*` plus length — both
  fields exist.
- `string-buf` allocates a fresh `EXP_STRING` whose payload is a
  malloc'd buffer. The cmd takes the form `(string-buf n [init])`.
- The init byte default is `' '` (space) so a freshly-built string
  printed for debugging shows whitespace, not random non-printables.
- `string-set!` should bounds-check; out-of-range is an error, not
  silent.
- Today, `(= (s i) ch)` already works for in-place mutation (see
  arkanoid). `string-set!` is a clearer name with explicit error
  semantics.

**Risk:** none — purely additive.

---

## Spec 2 — `now-ms` / `sleep-ms` / `time-of-day` builtins

**Status: ✅ shipped.** `sleep-ms` ✅ (web-aware: yields to the browser
via Asyncify under WASM). `now-ms` ✅ added (2026-06-03) —
`CLOCK_MONOTONIC` milliseconds (arbitrary epoch, unaffected by NTP/DST;
use differences for pacing). `time-of-day` was **not** added as a
separate builtin: `(time)` already returns wall-clock **µs** since the
Unix epoch, so `time-of-day` is exactly `(/ (time) 1000)`.

**Motivation:** every example so far reaches for `usleep` over libc
FFI to pace frames. The Mario shim re-exposes `clock_gettime` as
`gfx_now_ms`. Both are universal, OS-portable, and already wrapped by
SDL — but the language could promote them to first-class builtins so
games and scripts don't need to reinvent the platform layer.

**Proposed surface:**

```lisp
(now-ms)                     ; → fixnum: ms since some monotonic epoch
(sleep-ms n)                 ; sleep n ms; n<=0 returns immediately
(time-of-day)                ; → fixnum: ms since Unix epoch (wall clock)
```

**Why both monotonic and wall-clock?** Frame pacing wants monotonic
(unaffected by NTP / DST adjustments). Logging and replay timestamps
want wall-clock. Conflating them surprises both audiences.

**Implementation notes:**

- POSIX: `clock_gettime(CLOCK_MONOTONIC, ...)` and `gettimeofday(...)`
  / `clock_gettime(CLOCK_REALTIME, ...)`.
- Windows: not currently supported, so out of scope.
- `now-ms` should return a fixnum that fits comfortably in alcove's
  61-bit fixnum range — milliseconds since boot, mod 2^59, will not
  wrap for 18,000+ years. Or, since the existing `(time)` form prints
  elapsed wall time, expose its underlying clock.
- `sleep-ms` semantics: best-effort. `nanosleep` may return early on
  a signal; that's acceptable — frame loops re-time on the next
  iteration.

**Risk:** trivial — these are stable POSIX APIs. The only design
question is the name (alcove uses kebab-case, so `now-ms` over
`nowMs`).

---

## Spec 3 — `(platform)` and `(arch)` queries

**Status: ✅ shipped (2026-06-03).** `(platform)` → symbol
`web | darwin | linux | freebsd | unknown`; `(arch)` → symbol
`arm64 | amd64 | x86 | wasm | unknown`; `(dylib-suffix)` → string
`".dylib"` (macOS) / `".wasm"` (web) / `".so"` (else). Values come from
compile-time macros (`__APPLE__`, `__linux__`, `__aarch64__`,
`__x86_64__`, `ALCOVE_WEB`, …). Implemented exactly as the spec sketched
— see `platformcmd`/`archcmd`/`dylibsuffixcmd` in `builtins_stdlib.h`.
The example below works verbatim.

**Motivation:** the Mario Makefile has to switch on `uname -s` to
choose between `.dylib` and `.so` for the shim, and the alcove side
hard-codes `./libmariogfx.so` (relying on a Darwin symlink). A
runtime query would let portable scripts pick the right library name
without a Makefile incantation.

**Proposed surface:**

```lisp
(platform)                   ; → 'darwin | 'linux | 'freebsd | 'unknown
(arch)                       ; → 'arm64 | 'amd64 | 'x86 | 'unknown
(dylib-suffix)               ; → ".dylib" on macOS, ".so" elsewhere
```

**Behavior:** values come from compile-time constants
(`__APPLE__`, `__linux__`, `__aarch64__`, `__x86_64__`). They're stable
for the lifetime of the binary, so caching at startup is fine.

**Use case:**

```lisp
(= LIB (string-concat "./libmariogfx" (dylib-suffix)))
(= gfx-init (ffi-fn LIB "gfx_init" "int" "int" "int"))
```

(Pairs naturally with Spec 4.)

**Implementation notes:** ~15 lines. Static dispatch on `#ifdef`
inside the alcove builtin. No reason not to add this if Spec 4 also
ships.

**Risk:** none.

---

## Spec 4 — `string-concat` / `string-append`

**Status: ✅ shipped.** `string-append` has long existed; `string-concat`
is registered as an alias of the same builtin (`stringappendcmd`), so
both names work and the FFI-path example below runs verbatim. `format`
✅ is also satisfied: `(str x …)` already returns the printed form using
the same formatter as `pr` (`(str 42)` → `"42"`, `(str '(1 2 3))` →
`"(1 2 3)"`), and `format` is now registered as an alias of `str`. For
templated output use `fmt` (`(fmt "{} + {}" 1 2)`).

**Motivation:** `mario.alc` builds the FFI library path as a literal,
hard-coded string. With Spec 3, the natural form is
`(string-concat "./libmariogfx" (dylib-suffix))` — but alcove has no
`string-concat`. The current workaround is to use the same hard-coded
name on both platforms (working only because the Makefile creates a
symlink on macOS).

`pr` and `prn` already format and *print* arbitrary values; the same
formatter could power a `format` or `string-concat` that returns the
result instead of writing it.

**Proposed surface:**

```lisp
(string-concat s1 s2 ...)    ; concatenate strings, return new fresh string
(string-append s1 s2 ...)    ; alias of string-concat (Scheme name)
(format value)               ; → string representation of value
                              ;   (same formatter as `pr`, but returns)
```

**Implementation notes:**

- `string-concat` over `n` strings: sum lengths, allocate, memcpy.
- `format` is more useful — alcove already has the printer in
  `prnexp` / similar; refactor to optionally write into a stream
  abstraction, then provide a "string stream" that grows a buffer.
- `format` makes Spec 1's `string-buf` less critical, since you can
  build strings by formatting / concatenating instead of mutating.

**Risk:** low — the formatting code already exists; this exposes it
in return-rather-than-print mode.

---

## Spec 5 — `(input-state)` and structured key polling

**Status: ✅ shipped (runtime side).** This was a documentation-and-pattern
proposal, not a runtime change. Its prerequisite `bit-and` ✅ exists
(`(bit-and 6 3)` → `2`), and the `band` alias asked for here is now
registered (alongside the existing `&`). The recommended input bitmask
*pattern* itself is doc-only and can be written into `alcove-language.md`
when another input-heavy example lands.

**Motivation:** every shim that handles input ends up wrapping
SDL_PollEvent and exposing one zero-arg getter per key
(`gfx_left`, `gfx_right`, `gfx_jump`, ...). With more keys, this
becomes tedious. Either a single bitmask getter or a structured
`hash` return would be ergonomic.

This is **a shim convention**, not a language feature — but
documenting a recommended pattern in `alcove-language.md` would save
each game from reinventing it.

**Recommended pattern (no runtime change):**

```c
// in shim:
int input_keys(void);   // returns bitmask: 1=left 2=right 4=jump 8=quit ...
```

```lisp
;; in alcove:
(= keys (input-keys))
(when (> (band keys KEY-LEFT)  0) ...)
(when (> (band keys KEY-RIGHT) 0) ...)
```

This requires a `band` (bitwise AND) builtin. alcove has `bit-and`
already — confirm and document.

**Risk:** zero — this is a documentation / pattern proposal, not a
runtime change.

---

## Spec 6 — `try` / `catch` for FFI failure isolation

**Status: ✅ shipped — and exceeded — but the surface differs.** Landed
as `(try body handler [finally])` where `handler` is a **function**
applied to the error value, *not* the `(catch e …)` keyword form
sketched below. It went further than proposed: it *adds* a `finally`
clause (the spec said "no finally — keep it minimal"), is call/cc-safe,
and survives deep tail recursion (heap handler stack, commit `8678db6`).
Error accessors are `error?` (predicate) and `error-message` — not the
proposed `errorp`/`err-code`/`err-msg`. There is no `throw`/`catch`
keyword and no re-raise primitive (return the error value to propagate).
Example of the *actual* surface:

```lisp
(try (/ 1 0)
     (fn (e) (error-message e))     ; → "Illegal Division by 0"
     (prn "cleanup always runs"))   ; optional finally
```

**Motivation:** if the user runs `mario.alc` without building the
shim, `(ffi-fn "./libmariogfx.so" ...)` fails and brings down the
script. There's no way to recover gracefully (e.g. fall back to a
text mode, or print a specific install hint). A minimal exception
form would let example games handle this without `(quit)`-from-shim.

**Proposed surface:**

```lisp
(try expr
  (catch e handler-expr))    ; e is bound to the error in handler
```

Result is `expr`'s value if no error, else `handler-expr`'s value.

**Behavior:**

- `e` is bound to an error value with at least `(err-code e)` and
  `(err-msg e)` accessors.
- Re-raising: `(throw e)` (or simply not handling — control unwinds
  past the catch).
- No `finally` — keep it minimal.

**Implementation notes:** alcove already has an internal error
sentinel (`iserror(arg)` checks fire in `defcmd`, `disasmcmd`, etc.).
Wire it to a try/catch frame on the eval stack.

**Risk:** moderate — touches eval and the bytecode VM's error
propagation. But a clear win for reusability.

---

## Spec 7 — `defmacro` `,@` (unquote-splicing)

**Status: ✅ shipped.** Both the `,@xs` reader shorthand and the
`(unquote-splicing xs)` long form work inside quasiquote:
`` (= xs (list 2 3)) `(1 ,@xs 4) `` → `(1 2 3 4)`.

**Motivation:** documented in `alcove-language.md` as missing.
Variadic macros currently have to manually `cons` / `list` to splice
arguments. Adding `,@` is a parser/macro-expander change without
runtime cost.

**Proposed surface:**

```lisp
(defmacro my-and (a . rest)
  (if (null? rest)
      a
      `(if ,a (my-and ,@rest) nil)))   ; ,@rest splices the tail
```

**Implementation notes:**

- The reader needs to parse `,@` as a distinct token (different from
  `,`).
- The quasiquote expander treats `,@x` like `,x` but instead of
  embedding the value, splices its elements into the surrounding
  list.
- alcove's existing quasiquote logic is a good template; just add
  the splice case.

**Risk:** low. Well-understood feature with reference implementations
in every Lisp.

---

## Priority ordering (from build experience)

The original ranking, now scored against what shipped — **all complete**:

1. **Spec 6 (`try` + finally)** — ✅ done (shipped beyond the sketch).
2. **Spec 2 (`sleep-ms` / `now-ms`)** — ✅ done.
3. **Spec 3 (`platform` / `arch` / `dylib-suffix`)** + **Spec 4
   (`string-concat` / `format`)** — ✅ done; portable FFI-path
   construction unblocked.
4. **Spec 1 (`string-buf` family)** and **Spec 5 (`bit-and`/`band`)** —
   ✅ done.
5. **Spec 7 (`,@`)** — ✅ done.

**Remaining open items:** none from this document. The only deliberate
non-additions are equivalents that already exist — `time-of-day`
(`(/ (time) 1000)`), `string-len` (`length`/`count`) — and the doc-only
input-bitmask pattern for `alcove-language.md`. Forward-looking language
direction now lives in the language-evolution audit, not here.
