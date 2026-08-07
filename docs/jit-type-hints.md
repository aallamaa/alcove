# Type hints and the numeric-loop JIT

What a `:f64` / `:int` param annotation actually buys, what the JIT is allowed
to assume from it, and the standing ruling on guard elision.

This is the reference for the "types-for-JIT" bet named in the 2026-06
language-evolution audit: hints exist to unlock *native codegen*, not to add a
type system. They are advisory. A wrong hint must never be able to do more than
cost a deoptimization.

## What a hint is

A trailing keyword on a parameter:

```lisp
(def f (x :f64 r :f64 i :int n :int)
  (if (< i n) (f (* r (* x (- 1.0 x))) r (+ i 1) n) x))
```

Keywords map to the same first-class type ids as everywhere else (`:f64` and
`:float` → `Float`, `:int` → `Int`, `:vec` → `Vector`, …; see
`alc_type_from_name` in alcove.c). They are inert to the evaluator and the
bytecode VM — nothing checks them at runtime on those tiers. Their only
consumer is the JIT.

## What the JIT does with them

`numloop_analyze` (jit_common.h) seeds each parameter slot's class from the
hint, then runs an abstract interpretation of the loop body to a fixed point,
promoting slots to FLOAT as float arithmetic flows into them, followed by one
strict pass that finalizes the layout.

Only `Float` is load-bearing as a *seed*. Every other annotation — `Int`,
`String`, `List` — leaves the slot at the INT default, because a non-numeric
hint is not a claim that the slot holds an integer. The seed matters exactly
where inference cannot reach: a slot that never flows through a float operation
before the back-edge (`r` above is passed through untouched) has nothing to
promote it, so without `:f64` the loop is typed int and declines.

Slots then live in disjoint physical register files for the whole loop — floats
in xmm/d, ints in a GPR pool — which is what makes the generated code fast and
also why a class mix-up is a correctness bug rather than a slowdown.


## `:vec` — f64 vector slots (amd64, 2026-08)

`:f64` types a *value*; `:vec` types a *container the loop reads through*, and
it buys something different. A `:vec` parameter that the loop passes through
unchanged is **loop-invariant**, so everything the loop needs about it is
derived once, at entry, next to the type guards:

```
base = (double *)((char *)v->ptr + sizeof(alc_vec_t)) + v->vec_win.start
len  = v->vec_win.end - v->vec_win.start
```

What is left in the loop body is what the C you would have written by hand
contains: a bounds check and an indexed load. The measured case — a hinted sum
over a 1M-element f64 vector — went from 46ms in the VM (it did not JIT at all)
to 1ms.

```lisp
(def vsum (v :vec i :int n :int acc :f64)
  (if (< i n) (vsum v (+ i 1) n (+ acc (vec-ref v i))) acc))
```

Things worth knowing before relying on it:

- **Invariance is checked, not assumed.** The analyzer requires the slot's
  tail-call argument to be the slot itself. A loop that reassigns the vector
  declines rather than hoisting a stale base pointer.
- **Only `VEC_KIND_F64`.** A GEN vector would need boxing and an i64 one a
  convert, neither of which the emitted load does. The kind is an entry guard
  like every other class assumption here — wrong kind costs a deopt, and the
  VM's answer is identical (asserted in the corpus for i64, GEN and
  non-vector arguments).
- **A vector slot costs 2 GPRs** (base + length) plus one shared scratch for
  the untagged index. They are allocated from the *top* of the int pool while
  int homes grow from the bottom, because the entry sequence uses rax/rdx as
  scratch — a vector home landing on either would be clobbered by the code
  computing it.
- **One vector slot per kernel** in this slice. With one, the operand of a
  `(vec-ref v i)` is unambiguous; a second needs stack provenance — which slot
  each VEC stack entry came from — and that, not register pressure, is the
  work a mat-mul shape would need.
- **amd64 only so far.** arm64 declines vector slots and runs the VM, so the
  corpus asserts VALUES here rather than `(jit? ...)`: a tier-divergent
  `assert-jits` would fail on one architecture by construction.

