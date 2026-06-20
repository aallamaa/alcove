/* jit_arm64.h — arm64 (AArch64) JIT backend: instruction encoders + shape
 * matchers.
 *
 * FRAGMENT #included into alcove.c inside `#ifdef ALCOVE_JIT` — NOT a
 * standalone header and NOT separately compiled. It must stay in the single
 * alcove.c translation unit so the emitters inline against the value model
 * and the env layout (offsetof(env_t, inline_vals[0]) is baked into emitted
 * code). See the #include site in alcove.c. `make tidy` lints it via adder.c.
 */
/* ===================== arm64 backend ===================== */

/* arm64 instruction encoders. All return uint32_t little-endian; arm64
   is fixed-width 4-byte instructions. */

/* LDR Xt, [Xn, #imm]   — imm is 8-byte aligned offset, 0..32760. */
static uint32_t arm64_ldr_imm(int rt, int rn, int byte_offset) {
  uint32_t imm12 = (uint32_t)(byte_offset / 8) & 0xfff;
  return 0xF9400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
/* ADD Xd, Xn, #imm12 (no shift). */
static uint32_t arm64_add_imm(int rd, int rn, int imm) {
  return 0x91000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* SUB Xd, Xn, #imm12 (no shift). */
static uint32_t arm64_sub_imm(int rd, int rn, int imm) {
  return 0xD1000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* MOVZ Xd, #imm16, LSL #(hw*16) */
static uint32_t arm64_movz(int rd, uint16_t imm, int hw) {
  return 0xD2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) |
         (uint32_t)rd;
}
/* MOVK Xd, #imm16, LSL #(hw*16) — keep other bits */
static uint32_t arm64_movk(int rd, uint16_t imm, int hw) {
  return 0xF2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) |
         (uint32_t)rd;
}
/* RET (uses x30 by default). */
static uint32_t arm64_ret(void) { return 0xD65F03C0u; }
/* STR Xt, [Xn, #imm]   — imm is 8-byte aligned offset. */
static uint32_t arm64_str_imm(int rt, int rn, int byte_offset) {
  uint32_t imm12 = (uint32_t)(byte_offset / 8) & 0xfff;
  return 0xF9000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
/* CMP Xn, #imm12 — alias for SUBS XZR, Xn, #imm12. */
static uint32_t arm64_cmp_imm(int rn, int imm) {
  return 0xF1000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         31u;
}
/* B (unconditional, PC-relative). off is in INSTRUCTIONS (×4 for bytes),
 * signed. */
/* Range-check helper: returns 1 if `off_insns` fits a signed `bits`-bit
   field (i.e., -(1<<(bits-1)) <= off < (1<<(bits-1))). On out-of-range
   the encoders abort() rather than silently truncate — silently is the
   class of bug that gave us SIGBUS earlier (commit 6fc3101). Current
   shapes are <128 instructions so all branches stay well within range;
   this is defensive armor against future shape additions. */
static void arm64_check_off(int off_insns, int bits, const char *who) {
  int lim = 1 << (bits - 1);
  if (off_insns < -lim || off_insns >= lim) {
    fprintf(stderr, "alcove jit: %s offset %d out of signed %d-bit range\n",
            who, off_insns, bits);
    abort();
  }
}

static uint32_t arm64_b(int off_insns) {
  arm64_check_off(off_insns, 26, "B");
  return 0x14000000u | ((uint32_t)off_insns & 0x3FFFFFFu);
}
/* B.cond — off in instructions, signed 19-bit. cond is the 4-bit code:
   GE=10, LT=11, GT=12, LE=13. */
static uint32_t arm64_b_cond(int cond, int off_insns) {
  arm64_check_off(off_insns, 19, "B.cond");
  return 0x54000000u | (((uint32_t)off_insns & 0x7FFFFu) << 5) |
         ((uint32_t)cond & 0xfu);
}
/* TBZ Xt, #bit, label — branch if bit is zero. off in instructions, signed
 * 14-bit. */
static uint32_t arm64_tbz(int rt, int bit, int off_insns) {
  arm64_check_off(off_insns, 14, "TBZ");
  uint32_t b40 = (uint32_t)(bit & 0x1f);
  uint32_t b5 = (bit & 0x20) ? 1u : 0u;
  return 0x36000000u | (b5 << 31) | (b40 << 19) |
         (((uint32_t)off_insns & 0x3FFFu) << 5) | (uint32_t)(rt & 0x1f);
}
/* TBNZ Xt, #bit, label — branch if bit is non-zero. Used by the
   FLAG_SHARED gate (multi-threaded only) AND by always-on typed-vec
   kind checks in the listsum/nqueens shapes, so it must be available
   in single-threaded builds too. */
static uint32_t arm64_tbnz(int rt, int bit, int off_insns) {
  arm64_check_off(off_insns, 14, "TBNZ");
  uint32_t b40 = (uint32_t)(bit & 0x1f);
  uint32_t b5 = (bit & 0x20) ? 1u : 0u;
  return 0x37000000u | (b5 << 31) | (b40 << 19) |
         (((uint32_t)off_insns & 0x3FFFu) << 5) | (uint32_t)(rt & 0x1f);
}
/* LDRB Wt, [Xn, #imm] — unsigned byte load (zero-extended). Reads the
   low byte of exp_t.flags for both FLAG_SHARED and the typed-vec kind
   check; always compiled in. */
static uint32_t arm64_ldrb_imm(int rt, int rn, int byte_offset) {
  return 0x39400000u | (((uint32_t)byte_offset & 0xFFFu) << 10) |
         ((uint32_t)(rn & 0x1f) << 5) | (uint32_t)(rt & 0x1f);
}
/* MOV Xd, Xm  — alias for ORR Xd, XZR, Xm. */
static uint32_t arm64_mov_reg(int rd, int rm) {
  return 0xAA0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}
/* MUL Xd, Xn, Xm  — alias for MADD Xd, Xn, Xm, XZR (signed 64-bit mul,
   low 64 bits of result, no flags). */
static uint32_t arm64_mul(int rd, int rn, int rm) {
  return 0x9B007C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* ADD Xd, Xn, Xm — register form (no shift). */
static uint32_t arm64_add_reg(int rd, int rn, int rm) {
  return 0x8B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* SUB Xd, Xn, Xm — register form (no shift). */
__attribute__((unused)) static uint32_t arm64_sub_reg(int rd, int rn, int rm) {
  return 0xCB000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* CMP Xn, Xm — alias for SUBS XZR, Xn, Xm. */
static uint32_t arm64_cmp_reg(int rn, int rm) {
  return 0xEB000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | 31u;
}
/* Flag-setting arithmetic for integer-overflow detection (no jo on arm64 — we
   read the V flag with B.VS, or compare the SMULH high word for MUL). */
/* ADDS Xd, Xn, #imm12 (sets NZCV). */
static uint32_t arm64_adds_imm(int rd, int rn, int imm) {
  return 0xB1000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* SUBS Xd, Xn, #imm12 (sets NZCV). */
static uint32_t arm64_subs_imm(int rd, int rn, int imm) {
  return 0xF1000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* SMULH Xd, Xn, Xm — signed high 64 bits of the 128-bit product. */
static uint32_t arm64_smulh(int rd, int rn, int rm) {
  return 0x9B407C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* CMP Xn, Xm, ASR #sh — alias SUBS XZR, Xn, Xm, ASR #sh (sets NZCV). */
static uint32_t arm64_cmp_reg_asr(int rn, int rm, int sh) {
  return 0xEB800000u | ((uint32_t)rm << 16) | ((uint32_t)(sh & 0x3f) << 10) |
         ((uint32_t)rn << 5) | 31u;
}
#define ARM64_COND_NE 0x1
#define ARM64_COND_VS 0x6 /* overflow set */
/* ASR Xd, Xn, #shift  (arithmetic shift right; sign-extends top bit).
   Encoded via SBFM Xd, Xn, #shift, #63. */
static uint32_t arm64_asr_imm(int rd, int rn, int shift) {
  uint32_t s = (uint32_t)(shift & 0x3f);
  return 0x9340FC00u | (s << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* LSL Xd, Xn, #shift  (logical shift left).
   Encoded via UBFM Xd, Xn, #(-shift mod 64), #(63-shift). */
static uint32_t arm64_lsl_imm(int rd, int rn, int shift) {
  uint32_t s = (uint32_t)(shift & 0x3f);
  uint32_t imr = (64u - s) & 0x3fu;
  uint32_t ims = 63u - s;
  return 0xD3400000u | (imr << 16) | (ims << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* ORR Xd, Xn, #1  — set bit 0. We only need this exact form (re-tag a
   shifted value back into a tagged fixnum). Encodes a 64-bit logical
   immediate via N=1, immr=0, imms=0 (one-bit pattern at position 0). */
static uint32_t arm64_orr_imm_bit0(int rd, int rn) {
  return 0xB2400000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* STP Xt1, Xt2, [SP, #imm]!  — pre-indexed store-pair, SP -= |imm|.
   imm is in BYTES, must be 8-aligned, signed 7-bit shifted (×8). */
static uint32_t arm64_stp_pre_sp(int rt1, int rt2, int byte_offset) {
  uint32_t imm7 = (uint32_t)((byte_offset / 8) & 0x7f);
  return 0xA9800000u | (imm7 << 15) | ((uint32_t)rt2 << 10) | (31u << 5) |
         (uint32_t)rt1;
}
/* LDP Xt1, Xt2, [SP], #imm  — post-indexed load-pair, SP += imm. */
static uint32_t arm64_ldp_post_sp(int rt1, int rt2, int byte_offset) {
  uint32_t imm7 = (uint32_t)((byte_offset / 8) & 0x7f);
  return 0xA8C00000u | (imm7 << 15) | ((uint32_t)rt2 << 10) | (31u << 5) |
         (uint32_t)rt1;
}
/* STP Xt1, Xt2, [SP, #imm]   — signed-offset store-pair (no writeback). */
static uint32_t arm64_stp_off_sp(int rt1, int rt2, int byte_offset) {
  uint32_t imm7 = (uint32_t)((byte_offset / 8) & 0x7f);
  return 0xA9000000u | (imm7 << 15) | ((uint32_t)rt2 << 10) | (31u << 5) |
         (uint32_t)rt1;
}
/* LDP Xt1, Xt2, [SP, #imm]   — signed-offset load-pair (no writeback). */
static uint32_t arm64_ldp_off_sp(int rt1, int rt2, int byte_offset) {
  uint32_t imm7 = (uint32_t)((byte_offset / 8) & 0x7f);
  return 0xA9400000u | (imm7 << 15) | ((uint32_t)rt2 << 10) | (31u << 5) |
         (uint32_t)rt1;
}
/* MOV Xd, SP  — alias of ADD Xd, SP, #0. SP is encoded as Rn=31 in
   ADD/SUB-immediate forms (only XZR otherwise). */
static uint32_t arm64_mov_from_sp(int rd) {
  return 0x91000000u | (31u << 5) | (uint32_t)rd; /* add Rd, SP, #0 */
}
/* BL #imm  — branch with link, signed 26-bit instruction offset (±128MB).
   Caller computes off_insns relative to this BL's PC. */
__attribute__((unused)) static uint32_t arm64_bl(int off_insns) {
  return 0x94000000u | ((uint32_t)off_insns & 0x3FFFFFFu);
}
/* BLR Xn  — branch with link to register (indirect call). */
__attribute__((unused)) static uint32_t arm64_blr(int rn) {
  return 0xD63F0000u | ((uint32_t)rn << 5);
}
/* BR Xn  — branch to register, no link (tail call; callee's RET returns to
   our caller). */
__attribute__((unused)) static uint32_t arm64_br(int rn) {
  return 0xD61F0000u | ((uint32_t)(rn & 0x1f) << 5);
}
/* SDIV Xd, Xn, Xm  — signed 64-bit divide. */
static uint32_t arm64_sdiv(int rd, int rn, int rm) {
  return 0x9AC00C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* MSUB Xd, Xn, Xm, Xa  — Xd = Xa - Xn*Xm (used to compute remainder). */
static uint32_t arm64_msub(int rd, int rn, int rm, int ra) {
  return 0x9B008000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) |
         ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* CSEL Xd, Xn, Xm, cond  — Xd = (cond ? Xn : Xm). */
static uint32_t arm64_csel(int rd, int rn, int rm, int cond) {
  return 0x9A800000u | ((uint32_t)rm << 16) | ((uint32_t)(cond & 0xf) << 12) |
         ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* CBZ Xt, label — branch if Xt is zero. off in instructions, 19-bit signed. */
static uint32_t arm64_cbz(int rt, int off_insns) {
  arm64_check_off(off_insns, 19, "CBZ");
  return 0xB4000000u | (((uint32_t)off_insns & 0x7FFFFu) << 5) |
         (uint32_t)(rt & 0x1f);
}
/* CBNZ Xt, label — branch if Xt is non-zero. */
static uint32_t arm64_cbnz(int rt, int off_insns) {
  arm64_check_off(off_insns, 19, "CBNZ");
  return 0xB5000000u | (((uint32_t)off_insns & 0x7FFFFu) << 5) |
         (uint32_t)(rt & 0x1f);
}
/* 32-bit register encoders (W-form). nref is `int` so we load/store 4 bytes. */
/* LDR Wt, [Xn, #imm]  — imm is 4-byte aligned offset, 0..16380. */
static uint32_t arm64_ldr_w_imm(int rt, int rn, int byte_offset) {
  uint32_t imm12 = (uint32_t)(byte_offset / 4) & 0xfff;
  return 0xB9400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
/* STR Wt, [Xn, #imm]. */
static uint32_t arm64_str_w_imm(int rt, int rn, int byte_offset) {
  uint32_t imm12 = (uint32_t)(byte_offset / 4) & 0xfff;
  return 0xB9000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
/* ADD Wd, Wn, #imm12 (no shift). */
static uint32_t arm64_add_w_imm(int rd, int rn, int imm) {
  return 0x11000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* SUB Wd, Wn, #imm12 (no shift). */
static uint32_t arm64_sub_w_imm(int rd, int rn, int imm) {
  return 0x51000000u | ((uint32_t)(imm & 0xfff) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
/* CMP Wn, Wm — alias for SUBS WZR, Wn, Wm. */
__attribute__((unused)) static uint32_t arm64_cmp_reg_w(int rn, int rm) {
  return 0x6B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | 31u;
}
/* CBZ Wt, label — 32-bit variant. */
static uint32_t arm64_cbz_w(int rt, int off_insns) {
  arm64_check_off(off_insns, 19, "CBZ.W");
  return 0x34000000u | (((uint32_t)off_insns & 0x7FFFFu) << 5) |
         (uint32_t)(rt & 0x1f);
}
/* LDRH Wt, [Xn, #imm] — unsigned halfword load (zero-extended). imm scaled
   by 2 (0..8190). Reads the 2-byte exp_t.type field for the float-box check. */
static uint32_t arm64_ldrh_imm(int rt, int rn, int byte_offset) {
  uint32_t imm12 = (uint32_t)(byte_offset / 2) & 0xfff;
  return 0x79400000u | (imm12 << 10) | ((uint32_t)(rn & 0x1f) << 5) |
         (uint32_t)(rt & 0x1f);
}
/* AND Xd, Xn, #7 — mask the low 3 tag bits. Logical-immediate encoding for
   the 3-consecutive-ones pattern: N=1 (64-bit element), immr=0, imms=2
   (run length S+1 = 3). */
static uint32_t arm64_and_imm7(int rd, int rn) {
  return 0x92400800u | ((uint32_t)(rn & 0x1f) << 5) | (uint32_t)(rd & 0x1f);
}

/* ---- scalar floating-point (double) encoders — used by the float-acc loop.
   All operate on the D-register file (64-bit IEEE double, low half of V). ----
 */
/* LDR Dt, [Xn, #imm] — load double, imm 8-byte-aligned (×8, 0..32760).
   Same shape as the integer LDR (0xF9400000) with the SIMD&FP V bit (bit 26)
   set: 0xF9400000 | (1<<26) = 0xFD400000. */
static uint32_t arm64_ldr_d_imm(int rt, int rn, int byte_offset) {
  uint32_t imm12 = (uint32_t)(byte_offset / 8) & 0xfff;
  return 0xFD400000u | (imm12 << 10) | ((uint32_t)(rn & 0x1f) << 5) |
         (uint32_t)(rt & 0x1f);
}
/* FMOV Dd, Xn — copy the 64 raw bits of Xn into Dd (no conversion). Used to
   load a precomputed double bit-pattern (the float const) into a D-reg.
   sf=1 type=01(double) rmode=00 opcode=111 → 0x9E670000. */
static uint32_t arm64_fmov_d_x(int dd, int xn) {
  return 0x9E670000u | ((uint32_t)(xn & 0x1f) << 5) | (uint32_t)(dd & 0x1f);
}
/* SCVTF Dd, Xn — signed 64-bit integer → double (round to nearest).
   sf=1 type=01 rmode=00 opcode=010 → 0x9E620000. */
static uint32_t arm64_scvtf_d_x(int dd, int xn) {
  return 0x9E620000u | ((uint32_t)(xn & 0x1f) << 5) | (uint32_t)(dd & 0x1f);
}
/* FADD Dd, Dn, Dm — double add (type=01, opcode=0010): 0x1E602800. */
static uint32_t arm64_fadd_d(int dd, int dn, int dm) {
  return 0x1E602800u | ((uint32_t)(dm & 0x1f) << 16) |
         ((uint32_t)(dn & 0x1f) << 5) | (uint32_t)(dd & 0x1f);
}
/* FSUB Dd, Dn, Dm — double subtract (type=01, opcode=0011): 0x1E603800. */
static uint32_t arm64_fsub_d(int dd, int dn, int dm) {
  return 0x1E603800u | ((uint32_t)(dm & 0x1f) << 16) |
         ((uint32_t)(dn & 0x1f) << 5) | (uint32_t)(dd & 0x1f);
}
/* FMUL Dd, Dn, Dm — double multiply (type=01, opcode=0000): 0x1E600800. */
static uint32_t arm64_fmul_d(int dd, int dn, int dm) {
  return 0x1E600800u | ((uint32_t)(dm & 0x1f) << 16) |
         ((uint32_t)(dn & 0x1f) << 5) | (uint32_t)(dd & 0x1f);
}
/* FDIV Dd, Dn, Dm — double divide (type=01, opcode=0001): 0x1E601800. */
static uint32_t arm64_fdiv_d(int dd, int dn, int dm) {
  return 0x1E601800u | ((uint32_t)(dm & 0x1f) << 16) |
         ((uint32_t)(dn & 0x1f) << 5) | (uint32_t)(dd & 0x1f);
}
/* FMOV Dd, Dn — double register copy: 0x1E604000 | (Dn<<5) | Dd. */
__attribute__((unused)) static uint32_t arm64_fmov_d_d(int dd, int dn) {
  return 0x1E604000u | ((uint32_t)(dn & 0x1f) << 5) | (uint32_t)(dd & 0x1f);
}
/* FCMP Dn, Dm — double compare, sets NZCV (unordered/NaN → N=0,Z=0,C=1,V=1):
   0x1E602000 | (Dm<<16) | (Dn<<5). Used by the numeric-loop compiler. */
__attribute__((unused)) static uint32_t arm64_fcmp_d(int dn, int dm) {
  return 0x1E602000u | ((uint32_t)(dm & 0x1f) << 16) |
         ((uint32_t)(dn & 0x1f) << 5);
}
/* FCMP Dn, #0.0 — compare a double to zero: 0x1E602008 | (Dn<<5). */
__attribute__((unused)) static uint32_t arm64_fcmp_d_zero(int dn) {
  return 0x1E602008u | ((uint32_t)(dn & 0x1f) << 5);
}

/* arm64 shape-emitter helpers. PATCH_DEOPT_* and the EMIT macros below
   require `out`, `n`, and `deopt_pc` to be in scope. Note: the inline
   blocks in jit_compile use `insns[]` instead of `out` and cannot use
   these macros — see the comments at those sites.

   PATCH_DEOPT_*(slot, ...): back-patch a previously-reserved branch
   word at index `slot` in `out` so it targets the shared `deopt_pc`
   label. The relative offset is ALWAYS measured from `slot` itself —
   this removes the copy-paste hazard of writing
   `out[patch_a] = arm64_tbz(.., deopt_pc - patch_b)` with a mismatched
   slot, which would silently emit a wrong branch target. */
#define PATCH_DEOPT_TBZ(slot, rt, bit)                                         \
  (out[(slot)] = arm64_tbz((rt), (bit), deopt_pc - (slot)))
#define PATCH_DEOPT_TBNZ(slot, rt, bit)                                        \
  (out[(slot)] = arm64_tbnz((rt), (bit), deopt_pc - (slot)))
#define PATCH_DEOPT_CBZ(slot, rt)                                              \
  (out[(slot)] = arm64_cbz((rt), deopt_pc - (slot)))
#define PATCH_DEOPT_CBZ_W(slot, rt)                                            \
  (out[(slot)] = arm64_cbz_w((rt), deopt_pc - (slot)))

/* Emit the arm64 deopt stub. Must be placed after all PATCH_DEOPT_* calls
   that reference deopt_pc so the back-patches can target the correct pc. */
#define ARM64_EMIT_DEOPT()                                                     \
  do {                                                                         \
    out[n++] = arm64_movz(0, 0, 0); /* x0 = 0 (NULL) */                        \
    out[n++] = arm64_ret();                                                    \
  } while (0)

/* Pack an untagged int64 in src_reg as a fixnum in x0 and return. */
#define ARM64_EMIT_RETAG_RET(src_reg)                                          \
  do {                                                                         \
    out[n++] = arm64_lsl_imm(0, (src_reg), 3); /* x0 = src << 3  */            \
    out[n++] = arm64_orr_imm_bit0(0, 0);       /* x0 |= 1 (fixnum tag) */      \
    out[n++] = arm64_ret();                                                    \
  } while (0)

/* Checked re-tag: x0 = (src<<3)|1, with a round-trip test that the value fits
   the 61-bit range — (src<<3)>>3 == src, else the shift dropped high bits.
   (ADDS with a shifted operand can't be used: the barrel shifter computes
   src<<3 mod 2^64 before the add, so 0+X never sets V.) Uses x9 as scratch
   (caller-saved); reserves a B.NE slot at `ovf_slot` to patch toward deopt_pc.
 */
#define ARM64_EMIT_RETAG_RET_CK(src_reg, ovf_slot)                             \
  do {                                                                         \
    out[n++] = arm64_lsl_imm(0, (src_reg), 3); /* x0 = src<<3 */               \
    out[n++] = arm64_asr_imm(9, 0, 3);         /* x9 = x0 >>arith 3 */         \
    out[n++] = arm64_cmp_reg(9, (src_reg));    /* round-trips? */              \
    (ovf_slot) = n++;                          /* reserve B.NE deopt */        \
    out[n++] = arm64_orr_imm_bit0(0, 0);                                       \
    out[n++] = arm64_ret();                                                    \
  } while (0)
/* Patch a reserved B.cond branch word toward the shared deopt_pc. */
#define PATCH_DEOPT_BNE(slot)                                                  \
  (out[(slot)] = arm64_b_cond(ARM64_COND_NE, deopt_pc - (slot)))
#define PATCH_DEOPT_BVS(slot)                                                  \
  (out[(slot)] = arm64_b_cond(ARM64_COND_VS, deopt_pc - (slot)))

/* Materialize an arbitrary 64-bit immediate into Xd via MOVZ + up-to-3 MOVKs.
 */
static int emit_mov64(uint32_t *out, int rd, uint64_t v) {
  int n = 0;
  out[n++] = arm64_movz(rd, (uint16_t)(v & 0xffff), 0);
  if ((v >> 16) & 0xffff)
    out[n++] = arm64_movk(rd, (uint16_t)((v >> 16) & 0xffff), 1);
  if ((v >> 32) & 0xffff)
    out[n++] = arm64_movk(rd, (uint16_t)((v >> 32) & 0xffff), 2);
  if ((v >> 48) & 0xffff)
    out[n++] = arm64_movk(rd, (uint16_t)((v >> 48) & 0xffff), 3);
  return n;
}

/* Try to JIT a self-tail-recursive counter loop body of the form:
     (def f (n) (if (cmp n K1) (f (op n K2)) n))
   where cmp ∈ {<, <=, >, >=}, op ∈ {+, -}, K1 fits the cmp's tagged
   immediate range, K2 fits the arith immediate range, and the loop
   variable is a single param.
   Compiled bytecode (emit order from compile_if + compile_call's
   self-tail path with fused superinstructions):
     [SLOT_<cmp>_FIX slot K1]   4 bytes
     [BR_IF_FALSE off_to_else]  3 bytes
     [SLOT_<op>_FIX slot K2]    4 bytes
     [TAIL_SELF 1]              2 bytes
     [JUMP off]                 3 bytes  (unreachable, emitted by compile_if)
     [LOAD_SLOT slot]           2 bytes
     [RET]                      1 byte
   = 19 bytes total. */
static int try_jit_simple_tail_loop(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 19-byte self-tail counter loop — shape walk shared with the amd64 backend
     via match_simple_tail_loop (jit_common.h, which also enforces the arm64
     LDR-imm slot-offset ceiling); we emit only. */
  struct match_simple_tail_loop m;
  if (!match_simple_tail_loop(bc, &m))
    return 0;

  uint8_t cmp_op = m.cmp_op, arith_op = m.arith_op;
  int16_t cmp_imm = m.cmp_imm, arith_imm = m.arith_imm;
  int slot_off = m.slot_off;

  /* Tagged compare limit: FIX(K1) = (K1<<3)|1. If it fits arm64's u12 cmp
     immediate, compare against it directly; otherwise materialize it into a
     scratch reg (x2) once before the loop and compare register-to-register.
     This lifts the old "limit must be <= 511 (tagged <= 4095)" cap so a
     count-up loop to any int16 limit JITs — the gap that left e.g.
     (if (< n 1000) ...) on the VM path. Mirrors the wide-limit handling in
     try_jit_float_acc_loop. Tagging is monotonic for signed order, so the
     tagged compare equals the value compare in either form. */
  int64_t cmp_tagged_64 = ((int64_t)cmp_imm << 3) | 1;
  int cmp_wide = (cmp_tagged_64 < 0 || cmp_tagged_64 > 4095);

  /* Arithmetic delta is K2<<3 (preserves tag bit). Must fit u12 add/sub; a
     wider or negative step (rare — real counter loops step by ±1) deopts to
     the VM, which stays correct. */
  int arith_delta = ((int)arith_imm) << 3;
  if (arith_delta < 0 || arith_delta > 4095)
    return 0;

  /* Branch condition for "BR_IF_FALSE on cmp's result" — invert cmp.
     ARM64 cond codes: GE=10, LT=11, GT=12, LE=13. */
  int cond;
  switch (cmp_op) {
  case OP_SLOT_GT_FIX:
    cond = 13;
    break; /* !GT → LE */
  case OP_SLOT_LT_FIX:
    cond = 10;
    break; /* !LT → GE */
  case OP_SLOT_GE_FIX:
    cond = 11;
    break; /* !GE → LT */
  case OP_SLOT_LE_FIX:
    cond = 12;
    break; /* !LE → GT */
  case OP_SLOT_IS_FIX:
    cond = 0;
    break; /* base when (is slot K); loop exits on equal → EQ */
  default:
    return 0;
  }

  int n = 0;
  /* Load slot value once; verify it's a tagged fixnum (bit 0 set).
     If not, branch to deopt → return NULL → caller falls back to vm_run. */
  out[n++] = arm64_ldr_imm(1, 0, slot_off); /* ldr x1, [x0,#off] */
  int patch_tbz = n;
  out[n++] = 0; /* placeholder tbz x1,#0,deopt */
  /* Hoist a wide tagged limit into x2 (loop-invariant). */
  if (cmp_wide)
    n += emit_mov64(out + n, 2, (uint64_t)cmp_tagged_64);
  int loop_top = n;
  if (cmp_wide)
    out[n++] = arm64_cmp_reg(1, 2); /* cmp x1, x2 (wide FIX(K1)) */
  else
    out[n++] = arm64_cmp_imm(1, (int)cmp_tagged_64); /* cmp x1, #FIX(K1) */
  int patch_bcond = n;
  out[n++] = 0; /* placeholder b.cond end */
  if (arith_op == OP_SLOT_SUB_FIX)
    out[n++] = arm64_sub_imm(1, 1, arith_delta);
  else
    out[n++] = arm64_add_imm(1, 1, arith_delta);
  /* tag bit preserved across ±(K2<<3); subsequent loads stay tagged. */
  out[n++] = arm64_str_imm(1, 0, slot_off); /* str x1, [x0,#off] */
  /* Compute the rel-to-loop-top displacement from the branch's OWN PC
     (i.e. the current value of n) before writing it. Doing both in one
     `out[n++] = arm64_b(loop_top - n)` leaves the evaluation order of
     the LHS's n++ vs the RHS's read of n unspecified (C sequence-point
     rules) — gcc 14 was observed to pick the wanted order but it is
     not portable. */
  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur);
  } /* b loop_top */
  /* end: */
  int end_pc = n;
  out[patch_bcond] = arm64_b_cond(cond, end_pc - patch_bcond);
  out[n++] = arm64_mov_reg(0, 1); /* x0 = x1 (last value) */
  out[n++] = arm64_ret();
  /* deopt: */
  int deopt_pc = n;
  PATCH_DEOPT_TBZ(patch_tbz, 1, 0);
  ARM64_EMIT_DEOPT();

  /* Worst case: ~16 instructions (load, tbz, up-to-4 movz/movk for a wide
     limit, cmp, b.cond, sub/add, str, b loop, mov, ret, movz, ret + slack).
     Caller's buffer is uint32_t insns[128] — comfortable margin. Trip if a
     future tweak overruns. */
  JIT_GUARD(24);
  *outn = n;
  return 1;
}

/* Sibling of try_jit_simple_tail_loop for the *swapped-polarity* equality-base
   tail loop — the natural layout of `(def f (n) (if (is n K) n (f (- n S))))`.
   The arm64 mirror of jit_amd64.h's try_jit_simple_tail_loop_eq.

   The `is`-base if puts the BASE arm in THEN and the RECURSE arm in ELSE, so
   the bytecode is mirrored vs the gt/lt twin (step-before-base):

       SLOT_IS_FIX  slot K     4   ; cmp
       BR_IF_FALSE  off        3   ; if !=, fall to ELSE (recurse)
       LOAD_SLOT    slot       2   ; THEN: base arm = return the slot
       JUMP         off        3   ; skip ELSE
       SLOT_SUB_FIX slot S     4   ; ELSE: step (SUB or ADD)
       TAIL_SELF    1          2
       RET                     1
   = 19 bytes. Semantics: while (n != K) { n step= S; } return n.

   Emitted code is identical in spirit to the gt twin — counter in x1, in-buffer
   back-edge — only the exit condition flips: exit (return base) when EQUAL
   (B.cond EQ, cond=0), recurse when not-equal. Pure mirror reusing the
   already-present EQ codegen; no new hand-assembly.

   NOTE: arm64-mirrored but NOT validated on this x86-64 host — see TODO.lst. */
static int try_jit_simple_tail_loop_eq(bytecode_t *bc, uint32_t *out,
                                       int *outn) {
  /* shape walk shared with the amd64 backend via match_simple_tail_loop_eq
     (jit_common.h, which also enforces the arm64 LDR-imm ceiling); emit only.
   */
  struct match_simple_tail_loop m;
  if (!match_simple_tail_loop_eq(bc, &m))
    return 0;
  uint8_t arith_op = m.arith_op;
  int16_t cmp_imm = m.cmp_imm, arith_imm = m.arith_imm;
  int slot_off = m.slot_off;

  int64_t cmp_tagged_64 = ((int64_t)cmp_imm << 3) | 1;
  int cmp_wide = (cmp_tagged_64 < 0 || cmp_tagged_64 > 4095);

  int arith_delta = ((int)arith_imm) << 3;
  if (arith_delta < 0 || arith_delta > 4095)
    return 0;

  /* Equality-exit: loop runs while n != K, returns the slot when n == K.
     arm64 cond EQ = 0. */
  int cond = 0;

  int n = 0;
  out[n++] = arm64_ldr_imm(1, 0, slot_off); /* ldr x1, [x0,#off] */
  int patch_tbz = n;
  out[n++] = 0; /* placeholder tbz x1,#0,deopt */
  if (cmp_wide)
    n += emit_mov64(out + n, 2, (uint64_t)cmp_tagged_64);
  int loop_top = n;
  if (cmp_wide)
    out[n++] = arm64_cmp_reg(1, 2); /* cmp x1, x2 (wide FIX(K)) */
  else
    out[n++] = arm64_cmp_imm(1, (int)cmp_tagged_64); /* cmp x1, #FIX(K) */
  int patch_bcond = n;
  out[n++] = 0; /* placeholder b.eq end */
  if (arith_op == OP_SLOT_SUB_FIX)
    out[n++] = arm64_sub_imm(1, 1, arith_delta);
  else
    out[n++] = arm64_add_imm(1, 1, arith_delta);
  out[n++] = arm64_str_imm(1, 0, slot_off); /* str x1, [x0,#off] */
  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur);
  } /* b loop_top */
  /* end: */
  int end_pc = n;
  out[patch_bcond] = arm64_b_cond(cond, end_pc - patch_bcond);
  out[n++] = arm64_mov_reg(0, 1); /* x0 = x1 (last value, == K) */
  out[n++] = arm64_ret();
  /* deopt: */
  int deopt_pc = n;
  PATCH_DEOPT_TBZ(patch_tbz, 1, 0);
  ARM64_EMIT_DEOPT();

  JIT_GUARD(24);
  *outn = n;
  return 1;
}

/* Float-accumulator self-tail loop — arm64 mirror of the amd64
   try_jit_float_acc_loop (jit_amd64.h, commit 23c0fc4). Two-slot shape
   (25 bytes), produced by:
     (def f (n acc) (if (< n LIM) (f (+ n 1) (<fop> acc FC)) acc))
   where n is an INTEGER counter slot, LIM a fixnum const that doesn't fit
   in i16 (so the compare is the generic LOAD_SLOT/LOAD_CONST/<cmp> triple,
   not the fused SLOT_LT_FIX), and FC is a FLOAT const. fop ∈ {+, -, *}.

   Bytecode (identical to the amd64 matcher):
     [ 0] LOAD_SLOT  cslot           2   (integer counter)
     [ 2] LOAD_CONST lim_idx         2   (consts[lim_idx] must be a fixnum)
     [ 4] <cmp>                      1   (LT/GT/LE/GE)
     [ 5] BR_IF_FALSE off            3
     [ 8] SLOT_<op>_FIX cslot K      4   (counter step; op ∈ {ADD,SUB})
     [12] LOAD_SLOT  aslot           2   (float accumulator)
     [14] LOAD_CONST fc_idx          2   (consts[fc_idx] must be EXP_FLOAT)
     [16] <fop>                      1   (ADD/SUB/MUL)
     [17] TAIL_SELF 2                2
     [19] JUMP off                   3   (unreachable; back-edge)
     [22] LOAD_SLOT  aslot           2
     [24] RET                        1

   Codegen keeps the tagged counter in x1 and the UNBOXED accumulator in d0
   across iterations; the float const sits in d1, the tagged limit in x4. The
   whole self-tail loop folds into one in-buffer back-edge (no per-iteration
   invoke()). env lives in callee-saved x19 across the two exit callouts.

   Two runtime callouts on exit (no calls inside the loop, so d0/d1/x1/x4 need
   no save/restore):
     - 0 iterations  → return refexp(seed) UNCHANGED (matches the VM, which
       returns the raw seed box — an int seed stays an int, a float stays a
       float; we must NOT box a fresh float here).
     - >=1 iteration → the acc is provably a float after the first
       BIN_ARITH(..., FC) (int op float = float), so box the d0 result via
       make_floatf (AAPCS: arg already in d0), matching the VM exactly.
   All tag/type guards run before any slot mutation, and the loop body never
   deopts, so a deopt always re-runs the VM on an untouched env. */
static int try_jit_float_acc_loop(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 25-byte float-accumulator self-tail loop — shape walk shared with the
     amd64 backend via match_float_acc_loop (jit_common.h, which also enforces
     the arm64 LDR-imm slot-offset ceiling); we emit only. */
  struct match_float_acc_loop m;
  if (!match_float_acc_loop(bc, &m))
    return 0;

  uint8_t cmp_op = m.cmp_op, step_op = m.step_op, fop = m.fop;
  int coff = m.coff, aoff = m.aoff;
  uint64_t fc_bits = m.fc_bits;

  /* arm64 materializes the full 64-bit tagged limit into a register (no imm32
     ceiling like amd64), so any 61-bit fixnum limit is fine. */
  int64_t lim_tagged = (m.lim_val << 3) | 1;

  /* Counter step is ±(step_imm<<3) (preserves the tag bit). arm64 ADD/SUB
     immediate is a u12, so only a small positive delta JITs here; anything
     else (negative or >4095) deopts to the VM, which stays correct. The
     real shape's step is +1 → delta 8. */
  int step_delta = ((int)m.step_imm) << 3;
  if (step_delta < 0 || step_delta > 4095)
    return 0;

  int toff = (int)offsetof(exp_t, type); /* 2-byte type field */
  int foff = (int)offsetof(exp_t, f);    /* unboxed double inside the box */

  /* arm64 cond codes: GE=10, LT=11, GT=12, LE=13.
     cont_cc = compare TRUE  (back-edge, keep looping).
     exit_cc = compare FALSE (first test fails → 0 iterations). */
  int cont_cc, exit_cc;
  switch (cmp_op) {
  case OP_LT:
    cont_cc = 11;
    exit_cc = 10;
    break; /* <  : LT  / !< → GE */
  case OP_GT:
    cont_cc = 12;
    exit_cc = 13;
    break; /* >  : GT  / !> → LE */
  case OP_LE:
    cont_cc = 13;
    exit_cc = 12;
    break; /* <= : LE  / !<=→ GT */
  case OP_GE:
    cont_cc = 10;
    exit_cc = 11;
    break; /* >= : GE  / !>=→ LT */
  default:
    return 0;
  }

  int n = 0;

  /* Entry guard: counter must be a tagged fixnum (bit 0 set). Deopt here is
     a leaf (no frame yet) → just NULL + ret. */
  out[n++] = arm64_ldr_imm(1, 0, coff); /* x1 = env->slot[cslot] */
  int patch_deopt0 = n;
  out[n++] = 0; /* tbz x1,#0,deopt0 */

  /* Frame: save fp/lr + x19/x20 (16-aligned 32-byte frame); x19 = env. */
  out[n++] = arm64_stp_pre_sp(29, 30, -32);
  out[n++] = arm64_stp_off_sp(19, 20, 16);
  out[n++] = arm64_mov_from_sp(29);
  out[n++] = arm64_mov_reg(19, 0); /* x19 = env (callee-saved) */

  /* x4 = tagged limit; first compare. If the loop won't run, return the seed
     UNCHANGED via the zero_iter path. */
  n += emit_mov64(out + n, 4, (uint64_t)lim_tagged);
  out[n++] = arm64_cmp_reg(1, 4);
  int patch_zero = n;
  out[n++] = 0; /* b.<exit_cc> zero_iter */

  /* >=1 iteration: load + coerce the accumulator into d0.
     acc may be a fixnum seed (scvtf) or a float box (ldr d0,[box,#foff]).
     Anything else → deopt_framed. */
  out[n++] = arm64_ldr_imm(2, 19, aoff); /* x2 = acc */
  out[n++] = arm64_and_imm7(3, 2);       /* x3 = tag bits */
  out[n++] = arm64_cmp_imm(3, 1);        /* fixnum? */
  int patch_check_float = n;
  out[n++] = 0; /* b.ne check_float */
  /* fixnum seed: arithmetic-untag then signed int->double. */
  out[n++] = arm64_asr_imm(2, 2, 3); /* x2 = value (sign-extended) */
  out[n++] = arm64_scvtf_d_x(0, 2);  /* d0 = (double)x2 */
  int patch_acc_ready = n;
  out[n++] = 0; /* b acc_ready */
  /* check_float: tag must be PTR(0), non-null, type==EXP_FLOAT. */
  int check_float_pc = n;
  int patch_df_tag = n;
  out[n++] = 0; /* cbnz x3,deopt_framed  (tag != 0) */
  int patch_df_null = n;
  out[n++] = 0;                          /* cbz x2,deopt_framed   (null) */
  out[n++] = arm64_ldrh_imm(3, 2, toff); /* w3 = box->type */
  out[n++] = arm64_cmp_imm(3, EXP_FLOAT);
  int patch_df_type = n;
  out[n++] = 0;                           /* b.ne deopt_framed */
  out[n++] = arm64_ldr_d_imm(0, 2, foff); /* d0 = box->f */

  int acc_ready_pc = n;
  out[patch_acc_ready] = arm64_b(acc_ready_pc - patch_acc_ready);

  /* d1 = float const. */
  n += emit_mov64(out + n, 5, fc_bits);
  out[n++] = arm64_fmov_d_x(1, 5);

  /* Loop body: acc = acc <fop> fc; counter += step; while (compare) loop. */
  int loop_top = n;
  if (fop == OP_ADD)
    out[n++] = arm64_fadd_d(0, 0, 1);
  else if (fop == OP_SUB)
    out[n++] = arm64_fsub_d(0, 0, 1);
  else
    out[n++] = arm64_fmul_d(0, 0, 1);
  if (step_op == OP_SLOT_SUB_FIX)
    out[n++] = arm64_sub_imm(1, 1, step_delta);
  else
    out[n++] = arm64_add_imm(1, 1, step_delta);
  out[n++] = arm64_cmp_reg(1, 4);
  {
    int cur = n++;
    out[cur] = arm64_b_cond(cont_cc, loop_top - cur); /* back-edge */
  }

  /* >=1-iter exit: box d0 via make_floatf, tear down frame, return. */
  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)&make_floatf);
  out[n++] = arm64_blr(9);
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_ret();

  /* zero_iter: return refexp(seed) unchanged (x0 = seed). */
  int zero_iter_pc = n;
  out[n++] = arm64_ldr_imm(0, 19, aoff);
  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)&refexp);
  out[n++] = arm64_blr(9);
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_ret();

  /* deopt_framed: a tag/type guard failed AFTER the frame was set up. Can't
     use ARM64_EMIT_DEOPT()/PATCH_DEOPT_* here — those assume a single bare
     movz/ret stub named `deopt_pc`, but this label must first tear the frame
     down, and it's a second deopt target distinct from deopt0. Open-coded. */
  int deopt_framed_pc = n;
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_movz(0, 0, 0); /* x0 = NULL */
  out[n++] = arm64_ret();

  /* deopt0 (the leaf, pre-frame guard) is a bare movz/ret, so it reuses the
     shared ARM64_EMIT_DEOPT()/PATCH_DEOPT_TBZ helpers (deopt_pc by name). */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  /* Back-patch the forward branches with range-checked encoders. */
  PATCH_DEOPT_TBZ(patch_deopt0, 1, 0);
  out[patch_zero] = arm64_b_cond(exit_cc, zero_iter_pc - patch_zero);
  out[patch_check_float] =
      arm64_b_cond(1 /*NE*/, check_float_pc - patch_check_float);
  out[patch_df_tag] = arm64_cbnz(3, deopt_framed_pc - patch_df_tag);
  out[patch_df_null] = arm64_cbz(2, deopt_framed_pc - patch_df_null);
  out[patch_df_type] = arm64_b_cond(1 /*NE*/, deopt_framed_pc - patch_df_type);

  /* Worst case ≈ 55 instructions (entry guard + frame + limit/const
     materialization + acc coerce + loop body + 3 exits). Caller's buffer is
     uint32_t insns[128]. */
  JIT_GUARD(110);
  *outn = n;
  return 1;
}

/* Float telescoping-series self-tail loop (36-byte shape):
     (def f (k acc) (if (<cmp> k LIM)
                        (f (k step= S) (+ acc (- (/ N1 k) (/ N2 (k + OFF2)))))
                        acc))
   arm64 mirror of try_jit_float_series_loop in jit_amd64.h: same skeleton as
   float_acc (tagged counter in x1, unboxed accumulator in d0, integer compare
   against the materialized tagged limit, identical entry guard / acc coerce /
   zero-iter / make_floatf exit / deopt sleds). The body computes
   acc += N1/k - N2/(k+OFF2), guarding each divisor against 0 (a 0 divisor
   deopts so the VM raises the identical "Illegal division by 0"). N1→d1, N2→d6
   are materialized once before the loop. */
static int try_jit_float_series_loop(bytecode_t *bc, uint32_t *out, int *outn) {
  struct match_float_series_loop m;
  if (!match_float_series_loop(bc, &m))
    return 0;

  uint8_t cmp_op = m.cmp_op, step_op = m.step_op;
  int coff = m.coff, aoff = m.aoff;

  int64_t lim_tagged = (m.lim_val << 3) | 1;

  int step_delta = ((int)m.step_imm) << 3;
  if (step_delta < 0 || step_delta > 4095)
    return 0;
  int off2 = (int)m.off2;
  if (off2 < 0 || off2 > 4095) /* ADD imm is a u12 */
    return 0;

  int toff = (int)offsetof(exp_t, type);
  int foff = (int)offsetof(exp_t, f);

  int cont_cc, exit_cc;
  switch (cmp_op) {
  case OP_LT:
    cont_cc = 11;
    exit_cc = 10;
    break;
  case OP_GT:
    cont_cc = 12;
    exit_cc = 13;
    break;
  case OP_LE:
    cont_cc = 13;
    exit_cc = 12;
    break;
  case OP_GE:
    cont_cc = 10;
    exit_cc = 11;
    break;
  default:
    return 0;
  }

  int n = 0;

  /* Entry guard: counter must be a tagged fixnum (leaf deopt, no frame). */
  out[n++] = arm64_ldr_imm(1, 0, coff);
  int patch_deopt0 = n;
  out[n++] = 0; /* tbz x1,#0,deopt0 */

  /* Frame: save fp/lr + x19/x20; x19 = env. */
  out[n++] = arm64_stp_pre_sp(29, 30, -32);
  out[n++] = arm64_stp_off_sp(19, 20, 16);
  out[n++] = arm64_mov_from_sp(29);
  out[n++] = arm64_mov_reg(19, 0);

  /* x4 = tagged limit; first compare → zero_iter if the loop won't run. */
  n += emit_mov64(out + n, 4, (uint64_t)lim_tagged);
  out[n++] = arm64_cmp_reg(1, 4);
  int patch_zero = n;
  out[n++] = 0; /* b.<exit_cc> zero_iter */

  /* >=1 iteration: load + coerce the accumulator into d0 (identical to
     float_acc: fixnum→scvtf, float box→ldr d0,[box,#foff], else deopt). */
  out[n++] = arm64_ldr_imm(2, 19, aoff);
  out[n++] = arm64_and_imm7(3, 2);
  out[n++] = arm64_cmp_imm(3, 1);
  int patch_check_float = n;
  out[n++] = 0; /* b.ne check_float */
  out[n++] = arm64_asr_imm(2, 2, 3);
  out[n++] = arm64_scvtf_d_x(0, 2);
  int patch_acc_ready = n;
  out[n++] = 0; /* b acc_ready */
  int check_float_pc = n;
  int patch_df_tag = n;
  out[n++] = 0; /* cbnz x3,deopt_framed */
  int patch_df_null = n;
  out[n++] = 0;
  out[n++] = arm64_ldrh_imm(3, 2, toff);
  out[n++] = arm64_cmp_imm(3, EXP_FLOAT);
  int patch_df_type = n;
  out[n++] = 0; /* b.ne deopt_framed */
  out[n++] = arm64_ldr_d_imm(0, 2, foff);

  int acc_ready_pc = n;
  out[patch_acc_ready] = arm64_b(acc_ready_pc - patch_acc_ready);

  /* d1 = N1, d6 = N2 (materialized once). */
  n += emit_mov64(out + n, 5, m.n1_bits);
  out[n++] = arm64_fmov_d_x(1, 5);
  n += emit_mov64(out + n, 5, m.n2_bits);
  out[n++] = arm64_fmov_d_x(6, 5);

  /* Loop body: acc += N1/k - N2/(k+OFF2); counter += step; while (cmp) loop. */
  int patch_div1 = -1, patch_div2 = -1;
  int loop_top = n;
  out[n++] = arm64_asr_imm(2, 1, 3); /* x2 = k (untagged) */
  patch_div1 = n;
  out[n++] = 0;                         /* cbz x2,deopt_framed (k != 0) */
  out[n++] = arm64_scvtf_d_x(2, 2);     /* d2 = (double)k (x2 preserved) */
  out[n++] = arm64_fdiv_d(3, 1, 2);     /* d3 = N1/k */
  out[n++] = arm64_add_imm(2, 2, off2); /* x2 = k+OFF2 */
  patch_div2 = n;
  out[n++] = 0;                     /* cbz x2,deopt_framed ((k+OFF2) != 0) */
  out[n++] = arm64_scvtf_d_x(2, 2); /* d2 = (double)(k+OFF2) */
  out[n++] = arm64_fdiv_d(4, 6, 2); /* d4 = N2/(k+OFF2) */
  out[n++] = arm64_fsub_d(3, 3, 4); /* d3 = term */
  out[n++] = arm64_fadd_d(0, 0, 3); /* acc += term */
  if (step_op == OP_SLOT_SUB_FIX)
    out[n++] = arm64_sub_imm(1, 1, step_delta);
  else
    out[n++] = arm64_add_imm(1, 1, step_delta);
  out[n++] = arm64_cmp_reg(1, 4);
  {
    int cur = n++;
    out[cur] = arm64_b_cond(cont_cc, loop_top - cur);
  }

  /* >=1-iter exit: box d0 via make_floatf, tear down frame, return. */
  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)&make_floatf);
  out[n++] = arm64_blr(9);
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_ret();

  /* zero_iter: return refexp(seed) unchanged. */
  int zero_iter_pc = n;
  out[n++] = arm64_ldr_imm(0, 19, aoff);
  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)&refexp);
  out[n++] = arm64_blr(9);
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_ret();

  /* deopt_framed: a tag/type/divisor guard failed after the frame was set up.
   */
  int deopt_framed_pc = n;
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_movz(0, 0, 0);
  out[n++] = arm64_ret();

  /* deopt0 (leaf, pre-frame). */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  PATCH_DEOPT_TBZ(patch_deopt0, 1, 0);
  out[patch_zero] = arm64_b_cond(exit_cc, zero_iter_pc - patch_zero);
  out[patch_check_float] =
      arm64_b_cond(1 /*NE*/, check_float_pc - patch_check_float);
  out[patch_df_tag] = arm64_cbnz(3, deopt_framed_pc - patch_df_tag);
  out[patch_df_null] = arm64_cbz(2, deopt_framed_pc - patch_df_null);
  out[patch_df_type] = arm64_b_cond(1 /*NE*/, deopt_framed_pc - patch_df_type);
  out[patch_div1] = arm64_cbz(2, deopt_framed_pc - patch_div1);
  out[patch_div2] = arm64_cbz(2, deopt_framed_pc - patch_div2);

  JIT_GUARD(120);
  *outn = n;
  return 1;
}

/* General numeric self-tail-loop compiler (arm64). Mirror of the amd64
   try_jit_numloop: same numloop_analyze plan, same semantics; arm64 has far
   more registers, so it JITs kernels (e.g. Mandelbrot, 4 float locals) the
   amd64 backend declines. INT counter in x1 (untagged); float locals in d-regs
   (d0-d7 then d16-d31 — caller-saved, no save/restore; no mid-loop call); float
   const/addr scratch in x9; entry guards (INT=fixnum, FLOAT=strict EXP_FLOAT);
   fused compare+branch (GT/GE exclude NaN, matching VM NaN→false); div-by-0
   deopt via fcmp #0.0 + b.eq; conflict-free tail-self shuffle; frame (fp/lr)
   only when a FLOAT is returned (make_floatf call). */
static int try_jit_numloop(bytecode_t *bc, uint32_t *out, int *outn) {
  numloop_t nl;
  if (!numloop_analyze(bc, &nl))
    return 0;
  if (nl.nislots > 1)
    return 0; /* float-box guard uses x9/x10; one int home (x1) keeps it simple
               */
  /* arm64 budget: float homes+temps over d0-d7 + d16-d31 = 24 caller-saved. */
  if (nl.nfslots + nl.max_ftmp > 24 || nl.nislots + nl.max_itmp > 4)
    return 0;
  uint8_t *c = bc->code;
  int ncode = bc->ncode, np = nl.nparams;
  const int ipool[4] = {1, 2, 3, 4}; /* x1 = counter home; x2-x4 int temps */
  int toff = (int)offsetof(exp_t, type), foff = (int)offsetof(exp_t, f);
/* float vreg → d-reg (skip the callee-saved d8-d15) */
#define DFR(v) ((v) < 8 ? (v) : (v) + 8)

  /* Emit into a generous local buffer; copy out only if it fits insns[128]. */
  uint32_t local[2048];
  uint32_t *out_buf = out;
  out = local;

  int framed = nl.float_result;
  int n = 0;
  int dj0[64], ndj0 = 0;  /* pre-frame deopt (b.cond/cbz/tbz placeholders) */
  int djf[300], ndjf = 0; /* framed deopt (div-by-0) */
#define DJ0(t_at) (dj0[ndj0++] = (t_at))
#define DJF(t_at) (djf[ndjf++] = (t_at))

  /* ---- entry: load + guard each slot home (env in x0) ---- */
  for (int s = 0; s < np; s++) {
    int off = env_slot_off((uint8_t)s);
    if (nl.slot_class[s] == NLC_INT) {
      int hr = ipool[nl.iidx[s]];
      out[n++] = arm64_ldr_imm(hr, 0, off);
      dj0[ndj0++] = n;
      out[n++] = 0;                        /* tbz hr,#0,deopt0 (not fixnum) */
      out[n++] = arm64_asr_imm(hr, hr, 3); /* untag */
    } else {
      int hd = DFR(nl.fidx[s]);
      out[n++] = arm64_ldr_imm(9, 0, off); /* x9 = slot ptr */
      out[n++] = arm64_and_imm7(10, 9);    /* x10 = tag bits */
      dj0[ndj0++] = n;
      out[n++] = 0; /* cbnz x10,deopt0 (not a pointer) */
      dj0[ndj0++] = n;
      out[n++] = 0; /* cbz x9,deopt0 (null) */
      out[n++] = arm64_ldrh_imm(10, 9, toff);
      out[n++] = arm64_cmp_imm(10, EXP_FLOAT);
      dj0[ndj0++] = n;
      out[n++] = 0;                            /* b.ne deopt0 */
      out[n++] = arm64_ldr_d_imm(hd, 9, foff); /* home = box->f */
    }
  }
  if (framed)
    out[n++] = arm64_stp_pre_sp(29, 30, -16); /* save fp/lr for make_floatf */

  /* ---- loop body ---- */
  int noff[NL_MAXPC];
  for (int i = 0; i < ncode; i++)
    noff[i] = -1;
  struct {
    int at, target_pc, cond;
  } patch[300];
  int npatch = 0;
  int loop_top = n;

  int pf = 0, pf_float = 0, pf_op = 0, pf_ra = 0, pf_rb = 0, pf_slotimm = 0,
      pf_imm = 0;

  for (int pc = 0; pc < ncode;) {
    int len = bc_oplen(c[pc]);
    if (nl.depth[pc] < 0) {
      pc += len ? len : 1;
      continue;
    }
    noff[pc] = n;
    uint8_t op = c[pc];
    int d = nl.depth[pc];
    const uint8_t *st = nl.scls[pc];

    switch (op) {
    case OP_LOAD_SLOT: {
      uint8_t s = c[pc + 1];
      if (nl.slot_class[s] == NLC_FLOAT)
        out[n++] =
            arm64_fmov_d_d(DFR(nl_freg(st, d, nl.nfslots)), DFR(nl.fidx[s]));
      else
        out[n++] =
            arm64_mov_reg(ipool[nl_ireg(st, d, nl.nislots)], ipool[nl.iidx[s]]);
      break;
    }
    case OP_LOAD_CONST: {
      exp_t *k = bc->consts[c[pc + 1]];
      if (isfloat(k)) {
        uint64_t bits;
        double dv = k->f;
        memcpy(&bits, &dv, 8);
        n += emit_mov64(out + n, 9, bits);
        out[n++] = arm64_fmov_d_x(DFR(nl_freg(st, d, nl.nfslots)), 9);
      } else {
        n += emit_mov64(out + n, ipool[nl_ireg(st, d, nl.nislots)],
                        (uint64_t)FIX_VAL(k));
      }
      break;
    }
    case OP_LOAD_FIX: {
      int16_t v = (int16_t)(c[pc + 1] | (c[pc + 2] << 8));
      n += emit_mov64(out + n, ipool[nl_ireg(st, d, nl.nislots)],
                      (uint64_t)(int64_t)v);
      break;
    }
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV: {
      int ra = DFR(nl_freg(st, d - 2, nl.nfslots)),
          rb = DFR(nl_freg(st, d - 1, nl.nfslots));
      if (op == OP_DIV) {
        out[n++] =
            arm64_fcmp_d_zero(rb); /* divisor ±0 → deopt (NaN→fdiv=NaN) */
        djf[ndjf++] = n;
        out[n++] = 0; /* b.eq deopt_framed */
      }
      out[n++] = op == OP_ADD   ? arm64_fadd_d(ra, ra, rb)
                 : op == OP_SUB ? arm64_fsub_d(ra, ra, rb)
                 : op == OP_MUL ? arm64_fmul_d(ra, ra, rb)
                                : arm64_fdiv_d(ra, ra, rb);
      break;
    }
    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE: {
      uint8_t a = st[d - 2], b = st[d - 1];
      pf = 1;
      pf_op = op;
      pf_slotimm = 0;
      pf_float = (a == NLC_FLOAT || b == NLC_FLOAT);
      if (pf_float) {
        pf_ra = DFR(nl_freg(st, d - 2, nl.nfslots));
        pf_rb = DFR(nl_freg(st, d - 1, nl.nfslots));
      } else {
        pf_ra = ipool[nl_ireg(st, d - 2, nl.nislots)];
        pf_rb = ipool[nl_ireg(st, d - 1, nl.nislots)];
      }
      break;
    }
    case OP_SLOT_LT_FIX:
    case OP_SLOT_LE_FIX:
    case OP_SLOT_GT_FIX:
    case OP_SLOT_GE_FIX: {
      uint8_t s = c[pc + 1];
      if (nl.slot_class[s] != NLC_INT)
        return 0;
      pf = 1;
      pf_slotimm = 1;
      pf_float = 0;
      pf_op = (op == OP_SLOT_LT_FIX)   ? OP_LT
              : (op == OP_SLOT_LE_FIX) ? OP_LE
              : (op == OP_SLOT_GT_FIX) ? OP_GT
                                       : OP_GE;
      pf_ra = ipool[nl.iidx[s]];
      pf_imm = (int16_t)(c[pc + 2] | (c[pc + 3] << 8));
      break;
    }
    case OP_BR_IF_FALSE:
    case OP_BR_IF_TRUE: {
      if (!pf)
        return 0;
      int16_t off = (int16_t)(c[pc + 1] | (c[pc + 2] << 8));
      int tgt = pc + len + off, br_true = (op == OP_BR_IF_TRUE), cond;
      if (pf_float) {
        int sw = (pf_op == OP_LT || pf_op == OP_LE);
        int aa = sw ? pf_rb : pf_ra, bb = sw ? pf_ra : pf_rb;
        out[n++] = arm64_fcmp_d(aa, bb);
        if (pf_op == OP_LT || pf_op == OP_GT)
          cond = br_true ? 12 /*GT*/ : 13 /*LE*/;
        else
          cond = br_true ? 10 /*GE*/ : 11 /*LT*/;
      } else {
        if (pf_slotimm)
          out[n++] = arm64_cmp_imm(pf_ra, pf_imm);
        else
          out[n++] = arm64_cmp_reg(pf_ra, pf_rb);
        switch (pf_op) {
        case OP_LT:
          cond = br_true ? 11 : 10;
          break;
        case OP_GT:
          cond = br_true ? 12 : 13;
          break;
        case OP_LE:
          cond = br_true ? 13 : 12;
          break;
        default:
          cond = br_true ? 10 : 11;
          break;
        }
      }
      pf = 0;
      patch[npatch].at = n;
      patch[npatch].target_pc = tgt;
      patch[npatch].cond = cond;
      npatch++;
      out[n++] = 0; /* b.cond tgt */
      break;
    }
    case OP_JUMP: {
      int16_t off = (int16_t)(c[pc + 1] | (c[pc + 2] << 8));
      patch[npatch].at = n;
      patch[npatch].target_pc = pc + len + off;
      patch[npatch].cond = -1; /* unconditional */
      npatch++;
      out[n++] = 0;
      break;
    }
    case OP_SLOT_ADD_FIX:
    case OP_SLOT_SUB_FIX: {
      uint8_t s = c[pc + 1];
      int16_t imm = (int16_t)(c[pc + 2] | (c[pc + 3] << 8));
      if (nl.slot_class[s] != NLC_INT)
        return 0;
      if (imm < 0 || imm > 4095)
        return 0; /* add/sub imm is u12 */
      int dst = ipool[nl_ireg(st, d, nl.nislots)], src = ipool[nl.iidx[s]];
      out[n++] = arm64_mov_reg(dst, src);
      out[n++] = op == OP_SLOT_ADD_FIX ? arm64_add_imm(dst, dst, imm)
                                       : arm64_sub_imm(dst, dst, imm);
      break;
    }
    case OP_TAIL_SELF: {
      for (int i = 0; i < np; i++) {
        if (nl.slot_class[i] == NLC_FLOAT) {
          int src = DFR(nl_freg(st, i, nl.nfslots)), dst = DFR(nl.fidx[i]);
          if (src != dst)
            out[n++] = arm64_fmov_d_d(dst, src);
        } else {
          int src = ipool[nl_ireg(st, i, nl.nislots)], dst = ipool[nl.iidx[i]];
          if (src != dst)
            out[n++] = arm64_mov_reg(dst, src);
        }
      }
      patch[npatch].at = n;
      patch[npatch].target_pc = -1; /* loop_top */
      patch[npatch].cond = -1;
      npatch++;
      out[n++] = 0;
      break;
    }
    case OP_RET: {
      uint8_t r = st[d - 1];
      if (r == NLC_FLOAT) {
        int rd = DFR(nl_freg(st, d - 1, nl.nfslots));
        if (rd != 0)
          out[n++] = arm64_fmov_d_d(0, rd);
        n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)&make_floatf);
        out[n++] = arm64_blr(9);
      } else {
        int rr = ipool[nl_ireg(st, d - 1, nl.nislots)];
        out[n++] = arm64_lsl_imm(0, rr, 3);  /* v << 3 */
        out[n++] = arm64_orr_imm_bit0(0, 0); /* | 1 */
      }
      if (framed)
        out[n++] = arm64_ldp_post_sp(29, 30, 16);
      out[n++] = arm64_ret();
      break;
    }
    default:
      return 0;
    }
    pc += len;
  }

  int deoptf_pc = n;
  if (framed)
    out[n++] = arm64_ldp_post_sp(29, 30, 16);
  out[n++] = arm64_movz(0, 0, 0); /* x0 = NULL */
  out[n++] = arm64_ret();
  int deopt0_pc = n;
  out[n++] = arm64_movz(0, 0, 0);
  out[n++] = arm64_ret();

  /* patch entry guards: order in dj0 follows emission (tbz / cbnz / cbz / b.ne)
   */
  /* Re-emit the entry guard branches now that deopt0_pc is known. We stored the
     instruction slots; reconstruct each by its kind from the opcode position is
     awkward, so we patch by the known guard pattern: every dj0 slot is a
     forward branch to deopt0. Distinguish by re-deriving from the saved slot's
     intended kind isn't tracked — instead we encoded placeholders; fill them
     per-kind: */
  /* (handled inline below) */
  for (int i = 0; i < ndjf; i++)
    out[djf[i]] = arm64_b_cond(0 /*EQ*/, deoptf_pc - djf[i]);
  for (int i = 0; i < npatch; i++) {
    int tgt = patch[i].target_pc < 0 ? loop_top : noff[patch[i].target_pc];
    if (tgt < 0)
      return 0;
    out[patch[i].at] = patch[i].cond < 0
                           ? arm64_b(tgt - patch[i].at)
                           : arm64_b_cond(patch[i].cond, tgt - patch[i].at);
  }
  /* entry-guard placeholders: fill by kind. We know the sequence per slot, so
     re-walk and patch. */
  {
    int gi = 0;
    for (int s = 0; s < np && gi < ndj0; s++) {
      if (nl.slot_class[s] == NLC_INT) {
        out[dj0[gi]] = arm64_tbz(ipool[nl.iidx[s]], 0, deopt0_pc - dj0[gi]);
        gi++;
      } else {
        out[dj0[gi]] = arm64_cbnz(10, deopt0_pc - dj0[gi]);
        gi++;
        out[dj0[gi]] = arm64_cbz(9, deopt0_pc - dj0[gi]);
        gi++;
        out[dj0[gi]] = arm64_b_cond(1 /*NE*/, deopt0_pc - dj0[gi]);
        gi++;
      }
    }
  }
#undef DFR
#undef DJ0
#undef DJF
  if (n > 128) /* must fit the caller's insns[128] */
    return 0;
  for (int i = 0; i < n; i++)
    out_buf[i] = local[i];
  *outn = n;
  return 1;
}

/* Wide-limit integer counter loop — the generic-compare twin of
   try_jit_simple_tail_loop. When the loop limit is a fixnum that doesn't fit
   i16 the compiler emits LOAD_SLOT/LOAD_CONST/<cmp> (5 bytes) instead of the
   fused SLOT_<cmp>_FIX (4 bytes), so simple_tail_loop's matcher misses it and
   e.g. (if (< n 5000000) (f (+ n 1)) n) drops to the VM. Identical emitted
   loop; only the limit comes from a const (a wide fixnum, always materialized
   into a reg — same path simple_tail_loop already uses for >u12 limits).
   20-byte shape:
     LOAD_SLOT slot ; LOAD_CONST lim ; <cmp LT|GT|LE|GE> ; BR_IF_FALSE ;
     SLOT_<ADD|SUB>_FIX slot K ; TAIL_SELF 1 ; JUMP ; LOAD_SLOT slot ; RET */
static int try_jit_wide_counter_loop(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 20-byte wide-limit counter loop — shape walk shared with the amd64 backend
     via match_wide_counter_loop (jit_common.h, which also enforces the arm64
     LDR-imm slot-offset ceiling); we emit only. */
  struct match_wide_counter_loop m;
  if (!match_wide_counter_loop(bc, &m))
    return 0;

  uint8_t cmp_op = m.cmp_op, arith_op = m.arith_op;
  int slot_off = m.slot_off;
  int64_t lim_tagged = (m.lim_val << 3) | 1;

  int arith_delta = ((int)m.arith_imm) << 3;
  if (arith_delta < 0 || arith_delta > 4095)
    return 0;

  /* exit (compare FALSE) cond — invert. GE=10 LT=11 GT=12 LE=13. */
  int cond;
  switch (cmp_op) {
  case OP_LT:
    cond = 10;
    break; /* !< → GE */
  case OP_GT:
    cond = 13;
    break; /* !> → LE */
  case OP_LE:
    cond = 12;
    break; /* !<=→ GT */
  case OP_GE:
    cond = 11;
    break; /* !>=→ LT */
  default:
    return 0;
  }

  int n = 0;
  out[n++] = arm64_ldr_imm(1, 0, slot_off); /* x1 = counter */
  int patch_tbz = n;
  out[n++] = 0;                                      /* tbz x1,#0,deopt */
  n += emit_mov64(out + n, 2, (uint64_t)lim_tagged); /* x2 = tagged limit */
  int loop_top = n;
  out[n++] = arm64_cmp_reg(1, 2);
  int patch_bcond = n;
  out[n++] = 0; /* b.<exit_cc> end */
  if (arith_op == OP_SLOT_SUB_FIX)
    out[n++] = arm64_sub_imm(1, 1, arith_delta);
  else
    out[n++] = arm64_add_imm(1, 1, arith_delta);
  out[n++] = arm64_str_imm(1, 0, slot_off);
  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur);
  }
  int end_pc = n;
  out[patch_bcond] = arm64_b_cond(cond, end_pc - patch_bcond);
  out[n++] = arm64_mov_reg(0, 1); /* x0 = final counter */
  out[n++] = arm64_ret();
  int deopt_pc = n;
  PATCH_DEOPT_TBZ(patch_tbz, 1, 0);
  ARM64_EMIT_DEOPT();

  JIT_GUARD(24);
  *outn = n;
  return 1;
}

/* 28-byte two-call recursion shape — fib pattern.
     (def f (n) (if (cmp n K1) n (+ (f (n op K2)) (f (n op K3)))))
   Only the iterative-fib fast path is implemented on arm64 today: when
   both recursive calls go to the same callee, both arms are SUB, and
   {K2,K3}={1,2}, the exponential call tree collapses to a 2-term linear
   iteration (Fibonacci recurrence). General two-call recursion (different
   K2/K3, ADD instead of SUB, or different callees) falls through to the
   bytecode interpreter — it would need a value-returning two-call emitter
   that saves the first result across the second callout. */
static int try_jit_recurse_add_two(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 28-byte two-call recursion (fib shape) — shape walk shared with the amd64
     backend via match_recurse_add_two (jit_common.h, which also enforces the
     arm64 LDR-imm slot-offset ceiling); we emit only. The shared walk accepts
     OP_SLOT_IS_FIX in the cmp slot, but arm64 only handles the ORDERED base
     predicate (its exit_cc switch below rejects IS), so guard it explicitly to
     preserve the original behavior. */
  struct match_recurse_add_two mm;
  if (!match_recurse_add_two(bc, &mm))
    return 0;
  if (mm.cmp_op == OP_SLOT_IS_FIX)
    return 0;

  uint8_t cmp_op = mm.cmp_op, op_a = mm.op_a, op_b = mm.op_b;
  uint8_t idx_a = mm.idx_a, idx_b = mm.idx_b;
  int16_t K1 = mm.K1, K2 = mm.K2, K3 = mm.K3;
  int slot_off = mm.slot_off;

  /* Iterative fast path conditions: both calls go to THIS function
     (self-recursion, not just same-name-each-other), both SUB, K2/K3
     are {1,2} in either order. The base case must return n itself
     (LOAD_SLOT slot then RET — c[7]/c[8] enforce this).

     The self-name check is critical: without it any user lambda whose
     body shape matches gets silently rewritten as iterative-fib-of-its-
     own-arg, ignoring whatever callee the user actually wrote. */
  if (!(bc_calls_self(bc, idx_a) && bc_calls_self(bc, idx_b)))
    return 0;
  int is_fib_like = op_a == OP_SLOT_SUB_FIX && op_b == OP_SLOT_SUB_FIX &&
                    ((K2 == 1 && K3 == 2) || (K2 == 2 && K3 == 1));
  if (!is_fib_like)
    return 0; /* general 2-call recursion: fall back */

  /* Initial untagged seeds: a = K1-2, b = K1-1. Since base case returns
     n itself, f(x) = x for x < K1. Iteration computes f(n) for n >= K1
     by stepping i from K1 up to n, swapping (a,b) and adding. */
  int64_t init_a = (int64_t)K1 - 2;
  int64_t init_b = (int64_t)K1 - 1;

  /* exit cc for cmp_op TRUE (base case taken).
     ARM64 cond codes: GE=10, LT=11, GT=12, LE=13. */
  int exit_cc;
  switch (cmp_op) {
  case OP_SLOT_LT_FIX:
    exit_cc = 11;
    break; /* base when n <  K1 */
  case OP_SLOT_GT_FIX:
    exit_cc = 12;
    break; /* base when n >  K1 */
  case OP_SLOT_LE_FIX:
    exit_cc = 13;
    break; /* base when n <= K1 */
  case OP_SLOT_GE_FIX:
    exit_cc = 10;
    break; /* base when n >= K1 */
  default:
    return 0;
  }

  int n = 0;
  /* Load + tag-check + untag n into x1. */
  out[n++] = arm64_ldr_imm(1, 0, slot_off); /* x1 = env->inline_vals[slot] */
  int patch_tbz = n;
  out[n++] = 0;                      /* tbz x1,#0,deopt */
  out[n++] = arm64_asr_imm(1, 1, 3); /* x1 >>= 3 (sign-ext untag) */

  /* Compare untagged n vs K1; branch to base-case re-tag-and-return. */
  /* K1 fits a 12-bit cmp imm for the typical fib(<= 2000) range; if it
     overflows, fall back to bytecode rather than emit MOVZ/CMP_REG. */
  if ((int)K1 < 0 || (int)K1 > 4095)
    return 0;
  out[n++] = arm64_cmp_imm(1, (int)K1);
  int patch_base = n;
  out[n++] = 0; /* b.cond <exit_cc> base_pc */

  /* Iterative fib: x2 = a, x3 = b, x4 = i, x5 = scratch (for swap).
     Loop: cmp i, n; b.gt done; (a,b) = (b, a+b); i++; b loop. */
  n += emit_mov64(out + n, 2, (uint64_t)init_a);
  n += emit_mov64(out + n, 3, (uint64_t)init_b);
  n += emit_mov64(out + n, 4, (uint64_t)(int64_t)K1);

  int loop_top = n;
  out[n++] = arm64_cmp_reg(4, 1); /* cmp x4, x1  (i vs n) */
  int patch_done = n;
  out[n++] = 0;                      /* b.gt done */
  out[n++] = arm64_mov_reg(5, 2);    /* x5 = a (saved) */
  out[n++] = arm64_mov_reg(2, 3);    /* a = b */
  out[n++] = arm64_add_reg(3, 5, 3); /* b = old_a + b */
  out[n++] = arm64_add_imm(4, 4, 1); /* i++ */
  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur); /* b loop_top */
  }

  /* done: x0 = (b << 3) | 1 (re-tag), ret. Checked: a fib value beyond the
     61-bit range overflows the shift and deopts to the VM, which raises. */
  int done_pc = n;
  int ovf_done;
  ARM64_EMIT_RETAG_RET_CK(3, ovf_done);

  /* base: re-tag x1 (untagged n) into x0, ret. n < K1 (small) → no overflow. */
  int base_pc = n;
  ARM64_EMIT_RETAG_RET(1);

  /* deopt: return NULL. */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  /* Patch forward branches now that targets are known.
     b.gt = cond 12 (GT). Always emit GT regardless of cmp_op — the loop
     test is a fixed "i > n" comparison, independent of the recursion's
     base predicate. */
  out[patch_done] = arm64_b_cond(12, done_pc - patch_done);
  out[patch_base] = arm64_b_cond(exit_cc, base_pc - patch_base);
  PATCH_DEOPT_TBZ(patch_tbz, 1, 0);
  PATCH_DEOPT_BNE(ovf_done);

  JIT_GUARD(32);
  *outn = n;
  return 1;
}

/* 24-byte one-call recursion shape — fact pattern.
     (def f (n) (if (cmp n K1) BASE (* n (f (n op K2)))))
   Iteratively: acc = BASE; while !cmp(n, K1) { acc *= n; n = n op K2 }
   ~3 cycles per iteration vs ~60 in the bytecode dispatch. */
static int try_jit_recurse_mul_one(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 24-byte iterative-fact shape — shape walk shared with the amd64 backend
     via match_recurse_mul_one (jit_common.h, which also enforces the arm64
     LDR-imm slot-offset ceiling); we emit only. */
  struct match_recurse_mul_one mm;
  if (!match_recurse_mul_one(bc, &mm))
    return 0;

  uint8_t cmp_op = mm.cmp_op, step_op = mm.step_op;
  int16_t K1 = mm.K1, K2 = mm.K2, BASE = mm.BASE;
  int slot_off = mm.slot_off;

  /* Self-name guard (see recurse_add_two): the iterative-fact emission
     is only correct if the recursive call goes back to THIS function. */
  if (!bc_calls_self(bc, mm.idx))
    return 0;

  if ((int)K1 < 0 || (int)K1 > 4095)
    return 0;
  int k2_abs = (int)K2;
  if (k2_abs < 0)
    k2_abs = -k2_abs;
  if (k2_abs > 4095)
    return 0;

  /* exit cc: BASE returned when cmp_op holds. */
  int exit_cc;
  switch (cmp_op) {
  case OP_SLOT_LT_FIX:
    exit_cc = 11;
    break;
  case OP_SLOT_GT_FIX:
    exit_cc = 12;
    break;
  case OP_SLOT_LE_FIX:
    exit_cc = 13;
    break;
  case OP_SLOT_GE_FIX:
    exit_cc = 10;
    break;
  default:
    return 0;
  }

  int n = 0;
  /* x1 = untagged n; x2 = acc; x3 = scratch (K2 if needed). */
  out[n++] = arm64_ldr_imm(1, 0, slot_off);
  int patch_tbz = n;
  out[n++] = 0;                      /* tbz x1,#0,deopt */
  out[n++] = arm64_asr_imm(1, 1, 3); /* untag */
  n += emit_mov64(out + n, 2, (uint64_t)(int64_t)BASE);

  int loop_top = n;
  out[n++] = arm64_cmp_imm(1, (int)K1);
  int patch_done = n;
  out[n++] = 0; /* b.<exit_cc> done */
  /* acc *= n (untagged). Detect int64 overflow of the product via SMULH (high
     bits must be the sign-extension of the low bits); the 2^60..2^63 band is
     caught at the checked re-tag below. */
  out[n++] = arm64_smulh(9, 2, 1);        /* x9 = high(acc*n) */
  out[n++] = arm64_mul(2, 2, 1);          /* x2 = low(acc*n)  */
  out[n++] = arm64_cmp_reg_asr(9, 2, 63); /* high == sext(low)? */
  int ovf_mul = n++;                      /* reserve B.NE deopt */
  if (step_op == OP_SLOT_SUB_FIX)
    out[n++] = arm64_sub_imm(1, 1, k2_abs);
  else
    out[n++] = arm64_add_imm(1, 1, k2_abs);
  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur);
  }

  int done_pc = n;
  int ovf_tag;
  ARM64_EMIT_RETAG_RET_CK(2, ovf_tag);

  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_done] = arm64_b_cond(exit_cc, done_pc - patch_done);
  PATCH_DEOPT_TBZ(patch_tbz, 1, 0);
  PATCH_DEOPT_BNE(ovf_mul);
  PATCH_DEOPT_BNE(ovf_tag);

  JIT_GUARD(32);
  *outn = n;
  return 1;
}

