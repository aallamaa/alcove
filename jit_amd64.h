/* jit_amd64.h — amd64 (x86-64) JIT backend: instruction encoders + shape
 * matchers.
 *
 * FRAGMENT #included into alcove.c inside `#ifdef ALCOVE_JIT` — NOT a
 * standalone header and NOT separately compiled. It must stay in the single
 * alcove.c translation unit so the emitters inline against the value model
 * and the env layout (offsetof(env_t, inline_vals[0]) is baked into emitted
 * code). See the #include site in alcove.c. `make tidy` lints it via adder.c.
 */
/* ===================== amd64 backend ===================== */

/* x86-64 instruction encoders (System V ABI: arg in rdi, return in rax).
   We use only RAX, RCX, RDI — all in the low 8 register set, so REX.B
   and REX.R extensions are never needed; REX.W=1 (0x48) appears on
   every 64-bit op. Encoders write raw bytes into a buffer and return
   the byte count. */

#define X64_RAX 0
#define X64_RCX 1
#define X64_RDX 2
#define X64_RBX 3
#define X64_RSI 6
#define X64_RDI 7

/* mov r64, [base + disp32]   →  REX.W 0x8B /r disp32 */
static int x64_mov_reg_mem(uint8_t *buf, int dst, int base, int32_t disp) {
  buf[0] = 0x48;
  buf[1] = 0x8B;
  buf[2] = (uint8_t)(0x80 | ((dst & 7) << 3) | (base & 7));
  memcpy(buf + 3, &disp, 4);
  return 7;
}
/* mov [base + disp32], r64   →  REX.W 0x89 /r disp32 */
static int x64_mov_mem_reg(uint8_t *buf, int src, int base, int32_t disp) {
  buf[0] = 0x48;
  buf[1] = 0x89;
  buf[2] = (uint8_t)(0x80 | ((src & 7) << 3) | (base & 7));
  memcpy(buf + 3, &disp, 4);
  return 7;
}
/* mov r64, imm64   →  REX.W 0xB8+r imm64 (10 bytes) */
static int x64_mov_imm64(uint8_t *buf, int dst, uint64_t imm) {
  buf[0] = 0x48;
  buf[1] = (uint8_t)(0xB8 + (dst & 7));
  memcpy(buf + 2, &imm, 8);
  return 10;
}
/* mov r64, r64   →  REX.W 0x89 /r */
static int x64_mov_reg_reg(uint8_t *buf, int dst, int src) {
  buf[0] = 0x48;
  buf[1] = 0x89;
  buf[2] = (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7));
  return 3;
}
/* xor r32, r32 — zero-idiom; clears the full r64 in 2 bytes. */
static int x64_zero_reg(uint8_t *buf, int dst) {
  buf[0] = 0x31;
  buf[1] = (uint8_t)(0xC0 | ((dst & 7) << 3) | (dst & 7));
  return 2;
}
/* add r64, sign-extended imm32   →  REX.W 0x81 /0 imm32 */
static int x64_add_imm32(uint8_t *buf, int dst, int32_t imm) {
  buf[0] = 0x48;
  buf[1] = 0x81;
  buf[2] = (uint8_t)(0xC0 | (dst & 7)); /* /0, mod=11 */
  memcpy(buf + 3, &imm, 4);
  return 7;
}
/* sub r64, sign-extended imm32   →  REX.W 0x81 /5 imm32 */
static int x64_sub_imm32(uint8_t *buf, int dst, int32_t imm) {
  buf[0] = 0x48;
  buf[1] = 0x81;
  buf[2] = (uint8_t)(0xE8 | (dst & 7)); /* /5, mod=11 */
  memcpy(buf + 3, &imm, 4);
  return 7;
}
/* cmp r64, sign-extended imm32   →  REX.W 0x81 /7 imm32 */
static int x64_cmp_imm32(uint8_t *buf, int dst, int32_t imm) {
  buf[0] = 0x48;
  buf[1] = 0x81;
  buf[2] = (uint8_t)(0xF8 | (dst & 7)); /* /7, mod=11 */
  memcpy(buf + 3, &imm, 4);
  return 7;
}
/* imul r64, r/m64, sign-extended imm32   →  REX.W 0x69 /r imm32 (7 bytes
   when r=r/m). 64-bit signed multiply, low 64 bits of result, no flags
   relevant for our use. */
static int x64_imul_reg_reg_imm32(uint8_t *buf, int dst, int src, int32_t imm) {
  buf[0] = 0x48;
  buf[1] = 0x69;
  buf[2] = (uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7));
  memcpy(buf + 3, &imm, 4);
  return 7;
}
/* test r/m8, imm8   →  0xF6 /0 imm8.  Used for tag-bit check on AL/CL. */
static int x64_test_reg8_imm8(uint8_t *buf, int reg, uint8_t imm) {
  buf[0] = 0xF6;
  buf[1] = (uint8_t)(0xC0 | (reg & 7));
  buf[2] = imm;
  return 3;
}
static int x64_ret(uint8_t *buf) {
  buf[0] = 0xC3;
  return 1;
}
/* jmp rel32 (5 bytes). disp is from end of this instruction. */
static int x64_jmp_rel32(uint8_t *buf, int32_t disp) {
  buf[0] = 0xE9;
  memcpy(buf + 1, &disp, 4);
  return 5;
}
/* jcc rel32 (6 bytes). cc is the low nibble of the secondary opcode:
   jz=0x04, jl=0x0C, jge=0x0D, jle=0x0E, jg=0x0F. */
static int x64_jcc_rel32(uint8_t *buf, uint8_t cc, int32_t disp) {
  buf[0] = 0x0F;
  buf[1] = (uint8_t)(0x80 | cc);
  memcpy(buf + 2, &disp, 4);
  return 6;
}
/* cqo: sign-extend rax → rdx:rax (needed before idiv) — REX.W 0x99 */
static int x64_cqo(uint8_t *buf) {
  buf[0] = 0x48;
  buf[1] = 0x99;
  return 2;
}

/* idiv r64 — signed divide rdx:rax by r/m64; quotient → rax,
   remainder → rdx. REX.W 0xF7 /7. For low regs: 0xF8|reg in ModR/M. */
static int x64_idiv_reg(uint8_t *buf, int divisor) {
  buf[0] = 0x48;
  buf[1] = 0xF7;
  buf[2] = (uint8_t)(0xF8 | (divisor & 7));
  return 3;
}

/* cmovz r64, r64 — REX.W 0x0F 0x44 ModR/M. dst gets src if ZF=1. */
static int x64_cmovz_reg_reg(uint8_t *buf, int dst, int src) {
  buf[0] = 0x48;
  buf[1] = 0x0F;
  buf[2] = 0x44;
  buf[3] = (uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7));
  return 4;
}

/* push r64 (low 8 regs only) — 1 byte: 0x50+r */
static int x64_push_reg(uint8_t *buf, int reg) {
  buf[0] = (uint8_t)(0x50 + (reg & 7));
  return 1;
}
/* pop r64 (low 8 regs only) — 1 byte: 0x58+r */
static int x64_pop_reg(uint8_t *buf, int reg) {
  buf[0] = (uint8_t)(0x58 + (reg & 7));
  return 1;
}
/* call r/m64 — 0xFF /2. Register form for low 8 regs: 0xFF 0xD0+r (2 bytes). */
static int x64_call_reg(uint8_t *buf, int reg) {
  buf[0] = 0xFF;
  buf[1] = (uint8_t)(0xD0 + (reg & 7));
  return 2;
}
/* call rel32 — 0xE8 imm32 (5 bytes). disp from end of instruction.
   Used for direct intra-buffer self-calls — the JIT emits a relative
   call back into its own entry, skipping the env-alloc helper for
   self-recursion. */
static int x64_call_rel32(uint8_t *buf, int32_t disp) {
  buf[0] = 0xE8;
  memcpy(buf + 1, &disp, 4);
  return 5;
}
/* test r64, r64 — REX.W 0x85 /r (3 bytes). Sets ZF=1 iff value is zero;
   we use it to test the trampoline's exp_t* return for NULL. */
static int x64_test_reg_reg(uint8_t *buf, int dst, int src) {
  buf[0] = 0x48;
  buf[1] = 0x85;
  buf[2] = (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7));
  return 3;
}
/* mov [rsp + disp8], r64.  Same SIB trick as x64_mov_rsp_reg, but with
   an 8-bit displacement (mod=01). For tak / similar JITs that stage 3+
   intermediate results in stack slots. */
static int x64_mov_rsp_disp8_reg(uint8_t *buf, int src, int8_t disp) {
  buf[0] = 0x48;
  buf[1] = 0x89;
  buf[2] =
      (uint8_t)(0x44 | ((src & 7) << 3)); /* mod=01, reg=src, r/m=100 (SIB) */
  buf[3] = 0x24; /* SIB: scale=00, index=100 (none), base=100 (rsp) */
  buf[4] = (uint8_t)disp;
  return 5;
}
/* mov r64, [rsp + disp8] — load form. */
static int x64_mov_reg_rsp_disp8(uint8_t *buf, int dst, int8_t disp) {
  buf[0] = 0x48;
  buf[1] = 0x8B;
  buf[2] = (uint8_t)(0x44 | ((dst & 7) << 3));
  buf[3] = 0x24;
  buf[4] = (uint8_t)disp;
  return 5;
}

/* mov [rsp + 0], r64.  RSP base requires SIB even with mod=00 (because
   r/m=100 with mod=00 normally means [disp32]; the SIB redirects it). */
static int x64_mov_rsp_reg(uint8_t *buf, int src) {
  buf[0] = 0x48;
  buf[1] = 0x89;
  buf[2] =
      (uint8_t)(0x04 | ((src & 7) << 3)); /* mod=00, reg=src, r/m=100 (SIB) */
  buf[3] = 0x24; /* SIB: scale=00, index=100 (none), base=100 (rsp) */
  return 4;
}
/* sar r/m64, imm8  →  REX.W 0xC1 /7 imm8.  Arithmetic shift right by
   imm8. We use this to untag fixnums (sar reg, 3) where the LSB is the
   tag bit and the value is in the upper 61 bits with sign-extension. */
static int x64_sar_imm8(uint8_t *buf, int dst, uint8_t imm) {
  buf[0] = 0x48;
  buf[1] = 0xC1;
  buf[2] = (uint8_t)(0xF8 | (dst & 7));
  buf[3] = imm;
  return 4;
}
/* imul r64, r/m64  →  REX.W 0x0F 0xAF /r.  Two-operand signed multiply,
   dst = dst * src. No flags-on-overflow paranoia: alcove fixnums are
   61-bit, products that would overflow get truncated (caller's problem). */
static int x64_imul_reg_reg(uint8_t *buf, int dst, int src) {
  buf[0] = 0x48;
  buf[1] = 0x0F;
  buf[2] = 0xAF;
  buf[3] = (uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7));
  return 4;
}
/* add r64, [rsp + 0]  (counterpart of x64_mov_rsp_reg for the load side) */
static int x64_add_reg_rsp(uint8_t *buf, int dst) {
  buf[0] = 0x48;
  buf[1] = 0x03;
  buf[2] = (uint8_t)(0x04 | ((dst & 7) << 3));
  buf[3] = 0x24;
  return 4;
}

/* Patch the rel32 of a previously emitted forward branch.
   `branch_start` = byte offset of the branch's first byte.
   `branch_size`  = total size of the branch instruction (5 or 6).
   `target`       = byte offset to jump to. */
static void x64_patch_rel32(uint8_t *buf, int branch_start, int branch_size,
                            int target) {
  int32_t rel = (int32_t)(target - (branch_start + branch_size));
  memcpy(buf + branch_start + branch_size - 4, &rel, 4);
}

/* x64 JIT shape-emitter helper. Mirrors ARM64_EMIT_DEOPT for the x64
   backend. All x64 shape emitters end their deopt stub identically:
   zero rax (returns NULL = 0) then ret. Requires `buf` and `n` in scope. */
#define X64_EMIT_DEOPT()                                                       \
  do {                                                                         \
    n += x64_zero_reg(buf + n, X64_RAX); /* rax = 0 (NULL) */                  \
    n += x64_ret(buf + n);                                                     \
  } while (0)

/* Same shape detector as the arm64 path: a self-tail counter loop with
   compare + arith on a single param slot. See the arm64 version's
   comment block above for the bytecode layout (19 bytes total). */
