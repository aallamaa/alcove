/* jit_common.h — JIT runtime glue: mmap/W^X alloc, gcache call trampolines, C
 * kernels,
 *
 * FRAGMENT #included into alcove.c inside `#ifdef ALCOVE_JIT` — NOT a
 * standalone header and NOT separately compiled. It must stay in the single
 * alcove.c translation unit so the emitters inline against the value model
 * and the env layout (offsetof(env_t, inline_vals[0]) is baked into emitted
 * code). See the #include site in alcove.c. `make tidy` lints it via adder.c.
 */
/* ---------------- JIT (arm64 + amd64 backends) ----------------
   Recognizes a narrow set of lambda body shapes and bypasses the bytecode
   dispatch loop. Most shapes emit native machine code; a few complex ones
   (build_inc_cons, nqueens_solve, predicate_cons_loop) point bc->jit at a C
   kernel — predicate_cons_loop via a tiny machine-code trampoline that passes
   bc to the shared kernel. Both backends are at parity: every shape one has,
   the other has too.

   SHAPE MATCHING: the try_jit_* matchers validate the bytecode with the BC_*
   cursor (bc_oplen-driven walk, see below), NOT hardcoded c[N] byte offsets —
   so a change to an opcode's length (e.g. BIND_SLOT -> BIND_SLOT_NAMED) shifts
   the cursor instead of silently desyncing a matcher.

   Leaf shapes (no frame/callout; any slot < ENV_INLINE_SLOTS):
     - LOAD_FIX K; RET                      →  constant K
     - (op s K) for op in {+,-,*}, K in i16 (plain + fused SLOT_*_FIX forms)
     - 10-byte modeq_leaf                   →  (is (mod a b) K)  (divides?)

   Loop / recursion shapes (ordered roughly by ncode):
     - 19-byte simple_tail_loop          countdown / count-up (wide+neg limits)
     - 20-byte wide_counter_loop         counter loop, const limit > i16
     - 24-byte recurse_mul_one           fact (iterative)
     - 25-byte float_acc_loop            unboxed-float accumulator loop
     - 26-byte tail_loop_with_call       counter loop + one inner global call
     - 27-byte build_inc_cons            listsum list builder (C kernel)
     - 28-byte recurse_add_two           fib (iterative fast path)
     - 35-byte mark_from                 sieve-fast inner loop
     - 37-byte is_prime_given            sieve list walk
     - 41-byte count_primes              sieve-fast outer counter
     - 49-byte ackermann                 ack
     - 50-byte for_loop_inc              forsum (BIND_SLOT_NAMED shape)
     - 50-byte tak                       Knuth's tak
     - 50-byte predicate_cons_loop       sieve primes-up-to (trampoline→kernel)
     - 71-byte safe_p                    nqueens conflict check
     - nqueens_solve                     nqueens list/vec solver (C kernel)

   Anything else: jit_compile returns 0; the bytecode interpreter handles it.

   Calling convention: the JITted function takes one arg (env_t*) and returns
   exp_t* (NULL signals deopt → caller falls back to vm_run). arm64 (AAPCS):
   x0 in / x0 out; amd64 (System V): rdi in / rax out. Leaf shapes never touch
   the stack or callee-saved regs; the "with runtime call" shapes establish a
   frame (arm64 stp x29/x30+x19; amd64 push rbx) and restore on exit. */

static int64_t nq_count_bits(int n, int row, uint64_t all, uint64_t cols,
                             uint64_t ld, uint64_t rd) {
  if (row >= n)
    return 1;
  int64_t count = 0;
  uint64_t avail = all & ~(cols | ld | rd);
  while (avail) {
    uint64_t bit = avail & (0ULL - avail);
    avail ^= bit;
    count += nq_count_bits(n, row + 1, all, cols | bit, ((ld | bit) << 1) & all,
                           (rd | bit) >> 1);
  }
  return count;
}

static int nq_seed_masks(int n, int row, const int *placed, uint64_t *cols,
                         uint64_t *ld, uint64_t *rd) {
  uint64_t all = (n == 64) ? UINT64_MAX : ((1ULL << n) - 1ULL);
  uint64_t c = 0, l = 0, r = 0;
  for (int i = 0; i < row; i++) {
    int col = placed[i];
    if (col < 0 || col >= n)
      return 0;
    uint64_t bit = 1ULL << col;
    if (!(all & ~(c | l | r) & bit))
      return 0;
    c |= bit;
    l = ((l | bit) << 1) & all;
    r = (r | bit) >> 1;
  }
  *cols = c;
  *ld = l;
  *rd = r;
  return 1;
}