/* count-primes from sieve-fast — 41-byte exact-match shape.
     (def count-primes (i n marks acc)
       (if (> i n) acc
           (count-primes (+ i 1) n marks
                         (if (vec-ref marks i) (+ acc 1) acc))))
   Tail loop. Reads marks[i] (singleton t or nil), conditionally
   increments acc, increments i, tail-self. */
static int try_jit_count_primes(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 41-byte sieve count-primes outer loop — shape walk shared with the amd64
     backend via match_count_primes (jit_common.h, which also enforces the
     arm64 LDR-imm slot-offset ceiling); we emit only. */
  struct match_count_primes m;
  if (!match_count_primes(bc, &m))
    return 0;

  int off_i = m.off_i, off_n = m.off_n, off_acc = m.off_acc,
      off_marks = m.off_marks;
  int off_ptr = (int)offsetof(struct exp_t, ptr);
  if (off_ptr > 32760)
    return 0;

  int n = 0;
  int entry_pc = n;

  /* x9 = nil_singleton, kept across iterations. */
  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)nil_singleton);

  out[n++] = arm64_ldr_imm(1, 0, off_i); /* x1 = i */
  out[n++] = arm64_ldr_imm(2, 0, off_n); /* x2 = n */
  int patch_da = n;
  out[n++] = 0;
  int patch_db = n;
  out[n++] = 0;

  /* if (i > n) → done, return acc */
  out[n++] = arm64_cmp_reg(1, 2);
  int patch_done = n;
  out[n++] = 0; /* b.gt done */

  /* x3 = marks->ptr (alc_vec_t*).  Kind check: typed (I64/F64) vecs
     store raw scalars, so the GEN cell read below would dereference
     garbage as a pointer. Bail to bytecode VM if any kind bit is set. */
  out[n++] = arm64_ldr_imm(3, 0, off_marks);
  int off_flags_cp = (int)offsetof(struct exp_t, flags);
  out[n++] = arm64_ldrb_imm(7, 3, off_flags_cp);
  int patch_kind_cp_a = n;
  out[n++] = 0; /* tbnz w7,#4,deopt */
  int patch_kind_cp_b = n;
  out[n++] = 0; /* tbnz w7,#5,deopt */
  out[n++] = arm64_ldr_imm(3, 3, off_ptr);

  /* x4 = marks_ptr + i_tagged + 7;  x5 = *(x4) */
  out[n++] = arm64_add_reg(4, 3, 1);
  out[n++] = arm64_add_imm(4, 4, 7);
  out[n++] = arm64_ldr_imm(5, 4, 0);

  /* truthy = (x5 != 0) && (x5 != nil_singleton). If truthy: acc += 8. */
  int patch_skip_a = n;
  out[n++] = 0; /* cbz x5, skip */
  out[n++] = arm64_cmp_reg(5, 9);
  int patch_skip_b = n;
  out[n++] = 0; /* b.eq skip */
  /* tagged inc: load acc, add 8, store */
  out[n++] = arm64_ldr_imm(6, 0, off_acc);
  out[n++] = arm64_add_imm(6, 6, 8);
  out[n++] = arm64_str_imm(6, 0, off_acc);
  int skip_pc = n;

  /* i += 1 (tagged: add 8) */
  out[n++] = arm64_add_imm(1, 1, 8);
  out[n++] = arm64_str_imm(1, 0, off_i);

  /* tail-self: b entry */
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  /* done: x0 = acc */
  int done_pc = n;
  out[n++] = arm64_ldr_imm(0, 0, off_acc);
  out[n++] = arm64_ret();

  /* deopt */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_done] = arm64_b_cond(12 /* GT */, done_pc - patch_done);
  PATCH_DEOPT_TBZ(patch_da, 1, 0);
  PATCH_DEOPT_TBZ(patch_db, 2, 0);
  PATCH_DEOPT_TBNZ(patch_kind_cp_a, 7, 4);
  PATCH_DEOPT_TBNZ(patch_kind_cp_b, 7, 5);
  out[patch_skip_a] = arm64_cbz(5, skip_pc - patch_skip_a);
  out[patch_skip_b] = arm64_b_cond(0, skip_pc - patch_skip_b);

  JIT_GUARD(64);
  *outn = n;
  return 1;
}

