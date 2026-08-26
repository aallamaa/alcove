# Roadmap

Drafted 2026-07-07 (agy-reviewed), after the v0.4.0 class-system release and
the landing of the keyspace-watch layer (watches, blocking wait, gates,
docs). Ordered by theme; within a theme, by leverage. Each item carries
effort (S/M/L), risk, and acceptance criteria where stalling is a danger.
The identity this roadmap serves, per the 2026-06 language-evolution audit:
**an embeddable, JIT-compiled numeric Lisp** — the strategic bet is
types-for-JIT and float shapes, not language surface area.

## 0. Release housekeeping — DONE 2026-07-10

1. ~~CHANGELOG compare links~~ (59875f3).
2. ~~`--version` build variant~~ (59875f3 — arch/mono-threads/jit/ffi/
   readline/metrics/fixnum width).
3. ~~v0.5.0 cut & published~~ (089c99e + tag; 3 tarballs + SBOM +
   SHA256SUMS on GitHub; unsigned — ALCOVE_GPG_KEY wasn't set in the
   release environment, sign next time or re-attach .asc files).
Also done: the §3 warn-on-ignored `-R --threads` slice (59875f3).

## 1. Performance / JIT — the strategic bet

- ~~**Benchmarks as regression gates**~~ DONE 2026-07-14 (be99be9):
  `make bench-gate` + CI job — interleaved baseline-vs-HEAD min-of-15,
  >15% or checksum divergence fails; 6 kernels incl. the NLC path.
- **Types-for-JIT payoff** — slices 1 & 2 DONE.
  - 2026-07-14 (9800696): mixed int/float hinted kernels compile on
    amd64+arm64 (~22x on the canonical `:f64`+`:int` loop; tagged int
    slots, overflow deopts, strict back-edge classes; fixed a latent
    arm64 silent-wrap).
  - 2026-07-25 (351e4f6): generative coverage — `numloop-hinted` and
    `numloop-overflow` jit-fuzz generators, 0 -> 100%. Mutation-tested
    (removing the overflow `jo` makes the suite fail).
  - 2026-07-25 (41ccc7b): **the int register pool was the real cap.**
    9800696's "4 int slots" was not reachable: a slot-vs-SLOT compare
    like `(< i n)` materializes both operands as temps, so a 4-wide GPR
    pool left room for only a counter+limit. Widened to 8 with r8-r11
    (caller-saved → no prologue change; GPR encoders gained REX.R/.B,
    byte-identical for r0-r7). 48x on a 3-int-slot kernel (gate-nlmi
    pins it). New ceiling ~4 int slots — a 5th wants 10 registers.
    arm64 matched to 8 (x1-x7 + x11) in 95e1326 — the new arm64 lane
    caught the divergence on the first push, which is what it exists
    for. Pools are kept EQUAL on purpose: the corpus `assert-jits` on
    these kernels and runs on both arches.
  - 2026-07-25: **guard elision RULED OUT** — see docs/jit-type-hints.md.
    The premise was false: entry guards are emitted above `loop_top`, so
    they cost per CALL, not per iteration. Measured at ~4.5ns per guard
    per call (~4% of per-call cost, itself amortized over the trip
    count). Eliding them would turn a wrong hint from a deopt into
    silent type confusion in generated code. Revisit only if a profile
    shows guards >5% of a real workload.
  REMAINING in this bet: hints seeding the fixed SHAPES (not just
  numloop), and a `:f64`-hinted path through mat/vec loops. The latter
  is now scoped: `(vec-ref v i)` compiles to a dedicated OP_VEC_REF (not
  a generic call), `:vec` already parses to a Vector hint, and the
  vector slot is loop-INVARIANT — so the kind guard (VEC_KIND_F64) and
  the base pointer hoist to entry, leaving only a bounds-check deopt in
  the loop. Needs a third slot class (~2 GPRs per vector: base + len)
  and both emitters. Measured opportunity: a hand-written hinted sum
  over a 1M f64 vector runs 47ms in the VM today and does not JIT.
- **Numeric-loop bytecode→native compiler** (L, high risk, started
  2026-06). The float_series π shape shipped (47× CPython); generalize to
  compiling whole numeric loops instead of matched shapes. Depends on hints
  for typing loop variables (`:f64` disambiguated Mandelbrot).
- **SLOT_IS_FIX** (M, high risk — deferred from the 2026-05 perf audit with
  a measured 24% win / 76× microbench; risky JIT byte-offset rework). Do it
  AFTER hints land — hint-driven codegen may subsume part of the win.
  Re-measure before starting.
- **GEN_BUMP de-coarsening** (S/M, high risk — cache invalidation in a JIT
  is never low-risk). Profile first to confirm it still shows after the
  above; acceptance: a workload where the coarse bump measurably hurts.

## 2. Runtime / memory

- ~~**Closure-cycle collection**~~ DONE 2026-08-06 (678253e): envs are
  collector NODES discovered through lambda captures (they cannot be
  enumerated — destroy_env abandons non-top arena slots with no liveness
  bit). Reproducer returns to baseline exactly, 3 cells/cycle, idempotent;
  ASan green. Two traps recorded in docs/closure_env_cycle_leak.md: a
  COMPILED lambda owns its params and constant pool through bc, and
  over-reporting an edge frees LIVE objects (so a lambda must not be charged
  for bindings its env merely shares).
- ~~**Weak/watch/validator/gc free-path interaction audit**~~ DONE
  2026-08-06 (7cdb2e0). The interesting result: a watcher whose closure env
  binds its own target is an EXTERNAL root (the registry holds it, and the
  collector cannot see registry refs), so that cycle is kept — asserted in
  both directions, 0 collected while watched and >0 after unwatch!.

## 3. Server / concurrency