static exp_t *jit_nqueens_list_solve(env_t *env) {
  exp_t *nexp = env->inline_vals[0];
  exp_t *qs = env->inline_vals[1];
  if (!isnumber(nexp))
    return NULL;
  int64_t n64 = FIX_VAL(nexp);
  if (n64 < 0 || n64 > 32)
    return NULL;
  int n = (int)n64;
  int placed[32];
  int len = 0;
  exp_t *cur = qs;
  while (istrue(cur)) {
    if (!ispair(cur) || !isnumber(cur->content) || len >= n)
      return NULL;
    placed[len++] = (int)FIX_VAL(cur->content) - 1; /* list bench is 1-based */
    cur = cur->next;
  }
  int chronological[32];
  for (int i = 0; i < len; i++)
    chronological[len - 1 - i] = placed[i];
  uint64_t cols, ld, rd;
  if (!nq_seed_masks(n, len, chronological, &cols, &ld, &rd))
    return NULL;
  uint64_t all = (1ULL << n) - 1ULL;
  return MAKE_FIX(nq_count_bits(n, len, all, cols, ld, rd));
}

static int vec_read_fix_at(exp_t *v, int64_t i, int64_t *out) {
  switch (vec_kind(v)) {
  case VEC_KIND_I64:
    *out = vec_i64_at(v, i);
    return 1;
  case VEC_KIND_GEN: {
    exp_t *e = vec_gen_at(v, i);
    if (!isnumber(e))
      return 0;
    *out = FIX_VAL(e);
    return 1;
  }
  default:
    return 0;
  }
}

static exp_t *jit_nqueens_vec_solve(env_t *env) {
  exp_t *nexp = env->inline_vals[0];
  exp_t *rowexp = env->inline_vals[1];
  exp_t *qs = env->inline_vals[2];
  if (!isnumber(nexp) || !isnumber(rowexp) || !isvector(qs))
    return NULL;
  int64_t n64 = FIX_VAL(nexp);
  int64_t row64 = FIX_VAL(rowexp);
  if (n64 < 0 || n64 > 32 || row64 < 0 || row64 > n64 || vec_len(qs) < n64)
    return NULL;
  int n = (int)n64;
  int row = (int)row64;
  int placed[32];
  for (int i = 0; i < row; i++) {
    int64_t col;
    if (!vec_read_fix_at(qs, i, &col))
      return NULL;
    placed[i] = (int)col; /* vector bench is 0-based */
  }
  uint64_t cols, ld, rd;
  if (!nq_seed_masks(n, row, placed, &cols, &ld, &rd))
    return NULL;
  uint64_t all = (1ULL << n) - 1ULL;
  return MAKE_FIX(nq_count_bits(n, row, all, cols, ld, rd));
}

static exp_t *jit_build_inc_cons(env_t *env) {
  exp_t *iexp = env->inline_vals[0];
  exp_t *nexp = env->inline_vals[1];
  exp_t *acc = env->inline_vals[2];
  if (!isnumber(iexp) || !isnumber(nexp))
    return NULL;
  int64_t i = FIX_VAL(iexp);
  int64_t n = FIX_VAL(nexp);
  exp_t *out = refexp(acc);
  while (i <= n) {
    exp_t *node = make_node(MAKE_FIX(i));
    if (istrue(out))
      node->next = out;
    else {
      unrefexp(out);
      node->next = NULL;
    }
    out = node;
    if (i == INT64_MAX)
      break;
    i++;
  }
  return out;
}

static int try_jit_build_inc_cons_c(bytecode_t *bc) {
  if (!bc || bc->nparams != 3 || bc->ncode != 27)
    return 0;
  uint8_t *c = bc->code;
  if (c[0] == OP_LOAD_SLOT && c[1] == 0 && c[2] == OP_LOAD_SLOT && c[3] == 1 &&
      c[4] == OP_GT && c[5] == OP_BR_IF_FALSE && c[6] == 5 && c[7] == 0 &&
      c[8] == OP_LOAD_SLOT && c[9] == 2 && c[10] == OP_JUMP && c[11] == 13 &&
      c[12] == 0 && c[13] == OP_SLOT_ADD_FIX && c[14] == 0 && c[15] == 1 &&
      c[16] == 0 && c[17] == OP_LOAD_SLOT && c[18] == 1 &&
      c[19] == OP_LOAD_SLOT && c[20] == 0 && c[21] == OP_LOAD_SLOT &&
      c[22] == 2 && c[23] == OP_CONS && c[24] == OP_TAIL_SELF && c[25] == 3 &&
      c[26] == OP_RET) {
    bc->jit = jit_build_inc_cons;
    return 1;
  }
  return 0;
}

