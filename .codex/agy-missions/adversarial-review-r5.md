# Adversarial Review of Audit Round 5 Findings

## Objective

Adversarially verify each of the 33 findings below. For each finding:
1. **Confirm or refute** that the bug exists in the current code at HEAD (commit cf0b4f6).
2. **Assess severity** — is the stated severity correct? Too high? Too low?
3. **Check reachability** — can the bug actually be triggered by user code, or is it blocked by an upstream guard?
4. **Check for false positives** — is the code actually correct despite the finding? Did the auditor miss a guard?
5. **Suggest the correct fix** if the finding is confirmed.

Be skeptical. The auditor may have misread the code, missed an upstream guard, or confused signed/unsigned. Check the ACTUAL code at the cited lines.

## Working directory

`/home/kader/Code/alcove`

## Important context

- Rounds 1-4 fixed 84 findings. Commit cf0b4f6 contains all fixes.
- **R5-1 (expandmacrocmd UAF)** may be a regression from the R4-7 fix. The R4-7 fix changed the non-macro path to return `car(form)` (a borrowed pointer) instead of `refexp(form)`. Verify this carefully.
- **R5-9 (arm64 tak bail_pc)** is a compile error on aarch64 — verify the variable is truly undeclared.
- **R5-3 (eachcmd list leak)** may be a regression from the R4-22 fix (adding coll_to_list support). The original code may have had `tmpexp = retval` that was lost in the refactor.
- The JIT overflow check findings (R5-10 through R5-16) are about curated counter-loop shapes — R4-19 fixed some but not all of them.

## Findings to verify

### HIGH severity

#### R5-1: expandmacrocmd UAF on non-macro forms
- **File**: `alcove.c:3791-3810`
- **Claim**: When macroexpand-1 is called on a non-macro form (e.g. `(+ 1 2)`), `tmpexp = car(form)` is a borrowed pointer. The function returns `tmpexp` after `unrefexp(e)` frees `e` (and its subtree including `form` and `tmpexp`). The caller receives a dangling pointer. Confirmed crash under ASAN.
- **Verify**: Read alcove.c:3791-3812. Trace the non-macro path: does `tmpexp` get a refexp before return? Does the `finish:` label unrefexp(e) which frees form?

#### R5-2: exptcmd double-free with 1 argument
- **File**: `alcove.c:5621-5683`
- **Claim**: When `(expt 5)` is called with 1 arg, `v = e->next` is the raw AST node. `v2 = v->next` is NULL so v is never EVAL'd. The type checks fail, error path does `unrefexp(v)` which frees the AST node, then `unrefexp(e)` tries to free it again.
- **Verify**: Read alcove.c:5621-5683. Trace the 1-arg path.

#### R5-3: eachcmd leaks owned ref to cons-list collection
- **File**: `builtins_stdlib.h:3729-3812`
- **Claim**: When `each` iterates over a cons list, `retval = EVAL(curval->content, env)` creates an owned ref. `tmpexp` is only set for non-pair sequences (after coll_to_list). For pair lists, `retval` is walked to NULL via `retval = retval->next`, so at cleanup both `tmpexp` and `retval` are NULL — the original list ref is leaked.
- **Verify**: Read builtins_stdlib.h:3754-3805. Check if `tmpexp` is set for the pair-list path.

#### R5-4: watch_notify UAF when watcher calls unwatch!
- **File**: `watch.h:176-188`
- **Claim**: The loop saves `next = c->next` before EVAL, but if the watcher calls `(unwatch! obj)`, the entire watcher list is freed (including both `c` and `next`). After EVAL returns, `c = next` sets c to a dangling pointer.
- **Verify**: Read watch.h:170-195 and the unwatchcmd implementation. Check if unwatch frees the entire list.

#### R5-9: arm64 tak bail_pc undeclared — compile error
- **File**: `jit_arm64.h:2516-2518`
- **Claim**: `bail_pc` is used in patch statements but never declared. The bail block is emitted but its offset `n` is never captured. This is a compile error on aarch64.
- **Verify**: Read jit_arm64.h:2500-2520. Check if `int bail_pc = n;` exists.

### MEDIUM severity

#### R5-5: forcmd signed overflow in loop bound
- **File**: `builtins_stdlib.h:3697`
- **Claim**: `int64_t idx = FIX_VAL(lastidx) + 1` overflows when lastidx is INT64_MAX (UB).
- **Verify**: Read builtins_stdlib.h:3690-3700.

