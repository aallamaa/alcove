# Adversarial Review of Audit Round 6 Findings

## Objective

Adversarially verify each finding below. For each:
1. **Confirm or refute** that the bug exists at HEAD (commit f86056c).
2. **Assess severity** — correct? Too high? Too low?
3. **Check reachability** — can it be triggered by user code?
4. **Check for false positives** — did the auditor miss a guard?
5. **Suggest the correct fix** if confirmed.

Be skeptical. Check the ACTUAL code at the cited lines.

## Working directory

`/home/kader/Code/alcove`

## Findings to verify

### HIGH

#### R6-1: exptcmd UAF on missing-args path
- **File**: `alcove.c:5687-5689`
- **Claim**: `unrefexp(e)` called BEFORE `error(ERROR_MISSING_PARAMETER, e, env, ...)` on 0-arg/1-arg path. error() calls refexp(e), form_line(e) on freed memory.
- **Verify**: Read alcove.c:5685-5693. Check the order of unrefexp(e) vs error(...,e,...).

#### R6-2: resp_apply_incr INCR on non-existent key doesn't create key
- **File**: `resp.c:1454-1484`
- **Claim**: When cur_v is NULL (key absent), new_blob is allocated but the store logic is inside `if (cur_v)` — falls through to resp_write_int without storing. Blob leaked, key never created, reply claims success.
- **Verify**: Read resp.c:1454-1484. Check if the `else` branch (for missing key) stores via lfkv_set_nx.

#### R6-3: cmd_set NX/XX NULL deref on keyspace OOM
- **File**: `resp.c:1362-1365`
- **Claim**: NX/XX branch calls lfkv_set_nx/lfkv_set_xx with resp_kv (which may be NULL) without NULL guard. Other commands have the guard (added in R5-18).
- **Verify**: Read resp.c:1360-1370. Check if `if (!resp_kv)` guard exists.

#### R6-4: lfkv probe() never sets tombstone_idx — table exhaustion
- **File**: `lfkv.c:148-167`
- **Claim**: probe() declares tombstone_idx output but never sets it. Tombstones never reclaimed for different keys. Table exhausts after FLUSHDB cycles.
- **Verify**: Read lfkv.c:144-167. Check if tombstone_idx is ever assigned inside the loop.

### MEDIUM

#### R6-5: cross-function tail-call ref leak in invoke_body
- **File**: `debugger.h:573`
- **Claim**: `marker->content = NULL` drops refexp(fn) without unrefexp, leaking one ref per cross-function tail call.
- **Verify**: Read debugger.h:565-580. Check if marker->content is unrefexp'd before NULL.

#### R6-6: error? swallows call/cc escape continuations
- **File**: `builtins_stdlib.h:2821`
- **Claim**: iserror(a) is true for cont escapes, so error? returns TRUE_EXP instead of propagating.
- **Verify**: Read builtins_stdlib.h:2815-2825. Check if is_cont_escape is checked first.

#### R6-7: error-message swallows call/cc escape continuations
- **File**: `builtins_stdlib.h:2835`
- **Claim**: Same as R6-6 for error-message.
- **Verify**: Read builtins_stdlib.h:2830-2840.

#### R6-8: print.h dict key printing doesn't escape
- **File**: `print.h:215`
- **Claim**: Dict keys with embedded quotes/backslashes aren't escaped, breaking round-trip.
- **Verify**: Read print.h:210-220. Check if dict keys go through the escape loop.

#### R6-9: epoch_min_quiescent OOB read during registration overflow
- **File**: `epoch.c:42-50`
- **Claim**: When epoch_nthreads temporarily hits 65 during registration overflow, epoch_min_quiescent reads epoch_threads[64] — OOB.
- **Verify**: Read epoch.c:25-50. Check if n is clamped to EPOCH_MAX_THREADS.

#### R6-10: FFI MAKE_FIX without FIX_FITS for int/long returns
- **File**: `ffi.h:1147,1150`
- **Claim**: alc_ffi_call return marshalling uses MAKE_FIX without FIX_FITS for AFFI_INT and AFFI_LONG.
- **Verify**: Read ffi.h:1140-1155.

#### R6-11: FFI closure dispatch MAKE_FIX without FIX_FITS
- **File**: `ffi.h:427,431`
- **Claim**: C->alcove callback uses MAKE_FIX without FIX_FITS for int/long args.
- **Verify**: Read ffi.h:420-435.

#### R6-12: FFI unpack MAKE_FIX without FIX_FITS
- **File**: `ffi.h:858,864`
- **Claim**: ffiunpackcmd uses MAKE_FIX without FIX_FITS for int/long fields.
- **Verify**: Read ffi.h:850-870.

#### R6-13: dec_cmp double fallback wrong exponent sign
- **File**: `numeric.h:408-409`
- **Claim**: Uses `pow(10.0, a->scale)` instead of `pow(10.0, -(double)a->scale)`. Inverts comparison results.
- **Verify**: Read numeric.h:405-410. Check the exponent sign.

#### R6-14: I64 vector persist roundtrip break
- **File**: `persist.h:673-677` + `persist.h:597-601`
- **Claim**: dump writes raw int64, load rejects non-FIX_FITS values. Tensor ops can write out-of-range values, causing save→load failure.
- **Verify**: Read persist.h:595-605 and 673-684.