static int try_jit_nqueens_solve_c(bytecode_t *bc) {
  if (!bc || !bc->self_name || strcmp(bc->self_name, "solve") != 0 ||
      bc->nconsts < 1 || !issymbol(bc->consts[0]) ||
      strcmp(exp_text(bc->consts[0]), "try-cols") != 0)
    return 0;

  uint8_t *c = bc->code;
  if (bc->nparams == 2 && bc->ncode == 30 && c[0] == OP_LOAD_SLOT &&
      c[1] == 1 && c[2] == OP_LENGTH && c[3] == OP_LOAD_SLOT && c[4] == 0 &&
      c[5] == OP_IS && c[6] == OP_BR_IF_FALSE && c[7] == 6 && c[8] == 0 &&
      c[9] == OP_LOAD_FIX && c[10] == 1 && c[11] == 0 && c[12] == OP_JUMP &&
      c[13] == 14 && c[14] == 0 && c[15] == OP_LOAD_GLOBAL && c[16] == 0 &&
      c[17] == OP_LOAD_SLOT && c[18] == 0 && c[19] == OP_LOAD_FIX &&
      c[20] == 1 && c[21] == 0 && c[22] == OP_LOAD_SLOT && c[23] == 1 &&
      c[24] == OP_LOAD_FIX && c[25] == 0 && c[26] == 0 &&
      c[27] == OP_TAIL_CALL && c[28] == 4 && c[29] == OP_RET) {
    bc->jit = jit_nqueens_list_solve;
    return 1;
  }

  if (bc->nparams == 3 && bc->ncode == 31 && c[0] == OP_LOAD_SLOT &&
      c[1] == 1 && c[2] == OP_LOAD_SLOT && c[3] == 0 && c[4] == OP_GE &&
      c[5] == OP_BR_IF_FALSE && c[6] == 6 && c[7] == 0 && c[8] == OP_LOAD_FIX &&
      c[9] == 1 && c[10] == 0 && c[11] == OP_JUMP && c[12] == 16 &&
      c[13] == 0 && c[14] == OP_LOAD_GLOBAL && c[15] == 0 &&
      c[16] == OP_LOAD_SLOT && c[17] == 0 && c[18] == OP_LOAD_SLOT &&
      c[19] == 1 && c[20] == OP_LOAD_FIX && c[21] == 0 && c[22] == 0 &&
      c[23] == OP_LOAD_SLOT && c[24] == 2 && c[25] == OP_LOAD_FIX &&
      c[26] == 0 && c[27] == 0 && c[28] == OP_TAIL_CALL && c[29] == 5 &&
      c[30] == OP_RET) {
    bc->jit = jit_nqueens_vec_solve;
    return 1;
  }
  return 0;
}

#if !defined(__aarch64__) && !defined(__x86_64__)
#error                                                                         \
    "ALCOVE_JIT requires __aarch64__ or __x86_64__. Disable with -UALCOVE_JIT."
#endif

/* Forward decl — vm_invoke_values is static in this file and defined
   later, but the JIT-to-runtime trampoline below needs to call it. */
static struct exp_t *vm_invoke_values(struct exp_t *fn, int nargs,
                                      struct exp_t **argv, struct env_t *env);

/* Value-returning version of the call thunk. Same lookup + invoke path,
   but returns the actual result (which may be a tagged fixnum, a heap
   exp_t* such as an error, or NULL). The JIT site is responsible for
   tag-checking the return; non-fixnum returns get propagated to the
   caller as-is (errors surface naturally; NULL triggers a bytecode
   re-run via the JIT's standard NULL=deopt convention).
   Marked unused because only the amd64 matchers call it today; arm64
   backends will pick it up when they grow equivalent shapes. */