#### R5-6: randomcmd swallows argument evaluation errors
- **File**: `builtins_stdlib.h:2057-2074`
- **Claim**: When `(random expr)` evaluates expr to an error, iserror(a) is never checked. Returns 0 silently.
- **Verify**: Read builtins_stdlib.h:2050-2075.

#### R5-7: trycmd finally error priority inverted
- **File**: `builtins_stdlib.h:2891-2897`
- **Claim**: When both body and finally produce errors, the finally error is silently discarded because the condition requires `!iserror(ret)`.
- **Verify**: Read builtins_stdlib.h:2885-2900.

#### R5-8: print.h string escape double-outputs control chars
- **File**: `print.h:113-127`
- **Claim**: Control char escape branches emit the escape text, then fall through to `putchar(*s)` which emits the raw byte too.
- **Verify**: Read print.h:105-130. Check if there's a `continue` after each escape branch.

#### R5-10: match guard (? pred) leaks predicate
- **File**: `builtins_control.h:143-157`
- **Claim**: `pred = EVAL(rest->content, eval_env)` creates an owned ref. `alc_apply1` borrows pred. After the check, pred is never unrefexp'd.
- **Verify**: Read builtins_control.h:138-155. Check if pred is freed.

#### R5-11: for-gen leaks previous body result on generator error
- **File**: `builtins_control.h:669-670`
- **Claim**: When alc_gen_step returns an error, `ret = v` overwrites ret without freeing the previous value.
- **Verify**: Read builtins_control.h:660-675.

#### R5-12: set_key_for_value doesn't normalize -0.0
- **File**: `set.h:57-62`
- **Claim**: Set uses raw IEEE-754 bits for float keys, so +0.0 and -0.0 produce different keys but isequal says they're equal. Violates set invariant.
- **Verify**: Read set.h:50-65. Check if -0.0 is normalized.

#### R5-13: compile_arith arity error class divergence
- **File**: `compiler_impl.h:2913-2918 vs 3509-3520`
- **Claim**: l_tail_call uses ERROR_MISSING_PARAMETER for arity mismatch, vm_invoke_values uses ERROR_ILLEGAL_VALUE. Same error, different class depending on tail position.
- **Verify**: Read both cited lines. Compare error codes.

#### R5-14: compile_with accepts NULL value expression
- **File**: `compiler_impl.h:569-570`
- **Claim**: `(with (x) body)` compiles as if x is bound to nil, but the AST tier errors with "Missing parameter in with".
- **Verify**: Read compiler_impl.h:565-575. Check if NULL content is guarded.

#### R5-15: make_decimal_raw sequential rounding bug
- **File**: `numeric.h:248-259`
- **Claim**: One-digit-at-a-time rounding produces wrong results when multiple digits must be rounded and there's a chain of 5s.
- **Verify**: Read numeric.h:245-260. Trace coef=14445, scale=30 through the loop.

#### R5-17: SET EX/PX non-atomic
- **File**: `resp.c:1366-1374`
- **Claim**: SET with EX/PX does resp_kv_set then separately resp_kv_set_expiry — not atomic. Concurrent operations can see wrong state or apply TTL to wrong value.
- **Verify**: Read resp.c:1360-1380. Check if set+expiry are atomic.

#### R5-18: NULL deref on keyspace OOM
- **File**: `resp.c:1552,1494,1413,1779`
- **Claim**: CAS-loop commands use `resp_kv` (which expands to current_shard->kv) without NULL guard after resp_kv_ensure fails.
- **Verify**: Read the cited lines. Check if NULL is guarded.

#### R5-19: resp_dump_to_tmp ignores fflush/fsync errors
- **File**: `resp.c:1176-1178`
- **Claim**: fflush and fsync return values unchecked. Disk full silently corrupts dump by renaming incomplete file over good db.
- **Verify**: Read resp.c:1170-1185.

### MEDIUM (JIT overflow checks)

#### R5-20: amd64 simple_tail_loop_eq missing jo overflow deopt
- **File**: `jit_amd64.h:601-604`
- **Claim**: The eq variant of simple_tail_loop lacks the jo deopt that the non-eq twin has (added in R4-19).
- **Verify**: Read jit_amd64.h:595-610. Compare with simple_tail_loop (line ~510).

#### R5-21: arm64 simple_tail_loop missing overflow check
- **File**: `jit_arm64.h:519-522`
- **Claim**: Uses non-flag-setting add_imm/sub_imm, no B.VS deopt. amd64 twin has jo.
- **Verify**: Read jit_arm64.h:515-525.

