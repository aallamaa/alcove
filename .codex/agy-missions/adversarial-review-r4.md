# Adversarial Review of Audit Round 4 Findings

## Objective

Adversarially verify each of the 31 findings below. For each finding:
1. **Confirm or refute** that the bug exists in the current code at HEAD (commit a456af5).
2. **Assess severity** — is the stated severity correct? Too high? Too low?
3. **Check reachability** — can the bug actually be triggered by user code, or is it blocked by an upstream guard?
4. **Check for false positives** — is the code actually correct despite the finding? Did the auditor miss a guard?
5. **Suggest the correct fix** if the finding is confirmed.

Be skeptical. The auditor may have misread the code, missed an upstream guard, or confused signed/unsigned. Check the ACTUAL code at the cited lines.

## Working directory

`/home/kader/Code/alcove`

## Important context

- Round 3 fixed 15 findings. Commit a456af5 contains those fixes.
- **Finding R4-29 is a suspected REGRESSION from the R3-7 fix**: the R3-7 fix used `sed -i 's/return -1;/return 0;/g' lfkv.c` which changed ALL functions' -1 returns to 0, including `lfkv_set` which has a documented contract to return -1 on failure. Verify this regression carefully.
- **Finding R4-2 may be an incomplete R3-10 fix**: R3-10 fixed `den == INT64_MIN` in make_rational but may have missed `num == INT64_MIN` with negative den.
- **Finding R4-18 may be an incomplete R3-4 fix**: R3-4 added `jo` after the ADD in for_loop_inc, but the retag (SHL) path may still lack overflow checking on amd64 while arm64 has it.

## Findings to verify

### HIGH severity

#### R4-3: msgpack int64 (0xd3) decoder missing FIX_FITS
- **File**: `msgpack.h:277-286`
- **Claim**: The int64 decoder does `return MAKE_FIX(v)` at line 285 WITHOUT a FIX_FITS check. Values outside the 61-bit fixnum range silently wrap. Same bug class as the previously-fixed uint64 wrap (0xcf).
- **Verify**: Read msgpack.h:277-286. Check if FIX_FITS is called before MAKE_FIX for the 0xd3 case.

#### R4-8: updatebang triple refcount leak + silent NULL return
- **File**: `alcove.c:3084-3154`
- **Claim**: In the ispair(keyv) → else branch, when EVAL(key) returns a non-string, three owned refs (key, val, keyv) are leaked and NULL is silently returned. The error handling code is dead code after an unconditional return.
- **Verify**: Read alcove.c:3080-3160. Trace the control flow when EVAL(key) returns a non-string.

#### R4-11: vec-copy! self-aliasing use-after-free
- **File**: `vector.h:669-676`
- **Claim**: When `(vec-copy! v v)` is called on a VEC_KIND_GEN vector, dcells == scells. The loop does unrefexp(dcells[i]) which may free the element, then refexp(scells[i]) dereferences the freed pointer.
- **Verify**: Read vector.h:660-680. Check if self-aliasing (dexp == sexp) is handled.

#### R4-12: HAMT signed-zero hash invariant violation
- **File**: `hamt.h:45-48`
- **Claim**: hamt_hashkey hashes raw IEEE-754 bits, so +0.0 and -0.0 hash differently, but isequal uses == which returns true for both. Entry stored under +0.0 is unreachable by lookup with -0.0.
- **Verify**: Read hamt.h:40-50. Check if -0.0 is normalized before hashing.

#### R4-20: reduce fast path silent integer overflow
- **File**: `builtins_stdlib.h:2207`
- **Claim**: The reduce fast-path arithmetic does `MAKE_FIX(r)` WITHOUT checking FIX_FITS(r). Two 61-bit fixnums can sum to a value outside the fixnum range.
- **Verify**: Read builtins_stdlib.h:2200-2215. Check if FIX_FITS is called.

#### R4-24: l_cons uses istrue(b) — AST/VM divergence
- **File**: `compiler_impl.h:3007-3014`
- **Claim**: The VM's l_cons handler uses `istrue(b)` to decide whether to keep the cdr, but the AST conscmd was fixed to use `b && b != NIL_EXP`. Compiled code drops falsey-but-non-nil cdr values like 0, "".
- **Verify**: Read compiler_impl.h:3007-3014 and alcove.c:7461-7480. Compare the two conditions.