/* is-prime-given from sieve.alc — 37-byte exact-match shape.
     (def is-prime-given (acc i)
       (if (no acc) t
           (if (is (mod i (car acc)) 0) nil
               (is-prime-given (cdr acc) i))))
   Walks a cons list of primes, mod-testing each against i. Inline
   refexp/unrefexp on the cdr walk; deopts to bytecode if a count hits 0. */
static int try_jit_is_prime_given(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 37-byte sieve is-prime-given list walk — shape walk shared with the amd64
     backend via match_is_prime_given (jit_common.h, which also enforces the
     arm64 LDR-imm slot-offset ceiling); we emit only. The struct-field offset
     ceilings below stay arm64-specific. */
  struct match_is_prime_given m;
  if (!match_is_prime_given(bc, &m))
    return 0;

  int off_acc = m.off_acc, off_i = m.off_i;
  int off_cont = (int)offsetof(struct exp_t, content);
  int off_next = (int)offsetof(struct exp_t, next);
  int off_nref = (int)offsetof(struct exp_t, nref);
  if (off_acc > 32760 || off_i > 32760 || off_cont > 32760 ||
      off_next > 32760 || off_nref > 16380)
    return 0;
#if !ALCOVE_SINGLE_THREADED
  int off_flags = (int)offsetof(struct exp_t, flags);
  if (off_flags > 4095)
    return 0; /* LDRB unsigned-offset limit */
  int patch_shared_ref = -1, patch_shared_unref = -1;
#endif

  int n = 0;
  int entry_pc = n;

  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)nil_singleton);
  n += emit_mov64(out + n, 10, (uint64_t)(uintptr_t)true_singleton);

  out[n++] = arm64_ldr_imm(1, 0, off_acc);
  int patch_t1 = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(1, 9);
  int patch_t2 = n;
  out[n++] = 0;

  out[n++] = arm64_ldr_imm(2, 1, off_cont);
  int patch_da = n;
  out[n++] = 0;

  out[n++] = arm64_ldr_imm(3, 0, off_i);
  int patch_db = n;
  out[n++] = 0;

  out[n++] = arm64_sub_imm(2, 2, 1);
  out[n++] = arm64_sub_imm(3, 3, 1);
  int patch_dc = n;
  out[n++] = 0;

  out[n++] = arm64_sdiv(4, 3, 2);
  out[n++] = arm64_msub(5, 4, 2, 3);
  int patch_n1 = n;
  out[n++] = 0;

  out[n++] = arm64_ldr_imm(4, 1, off_next);

  int patch_skip_ref_a = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(4, 9);
  int patch_skip_ref_b = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(4, 10);
  int patch_skip_ref_c = n;
  out[n++] = 0;