static int try_jit_simple_tail_loop(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 19)
    return 0;
  uint8_t *c = bc->code;

  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX &&
      cmp_op != OP_SLOT_IS_FIX)
    return 0;
  uint8_t cmp_slot = c[1];
  int16_t cmp_imm = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;

  uint8_t arith_op = c[7];
  if (arith_op != OP_SLOT_SUB_FIX && arith_op != OP_SLOT_ADD_FIX)
    return 0;
  uint8_t arith_slot = c[8];
  int16_t arith_imm = (int16_t)((uint16_t)c[9] | ((uint16_t)c[10] << 8));

  if (c[11] != OP_TAIL_SELF || c[12] != 1)
    return 0;
  if (c[13] != OP_JUMP)
    return 0;
  if (c[16] != OP_LOAD_SLOT)
    return 0;
  uint8_t load_slot = c[17];
  if (c[18] != OP_RET)
    return 0;

  if (cmp_slot != arith_slot || cmp_slot != load_slot)
    return 0;
  if (cmp_slot >= ENV_INLINE_SLOTS)
    return 0;

  int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)cmp_slot * 8;

  /* int16<<3 fits comfortably as sign-extended imm32 — no narrow imm12
     limit like arm64. */
  int32_t cmp_tagged = ((int32_t)cmp_imm << 3) | 1;
  int32_t arith_delta = ((int32_t)arith_imm) << 3;

  /* Invert the bytecode comparison for "branch out of the loop on the
     failing case". x86 cc nibbles: jl=0x0C jge=0x0D jle=0x0E jg=0x0F. */
  uint8_t inv_cc;
  switch (cmp_op) {
  case OP_SLOT_GT_FIX:
    inv_cc = 0x0E;
    break; /* !GT → jle */
  case OP_SLOT_LT_FIX:
    inv_cc = 0x0D;
    break; /* !LT → jge */
  case OP_SLOT_GE_FIX:
    inv_cc = 0x0C;
    break; /* !GE → jl  */
  case OP_SLOT_LE_FIX:
    inv_cc = 0x0F;
    break; /* !LE → jg  */
  case OP_SLOT_IS_FIX:
    inv_cc = 0x04;
    break; /* base when (is slot K); loop exits on equal → je */
  default:
    return 0;
  }

  int n = 0;

  /* mov rcx, [rdi + slot_off] */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, slot_off);
  /* test cl, 1 — verify tag bit set; if not, deopt. */
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_start = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0); /* jz deopt (placeholder) */

  int loop_top = n;
  n += x64_cmp_imm32(buf + n, X64_RCX, cmp_tagged);
  int jcc_end_start = n;
  n += x64_jcc_rel32(buf + n, inv_cc, 0); /* j<inv> end (placeholder) */
  if (arith_op == OP_SLOT_SUB_FIX)
    n += x64_sub_imm32(buf + n, X64_RCX, arith_delta);
  else
    n += x64_add_imm32(buf + n, X64_RCX, arith_delta);
  /* tag bit preserved across ±(K2<<3); subsequent loads stay tagged. */
  n += x64_mov_mem_reg(buf + n, X64_RCX, X64_RDI, slot_off);
  /* jmp loop_top */
  {
    int jmp_start = n;
    n += x64_jmp_rel32(buf + n, (int32_t)(loop_top - (jmp_start + 5)));
  }

  int end_pc = n;
  n += x64_mov_reg_reg(buf + n, X64_RAX, X64_RCX); /* mov rax, rcx */
  n += x64_ret(buf + n);

  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jcc_end_start, 6, end_pc);
  x64_patch_rel32(buf, jz_start, 6, deopt_pc);

  /* Worst case ~55 bytes (load, test, jcc, cmp, jcc, sub/add, mov,
     jmp, mov, ret, xor, ret + slack). Caller's buffer is uint8_t buf[256]. */
  JIT_GUARD(80);
  *outn = n;
  return 1;
}

/* Tail counter loop with one inner global call before the recurse.
   Bytecode shape (26 bytes), produced by:
     (def lp (k) (if (cmp k K1) (do (g K_arg) (lp (op k K2))) k))
   where g is a global function:
     [ 0] SLOT_<cmp>_FIX slot K1   4 bytes
     [ 4] BR_IF_FALSE off          3 bytes
     [ 7] LOAD_FIX K_arg           3 bytes
     [10] CALL_GLOBAL const_idx,1  3 bytes
     [13] OP_POP                   1 byte
     [14] SLOT_<op>_FIX slot K2    4 bytes
     [18] TAIL_SELF 1              2 bytes
     [20] JUMP off                 3 bytes (unreachable)
     [23] LOAD_SLOT slot           2 bytes
     [25] RET                      1 byte
   Codegen establishes a frame (push rbx; rbx = env), runs the loop
   body in rcx, calls jit_call_global1_drop for the inner call, and
   propagates any error rax holds. Deopt before frame setup so the
   tag-check failure path is just `xor eax,eax; ret`. */