#### R5-22: arm64 simple_tail_loop_eq missing overflow check
- **File**: `jit_arm64.h:619-622`
- **Claim**: Same as R5-21 but in eq variant.
- **Verify**: Read jit_arm64.h:615-625.

#### R5-23: arm64 wide_counter_loop missing overflow check
- **File**: `jit_arm64.h:1489-1492`
- **Claim**: Uses non-flag-setting add_imm/sub_imm, no B.VS. amd64 twin has jo (added R4-19).
- **Verify**: Read jit_arm64.h:1485-1495.

### LOW severity

#### R5-24: flatten_into misleading comment (recursive not iterative)
- **File**: `builtins_stdlib.h:2600-2616`
- **Claim**: Comment says non-recursive but the function calls itself recursively.
- **Verify**: Read builtins_stdlib.h:2598-2618.

#### R5-25: defstruct silent name truncation at 127 chars
- **File**: `builtins_stdlib.h:700-702`
- **Claim**: `char nbuf[128]; snprintf` truncates without error.
- **Verify**: Read builtins_stdlib.h:695-705.

#### R5-26: random docstring/behavior mismatch
- **File**: `builtins_stdlib.h:2051-2073`
- **Claim**: Docstring says "64-bit value" but no-arg case returns 0.
- **Verify**: Read the docstring and implementation.

#### R5-27: vec I64 cells missing FIX_FITS on load/get
- **File**: `vector.h:178,199`
- **Claim**: load_vec_v2 loads raw int64 into I64 cells without validation. vec_get_boxed/vec_promote_to_gen use MAKE_FIX without FIX_FITS.
- **Verify**: Read vector.h:175-200 and persist.h load_vec_v2.

#### R5-28: NIL_EXP refcount leak in vec load error paths
- **File**: `persist.h:625-628, 664-668`
- **Claim**: On truncated read, cells [i+1, n-1] still hold refexp(NIL_EXP) refs that are never released.
- **Verify**: Read persist.h:620-635 and 658-670.

#### R5-29: reader multiple dots in number literal
- **File**: `reader.c:163-166`
- **Claim**: `1.2.3` silently parsed as 1.2 due to `||` instead of `&&` in dot guard.
- **Verify**: Read reader.c:160-168.

#### R5-30: reader CR not recognized as # comment whitespace
- **File**: `reader.c:488`
- **Claim**: `ifmatch(y, ' ', '\t', '\n')` omits '\r'.
- **Verify**: Read reader.c:485-492.

#### R5-31: CAS loops infinite-loop under sustained OOM
- **File**: `resp.c:1420-1450,1490-1525,1560-1575`
- **Claim**: lfkv_set_nx returns 0 for both "key exists" and "OOM", so CAS loops can't distinguish and retry forever on OOM.
- **Verify**: Read the CAS loop paths.

#### R5-32: cmd_set accepts duplicate EX/PX silently
- **File**: `resp.c:1295-1320`
- **Claim**: Both EX and PX can be specified; second overwrites silently. Redis rejects this.
- **Verify**: Read resp.c:1290-1325.

#### R5-33: *0 empty multibulk treated as protocol error
- **File**: `resp.c:728`
- **Claim**: `if (n <= 0) return -1` rejects *0 which Redis accepts as empty command.
- **Verify**: Read resp.c:725-735.

#### R5-34: memcpy NULL UB on 0-length LPOP
- **File**: `resp.c:1602-1642`
- **Claim**: malloc(0) may return NULL, then resp_write_bulk passes NULL to memcpy.
- **Verify**: Read resp.c:1598-1645.

#### R5-35: strtoll accepts leading whitespace in RESP args
- **File**: `resp.c:808-821`
- **Claim**: strtoll skips leading whitespace, so '  123' is accepted.
- **Verify**: Read resp.c:808-822.

#### R5-36: amd64/arm64 tail_loop_with_call missing overflow check
- **File**: `jit_amd64.h:1573-1576, jit_arm64.h:2358-2361`
- **Claim**: Missing jo/B.VS after tagged counter add/sub.
- **Verify**: Read the cited lines.

## Report format

For each finding, output:
```
R5-N: CONFIRMED/REFUTED/DOWNGRADED
Severity: HIGH/MEDIUM/LOW (adjusted if needed)
Reason: <brief explanation with code evidence>
Fix: <suggested fix if confirmed>
```

At the end, provide a summary count of confirmed/refuted/downgraded findings.