#if !ALCOVE_SINGLE_THREADED
  /* Deopt to bytecode if the cdr target is FLAG_SHARED — the bytecode
     interp uses atomic refcount macros, the JIT inlines plain ldr/str. */
  out[n++] = arm64_ldrb_imm(7, 4, off_flags);
  patch_shared_ref = n;
  out[n++] = 0; /* tbnz w7, #3, deopt */
#endif
  out[n++] = arm64_ldr_w_imm(6, 4, off_nref);
  out[n++] = arm64_add_w_imm(6, 6, 1);
  out[n++] = arm64_str_w_imm(6, 4, off_nref);
  int skip_ref_pc = n;

  int patch_skip_unref_a = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(1, 9);
  int patch_skip_unref_b = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(1, 10);
  int patch_skip_unref_c = n;
  out[n++] = 0;
#if !ALCOVE_SINGLE_THREADED
  out[n++] = arm64_ldrb_imm(7, 1, off_flags);
  patch_shared_unref = n;
  out[n++] = 0; /* tbnz w7, #3, deopt */
#endif
  out[n++] = arm64_ldr_w_imm(6, 1, off_nref);
  out[n++] = arm64_sub_w_imm(6, 6, 1);
  out[n++] = arm64_str_w_imm(6, 1, off_nref);
  int patch_to_deopt = n;
  out[n++] = 0;
  int skip_unref_pc = n;

  out[n++] = arm64_str_imm(4, 0, off_acc);

  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  int ret_t_pc = n;
  out[n++] = arm64_mov_reg(0, 10);
  out[n++] = arm64_ret();

  int ret_nil_pc = n;
  out[n++] = arm64_mov_reg(0, 9);
  out[n++] = arm64_ret();

  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_t1] = arm64_cbz(1, ret_t_pc - patch_t1);
  out[patch_t2] = arm64_b_cond(0, ret_t_pc - patch_t2);
  PATCH_DEOPT_TBZ(patch_da, 2, 0);
  PATCH_DEOPT_TBZ(patch_db, 3, 0);
  PATCH_DEOPT_CBZ(patch_dc, 2);
  out[patch_n1] = arm64_cbz(5, ret_nil_pc - patch_n1);

  out[patch_skip_ref_a] = arm64_cbz(4, skip_ref_pc - patch_skip_ref_a);
  out[patch_skip_ref_b] = arm64_b_cond(0, skip_ref_pc - patch_skip_ref_b);
  out[patch_skip_ref_c] = arm64_b_cond(0, skip_ref_pc - patch_skip_ref_c);

  out[patch_skip_unref_a] = arm64_cbz(1, skip_unref_pc - patch_skip_unref_a);
  out[patch_skip_unref_b] = arm64_b_cond(0, skip_unref_pc - patch_skip_unref_b);
  out[patch_skip_unref_c] = arm64_b_cond(0, skip_unref_pc - patch_skip_unref_c);
  PATCH_DEOPT_CBZ_W(patch_to_deopt, 6);
