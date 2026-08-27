# Mission: Adversarial Review of Round-7 Audit Findings

## Objective
Review each R7 finding below and determine whether it is a CONFIRMED real bug or a REFUTED false positive. For each confirmed finding, provide the correct fix. For each refuted finding, explain why it's not a bug.

## Working Directory
`/home/kader/Code/alcove`

## Findings to Review

### HIGH Severity

**R7-1 (HIGH): dec_div rounding overflow check always fires — REGRESSION from R6-27**
- File: `numeric.h:396`
- The expression `(((__int128)(((unsigned __int128)1 << 126) - 1)) + 1) * 2` evaluates to INT128_MIN (signed overflow at the `* 2` step, since 2^127 > INT128_MAX = 2^127-1). Since `q` is always non-negative, `q >= INT128_MIN` is ALWAYS true. This causes dec_div to ALWAYS return an overflow error whenever rounding is needed (remainder >= divisor - remainder), breaking all decimal division that produces non-exact results.
- Verify: Is this expression really INT128_MIN? Is `q` always non-negative? Does this break decimal division?
- Claimed fix: Remove the check entirely (q < 10^29 << INT128_MAX, so overflow is impossible), or use a correct constant.

**R7-2 (HIGH): ARM64 for_loop_inc missing loop exit condition — REGRESSION from R6-19**
- File: `jit_arm64.h:2812-2826`
- The comment says `loop_top: cmp i, n_max; b.gt done` but NO cmp or b.gt instructions are emitted at loop_top. The R6-19 fix (changing add_imm→adds_imm + B.VS deopt) accidentally dropped the cmp+b.gt. The loop runs until overflow deopt instead of stopping at n_max.
- Verify: Read the code at lines 2812-2826. Is there really no cmp/b.gt? Compare with the x86_64 twin at jit_amd64.h:3064-3070 which has `cmp rcx, rax; jg done`.
- Claimed fix: Add `arm64_cmp_reg(2, 1)` + `arm64_b_cond(ARM64_COND_GT, done_pc - ...)` at loop_top, before the adds_imm.

**R7-3 (HIGH): try_jit_tail_loop_with_call deopt missing frame restore**
- File: `jit_amd64.h:1595-1602` (x86_64), `jit_arm64.h:2389-2390` (ARM64 twin)
- The overflow deopt (jov_tlc) jumps to deopt_pc which does `zero rax; ret` without popping rbx, but the overflow happens AFTER the frame was established (push rbx at ~line 1548). This leaks 8 bytes on the stack.
- Verify: Read the deopt_pc code. Does it pop rbx? Is the overflow deopt reached only after the push? Are err_pc and end_pc different from deopt_pc?
- Claimed fix: Add a separate deopt_pc_framed that pops rbx before ret, and point jov_tlc to it.

**R7-4 (HIGH): lfkv_set tombstone reuse double-free + UAF — REGRESSION from R6-4**
- File: `lfkv.c:193-194`
- `free(old->key); free(old);` — `key` is a flexible array member (`char key[]`) embedded in the lfslot_t allocation, so `free(old->key)` frees a pointer into the middle of the allocation (UB). Also, the lfkv.h contract says slot records are never freed except at lfkv_destroy — another thread may be concurrently probing and holding a pointer to `old`.
- Verify: Read lfkv.c around line 193. Is key[] a flexible array member? Is there a concurrent probing risk? Read the lfkv.h contract comment.
- Claimed fix: Don't free the old slot. Instead of CAS-swapping the old tombstoned slot with a fresh allocation, just reuse the existing slot's memory: clear the tombstone flag, update the key/khash/klen, and set the new value. Or simply skip tombstone reuse entirely (the table still works, just with lower load factor after tombstones accumulate).

### MEDIUM Severity