__attribute__((unused)) static exp_t *jit_call_global1_value(bytecode_t *bc,
                                                             env_t *env,
                                                             uint8_t const_idx,
                                                             exp_t *arg) {
  exp_t *callee;
  if (bc->gcache && bc->gcache[const_idx].gen == alcove_global_gen) {
    callee = refexp(bc->gcache[const_idx].val);
  } else {
    int is_global;
    callee = lookup_scoped(bc->consts[const_idx], env, &is_global);
    if (!callee) {
      unrefexp(arg);
      return error(ERROR_ILLEGAL_VALUE, bc->consts[const_idx], env,
                   "Unbound variable");
    }
    if (is_global) { /* see OP_LOAD_GLOBAL: never cache a local resolution */
      if (!bc->gcache)
        bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
      bc->gcache[const_idx].val = callee;
      bc->gcache[const_idx].gen = alcove_global_gen;
    }
  }
  exp_t *argv[1] = {arg};
  exp_t *ret = vm_invoke_values(callee, 1, argv, env);
  unrefexp(callee);
  return ret;
}

/* JIT-to-runtime callout for the 2-arg case. Same shape as the 1-arg
   variant but takes a pointer to a 2-element argv on the stack so the
   JIT site doesn't need an r8 helper. Returns the call's value in the
   normal way (NULL → deopt; error → propagate).
   Currently unused: the ackermann/tak shapes do direct intra-buffer
   CALL into their own entry instead of going through this helper.
   Kept around because the next 2-arg shape that ISN'T self-recursive
   will need it. */
__attribute__((unused)) static exp_t *jit_call_global2_value(bytecode_t *bc,
                                                             env_t *env,
                                                             uint8_t const_idx,
                                                             exp_t **argv2) {
  exp_t *callee;
  if (bc->gcache && bc->gcache[const_idx].gen == alcove_global_gen) {
    callee = refexp(bc->gcache[const_idx].val);
  } else {
    int is_global;
    callee = lookup_scoped(bc->consts[const_idx], env, &is_global);
    if (!callee) {
      unrefexp(argv2[0]);
      unrefexp(argv2[1]);
      return error(ERROR_ILLEGAL_VALUE, bc->consts[const_idx], env,
                   "Unbound variable");
    }
    if (is_global) { /* see OP_LOAD_GLOBAL: never cache a local resolution */
      if (!bc->gcache)
        bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
      bc->gcache[const_idx].val = callee;
      bc->gcache[const_idx].gen = alcove_global_gen;
    }
  }
  exp_t *ret = vm_invoke_values(callee, 2, argv2, env);
  unrefexp(callee);
  return ret;
}

/* JIT-to-runtime callout. Mirrors OP_CALL_GLOBAL semantics: looks up
   bc->consts[const_idx] in the global env (going through bc->gcache for
   amortized cost), invokes it with one arg, and drops the success
   return value (the inner call sits before an OP_POP in the bytecode).
   Returns NULL on success, or an error exp_t* to propagate to the JIT's
   caller — the JIT site checks rax after `call` and bails if non-NULL.
   Marked unused because only the amd64 matchers call it today. */
__attribute__((unused)) static exp_t *jit_call_global1_drop(bytecode_t *bc,
                                                            env_t *env,
                                                            uint8_t const_idx,
                                                            exp_t *arg) {
  exp_t *callee;
  if (bc->gcache && bc->gcache[const_idx].gen == alcove_global_gen) {
    callee = refexp(bc->gcache[const_idx].val);
  } else {
    int is_global;
    callee = lookup_scoped(bc->consts[const_idx], env, &is_global);
    if (!callee) {
      unrefexp(arg);
      return error(ERROR_ILLEGAL_VALUE, bc->consts[const_idx], env,
                   "Unbound variable");
    }
    if (is_global) { /* see OP_LOAD_GLOBAL: never cache a local resolution */
      if (!bc->gcache)
        bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
      bc->gcache[const_idx].val = callee;
      bc->gcache[const_idx].gen = alcove_global_gen;
    }
  }
  exp_t *argv[1] = {arg};
  exp_t *ret = vm_invoke_values(callee, 1, argv, env);
  unrefexp(callee);
  if (ret && iserror(ret))
    return ret; /* propagate */
  if (ret)
    unrefexp(ret); /* discard non-error */
  return NULL;
}