#if !ALCOVE_SINGLE_THREADED
  PATCH_DEOPT_TBNZ(patch_shared_ref, 7, 3);
  PATCH_DEOPT_TBNZ(patch_shared_unref, 7, 3);
#endif

  JIT_GUARD(96);
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
   Hot inner loop in nqueens. Walks the placed-queens list, checking
   column + diagonal conflicts. Inline refexp/unrefexp for the cdr walk;
   falls through to bytecode (NULL deopt) if a refcount actually hits 0. */
static int try_jit_safe_p(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 71-byte nqueens safe? — shape walk shared with the amd64 backend via
     match_safe_p (jit_common.h, which also enforces the arm64 LDR-imm
     slot-offset ceiling); we emit only. The struct-field offset ceilings
     below (content/next/nref/flags) stay arm64-specific. */
  struct match_safe_p m;
  if (!match_safe_p(bc, &m))
    return 0;

  int off_c = m.off_c, off_qs = m.off_qs, off_off = m.off_off;
  int off_cont = (int)offsetof(struct exp_t, content);
  int off_next = (int)offsetof(struct exp_t, next);
  int off_nref = (int)offsetof(struct exp_t, nref);
  if (off_c > 32760 || off_qs > 32760 || off_off > 32760 || off_cont > 32760 ||
      off_next > 32760 || off_nref > 16380)
    return 0;
#if !ALCOVE_SINGLE_THREADED
  int off_flags = (int)offsetof(struct exp_t, flags);
  if (off_flags > 4095)
    return 0; /* LDRB unsigned-offset limit */
  int patch_shared_ref = -1, patch_shared_unref = -1;
#endif

  int n = 0;
  int entry_pc = n;

  /* Preload nil + true into x9, x10 (live across iterations). */
  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)nil_singleton);
  n += emit_mov64(out + n, 10, (uint64_t)(uintptr_t)true_singleton);

  /* x1 = qs. If null or nil → return t. */
  out[n++] = arm64_ldr_imm(1, 0, off_qs);
  int patch_t1 = n;
  out[n++] = 0; /* cbz x1, ret_t */
  out[n++] = arm64_cmp_reg(1, 9);
  int patch_t2 = n;
  out[n++] = 0; /* b.eq ret_t */

  /* x2 = car(qs) tagged. Tag-check. */
  out[n++] = arm64_ldr_imm(2, 1, off_cont);
  int patch_da = n;
  out[n++] = 0; /* tbz x2,#0,deopt */

  /* x3 = c. If c == car → return nil. */
  out[n++] = arm64_ldr_imm(3, 0, off_c);
  out[n++] = arm64_cmp_reg(3, 2);
  int patch_n1 = n;
  out[n++] = 0; /* b.eq ret_nil */

  /* x4 = offset. Tag-check. */
  out[n++] = arm64_ldr_imm(4, 0, off_off);
  int patch_db = n;
  out[n++] = 0; /* tbz x4,#0,deopt */

  /* (c + off)_tagged = c_tagged + off_tagged - 1. Compare with car. */
  out[n++] = arm64_add_reg(5, 3, 4);
  out[n++] = arm64_sub_imm(5, 5, 1);
  out[n++] = arm64_cmp_reg(5, 2);
  int patch_n2 = n;
  out[n++] = 0; /* b.eq ret_nil */

  /* (c - off)_tagged = c_tagged - off_tagged + 1. Compare with car. */
  out[n++] = arm64_sub_reg(5, 3, 4);
  out[n++] = arm64_add_imm(5, 5, 1);
  out[n++] = arm64_cmp_reg(5, 2);
  int patch_n3 = n;
  out[n++] = 0; /* b.eq ret_nil */

  /* Cdr walk. x5 = cdr(qs) = qs->next. */
  out[n++] = arm64_ldr_imm(5, 1, off_next);

  /* refexp(x5) inline: skip if NULL/nil/true; else nref++. */
  int patch_skip_ref_a = n;
  out[n++] = 0; /* cbz x5, skip_ref */
  out[n++] = arm64_cmp_reg(5, 9);
  int patch_skip_ref_b = n;
  out[n++] = 0; /* b.eq skip_ref */
  out[n++] = arm64_cmp_reg(5, 10);
  int patch_skip_ref_c = n;
  out[n++] = 0; /* b.eq skip_ref */
