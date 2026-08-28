# Mission: Adversarial Review of Round-8 Audit Findings

## Objective
Review each R8 finding below and determine whether it is a CONFIRMED real bug or a REFUTED false positive. For each confirmed finding, provide the correct fix. For each refuted finding, explain why it's not a bug.

## Working Directory
`/home/kader/Code/alcove`

## Findings to Review

### HIGH Severity

**R8-1 (HIGH): updatebang NULL deref in car/cdr place-form**
- File: `alcove.c:3059-3078`
- When EVAL(cadr(keyv), env) returns NULL (place form has no argument, e.g. `(= (car) 5)`), iserror(NULL) is false, then `key->content` (line 3065) or `key->next` (line 3073) is dereferenced through NULL pointer → crash.
- Verify: Read updatebang's car/cdr branches. Does it check for NULL from EVAL before dereferencing key?

### MEDIUM Severity

**R8-2 (MEDIUM): whilecmd ref leak — last body value leaked on every iteration**
- File: `builtins_stdlib.h:3143-3168`
- The while condition `istrue(ret = EVAL(val, env))` overwrites `ret` (which holds the last body form's value from the previous iteration) without unreffing it. R7 fixed this for repeat/for but whilecmd was missed.
- Verify: Read whilecmd. Does the while-condition EVAL overwrite ret without unref? Compare with the body do-while which correctly unrefexp(ret).

**R8-3 (MEDIUM): rat_binop unchecked __int128 overflow in +/-**
- File: `numeric.h:130,133`
- `num = (__int128)an * bd + (__int128)bn * ad` — each product fits __int128 but the sum can reach 2^127, exceeding INT128_MAX. Signed overflow = UB. Reachable with large coprime rationals.
- Verify: Read rat_binop. Does it use i128_add_ovf like dec_addsub? Is the overflow reachable?

**R8-4 (MEDIUM): rat_from_i128 negation UB when num == INT128_MIN**
- File: `numeric.h:85`
- GCD computation does `__int128 a = num < 0 ? -num : num`. If num == INT128_MIN, -num overflows. Reachable as consequence of R8-3.
- Verify: Read rat_from_i128. Does it guard against INT128_MIN before negation?

**R8-5 (MEDIUM): lfkv tombstone exhaustion after R7-4 fix**
- File: `lfkv.c:168-219`
- After R7-4 removed tombstone slot reuse, tombstoned slots are never reclaimed for new keys. FLUSHDB cycles with distinct keys exhaust the table. SET returns "table full" permanently.
- Verify: Read probe() and lfkv_set. Can a new key claim a tombstoned slot? What happens after FLUSHDB + reinsert with different keys?

**R8-6 (MEDIUM): is_acmd swallows error from first argument**
- File: `builtins_stdlib.h:621-626`
- If `(is-a? bad-expr SomeType)` evaluates bad-expr to an error, x holds the error but is never checked with iserror(). The user sees a misleading error about t not being a type.
- Verify: Read is_acmd. Does it check iserror(x) before proceeding?

**R8-7 (MEDIUM): JIT recurse_add_two ref leak on second call bail**
- File: `jit_amd64.h:1957-1966`
- When the second recursive call returns non-fixnum, bail_pc discards [rsp+0] (first call's result) without unref. The first call's result is leaked.
- Verify: Read the bail_pc code. Is [rsp+0] (saved call1 result) unref'd before `add rsp, 16`?

### LOW Severity

**R8-8 (LOW): updatebang string-index branch swallows error**
- File: `alcove.c:3081-3085`
- EVAL(key, env) can return an error from an unbound symbol, but it's not checked for iserror before isstring test. The error is silently replaced with "unsupported place form".
- Verify: Read the string-index branch. Is iserror checked?

**R8-9 (LOW): JIT iterative fib missing overflow check on b = old_a + b**
- File: `jit_amd64.h:1827-1829`, `jit_arm64.h:1647`
- The `add` has no overflow check. If old_a + b overflows int64 but the wrapped value lands in fixnum range, a wrong result is returned instead of deopting.
- Verify: Is there a `jo`/`B.VS` after the add? Is the re-tag check sufficient?

**R8-10 (LOW): JIT ackermann missing overflow check on m==0 return (n+1)**
- File: `jit_amd64.h:2884-2885`, `jit_arm64.h:2588`
- `n + 1` (tagged as n+8) has no overflow check. If n is near 2^60, n+1 exceeds fixnum range.
- Verify: Is there a `jo`/`B.VS` after the add?

**R8-11 (LOW): JIT ackermann/tak bail returns NULL with modified env**
- File: `jit_amd64.h:2916-2920,2945-2949`, `jit_arm64.h:2615-2616,2643-2647`
- On deopt from deeper frame, bail returns NULL but env slots were already modified (slot_n = n-1). vm_run re-runs with wrong env.
- Verify: Are env slots modified before the inner call? Does bail restore them?

**R8-12 (LOW): JIT mark_from missing overflow check on tagged step add**
- File: `jit_amd64.h:2627-2631`, `jit_arm64.h:2263-2266`
- `j + step` (tagged add) has no overflow check. If it overflows, wrong j value is stored → infinite loop or wrong memory writes.
- Verify: Is there a `jo`/`B.VS` after the add?

**R8-13 (LOW): make_decimal_raw technically-UB signed shift**
- File: `numeric.h:258`
- `(__int128)1 << 127` is UB per C11 §6.5.7 (shifting into sign bit). Works on GCC/Clang but not standard-guaranteed.
- Verify: Read the check. Is it a signed shift?

**R8-14 (LOW): reader #b blob truncates at embedded NUL**
- File: `reader.c:539`
- `make_blob(sb ? sb : "", sb ? strlen(sb) : 0)` uses strlen on string with embedded NUL from \x00 escape. #b"\x00\x01\x02" produces a 0-byte blob.
- Verify: Read the #b reader. Does it use strlen?

**R8-15 (LOW): savedbcmd UAF if strdup fails + I/O error**
- File: `alcove.c:4777`
- path_snap = strdup(path) returns NULL on OOM. On fopen failure, unrefexp(path_arg) frees path before error() uses it.
- Verify: Read savedbcmd. Is path_snap NULL-checked before using path in error()?

**R8-16 (LOW): FFI NaN/Inf→integer cast UB**
- File: `ffi.h:1074,1078,456-457`
- `(int32_t)a->f` or `(int64_t)a->f` is UB when a->f is NaN/Inf per C11 §6.3.1.4.
- Verify: Is there a finite check before the cast?

**R8-17 (LOW): RESP unbounded CAS retry loops**
- File: `resp.c:1527,1587,1627,1802,1872`
- COW commands (LPUSH/LPOP/HSET/HDEL/APPEND) use infinite for(;;) CAS retry with no limit, unlike resp_apply_incr's 1000 cap.
- Verify: Is there a retry counter in these loops?

**R8-18 (LOW): mapcmd docstring mismatch**
- File: `builtins_stdlib.h:2091`
- Doc says "(map fn xs ...)" accepts multiple lists but implementation only handles one.
- Verify: Does mapcmd accept multiple list arguments?

## Review Instructions
For each finding:
1. Read the actual code at the specified location
2. Determine if the bug is real (CONFIRMED) or not (REFUTED)
3. For confirmed bugs, describe the correct fix
4. For refuted findings, explain why the code is actually safe

## Output Format
For each finding, output:
```
R8-N: CONFIRMED/REFUTED
Reason: [explanation]
Fix: [if confirmed, describe the fix]
```

## Non-Goals
- Do NOT edit any files
- Do NOT run build or test commands
- Do NOT commit anything
