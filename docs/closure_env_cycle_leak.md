# Closure ↔ captured-env refcount cycle leak (#6)

Status: **FIXED** (2026-05-29). Root cause was two issues compounding; both
addressed. Verified 951/0 across all 10 build variants, ASan-clean, and the
repro RSS went from ~120 MB to a flat 2.0 MB at 5000 rounds. See "Resolution"
at the bottom. The investigation that follows is kept for the record.

## The bug

alcove manages memory with manual reference counting and has **no cycle
collector**. A closure that becomes reachable from the very environment it
captured forms a 2-node strong reference cycle that refcounting cannot
reclaim. It leaks permanently.

### The cycle, edge by edge

For `(def inner (x) ...)` (or `(let inner (fn ...) ...)`) evaluated *inside*
a function body:

1. `defcmd` / `fncmd` / `defmacrocmd` capture the defining env onto the
   lambda's body-wrapper node:
   ```c
   val->next->meta = (struct keyval_t *)ref_env(env);   // alcove.c:3372/3440/3508
   ```
   → **lambda owns a ref to env** (`env->nref++`). This is deliberate: the
   closure's free variables live in that env, so it must stay alive as long
   as the closure does.

2. `defcmd` also binds the lambda *into that same env*:
   ```c
   set_get_keyval_dict(env->d, name, val);   // alcove.c:3446
   // → k->val = refexp(val)                  // alcove.c:1083
   ```
   → **env owns a ref to the lambda** (`lambda->nref++`).

Result: `env → (dict entry) → lambda → (next->meta) → env`.

### Why teardown can't break it

When the call frame ends, `destroy_env(env)` runs:

```c
if (REFCOUNT_DEC(&env->nref) > 0)
  break;   // alcove.c (destroy_env) — still referenced, stop here
```

The lambda still holds its ref to `env`, so `env->nref` stays > 0 and we
`break` **before** releasing `env`'s inline vals / dict. That dict still
holds the only ref to the lambda, so the lambda is never freed, so its
`next->meta` ref on `env` is never released. Stalemate — both objects leak.

`unrefexp` *does* release a closure's captured env (alcove.c:613-617), but
only when the lambda itself is freed — which can't happen here.

### Reproduction (confirmed on current build)

```lisp
(def work (k)
  (if (is k 0) 0
    (do (def inner (x) (* x x))         ; local closure, captures work's frame
        (+ (inner k) (work (- k 1))))))
(def driver (rounds)
  (if (is rounds 0) 0 (do (work 30) (driver (- rounds 1)))))
```

Peak RSS scales with the number of closures created, not with live set:

| rounds | peak RSS |
|--------|----------|
| 50     | 3.1 MB   |
| 500    | 13.7 MB  |

Recursion depth is fixed, so only leaked cycles accumulate.
**Top-level defs are unaffected** — they capture the immortal global env,
which is never torn down, so there is no reclaimable cycle to leak. That is
why this has gone unnoticed: it only bites long-running programs that churn
*local* closures.

## Fix options

### Option A — Weak captured-env reference  ❌ REJECTED
Capture `env` without `ref_env` (borrowed). Breaks the cycle, but the env is
then freed when the defining frame exits, leaving the closure's free
variables dangling → use-after-free for any closure that outlives its
defining scope (`make-counter` and friends — the whole point of closures).
Reintroduces a worse bug.