#if !ALCOVE_SINGLE_THREADED
  /* If the target is FLAG_SHARED, deopt to bytecode (which uses atomic
     refcount macros). Reads the low byte of flags — FLAG_SHARED=8 lives
     in bit 3, well within the byte. */
  out[n++] = arm64_ldrb_imm(7, 5, off_flags);
  patch_shared_ref = n;
  out[n++] = 0; /* tbnz w7, #3, deopt */
#endif
  out[n++] = arm64_ldr_w_imm(6, 5, off_nref);
  out[n++] = arm64_add_w_imm(6, 6, 1);
  out[n++] = arm64_str_w_imm(6, 5, off_nref);
  int skip_ref_pc = n;

  /* unrefexp(x1=qs) inline: skip if NULL/nil/true; else nref--; if 0 deopt. */
  int patch_skip_unref_a = n;
  out[n++] = 0; /* cbz x1, skip_unref */
  out[n++] = arm64_cmp_reg(1, 9);
  int patch_skip_unref_b = n;
  out[n++] = 0;
  out[n++] = arm64_cmp_reg(1, 10);
  int patch_skip_unref_c = n;
  out[n++] = 0;
#if !ALCOVE_SINGLE_THREADED
  out[n++] = arm64_ldrb_imm(7, 1, off_flags);
  patch_shared_unref = n;
  out[n++] = 0; /* tbnz w7, #3, deopt */
#endif
  out[n++] = arm64_ldr_w_imm(6, 1, off_nref);
  out[n++] = arm64_sub_w_imm(6, 6, 1);
  out[n++] = arm64_str_w_imm(6, 1, off_nref);
  int patch_to_deopt = n;
  out[n++] = 0; /* cbz w6, deopt */
  int skip_unref_pc = n;

  /* slot[qs] = cdr (x5) */
  out[n++] = arm64_str_imm(5, 0, off_qs);

  /* offset += 8 (tagged add 1). */
  out[n++] = arm64_add_imm(4, 4, 8);
  out[n++] = arm64_str_imm(4, 0, off_off);

  /* tail-self: b entry */
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  /* ret_t: x0 = true_singleton */
  int ret_t_pc = n;
  out[n++] = arm64_mov_reg(0, 10);
  out[n++] = arm64_ret();

  /* ret_nil: x0 = nil_singleton */
  int ret_nil_pc = n;
  out[n++] = arm64_mov_reg(0, 9);
  out[n++] = arm64_ret();

  /* deopt: x0 = NULL */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  /* Patch all forward branches. */
  out[patch_t1] = arm64_cbz(1, ret_t_pc - patch_t1);
  out[patch_t2] = arm64_b_cond(0 /* EQ */, ret_t_pc - patch_t2);
  PATCH_DEOPT_TBZ(patch_da, 2, 0);
  PATCH_DEOPT_TBZ(patch_db, 4, 0);
  out[patch_n1] = arm64_b_cond(0, ret_nil_pc - patch_n1);
  out[patch_n2] = arm64_b_cond(0, ret_nil_pc - patch_n2);
  out[patch_n3] = arm64_b_cond(0, ret_nil_pc - patch_n3);

  out[patch_skip_ref_a] = arm64_cbz(5, skip_ref_pc - patch_skip_ref_a);
  out[patch_skip_ref_b] = arm64_b_cond(0, skip_ref_pc - patch_skip_ref_b);
  out[patch_skip_ref_c] = arm64_b_cond(0, skip_ref_pc - patch_skip_ref_c);

  out[patch_skip_unref_a] = arm64_cbz(1, skip_unref_pc - patch_skip_unref_a);
  out[patch_skip_unref_b] = arm64_b_cond(0, skip_unref_pc - patch_skip_unref_b);
  out[patch_skip_unref_c] = arm64_b_cond(0, skip_unref_pc - patch_skip_unref_c);
  PATCH_DEOPT_CBZ_W(patch_to_deopt, 6);
#if !ALCOVE_SINGLE_THREADED
  PATCH_DEOPT_TBNZ(patch_shared_ref, 7, 3);
  PATCH_DEOPT_TBNZ(patch_shared_unref, 7, 3);
#endif

  /* Suppress unused-on-some-paths warnings. */
  (void)arm64_cbnz;
  (void)arm64_cmp_reg_w;

  JIT_GUARD(96);
  *outn = n;
  return 1;
}

/* mark-from from sieve-fast — 35-byte exact-match shape.
     (def mark-from (step j n marks)
       (if (> j n) nil
           (do (vec-set! marks j nil)
               (mark-from step (+ j step) n marks))))
   Tight inner loop — writes nil into marks[j], increments j by step,
   tail-self. ~10 instructions per iteration. */
static int try_jit_mark_from(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 35-byte sieve mark-from inner loop — shape walk shared with the amd64
     backend via match_mark_from (jit_common.h, which also enforces the arm64
     LDR-imm slot-offset ceiling); we emit only. */
  struct match_mark_from m;
  if (!match_mark_from(bc, &m))
    return 0;

  int off_j = m.off_j, off_n = m.off_n, off_step = m.off_step,
      off_marks = m.off_marks;
  int off_ptr = (int)offsetof(struct exp_t, ptr);
  if (off_ptr > 32760)
    return 0;

  int n = 0;

  int entry_pc = n;
  /* x1 = j tagged, x2 = n tagged. */
  out[n++] = arm64_ldr_imm(1, 0, off_j);
  out[n++] = arm64_ldr_imm(2, 0, off_n);
  int patch_da = n;
  out[n++] = 0; /* tbz x1,#0,deopt */
  int patch_db = n;
  out[n++] = 0; /* tbz x2,#0,deopt */

  /* if (j > n): return nil. cmp x1, x2; b.gt done */
  out[n++] = arm64_cmp_reg(1, 2);
  int patch_done = n;
  out[n++] = 0; /* b.gt done */

  /* x3 = marks (exp_t*), then x3 = marks->ptr (alc_vec_t*).  We assume
     VEC_KIND_GEN (8-byte exp_t* cells); for typed kinds the JIT'd write
     would corrupt int64/double payload. Check the flags byte and bail. */
  out[n++] = arm64_ldr_imm(3, 0, off_marks);
  int off_flags_m = (int)offsetof(struct exp_t, flags);
  out[n++] = arm64_ldrb_imm(7, 3, off_flags_m);
  int patch_kind_m_a = n;
  out[n++] = 0; /* tbnz w7,#4,deopt */
  int patch_kind_m_b = n;
  out[n++] = 0; /* tbnz w7,#5,deopt */
  out[n++] = arm64_ldr_imm(3, 3, off_ptr);

  /* x4 = marks_ptr + j_tagged + 7 = &data[j_untagged]. */
  out[n++] = arm64_add_reg(4, 3, 1);
  out[n++] = arm64_add_imm(4, 4, 7);

  /* x5 = nil_singleton; *(x4) = x5. */
  n += emit_mov64(out + n, 5, (uint64_t)(uintptr_t)nil_singleton);
  out[n++] = arm64_str_imm(5, 4, 0);

  /* j = j + step - 1 (tagged-arith — drop the extra tag bit). */
  out[n++] = arm64_ldr_imm(6, 0, off_step);
  out[n++] = arm64_add_reg(1, 1, 6);
  out[n++] = arm64_sub_imm(1, 1, 1);
  out[n++] = arm64_str_imm(1, 0, off_j);

  /* tail-self: b entry */
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  /* done: x0 = nil */
  int done_pc = n;
  n += emit_mov64(out + n, 0, (uint64_t)(uintptr_t)nil_singleton);
  out[n++] = arm64_ret();

  /* deopt */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_done] = arm64_b_cond(12 /* GT */, done_pc - patch_done);
  PATCH_DEOPT_TBZ(patch_da, 1, 0);
  PATCH_DEOPT_TBZ(patch_db, 2, 0);
  PATCH_DEOPT_TBNZ(patch_kind_m_a, 7, 4);
  PATCH_DEOPT_TBNZ(patch_kind_m_b, 7, 5);

  JIT_GUARD(64);
  *outn = n;
  return 1;
}

/* Tail counter loop with one inner global call before the recurse.
   26-byte body produced by:
     (def lp (k) (if (cmp k K1) (do (g K_arg) (lp (op k K2))) k))
   Establish a frame, run the loop in registers, BLR
   jit_call_global1_drop for the inner call, propagate any error. */
