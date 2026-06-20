/* compiler.h — bytecode compiler and VM disassembly helpers. FRAGMENT
 * #included into alcove.c (single TU). NOT standalone, NOT separately compiled.
 * `make tidy` lints it in context via alcove.c.
 */

/* ---------------- Bytecode compiler + VM ---------------- */

/* Map an opcode byte to its mnemonic. Used by disasm_bytecode. */
static const char *bc_opname(uint8_t op) {
  switch (op) {
/* one case per OPCODE_LIST row (alcove.h): the name is the opcode minus OP_. */
#define X(n)                                                                   \
  case OP_##n:                                                                 \
    return #n;
    OPCODE_LIST(X)
#undef X
  default:
    return "??";
  }
}

/* Decode one instruction at code[pc] and print it. Returns the byte
   length (1..4) so the caller can advance. NOTE: bc_oplen() in jit_common.h
   is the length-only twin of this switch — keep the two in lockstep when
   adding or resizing opcodes (the JIT shape matchers walk via bc_oplen). */
static int bc_disasm_one(const uint8_t *code, int pc) {
  uint8_t op = code[pc];
  switch (op) {
  case OP_HALT:
  case OP_RET:
  case OP_POP:
  case OP_DUP:
  case OP_ADD:
  case OP_SUB:
  case OP_MUL:
  case OP_DIV:
  case OP_MOD:
  case OP_LT:
  case OP_GT:
  case OP_LE:
  case OP_GE:
  case OP_IS:
  case OP_ISO:
  case OP_NOT:
  case OP_CONS:
  case OP_CAR:
  case OP_CDR:
  case OP_VEC_REF:
  case OP_VEC_SET:
  case OP_VEC_LEN:
  case OP_VEC_NEW:
  case OP_SQRT_INT:
  case OP_ABS:
  case OP_NMAX:
  case OP_NMIN:
  case OP_LENGTH:
    printf("  %04d  %s\n", pc, bc_opname(op));
    return 1;
  case OP_LOAD_FIX:
  case OP_JUMP:
  case OP_BR_IF_FALSE:
  case OP_BR_IF_TRUE: {
    int16_t imm =
        (int16_t)((uint16_t)code[pc + 1] | ((uint16_t)code[pc + 2] << 8));
    printf("  %04d  %s %d\n", pc, bc_opname(op), (int)imm);
    return 3;
  }
  case OP_LOAD_CONST:
  case OP_EVAL_AST:
  case OP_LOAD_SLOT:
  case OP_LOAD_GLOBAL:
  case OP_SETQ_DYN:
  case OP_STORE_FREE:
  case OP_STORE_SLOT:
  case OP_BIND_SLOT:
  case OP_UNBIND_SLOT:
  case OP_CALL:
  case OP_TAIL_SELF:
  case OP_TAIL_CALL:
  case OP_LIST:
    printf("  %04d  %s %d\n", pc, bc_opname(op), (int)code[pc + 1]);
    return 2;
  case OP_CALL_GLOBAL:
    printf("  %04d  %s const_idx=%d nargs=%d\n", pc, bc_opname(op),
           (int)code[pc + 1], (int)code[pc + 2]);
    return 3;
  case OP_BIND_SLOT_NAMED:
    printf("  %04d  %s slot=%d name_const=%d\n", pc, bc_opname(op),
           (int)code[pc + 1], (int)code[pc + 2]);
    return 3;
  case OP_PUSH_HANDLER:
    printf("  %04d  %s handler_idx=%d finally_idx=%d\n", pc, bc_opname(op),
           (int)code[pc + 1], (int)code[pc + 2]);
    return 3;
  case OP_SLOT_ADD_FIX:
  case OP_SLOT_SUB_FIX:
  case OP_SLOT_LT_FIX:
  case OP_SLOT_LE_FIX:
  case OP_SLOT_GT_FIX:
  case OP_SLOT_GE_FIX:
  case OP_SLOT_IS_FIX: {
    int16_t imm =
        (int16_t)((uint16_t)code[pc + 2] | ((uint16_t)code[pc + 3] << 8));
    printf("  %04d  %s slot=%d imm=%d\n", pc, bc_opname(op), (int)code[pc + 1],
           (int)imm);
    return 4;
  }
  case OP_SLOT_LE_SLOT:
    printf("  %04d  %s slot_a=%d slot_b=%d\n", pc, bc_opname(op),
           (int)code[pc + 1], (int)code[pc + 2]);
    return 3;
  default:
    printf("  %04d  ?? 0x%02x\n", pc, op);
    return 1;
  }
}

/* Print a human-readable dump of a bytecode body: header (size + nconsts
   + JIT status) followed by one line per instruction. */
void disasm_bytecode(bytecode_t *bc) {
  if (!bc) {
    printf("  (no bytecode)\n");
    return;
  }
  printf("\x1B[96mbytecode: %d bytes, %d consts", bc->ncode, bc->nconsts);
#ifdef ALCOVE_JIT
  if (bc->jit)
    printf(", jit installed (%zu byte mmap page)", bc->jit_size);
  else
    printf(", jit not installed");
#endif
  printf("\x1B[39m\n");
  /* Type annotations, if any were declared (def f (x :int) :f64 ...). */
  int any_hint = bc->ret_hint != TYPE_HINT_NONE;
  for (int i = 0; i < bc->nparams; i++)
    if (bc->param_hints[i] != TYPE_HINT_NONE)
      any_hint = 1;
  if (any_hint) {
    printf("\x1B[96mhints:");
    for (int i = 0; i < bc->nparams; i++)
      printf(" %s %s", bc->param_keys[i] ? bc->param_keys[i] : "?",
             type_hint_name(bc->param_hints[i]));
    if (bc->ret_hint != TYPE_HINT_NONE)
      printf(" -> %s", type_hint_name(bc->ret_hint));
    printf("\x1B[39m\n");
  }
  int pc = 0;
  while (pc < bc->ncode) {
    int adv = bc_disasm_one(bc->code, pc);
    if (adv <= 0)
      break;
    pc += adv;
  }
}

void bytecode_free(bytecode_t *bc) {
  if (!bc)
    return;
  int i;
  if (bc->content)
    unrefexp(bc->content);
  for (i = 0; i < bc->nconsts; i++)
    unrefexp(bc->consts[i]);
  free(bc->consts);
  free(bc->gcache);
  free(bc->code);
  free(bc->locs);
#ifdef ALCOVE_JIT
  if (bc->jit_mem)
    munmap(bc->jit_mem, bc->jit_size);
#endif
  free(bc);
}

/* Source line/col for the instruction at code offset `pc` — the largest loc
   entry with pc <= the fault offset. Returns 1 and fills the out-params, else
   0. Runs only when raising a runtime error (cold), so a linear scan from the
   end is fine: the table holds one entry per source line of the function. */
static int bc_loc_at(bytecode_t *bc, int pc, int *line, int *col) {
  if (!bc || !bc->locs)
    return 0;
  for (int i = bc->nlocs - 1; i >= 0; i--)
    if (bc->locs[i].pc <= pc) {
      *line = bc->locs[i].line;
      *col = bc->locs[i].col;
      return 1;
    }
  return 0;
}