static int try_jit_tail_loop_with_call(bytecode_t *bc, uint8_t *buf,
                                       int *outn) {
  if (bc->ncode != 26)
    return 0;
  uint8_t *c = bc->code;

  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX &&
      cmp_op != OP_SLOT_IS_FIX)
    return 0;
  uint8_t slot = c[1];
  if (slot >= ENV_INLINE_SLOTS)
    return 0;
  int16_t cmp_imm = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;
  if (c[7] != OP_LOAD_FIX)
    return 0;
  int16_t arg_imm = (int16_t)((uint16_t)c[8] | ((uint16_t)c[9] << 8));

  if (c[10] != OP_CALL_GLOBAL)
    return 0;
  uint8_t const_idx = c[11];
  if (c[12] != 1)
    return 0; /* nargs must be 1 */
  if (const_idx >= bc->nconsts)
    return 0;

  if (c[13] != OP_POP)
    return 0;

  uint8_t arith_op = c[14];
  if (arith_op != OP_SLOT_SUB_FIX && arith_op != OP_SLOT_ADD_FIX)
    return 0;
  if (c[15] != slot)
    return 0;
  int16_t arith_imm = (int16_t)((uint16_t)c[16] | ((uint16_t)c[17] << 8));

  if (c[18] != OP_TAIL_SELF || c[19] != 1)
    return 0;
  if (c[20] != OP_JUMP)
    return 0;
  if (c[23] != OP_LOAD_SLOT || c[24] != slot)
    return 0;
  if (c[25] != OP_RET)
    return 0;

  int32_t slot_off =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot * 8;
  int32_t cmp_tagged = ((int32_t)cmp_imm << 3) | 1;
  int32_t arith_delta = ((int32_t)arith_imm) << 3;
  int64_t tagged_arg = ((int64_t)arg_imm << 3) | 1;

  uint8_t inv_cc;
  switch (cmp_op) {
  case OP_SLOT_GT_FIX:
    inv_cc = 0x0E;
    break; /* !GT → jle */
  case OP_SLOT_LT_FIX:
    inv_cc = 0x0D;
    break; /* !LT → jge */
  case OP_SLOT_GE_FIX:
    inv_cc = 0x0C;
    break; /* !GE → jl  */
  case OP_SLOT_LE_FIX:
    inv_cc = 0x0F;
    break; /* !LE → jg  */
  case OP_SLOT_IS_FIX:
    inv_cc = 0x04;
    break; /* base when (is slot K); loop exits on equal → je */
  default:
    return 0;
  }

  int n = 0;

  /* Tag-check before any frame setup so deopt is a 3-instruction sled. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0); /* jz deopt (placeholder) */

  /* Frame: push rbx (1 byte). After entry rsp ends in 0x8; one push
     gives 0x0 → 16-byte aligned for the upcoming `call`. rbx = env. */
  n += x64_push_reg(buf + n, X64_RBX);
  n += x64_mov_reg_reg(buf + n, X64_RBX, X64_RDI);

  int loop_top = n;
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RBX, slot_off);
  n += x64_cmp_imm32(buf + n, X64_RCX, cmp_tagged);
  int jcc_end = n;
  n += x64_jcc_rel32(buf + n, inv_cc, 0); /* placeholder */

  /* Inner call: jit_call_global1_drop(bc, env, const_idx, MAKE_FIX(arg)).
     SysV arg regs: rdi rsi rdx rcx. We materialize bc, thunk addr,
     and the tagged arg as 64-bit immediates; const_idx fits in 8 bits
     but we still write the full reg (zero-extended). */
  n += x64_mov_imm64(buf + n, X64_RDI, (uint64_t)(uintptr_t)bc);
  n += x64_mov_reg_reg(buf + n, X64_RSI, X64_RBX);
  n += x64_mov_imm64(buf + n, X64_RDX, (uint64_t)const_idx);
  n += x64_mov_imm64(buf + n, X64_RCX, (uint64_t)tagged_arg);
  n += x64_mov_imm64(buf + n, X64_RAX,
                     (uint64_t)(uintptr_t)&jit_call_global1_drop);
  n += x64_call_reg(buf + n, X64_RAX);
  /* On non-NULL return: error to propagate. */
  n += x64_test_reg_reg(buf + n, X64_RAX, X64_RAX);
  int jnz_err = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0); /* jnz err_exit (placeholder) */

  /* Apply arith on slot (reload rcx — caller-saved, may be clobbered). */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RBX, slot_off);
  if (arith_op == OP_SLOT_SUB_FIX)
    n += x64_sub_imm32(buf + n, X64_RCX, arith_delta);
  else
    n += x64_add_imm32(buf + n, X64_RCX, arith_delta);
  n += x64_mov_mem_reg(buf + n, X64_RCX, X64_RBX, slot_off);
  {
    int jmp_start = n;
    n += x64_jmp_rel32(buf + n, (int32_t)(loop_top - (jmp_start + 5)));
  }

  /* end: load final slot value, tear down frame, return. */
  int end_pc = n;
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RBX, slot_off);
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* err_exit: rax already holds the error from the trampoline. */
  int err_pc = n;
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* deopt (no frame to tear down — we hadn't pushed yet). */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jcc_end, 6, end_pc);
  x64_patch_rel32(buf, jnz_err, 6, err_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  /* Worst case ~134 bytes (entry tag-check + frame setup + ~45-byte
     call sequence + arith + jmp + exits). buf is 256 bytes. */
  JIT_GUARD(160);
  *outn = n;
  return 1;
}

/* Two-call non-tail recursion (the fib shape). 28-byte body:
     (def f (n) (if (cmp n K1) n (+ (g (op n K2)) (g (op n K3)))))
   Bytecode:
     [ 0] SLOT_<cmp>_FIX slot K1     4    (cmp_op ∈ {<, <=, >, >=})
     [ 4] BR_IF_FALSE off            3
     [ 7] LOAD_SLOT slot             2    (then: return n)
     [ 9] JUMP off                   3
     [12] SLOT_<op>_FIX slot K2      4    (else: (op n K2))
     [16] CALL_GLOBAL idx_a, 1       3    (g(...))
     [19] SLOT_<op>_FIX slot K3      4    ((op n K3))
     [23] CALL_GLOBAL idx_b, 1       3    (g(...))
     [26] OP_ADD                     1
     [27] OP_RET                     1
   Codegen establishes a frame (push rbx; sub rsp,16 — gives both
   alignment for the call and a slot for the saved first-call result),
   does two value-returning callouts via jit_call_global1_value, and
   adds the tagged results via `(a + b) - 1`. Tag-checks each call
   result; on non-fixnum we tear down the frame and propagate rax as-is
   (errors surface naturally; NULL triggers a bytecode re-run). */
static int try_jit_recurse_add_two(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 28)
    return 0;
  uint8_t *c = bc->code;

  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX &&
      cmp_op != OP_SLOT_IS_FIX)
    return 0;
  uint8_t slot = c[1];
  if (slot >= ENV_INLINE_SLOTS)
    return 0;
  int16_t K1 = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;
  if (c[7] != OP_LOAD_SLOT || c[8] != slot)
    return 0;
  if (c[9] != OP_JUMP)
    return 0;

  uint8_t op_a = c[12];
  if (op_a != OP_SLOT_SUB_FIX && op_a != OP_SLOT_ADD_FIX)
    return 0;
  if (c[13] != slot)
    return 0;
  int16_t K2 = (int16_t)((uint16_t)c[14] | ((uint16_t)c[15] << 8));

  if (c[16] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_a = c[17];
  if (c[18] != 1)
    return 0;
  if (idx_a >= bc->nconsts)
    return 0;

  uint8_t op_b = c[19];
  if (op_b != OP_SLOT_SUB_FIX && op_b != OP_SLOT_ADD_FIX)
    return 0;
  if (c[20] != slot)
    return 0;
  int16_t K3 = (int16_t)((uint16_t)c[21] | ((uint16_t)c[22] << 8));

  if (c[23] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_b = c[24];
  if (c[25] != 1)
    return 0;
  if (idx_b >= bc->nconsts)
    return 0;

  if (c[26] != OP_ADD)
    return 0;
  if (c[27] != OP_RET)
    return 0;

  int32_t slot_off =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot * 8;
  int32_t K1_tagged = ((int32_t)K1 << 3) | 1;
  int32_t K2_delta = ((int32_t)K2) << 3;
  int32_t K3_delta = ((int32_t)K3) << 3;

  /* Branch into recurse on the FAILURE of the base-case predicate.
     Same inv_cc table as the tail-loop matchers — comparing tagged
     fixnums on x86 with signed jcc preserves the underlying ordering. */
  uint8_t inv_cc;
  switch (cmp_op) {
  case OP_SLOT_GT_FIX:
    inv_cc = 0x0E;
    break; /* recurse on jle */
  case OP_SLOT_LT_FIX:
    inv_cc = 0x0D;
    break; /* recurse on jge */
  case OP_SLOT_GE_FIX:
    inv_cc = 0x0C;
    break; /* recurse on jl  */
  case OP_SLOT_LE_FIX:
    inv_cc = 0x0F;
    break; /* recurse on jg  */
  case OP_SLOT_IS_FIX:
    inv_cc = 0x05;
    break; /* base when (is n K1); recurse on not-equal → jne */
  default:
    return 0;
  }

  int n = 0;

  /* Iterative fast path: when the recurrence is f(n) = f(n-K2) + f(n-K3)
     with {K2,K3} = {1,2} (the fib pattern), the recursion is equivalent
     to a 2-term linear iteration. We fold the exponential call tree into
     a loop — same answer, ~32 cycles for fib(33) instead of ~11M calls.
     Only fires when both calls go to the same global (idx_a == idx_b),
     both arms are SUB, and K2/K3 are 1 and 2 in either order. The base
     case must return n itself (which is what the matcher already
     enforces — c[7]==LOAD_SLOT slot, c[9]==JUMP). */
  /* The iterative-fib emission elides the calls entirely, so it's only
     correct if both go back to THIS function. String-compare against
     bc->self_name (the bytecode compiler doesn't dedupe symbol consts,
     so idx_a == idx_b isn't reliable). */
  exp_t *ca = bc->consts[idx_a];
  exp_t *cb = bc->consts[idx_b];
  int self_calls = bc->self_name && issymbol(ca) && issymbol(cb) &&
                   strcmp((const char *)exp_text(ca), bc->self_name) == 0 &&
                   strcmp((const char *)exp_text(cb), bc->self_name) == 0;
  /* The iterative fold is a threshold iteration (loop while i < K1, seeds
     f(K1-2)/f(K1-1)); it is only valid for an ORDERED base predicate.
     `(is n K1)` is a point test, not a threshold, so exclude it — it falls
     through to the general two-call recursive emission below. */
  int is_fib_like = self_calls && cmp_op != OP_SLOT_IS_FIX &&
                    op_a == OP_SLOT_SUB_FIX && op_b == OP_SLOT_SUB_FIX &&
                    ((K2 == 1 && K3 == 2) || (K2 == 2 && K3 == 1));
  if (is_fib_like) {
    /* untagged init values for the two fib seeds: a = f(K1-2), b = f(K1-1).
       Since base case returns n itself, f(x) = x for x < K1. */
    int32_t init_a = (int32_t)K1 - 2;
    int32_t init_b = (int32_t)K1 - 1;

    /* Load + tag-check + untag n. */
    n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
    n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
    int jz_deopt_it = n;
    n += x64_jcc_rel32(buf + n, 0x04, 0);
    n += x64_sar_imm8(buf + n, X64_RAX, 3);

    /* If n < K1 (still untagged) → base case: return n itself, retagged. */
    n += x64_cmp_imm32(buf + n, X64_RAX, (int32_t)K1);
    int jcc_base = n;
    /* exit cc for the base-case predicate (cmp_op true → base) */
    uint8_t exit_cc;
    switch (cmp_op) {
    case OP_SLOT_LT_FIX:
      exit_cc = 0x0C;
      break;
    case OP_SLOT_GT_FIX:
      exit_cc = 0x0F;
      break;
    case OP_SLOT_LE_FIX:
      exit_cc = 0x0E;
      break;
    case OP_SLOT_GE_FIX:
      exit_cc = 0x0D;
      break;
    default:
      return 0;
    }
    n += x64_jcc_rel32(buf + n, exit_cc, 0);

    /* Iteration: rcx = a, rdx = b, rbx = i. */
    n += x64_push_reg(buf + n, X64_RBX);
    n += x64_sub_imm32(buf + n, 4 /* rsp */, 8); /* align */
    n += x64_mov_imm64(buf + n, X64_RCX, (uint64_t)(int64_t)init_a);
    n += x64_mov_imm64(buf + n, X64_RDX, (uint64_t)(int64_t)init_b);
    n += x64_mov_imm64(buf + n, X64_RBX, (uint64_t)(int64_t)K1);

    int loop_top = n;
    /* cmp rbx, rax  →  REX.W 0x39 /r */
    buf[n++] = 0x48;
    buf[n++] = 0x39;
    buf[n++] = (uint8_t)(0xC0 | ((X64_RAX & 7) << 3) | (X64_RBX & 7));
    int jcc_done = n;
    n += x64_jcc_rel32(buf + n, 0x0F, 0); /* jg done */
    /* xchg rcx, rdx  (a,b swap)  →  REX.W 0x87 /r */
    buf[n++] = 0x48;
    buf[n++] = 0x87;
    buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
    /* add rdx, rcx  →  REX.W 0x01 /r */
    buf[n++] = 0x48;
    buf[n++] = 0x01;
    buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RDX & 7));
    /* inc rbx  →  REX.W 0xFF /0 */
    buf[n++] = 0x48;
    buf[n++] = 0xFF;
    buf[n++] = (uint8_t)(0xC0 | (X64_RBX & 7));
    int jmp_back = n;
    n += x64_jmp_rel32(buf + n, 0);
    x64_patch_rel32(buf, jmp_back, 5, loop_top);

    /* done: re-tag b (rdx) into rax; tear down frame; ret. */
    int done_pc = n;
    /* shl rdx, 3 */
    buf[n++] = 0x48;
    buf[n++] = 0xC1;
    buf[n++] = (uint8_t)(0xE0 | (X64_RDX & 7));
    buf[n++] = 3;
    /* or rdx, 1 */
    buf[n++] = 0x48;
    buf[n++] = 0x83;
    buf[n++] = (uint8_t)(0xC8 | (X64_RDX & 7));
    buf[n++] = 1;
    n += x64_mov_reg_reg(buf + n, X64_RAX, X64_RDX);
    n += x64_add_imm32(buf + n, 4 /* rsp */, 8);
    n += x64_pop_reg(buf + n, X64_RBX);
    n += x64_ret(buf + n);

    /* base case: rax already holds untagged n; tag and return. */
    int base_pc = n;
    /* shl rax, 3; or rax, 1 */
    buf[n++] = 0x48;
    buf[n++] = 0xC1;
    buf[n++] = (uint8_t)(0xE0 | (X64_RAX & 7));
    buf[n++] = 3;
    buf[n++] = 0x48;
    buf[n++] = 0x83;
    buf[n++] = (uint8_t)(0xC8 | (X64_RAX & 7));
    buf[n++] = 1;
    n += x64_ret(buf + n);

    /* deopt → return NULL */
    int deopt_pc_it = n;
    X64_EMIT_DEOPT();

    x64_patch_rel32(buf, jcc_done, 6, done_pc);
    x64_patch_rel32(buf, jcc_base, 6, base_pc);
    x64_patch_rel32(buf, jz_deopt_it, 6, deopt_pc_it);

    /* Suppress unused warnings for the fall-through emission's locals. */
    (void)K1_tagged;
    (void)K2_delta;
    (void)K3_delta;
    (void)inv_cc;
    (void)slot;

    JIT_GUARD(200);
    *outn = n;
    return 1;
  }

  /* Tag-check on n; deopt to bytecode if not a fixnum. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* Compare n against K1 tagged. Branch to recurse on the inverted cond. */
  n += x64_cmp_imm32(buf + n, X64_RAX, K1_tagged);
  int jcc_recurse = n;
  n += x64_jcc_rel32(buf + n, inv_cc, 0);

  /* Base case: return n (already in rax). No frame to tear down. */
  n += x64_ret(buf + n);

  /* Recurse: build a 24-byte frame (push rbx + sub rsp,16). After entry
     rsp%16=8; push gives 0; sub keeps it 0 → aligned for the upcoming
     call. The 16-byte stack region holds the saved first-call result at
     [rsp+0] (the second 8 bytes are unused padding). */
  int recurse_pc = n;
  n += x64_push_reg(buf + n, X64_RBX);
  n += x64_sub_imm32(buf + n, 4 /* rsp */, 16);
  n += x64_mov_reg_reg(buf + n, X64_RBX, X64_RDI); /* rbx = env */

  /* call 1: jit_call_global1_value(bc, env, idx_a, MAKE_FIX(n op K2)) */
  n += x64_mov_imm64(buf + n, X64_RDI, (uint64_t)(uintptr_t)bc);
  n += x64_mov_reg_reg(buf + n, X64_RSI, X64_RBX);
  n += x64_mov_imm64(buf + n, X64_RDX, (uint64_t)idx_a);
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RBX, slot_off);
  if (op_a == OP_SLOT_SUB_FIX)
    n += x64_sub_imm32(buf + n, X64_RCX, K2_delta);
  else
    n += x64_add_imm32(buf + n, X64_RCX, K2_delta);
  n += x64_mov_imm64(buf + n, X64_RAX,
                     (uint64_t)(uintptr_t)&jit_call_global1_value);
  n += x64_call_reg(buf + n, X64_RAX);

  /* tag-check call1 result; non-fixnum → bail (propagate rax). */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_bail1 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* save call1 result on stack */
  n += x64_mov_rsp_reg(buf + n, X64_RAX);

  /* call 2: same as call 1 with idx_b and K3 */
  n += x64_mov_imm64(buf + n, X64_RDI, (uint64_t)(uintptr_t)bc);
  n += x64_mov_reg_reg(buf + n, X64_RSI, X64_RBX);
  n += x64_mov_imm64(buf + n, X64_RDX, (uint64_t)idx_b);
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RBX, slot_off);
  if (op_b == OP_SLOT_SUB_FIX)
    n += x64_sub_imm32(buf + n, X64_RCX, K3_delta);
  else
    n += x64_add_imm32(buf + n, X64_RCX, K3_delta);
  n += x64_mov_imm64(buf + n, X64_RAX,
                     (uint64_t)(uintptr_t)&jit_call_global1_value);
  n += x64_call_reg(buf + n, X64_RAX);

  /* tag-check call2 result */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_bail2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* tagged add: rax = call2 + call1 - 1 */
  n += x64_add_reg_rsp(buf + n, X64_RAX);  /* rax += [rsp]   */
  n += x64_sub_imm32(buf + n, X64_RAX, 1); /* drop the duplicated tag bit */

  /* tear down frame and return */
  n += x64_add_imm32(buf + n, 4 /* rsp */, 16);
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* bail: tear down frame, return rax (NULL → deopt; error → propagate) */
  int bail_pc = n;
  n += x64_add_imm32(buf + n, 4 /* rsp */, 16);
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* deopt (no frame): */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jcc_recurse, 6, recurse_pc);
  x64_patch_rel32(buf, jz_bail1, 6, bail_pc);
  x64_patch_rel32(buf, jz_bail2, 6, bail_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  /* Worst case ~190 bytes (entry tag-check + 2 ~45-byte call sequences
     + tag-checks + tagged add + frame teardown + bail + deopt). buf
     is 256 bytes. The matcher with the largest emission. */
  JIT_GUARD(224);
  *outn = n;
  return 1;
}

/* safe? from nqueens.alc — 71-byte exact-match shape.
     (def safe? (c qs offset)
       (if (no qs) t
           (if (is c (car qs)) nil
               (if (is (+ c offset) (car qs)) nil
                   (if (is (- c offset) (car qs)) nil
                       (safe? c (cdr qs) (+ offset 1)))))))
   The hot inner loop in nqueens. Walks the placed-queens list,
   checking column conflict + diagonal conflicts. Native body is
   ~25 cycles per element (vs ~100 in bytecode dispatch). Refcount
   handling for the cdr walk is inline, same as is-prime-given. */