static int try_jit_tail_loop_with_call(bytecode_t *bc, uint32_t *out,
                                       int *outn) {
  /* 26-byte tail-counter loop with one inner global call — shape walk shared
     with the amd64 backend via match_tail_loop_with_call (jit_common.h, which
     also enforces the arm64 LDR-imm slot-offset ceiling); we emit only. */
  struct match_tail_loop_with_call m;
  if (!match_tail_loop_with_call(bc, &m))
    return 0;

  uint8_t cmp_op = m.cmp_op, arith_op = m.arith_op;
  uint8_t const_idx = m.const_idx;
  int slot_off = m.slot_off;
  int64_t cmp_tagged = ((int64_t)m.cmp_imm << 3) | 1;
  if (cmp_tagged < 0 || cmp_tagged > 4095)
    return 0;
  int arith_delta = ((int)m.arith_imm) << 3;
  if (arith_delta < 0 || arith_delta > 4095)
    return 0;
  int64_t tagged_arg = ((int64_t)m.arg_imm << 3) | 1;

  int inv_cc;
  switch (cmp_op) {
  case OP_SLOT_GT_FIX:
    inv_cc = 13;
    break;
  case OP_SLOT_LT_FIX:
    inv_cc = 10;
    break;
  case OP_SLOT_GE_FIX:
    inv_cc = 11;
    break;
  case OP_SLOT_LE_FIX:
    inv_cc = 12;
    break;
  case OP_SLOT_IS_FIX:
    inv_cc = 0;
    break; /* base when (is slot K); loop exits on equal → EQ */
  default:
    return 0;
  }

  int n = 0;
  out[n++] = arm64_ldr_imm(1, 0, slot_off);
  int patch_deopt = n;
  out[n++] = 0;

  out[n++] = arm64_stp_pre_sp(29, 30, -32);
  out[n++] = arm64_stp_off_sp(19, 20, 16);
  out[n++] = arm64_mov_from_sp(29);
  out[n++] = arm64_mov_reg(19, 0);

  int loop_top = n;
  out[n++] = arm64_ldr_imm(1, 19, slot_off);
  out[n++] = arm64_cmp_imm(1, (int)cmp_tagged);
  int patch_end = n;
  out[n++] = 0;

  n += emit_mov64(out + n, 0, (uint64_t)(uintptr_t)bc);
  out[n++] = arm64_mov_reg(1, 19);
  n += emit_mov64(out + n, 2, (uint64_t)const_idx);
  n += emit_mov64(out + n, 3, (uint64_t)tagged_arg);
  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)&jit_call_global1_drop);
  out[n++] = arm64_blr(9);

  int patch_err = n;
  out[n++] = 0; /* cbnz x0, err */

  out[n++] = arm64_ldr_imm(1, 19, slot_off);
  if (arith_op == OP_SLOT_SUB_FIX)
    out[n++] = arm64_sub_imm(1, 1, arith_delta);
  else
    out[n++] = arm64_add_imm(1, 1, arith_delta);
  out[n++] = arm64_str_imm(1, 19, slot_off);
  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur);
  }

  int end_pc = n;
  out[n++] = arm64_ldr_imm(0, 19, slot_off);
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_ret();

  int err_pc = n;
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_ret();

  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  /* Use the proper helper so the offset gets range-checked instead of
     silently truncated by the inline mask. */
  out[patch_err] = arm64_cbnz(0, err_pc - patch_err);
  out[patch_end] = arm64_b_cond(inv_cc, end_pc - patch_end);
  PATCH_DEOPT_TBZ(patch_deopt, 1, 0);

  JIT_GUARD(64);
  *outn = n;
  return 1;
}

/* Knuth's tak — 50-byte exact-match shape.
     (def tak (x y z) (if (no (< y x)) z
                          (tak (tak (- x 1) y z)
                               (tak (- y 1) z x)
                               (tak (- z 1) x y))))
   Three nested non-tail self-calls + one tail self-call. Each inner call
   is a direct intra-buffer BL into our own entry. We stash the 3 originals
   and 3 intermediate results in the stack frame across calls. */
static int try_jit_tak(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 50-byte tak — shape walk shared with the amd64 backend via match_tak
     (jit_common.h, which also enforces the arm64 LDR-imm slot-offset
     ceiling); we emit only. */
  struct match_tak m;
  if (!match_tak(bc, &m))
    return 0;

  /* All three CALL_GLOBALs must target THIS function — the matcher
     emits intra-buffer BL to entry. Without the check, any (def f (x y z)
     ...) with the tak shape silently rewires the calls. */
  if (!(bc_calls_self(bc, m.idx_a) && bc_calls_self(bc, m.idx_b) &&
        bc_calls_self(bc, m.idx_c)))
    return 0;

  int off_x = m.off_x, off_y = m.off_y, off_z = m.off_z;

  /* Frame: 80 bytes. [sp+0]=fp, +8=lr, +16=x19, +24=pad, +32..+48=orig
     x/y/z, +56..+72=t1/t2/t3. */
  int n = 0;
  int entry_pc = n;
  out[n++] = arm64_ldr_imm(1, 0, off_y);
  out[n++] = arm64_ldr_imm(2, 0, off_x);
  int patch_da = n;
  out[n++] = 0;
  int patch_db = n;
  out[n++] = 0;

  out[n++] = arm64_cmp_reg(1, 2);
  int patch_recurse = n;
  out[n++] = 0;
  out[n++] = arm64_ldr_imm(0, 0, off_z);
  out[n++] = arm64_ret();

  int recurse_pc = n;
  out[n++] = arm64_stp_pre_sp(29, 30, -80);
  out[n++] = arm64_stp_off_sp(19, 20, 16);
  out[n++] = arm64_mov_from_sp(29);
  out[n++] = arm64_mov_reg(19, 0);

  out[n++] = arm64_str_imm(2, 31, 32);
  out[n++] = arm64_str_imm(1, 31, 40);
  out[n++] = arm64_ldr_imm(3, 0, off_z);
  out[n++] = arm64_str_imm(3, 31, 48);

  out[n++] = arm64_sub_imm(2, 2, 8);
  out[n++] = arm64_str_imm(2, 0, off_x);
  {
    int cur = n++;
    out[cur] = 0x94000000u | ((uint32_t)(entry_pc - cur) & 0x3FFFFFFu);
  }
  int patch_b1 = n;
  out[n++] = 0;
  out[n++] = arm64_str_imm(0, 31, 56);
  out[n++] = arm64_mov_reg(0, 19);

  out[n++] = arm64_ldr_imm(1, 31, 40);
  out[n++] = arm64_sub_imm(1, 1, 8);
  out[n++] = arm64_str_imm(1, 0, off_x);
  out[n++] = arm64_ldr_imm(1, 31, 48);
  out[n++] = arm64_str_imm(1, 0, off_y);
  out[n++] = arm64_ldr_imm(1, 31, 32);
  out[n++] = arm64_str_imm(1, 0, off_z);
  {
    int cur = n++;
    out[cur] = 0x94000000u | ((uint32_t)(entry_pc - cur) & 0x3FFFFFFu);
  }
  int patch_b2 = n;
  out[n++] = 0;
  out[n++] = arm64_str_imm(0, 31, 64);
  out[n++] = arm64_mov_reg(0, 19);

  out[n++] = arm64_ldr_imm(1, 31, 48);
  out[n++] = arm64_sub_imm(1, 1, 8);
  out[n++] = arm64_str_imm(1, 0, off_x);
  out[n++] = arm64_ldr_imm(1, 31, 32);
  out[n++] = arm64_str_imm(1, 0, off_y);
  out[n++] = arm64_ldr_imm(1, 31, 40);
  out[n++] = arm64_str_imm(1, 0, off_z);
  {
    int cur = n++;
    out[cur] = 0x94000000u | ((uint32_t)(entry_pc - cur) & 0x3FFFFFFu);
  }
  int patch_b3 = n;
  out[n++] = 0;
  out[n++] = arm64_str_imm(0, 31, 72);
  out[n++] = arm64_mov_reg(0, 19);

  out[n++] = arm64_ldr_imm(1, 31, 56);
  out[n++] = arm64_str_imm(1, 0, off_x);
  out[n++] = arm64_ldr_imm(1, 31, 64);
  out[n++] = arm64_str_imm(1, 0, off_y);
  out[n++] = arm64_ldr_imm(1, 31, 72);
  out[n++] = arm64_str_imm(1, 0, off_z);

  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 80);
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  int bail_pc = n;
  out[n++] = arm64_ldp_off_sp(19, 20, 16);
  out[n++] = arm64_ldp_post_sp(29, 30, 80);
  out[n++] = arm64_ret();

  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_recurse] = arm64_b_cond(11 /* LT */, recurse_pc - patch_recurse);
  out[patch_b1] = arm64_tbz(0, 0, bail_pc - patch_b1);
  out[patch_b2] = arm64_tbz(0, 0, bail_pc - patch_b2);
  out[patch_b3] = arm64_tbz(0, 0, bail_pc - patch_b3);
  PATCH_DEOPT_TBZ(patch_da, 1, 0);
  PATCH_DEOPT_TBZ(patch_db, 2, 0);

  JIT_GUARD(96);
  *outn = n;
  return 1;
}

/* The Ackermann function: 49-byte exact-match shape.
     (def ack (m n)
       (if (is m 0) (+ n 1)
           (if (is n 0) (ack (- m 1) 1)
               (ack (- m 1) (ack m (- n 1))))))
   m==0 and n==0 cases run inline (no frame); the general case opens a
   frame, recursive-CALLs the inner ack(m, n-1) via intra-buffer BL,
   then tail-self's to ack(m-1, result).

   Both `(is m 0)` / `(is n 0)` fuse to OP_SLOT_IS_FIX (4 bytes each vs the
   old LOAD_SLOT+LOAD_FIX+IS = 6), so the shape is 4 bytes shorter than the
   pre-fusion 53-byte form. The emission is offset-independent native code
   (only uses slot_m/slot_n/idx_call); only the verify offsets/ncode move. */
static int try_jit_ackermann(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 49-byte ackermann — shape walk shared with the amd64 backend via
     match_ackermann (jit_common.h, which also enforces the arm64 LDR-imm
     slot-offset ceiling); we emit only. */
  struct match_ackermann m;
  if (!match_ackermann(bc, &m))
    return 0;

  /* CALL_GLOBAL must target THIS function (intra-buffer BL). */
  if (!bc_calls_self(bc, m.idx))
    return 0;

  int off_m = m.off_m, off_n = m.off_n;

  int n = 0;

  /* entry: load m,n into x1,x2; tag-check both. x0 stays as env. */
  int entry_pc = n;
  out[n++] = arm64_ldr_imm(1, 0, off_m); /* x1 = m */
  out[n++] = arm64_ldr_imm(2, 0, off_n); /* x2 = n */
  int patch_da = n;
  out[n++] = 0; /* tbz x1,#0,deopt */
  int patch_db = n;
  out[n++] = 0; /* tbz x2,#0,deopt */

  /* if m == FIX(0) (= 1): return n + 8 (= n + FIX(1) - FIX(0) = n+1 tagged). */
  out[n++] = arm64_cmp_imm(1, 1);
  int patch_not_m0 = n;
  out[n++] = 0;                      /* b.ne not_m0 */
  out[n++] = arm64_add_imm(0, 2, 8); /* x0 = x2 + 8 (tagged n+1) */
  out[n++] = arm64_ret();

  int not_m0_pc = n;
  /* if n == FIX(0) (= 1): tail-self (m-1, 1). */
  out[n++] = arm64_cmp_imm(2, 1);
  int patch_not_n0 = n;
  out[n++] = 0;                      /* b.ne not_n0 */
  out[n++] = arm64_sub_imm(1, 1, 8); /* x1 = m - 8 (tagged m-1) */
  out[n++] = arm64_str_imm(1, 0, off_m);
  n += emit_mov64(out + n, 3, 9); /* tagged 1 = 9 */
  out[n++] = arm64_str_imm(3, 0, off_n);
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur); /* b entry */
  }

  /* not_n0: nested CALL ack(m, n-1), then tail-self (m-1, result). */
  int not_n0_pc = n;

  /* prologue: stp x29,x30 (FP/LR); stp x19,x20. 32-byte frame, 16-aligned. */
  out[n++] = arm64_stp_pre_sp(29, 30, -32); /* sp -= 32; [sp+0]=fp,[sp+8]=lr */
  out[n++] = arm64_stp_off_sp(19, 20, 16);  /* stp x19, x20, [sp, #16] */
  out[n++] = arm64_mov_from_sp(29);         /* mov x29, sp */

  out[n++] = arm64_mov_reg(19, 0); /* x19 = env */
  out[n++] = arm64_mov_reg(20, 1); /* x20 = m_orig */

  /* slot_n = n - 1 (tagged: -8). x2 still has n. */
  out[n++] = arm64_sub_imm(2, 2, 8);
  out[n++] = arm64_str_imm(2, 0, off_n);
  /* slot_m unchanged — inner needs ack(m, n-1). */

  /* BL entry (intra-buffer). */
  {
    int cur = n++;
    int off = entry_pc - cur;
    out[cur] = 0x94000000u | ((uint32_t)off & 0x3FFFFFFu);
  }

  /* tag-check result in x0; bail on non-fixnum. */
  int patch_bail = n;
  out[n++] = 0; /* tbz x0,#0,bail */

  /* tail-self prep: slot_m = m_orig - 1, slot_n = result, env back in x0. */
  out[n++] = arm64_sub_imm(20, 20, 8);
  out[n++] = arm64_str_imm(20, 19, off_m);
  out[n++] = arm64_str_imm(0, 19, off_n);
  out[n++] = arm64_mov_reg(0, 19);

  /* epilogue then b entry (tail-self). */
  out[n++] = arm64_ldp_off_sp(19, 20, 16);  /* ldp x19, x20, [sp, #16] */
  out[n++] = arm64_ldp_post_sp(29, 30, 32); /* ldp fp,lr ; sp += 32 */
  {
    int cur = n++;
    out[cur] = arm64_b(entry_pc - cur);
  }

  /* bail: tear down + return x0 (NULL/error). */
  int bail_pc = n;
  out[n++] = arm64_ldp_off_sp(19, 20, 16); /* ldp x19, x20, [sp, #16] */
  out[n++] = arm64_ldp_post_sp(29, 30, 32);
  out[n++] = arm64_ret();

  /* deopt (no frame yet). */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  /* Patch forward branches. */
  out[patch_not_m0] = arm64_b_cond(1 /* NE */, not_m0_pc - patch_not_m0);
  out[patch_not_n0] = arm64_b_cond(1 /* NE */, not_n0_pc - patch_not_n0);
  out[patch_bail] = arm64_tbz(0, 0, bail_pc - patch_bail);
  PATCH_DEOPT_TBZ(patch_da, 1, 0);
  PATCH_DEOPT_TBZ(patch_db, 2, 0);

  /* Suppress unused-warning for the helper we resolved inline. */
  (void)arm64_stp_pre_sp;
  (void)arm64_ldp_post_sp;

  JIT_GUARD(64);
  *outn = n;
  return 1;
}

/* (fn (a b) (is (mod a b) K)) — 10-byte 2-param leaf, the divides? shape.
   Computes (a mod b == K) and returns t/nil. Native: sdiv + msub for the
   remainder, csel for the boolean result. ~10 cycles vs ~150 in bytecode. */
static int try_jit_modeq_leaf(bytecode_t *bc, uint32_t *out, int *outn) {
  /* 10-byte (is (mod a b) K) leaf — shape walk shared with the amd64 backend
     via match_modeq_leaf (jit_common.h, which also enforces the arm64 LDR-imm
     slot-offset ceiling); we emit only. */
  struct match_modeq_leaf m;
  if (!match_modeq_leaf(bc, &m))
    return 0;

  int off_a = m.off_a, off_b = m.off_b;

  /* (K << 3) is the value we compare against. Untagged a%b is (a<<3) %
     (b<<3) once we've stripped the tag bit. */
  int64_t k_shifted = ((int64_t)m.K) << 3;

  int n = 0;
  /* Load both slots. */
  out[n++] = arm64_ldr_imm(1, 0, off_a); /* x1 = a tagged */
  out[n++] = arm64_ldr_imm(2, 0, off_b); /* x2 = b tagged */
  /* Tag-check both. */
  int patch_t1 = n;
  out[n++] = 0; /* tbz x1,#0,deopt */
  int patch_t2 = n;
  out[n++] = 0; /* tbz x2,#0,deopt */
  /* Untag (sub 1). After this, x1=a<<3, x2=b<<3. */
  out[n++] = arm64_sub_imm(1, 1, 1);
  out[n++] = arm64_sub_imm(2, 2, 1);
  /* Guard against div-by-zero. */
  int patch_dz = n;
  out[n++] = 0; /* cbz x2, deopt */
  /* x3 = x1 / x2, then x4 = x1 - x3*x2  (= a%b << 3). */
  out[n++] = arm64_sdiv(3, 1, 2);
  out[n++] = arm64_msub(4, 3, 2, 1);
  /* Compare remainder to K_shifted. K_shifted may be negative or > 4095
     for some K — go through a register if it doesn't fit imm12. */
  if (k_shifted >= 0 && k_shifted <= 4095) {
    out[n++] = arm64_cmp_imm(4, (int)k_shifted);
  } else {
    n += emit_mov64(out + n, 5, (uint64_t)k_shifted);
    out[n++] = arm64_cmp_reg(4, 5);
  }
  /* x0 = (eq ? TRUE_EXP : NIL_EXP). */
  n += emit_mov64(out + n, 6, (uint64_t)(uintptr_t)true_singleton);
  n += emit_mov64(out + n, 7, (uint64_t)(uintptr_t)nil_singleton);
  out[n++] = arm64_csel(0, 6, 7, 0 /* EQ */);
  out[n++] = arm64_ret();

  /* deopt → return NULL */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_t1] = arm64_tbz(1, 0, deopt_pc - patch_t1);
  out[patch_t2] = arm64_tbz(2, 0, deopt_pc - patch_t2);
  PATCH_DEOPT_CBZ(patch_dz, 2);

  JIT_GUARD(32);
  *outn = n;
  return 1;
}