#### R4-27: resp_write_reserve OOM heap overflow / NULL deref
- **File**: `resp.c:482-487`
- **Claim**: When realloc fails, the code returns c->wbuf + c->wlen without advancing wlen. The caller writes n bytes past the buffer end — heap overflow. If wbuf is NULL, segfault.
- **Verify**: Read resp.c:470-500. Trace what happens when realloc returns NULL.

### MEDIUM severity

#### R4-1: make_decimal_raw INT128_MIN negation UB
- **File**: `numeric.h:255`
- **Claim**: `__int128 mag = coef < 0 ? -coef : coef` overflows when coef == INT128_MIN. Reachable via dec_mul of two large decimals.
- **Verify**: Read numeric.h:250-260. Check if INT128_MIN is handled.

#### R4-2: make_rational INT64_MIN numerator negation UB (incomplete R3-10 fix?)
- **File**: `numeric.h:58`
- **Claim**: `num = -num` when den < 0 doesn't check if num == INT64_MIN. The R3-10 fix only checked den == INT64_MIN, not num. Reachable via load_rational on crafted dump.
- **Verify**: Read numeric.h:45-60. Check if num == INT64_MIN is handled.

#### R4-4: persist load_number missing FIX_FITS
- **File**: `persist.h:326`
- **Claim**: `return MAKE_FIX(v)` without FIX_FITS check. Corrupt dump can contain any int64.
- **Verify**: Read persist.h:320-330.

#### R4-7: expandmacrocmd refcount leaks
- **File**: `alcove.c:3799-3806`
- **Claim**: refexp(tmpexp) passed to lookup is never freed (lookup borrows). tmpexp2 (lookup result) never freed when non-macro. refexp(form) passed to expandmacro is leaked (expandmacro borrows).
- **Verify**: Read alcove.c:3795-3810. Check lookup and expandmacro ownership conventions.

#### R4-13: vec-shift! missing watch_notify
- **File**: `vector.h:1713-1733`
- **Claim**: vecshiftcmd mutates the vector but doesn't call watch_notify or watch_validate, unlike all other container mutators.
- **Verify**: Read vector.h:1713-1733. Compare with vecpushcmd/vecpopcmd.

#### R4-14: vec-unshift! missing watch_notify
- **File**: `vector.h:1669-1707`
- **Claim**: Same as R4-13 for vecunshiftcmd.
- **Verify**: Read vector.h:1669-1707.

#### R4-15: hamt constructor NaN key validation
- **File**: `hamt.h:454-484`
- **Claim**: hamtcmd and load_hamt_value don't reject NaN float keys, while hamtassoccmd does. NaN key can be inserted but never looked up or removed.
- **Verify**: Read hamt.h:454-484 and hamt.h:497-501. Compare NaN handling.

#### R4-17: x64_test_reg8_imm8 missing REX for regs 4-7
- **File**: `jit_amd64.h:121-128`
- **Claim**: The encoder omits REX prefix for registers 4-7 (RSP/RBP/RSI/RDI), causing it to reference AH/CH/DH/BH instead of SPL/BPL/SIL/DIL. Only affects safe_p's offset tag check at line 2055.
- **Verify**: Read jit_amd64.h:121-128. Check x86-64 byte register encoding rules.

#### R4-18: for_loop_inc retag SHL no overflow check (incomplete R3-4 fix?)
- **File**: `jit_amd64.h:3077-3084`
- **Claim**: The done path retags via SHL RDX,3 + OR RDX,1 without overflow check. The arm64 twin uses ARM64_EMIT_RETAG_RET_CK. The R3-4 fix added jo after the ADD but the retag itself is still unchecked.
- **Verify**: Read jit_amd64.h:3070-3090. Compare with jit_arm64.h:2817-2820.

#### R4-21: take-while / drop-while form leak
- **File**: `builtins_stdlib.h:4685-4737`
- **Claim**: Both functions build a call form via make_node, EVAL it, but never unrefexp the local `call` variable. Leaks 2 exp_t nodes per element.
- **Verify**: Read builtins_stdlib.h:4685-4740. Check if `call` is freed.

#### R4-22: eachcmd rejects vectors/strings despite docstring
- **File**: `builtins_stdlib.h:3722-3740`
- **Claim**: Docstring promises list/string/vector support, but implementation only accepts ispair. Vectors and strings error.
- **Verify**: Read builtins_stdlib.h:3722-3745. Check what types are accepted.