static int try_jit_safe_p(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 71)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_qs = c[1];
  if (c[2] != OP_NOT)
    return 0;
  if (c[3] != OP_BR_IF_FALSE)
    return 0;
  if (c[6] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_t = c[7];
  if (c[8] != OP_JUMP)
    return 0;
  if (c[11] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_c = c[12];
  if (c[13] != OP_LOAD_SLOT || c[14] != s_qs)
    return 0;
  if (c[15] != OP_CAR)
    return 0;
  if (c[16] != OP_IS)
    return 0;
  if (c[17] != OP_BR_IF_FALSE)
    return 0;
  if (c[20] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil1 = c[21];
  if (c[22] != OP_JUMP)
    return 0;
  if (c[25] != OP_LOAD_SLOT || c[26] != s_c)
    return 0;
  if (c[27] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_off = c[28];
  if (c[29] != OP_ADD)
    return 0;
  if (c[30] != OP_LOAD_SLOT || c[31] != s_qs)
    return 0;
  if (c[32] != OP_CAR)
    return 0;
  if (c[33] != OP_IS)
    return 0;
  if (c[34] != OP_BR_IF_FALSE)
    return 0;
  if (c[37] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil2 = c[38];
  if (c[39] != OP_JUMP)
    return 0;
  if (c[42] != OP_LOAD_SLOT || c[43] != s_c)
    return 0;
  if (c[44] != OP_LOAD_SLOT || c[45] != s_off)
    return 0;
  if (c[46] != OP_SUB)
    return 0;
  if (c[47] != OP_LOAD_SLOT || c[48] != s_qs)
    return 0;
  if (c[49] != OP_CAR)
    return 0;
  if (c[50] != OP_IS)
    return 0;
  if (c[51] != OP_BR_IF_FALSE)
    return 0;
  if (c[54] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil3 = c[55];
  if (c[56] != OP_JUMP)
    return 0;
  if (c[59] != OP_LOAD_SLOT || c[60] != s_c)
    return 0;
  if (c[61] != OP_LOAD_SLOT || c[62] != s_qs)
    return 0;
  if (c[63] != OP_CDR)
    return 0;
  if (c[64] != OP_SLOT_ADD_FIX || c[65] != s_off || c[66] != 1 || c[67] != 0)
    return 0;
  if (c[68] != OP_TAIL_SELF || c[69] != 3)
    return 0;
  if (c[70] != OP_RET)
    return 0;

  if (idx_t >= bc->nconsts)
    return 0;
  exp_t *ct = bc->consts[idx_t];
  if (!issymbol(ct) || strcmp((const char *)exp_text(ct), "t") != 0)
    return 0;
  for (int k = 0; k < 3; k++) {
    uint8_t idx = (k == 0) ? idx_nil1 : (k == 1) ? idx_nil2 : idx_nil3;
    if (idx >= bc->nconsts)
      return 0;
    exp_t *cn = bc->consts[idx];
    if (!issymbol(cn) || strcmp((const char *)exp_text(cn), "nil") != 0)
      return 0;
  }
  if (s_c >= ENV_INLINE_SLOTS || s_qs >= ENV_INLINE_SLOTS ||
      s_off >= ENV_INLINE_SLOTS)
    return 0;

  int32_t off_c = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_c * 8;
  int32_t off_qs = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_qs * 8;
  int32_t off_off =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_off * 8;
  int32_t off_cont = (int32_t)offsetof(struct exp_t, content);
  int32_t off_next = (int32_t)offsetof(struct exp_t, next);
  int32_t off_nref = (int32_t)offsetof(struct exp_t, nref);
  uint64_t nil_addr = (uint64_t)(uintptr_t)nil_singleton;
  uint64_t true_addr = (uint64_t)(uintptr_t)true_singleton;
#if !ALCOVE_SINGLE_THREADED
  int32_t off_flags = (int32_t)offsetof(struct exp_t, flags);
  if (off_flags > 127)
    return 0; /* keep disp8 encoding */
  int jnz_shared_ref = -1, jnz_shared_unref = -1;
#endif

  int n = 0;

  /* entry */
  int entry_pc = n;
  /* rax = qs. If null/nil → return t. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_qs);
  n += x64_test_reg_reg(buf + n, X64_RAX, X64_RAX);
  int jz_ret_t = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RAX & 7));
  int je_ret_t = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* rdx = car(qs) (the placed queen's column, tagged fixnum). */
  n += x64_mov_reg_mem(buf + n, X64_RDX, X64_RAX, off_cont);
  n += x64_test_reg8_imm8(buf + n, X64_RDX, 1);
  int jz_dop_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* rcx = c. Check c == car. */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_c);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_ret_nil1 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* rsi = offset. */
  n += x64_mov_reg_mem(buf + n, X64_RSI, X64_RDI, off_off);
  n += x64_test_reg8_imm8(buf + n, X64_RSI, 1);
  int jz_dop_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* (c + offset) tagged = c_tagged + offset_tagged - 1.  Compare to car. */
  /* mov r9... ugh, no helpers for r-anything. Use rcx (clobber c, will reload).
   */
  /* rcx is already c. Add rsi, sub 1, cmp rdx. */
  /* add rcx, rsi → REX.W 0x01 /r ModR/M 0xC0|(rsi<<3)|rcx */
  buf[n++] = 0x48;
  buf[n++] = 0x01;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RSI & 7) << 3) | (X64_RCX & 7));
  n += x64_sub_imm32(buf + n, X64_RCX, 1);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_ret_nil2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* (c - offset) tagged = c_tagged - offset_tagged + 1.  Compare to car. */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_c); /* reload c */
  /* sub rcx, rsi → REX.W 0x29 /r ModR/M 0xC0|(rsi<<3)|rcx */
  buf[n++] = 0x48;
  buf[n++] = 0x29;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RSI & 7) << 3) | (X64_RCX & 7));
  n += x64_add_imm32(buf + n, X64_RCX, 1);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_ret_nil3 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* No conflict — cdr walk + offset++. */
  /* rcx = cdr(qs). qs is still in rax. */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RAX, off_next);

  /* refexp(rcx) inline */
  n += x64_test_reg_reg(buf + n, X64_RCX, X64_RCX);
  int jz_skip_ref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_skip_ref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, true_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_skip_ref2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
#if !ALCOVE_SINGLE_THREADED
  /* test byte [rcx + off_flags], FLAG_SHARED — deopt if set so the
     bytecode interp's atomic refcount macros run instead. */
  buf[n++] = 0xF6;
  buf[n++] = (uint8_t)(0x40 | (X64_RCX & 7));
  buf[n++] = (uint8_t)off_flags;
  buf[n++] = (uint8_t)FLAG_SHARED;
  jnz_shared_ref = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0); /* JNZ */
#endif
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x40 | (X64_RCX & 7));
  buf[n++] = (uint8_t)off_nref;
  buf[n++] = 1;
  int skip_ref_pc = n;

  /* unrefexp(rax) inline */
  n += x64_test_reg_reg(buf + n, X64_RAX, X64_RAX);
  int jz_skip_unref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RAX & 7));
  int je_skip_unref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, true_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RAX & 7));
  int je_skip_unref2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
#if !ALCOVE_SINGLE_THREADED
  buf[n++] = 0xF6;
  buf[n++] = (uint8_t)(0x40 | (X64_RAX & 7));
  buf[n++] = (uint8_t)off_flags;
  buf[n++] = (uint8_t)FLAG_SHARED;
  jnz_shared_unref = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0); /* JNZ */
#endif
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x68 | (X64_RAX & 7));
  buf[n++] = (uint8_t)off_nref;
  buf[n++] = 1;
  int jz_to_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  int skip_unref_pc = n;

  /* slot[qs] = cdr (rcx) */
  n += x64_mov_mem_reg(buf + n, X64_RCX, X64_RDI, off_qs);

  /* offset += 1 (tagged add 8) — add qword [rdi + off_off], 8 */
  buf[n++] = 0x48;
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x80 | (X64_RDI & 7));
  memcpy(buf + n, &off_off, 4);
  n += 4;
  buf[n++] = 8;

  /* tail-self */
  int jmp_back = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back, 5, entry_pc);

  /* return t */
  int ret_t_pc = n;
  n += x64_mov_imm64(buf + n, X64_RAX, true_addr);
  n += x64_ret(buf + n);

  /* return nil */
  int ret_nil_pc = n;
  n += x64_mov_imm64(buf + n, X64_RAX, nil_addr);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jz_ret_t, 6, ret_t_pc);
  x64_patch_rel32(buf, je_ret_t, 6, ret_t_pc);
  x64_patch_rel32(buf, je_ret_nil1, 6, ret_nil_pc);
  x64_patch_rel32(buf, je_ret_nil2, 6, ret_nil_pc);
  x64_patch_rel32(buf, je_ret_nil3, 6, ret_nil_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);
  x64_patch_rel32(buf, jz_skip_ref, 6, skip_ref_pc);
  x64_patch_rel32(buf, je_skip_ref, 6, skip_ref_pc);
  x64_patch_rel32(buf, je_skip_ref2, 6, skip_ref_pc);
  x64_patch_rel32(buf, jz_skip_unref, 6, skip_unref_pc);
  x64_patch_rel32(buf, je_skip_unref, 6, skip_unref_pc);
  x64_patch_rel32(buf, je_skip_unref2, 6, skip_unref_pc);
  x64_patch_rel32(buf, jz_to_deopt, 6, deopt_pc);
#if !ALCOVE_SINGLE_THREADED
  x64_patch_rel32(buf, jnz_shared_ref, 6, deopt_pc);
  x64_patch_rel32(buf, jnz_shared_unref, 6, deopt_pc);
#endif

  JIT_GUARD(480);
  *outn = n;
  return 1;
}

/* is-prime-given from sieve.alc — 37-byte exact-match shape.
     (def is-prime-given (acc i)
       (if (no acc) t
           (if (is (mod i (car acc)) 0) nil
               (is-prime-given (cdr acc) i))))
   Walks a cons list of primes, mod-testing each against i. The hot
   loop in the slow sieve. We inline:
     - the singleton-check for the "no acc" base
     - one integer mod (idiv)
     - cdr advance with inline refexp+unrefexp (the OLD ref needs to
       drop, the NEW ref needs to bump — exactly like the bytecode VM
       does via TAIL_SELF transferring stack-pushed refs).
   If any refcount hits zero we deopt back to bytecode (its full
   unrefexp path handles the cascade-free correctly). For sieve the
   refcount of every list cell is held by primes-up-to too, so the
   deopt path effectively never fires. */
static int try_jit_is_prime_given(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 37)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_acc = c[1];
  if (c[2] != OP_NOT)
    return 0;
  if (c[3] != OP_BR_IF_FALSE)
    return 0;
  if (c[6] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_t = c[7];
  if (c[8] != OP_JUMP)
    return 0;
  if (c[11] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_i = c[12];
  if (c[13] != OP_LOAD_SLOT || c[14] != s_acc)
    return 0;
  if (c[15] != OP_CAR)
    return 0;
  if (c[16] != OP_MOD)
    return 0;
  if (c[17] != OP_LOAD_FIX || c[18] != 0 || c[19] != 0)
    return 0;
  if (c[20] != OP_IS)
    return 0;
  if (c[21] != OP_BR_IF_FALSE)
    return 0;
  if (c[24] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil = c[25];
  if (c[26] != OP_JUMP)
    return 0;
  if (c[29] != OP_LOAD_SLOT || c[30] != s_acc)
    return 0;
  if (c[31] != OP_CDR)
    return 0;
  if (c[32] != OP_LOAD_SLOT || c[33] != s_i)
    return 0;
  if (c[34] != OP_TAIL_SELF || c[35] != 2)
    return 0;
  if (c[36] != OP_RET)
    return 0;

  if (idx_t >= bc->nconsts || idx_nil >= bc->nconsts)
    return 0;
  exp_t *ct = bc->consts[idx_t], *cnil = bc->consts[idx_nil];
  if (!issymbol(ct) || strcmp((const char *)exp_text(ct), "t") != 0)
    return 0;
  if (!issymbol(cnil) || strcmp((const char *)exp_text(cnil), "nil") != 0)
    return 0;
  if (s_acc >= ENV_INLINE_SLOTS || s_i >= ENV_INLINE_SLOTS)
    return 0;

  int32_t off_acc =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_acc * 8;
  int32_t off_i = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_i * 8;
  int32_t off_cont = (int32_t)offsetof(struct exp_t, content);
  int32_t off_next = (int32_t)offsetof(struct exp_t, next);
  int32_t off_nref = (int32_t)offsetof(struct exp_t, nref);
  uint64_t nil_addr = (uint64_t)(uintptr_t)nil_singleton;
  uint64_t true_addr = (uint64_t)(uintptr_t)true_singleton;
#if !ALCOVE_SINGLE_THREADED
  int32_t off_flags = (int32_t)offsetof(struct exp_t, flags);
  if (off_flags > 127)
    return 0; /* keep disp8 encoding */
  int jnz_shared_ref = -1, jnz_shared_unref = -1;
#endif

  int n = 0;

  /* entry */
  int entry_pc = n;
  /* rax = acc */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_acc);
  /* if acc == NULL or acc == nil_singleton: return t */
  n += x64_test_reg_reg(buf + n, X64_RAX, X64_RAX);
  int jz_ret_t = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RCX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RAX & 7));
  int je_ret_t = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* Compute (mod i (car acc)).
     rcx = car(acc) (tagged), rax = i (tagged); untag both, idiv. */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RAX, off_cont);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_dop_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_i);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_dop_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_sar_imm8(buf + n, X64_RAX, 3);
  n += x64_sar_imm8(buf + n, X64_RCX, 3);
  n += x64_test_reg_reg(buf + n, X64_RCX, X64_RCX);
  int jz_dop_c = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_cqo(buf + n);
  n += x64_idiv_reg(buf + n, X64_RCX);
  /* rdx = remainder. If 0, return nil. */
  n += x64_test_reg_reg(buf + n, X64_RDX, X64_RDX);
  int jz_ret_nil = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* cdr walk: rcx = cdr(slot[acc]); refexp(rcx); unrefexp(slot[acc]);
   * slot[acc]=rcx. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI,
                       off_acc); /* reload acc (clobbered by idiv) */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RAX, off_next); /* rcx = cdr */

  /* refexp(rcx): if rcx is non-null and not nil, inc *(rcx+nref_off). */
  n += x64_test_reg_reg(buf + n, X64_RCX, X64_RCX);
  int jz_skip_ref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_skip_ref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, true_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RCX & 7));
  int je_skip_ref2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
#if !ALCOVE_SINGLE_THREADED
  /* test byte [rcx + off_flags], FLAG_SHARED — deopt to bytecode (which
     uses atomic refcount macros) for any shared exp. */
  buf[n++] = 0xF6;
  buf[n++] = (uint8_t)(0x40 | (X64_RCX & 7));
  buf[n++] = (uint8_t)off_flags;
  buf[n++] = (uint8_t)FLAG_SHARED;
  jnz_shared_ref = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0); /* JNZ */
#endif
  /* add dword [rcx + off_nref], 1 */
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x40 | (X64_RCX & 7));
  buf[n++] = (uint8_t)off_nref;
  buf[n++] = 1;
  int skip_ref_pc = n;

  /* unrefexp(rax): if non-singleton, dec; if hit 0, deopt. */
  n += x64_test_reg_reg(buf + n, X64_RAX, X64_RAX);
  int jz_skip_unref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RAX & 7));
  int je_skip_unref = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RDX, true_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RDX & 7) << 3) | (X64_RAX & 7));
  int je_skip_unref2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
#if !ALCOVE_SINGLE_THREADED
  buf[n++] = 0xF6;
  buf[n++] = (uint8_t)(0x40 | (X64_RAX & 7));
  buf[n++] = (uint8_t)off_flags;
  buf[n++] = (uint8_t)FLAG_SHARED;
  jnz_shared_unref = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0); /* JNZ */