### Option B — Targeted cycle-break in `destroy_env`  (pragmatic, medium risk)
Before the `REFCOUNT_DEC > 0` early-break, detect closures stored in *this*
env (`inline_vals` / `d`) whose captured env (`->next->meta`) is this same
env, and whose lambda refcount shows env is its *only* holder. NULL out the
lambda→env edge (drop env's ref) so the count can fall to zero, then proceed
to free both.
- **Pro:** stays inside the existing model; fixes exactly the leaking shape.
- **Con:** must prove "env is the closure's only referrer" precisely, or we
  free a closure that was also returned / stored elsewhere → UAF. Needs the
  refcount-accounting equivalent of a trial deletion on the 2-cycle. Fiddly;
  demands thorough tests (returned closure, closure stored in a global,
  closure captured by a sibling, mutually-recursive local defs).

### Option C — Asymmetric ownership: weak env→closure binding  (smaller, narrow risk)
Keep lambda→env **strong** (closure owns its env). Make the env→lambda
binding created by a *self-capturing* `def` **non-owning**. Then when the
frame's external refs drop and nothing else holds the lambda, the lambda
hits 0, is freed, and releases env — no cycle.
- **Pro:** localized to the def/capture site; no teardown-time detection.
- **Con:** if the env outlives the closure through a *different* reference
  (e.g. a sibling frame holds env) and someone then looks up the closure's
  name in env, the weak binding dangles → UAF. The window is narrow (the
  binding name is local to the frame) but must be bounded and tested.

### Option D — Document + escape hatch  (lowest risk)
Accept the limitation, document it (here + TODO.lst), and advise hoisting
hot helpers to top-level `def` (which doesn't leak). Optionally add an
explicit scope/release form later. No correctness risk; the leak remains for
local-closure-churning workloads.

### Option E — Real (opt-in) cycle collector
A trial-deletion / mark-sweep pass over the env+closure graph. Correct and
general, but the largest change by far and arguably over-engineered for
alcove's refcount-only design. Not recommended now.

## Recommendation

If we fix it: **Option B**, gated behind a tight, well-tested predicate and
a regression suite covering the "must NOT collect" cases (returned closure,
globally-stored closure, sibling-captured env). It fixes the real shape
without changing closure semantics. If we'd rather not touch core teardown
right now: **Option D** (document) is the safe hold, since top-level defs —
the common case — are unaffected.

## Resolution (what was actually done)

Investigation while implementing Option B uncovered that the leak was **two
compounding bugs**, not just the cycle:

### 1. Per-call refcount leak on env-resolved closures (the dominant cause)
In `evaluate`, the `(operator args)` dispatch resolves the operator via
`tmpexp2 = lookup(tmpexp, env)` — an **owned** ref. Every sibling branch
released it (the FFI branch, the tail-marker branch, the string-index
branches), but the non-tail `invoke` branch did not:

```c
ret = invoke(e, tmpexp2, env);   // invoke() BORROWS fn (own refexp/unrefexp
goto finisht;                    // net to zero) and consumes e — tmpexp2 leaks
```

`invoke` takes its own ref on `fn` and consumes `e`, so the caller's
`tmpexp2` ref was leaked **once per call**. For a global function this is
masked (its other refs churn), but for a closure resolved from a local frame
it pinned the closure alive — `inner` called once sat at `nref == 2`. Fix:
add `unrefexp(tmpexp2)` after the `invoke`, matching the tail-marker branch
immediately above. (The `tmpexp`-block `invoke` site is NOT affected — there
`tmpexp` is borrowed from `e`, which `invoke` consumes.)

The sibling `ismacro` branch in the same block had the identical bug —
`invokemacro` also borrows `fn` and consumes `e` — so a macro defined inside
a function body leaked its frame the same way (24.6 MB → 2.0 MB at 2000
rounds after the matching `unrefexp(tmpexp2)`). Fixed in the same pass.

### 2. The closure↔env cycle itself (Option B)
Even with refcounts balanced, a self-capturing closure bound in its own frame
is a 2-cycle. `env_break_self_cycle` (called from `destroy_env` at the
`REFCOUNT_DEC > 0` early-break) detects when every remaining ref to env is a
back-ref from a closure that env solely owns, and severs those edges so env
and the closures can free. Conservative guards (`residual == self_refs`,
per-closure `nref == 1`, `!FLAG_SHARED`) ensure a returned / globally-stored /
sibling-captured closure is never collected — proven by the "must NOT
collect" tests in test.alc (`cycle: m3 valid after m4`, `cycle: g-mul
survives churn`, the mutual-recursion cases) and ASan.

Fix #1 alone unmasks the cycle for once-called closures; fix #2 reclaims the
true cycle (e.g. a closure created but whose frame would otherwise persist).
Together the repro is flat. Tests added under "20c" in test.alc.

---

## General reference cycles & the `(heap-stats)` leak audit (2026-06)

The closure↔env 2-node cycle above is broken automatically
(`env_break_self_cycle`). **Arbitrary** reference cycles are not reclaimed by
the refcounter — a cyclic list, a vector that contains itself, a callback that
captures a dict that holds the callback, etc. There is still **no tracing GC**
(a settled non-goal), but since 2026-07 the growth is no longer unbounded:
**`(gc-cycles)`** (gc.h) is an on-demand trial-deletion collector that sweeps
the calling thread's arena and reclaims container-threaded cycles explicitly,
at zero cost to the hot paths. Its one deliberate gap is cycles threaded
through closure captures (the callback↔dict shape): those are conservatively
kept — break them by hand (nil the key) before dropping the last reference,
or accept the leak and monitor with the audit below.

`(heap-stats)` is the audit handle. It returns a property list for the calling
thread's `exp_t` arena:

```
(heap-stats)  ; => (:live L :free F :allocated A :chunks C)
```

`:live` is the number of cells currently in use (allocated minus the free-list).
To detect a cycle leak, diff `:live` across a workload that *should* free
everything it allocates:

```
(= before (nth (heap-stats) 1))
(repeat 100000 (do-one-request))     ; should be net-zero if cycle-free
(prn (- (nth (heap-stats) 1) before)) ; ~0 = clean; steadily rising = a cycle leak
```

An acyclic workload returns to its baseline (the cells go back to the
free-list); a cyclic one grows ~one cell per cycle created. The check is
zero-cost on the hot path (only a per-chunk counter and an on-demand free-list
walk), so it is always available, including under `--threads` (per reactor).

Avoiding cycles: break them explicitly before dropping the last external
reference (e.g. `(vec-set! v 0 nil)` before letting a self-referential vector
go), or keep cyclic ownership out of long-lived structures.