#### R4-25: compile_arith left-folds is/iso/mod with 3+ args
- **File**: `compiler_impl.h:347-348`
- **Claim**: IS/ISO/MOD are binary-only in the AST but left-folded by the compiler. `(is 5 5 99)` → AST returns t, VM returns nil.
- **Verify**: Read compiler_impl.h:340-360. Check which ops are guarded against >2 args.

#### R4-28: lfkv_get_expiry returns 0 for absent keys
- **File**: `lfkv.c:349,352`
- **Claim**: Returns 0 for absent keys instead of documented -1. cmd_ttl returns -1 (no expire) instead of -2 (key missing) for non-existent keys.
- **Verify**: Read lfkv.c:344-360 and lfkv.h:101. Check the return values.

#### R4-29: lfkv_set returns 0 on failure — REGRESSION from R3-7 fix
- **File**: `lfkv.c:166-220`
- **Claim**: The R3-7 fix used `sed -i 's/return -1;/return 0;/g' lfkv.c` which changed lfkv_set's failure returns from -1 to 0. The caller resp_kv_set checks `< 0`, so the error branch is now dead code. Value ref is leaked on OOM/table-full.
- **Verify**: Read lfkv.c:160-220 and lfkv.h:105-108 and resp.c:341-349. Check if lfkv_set returns -1 on failure.

#### R4-30: cmd_set NX/XX OOM misreported
- **File**: `resp.c:1340-1344`
- **Claim**: lfkv_set_nx/lfkv_set_xx return 0 on OOM (same as condition-not-met). cmd_set writes nil instead of an error.
- **Verify**: Read resp.c:1335-1345. Check how ok==0 is handled.

#### R4-31: pp_flat_width no depth guard
- **File**: `pp.h:23-65`
- **Claim**: pp_flat_width recurses without depth guard. Persist loader allows 16384 nesting levels; printing such a form overflows the C stack. pp_form's depth guard doesn't protect pp_flat_width's independent recursion.
- **Verify**: Read pp.h:23-65 and pp.h:90-110. Check if pp_flat_width has a depth limit.

### LOW severity

#### R4-5: load_pair memory leak on truncated reads
- **File**: `persist.h:429-434`
- **Claim**: load_pair doesn't unrefexp(e) on sub-read failure. Leaks the placeholder exp_t.
- **Verify**: Read persist.h:419-437.

#### R4-6: dec_cmp strcmp fallback wrong (dead code)
- **File**: `numeric.h:402-404`
- **Claim**: strcmp gives lexicographic ordering, not numeric. Currently dead code but would be wrong if reached.
- **Verify**: Read numeric.h:395-410. Check if the fallback is reachable.

#### R4-9: updatebang dead code
- **File**: `alcove.c:3131-3138`
- **Claim**: Error-handling code after an unconditional return is unreachable.
- **Verify**: Read alcove.c:3125-3140.

#### R4-10: load_native_module dlopen handle leak
- **File**: `alcove.c:7398-7435`
- **Claim**: Three error paths after dlopen don't call dlclose.
- **Verify**: Read alcove.c:7395-7440.

#### R4-16: dump_dict silent error swallow (dead code)
- **File**: `dict.h:150-178`
- **Claim**: Legacy dump_dict swallows __DUMP__ failures. No active call sites.
- **Verify**: Read dict.h:150-180. Check for callers.

#### R4-19: counter-loop shapes missing overflow check
- **File**: `jit_amd64.h:503,1630; jit_arm64.h:498,1510`
- **Claim**: Curated counter-loop shapes lack JO/B.VS after tagged add/sub. General numloop path has it.
- **Verify**: Read the cited lines. Compare with numloop overflow handling.

#### R4-23: sort_cmp_default transitivity violation
- **File**: `builtins_stdlib.h:2624-2640`
- **Claim**: Returns 0 for incomparable types, breaking strict weak ordering.
- **Verify**: Read builtins_stdlib.h:2620-2645.

#### R4-26: l_vec_ref/l_vec_set NaN cast UB
- **File**: `compiler_impl.h:3153,3175`
- **Claim**: `(int64_t)iexp->f` without isnan check. NaN-to-int is UB. Shared by both tiers.
- **Verify**: Read compiler_impl.h:3150-3180.

## Report format

For each finding, output:
```
R4-N: CONFIRMED/REFUTED/DOWNGRADED
Severity: HIGH/MEDIUM/LOW (adjusted if needed)
Reason: <brief explanation with code evidence>
Fix: <suggested fix if confirmed>
```

At the end, provide a summary count of confirmed/refuted/downgraded findings.