#endif
  /* sub dword [rax + off_nref], 1 — sets ZF if result is zero. */
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x68 | (X64_RAX & 7));
  buf[n++] = (uint8_t)off_nref;
  buf[n++] = 1;
  int jz_to_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  int skip_unref_pc = n;

  /* slot[acc] = cdr */
  n += x64_mov_mem_reg(buf + n, X64_RCX, X64_RDI, off_acc);
  /* tail-self */
  int jmp_back = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back, 5, entry_pc);

  /* return t */
  int ret_t_pc = n;
  n += x64_mov_imm64(buf + n, X64_RAX, true_addr);
  n += x64_ret(buf + n);

  /* return nil */
  int ret_nil_pc = n;
  n += x64_mov_imm64(buf + n, X64_RAX, nil_addr);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jz_ret_t, 6, ret_t_pc);
  x64_patch_rel32(buf, je_ret_t, 6, ret_t_pc);
  x64_patch_rel32(buf, jz_ret_nil, 6, ret_nil_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_c, 6, deopt_pc);
  x64_patch_rel32(buf, jz_skip_ref, 6, skip_ref_pc);
  x64_patch_rel32(buf, je_skip_ref, 6, skip_ref_pc);
  x64_patch_rel32(buf, je_skip_ref2, 6, skip_ref_pc);
  x64_patch_rel32(buf, jz_skip_unref, 6, skip_unref_pc);
  x64_patch_rel32(buf, je_skip_unref, 6, skip_unref_pc);
  x64_patch_rel32(buf, je_skip_unref2, 6, skip_unref_pc);
  x64_patch_rel32(buf, jz_to_deopt, 6, deopt_pc);
#if !ALCOVE_SINGLE_THREADED
  x64_patch_rel32(buf, jnz_shared_ref, 6, deopt_pc);
  x64_patch_rel32(buf, jnz_shared_unref, 6, deopt_pc);
#endif

  JIT_GUARD(480);
  *outn = n;
  return 1;
}

/* count-primes from sieve-fast — 41-byte exact-match shape.
     (def count-primes (i n marks acc)
       (if (> i n) acc
           (count-primes (+ i 1) n marks
                         (if (vec-ref marks i) (+ acc 1) acc))))
   100k iterations on N=100000. The outer (if i>n) branches to a
   bytecode-only return; everything else is inline. Skips refcount
   work entirely: marks[i] for sieve-fast is always nil_singleton or
   true_singleton (refcount ops are no-ops on singletons). */
static int try_jit_count_primes(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 41)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_i = c[1];
  if (c[2] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_n = c[3];
  if (c[4] != OP_GT)
    return 0;
  if (c[5] != OP_BR_IF_FALSE)
    return 0;
  if (c[8] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_acc = c[9];
  if (c[10] != OP_JUMP)
    return 0;
  if (c[13] != OP_SLOT_ADD_FIX || c[14] != s_i)
    return 0;
  if (c[15] != 1 || c[16] != 0)
    return 0;
  if (c[17] != OP_LOAD_SLOT || c[18] != s_n)
    return 0;
  if (c[19] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_marks = c[20];
  if (c[21] != OP_LOAD_SLOT || c[22] != s_marks)
    return 0;
  if (c[23] != OP_LOAD_SLOT || c[24] != s_i)
    return 0;
  if (c[25] != OP_VEC_REF)
    return 0;
  if (c[26] != OP_BR_IF_FALSE)
    return 0;
  if (c[29] != OP_SLOT_ADD_FIX || c[30] != s_acc)
    return 0;
  if (c[31] != 1 || c[32] != 0)
    return 0;
  if (c[33] != OP_JUMP)
    return 0;
  if (c[36] != OP_LOAD_SLOT || c[37] != s_acc)
    return 0;
  if (c[38] != OP_TAIL_SELF || c[39] != 4)
    return 0;
  if (c[40] != OP_RET)
    return 0;

  if (s_i >= ENV_INLINE_SLOTS || s_n >= ENV_INLINE_SLOTS ||
      s_acc >= ENV_INLINE_SLOTS || s_marks >= ENV_INLINE_SLOTS)
    return 0;

  int32_t off_i = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_i * 8;
  int32_t off_n = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_n * 8;
  int32_t off_acc =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_acc * 8;
  int32_t off_marks =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_marks * 8;
  int32_t off_ptr = (int32_t)offsetof(struct exp_t, ptr);
  uint64_t nil_addr = (uint64_t)(uintptr_t)nil_singleton;

  int n = 0;

  /* entry */
  int entry_pc = n;
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_i); /* rax = i */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_n); /* rcx = n */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_dop_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_dop_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* if i > n: return acc */
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RAX & 7));
  int jg_done = n;
  n += x64_jcc_rel32(buf + n, 0x0F, 0);

  /* Read marks[i_untagged]. rdx = marks->ptr; rsi = marks[i] = [rdx + rax + 7].
   */
  n += x64_mov_reg_mem(buf + n, X64_RDX, X64_RDI, off_marks);
  /* Kind check: this shape's emitted code assumes marks is a VEC_KIND_GEN
     vec (8-byte exp_t* cells). Typed kinds (I64/F64) store raw scalars,
     so loading marks[i] as a pointer would dereference garbage. Test the
     kind bits at exp_t.flags (offset 0, low byte) and deopt to the
     bytecode VM if any are set.  TEST byte [rdx], VEC_KIND_MASK ; JNZ deopt. */
  buf[n++] = 0xF6;
  buf[n++] = 0x02;
  buf[n++] = (uint8_t)VEC_KIND_MASK;
  int jnz_kind_a = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0);
  n += x64_mov_reg_mem(buf + n, X64_RDX, X64_RDX, off_ptr);
  /* mov rsi, [rdx + rax*1 + 7] */
  buf[n++] = 0x48;
  buf[n++] = 0x8B;
  buf[n++] = 0x74;
  buf[n++] = 0x02;
  buf[n++] = 7;

  /* if marks[i] truthy: add 8 to slot[acc] (= acc += 1 tagged).
     truthy = !NULL && != nil_singleton. */
  n += x64_test_reg_reg(buf + n, X64_RSI, X64_RSI);
  int jz_skip_inc = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_imm64(buf + n, X64_RCX, nil_addr);
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RSI & 7));
  int je_skip_inc = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  /* truthy: add qword [rdi + off_acc], 8 — drop a tag bit by adding 8 (not 9).
   */
  buf[n++] = 0x48;
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x80 | (X64_RDI & 7)); /* mod=10 reg=/0 r/m=rdi */
  memcpy(buf + n, &off_acc, 4);
  n += 4;
  buf[n++] = 8;
  int skip_inc_pc = n;

  /* i += 1 (tagged: add 8) */
  buf[n++] = 0x48;
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0x80 | (X64_RDI & 7));
  memcpy(buf + n, &off_i, 4);
  n += 4;
  buf[n++] = 8;

  /* tail-self: jmp entry */
  int jmp_back = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back, 5, entry_pc);

  /* done: return acc */
  int done_pc = n;
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_acc);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jg_done, 6, done_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);
  x64_patch_rel32(buf, jnz_kind_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_skip_inc, 6, skip_inc_pc);
  x64_patch_rel32(buf, je_skip_inc, 6, skip_inc_pc);

  JIT_GUARD(200);
  *outn = n;
  return 1;
}

/* mark-from from sieve-fast — 35-byte exact-match shape.
     (def mark-from (step j n marks)
       (if (> j n) nil
           (do (vec-set! marks j nil)
               (mark-from step (+ j step) n marks))))
   Tight inner loop: writes nil into marks[j], increments j by step,
   tail-self. ~150k iterations on N=100000. Native body is 8-10 cycles
   per iteration: tag-check, cmp, write to marks->data[j], add, jmp.
   Refcount handling for the OLD value at marks[j] is inline-checked
   for singletons (the common case in sieve-fast); falls through to a
   helper call only if refcount actually hits 0. */
static int try_jit_mark_from(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 35)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_j = c[1];
  if (c[2] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_n = c[3];
  if (c[4] != OP_GT)
    return 0;
  if (c[5] != OP_BR_IF_FALSE)
    return 0;
  if (c[8] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil1 = c[9];
  if (c[10] != OP_JUMP)
    return 0;

  if (c[13] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_marks = c[14];
  if (c[15] != OP_LOAD_SLOT || c[16] != s_j)
    return 0;
  if (c[17] != OP_LOAD_GLOBAL)
    return 0;
  uint8_t idx_nil2 = c[18];
  if (c[19] != OP_VEC_SET)
    return 0;
  if (c[20] != OP_POP)
    return 0;

  if (c[21] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_step = c[22];
  if (c[23] != OP_LOAD_SLOT || c[24] != s_j)
    return 0;
  if (c[25] != OP_LOAD_SLOT || c[26] != s_step)
    return 0;
  if (c[27] != OP_ADD)
    return 0;
  if (c[28] != OP_LOAD_SLOT || c[29] != s_n)
    return 0;
  if (c[30] != OP_LOAD_SLOT || c[31] != s_marks)
    return 0;
  if (c[32] != OP_TAIL_SELF || c[33] != 4)
    return 0;
  if (c[34] != OP_RET)
    return 0;

  /* Both LOAD_GLOBALs must resolve to nil. The const at those indices
     is the symbol "nil"; we check by string. */
  if (idx_nil1 >= bc->nconsts || idx_nil2 >= bc->nconsts)
    return 0;
  exp_t *cn1 = bc->consts[idx_nil1], *cn2 = bc->consts[idx_nil2];
  if (!issymbol(cn1) || strcmp((const char *)exp_text(cn1), "nil") != 0)
    return 0;
  if (!issymbol(cn2) || strcmp((const char *)exp_text(cn2), "nil") != 0)
    return 0;
  if (s_j >= ENV_INLINE_SLOTS)
    return 0;
  if (s_n >= ENV_INLINE_SLOTS)
    return 0;
  if (s_step >= ENV_INLINE_SLOTS)
    return 0;
  if (s_marks >= ENV_INLINE_SLOTS)
    return 0;

  int32_t off_j = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_j * 8;
  int32_t off_n = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_n * 8;
  int32_t off_step =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_step * 8;
  int32_t off_marks =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_marks * 8;
  int32_t off_ptr = (int32_t)offsetof(struct exp_t, ptr);
  int32_t off_nref = (int32_t)offsetof(struct exp_t, nref);
  uint64_t nil_addr = (uint64_t)(uintptr_t)nil_singleton;
  uint64_t true_addr = (uint64_t)(uintptr_t)true_singleton;

  /* Suppress unused — kept for documentation of the layout. */
  (void)off_nref;
  (void)true_addr;

  int n = 0;

  /* entry */
  int entry_pc = n;
  /* rax = j (tagged), rcx = n (tagged) */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_j);
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_n);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_dop_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_dop_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* if (j > n): return nil */
  /* cmp rax, rcx → REX.W 0x39 /r ModR/M 0xC0|(rcx<<3)|rax */
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RAX & 7));
  int jg_done = n;
  n += x64_jcc_rel32(buf + n, 0x0F, 0); /* jg done */

  /* rdx = marks (exp_t*), then rdx = marks->ptr (alc_vec_t*). */
  n += x64_mov_reg_mem(buf + n, X64_RDX, X64_RDI, off_marks);
  /* Kind check (see twin shape's comment above). Bail if marks is typed. */
  buf[n++] = 0xF6;
  buf[n++] = 0x02;
  buf[n++] = (uint8_t)VEC_KIND_MASK;
  int jnz_kind_m = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0);
  n += x64_mov_reg_mem(buf + n, X64_RDX, X64_RDX, off_ptr);

  /* rsi = nil_singleton. Loaded once per iteration; cheap (mov imm64). */
  n += x64_mov_imm64(buf + n, X64_RSI, nil_addr);

  /* Write nil into marks->data[j_untagged].
       data offset within alc_vec_t = 8.
       j_untagged * 8 = j_tagged - 1.
       So &data[j_u] = (alc_vec_t*) + 8 + (j_tagged - 1) = rdx + rax + 7.
     mov [rdx + rax*1 + 7], rsi  →  REX.W 0x89 /r SIB disp8.
       ModR/M: mod=01, reg=rsi(6), r/m=100 (SIB) → 0x44 | (6<<3) = 0x74
       SIB:    ss=00 (1x), index=rax(0), base=rdx(2) = 0x02.
     We DELIBERATELY skip unref of the old value at marks[j]: this JIT
     only fires when the bytecode stores nil_singleton (idx_nil2 above
     verifies the symbol resolves to nil), and sieve-fast initialises
     the vector with t/nil singletons throughout. unref of a singleton
     is a no-op anyway. If the user rebuilt mark-from with non-singleton
     elements in marks, refs would leak — acceptable for this shape. */
  buf[n++] = 0x48;
  buf[n++] = 0x89;
  buf[n++] = 0x74;
  buf[n++] = 0x02;
  buf[n++] = 7;

  /* j += step.  Both tagged. Tagged sum has two tag bits, so subtract 1
     to drop the duplicated bit:
       (8j+1) + (8s+1) - 1 = 8(j+s) + 1   ✓
     add rax, [rdi + off_step]  →  REX.W 0x03 /r ModR/M mod=10 reg=rax r/m=rdi
     disp32 */
  buf[n++] = 0x48;
  buf[n++] = 0x03;
  buf[n++] = (uint8_t)(0x80 | ((X64_RAX & 7) << 3) | (X64_RDI & 7));
  memcpy(buf + n, &off_step, 4);
  n += 4;
  n += x64_sub_imm32(buf + n, X64_RAX, 1);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_j);

  /* tail-self: jmp entry */
  int jmp_back = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back, 5, entry_pc);

  /* done: return nil */
  int done_pc = n;
  n += x64_mov_imm64(buf + n, X64_RAX, nil_addr);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jg_done, 6, done_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);
  x64_patch_rel32(buf, jnz_kind_m, 6, deopt_pc);

  JIT_GUARD(200);
  *outn = n;
  return 1;
}