**R7-5 (MEDIUM): loaddbcmd missing unrefexp(e) on error path**
- File: `alcove.c:4888-4893`
- On the `n < 0` error path, `error(...,e,...)` is called but `e` is never unrefexp'd. The *cmd function consumes `e`, so this leaks the call form.
- Verify: Read loaddbcmd. Is e unrefexp'd on the error path? Is it on the success path?

**R7-6 (MEDIUM): repeatcmd budget_check refcount leak**
- File: `builtins_stdlib.h:3320`
- On iteration 2+, when budget_check fires, `ret = error(...)` overwrites the previous iteration's result without unrefexp. Compare with whilecmd which correctly does `unrefexp(ret); ret = error(...)`.
- Verify: Read the budget_check block in repeatcmd. Is ret unrefexp'd before the error assignment?

**R7-7 (MEDIUM): forcmd budget_check refcount leak**
- File: `builtins_stdlib.h:3746`
- Same pattern as R7-6. On iteration 2+, budget_check overwrites ret without unrefexp.
- Verify: Read the budget_check block in forcmd. Is ret unrefexp'd before the error assignment?

**R7-8 (MEDIUM): applycmd crash on improper list (missing ispair check)**
- File: `builtins_stdlib.h:2359`
- `while (c && c->content)` without `ispair(c)` check. If args is an improper list (e.g. `(cons 1 2)`), `c->next` is a fixnum which is dereferenced as a struct pointer. Repro: `(apply + (cons 1 2))`.
- Verify: Read the list-walking loop. Does it use ispair? Compare with other list-walking loops in the file.

**R7-9 (MEDIUM): eachcmd crash on improper list (missing ispair check)**
- File: `builtins_stdlib.h:3833`
- Same pattern as R7-8. `while (retval && retval->content)` without ispair check. Repro: `(each x (cons 1 2) (prn x))`.
- Verify: Read the list-walking loop. Does it use ispair?

**R7-10 (MEDIUM): FFI AFFI_PTR MAKE_FIX without FIX_FITS (3 sites)**
- Files: `ffi.h:425` (closure dispatch), `ffi.h:1182` (call return), `ffi.h:875` (unpack)
- `MAKE_FIX((int64_t)(uintptr_t)ptr)` without FIX_FITS check. Pointers with bits 48-60 set (5-level paging, kernel pointers) would silently wrap.
- Verify: Check each site. Does it use FIX_FITS? Compare with AFFI_INT/AFFI_LONG which were fixed in R6-10/R6-11/R6-12.

**R7-11 (MEDIUM): vec_promote_to_gen I64→GEN truncates without FIX_FITS**
- File: `vector.h:178`
- `MAKE_FIX(src[start + i])` without FIX_FITS check. I64 vectors can hold out-of-fixnum-range values (per R6-14). Compare with vec_get_boxed which correctly checks FIX_FITS.
- Verify: Read vec_promote_to_gen. Does it check FIX_FITS? Compare with vec_get_boxed.

**R7-12 (MEDIUM): cmd_set returns OK on table-full**
- File: `resp.c:1382-1395`
- When `lfkv_set` returns -1 (table full), `resp_kv_set` unrefs fresh and returns, but cmd_set continues to `resp_write_simple(c, "OK")`. Client thinks data was stored.
- Verify: Read cmd_set. Does it check the return of resp_kv_set? What happens when lfkv_set fails?

**R7-13 (MEDIUM): reader quote/quasiquote/unquote bury EOF errors**
- File: `reader.c:314-316, 322-324, 336-338`
- When `reader(stream, 0, 0)` returns EOF error, it's wrapped inside `(quote <error>)` and returned as a pair. The caller checks `iserror(ret)` but a pair wrapping an error is NOT an error.
- Verify: Read the quote reader. Does it check iserror on the inner reader result before wrapping?

**R7-14 (MEDIUM): epoch_retire OOM path use-after-free**
- File: `epoch.c:89-93`
- When malloc fails, `freer(ptr)` is called directly. If a peer reactor holds the pointer, this is UAF. The comment says "OOM is fatal" but alcove has OOM recovery (g_oom_jmp/longjmp).
- Verify: Read epoch_retire. What happens on malloc failure? Does alcove have OOM recovery?