#### R6-15: repeatcmd swallows body errors
- **File**: `builtins_stdlib.h:3286-3306`
- **Claim**: No `if (ret && iserror(ret)) break;` after do/while. Error is unref'd and overwritten each iteration.
- **Verify**: Read builtins_stdlib.h:3286-3310. Compare with whilecmd.

#### R6-16: nocmd crashes on zero-arg call
- **File**: `builtins_stdlib.h:3376-3380`
- **Claim**: `(no)` with no args — car(NULL) returns NULL, EVAL(NULL) derefs NULL.
- **Verify**: Read builtins_stdlib.h:3375-3385.

#### R6-17: VM try-handler env snapshot omits env->d
- **File**: `compiler_impl.h:2375-2383`
- **Claim**: vm_handler_push snapshots inline_vals but not env->d. Handler can't see dict bindings.
- **Verify**: Read compiler_impl.h:2370-2390 and vm_handler_push.

#### R6-18: ARM64 numloop float BR_IF_FALSE NaN mishandling for LT/GT
- **File**: `jit_arm64.h:1275-1287`
- **Claim**: Uses LE(13) condition for LT/GT br_false, which is NOT taken for NaN. VM returns false for NaN, so BR_IF_FALSE should branch.
- **Verify**: Read jit_arm64.h:1270-1290. Compare with amd64 twin.

#### R6-19: ARM64 for_loop_inc missing per-iteration overflow check
- **File**: `jit_arm64.h:2814-2817`
- **Claim**: Uses arm64_add_imm (non-flag-setting) with no B.VS. Only final retag check catches overflow. amd64 twin has jo.
- **Verify**: Read jit_arm64.h:2810-2820. Compare with amd64.

#### R6-20: SET EX/PX TTL race (peek-then-set_expiry)
- **File**: `resp.c:1387-1395`
- **Claim**: peek+set_expiry doesn't verify pointer identity. Concurrent SET can inherit unwanted TTL.
- **Verify**: Read resp.c:1385-1400. Check if lfkv_touch_if_value is used.

### LOW

#### R6-21: hamt load_hamt_value doesn't reject NaN keys
- **File**: `hamt.h:419-447`
- **Claim**: Constructor and assoc reject NaN, but load path doesn't.
- **Verify**: Read hamt.h:419-450. Check for NaN validation.

#### R6-22: repeatcmd counter-- signed overflow with INT64_MIN
- **File**: `builtins_stdlib.h:3293`
- **Claim**: `while (counter-- > 0)` with INT64_MIN decrements to INT64_MIN-1 (UB).
- **Verify**: Read builtins_stdlib.h:3290-3295.

#### R6-23: forcmd idx = FIX_VAL(lastidx)+1 overflow with INT64_MAX
- **File**: `builtins_stdlib.h:3702`
- **Claim**: R5-5 was refuted because FIX_VAL is 61-bit. Re-check: can lastidx be INT64_MAX?
- **Verify**: Read builtins_stdlib.h:3698-3705. Note: R5-5 was REFUTED — FIX_VAL max is 2^60-1, +1 = 2^60, no overflow.

#### R6-24: for/repeat lack budget_check
- **File**: `builtins_stdlib.h:3273-3306,3648-3712`
- **Claim**: whilecmd has budget_check, but for/repeat don't. Not interruptible by with-time-limit.
- **Verify**: Read both loops. Check for budget_check() calls.

#### R6-25: sort_cmp_default NaN violates strict weak ordering
- **File**: `builtins_stdlib.h:2644-2655`
- **Claim**: R4-23 fixed mixed types, but NaN float comparison still returns 0, breaking strict weak ordering.
- **Verify**: Read builtins_stdlib.h:2640-2660.

#### R6-26: msgpack str decode int truncation for >2GB
- **File**: `msgpack.h:316`
- **Claim**: make_string((char*)(b+*pos), (int)n) truncates size_t n to int.
- **Verify**: Read msgpack.h:310-320.

#### R6-27: dec_div rounding increment overflow
- **File**: `numeric.h:396`
- **Claim**: `q += 1` not overflow-checked after the loop.
- **Verify**: Read numeric.h:393-397.

#### R6-28: defclass NaN/Inf default breaks round-trip
- **File**: `builtins_stdlib.h` (defclass_write_form)
- **Claim**: Float defaults with NaN/Inf produce non-readable output.
- **Verify**: Find and read defclass_write_form.

#### R6-29: resp_apply_incr dead code (duplicate if(!ok))
- **File**: `resp.c:1473-1481`
- **Claim**: Duplicate if(!ok) block is dead code.
- **Verify**: Read resp.c:1465-1485.

#### R6-30: FFI unpack int32 MAKE_FIX (same class as R6-12)
- **File**: `ffi.h:858`
- **Claim**: int32 values always fit fixnum (max 2^31-1 < 2^60-1), so this is NOT a bug. Check if the auditor was wrong.
- **Verify**: Read ffi.h:855-860. int32 max is 2^31-1, well within fixnum range.

## Report format

For each finding:
```
R6-N: CONFIRMED/REFUTED/DOWNGRADED
Severity: HIGH/MEDIUM/LOW (adjusted)
Reason: <brief with code evidence>
Fix: <suggested fix if confirmed>
```

At the end, provide summary counts.