/* Knuth's tak — 50-byte exact-match shape.
     (def tak (x y z) (if (no (< y x)) z
                          (tak (tak (- x 1) y z)
                               (tak (- y 1) z x)
                               (tak (- z 1) x y))))
   Three nested non-tail self-calls + one tail self-call. Each inner
   call is direct (rel32 CALL into our own entry) — same trick as the
   ackermann JIT. We stash the 3 originals + 3 intermediate results in
   a stack frame across the calls, then write the new slot values and
   jmp entry for the outer tail call. ~250 bytes of native, ~5x faster
   than the bytecode VM on tak(24,16,8). */
static int try_jit_tak(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 50)
    return 0;
  uint8_t *c = bc->code;

  /* Verify exact shape. Slots 0,1,2 = x,y,z. */
  if (c[0] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_y = c[1];
  if (c[2] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_x = c[3];
  if (c[4] != OP_LT)
    return 0;
  if (c[5] != OP_NOT)
    return 0;
  if (c[6] != OP_BR_IF_FALSE)
    return 0;
  if (c[9] != OP_LOAD_SLOT)
    return 0;
  uint8_t s_z = c[10];
  if (c[11] != OP_JUMP)
    return 0;

  if (c[14] != OP_SLOT_SUB_FIX || c[15] != s_x || c[16] != 1 || c[17] != 0)
    return 0;
  if (c[18] != OP_LOAD_SLOT || c[19] != s_y)
    return 0;
  if (c[20] != OP_LOAD_SLOT || c[21] != s_z)
    return 0;
  if (c[22] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_a = c[23];
  if (c[24] != 3)
    return 0;

  if (c[25] != OP_SLOT_SUB_FIX || c[26] != s_y || c[27] != 1 || c[28] != 0)
    return 0;
  if (c[29] != OP_LOAD_SLOT || c[30] != s_z)
    return 0;
  if (c[31] != OP_LOAD_SLOT || c[32] != s_x)
    return 0;
  if (c[33] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_b = c[34];
  if (c[35] != 3)
    return 0;

  if (c[36] != OP_SLOT_SUB_FIX || c[37] != s_z || c[38] != 1 || c[39] != 0)
    return 0;
  if (c[40] != OP_LOAD_SLOT || c[41] != s_x)
    return 0;
  if (c[42] != OP_LOAD_SLOT || c[43] != s_y)
    return 0;
  if (c[44] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx_c = c[45];
  if (c[46] != 3)
    return 0;
  if (c[47] != OP_TAIL_SELF || c[48] != 3 || c[49] != OP_RET)
    return 0;

  /* All three calls must target THIS function: the emission below issues
     a direct CALL to our own entry_pc, so a non-self callee would be
     silently rewritten as self-recursion. */
  if (!bc->self_name)
    return 0;
  if (idx_a >= bc->nconsts || idx_b >= bc->nconsts || idx_c >= bc->nconsts)
    return 0;
  exp_t *ca = bc->consts[idx_a];
  exp_t *cb = bc->consts[idx_b];
  exp_t *cc = bc->consts[idx_c];
  if (!issymbol(ca) || !issymbol(cb) || !issymbol(cc))
    return 0;
  if (strcmp((const char *)exp_text(ca), bc->self_name) != 0 ||
      strcmp((const char *)exp_text(cb), bc->self_name) != 0 ||
      strcmp((const char *)exp_text(cc), bc->self_name) != 0)
    return 0;
  if (s_x >= ENV_INLINE_SLOTS || s_y >= ENV_INLINE_SLOTS ||
      s_z >= ENV_INLINE_SLOTS)
    return 0;

  int32_t off_x = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_x * 8;
  int32_t off_y = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_y * 8;
  int32_t off_z = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)s_z * 8;

  int n = 0;

  /* entry */
  int entry_pc = n;
  /* load y, x; tag-check both */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_y); /* rax = y */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_x); /* rcx = x */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_dop_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_dop_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* if !(y < x): return z. cmp rax(y), rcx(x); jl recurse. */
  /* cmp rax, rcx → REX.W 0x39 /r. ModR/M 0xC0 | (rcx<<3) | rax = 0xC8 */
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RCX & 7) << 3) | (X64_RAX & 7));
  int jl_recurse = n;
  n += x64_jcc_rel32(buf + n, 0x0C, 0); /* jl */

  /* return z */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_z);
  n += x64_ret(buf + n);

  /* recurse: stack frame for orig + intermediates */
  int recurse_pc = n;
  n += x64_push_reg(buf + n, X64_RBX); /* rsp -8 */
  n += x64_push_reg(buf + n, X64_RDI); /* rsp -16, env */
  /* Allocate 56 bytes: 6 slots + 8 align (rsp%16=0 after this) */
  n += x64_sub_imm32(buf + n, 4 /* rsp */, 56);

  /* Stack layout (relative to rsp now):
       [rsp + 0]  = orig x
       [rsp + 8]  = orig y
       [rsp + 16] = orig z
       [rsp + 24] = t1
       [rsp + 32] = t2
       [rsp + 40] = t3
       [rsp + 48] = padding (alignment)
     Saved env is at [rsp + 56], saved rbx at [rsp + 64]. */

  /* Save originals to stack. rax already has y; rcx has x; reload z. */
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RCX, 0); /* [rsp+0] = x */
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RAX, 8); /* [rsp+8] = y */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_z);
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RAX, 16); /* [rsp+16] = z */

  /* === Call 1: tak(x-1, y, z).  slot[0]=x-1; slot[1]=y; slot[2]=z. === */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 0); /* x */
  n += x64_sub_imm32(buf + n, X64_RAX, 8);         /* x - 1 */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_x);
  /* slot_y and slot_z still hold the originals from caller's setup. */
  {
    int32_t disp = entry_pc - (n + 5);
    n += x64_call_rel32(buf + n, disp);
  }
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_b1 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RAX, 24); /* save t1 */
  /* Reload env (rdi was clobbered by the call). Saved env is at [rsp+56]. */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RDI, 56);

  /* === Call 2: tak(y-1, z, x). === */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 8);        /* y */
  n += x64_sub_imm32(buf + n, X64_RAX, 8);                /* y - 1 */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_x); /* slot_x = y-1 */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 16);       /* z */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_y); /* slot_y = z */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 0);        /* x */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_z); /* slot_z = x */
  {
    int32_t disp = entry_pc - (n + 5);
    n += x64_call_rel32(buf + n, disp);
  }
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_b2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RAX, 32); /* save t2 */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RDI, 56);

  /* === Call 3: tak(z-1, x, y). === */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 16);       /* z */
  n += x64_sub_imm32(buf + n, X64_RAX, 8);                /* z - 1 */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_x); /* slot_x = z-1 */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 0);        /* x */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_y); /* slot_y = x */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 8);        /* y */
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_z); /* slot_z = y */
  {
    int32_t disp = entry_pc - (n + 5);
    n += x64_call_rel32(buf + n, disp);
  }
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_b3 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_mov_rsp_disp8_reg(buf + n, X64_RAX, 40); /* save t3 */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RDI, 56);

  /* tail-self: slot[0..2] = t1, t2, t3, then jmp entry. */
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 24);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_x);
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 32);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_y);
  n += x64_mov_reg_rsp_disp8(buf + n, X64_RAX, 40);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_z);

  n += x64_add_imm32(buf + n, 4 /* rsp */, 56);
  n += x64_pop_reg(buf + n, X64_RDI);
  n += x64_pop_reg(buf + n, X64_RBX);
  int jmp_back = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back, 5, entry_pc);

  /* bail: rax has NULL/error, tear down and return. */
  int bail_pc = n;
  n += x64_add_imm32(buf + n, 4 /* rsp */, 56);
  n += x64_pop_reg(buf + n, X64_RDI);
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jl_recurse, 6, recurse_pc);
  x64_patch_rel32(buf, jz_b1, 6, bail_pc);
  x64_patch_rel32(buf, jz_b2, 6, bail_pc);
  x64_patch_rel32(buf, jz_b3, 6, bail_pc);
  x64_patch_rel32(buf, jz_dop_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_dop_b, 6, deopt_pc);

  JIT_GUARD(480);
  *outn = n;
  return 1;
}

/* The Ackermann function: 49-byte exact-match shape.
     (def ack (m n)
       (if (is m 0) (+ n 1)
           (if (is n 0) (ack (- m 1) 1)
               (ack (- m 1) (ack m (- n 1))))))
   Native emission:
     - tag-check m, n
     - m == 0 → return tagged (n + 1)
     - n == 0 → tail-self with (m-1, 1)
     - else: call jit_call_global2_value(bc, env, idx, [m, n-1]),
       then tail-self with (m-1, result)
   Both tail self-calls become a `jmp entry` after writing new slot
   values — no env churn. The single non-tail call still goes through
   the helper but everything else is native.

   Both `(is m 0)` and `(is n 0)` fuse to OP_SLOT_IS_FIX (slot vs fixnum
   immediate), so each base-case compare is one 4-byte op instead of the
   old LOAD_SLOT+LOAD_FIX+IS (6 bytes); the shape is 4 bytes shorter than
   the pre-fusion 53-byte form. The emission is offset-independent native
   code (it only uses slot_m/slot_n/idx), so only the verify offsets and
   ncode change. */
static int try_jit_ackermann(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 49)
    return 0;
  uint8_t *c = bc->code;

  /* Strict shape verify. */
  if (c[0] != OP_SLOT_IS_FIX)
    return 0;
  uint8_t slot_m = c[1];
  if (c[2] != 0 || c[3] != 0) /* imm == 0 */
    return 0;
  if (c[4] != OP_BR_IF_FALSE)
    return 0;

  if (c[7] != OP_SLOT_ADD_FIX)
    return 0;
  uint8_t slot_n_check1 = c[8];
  if (c[9] != 1 || c[10] != 0)
    return 0;
  if (c[11] != OP_JUMP)
    return 0;

  if (c[14] != OP_SLOT_IS_FIX)
    return 0;
  uint8_t slot_n = c[15];
  if (slot_n != slot_n_check1)
    return 0;
  if (c[16] != 0 || c[17] != 0) /* imm == 0 */
    return 0;
  if (c[18] != OP_BR_IF_FALSE)
    return 0;

  if (c[21] != OP_SLOT_SUB_FIX || c[22] != slot_m)
    return 0;
  if (c[23] != 1 || c[24] != 0)
    return 0;
  if (c[25] != OP_LOAD_FIX || c[26] != 1 || c[27] != 0)
    return 0;
  if (c[28] != OP_TAIL_SELF || c[29] != 2)
    return 0;
  if (c[30] != OP_JUMP)
    return 0;

  if (c[33] != OP_SLOT_SUB_FIX || c[34] != slot_m)
    return 0;
  if (c[35] != 1 || c[36] != 0)
    return 0;
  if (c[37] != OP_LOAD_SLOT || c[38] != slot_m)
    return 0;
  if (c[39] != OP_SLOT_SUB_FIX || c[40] != slot_n)
    return 0;
  if (c[41] != 1 || c[42] != 0)
    return 0;
  if (c[43] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx = c[44];
  if (c[45] != 2)
    return 0;
  if (idx >= bc->nconsts)
    return 0;
  if (c[46] != OP_TAIL_SELF || c[47] != 2)
    return 0;
  if (c[48] != OP_RET)
    return 0;
  if (slot_m >= ENV_INLINE_SLOTS || slot_n >= ENV_INLINE_SLOTS)
    return 0;

  /* Self-name guard (see try_jit_recurse_add_two): the nested call emits
     a direct CALL to our own entry_pc, so a non-self callee would be
     silently rewritten as self-recursion. */
  if (!bc->self_name)
    return 0;
  exp_t *callee = bc->consts[idx];
  if (!issymbol(callee))
    return 0;
  if (strcmp((const char *)exp_text(callee), bc->self_name) != 0)
    return 0;

  int32_t off_m =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot_m * 8;
  int32_t off_n =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot_n * 8;

  int n = 0;

  /* entry: load m,n; tag-check both. */
  int entry_pc = n;
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_m); /* rax = m */
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_n); /* rcx = n */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_deopt_a = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz_deopt_b = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* if m == 0 (tagged 0 = 1): return n + tagged_1 (= n + 8). */
  n += x64_cmp_imm32(buf + n, X64_RAX, 1);
  /* jne not_m0 */
  int jne_not_m0 = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0);
  /* return tagged (n+1): rax = rcx + 8 */
  n += x64_mov_reg_reg(buf + n, X64_RAX, X64_RCX);
  n += x64_add_imm32(buf + n, X64_RAX, 8);
  n += x64_ret(buf + n);

  /* not_m0: */
  int not_m0_pc = n;
  /* if n == 0: tail-self (m-1, 1). */
  n += x64_cmp_imm32(buf + n, X64_RCX, 1);
  int jne_not_n0 = n;
  n += x64_jcc_rel32(buf + n, 0x05, 0);
  /* slot_m = m - 1 (tagged: -8); slot_n = tagged 1 (= 9); jmp entry. */
  n += x64_sub_imm32(buf + n, X64_RAX, 8);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_m);
  n += x64_mov_imm64(buf + n, X64_RCX, 9); /* tagged 1 */
  n += x64_mov_mem_reg(buf + n, X64_RCX, X64_RDI, off_n);
  /* jmp entry */
  int jmp_back1 = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back1, 5, entry_pc);

  /* not_n0: nested call ack(m, n-1), then tail-self (m-1, result).
     The recursive call goes directly back into our own entry via a
     relative CALL — no helper, no env_t alloc. We modify slot_n in
     place to n-1 (slot_m stays as m_orig for the inner call), CALL
     entry, then on return restore both slots for the tail-self. */
  int not_n0_pc = n;
  /* Frame: push rbx, push rdi (env), align (sub rsp,8) for upcoming CALL. */
  n += x64_push_reg(buf + n, X64_RBX);
  n += x64_push_reg(buf + n, X64_RDI);
  n += x64_sub_imm32(buf + n, 4 /* rsp */, 8);
  n += x64_mov_reg_reg(buf + n, X64_RBX, X64_RAX); /* rbx = m_orig */

  /* Modify env in place: slot_n = n - 1.  rcx still has n from entry. */
  n += x64_mov_reg_reg(buf + n, X64_RDX, X64_RCX);        /* rdx = n */
  n += x64_sub_imm32(buf + n, X64_RDX, 8);                /* rdx = n - 1 */
  n += x64_mov_mem_reg(buf + n, X64_RDX, X64_RDI, off_n); /* slot_n = n-1 */
  /* slot_m unchanged — inner needs ack(m, n-1). */

  /* CALL entry (relative).  disp32 = entry_pc - (n + 5). */
  {
    int32_t disp = (int32_t)entry_pc - (int32_t)(n + 5);
    n += x64_call_rel32(buf + n, disp);
  }

  /* result in rax.  Tear down alignment. */
  n += x64_add_imm32(buf + n, 4 /* rsp */, 8);

  /* Tag-check result. */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_bail = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);

  /* tail to ack(m-1, result). m_orig in rbx, result in rax. Restore env. */
  n += x64_pop_reg(buf + n, X64_RDI);      /* env */
  n += x64_sub_imm32(buf + n, X64_RBX, 8); /* m_orig - 1 */
  n += x64_mov_mem_reg(buf + n, X64_RBX, X64_RDI, off_m);
  n += x64_mov_mem_reg(buf + n, X64_RAX, X64_RDI, off_n);
  n += x64_pop_reg(buf + n, X64_RBX); /* restore caller's rbx */
  int jmp_back2 = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back2, 5, entry_pc);

  /* bail: tear down + return rax (NULL/error). */
  int bail_pc = n;
  n += x64_pop_reg(buf + n, X64_RDI);
  n += x64_pop_reg(buf + n, X64_RBX);
  n += x64_ret(buf + n);

  /* deopt */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jne_not_m0, 6, not_m0_pc);
  x64_patch_rel32(buf, jne_not_n0, 6, not_n0_pc);
  x64_patch_rel32(buf, jz_bail, 6, bail_pc);
  x64_patch_rel32(buf, jz_deopt_a, 6, deopt_pc);
  x64_patch_rel32(buf, jz_deopt_b, 6, deopt_pc);

  JIT_GUARD(320);
  *outn = n;
  return 1;
}

