# Adversarial Review of Audit Round 3 Findings

## Objective

Adversarially verify each of the 17 findings below. For each finding:
1. **Confirm or refute** that the bug exists in the current code at HEAD (commit c712972).
2. **Assess severity** — is the stated severity correct? Too high? Too low?
3. **Check reachability** — can the bug actually be triggered by user code, or is it blocked by an upstream guard?
4. **Check for false positives** — is the code actually correct despite the finding? Did the auditor miss a guard?
5. **Suggest the correct fix** if the finding is confirmed.

Be skeptical. The auditor may have misread the code, missed an upstream guard, or confused signed/unsigned. Check the ACTUAL code at the cited lines.

## Working directory

`/home/kader/Code/alcove`

## Findings to verify

### HIGH severity

#### R3-1 / V1: msgpack unsigned int sign-extension
- **File**: `msgpack.h:263-276`
- **Claim**: The consolidated uint decode path (0xcc-0xcf) uses `mp_sext(uv, nb)` for nb<8, which sign-extends. uint8 255 → -1, uint16 65535 → -1.
- **Verify**: Read the actual code at msgpack.h:260-278. Check whether `mp_sext` is used for unsigned formats. Test: `(msgpack-decode (msgpack-encode 255))` should return -1 if the bug is real.
- **Also check**: Was the original code (before the round-1 fix that consolidated 0xcc-0xcf into one case) correct? The original had separate `(int64_t)` casts for 0xcc/0xcd/0xce and `(int64_t)mp_get_be(...)` for 0xcf.

#### R3-2: tcp-send SIGPIPE kills process
- **File**: `builtins_os.h:716-719`
- **Claim**: `send(fd, data, len, 0)` without MSG_NOSIGNAL. SIGPIPE only ignored in RESP reactor (resp.c:2377), not globally.
- **Verify**: Read builtins_os.h around line 716. Check if SIGPIPE is ignored globally in main() or alcove_init(). Check if the send() call uses 0 or MSG_NOSIGNAL.

### MEDIUM severity

#### R3-3: arm64 JIT negative step sign error
- **File**: `jit_arm64.h:2786-2801` (for_loop_inc), `jit_arm64.h:1745-1750` (recurse_mul_one)
- **Claim**: These use `|K_step|` (absolute value) and always emit add for OP_SLOT_ADD_FIX, sub for OP_SLOT_SUB_FIX. For negative K (e.g. `s += -5` = `s - 5`), it emits `add x3, x3, 5` (wrong).
- **Verify**: Read the actual arm64 code. Check what `K_step_s` is (is it the raw signed constant?). Check whether the amd64 twin handles this differently. Check whether negative step constants are actually reachable (does the shape matcher reject them?).

#### R3-4: JIT for_loop_inc missing fixnum overflow check
- **File**: `jit_amd64.h:3054-3067`, `jit_arm64.h:2797-2803`
- **Claim**: Loop accumulator `s += K_step` has no overflow check, but the VM's SLOT_ADD_FIX raises on overflow.
- **Verify**: Read the actual JIT code. Check if there's a `jo` (amd64) or `b.vs` (arm64) after the add. Check what the VM does for OP_SLOT_ADD_FIX. Is this reachable with values that actually overflow?

#### R3-5: gen-range fixnum overflow → infinite loop
- **File**: `builtins_control.h:327`
- **Claim**: `MAKE_FIX(cur + stp)` without FIX_FITS check. When cur+stp exceeds 61-bit range, the wrapped cursor is far below end → infinite loop.
- **Verify**: Read builtins_control.h:320-340. Check if there's a FIX_FITS guard. Check if `cur` and `stp` are fixnums (61-bit). Can `cur + stp` actually overflow given that both are fixnums? What's the max fixnum value? Can a user create a gen-range with start near 2^60?