/* 50-byte for-loop accumulator shape — forsum pattern.
     (fn (n) (let s K_INIT_S (for i K_INIT_I n (= s (op s K_STEP_S)))))
   Iteratively: i, s untagged; loop while i <= n; s += K_step_s; i++. */
static int try_jit_for_loop_inc(bytecode_t *bc, uint32_t *out, int *outn) {
  /* Walk the shape with the BC_* cursor so the 3-byte OP_BIND_SLOT_NAMED
     binds for the named let/for slots `s` and `i` (commit 997ffbb) can't
     silently desync the later offsets again — that drift dropped this shape
     to the VM until the offsets were re-derived. The loop-limit temp is the
     only unnamed (2-byte OP_BIND_SLOT) bind. The cursor adjusts to either
     bind length automatically.
       LOAD_FIX Ks ; BIND_SLOT_NAMED s ; LOAD_FIX Ki ; BIND_SLOT_NAMED i ;
       LOAD_SLOT n ; BIND_SLOT lim ; LOAD_CONST ; SLOT_LE_SLOT i lim ;
       BR_IF_FALSE 19 ; POP ; SLOT_<ADD|SUB>_FIX s Kstep ; STORE_SLOT s ;
       LOAD_SLOT i ; LOAD_FIX 1 ; ADD ; STORE_SLOT i ; POP ; JUMP -25 ;
       UNBIND_SLOT x3 ; RET */
  uint8_t *c = bc->code;
  int pc = 0, at_is, at_ii, at_arg, at_br, at_step, at_ki, at_jmp;
  uint8_t step_s_op;
  BC_TAKE(at_is, OP_LOAD_FIX);
  int16_t K_init_s = BC_I16(at_is, 0);
  BC_EAT(OP_BIND_SLOT_NAMED); /* s (named) */
  BC_TAKE(at_ii, OP_LOAD_FIX);
  int16_t K_init_i = BC_I16(at_ii, 0);
  BC_EAT(OP_BIND_SLOT_NAMED); /* i (named) */
  BC_TAKE(at_arg, OP_LOAD_SLOT);
  uint8_t slot_arg = BC_ARG(at_arg, 0);
  BC_EAT(OP_BIND_SLOT); /* loop-limit temp (unnamed) */
  BC_EAT(OP_LOAD_CONST);
  BC_EAT(OP_SLOT_LE_SLOT);
  BC_TAKE(at_br, OP_BR_IF_FALSE);
  if (BC_I16(at_br, 0) != 19) /* exit branch must clear the loop body */
    return 0;
  BC_EAT(OP_POP);
  BC_TAKE_ANY(at_step, step_s_op);
  if (step_s_op != OP_SLOT_ADD_FIX && step_s_op != OP_SLOT_SUB_FIX)
    return 0;
  int16_t K_step_s = BC_I16(at_step, 1);
  BC_EAT(OP_STORE_SLOT);
  BC_EAT(OP_LOAD_SLOT);
  BC_TAKE(at_ki, OP_LOAD_FIX);
  if (BC_I16(at_ki, 0) != 1) /* counter steps by +1 */
    return 0;
  BC_EAT(OP_ADD);
  BC_EAT(OP_STORE_SLOT);
  BC_EAT(OP_POP);
  BC_TAKE(at_jmp, OP_JUMP);
  if (BC_I16(at_jmp, 0) != -25) /* back-edge to the compare */
    return 0;
  BC_EAT(OP_UNBIND_SLOT);
  BC_EAT(OP_UNBIND_SLOT);
  BC_EAT(OP_UNBIND_SLOT);
  BC_EAT(OP_RET);
  BC_END();

  if (slot_arg >= ENV_INLINE_SLOTS)
    return 0;

  int arg_off = (int)offsetof(env_t, inline_vals[0]) + (int)slot_arg * 8;
  if (arg_off < 0 || arg_off > 32760)
    return 0;

  /* K_step_s clamped to arm64 add_imm/sub_imm 12-bit range. K_step_i
     fixed at 1 (verified above). K_init_i / K_init_s arbitrary int16
     — emit via mov64 to be safe. */
  int step_abs = (int)K_step_s;
  if (step_abs < 0)
    step_abs = -step_abs;
  if (step_abs > 4095)
    return 0;

  int n = 0;
  /* Load + tag-check + untag n_max into x1. */
  out[n++] = arm64_ldr_imm(1, 0, arg_off);
  int patch_tbz = n;
  out[n++] = 0;                      /* tbz x1,#0,deopt */
  out[n++] = arm64_asr_imm(1, 1, 3); /* x1 = n_max (untagged) */

  /* x2 = i (init), x3 = s (init). */
  n += emit_mov64(out + n, 2, (uint64_t)(int64_t)K_init_i);
  n += emit_mov64(out + n, 3, (uint64_t)(int64_t)K_init_s);

  /* loop_top: cmp i, n_max; b.gt done */
  int loop_top = n;
  out[n++] = arm64_cmp_reg(2, 1);
  int patch_done = n;
  out[n++] = 0; /* b.gt done */

  /* s op= K_step_s */
  if (step_s_op == OP_SLOT_ADD_FIX)
    out[n++] = arm64_add_imm(3, 3, step_abs);
  else
    out[n++] = arm64_sub_imm(3, 3, step_abs);

  /* i++ */
  out[n++] = arm64_add_imm(2, 2, 1);

  {
    int cur = n++;
    out[cur] = arm64_b(loop_top - cur);
  }

  /* done: x0 = (s << 3) | 1, ret. */
  int done_pc = n;
  ARM64_EMIT_RETAG_RET(3);

  /* deopt: x0 = NULL, ret. */
  int deopt_pc = n;
  ARM64_EMIT_DEOPT();

  out[patch_done] = arm64_b_cond(12 /* GT */, done_pc - patch_done);
  PATCH_DEOPT_TBZ(patch_tbz, 1, 0);

  JIT_GUARD(32);
  *outn = n;
  return 1;
}

/* Predicate-gated cons loop (sieve primes-up-to). The real work is the C
   kernel jit_predicate_cons_loop (jit_common.h); here we emit only a tiny
   trampoline that tail-calls it with (bc, env) — x0=bc, x1=env, br. The
   kernel's RET returns straight to our caller (no frame needed). */
static int try_jit_predicate_cons_loop(bytecode_t *bc, uint32_t *out,
                                       int *outn) {
  if (!match_predicate_cons_loop(bc))
    return 0;
  int n = 0;
  out[n++] = arm64_mov_reg(1, 0); /* x1 = env (before x0 is clobbered) */
  n += emit_mov64(out + n, 0, (uint64_t)(uintptr_t)bc); /* x0 = bc */
  n += emit_mov64(out + n, 9, (uint64_t)(uintptr_t)&jit_predicate_cons_loop);
  out[n++] = arm64_br(9); /* tail-call the kernel */
  JIT_GUARD(16);
  *outn = n;
  return 1;
}

int jit_compile(bytecode_t *bc) {
  if (!bc || bc->jit)
    return bc && bc->jit ? 1 : 0;
  uint8_t *c = bc->code;

  if (try_jit_build_inc_cons_c(bc))
    return 1;

  if (try_jit_nqueens_solve_c(bc))
    return 1;

  /* Identify the body shape. arm64 instructions are fixed 4 bytes each;
     128 ints = 512 bytes, matching the amd64 backend's buf[512]. The
     widest shape today is ackermann (~50 instructions). */
  uint32_t insns[128];
  int n = 0;

  if (try_jit_simple_tail_loop(bc, insns, &n)) {
    /* matched — fall through to mmap+install */
  } else if (try_jit_simple_tail_loop_eq(bc, insns, &n)) {
    /* swapped-polarity equality-base self-tail loop (is-base countdown) */
  } else if (try_jit_float_series_loop(bc, insns, &n)) {
    /* telescoping reciprocal float series (Leibniz/Nilakantha π) */
  } else if (try_jit_float_acc_loop(bc, insns, &n)) {
    /* integer-counter + unboxed-float-accumulator self-tail loop */
  } else if (try_jit_wide_counter_loop(bc, insns, &n)) {
    /* counter loop with a wide (>i16) const limit — generic-compare twin
       of simple_tail_loop */
  } else if (try_jit_predicate_cons_loop(bc, insns, &n)) {
    /* predicate-gated cons loop (sieve primes-up-to) — trampoline to a C
       kernel */
  } else if (try_jit_tail_loop_with_call(bc, insns, &n)) {
    /* tail loop with one inner call — fall through */
  } else if (try_jit_recurse_add_two(bc, insns, &n)) {
    /* iterative-fib fast path — fall through */
  } else if (try_jit_recurse_mul_one(bc, insns, &n)) {
    /* iterative-fact fast path — fall through */
  } else if (try_jit_for_loop_inc(bc, insns, &n)) {
    /* iterative for-loop accumulator (forsum) — fall through */
  } else if (try_jit_modeq_leaf(bc, insns, &n)) {
    /* (is (mod a b) K) leaf — fall through */
  } else if (try_jit_ackermann(bc, insns, &n)) {
    /* ackermann — fall through */
  } else if (try_jit_tak(bc, insns, &n)) {
    /* tak — fall through */
  } else if (try_jit_mark_from(bc, insns, &n)) {
    /* sieve-fast inner loop — fall through */
  } else if (try_jit_safe_p(bc, insns, &n)) {
    /* nqueens safe? — fall through */
  } else if (try_jit_is_prime_given(bc, insns, &n)) {
    /* sieve is-prime-given — fall through */
  } else if (try_jit_count_primes(bc, insns, &n)) {
    /* sieve-fast count-primes — fall through */
  } else

      if (bc->ncode == 4 && c[0] == OP_LOAD_FIX && c[3] == OP_RET) {
    /* (fn () K) — return MAKE_FIX(K). */
    int16_t k = (int16_t)((uint16_t)c[1] | ((uint16_t)c[2] << 8));
    uint64_t tagged = ((uint64_t)(int64_t)k << 3) | 1;
    n += emit_mov64(insns + n, 0, tagged);
    insns[n++] = arm64_ret();
  } else if (bc->ncode == 7 && c[0] == OP_LOAD_SLOT &&
             c[1] < ENV_INLINE_SLOTS && c[2] == OP_LOAD_FIX &&
             (c[5] == OP_ADD || c[5] == OP_SUB || c[5] == OP_MUL) &&
             c[6] == OP_RET) {
    /* (fn (... s ...) (op s K)) for K in int16, op in {+, -, *}.
       For ADD/SUB: ±(K<<3) preserves the tag bit directly.
       For MUL:     drop tag (sub 1), imul by K, re-tag (add 1). */
    int16_t k = (int16_t)((uint16_t)c[3] | ((uint16_t)c[4] << 8));
    int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)c[1] * 8;
    if (c[5] == OP_MUL) {
      insns[n++] = arm64_ldr_imm(0, 0, slot_off);
      int patch_tbz = n;
      insns[n++] = 0;                      /* tbz x0,#0,deopt */
      insns[n++] = arm64_sub_imm(0, 0, 1); /* drop tag bit -> x0 = v<<3 */
      n += emit_mov64(insns + n, 1,
                      (uint64_t)(int64_t)k); /* x1 = K (sign-ext) */
      /* (v<<3)*K overflows int64 iff v*K leaves the 61-bit range — SMULH high
         must equal the sign-extension of the low word, else deopt. */
      insns[n++] = arm64_smulh(9, 0, 1);        /* high((v<<3)*K) */
      insns[n++] = arm64_mul(0, 0, 1);          /* low = (v*K)<<3 */
      insns[n++] = arm64_cmp_reg_asr(9, 0, 63); /* high == sext(low)? */
      int patch_ovf = n;
      insns[n++] = 0;                      /* b.ne deopt */
      insns[n++] = arm64_add_imm(0, 0, 1); /* re-tag */
      insns[n++] = arm64_ret();
      int deopt_pc = n;
      insns[patch_tbz] = arm64_tbz(0, 0, deopt_pc - patch_tbz);
      insns[patch_ovf] = arm64_b_cond(ARM64_COND_NE, deopt_pc - patch_ovf);
      /* ARM64_EMIT_DEOPT() cannot be used here: this dispatcher uses
         insns[], while the macro references `out` (the shape emitter param). */
      insns[n++] = arm64_movz(0, 0, 0); /* x0 = NULL */
      insns[n++] = arm64_ret();
    } else {
      int delta = ((int)k) << 3;
      if (delta < 0 || delta > 4095)
        return 0; /* arm64 imm12 limit */
      insns[n++] = arm64_ldr_imm(0, 0, slot_off);
      int patch_tbz = n;
      insns[n++] = 0; /* tbz x0,#0,deopt */
      /* ADDS/SUBS so a tagged result outside the 61-bit range sets V -> deopt.
       */
      insns[n++] = (c[5] == OP_ADD) ? arm64_adds_imm(0, 0, delta)
                                    : arm64_subs_imm(0, 0, delta);
      int patch_ovf = n;
      insns[n++] = 0; /* b.vs deopt */
      insns[n++] = arm64_ret();
      int deopt_pc = n;
      insns[patch_tbz] = arm64_tbz(0, 0, deopt_pc - patch_tbz);
      insns[patch_ovf] = arm64_b_cond(ARM64_COND_VS, deopt_pc - patch_ovf);
      /* See comment above — ARM64_EMIT_DEOPT() uses `out`, not `insns`. */
      insns[n++] = arm64_movz(0, 0, 0); /* x0 = NULL */
      insns[n++] = arm64_ret();
    }
  } else if (bc->ncode == 5 &&
             (c[0] == OP_SLOT_ADD_FIX || c[0] == OP_SLOT_SUB_FIX) &&
             c[1] < ENV_INLINE_SLOTS && c[4] == OP_RET) {
    int16_t k = (int16_t)((uint16_t)c[2] | ((uint16_t)c[3] << 8));
    int delta = ((int)k) << 3;
    if (delta < 0 || delta > 4095)
      return 0;
    int slot_off = (int)offsetof(env_t, inline_vals[0]) + (int)c[1] * 8;
    insns[n++] = arm64_ldr_imm(0, 0, slot_off);
    int patch_tbz = n;
    insns[n++] = 0; /* tbz x0,#0,deopt */
    /* ADDS/SUBS so a tagged result outside the 61-bit range sets V -> deopt. */
    insns[n++] = (c[0] == OP_SLOT_ADD_FIX) ? arm64_adds_imm(0, 0, delta)
                                           : arm64_subs_imm(0, 0, delta);
    int patch_ovf = n;
    insns[n++] = 0; /* b.vs deopt */
    insns[n++] = arm64_ret();
    int deopt_pc = n;
    insns[patch_tbz] = arm64_tbz(0, 0, deopt_pc - patch_tbz);
    insns[patch_ovf] = arm64_b_cond(ARM64_COND_VS, deopt_pc - patch_ovf);
    /* See comment above — ARM64_EMIT_DEOPT() uses `out`, not `insns`. */
    insns[n++] = arm64_movz(0, 0, 0); /* x0 = NULL */
    insns[n++] = arm64_ret();
  } else if (try_jit_numloop(bc, insns, &n)) {
    /* general numeric self-tail loop (last-resort, after the curated shapes) */
  } else {
    return 0; /* shape not recognized */
  }

  /* Hard cap tied to the insns[] declaration above (drift-proof: stays
     correct if the buffer is ever resized), mirroring the amd64 backend's
     `n > sizeof(buf)` catch-all. The widest shape today is ackermann (~50),
     and every shape matcher is exact-ncode-gated, so emission is a
     compile-time-constant count per shape — well under this bound. */
  if (n > (int)(sizeof(insns) / sizeof(insns[0])))
    return 0;
  size_t sz = (size_t)n * 4;
  size_t pagesz = 4096;
  size_t mapsz = (sz + pagesz - 1) & ~(pagesz - 1);
  void *page = jit_alloc(mapsz);
  if (!page)
    return 0;
  jit_write_begin();
  memcpy(page, insns, sz);
  jit_write_end(page, sz);
  bc->jit = (exp_t * (*)(env_t *)) page;
  bc->jit_mem = page;
  bc->jit_size = mapsz;
  return 1;
}