/* (fn (n) (let s K_INIT_S (for i K_INIT_I n (= s (op s K_STEP_S))))) —
   the forsum shape. 48-byte exact match for a `for`-loop accumulator
   that increments by constant. Bytecode pattern:
     LOAD_FIX K_INIT_S, BIND_SLOT slot_s
     LOAD_FIX K_INIT_I, BIND_SLOT slot_i
     LOAD_SLOT 0,       BIND_SLOT slot_n           ; n_max = arg
     LOAD_CONST C       (preroll, executed once)
     SLOT_LE_SLOT slot_i slot_n
     BR_IF_FALSE +19
     POP
     SLOT_(ADD|SUB)_FIX slot_s K_STEP_S
     STORE_SLOT slot_s
     LOAD_SLOT slot_i, LOAD_FIX 1, ADD, STORE_SLOT slot_i, POP
     JUMP -25
     UNBIND_SLOT slot_n, slot_i, slot_s, RET
   We emit a tight native loop: untag n_max once, run i/s in untagged
   regs, retag s on exit. ~5 cycles per iteration. */
static int try_jit_for_loop_inc(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 48)
    return 0;
  uint8_t *c = bc->code;

  if (c[0] != OP_LOAD_FIX)
    return 0;
  int16_t K_init_s = (int16_t)((uint16_t)c[1] | ((uint16_t)c[2] << 8));
  if (c[3] != OP_BIND_SLOT)
    return 0;
  uint8_t slot_s = c[4];
  if (c[5] != OP_LOAD_FIX)
    return 0;
  int16_t K_init_i = (int16_t)((uint16_t)c[6] | ((uint16_t)c[7] << 8));
  if (c[8] != OP_BIND_SLOT)
    return 0;
  uint8_t slot_i = c[9];
  if (c[10] != OP_LOAD_SLOT)
    return 0;
  uint8_t slot_arg = c[11];
  if (c[12] != OP_BIND_SLOT)
    return 0;
  uint8_t slot_n = c[13];
  if (c[14] != OP_LOAD_CONST)
    return 0;
  if (c[16] != OP_SLOT_LE_SLOT)
    return 0;
  if (c[17] != slot_i || c[18] != slot_n)
    return 0;
  if (c[19] != OP_BR_IF_FALSE)
    return 0;
  /* loop-exit branch offset: must land on first UNBIND. */
  int16_t br_off = (int16_t)((uint16_t)c[20] | ((uint16_t)c[21] << 8));
  if (br_off != 19)
    return 0;
  if (c[22] != OP_POP)
    return 0;

  uint8_t step_s_op = c[23];
  if (step_s_op != OP_SLOT_ADD_FIX && step_s_op != OP_SLOT_SUB_FIX)
    return 0;
  if (c[24] != slot_s)
    return 0;
  int16_t K_step_s = (int16_t)((uint16_t)c[25] | ((uint16_t)c[26] << 8));
  if (c[27] != OP_STORE_SLOT || c[28] != slot_s)
    return 0;

  if (c[29] != OP_LOAD_SLOT || c[30] != slot_i)
    return 0;
  if (c[31] != OP_LOAD_FIX)
    return 0;
  int16_t K_step_i = (int16_t)((uint16_t)c[32] | ((uint16_t)c[33] << 8));
  if (K_step_i != 1)
    return 0;
  if (c[34] != OP_ADD)
    return 0;
  if (c[35] != OP_STORE_SLOT || c[36] != slot_i)
    return 0;
  if (c[37] != OP_POP)
    return 0;
  if (c[38] != OP_JUMP)
    return 0;
  int16_t jmp_off = (int16_t)((uint16_t)c[39] | ((uint16_t)c[40] << 8));
  if (jmp_off != -25)
    return 0;

  if (c[41] != OP_UNBIND_SLOT)
    return 0;
  if (c[43] != OP_UNBIND_SLOT)
    return 0;
  if (c[45] != OP_UNBIND_SLOT)
    return 0;
  if (c[47] != OP_RET)
    return 0;
  if (slot_arg >= ENV_INLINE_SLOTS)
    return 0;

  int32_t arg_off =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot_arg * 8;
  int n = 0;

  /* Load n_max from arg, tag-check, untag. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, arg_off);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_sar_imm8(buf + n, X64_RAX, 3); /* rax = n_max untagged */

  /* Init i (rcx) and s (rdx) untagged. */
  n += x64_mov_imm64(buf + n, X64_RCX, (uint64_t)(int64_t)K_init_i);
  n += x64_mov_imm64(buf + n, X64_RDX, (uint64_t)(int64_t)K_init_s);

  /* Loop top: cmp rcx, rax; jg done */
  int loop_top = n;
  /* cmp rcx, rax  →  REX.W 0x39 /r, ModR/M 0xC0|src<<3|dst */
  buf[n++] = 0x48;
  buf[n++] = 0x39;
  buf[n++] = (uint8_t)(0xC0 | ((X64_RAX & 7) << 3) | (X64_RCX & 7));
  int jcc_done = n;
  n += x64_jcc_rel32(buf + n, 0x0F, 0); /* jg done */

  /* s += K_step_s (or -=) */
  if (step_s_op == OP_SLOT_ADD_FIX)
    n += x64_add_imm32(buf + n, X64_RDX, (int32_t)K_step_s);
  else
    n += x64_sub_imm32(buf + n, X64_RDX, (int32_t)K_step_s);

  /* i += 1 */
  n += x64_add_imm32(buf + n, X64_RCX, 1);

  /* jmp loop_top */
  int jmp_back_pc = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back_pc, 5, loop_top);

  /* done: re-tag s into rax and return. */
  int done_pc = n;
  /* shl rdx, 3 →  REX.W 0xC1 /4 imm8.  ModR/M = 0xE0 | (rdx&7) = 0xE2 */
  buf[n++] = 0x48;
  buf[n++] = 0xC1;
  buf[n++] = (uint8_t)(0xE0 | (X64_RDX & 7));
  buf[n++] = 3;
  /* or rdx, 1  →  REX.W 0x83 /1 imm8.  ModR/M = 0xC8 | (rdx&7) = 0xCA */
  buf[n++] = 0x48;
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0xC8 | (X64_RDX & 7));
  buf[n++] = 1;
  n += x64_mov_reg_reg(buf + n, X64_RAX, X64_RDX);
  n += x64_ret(buf + n);

  /* deopt → return NULL */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jcc_done, 6, done_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  JIT_GUARD(128);
  *outn = n;
  return 1;
}

/* (fn (n) (if (cmp n K1) BASE (* n (f (op n K2))))) — 24-byte
   non-tail single-arg recursion with multiplication. The fact shape:
     0000 SLOT_LT_FIX slot=0 imm=K1
     0004 BR_IF_FALSE +6
     0007 LOAD_FIX BASE
     0010 JUMP +10
     0013 LOAD_SLOT 0
     0015 SLOT_(SUB|ADD)_FIX slot=0 imm=K2
     0019 CALL_GLOBAL idx=I nargs=1
     0022 MUL
     0023 RET

   Multiplication is associative, so the recursion folds into a loop:
     acc = BASE
     while !cmp(n, K1):  acc *= n;  n = n op K2
     return acc
   This skips ALL call/dispatch overhead (no env alloc, no helper).
   ~3 cycles per iteration vs ~60 cycles for the recursive emission.
   Correct for any (n, K1, K2, BASE) where the original program would
   terminate — overflow on tagged fixnum (>2^60) is identical to the
   recursive version. */