### LOW Severity

**R7-15 (LOW): print.h dict key escape incomplete**
- File: `print.h:209-214`
- Dict key escape only handles `"` and `\`, not control characters (`\n`, `\t`, `\r`). Compare with EXP_STRING escape (lines 146-160) which handles all control chars.
- Verify: Compare the dict key escape loop with the string escape loop.

**R7-16 (LOW): msgpack mp_put_sized str32 truncation**
- File: `msgpack.h:92-94`
- `mp_put_be(m, n, 4)` silently truncates size_t n to 32 bits. If n > 0xffffffff, the encoded length prefix is corrupt.
- Verify: Read mp_put_sized. Is there a bounds check on n for the 32-bit tier?

**R7-17 (LOW): repeat/for int n overflow in sort**
- File: `builtins_stdlib.h:2683, 2736`
- `int n = 0;` overflow if list has > INT_MAX elements. `memalloc(n, ...)` allocates too little, then `arr[i++] = c->content` writes out of bounds.
- Verify: Read sortcmd/sortbycmd. Is n checked for overflow?

**R7-18 (LOW): isequal missing NULL check for rational/decimal ptr**
- File: `builtins_stdlib.h:3463-3465`
- `isrational`/`isdecimal` cases dereference `cur->ptr` without NULL check, unlike `isblob` which checks `a && b`.
- Verify: Compare the isblob case with isrational/isdecimal.

**R7-19 (LOW): make_string strlen without (int) cast**
- File: `builtins_stdlib.h:549`
- `make_string((char *)name, strlen(name))` — strlen returns size_t, make_string takes int. Implicit narrowing UB if > INT_MAX.
- Verify: Check the call. Do sibling calls use (int) cast?

**R7-20 (LOW): FFI strnlen 16MB cap truncation**
- File: `ffi.h:421, 1176`
- `strnlen(s, 1u << 24)` caps FFI strings at 16MB. Longer strings silently truncated.
- Verify: Read the strnlen calls.

**R7-21 (LOW): OP_TAIL_CALL doesn't update env->callingfnc**
- File: `compiler_impl.h:2985`
- After cross-function tail call, callingfnc still points to old call form. Debugger display inaccuracy only.
- Verify: Read the OP_TAIL_CALL handler. Is callingfnc updated?

**R7-22 (LOW): strdup NULL dereference in dlopen cache on OOM**
- File: `ffi.h:174`
- `c->name = strdup(name)` can return NULL on OOM. Next cache lookup `strcmp(c->name, name)` dereferences NULL.
- Verify: Read the dlopen cache code. Is strdup result checked?

**R7-23 (LOW): lfkv bernstein_hash klen int truncation**
- File: `lfkv.c:49`
- `bernstein_hash((unsigned char *)(k), (int)(klen))` — klen is size_t, cast to int. If klen > INT_MAX, hash is wrong.
- Verify: Read the LFKV_PROBE macro. Is klen cast to int?

**R7-24 (LOW): vec_write_double I64 path UB on overflow**
- File: `vector.h:286`
- `(int64_t)x` is UB when x exceeds INT64 range. Currently unreachable but latent.
- Verify: Read vec_write_double I64 path.

## Review Instructions
For each finding:
1. Read the actual code at the specified location
2. Determine if the bug is real (CONFIRMED) or not (REFUTED)
3. For confirmed bugs, describe the correct fix
4. For refuted findings, explain why the code is actually safe

## Output Format
For each finding, output:
```
R7-N: CONFIRMED/REFUTED
Reason: [explanation]
Fix: [if confirmed, describe the fix]
```

## Non-Goals
- Do NOT edit any files
- Do NOT run build or test commands
- Do NOT commit anything