#### R3-6: GK_MAP/GK_FILTER inner generator UAF (theoretical)
- **File**: `builtins_control.h:333-345, 354`
- **Claim**: `inner` borrowed from generator dict without refexp() before alc_gen_step(inner, env). fn/pred ARE protected with refexp().
- **Verify**: Read the code. Check if inner is actually borrowed (gen_dict_get returns a borrowed pointer?). Check if user code during alc_gen_step could drop the last ref to the generator dict. Is this actually reachable?

#### R3-7: lfkv OOM return mishandled by resp.c callers
- **File**: `lfkv.c:440,388,464` → `resp.c:1428,1506,1554,1601`
- **Claim**: lfkv_cas/lfkv_set_nx/lfkv_set_xx return -1 on OOM, but callers use `if (!ok)` which treats -1 as success.
- **Verify**: Read lfkv.c to check what the functions return on OOM. Read resp.c callers to check how they handle the return value. Is -1 actually treated as success?

#### R3-8: Unchecked strdup in regex cache
- **File**: `builtins_regex.h:51-53`
- **Claim**: `slot->pat = strdup(pat)` without NULL check. On OOM, regex_t is leaked and cache is defeated.
- **Verify**: Read the code. Is strdup actually unchecked? What happens on NULL?

#### R3-9: Unchecked strdup in make-dir
- **File**: `builtins_os.h:173-174`
- **Claim**: `char *tmp = strdup(path)` without NULL check. Next line dereferences NULL+1.
- **Verify**: Read the code. Is strdup actually unchecked?

### LOW severity

#### R3-10/V3: make_rational INT64_MIN den stays negative
- **File**: `numeric.h:49-57`
- **Claim**: Divides by 2 but doesn't negate, leaving den negative.
- **Verify**: Read the code. Is den actually negative after the /2? Is this reachable?

#### R3-11: tak/ackermann JIT modifies env before self-call
- **File**: `jit_amd64.h:2689-2710, 2900-2920`
- **Claim**: Env slots overwritten before recursive calls; bail path doesn't restore.
- **Verify**: Is this actually reachable? Check if deopt can occur for valid fixnum inputs.

#### R3-12: count_primes/mark_from JIT missing overflow check
- **File**: `jit_amd64.h:2490-2498, 2597-2603`
- **Claim**: Tagged increment without jo. Unreachable due to sieve bounds.
- **Verify**: Confirm it's unreachable.

#### R3-13: match guard NULL → nil error swallowing
- **File**: `builtins_control.h:143-148`
- **Claim**: alc_apply1 returning NULL stores NIL_EXP instead of proper error.
- **Verify**: Read the code. Is this actually what happens?

#### R3-14: var2env rest-param leak (latent)
- **File**: `builtins_control.h:731-736`
- **Claim**: Error path doesn't free rest_head. Currently unreachable.
- **Verify**: Confirm it's unreachable (var2env only called with evalexp=false?).

#### R3-15: regex group truncation beyond 32
- **File**: `builtins_regex.h:108-116`
- **Claim**: Patterns with >31 capture groups silently drop groups.
- **Verify**: Read the code. Is RE_MAX_GROUPS 32? Is the clamp silent?

#### R3-16: tcp-send partial send not retried
- **File**: `builtins_os.h:684-690`
- **Claim**: send() may return fewer bytes than requested.
- **Verify**: Confirm the return value is the actual bytes sent (honest but potentially misleading).

## Required output format

For each finding, output:

```
### Finding R3-N: [title]
**Verdict**: CONFIRMED / REFUTED / PARTIALLY CONFIRMED
**Severity assessment**: [correct / should be HIGHER / should be LOWER]
**Reachability**: [actually reachable / blocked by upstream guard / theoretical only]
**Evidence**: [exact file:line, quoted code]
**Notes**: [any additional context, false positive explanation, or correction]
```

## Constraints
- READ-ONLY. Do not edit any files.
- Check the ACTUAL code at HEAD c712972, not what you assume.
- Be adversarial — try to prove findings WRONG. False positives are common in audits.
- If a finding is a false positive, say so clearly and explain why.