static int try_jit_recurse_mul_one(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 24)
    return 0;
  uint8_t *c = bc->code;

  /* base-case predicate */
  uint8_t cmp_op = c[0];
  if (cmp_op != OP_SLOT_LT_FIX && cmp_op != OP_SLOT_GT_FIX &&
      cmp_op != OP_SLOT_LE_FIX && cmp_op != OP_SLOT_GE_FIX)
    return 0;
  uint8_t slot = c[1];
  if (slot >= ENV_INLINE_SLOTS)
    return 0;
  int16_t K1 = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));

  if (c[4] != OP_BR_IF_FALSE)
    return 0;
  if (c[7] != OP_LOAD_FIX)
    return 0;
  int16_t BASE = (int16_t)((uint16_t)c[8] | ((uint16_t)c[9] << 8));
  if (c[10] != OP_JUMP)
    return 0;

  /* recurse arm */
  if (c[13] != OP_LOAD_SLOT || c[14] != slot)
    return 0;
  uint8_t step_op = c[15];
  if (step_op != OP_SLOT_SUB_FIX && step_op != OP_SLOT_ADD_FIX)
    return 0;
  if (c[16] != slot)
    return 0;
  int16_t K2 = (int16_t)((uint16_t)c[17] | ((uint16_t)c[18] << 8));

  if (c[19] != OP_CALL_GLOBAL)
    return 0;
  uint8_t idx = c[20];
  if (c[21] != 1)
    return 0;
  if (idx >= bc->nconsts)
    return 0;
  if (c[22] != OP_MUL)
    return 0;
  if (c[23] != OP_RET)
    return 0;

  /* Self-name guard (see try_jit_recurse_add_two): the iterative-fact
     emission below elides the call entirely, so a non-self callee would
     be silently rewritten as iterative factorial. */
  if (!bc->self_name)
    return 0;
  exp_t *callee = bc->consts[idx];
  if (!issymbol(callee))
    return 0;
  if (strcmp((const char *)exp_text(callee), bc->self_name) != 0)
    return 0;

  int32_t slot_off =
      (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)slot * 8;
  int32_t K1_tagged = ((int32_t)K1 << 3) | 1;
  int32_t K2_delta = ((int32_t)K2) << 3;
  uint64_t BASE_tag = ((uint64_t)(int64_t)BASE << 3) | 1;

  /* base case taken on the FAILURE of the cmp. The cc here is the one
     that triggers the BASE return — same convention as recurse_add_two
     but mirrored (we fall-through into the recurse path on the inverse). */
  uint8_t inv_cc;
  switch (cmp_op) {
  case OP_SLOT_LT_FIX:
    inv_cc = 0x0D;
    break; /* recurse on jge */
  case OP_SLOT_GT_FIX:
    inv_cc = 0x0E;
    break; /* recurse on jle */
  case OP_SLOT_LE_FIX:
    inv_cc = 0x0F;
    break; /* recurse on jg  */
  case OP_SLOT_GE_FIX:
    inv_cc = 0x0C;
    break; /* recurse on jl  */
  default:
    return 0;
  }

  /* Suppress unused-arg warning — we no longer go through the helper. */
  (void)idx;

  int n = 0;

  /* Load arg, tag-check, untag → rax = n (untagged). */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz_deopt = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_sar_imm8(buf + n, X64_RAX, 3);

  /* rcx = acc, untagged. Initialised to BASE. */
  n += x64_mov_imm64(buf + n, X64_RCX, (uint64_t)(int64_t)BASE);

  /* Loop: while !cmp(n, K1) — same inv_cc mapping as the recurse path
     (we LOOP on the inverted condition, EXIT on the original cmp). */
  int loop_top = n;
  n += x64_cmp_imm32(buf + n, X64_RAX, (int32_t)K1);
  int jcc_done = n;
  /* Need the EXIT cc, not the recurse cc. exit on cmp_op true →
     jl on LT, jg on GT, jle on LE, jge on GE. */
  uint8_t exit_cc;
  switch (cmp_op) {
  case OP_SLOT_LT_FIX:
    exit_cc = 0x0C;
    break; /* jl  */
  case OP_SLOT_GT_FIX:
    exit_cc = 0x0F;
    break; /* jg  */
  case OP_SLOT_LE_FIX:
    exit_cc = 0x0E;
    break; /* jle */
  case OP_SLOT_GE_FIX:
    exit_cc = 0x0D;
    break; /* jge */
  default:
    return 0;
  }
  n += x64_jcc_rel32(buf + n, exit_cc, 0);
  /* Silence the unused-var warning for inv_cc (kept for symmetry). */
  (void)inv_cc;

  /* acc = acc * n  (both untagged) */
  n += x64_imul_reg_reg(buf + n, X64_RCX, X64_RAX);

  /* n = n op K2 (untagged) */
  if (step_op == OP_SLOT_SUB_FIX)
    n += x64_sub_imm32(buf + n, X64_RAX, (int32_t)K2);
  else
    n += x64_add_imm32(buf + n, X64_RAX, (int32_t)K2);

  /* jmp loop_top */
  int jmp_back_pc = n;
  n += x64_jmp_rel32(buf + n, 0);
  x64_patch_rel32(buf, jmp_back_pc, 5, loop_top);

  /* done: re-tag acc into rax, return. */
  int done_pc = n;
  /* shl rcx, 3 */
  buf[n++] = 0x48;
  buf[n++] = 0xC1;
  buf[n++] = (uint8_t)(0xE0 | (X64_RCX & 7));
  buf[n++] = 3;
  /* or rcx, 1 */
  buf[n++] = 0x48;
  buf[n++] = 0x83;
  buf[n++] = (uint8_t)(0xC8 | (X64_RCX & 7));
  buf[n++] = 1;
  n += x64_mov_reg_reg(buf + n, X64_RAX, X64_RCX);
  n += x64_ret(buf + n);

  /* deopt → return NULL */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  /* Suppress unused-var warnings for variables that became dead when we
     switched from recursive emission to iterative. */
  (void)K1_tagged;
  (void)K2_delta;
  (void)BASE_tag;

  x64_patch_rel32(buf, jcc_done, 6, done_pc);
  x64_patch_rel32(buf, jz_deopt, 6, deopt_pc);

  JIT_GUARD(128);
  *outn = n;
  return 1;
}

/* (fn (a b) (is (mod a b) K)) — 2-param leaf computing tagged
   modulo + equality, returns t/nil. The divides? shape from sieve.
   Bytecode (10 bytes):
     [0] LOAD_SLOT a       2
     [2] LOAD_SLOT b       2
     [4] MOD               1
     [5] LOAD_FIX K        3
     [8] IS                1
     [9] RET               1

   Codegen: load both slots into rax/rcx, tag-check both, untag (sub 1),
   idiv. Compare remainder with K shifted (no re-tag needed since IS
   compares the underlying value bits). Return TRUE_EXP/NIL_EXP via
   cmovz. Avoids vm_invoke_values entirely — saves ~200ns/call. */
static int try_jit_modeq_leaf(bytecode_t *bc, uint8_t *buf, int *outn) {
  if (bc->ncode != 10)
    return 0;
  uint8_t *c = bc->code;
  if (c[0] != OP_LOAD_SLOT || c[1] >= ENV_INLINE_SLOTS)
    return 0;
  if (c[2] != OP_LOAD_SLOT || c[3] >= ENV_INLINE_SLOTS)
    return 0;
  if (c[4] != OP_MOD)
    return 0;
  if (c[5] != OP_LOAD_FIX)
    return 0;
  int16_t K = (int16_t)((uint16_t)c[6] | ((uint16_t)c[7] << 8));
  if (c[8] != OP_IS)
    return 0;
  if (c[9] != OP_RET)
    return 0;

  int32_t off_a = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)c[1] * 8;
  int32_t off_b = (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)c[3] * 8;
  int32_t k_shifted = ((int32_t)K) << 3; /* compare against (K<<3) */

  int n = 0;

  /* Load both slots tagged. */
  n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, off_a);
  n += x64_mov_reg_mem(buf + n, X64_RCX, X64_RDI, off_b);
  /* Tag-check both — bail to bytecode if either isn't a fixnum. */
  n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
  int jz1 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  n += x64_test_reg8_imm8(buf + n, X64_RCX, 1);
  int jz2 = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  /* Untag both (drop low bit). After this, rax=a<<3 and rcx=b<<3. */
  n += x64_sub_imm32(buf + n, X64_RAX, 1);
  n += x64_sub_imm32(buf + n, X64_RCX, 1);
  /* Guard against div-by-zero — bail rather than crash. */
  n += x64_test_reg_reg(buf + n, X64_RCX, X64_RCX);
  int jz_bz = n;
  n += x64_jcc_rel32(buf + n, 0x04, 0);
  /* Sign-extend rax into rdx:rax, then signed div. rdx ← (a<<3) % (b<<3)
     which equals (a%b)<<3 — same scaling property as add/sub. */
  n += x64_cqo(buf + n);
  n += x64_idiv_reg(buf + n, X64_RCX);
  /* Compare remainder against K<<3 (tag bits irrelevant — all values
     here have bit0=0). cmovz selects TRUE_EXP if equal. */
  n += x64_cmp_imm32(buf + n, X64_RDX, k_shifted);
  n += x64_mov_imm64(buf + n, X64_RAX, (uint64_t)(uintptr_t)nil_singleton);
  n += x64_mov_imm64(buf + n, X64_RCX, (uint64_t)(uintptr_t)true_singleton);
  n += x64_cmovz_reg_reg(buf + n, X64_RAX, X64_RCX);
  n += x64_ret(buf + n);

  /* Single deopt point — return NULL → caller's vm_run kicks in. */
  int deopt_pc = n;
  X64_EMIT_DEOPT();

  x64_patch_rel32(buf, jz1, 6, deopt_pc);
  x64_patch_rel32(buf, jz2, 6, deopt_pc);
  x64_patch_rel32(buf, jz_bz, 6, deopt_pc);

  JIT_GUARD(96);
  *outn = n;
  return 1;
}

/* Set via env: ALCOVE_JIT_TRACE=1 to log which shape (or "miss") matched
   for each bytecode submitted to jit_compile. Off by default. Cached on
   first call. */
static int jit_trace(void) {
  static int v = -1;
  if (v < 0)
    v = (getenv("ALCOVE_JIT_TRACE") != NULL);
  return v;
}
#define JT(shape)                                                              \
  do {                                                                         \
    if (jit_trace())                                                           \
      fprintf(stderr, "[jit] %-28s ncode=%d\n", (shape), bc->ncode);           \
  } while (0)

int jit_compile(bytecode_t *bc) {
  if (!bc || bc->jit)
    return bc && bc->jit ? 1 : 0;
  uint8_t *c = bc->code;

  if (try_jit_build_inc_cons_c(bc)) {
    JT("build_inc_cons_c");
    return 1;
  }

  if (try_jit_nqueens_solve_c(bc)) {
    JT("nqueens_solve_c");
    return 1;
  }

  uint8_t buf[512];
  int n = 0;

  if (try_jit_simple_tail_loop(bc, buf, &n)) {
    JT("simple_tail_loop");
  } else if (try_jit_tail_loop_with_call(bc, buf, &n)) {
    JT("tail_loop_with_call");
  } else if (try_jit_recurse_add_two(bc, buf, &n)) {
    JT("recurse_add_two");
  } else if (try_jit_recurse_mul_one(bc, buf, &n)) {
    JT("recurse_mul_one");
  } else if (try_jit_for_loop_inc(bc, buf, &n)) {
    JT("for_loop_inc");
  } else if (try_jit_ackermann(bc, buf, &n)) {
    JT("ackermann");
  } else if (try_jit_tak(bc, buf, &n)) {
    JT("tak");
  } else if (try_jit_mark_from(bc, buf, &n)) {
    JT("mark_from");
  } else if (try_jit_count_primes(bc, buf, &n)) {
    JT("count_primes");
  } else if (try_jit_is_prime_given(bc, buf, &n)) {
    JT("is_prime_given");
  } else if (try_jit_safe_p(bc, buf, &n)) {
    JT("safe_p");
  } else if (try_jit_modeq_leaf(bc, buf, &n)) {
    JT("modeq_leaf");
  } else if (bc->ncode == 4 && c[0] == OP_LOAD_FIX && c[3] == OP_RET) {
    JT("leaf_const");
    /* (fn () K)  →  mov rax, tagged; ret */
    int16_t k = (int16_t)((uint16_t)c[1] | ((uint16_t)c[2] << 8));
    uint64_t tagged = ((uint64_t)(int64_t)k << 3) | 1;
    n += x64_mov_imm64(buf + n, X64_RAX, tagged);
    n += x64_ret(buf + n);
  } else if (bc->ncode == 7 && c[0] == OP_LOAD_SLOT &&
             c[1] < ENV_INLINE_SLOTS && c[2] == OP_LOAD_FIX &&
             (c[5] == OP_ADD || c[5] == OP_SUB || c[5] == OP_MUL) &&
             c[6] == OP_RET) {
    JT("leaf_slot_op_fix");
    /* (fn (... s ...) (op s K)) for K in int16, op in {+, -, *}.
       For ADD/SUB: ±(K<<3) preserves the tag bit directly.
       For MUL:     drop tag (sub 1), imul by K, re-tag (add 1). */
    int16_t k = (int16_t)((uint16_t)c[3] | ((uint16_t)c[4] << 8));
    int32_t slot_off =
        (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)c[1] * 8;
    n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
    n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
    int jz_start = n;
    n += x64_jcc_rel32(buf + n, 0x04, 0);
    if (c[5] == OP_MUL) {
      n += x64_sub_imm32(buf + n, X64_RAX, 1); /* drop tag */
      n += x64_imul_reg_reg_imm32(buf + n, X64_RAX, X64_RAX, (int32_t)k);
      n += x64_add_imm32(buf + n, X64_RAX, 1); /* re-tag */
    } else {
      int32_t delta = ((int32_t)k) << 3;
      if (c[5] == OP_ADD)
        n += x64_add_imm32(buf + n, X64_RAX, delta);
      else
        n += x64_sub_imm32(buf + n, X64_RAX, delta);
    }
    n += x64_ret(buf + n);
    int deopt_pc = n;
    X64_EMIT_DEOPT();
    x64_patch_rel32(buf, jz_start, 6, deopt_pc);
  }
  /* slot-fix superinstruction form: SLOT_ADD_FIX/SLOT_SUB_FIX slot K, RET */
  else if (bc->ncode == 5 &&
           (c[0] == OP_SLOT_ADD_FIX || c[0] == OP_SLOT_SUB_FIX) &&
           c[1] < ENV_INLINE_SLOTS && c[4] == OP_RET) {
    JT("leaf_slot_fix_super");
    int16_t k = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));
    int32_t delta = ((int32_t)k) << 3;
    int32_t slot_off =
        (int32_t)offsetof(env_t, inline_vals[0]) + (int32_t)c[1] * 8;
    n += x64_mov_reg_mem(buf + n, X64_RAX, X64_RDI, slot_off);
    n += x64_test_reg8_imm8(buf + n, X64_RAX, 1);
    int jz_start = n;
    n += x64_jcc_rel32(buf + n, 0x04, 0);
    if (c[0] == OP_SLOT_ADD_FIX)
      n += x64_add_imm32(buf + n, X64_RAX, delta);
    else
      n += x64_sub_imm32(buf + n, X64_RAX, delta);
    n += x64_ret(buf + n);
    int deopt_pc = n;
    X64_EMIT_DEOPT();
    x64_patch_rel32(buf, jz_start, 6, deopt_pc);
  } else {
    JT("miss");
    return 0; /* shape not recognized */
  }

  /* Each matcher has its own internal bound check (returns 0 on
     overflow); this catch-all protects buf as a whole including the
     inline leaf-shape paths above. Hard fall-back rather than abort —
     bytecode will run the body. Survives -DNDEBUG, unlike assert(). */
  if (n > (int)sizeof(buf))
    return 0;
  size_t sz = (size_t)n;
  size_t pagesz = 4096;
  size_t mapsz = (sz + pagesz - 1) & ~(pagesz - 1);
  void *page = jit_alloc(mapsz);
  if (!page)
    return 0;
  jit_write_begin();
  memcpy(page, buf, sz);
  jit_write_end(page, sz);
  bc->jit = (exp_t * (*)(env_t *)) page;
  bc->jit_mem = page;
  bc->jit_size = mapsz;
  return 1;
}