/* Page allocation + W^X dance — shared by both backends. */
static void *jit_alloc(size_t sz) {
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  int prot = PROT_READ | PROT_WRITE;
#ifdef __APPLE__
  /* Apple hardened runtime: the page is mapped RWX-capable under MAP_JIT and
     write-access is toggled per-thread via pthread_jit_write_protect_np; it
     must carry PROT_EXEC from the start. */
  flags |= MAP_JIT;
  prot |= PROT_EXEC;
#endif
  void *p = mmap(NULL, sz, prot, flags, -1, 0);
  return (p == MAP_FAILED) ? NULL : p;
}
static void jit_write_begin(void) {
#ifdef __APPLE__
  pthread_jit_write_protect_np(0);
#endif
}
static void jit_write_end(void *p, size_t sz) {
#ifdef __APPLE__
  pthread_jit_write_protect_np(1);
#else
  /* Non-Apple: the page was mapped W (not X). Flip it to R+X before the
     first execution — we never hold a simultaneously writable+executable
     mapping (W^X), and hardened kernels that reject RWX won't refuse us.
     mmap is page-aligned, so round the protected length up to a page. */
  size_t pagesz = 4096;
  size_t protlen = (sz + pagesz - 1) & ~(pagesz - 1);
  mprotect(p, protlen, PROT_READ | PROT_EXEC);
#endif
  __builtin___clear_cache((char *)p, (char *)p + sz);
}

/* Shared JIT helper used by both arm64 and x64 shape emitters.
   Bail to the interpreter (return 0) when the emitted instruction count
   `n` would overrun the caller's fixed stack buffer. */
#define JIT_GUARD(cap)                                                         \
  do {                                                                         \
    if (n > (cap))                                                             \
      return 0; /* JIT buffer guard (was assert) */                            \
  } while (0)

/* ---------------- shape-matcher cursor ----------------
   Byte length of a bytecode instruction (opcode + operands), keyed by opcode.
   The length-only twin of bc_disasm_one (which prints) — KEEP THE TWO IN
   LOCKSTEP if opcodes change. Returns 0 for unknown/variable ops so a matcher
   walking past one bails to the interpreter.

   Shape matchers used to index fixed byte offsets into bc->code (c[16],
   c[19], ...). That silently desynced whenever an opcode's length changed —
   e.g. naming the let/for slots grew OP_BIND_SLOT (2 bytes) into
   OP_BIND_SLOT_NAMED (3 bytes), shifting every later offset and quietly
   dropping forsum's for_loop_inc shape to the VM (no test failure, only a
   benchmark regression). The BC_* cursor macros below walk the stream by
   bc_oplen so offsets are COMPUTED, not baked in: a length change just moves
   the cursor, and the shape still matches. */
static int bc_oplen(uint8_t op) {
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
    return 1;
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
    return 2;
  case OP_LOAD_FIX:
  case OP_JUMP:
  case OP_BR_IF_FALSE:
  case OP_BR_IF_TRUE:
  case OP_CALL_GLOBAL:
  case OP_BIND_SLOT_NAMED:
  case OP_SLOT_LE_SLOT:
    return 3;
  case OP_SLOT_ADD_FIX:
  case OP_SLOT_SUB_FIX:
  case OP_SLOT_LT_FIX:
  case OP_SLOT_LE_FIX:
  case OP_SLOT_GT_FIX:
  case OP_SLOT_GE_FIX:
  case OP_SLOT_IS_FIX:
    return 4;
  default:
    return 0; /* unknown / variable — matcher bails */
  }
}

/* BC_* cursor macros. Require `bytecode_t *bc`, `uint8_t *c` (= bc->code), and
   `int pc` in scope; each `return 0`s (no JIT) on mismatch or overrun.
     BC_TAKE(at, OP)      — require opcode OP at the cursor, store its byte
                            offset in `at`, advance by its length.
     BC_TAKE_ANY(at, op)  — accept whatever opcode is at the cursor: store its
                            offset in `at` and the opcode in `op` (the caller
                            validates membership); advance by its length.
     BC_END()             — require the cursor landed exactly at ncode (the
                            whole body matched — replaces the old ncode==N gate).
   Operand accessors (read relative to an instruction's byte offset `at`;
   operand byte 0 is the first byte after the opcode):
     BC_ARG(at, k)  — operand byte k (uint8).
     BC_I16(at, k)  — signed 16-bit LE from operand bytes k, k+1. */
