# Changelog

All notable changes to alcove (and its Adder dialect) are recorded here.
The format follows [Keep a Changelog](https://keepachangelog.com/), and the
project aims at [Semantic Versioning](https://semver.org/) — with the pre-1.0
caveats spelled out in [docs/stability.md](docs/stability.md).

## [Unreleased]

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

[Unreleased]: https://github.com/aallamaa/alcove/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/aallamaa/alcove/releases/tag/v0.2.0
[0.1.0]: https://github.com/aallamaa/alcove/releases/tag/v0.1.0
