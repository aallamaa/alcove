# Changelog

All notable changes to alcove (and its Adder dialect) are recorded here.
The format follows [Keep a Changelog](https://keepachangelog.com/), and the
project aims at [Semantic Versioning](https://semver.org/) — with the pre-1.0
caveats spelled out in [docs/stability.md](docs/stability.md).

## [Unreleased]

### Added
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

### Fixed
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