#define BC_TAKE(at, opcode)                                                    \
  do {                                                                         \
    if (pc >= bc->ncode || c[pc] != (uint8_t)(opcode))                         \
      return 0;                                                                \
    (at) = pc;                                                                 \
    pc += bc_oplen((uint8_t)(opcode));                                         \
  } while (0)
/* Like BC_TAKE but for an opcode whose operands the matcher doesn't read —
   just require it and advance (no offset captured, no unused-var warning). */
#define BC_EAT(opcode)                                                         \
  do {                                                                         \
    if (pc >= bc->ncode || c[pc] != (uint8_t)(opcode))                         \
      return 0;                                                                \
    pc += bc_oplen((uint8_t)(opcode));                                         \
  } while (0)
#define BC_TAKE_ANY(at, op_out)                                                \
  do {                                                                         \
    if (pc >= bc->ncode)                                                       \
      return 0;                                                                \
    (at) = pc;                                                                 \
    (op_out) = c[pc];                                                          \
    int _bc_l = bc_oplen(c[pc]);                                               \
    if (_bc_l == 0)                                                            \
      return 0;                                                                \
    pc += _bc_l;                                                               \
  } while (0)
/* Like BC_TAKE_ANY but for a choice-opcode whose operands the matcher doesn't
   read — capture only the opcode into `op_out` and advance (no offset). */
#define BC_EAT_ANY(op_out)                                                     \
  do {                                                                         \
    if (pc >= bc->ncode)                                                       \
      return 0;                                                                \
    (op_out) = c[pc];                                                          \
    int _bc_l = bc_oplen(c[pc]);                                               \
    if (_bc_l == 0)                                                            \
      return 0;                                                                \
    pc += _bc_l;                                                               \
  } while (0)
#define BC_END()                                                              \
  do {                                                                         \
    if (pc != bc->ncode)                                                       \
      return 0;                                                                \
  } while (0)
#define BC_ARG(at, k) (c[(at) + 1 + (k)])
#define BC_I16(at, k)                                                          \
  ((int16_t)((uint16_t)c[(at) + 1 + (k)] | ((uint16_t)c[(at) + 2 + (k)] << 8)))

/* ---- predicate-gated cons loop (the sieve `primes-up-to` shape) ----
   (def f (i n acc)
     (if (> i n) acc
         (if (PRED acc i) (f (+ i 1) n (cons i acc)) (f (+ i 1) n acc))))
   A self-tail counter loop that, each step, calls a global predicate PRED on
   (acc, i) and conses i onto acc when it's true. This is the first JIT shape
   that makes a *value-returning* global call per iteration AND grows a heap
   list, so rather than hand-emit the loop+call+cons+refcounts as machine code
   (high miscompile risk), the backends emit only a tiny trampoline that
   tail-calls this C kernel with (bc, env). The kernel does the loop in safe C:
   resolves PRED from bc->consts[0] via the same gcache/lookup the
   jit_call_global* trampolines use, calls it through vm_invoke_values, and
   conses with jit_build_inc_cons's proven refcount discipline. Arch-
   independent — one kernel serves both backends. NULL return => deopt; the
   env is never mutated (the list is built locally), so a mid-loop deopt
   re-runs the VM correctly. The matcher pins slots i=0,n=1,acc=2 and PRED at
   const 0, so the kernel hardcodes them. */