- ~~**Warn when `-R` ignores `--threads`**~~ DONE (59875f3): prints
  "--threads N ignored — -R is single-reactor; use -r for the reactor pool".
- ~~**Thread-safety contract design for a `-R` reactor pool**~~ DONE
  2026-07-14 (509cc83): docs/multithreading.md "-R reactor pool:
  thread-safety contract" — stop-the-world park/unpark on REPL global
  mutation, with the TLS-allocator and epoch corners ruled. The
  IMPLEMENTATION below is still open.
  No implementation in this item.
- **`-R --threads N` pool implementation** (L, high risk — retrofitting a
  single-reactor REPL onto the lock-free keyspace + pool). Only after the
  contract design lands. Payoff: live consumers become real —
  `redis-wait-event!` woken by network clients in-process; live drains
  while serving. Acceptance: the resp-watch gate grows a true cross-thread
  wake phase; resp-tsan green.
- ~~**Fuzz the network surface**~~ DONE 2026-08-06 (857b09e):
  tools/resp_fuzz.py drives a real socket with malformed frames, lying
  lengths, truncation at every byte boundary, split writes, pipelines and
  binary keys; oracle is "still alive and answers PING". CI runs it against
  an ASan+UBSan server (make resp-fuzz-asan, 400 cases). resp-tsan now also
  runs with the watch ENABLED so 4 reactors multi-produce into the event
  queue. NOTE the other half of this line is structurally impossible, not
  untested: the MPSC queue permits one consumer, so watch enable/disable/
  drain are main-thread-only and refused from a callback.
- ~~**Expose `lfkv_cas` to Lisp**~~ DONE 2026-08-06 (857b09e):
  `(redis-cas k expected new)`, nil `new` deletes. lfkv_cas compares by
  POINTER (which a Lisp caller cannot supply), so the builtin reads the live
  pointer, compares BY VALUE, then CASes on that exact pointer — an
  equal-but-different racing write makes it fail, which is what a CAS is
  for.
- **Watch layer follow-ons, on demand only** (S each): key-pattern
  filtering, `:old` values, expiry-distinct events. Scoped out by design;
  add when a real consumer (ORM change-feeds, swarm blackboard) asks.

## 4. Platforms / distribution

- ~~**arm64 CI lane**~~ DONE 2026-07-25 (8900853): `make arm64-test` +
  CI job — cross-compile, whole corpus under qemu-aarch64 (gate is "0
  failed", not a fixed total: no cross libffi), plus an assertion that
  the hinted numloop shapes actually JIT there and match amd64
  bit-for-bit (a corpus-only check would pass with the JIT declining
  everything).
- ~~**wasm** delta note~~ DONE 2026-08-06 (ba5c958) — and it was not a doc
  edit. The fixnum width is pointer-width-3, i.e. **29** bits on wasm32 (not
  31), and make_integer range-checked literals against a hardcoded 2^60, so
  every literal between 2^28 and 2^60 was silently TRUNCATED in the browser
  (1073741823 read back as -1). Now checked with FIX_FITS. The wasm section
  of docs/alcove-language.md names the fixnum range as the fourth, semantic
  difference.
- ~~**Packaging**~~ DONE 2026-08-06 (796a662): `make install-dev` ships the
  engine sources + a GENERATED alcove.pc (generated so its Libs match the
  build's autodetection), and docs/embedding.md explains why --cflags is an
  include path and not a link line: the unity build means there is no
  libalcove.so. Verified by building examples/embed/host.c AND the doc's
  hello-world against a staged install through pkg-config alone. A homebrew
  formula is still open, and still waits on a real consumer.

## 5. Developer experience / tooling

- ~~**REPL class-redefinition escape hatch**~~ DONE 2026-08-06 (49a2557):
  `--dev` re-registers over the existing entry KEEPING its type id, so
  instances built before the change stay is-a?/type-of correct and the new
  schema's validator is enforced. make dev-redefine-test asserts both halves
  (a normal build still refuses; --dev redefines and revalidates).
- ~~**agy MCP bridge hardening**~~ CLOSED 2026-08-06 — no longer reproduces,
  so nothing to harden. Retested against agy 1.1.10 through the bridge's
  exact subprocess path: a **59KB** prompt returns the right answer in 14.6s
  with rc=0 (41KB: 8.2s), past the 50KB acceptance bar. The exit-1 and the
  ~10KB flakiness were agy 1.0.16 bugs. (claude-to-agy is a third-party repo;
  patching it was never ours to do. `fastmcp` is not installed here, so the
  MCP server itself still cannot start — the CLI path is what works.)
- ~~**Book**: chapters for the watch layer + `redis-wait-event!` and the ORM~~
  DONE 2026-07-25 (1c2b6d3): §26.7 keyspace watches, §26.8 lib/orm, §31.4
  type hints + the numeric-loop JIT. The book has its own CI gate (every
  exec block runs) plus a weekly drift cron — check it after language
  changes.
- No parametric first-class types ((list-of Int) as a value), no CLOS
  call-next-method, no keyword args.
- No speculative language surface: features enter through the numeric/
  embeddable identity or not at all (this is why redefinition support is a
  CLI flag, not a form).
- Don't make fragments separately compilable; the unity build is
  load-bearing.
- Don't relitigate the phase-7 rulings (compound schemas syntax-only,
  optional/default orthogonality, super semantics).

## Sequencing sketch

1. §0 in order (a day, mostly release mechanics) + the §3 warn-on-ignored
   `--threads` one-liner.
2. §1 benchmark gates, then the types-for-JIT payoff — the long pole.
3. §3 contract design doc in parallel with §1; pool implementation only
   after both.
4. §2 closure cycles at a natural pause in §1; §3 network fuzzing anytime
   (independent).
5. §4/§5 opportunistically.