Bounds failures **deopt** rather than fault: one unsigned compare catches
`i < 0` and `i >= len` together, and the VM then raises the real
index-out-of-range error, so the tiers still agree.

### Register budgets

| | amd64 | arm64 |
|---|---|---|
| float homes + temps | 16 (xmm0-15) | 24 (d0-d7, d16-d31) |
| int homes + temps | 8 (`NL_X64_IPOOL`: rcx/rbx/rdx/rax + r8-r11) | 8 (`NL_A64_IPOOL`: x1-x7, x11) |
| f64 vector slots | 2 GPRs each + 1 shared scratch, from the top of the int pool | not emitted yet |

The two int pools are deliberately kept **equal**. The corpus asserts
`assert-jits` on specific kernels and runs on both architectures, so a kernel
whose JIT-vs-VM decision differs by arch is a test failure by construction.

Budget is consumed by homes *and* temps: every tail-call update materializes a
temp alongside its home, and a slot-vs-slot compare like `(< i n)` burns two.
In practice that caps a kernel at roughly 4 int slots. Exceeding a budget is
not an error — the loop simply runs in the VM.

## The guard model

A wrong hint is made safe by **entry guards**. On entry the emitter checks each
slot's actual runtime representation — a float slot must really be a float box,
an int slot a fixnum — and jumps to a deopt sled that returns NULL if not,
handing the call back to the VM. Guards are emitted *before* `loop_top`, so
they execute **once per call, never per iteration**.

Two other checks do live in the loop, and they are not type guards:

- **integer overflow** (`jo` / `B.VS` after a tagged add). Alcove defines
  integer overflow as an error, so this is a semantic requirement, not a safety
  net. Int slots are held tagged (`(v<<3)|1`) specifically so overflow is
  detectable here and deopts into the VM's error rather than wrapping.
- **division by zero**, for the same reason.

## Ruling: do NOT elide entry guards (2026-07-25)

The roadmap carried "hint-driven guard elision where the callee is statically
known" as an open item pending a ruling. The ruling is **no**, and the premise
behind it turns out to be false.

Elision is worth considering only if guards are a meaningful tax. They are not.
Measured on zero-iteration calls, where entry cost is 100% of the work and
nothing is amortized:

```
1 float guard : 208ms / 2,000,000 calls
4 float guards: 235ms / 2,000,000 calls
→ 3 extra guards = 27ms / 2M calls ≈ 4.5ns per guard per call
```

Out of ~104ns of total per-call cost, guards are ~4%; the rest is environment
setup and dispatch. And because they are hoisted, that 4.5ns is amortized over
the entire trip count — for a 1000-iteration loop it is roughly 0.01% of
runtime. There is no workload where removing them is worth measuring.

Against that, the downside is categorical rather than incremental. Today a
wrong hint costs a deopt. With guards elided it becomes type confusion: the
emitted code would read an arbitrary `exp_t*` as a `double`, or worse write
through it. That converts hints from an advisory optimization into an unsafe
`unsafe`-style assertion, and it does so *silently* — the failure mode is
memory corruption in generated code, which is the single hardest class of bug
to attribute in this codebase.

So the trade is ~4ns against memory safety, on the tier that has no other
safety net. Declined.

If short-trip-count calls ever become the bottleneck, the fix is to attack
per-call overhead as a whole (env setup dominates it at ~100ns), not to delete
the 4.5ns that keeps a wrong hint survivable.

**Revisit only if** a profile shows entry guards above ~5% of a real workload —
which, given they are hoisted, essentially requires loops whose trip count is
in the low single digits, and those should not be JIT'd as loops at all.

## Practical guidance

- Hint `:f64` on float slots the fixed point cannot reach — typically an
  accumulator or coefficient passed through the back-edge unchanged.
- `:int` is documentation; it is not required to get the int default.
- Check your work with `(jit? f)`. It returns `nil` when the loop declined,
  and declining is silent by design.
- Keep int slots to ~4. Past that the loop runs in the VM regardless of hints.
- A hint never changes results. If adding one changes an answer, that is a JIT
  bug — `make jit-fuzz` is the differential that should have caught it.
