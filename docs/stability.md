# Stability & versioning

This document states what you can depend on across alcove/Adder releases, what
is still moving, and how changes are communicated. It is the contract behind
"can I bet mission-critical work on this?".

## Versioning

alcove follows [Semantic Versioning](https://semver.org/) with an explicit
**pre-1.0 caveat**: while the version is `0.x`, the language is still
converging, so a **minor** bump (`0.2 → 0.3`) may include a breaking change to a
surface marked *unstable* below. Every such change is called out under
**Changed**/**Breaking** in [CHANGELOG.md](../CHANGELOG.md). Patch bumps
(`0.2.0 → 0.2.1`) are always backward-compatible.

At **1.0** the *stable surface* below is frozen under normal semver (no breaking
changes without a major bump), and the dialect surface (Adder syntax) is
finalized.

The running version is `ALCOVE_VERSION` in `alcove.h`; `tools/release.sh` reads
it for artifact names and tags.

## Stable surface (depend on it; changes are breaking and announced)

- **The core evaluation model** — lexical scope, the special forms
  (`def`/`fn`/`if`/`let`/`do`/`and`/`or`/`cond`/`case`/`while`/…), proper tail
  calls, and macro expansion semantics.
- **The error-as-value model** — errors are first-class values that propagate
  through normal calls; `error?` / `error-message` / `try` are the safe
  operations on a possibly-error value. (Pinned with conformance asserts; see
  the semantics appendix of `docs/alcove-language.md`.)
- **The numeric tower** — fixnums, exact rationals, bounded decimals, and
  floats, with the documented strict contagion rules and integer-overflow-is-an-
  error policy.
- **Core builtins and their contracts** — arithmetic/comparison, list/vector/
  string/blob ops, the sequence protocol, `map`/`filter`/`reduce`, etc.
- **Truthiness** — `nil` is falsey, and so are the *empty/zero* values: `0`,
  `0.0`, the empty string `""`, the empty list, and the empty vector. Every
  non-empty, non-zero value is truthy.
- **The RESP keyspace commands** and the on-disk dump format version (migrations
  are handled with explicit format-version bumps).

## Unstable / experimental surface (may change in a 0.x minor)

- **`--threads N` (multi-reactor RESP)** — EXPERIMENTAL. See the concurrency
  contract in `docs/multithreading.md`. Single-reactor (`N=1`) is stable.
- **The C embedding API/ABI** — usable, but not yet versioned for ABI
  compatibility (see `examples/embed/`). Native modules must be built against
  the same source revision.
- **JIT internals and `:type` hints** — the *behavior* is stable (results match
  the VM byte-for-byte, enforced by `make jit-fuzz`); which shapes JIT and the
  hint syntax may evolve.
- **Adder surface syntax** — converging toward a 1.0 freeze; recent additions
  (infix, chained-call sugar) may still be refined.
- **The programmable-REPL hooks** (`lib/repl.adr`, `*prompt-*`, `bind-key`).

## Deprecation policy

A feature slated for removal is first **deprecated**: it keeps working, is
marked **Deprecated** in the CHANGELOG, and (where practical) emits a one-time
notice. It is removed no sooner than the **next minor release** after the one
that deprecated it, and the removal is listed under **Breaking**. Aliases kept
for compatibility (e.g. `band`/`bit-and`) are documented as such.

## How correctness is defended

Trust rests on differential gates that run in CI on every push:
- **AST ≡ VM** — `tools/equiv_sweep.alc` plus the generative `tools/eval_fuzz.py`
  (random programs, byte-identical, under ASan).
- **VM ≡ JIT** — `make jit-fuzz` (byte-identical machine code vs bytecode).
- **Hot path under ASan+UBSan** — the full `test.alc`/`test.adr` suite.
- **ThreadSanitizer** on the concurrency primitive, libFuzzer on the reader /
  transpiler / msgpack / JSON, and a coverage signal.

See [CHANGELOG.md](../CHANGELOG.md) for the per-release record.
