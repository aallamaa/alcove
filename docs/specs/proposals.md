# Language feature proposals — discovered while building examples/mario

These are concrete language/runtime additions whose absence forced
either a workaround in `mario.alc`, a new C shim entry point, or
verbose alcove-side scaffolding. Each spec includes motivation, the
proposed surface, sketch of behavior, and rough implementation notes.

The bug ticket `docs/bugs/alcove-001-for-body-tail-call.md` covers a
correctness issue separately. This document covers *capability gaps*.

---

## Spec 1 — `string-buf` / `string-set!` / `string-len` (mutable string buffers)

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

If only one or two were to land:

1. **Spec 6 (`try` / `catch`)** — highest leverage. Once games can
   handle FFI errors, every other shim integration becomes safer.
2. **Spec 2 (`now-ms` / `sleep-ms`)** — every game / animation
   reinvents these. Trivial to ship.
3. **Spec 3 (`platform` / `dylib-suffix`)** + **Spec 4
   (`string-concat`)** together — they unblock truly portable FFI
   path construction.
4. **Spec 1 (`string-buf`)** and **Spec 5 (`input-state`
   pattern)** — nice ergonomic wins; not blocking.
5. **Spec 7 (`,@`)** — useful but rarely needed in early-day code.