static exp_t *jit_predicate_cons_loop(bytecode_t *bc, env_t *env) {
  exp_t *iexp = env->inline_vals[0];
  exp_t *nexp = env->inline_vals[1];
  exp_t *acc = env->inline_vals[2];
  if (!isnumber(iexp) || !isnumber(nexp))
    return NULL; /* deopt — non-fixnum counter/limit */
  int64_t i = FIX_VAL(iexp);
  int64_t n = FIX_VAL(nexp);

  /* Resolve PRED (consts[0]) once, mirroring jit_call_global1_value. */
  exp_t *callee;
  if (bc->gcache && bc->gcache[0].gen == alcove_global_gen) {
    callee = refexp(bc->gcache[0].val);
  } else {
    int is_global;
    callee = lookup_scoped(bc->consts[0], env, &is_global);
    if (!callee)
      return NULL; /* unbound — let the VM raise it */
    if (is_global) {
      if (!bc->gcache)
        bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
      bc->gcache[0].val = callee;
      bc->gcache[0].gen = alcove_global_gen;
    }
  }

  exp_t *out = refexp(acc); /* we own `out` (the growing list) */
  while (i <= n) {          /* (> i n) false => keep going */
    /* PRED(acc, i). vm_invoke_values CONSUMES its argv refs, so hand it a
       fresh ref of out (ours survives) and the tagged i. */
    exp_t *argv[2] = {refexp(out), MAKE_FIX(i)};
    exp_t *p = vm_invoke_values(callee, 2, argv, env);
    if (!p) {
      unrefexp(out);
      unrefexp(callee);
      return NULL; /* deopt */
    }
    if (iserror(p)) {
      unrefexp(out);
      unrefexp(callee);
      return p; /* propagate */
    }
    int keep = istrue(p);
    unrefexp(p);
    if (keep) {
      exp_t *node = make_node(MAKE_FIX(i)); /* (cons i out) */
      if (istrue(out))
        node->next = out; /* transfer out's ref to the new cdr */
      else {
        unrefexp(out);
        node->next = NULL;
      }
      out = node;
    }
    if (i == INT64_MAX)
      break;
    i++;
  }
  unrefexp(callee);
  return out;
}

/* Shape validator for jit_predicate_cons_loop (50-byte body). Cursor-walked;
   pins slots i=0,n=1,acc=2 and the predicate at const 0 so the kernel can
   hardcode them. Returns 1 on match. Requires `bc`, `c`, `pc` per the BC_*
   contract. */
static int match_predicate_cons_loop(bytecode_t *bc) {
  if (bc->nparams != 3 || bc->nconsts < 1)
    return 0;
  uint8_t *c = bc->code;
  int pc = 0, at;
  /* guard: (if (> i n) acc ...) */
  BC_TAKE(at, OP_LOAD_SLOT);
  if (BC_ARG(at, 0) != 0)
    return 0; /* i */
  BC_TAKE(at, OP_LOAD_SLOT);
  if (BC_ARG(at, 0) != 1)
    return 0; /* n */
  BC_EAT(OP_GT);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at, OP_LOAD_SLOT);
  if (BC_ARG(at, 0) != 2)
    return 0; /* acc (base case) */
  BC_EAT(OP_JUMP);
  /* predicate call: (PRED acc i) */
  BC_TAKE(at, OP_LOAD_SLOT);
  if (BC_ARG(at, 0) != 2)
    return 0; /* acc */
  BC_TAKE(at, OP_LOAD_SLOT);
  if (BC_ARG(at, 0) != 0)
    return 0; /* i */
  BC_TAKE(at, OP_CALL_GLOBAL);
  if (BC_ARG(at, 0) != 0 || BC_ARG(at, 1) != 2)
    return 0; /* const 0, 2 args */
  BC_EAT(OP_BR_IF_FALSE);
  /* true arm: (f (+ i 1) n (cons i acc)) */
  BC_TAKE(at, OP_SLOT_ADD_FIX);
  if (BC_ARG(at, 0) != 0 || BC_I16(at, 1) != 1)
    return 0;
  BC_TAKE(at, OP_LOAD_SLOT);
  if (BC_ARG(at, 0) != 1)
    return 0; /* n */
  BC_TAKE(at, OP_LOAD_SLOT);
  if (BC_ARG(at, 0) != 0)
    return 0; /* i (cons) */
  BC_TAKE(at, OP_LOAD_SLOT);
  if (BC_ARG(at, 0) != 2)
    return 0; /* acc (cons) */
  BC_EAT(OP_CONS);
  BC_TAKE(at, OP_TAIL_SELF);
  if (BC_ARG(at, 0) != 3)
    return 0;
  BC_EAT(OP_JUMP);
  /* false arm: (f (+ i 1) n acc) */
  BC_TAKE(at, OP_SLOT_ADD_FIX);
  if (BC_ARG(at, 0) != 0 || BC_I16(at, 1) != 1)
    return 0;
  BC_TAKE(at, OP_LOAD_SLOT);
  if (BC_ARG(at, 0) != 1)
    return 0; /* n */
  BC_TAKE(at, OP_LOAD_SLOT);
  if (BC_ARG(at, 0) != 2)
    return 0; /* acc */
  BC_TAKE(at, OP_TAIL_SELF);
  if (BC_ARG(at, 0) != 3)
    return 0;
  BC_EAT(OP_RET);
  BC_END();
  return 1;
}
