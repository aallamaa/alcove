# Changelog

All notable changes to alcove (and its Adder dialect) are recorded here.
The format follows [Keep a Changelog](https://keepachangelog.com/), and the
project aims at [Semantic Versioning](https://semver.org/) — with the pre-1.0
caveats spelled out in [docs/stability.md](docs/stability.md).

## [Unreleased]

### Added
- **`(redis-wait-event! [ms])` — blocking consumption for keyspace
  watches.** Like `redis-next-event!` but sleeps on an eventfd until a
  producer emits: `ms > 0` waits up to ms milliseconds, `0`/nil/no-arg
  waits forever, negative never blocks; nil on timeout, Ctrl-C, or
  disabled watch. The mutator hot path pays one relaxed load when nobody
  waits; Dekker fences on both sides prevent lost wakeups. Main-thread
  only (enforced, like the other consumers).
- **RESP layer-2 keyspace watches — opt-in event stream for keyspace
  mutations.** `(redis-watch! flag)` toggles (returns prior state; disable
  frees queued events), `(redis-next-event!)` polls one
  `(:op set|del :key str [:new blob])` plist, `(redis-drain-events!)`
  returns all pending oldest-first, `(redis-watch-dropped)` reads and
  resets the overflow counter. Producers emit from any reactor thread
  through a bounded (65536) lock-free MPSC queue; consuming is
  main-thread-only and enforced (RESP callbacks are refused). Gate:
  `make resp-watch-test`. See docs/multithreading.md.
- **`lib/orm.adr` — object persistence for defclass models.** `orm/init`
  (path + load), `orm/save` (upsert with auto-ids via the checked-setter
  path — models declare `(id (optional Int) nil)`), `orm/get`, `orm/all`,
  `orm/query` (predicate scan), `orm/delete`, `orm/commit`; instances
  persist by class name (dump v5) with validators live after reload.
  Demo/test: `examples/orm/run.adr`.

## [0.4.0] — 2026-07-05

The class-system release: types become first-class runtime values and grow
into a complete, schema-enforced class system — `defclass` with inheritance,
generic-function methods, compound field types, defaults, and `super` — plus
comprehensions, cycle collection, weak references, watches/validators, error
introspection with a self-healing harness, and Adder dot syntax. The largest
release to date.

### Added
- **`defclass` phase 7 — compound field schemas, field defaults, `super`, and
  `(instance? T)`.** Field types may now be compound: `(optional TE)` also
  admits nil, `(list-of TE)` admits a proper list (or nil) of TE, and
  `(or TE TE …)` admits any arm — TEs nest, class names (including the class
  being defined) work inside compounds, and subsumption flows through them.
  Compounds are parsed as syntax at defclass time (unknown combinator/type
  names and arity mistakes are definition errors), checks stay **shallow**
  (they fire at construction/assignment of the field; mutating a nested
  container afterwards is not tracked), and compounds are not first-class
  types — `(is-a? x (list-of Int))` does not exist. A field clause may carry a
  **default**: `(name Type default-expr)` makes the constructor argument
  optional; the default is evaluated at **call time** (a `(list)` default
  yields a fresh list per construction), must pass the field's type check, and
  once a field (in inherited-then-own order) has a default all later fields
  must too. `(optional T)` does **not** imply a default. The `Name__class`
  schema records compound type expressions (as quoted sexprs) and defaults (as
  their unevaluated expression under a `"default"` marker). Inside `(:method …)`
  bodies of a class with a parent, **`(super name args…)`** resumes generic
  dispatch for `name` at the *defining* class's parent — rewritten at
  class-definition time (never inside quoted forms; an error in a parentless
  class; any parent generic may be named), so a grandchild running an inherited
  method cannot loop. New builtin **`(instance? T)`** returns a predicate
  closure `(fn (v) …)` testing `(is-a? v T)` — for `filter`, `match` guards
  `(? (instance? T))`, and other HOFs. (Optional `(param default)` parameters
  now also bind on the unified VM callee path, so constructors with defaults
  work identically across tiers.)
- **First-class type objects** — `Int`, `Float`, `String`, … are now real
  runtime values (`TAG_TYPE` immediates), not just JIT annotations. `(type x)`
  returns the type object for a value, `(type? v)` tests for one, `(type-name t)`
  gives its string name, and `(is-a? x t)` tests conformance (with `Number`,
  `List`, `Fn`, and user-class subsumption). A JIT hint code **is** a type id, so
  `(x Int)` and `(x :int)` are the same annotation. This **reserves 30 new
  names** (`Int Float String Bool Nil …`) — a breaking change for scripts that
  bound any of them as ordinary identifiers. The db dump format bumps to **v4**.
- **`(defclass Name (field Type)…)` — typed, dict-backed classes.** Generates a
  constructor, a predicate `Name?`, per-field getters `Name-field`, and checked
  setters `Name-field!`; the **type object itself is callable** as the
  constructor. Every field write (constructor, setter, or a raw `assoc!`) is
  schema-enforced through the mutation-validator layer, and field types may be
  builtin types or other user classes. Zero fields is `(defclass Name)`. Classes
  **cannot be redefined** in a session. `savedb`/`loaddb` persists instances by
  class **name** (dump **v5**): a load in a different class-definition order
  keeps the right identity, and when the class is defined before `loaddb` the
  schema validator is reattached so enforcement is fully restored (see Fixed).
- **`defclass` inheritance — `(:extends Parent)`.** As the first clause (at most
  one), a class inherits `Parent`'s fields: the constructor takes all
  inherited-then-own fields, `(is-a? child Parent)` holds (so `Parent`'s
  accessors and any class-typed field accept a child), `type-of` stays
  most-derived, and a child field may not shadow an inherited one. `Parent` must
  be a fully-defined class; the chain is transitive (grandchildren work) and its
  parent type object is recorded under `Name__class`'s `"parent"` key.
- **`defclass` generic methods — `(:method name (self args…) body…)`.** After the
  field clauses, method clauses define generic functions dispatched on the first
  argument's type: `(name inst …)`. A child inherits a parent method it does not
  override, a method registered for `Any` is the default, and several classes may
  share one generic name. Built on the multimethod machinery (`defmulti` is now
  **idempotent** so re-declaring a shared generic keeps its method table), with a
  new internal `__mm-lookup` resolver that walks the `:extends` chain then falls
  back to `Any`. `(defmethod name Type …)` keyed by a type object — user class or
  builtin — works the same way. There is no `super` yet. In Adder, write the
  block form (`defclass Dog:` / `  :extends Animal` / `  :method speak (self):`
  with the body indented under a trailing-colon method line).
- **Adder-only dot syntax — `a.field` attribute access.** A `.` **between
  identifier characters inside one token** is attribute sugar; the transpiler
  lowers it three ways, context-sensitively: a **read** `a.owner` →
  `(a "owner")` (chained `a.b.c` → `((a "b") "c")`); an **assignment-statement
  write** `a.owner = v` → `(assoc! a "owner" v)` (chained LHS
  `a.b.c = v` → `(assoc! (a "b") "c" v)`); and a **method call** — a dotted token
  glued to `(` — `a.speak(x y)` → `(speak a x y)` (`a.speak()` → `(speak a)`,
  chained receiver `a.b.speak(x)` → `(speak (a "b") x)`), where the last dotted
  segment is the generic-function name and the earlier segments are the receiver
  read-chain. Receivers are dynamic (instances are callable dicts, so reads use
  the existing container access and writes go through the schema validator).
  A standalone `.` (dotted pair), a leading/trailing dot (`.foo`, `foo.`), an
  empty segment (`a..b`), a digit-adjacent dot (floats: `1.5`, `100.0m`), and
  keywords/strings/dispatch tokens are all left untouched. This is **Adder-only**
  — the Alcove s-expression reader is unchanged.
- **`(gc-cycles)` — on-demand cycle collector** (new fragment `gc.h`).
  Reference cycles (only constructible through the mutating containers:
  `assoc!`, `push-right!`/`push-left!`, `vec-set!`) previously leaked with no
  recourse beyond a `(heap-stats)` audit. `(gc-cycles)` reclaims them
  explicitly via whole-arena trial deletion and returns the number of cells
  freed — with **zero cost on the alloc/refcount hot paths** (nothing is
  buffered or tagged; no `exp_t` layout change). Anything the collector
  cannot see (C/VM stack, envs, bytecode constants, the keyspace, other
  threads) keeps its target alive: unknown = kept. Cycles threaded through
  closure captures are conservatively retained. Per-thread arena; not
  concurrency-safe — never call it from a `--threads` RESP callback.
- **Adder/Alcove list-call sugar** — `(v i)` (Adder: `v i` / `v(i)`) on a
  list or deque is sugar for `(nth v i)`, matching the other callable
  containers (vector, dict, HAMT).
- **Self-healing harness: `lib/heal.adr` + `examples/heal/`** — the error
  introspection surface put to work: `heal/diagnose` bundles a failure's
  code/message/location/backtrace/form into a diagnosis value;
  `heal/attempt` tries candidate patches from a generator (an LLM in
  production, any list in tests) — parse + eval under `with-time-limit`,
  install under the broken name via `setf`, keep the first that passes the
  tests. The example heals a crashing function live: no restart, syntax
  errors are skipped values, runaways time out, and nothing survives that
  the tests reject.
- **`(error-codes)`** — every machine-readable error class as a list of
  `(code "description")` pairs, single-sourced from the same table as the
  codes themselves — the discoverability companion to `(error-code e)`
  dispatch.
- **Error introspection: `(error-location e)` / `(error-backtrace e)` /
  `(error-form e)`** — every error now records, at raise time, its source
  `(line col)` (both tiers; the VM maps its pc back to the line), a private
  copy of the raise-site call stack (a list of function names, innermost
  first, that survives the catch and later errors — unlike `(backtrace)`,
  which reads the current stack), and the failing form itself (the code as
  data on the AST tier; the enclosing procedure from a compiled frame).
  None of the inspectors re-raise. This is the introspection floor for
  self-healing harnesses: a handler can now see what failed, where, in
  which call context, and read the offending code.
- **Custom errors: `(raise 'code "msg")`** — raise a first-class error
  whose `(error-code e)` is YOUR symbol (class `'user-error`; the one-arg
  form `(raise "msg")` uses `'user-error` as the code). Propagates, is
  caught by `try`, and dispatches like any builtin error.
- **Validators: `(set-validator! obj fn)`** — the PRE-write veto layer
  complementing `watch!`. Before each structural mutation `fn` runs as
  `(fn obj op key new)` and rules on the proposed change: truthy allows,
  nil rejects with a standard error, and an error value (e.g. from
  `raise`) rejects with *that* error — so `(raise 'not-positive "score
  must be > 0")` surfaces intact as the mutator's result. On rejection
  the object is unchanged and watchers stay silent. One validator per
  object (replace by setting again, remove with nil); deletions validate
  too (dispatch on `op`); a validator mutating its own object is not
  re-validated.
- **Watches (Django-signals / Clojure add-watch flavor)** — `(watch! obj
  fn)` calls `(fn obj ev)` AFTER each structural mutation of a hash-map,
  deque, vector, or set (`assoc!`/`dissoc!`, `push-*!`/`pop-*!`,
  `vec-set!`, `set-add!`/`set-del!`); `ev` is a plist `(:op sym :key k
  :old o :new n)` with the overwritten/removed value captured as `:old`
  where the operation has one. Watchers stack (newest first); a watcher
  error propagates as the mutator's result while the write persists; a
  watcher mutating its own object does not re-fire; `(unwatch! obj)`
  removes all watchers, `(watched? obj)` tests. Zero cost on unwatched
  objects (one flag test in the mutator builtins — the same
  FLAG+TLS-registry pattern as weak references); the bulk numeric vec ops
  deliberately do not notify. Post-only by design — a veto layer
  (`set-validator!`) can come later.
- **Comprehension reader sugar** — `#l[…]`/`#g[…]` read as
  `(lfor …)`/`(gfor …)` and `#s{…}`/`#d{…}` as `(sfor …)`/`(dfor …)` in
  both dialects (the bracket mirrors the result shape, matching `#[`
  vector vs `#{` set), e.g. `#l[x (range 0 10) (odd x) (* x x)]`.
- **Comprehensions** — the Hy-flavored positional family, in both dialects:
  `(lfor x (range 0 10) (odd x) (* x x))` → `(1 9 25 49 81)`. `lfor` builds
  a list, `sfor` a set, `dfor` a hash-map (`(dfor var coll [pred] kexpr
  vexpr)`), and `gfor` the lazy generator twin (sugar over
  `map!`/`filter!`/`iter!` — nothing runs until pulled). The predicate
  clause is optional everywhere; body errors propagate; the loop variable
  respects the reserved-name check.
- **Weak references** — `(weak v)` returns a weak cell that does NOT keep
  `v` alive; `(weak-get w)` returns the target while it has strong
  references and `nil` after it is freed (including when freed by the
  `gc-cycles` sweep); `(weak? x)` is the predicate. The designed escape
  hatch for reference cycles: hold the back-pointer weakly and the cycle
  never forms. Zero cost on the refcount hot path — only objects that were
  ever weakly referenced pay a registry lookup, and only when freed.

### Changed
- **`iso` now compares deques deeply** (same length, elements `iso`-equal
  in order) — it was the one container compared by identity only, while
  vector/dict/set/HAMT already compared structurally. Cyclic input still
  bails out safely (`nil`) at the existing depth cap.

### Fixed
- **`defclass` instance persistence** — `savedb`/`loaddb` now round-trips class
  instances portably. The dump format bumps to **v5**: a user (`defclass`) type
  object is written by class **name** (builtin type ids keep the v4 encoding),
  so a load in a different class-definition order — or before the class is
  defined — no longer misidentifies the instance's type or degrades it to a
  plain dict. Loading before the class exists pre-registers a *claimable*,
  constructorless type entry (identity via `type-of` / `is-a?` survives, and a
  later `defclass` claims it); loading after the class is defined **reattaches
  the schema validator**, restoring checked mutation on the reloaded instance
  (nested instances too). Two-process gate: `make defclass-persist-test`.
  could combine a fresh value with a stale expiry from a previous write;
  both now live in one atomically-swapped entry, expiry is honored
  consistently across every read path (`GET`/`EXISTS`/`TTL`/`DEL`/`SET
  NX|XX`/`PERSIST`/`EXPIRE`/`DBSIZE`), and a live-server expiry gate
  (`make resp-expiry-test`) pins the semantics. TSan-clean under
  4-reactor contention.
- **REPL editing keys** — `C-a`/`C-k` are reasserted after inputrc, and
  `M-f`/`M-b` move over *language symbols* (`+`, `foo/bar`, `set!`) rather
  than readline's alnum words; covered by a pty-driven REPL test.
- **REPL echo of a cyclic container crashed the process** — `print_node` had
  no recursion guard, so merely evaluating `(assoc! d "self" d)` at the REPL
  overflowed the C stack printing the result. The printer is now depth-capped
  (256, prints `...` beyond), covering pairs, vectors, dicts/sets, deques,
  and HAMTs through the single entry point.
- **`(str cyclic)` and `(msgpack-encode cyclic)` crashed the process** — the
  same unbounded-recursion class as the printer. `str` now depth-caps with an
  ellipsis like `print_node`; `msgpack-encode` fails with an `illegal-value`
  error beyond `MP_MAX_DEPTH` (512) rather than ever emitting truncated
  bytes — mirroring the guard `json-encode` already had (`JS_MAX_DEPTH`).
- `adr.py` / `alc2adr.py` read stdin when no file is given and handle `[..]`
  bracket lambdas (previously looped forever).
- **The AST tier (`--interpret`) had three long-hidden tail-call bugs** —
  found by running the WHOLE suite under `--interpret`, which no gate did
  (the differentials sample forms; `test-all` now pins a full AST-tier
  suite run):
  1. `if`'s **else branch** was evaluated in the condition slot with the
     tail flag cleared, silently disabling TCO for every else-branch tail
     call — `(def g (n acc) (if (is n 0) acc (g ...)))` overflowed the C
     stack under `--interpret` while the VM looped fine.
  2. The cross-function tail trampoline jumped into a **multi-arity (defn)
     wrapper** without re-dispatching to a clause — the wrapper has no
     body, so this read through NULL and **segfaulted** (the crash that
     took the whole suite down).
  3. `make_tail_marker` stored raw-NULL nil arguments, so a nil flowing
     through a tail-call rebind (e.g. `(cdr last-pair)`) reported the
     parameter "unbound".
  The VM gained the mirror of fix 2: a defn wrapper in tail position now
  re-dispatches to its clause in `OP_TAIL_CALL`, so compiled cross-clause
  recursion keeps full TCO instead of paying one C frame per hop (deep
  `defn` recursion overflowed there too). Also: an empty/descending `for`
  range on the AST tier returned a "missing parameter" error instead of
  nil (the compiled tier's behavior).
- **Error classes were lost across compiled calls** — the VM blanket-raised
  `illegal-value` for every runtime error with the same *message* the AST
  tier uses, so the divergence was invisible to the output-comparing equiv
  sweep while `(error-code e)` dispatch silently broke for any error
  crossing a compiled function call (`div-by-zero` arrived as
  `illegal-value`, arity errors weren't `missing-parameter`, vector index
  errors weren't `index-out-of-range`). The VM now raises the same class
  the AST does at each site; tier-parity tests pin all four classes.
- **`defmacro` accepted reserved names as parameters** — `def`/`let`/`each`
  refuse them loudly, but `(defmacro m (var seq cond body) ...)` was
  accepted silently and the body's `,seq`/`,cond` unquotes resolved to the
  BUILTINS, splicing `#<builtin>` into every expansion. Now the same
  `CHECK_RESERVED_BIND` guard applies: "cannot bind reserved name 'seq' as
  a macro parameter".
- **`(odd x)` never evaluated its argument** — it read the raw form, so
  `(odd 1)` worked (a literal is already a fixnum) but `(odd k)` errored on
  every AST-path call, e.g. as a `when`/`if` condition inside `for`/`each`
  (the classic loop-accumulate idiom). It was the only builtin with the
  pattern; it now evaluates like every other applicative builtin, and the
  non-integer error message is specific (`odd: argument must be an integer`
  instead of `Illegal value in operation`).

- **Stream/Port File IO** — add buffered stream handle type `EXP_PORT` and builtins `open`/`close`/`write`/`eof?`/`port?`, and extend `read-line`/`flush` to accept an optional port.
- **Observability** — make a server/embedded deployment debuggable.
  - **Structured error codes** — `(error-code e)` returns an error's
    machine-readable class as a stable, prose-independent symbol
    (`'div-by-zero`, `'unbound-variable`, `'illegal-value`,
    `'missing-parameter`, `'index-out-of-range`, `'parse-error`, …), or `nil`
    for a non-error. Handlers can dispatch on it
    (`(if (is (error-code e) 'div-by-zero) …)`) without matching prose. Like
    `error-message`, it does not re-raise the error it inspects.
  - **Leveled logfmt logging** — `(log! LEVEL MSG key val …)` emits one
    `key=value` line to stderr (`ts=<ISO8601> level=<lvl> msg=<MSG> k=v …`)
    when `LEVEL` is at or above `(log-level)`; below threshold it writes
    nothing and returns `nil`. `(log-debug)`/`(log-info)`/`(log-warn)`/
    `(log-error)` convenience wrappers; `(log-level)` / `(set-log-level …)`
    read/set the threshold (atomic, shared across reactors). Always shipped —
    zero passive cost. (`log!`, not `log` — `log` is the natural logarithm.)
  - **Metrics (opt-in)** — counters/gauges with a small fixed registry:
    `(counter! NAME n?)`, `(gauge! NAME v)`, `(metric NAME)`, `(metrics)`
    snapshot. Compiled **only** with `-DALCOVE_METRICS` (`make
    alcove-with-metrics` / `adder-with-metrics`); the default build ships none
    of it, so the RESP hot path carries no shared-counter atomic bump. A metrics
    build also auto-instruments the RESP server: `resp.connections`,
    `resp.commands`, `resp.errors`. A new `obs-test` CI gate exercises both
    halves.

## [0.3.0] — 2026-06-18

The correctness-and-robustness arc: make the engine trustworthy for
mission-critical / embedded use. See [docs/stability.md](docs/stability.md) for
what is frozen vs experimental.

### Added
- **Exact numeric tower** — rationals (`int64/int64`, `1/3` literal,
  `(rational n d)`) and bounded base-10 decimals (`1.5m` literal,
  `(decimal "…")`), with strict contagion (the two exact systems and floats do
  not silently mix).
- **Infix application** — `(A op B)` evaluates as `(op A B)` when the head is a
  non-callable value; generalized to any function, `(a f b)` → `(f a b)`. Both
  tiers (AST and VM) agree, and the hint-gated form compiles to native prefix.
- **Adder chained call sugar** — `f(a)(b)` → `((f a) b)` (currying).
- **Programmable REPL** — `lib/repl.adr` toolkit: input/output hooks,
  configurable prompts (`*prompt-in*` / `*prompt-out*` / `*prompt-cont*`),
  Emacs-style `bind-key` line editing, per-dialect init files, and `(dialect)`.
- **Robustness builtins**
  - `(with-time-limit MS THUNK)` — bound a runaway loop / tail recursion with a
    catchable "interrupted: time limit exceeded" error.
  - `(with-memory-limit BYTES THUNK)` — bound runaway accumulation with a
    catchable "interrupted: memory limit exceeded" error.
  - `(heap-stats)` — per-thread arena stats (`:live :free :allocated :chunks`)
    for reference-cycle leak audits (refcounting does not reclaim cycles).
- **`*readline*`** — a global that is `t` when the build links the readline
  line-editor, `nil` otherwise (e.g. the static release binary), so programs can
  skip interactive-only features.

### Supply chain
- `tools/release.sh` now emits a minimal `SBOM.txt`, a `SHA256SUMS` manifest
  over every artifact, and detached GPG signatures when `ALCOVE_GPG_KEY` is set.

### Embedding
- **`ALCOVE_API_VERSION`** — an embedding API/ABI version macro. Native modules
  may export `int alcove_module_abi(void) { return ALCOVE_API_VERSION; }`; the
  host checks it at `(require)` time and **refuses an ABI mismatch** with a clear
  error instead of dlopen'ing a silently-incompatible binary. The single-engine-
  per-process constraint and the pre-1.0/not-yet-ABI-frozen status of the C embed
  API are now documented (`examples/embed/README.md`, `docs/stability.md`).

### Changed
- **Integer overflow is an error**, never a silent wrap or implicit float
  promotion — use a float, rational, or decimal explicitly.
- **Deep non-tail recursion raises a catchable error** instead of segfaulting
  the C stack.
- **Out-of-memory is recoverable.** A failed allocation on the eval path used to
  `exit()` the process; it now aborts the current top-level form with a surfaced
  out-of-memory error and the process + engine survive (the next form runs).
  It unwinds to the eval boundary, so it is *not* catchable by an in-program
  `(try …)` — it is a process-survival guarantee, not a normal condition. An
  unsafe `(alloc-fail-after N)` fault-injection builtin + an `oom-test` CI gate
  exercise it (normal and ASan builds). True OOM also no longer segfaults the
  exp_t arena (the bump-chunk allocation was an unchecked `calloc`).
- **`--threads N` (RESP) is marked EXPERIMENTAL** with a documented concurrency
  contract (see `docs/multithreading.md`): the keyspace is concurrency-safe;
  the user-command table is immutable-after-spawn (`redis-defcmd`/`undefcmd`
  refuse once multi-reactor serving is live); callbacks must be read-only
  w.r.t. Lisp globals.

### Fixed
- AST and bytecode VM now agree byte-for-byte on the numeric tower, on
  `NULL`/`nil` identity (`(cdr nil)`), and on `length` of a vector/blob/empty
  list — previously these diverged between interpreted and compiled code.
- Numeric-tower error paths no longer use-after-free the call form (a cumulative
  double-free that aborted after repeated tower-type errors).
- A builtin held in a variable can be tail-called; calling a non-callable,
  non-infix head is a consistent error in both tiers.
- Macros expand in compiled bodies; `def name()` parses with no space.

### Fixed (concurrency)
- **`--threads` startup data races** eliminated. A ThreadSanitizer harness
  (`make resp-tsan`, a 4-reactor server under concurrent load) found that each
  reactor ran the shared server setup concurrently — racing writes to
  `resp_active_port`, the command table, and the lazily-created keyspace. Shared
  setup now runs once before the reactor pool spawns (published with a
  happens-before), and `resp_kv_ensure` reads the keyspace pointer atomically.
  The lock-free keyspace algorithm itself was already sound.

### Security / Hardening (CI gates)
- The hot path (evaluator / VM / JIT) now runs the full suite under
  **ASan+UBSan** in CI (previously only isolated unit harnesses were sanitized).
- A **generative whole-program fuzzer** (`tools/eval_fuzz.py`) checks random
  terminating programs are byte-identical AST≡VM under ASan; it found the
  `length` divergence and the tower use-after-free above.
- `json-test`, `json-fuzz`, and the MPSC queue under **ThreadSanitizer** are now
  CI gates; a **code-coverage signal** (`make coverage`) tracks hot-path reach.

## [0.2.0] — 2026-06

### Added
- Scripting surface: `*args*` + shebang scripts, environment access, shell-out,
  filesystem ops, JSON, and regex; a broader standard library.
- Adder-first ergonomics across the docs, site, and examples.

## [0.1.0] — 2026-06

### Added
- First public release: the embeddable JIT'd numeric Lisp (Alcove) and its
  Python-flavored dialect (Adder), with three prebuilt artifacts and an Adder
  playground.

[Unreleased]: https://github.com/aallamaa/alcove/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/aallamaa/alcove/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/aallamaa/alcove/releases/tag/v0.2.0
[0.1.0]: https://github.com/aallamaa/alcove/releases/tag/v0.1.0
