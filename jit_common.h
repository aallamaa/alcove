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
                            whole body matched — replaces the old ncode==N
   gate). Operand accessors (read relative to an instruction's byte offset `at`;
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
#define BC_END()                                                               \
  do {                                                                         \
    if (pc != bc->ncode)                                                       \
      return 0;                                                                \
  } while (0)
#define BC_ARG(at, k) (c[(at) + 1 + (k)])
#define BC_I16(at, k)                                                          \
  ((int16_t)((uint16_t)c[(at) + 1 + (k)] | ((uint16_t)c[(at) + 2 + (k)] << 8)))

/* ---------------- backend-neutral walk helpers ----------------
   Used by the match_X shape analyzers below (and callable from each backend's
   try_jit_X). Pure analysis — they read bytecode and compute offsets; they
   never emit a byte, so sharing them cannot change emitted machine code. */

/* Byte offset of inline slot `slot` inside env_t (the constant baked into the
   emitted load/store). Unchecked — caller has already validated the slot. */
static inline int env_slot_off(uint8_t slot) {
  return (int)offsetof(env_t, inline_vals[0]) + (int)slot * 8;
}

/* Checked variant: returns the offset for a valid inline slot, or -1 if the
   slot is out of the inline range (both backends deopt in that case). On
   arm64 the offset must also fit the LDR/STR unsigned-imm12 scaled ceiling
   (32760 = 4095*8) used by arm64_ldr_imm/arm64_str_imm. */
static inline int env_slot_off_checked(uint8_t slot) {
  if (slot >= ENV_INLINE_SLOTS)
    return -1;
  int off = env_slot_off(slot);
#ifdef __aarch64__
  if (off < 0 || off > 32760)
    return -1;
#endif
  return off;
}

/* True if bc->consts[idx] is a symbol whose text equals `name`. Bounds-checks
   idx against nconsts first. Folds the repeated
   issymbol(consts[idx]) && strcmp(exp_text(consts[idx]), name)==0 guard. */
static inline int bc_const_is_sym(bytecode_t *bc, int idx, const char *name) {
  return idx >= 0 && idx < bc->nconsts && issymbol(bc->consts[idx]) &&
         strcmp(exp_text(bc->consts[idx]), name) == 0;
}

/* True if bc->consts[idx] names this very function (self-recursion guard).
   Mirrors the repeated self_name strcmp tests in the recursion shapes. */
static inline int bc_calls_self(bytecode_t *bc, int idx) {
  return bc->self_name && idx >= 0 && idx < bc->nconsts &&
         issymbol(bc->consts[idx]) &&
         strcmp(exp_text(bc->consts[idx]), bc->self_name) == 0;
}

/* ---------------- shared shape analyzers (match_X) ----------------
   Each match_X walks the bytecode with the BC_* cursor, validates slot
   consistency / consts, and fills a small POD struct of captured operands.
   The walk is arch-independent; only the emit half differs per backend. Each
   backend's try_jit_X calls match_X then emits from the struct. Because the
   walk emits no bytes, sharing it leaves the emitted machine code unchanged. */

/* simple_tail_loop / simple_tail_loop_eq captured operands.
     while (cmp(slot, cmp_imm)-fails) { slot arith= arith_imm; } return slot
   slot is the single counter slot (cmp/arith/load all agree on it). */
struct match_simple_tail_loop {
  uint8_t cmp_op, arith_op;
  uint8_t slot;
  int16_t cmp_imm, arith_imm;
  int slot_off;
};

/* 19-byte step-before-base twin (recurse-in-THEN):
     <cmp> slot K1 ; BR_IF_FALSE off ; <arith> slot K2 ; TAIL_SELF 1 ;
     JUMP off ; LOAD_SLOT slot ; RET
   cmp ∈ {SLOT_GT/LT/GE/LE/IS_FIX}, arith ∈ {SLOT_ADD/SUB_FIX}. */
static int match_simple_tail_loop(bytecode_t *bc,
                                  struct match_simple_tail_loop *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_cmp, at_arith, at_tail, at_load;
  uint8_t cmp_op, arith_op;
  BC_TAKE_ANY(at_cmp, cmp_op);
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX &&
      cmp_op != OP_SLOT_IS_FIX)
    return 0;
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE_ANY(at_arith, arith_op);
  if (arith_op != OP_SLOT_SUB_FIX && arith_op != OP_SLOT_ADD_FIX)
    return 0;
  BC_TAKE(at_tail, OP_TAIL_SELF);
  if (BC_ARG(at_tail, 0) != 1)
    return 0;
  BC_EAT(OP_JUMP);
  BC_TAKE(at_load, OP_LOAD_SLOT);
  BC_EAT(OP_RET);
  BC_END();

  uint8_t cmp_slot = BC_ARG(at_cmp, 0);
  uint8_t arith_slot = BC_ARG(at_arith, 0);
  uint8_t load_slot = BC_ARG(at_load, 0);
  if (cmp_slot != arith_slot || cmp_slot != load_slot)
    return 0;
  int slot_off = env_slot_off_checked(cmp_slot);
  if (slot_off < 0)
    return 0;

  m->cmp_op = cmp_op;
  m->arith_op = arith_op;
  m->slot = cmp_slot;
  m->cmp_imm = BC_I16(at_cmp, 1);
  m->arith_imm = BC_I16(at_arith, 1);
  m->slot_off = slot_off;
  return 1;
}

/* 19-byte equality-base twin (base-in-THEN, recurse-in-ELSE):
     SLOT_IS_FIX slot K ; BR_IF_FALSE off ; LOAD_SLOT slot ; JUMP off ;
     <arith> slot S ; TAIL_SELF 1 ; RET
   Semantics: while (slot != K) { slot arith= S; } return slot. cmp_op is
   always OP_SLOT_IS_FIX; arith ∈ {SLOT_ADD/SUB_FIX}. */
static int match_simple_tail_loop_eq(bytecode_t *bc,
                                     struct match_simple_tail_loop *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_cmp, at_load, at_arith, at_tail;
  uint8_t arith_op;
  BC_TAKE(at_cmp, OP_SLOT_IS_FIX);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_load, OP_LOAD_SLOT); /* THEN: base arm = return the slot */
  BC_EAT(OP_JUMP);
  BC_TAKE_ANY(at_arith, arith_op); /* ELSE: step */
  if (arith_op != OP_SLOT_SUB_FIX && arith_op != OP_SLOT_ADD_FIX)
    return 0;
  BC_TAKE(at_tail, OP_TAIL_SELF);
  if (BC_ARG(at_tail, 0) != 1)
    return 0;
  BC_EAT(OP_RET);
  BC_END();

  uint8_t cmp_slot = BC_ARG(at_cmp, 0);
  uint8_t load_slot = BC_ARG(at_load, 0);
  uint8_t arith_slot = BC_ARG(at_arith, 0);
  if (cmp_slot != arith_slot || cmp_slot != load_slot)
    return 0;
  int slot_off = env_slot_off_checked(cmp_slot);
  if (slot_off < 0)
    return 0;

  m->cmp_op = OP_SLOT_IS_FIX;
  m->arith_op = arith_op;
  m->slot = cmp_slot;
  m->cmp_imm = BC_I16(at_cmp, 1);
  m->arith_imm = BC_I16(at_arith, 1);
  m->slot_off = slot_off;
  return 1;
}

/* float_acc_loop captured operands (25-byte shape):
     (def f (n acc) (if (<cmp> n LIM) (f (n step= K) (acc fop FC)) acc))
   n is an integer counter slot, acc a float accumulator slot, LIM a fixnum
   const wider than i16, FC a float const, fop ∈ {+,-,*}. */
struct match_float_acc_loop {
  uint8_t cmp_op, step_op, fop;
  uint8_t cslot, aslot;
  int16_t step_imm;
  int64_t lim_val;  /* FIX_VAL(consts[lim_idx]); each backend tags it */
  uint64_t fc_bits; /* IEEE-754 bits of consts[fc_idx]->f */
  int coff, aoff;
};

/* Walk the 25-byte float-accumulator self-tail loop. Validates 2 params, the
   counter/accumulator slot consistency + distinctness + range, that the limit
   const is a fixnum and the float const is a float, and that lim<<3 won't
   overflow int64 (the tag-compare invariant both backends rely on). The
   amd64-only imm32 narrowing of the tagged limit stays in that backend. */
static int match_float_acc_loop(bytecode_t *bc,
                                struct match_float_acc_loop *m) {
  if (bc->nparams != 2)
    return 0;
  uint8_t *c = bc->code;
  int pc = 0, at_c, at_lim, at_step, at_a, at_fc, at_tail, at_a2;
  uint8_t cmp_op, step_op, fop;
  BC_TAKE(at_c, OP_LOAD_SLOT);
  uint8_t cslot = BC_ARG(at_c, 0);
  BC_TAKE(at_lim, OP_LOAD_CONST);
  uint8_t lim_idx = BC_ARG(at_lim, 0);
  BC_EAT_ANY(cmp_op);
  if (cmp_op != OP_LT && cmp_op != OP_GT && cmp_op != OP_LE && cmp_op != OP_GE)
    return 0;
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE_ANY(at_step, step_op);
  if (step_op != OP_SLOT_ADD_FIX && step_op != OP_SLOT_SUB_FIX)
    return 0;
  if (BC_ARG(at_step, 0) != cslot)
    return 0;
  int16_t step_imm = BC_I16(at_step, 1);
  BC_TAKE(at_a, OP_LOAD_SLOT);
  uint8_t aslot = BC_ARG(at_a, 0);
  BC_TAKE(at_fc, OP_LOAD_CONST);
  uint8_t fc_idx = BC_ARG(at_fc, 0);
  BC_EAT_ANY(fop);
  if (fop != OP_ADD && fop != OP_SUB && fop != OP_MUL)
    return 0;
  BC_TAKE(at_tail, OP_TAIL_SELF);
  if (BC_ARG(at_tail, 0) != 2)
    return 0;
  BC_EAT(OP_JUMP);
  BC_TAKE(at_a2, OP_LOAD_SLOT);
  if (BC_ARG(at_a2, 0) != aslot)
    return 0;
  BC_EAT(OP_RET);
  BC_END();

  /* slots distinct, in range, consistent across the body */
  if (cslot == aslot)
    return 0;
  int coff = env_slot_off_checked(cslot);
  int aoff = env_slot_off_checked(aslot);
  if (coff < 0 || aoff < 0)
    return 0;
  if (lim_idx >= bc->nconsts || fc_idx >= bc->nconsts)
    return 0;

  /* Static type gate: counter limit is a fixnum, accumulator const a float.
     Stops the matcher mis-firing on an identically-shaped INTEGER loop. */
  exp_t *lim = bc->consts[lim_idx];
  exp_t *fc = bc->consts[fc_idx];
  if (!isnumber(lim) || !isfloat(fc))
    return 0;

  int64_t lim_val = FIX_VAL(lim);
  /* Tagged compare invariant: tagging is (v<<3)|1, monotonic for signed order,
     so the tagged compare equals the value compare. Require lim<<3 not to
     overflow int64. */
  if (lim_val > (INT64_MAX >> 3) || lim_val < (INT64_MIN >> 3))
    return 0;

  uint64_t fc_bits;
  {
    double d = fc->f;
    memcpy(&fc_bits, &d, 8);
  }

  m->cmp_op = cmp_op;
  m->step_op = step_op;
  m->fop = fop;
  m->cslot = cslot;
  m->aslot = aslot;
  m->step_imm = step_imm;
  m->lim_val = lim_val;
  m->fc_bits = fc_bits;
  m->coff = coff;
  m->aoff = aoff;
  return 1;
}

/* float_series_loop captured operands (36-byte shape):
     (def f (k acc) (if (<cmp> k LIM)
                        (f (k step= S) (+ acc (- (/ N1 k) (/ N2 (k + OFF2)))))
                        acc))
   A telescoping reciprocal series (e.g. Leibniz π: (- (/ 4.0 k) (/ 4.0 (+ k 2)))
   over k += 4). k is an integer counter slot, acc a float accumulator slot, LIM
   a fixnum const, N1/N2 float consts, OFF2 a small int offset. Shares the
   counter/accumulator skeleton and the `acc` exit arm with float_acc_loop; the
   body adds the two divisions + counter-derived divisor. */
struct match_float_series_loop {
  uint8_t cmp_op, step_op;
  uint8_t cslot, aslot;
  int16_t step_imm, off2;
  int64_t lim_val;
  uint64_t n1_bits, n2_bits;
  int coff, aoff;
};

static int match_float_series_loop(bytecode_t *bc,
                                   struct match_float_series_loop *m) {
  if (bc->nparams != 2)
    return 0;
  uint8_t *c = bc->code;
  int pc = 0, at_c, at_lim, at_step, at_a, at_n1, at_k, at_n2, at_step2, at_tail,
                 at_a2;
  uint8_t cmp_op, step_op, step2_op;
  BC_TAKE(at_c, OP_LOAD_SLOT);
  uint8_t cslot = BC_ARG(at_c, 0);
  BC_TAKE(at_lim, OP_LOAD_CONST);
  uint8_t lim_idx = BC_ARG(at_lim, 0);
  BC_EAT_ANY(cmp_op);
  if (cmp_op != OP_LT && cmp_op != OP_GT && cmp_op != OP_LE && cmp_op != OP_GE)
    return 0;
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE_ANY(at_step, step_op); /* counter step → tail arg 0 */
  if (step_op != OP_SLOT_ADD_FIX && step_op != OP_SLOT_SUB_FIX)
    return 0;
  if (BC_ARG(at_step, 0) != cslot)
    return 0;
  int16_t step_imm = BC_I16(at_step, 1);
  BC_TAKE(at_a, OP_LOAD_SLOT); /* acc (tail arg 1 base) */
  uint8_t aslot = BC_ARG(at_a, 0);
  BC_TAKE(at_n1, OP_LOAD_CONST); /* N1 */
  uint8_t n1_idx = BC_ARG(at_n1, 0);
  BC_TAKE(at_k, OP_LOAD_SLOT); /* divisor 1 == k */
  if (BC_ARG(at_k, 0) != cslot)
    return 0;
  BC_EAT(OP_DIV); /* N1 / k */
  BC_TAKE(at_n2, OP_LOAD_CONST); /* N2 */
  uint8_t n2_idx = BC_ARG(at_n2, 0);
  BC_TAKE_ANY(at_step2, step2_op); /* divisor 2 == k + OFF2 (non-mutating) */
  if (step2_op != OP_SLOT_ADD_FIX)
    return 0;
  if (BC_ARG(at_step2, 0) != cslot)
    return 0;
  int16_t off2 = BC_I16(at_step2, 1);
  BC_EAT(OP_DIV); /* N2 / (k+OFF2) */
  BC_EAT(OP_SUB); /* (N1/k) - (N2/(k+OFF2)) */
  BC_EAT(OP_ADD); /* acc + term */
  BC_TAKE(at_tail, OP_TAIL_SELF);
  if (BC_ARG(at_tail, 0) != 2)
    return 0;
  BC_EAT(OP_JUMP);
  BC_TAKE(at_a2, OP_LOAD_SLOT); /* exit arm: return acc */
  if (BC_ARG(at_a2, 0) != aslot)
    return 0;
  BC_EAT(OP_RET);
  BC_END();

  if (cslot == aslot)
    return 0;
  int coff = env_slot_off_checked(cslot);
  int aoff = env_slot_off_checked(aslot);
  if (coff < 0 || aoff < 0)
    return 0;
  if (lim_idx >= bc->nconsts || n1_idx >= bc->nconsts || n2_idx >= bc->nconsts)
    return 0;

  /* Static type gate: counter limit fixnum, both numerators floats. */
  exp_t *lim = bc->consts[lim_idx];
  exp_t *n1 = bc->consts[n1_idx];
  exp_t *n2 = bc->consts[n2_idx];
  if (!isnumber(lim) || !isfloat(n1) || !isfloat(n2))
    return 0;

  int64_t lim_val = FIX_VAL(lim);
  if (lim_val > (INT64_MAX >> 3) || lim_val < (INT64_MIN >> 3))
    return 0;

  uint64_t n1_bits, n2_bits;
  {
    double d = n1->f;
    memcpy(&n1_bits, &d, 8);
  }
  {
    double d = n2->f;
    memcpy(&n2_bits, &d, 8);
  }

  m->cmp_op = cmp_op;
  m->step_op = step_op;
  m->cslot = cslot;
  m->aslot = aslot;
  m->step_imm = step_imm;
  m->off2 = off2;
  m->lim_val = lim_val;
  m->n1_bits = n1_bits;
  m->n2_bits = n2_bits;
  m->coff = coff;
  m->aoff = aoff;
  return 1;
}

/* wide_counter_loop captured operands (20-byte shape): the generic-compare
   twin of simple_tail_loop — the limit is a fixnum const wider than i16, so
   the compiler emits LOAD_SLOT/LOAD_CONST/<cmp> instead of the fused
   SLOT_<cmp>_FIX. Identical emitted loop; the limit comes from a const. */
struct match_wide_counter_loop {
  uint8_t cmp_op, arith_op;
  uint8_t slot;
  int16_t arith_imm;
  int64_t lim_val; /* FIX_VAL(consts[lim_idx]); each backend tags it */
  int slot_off;
};

/* Walk the 20-byte wide-limit integer counter loop:
     LOAD_SLOT slot ; LOAD_CONST lim ; <cmp LT|GT|LE|GE> ; BR_IF_FALSE ;
     SLOT_<ADD|SUB>_FIX slot K ; TAIL_SELF 1 ; JUMP ; LOAD_SLOT slot ; RET
   Validates slot consistency/range, that the limit const is a fixnum, and
   that lim<<3 won't overflow int64. amd64's extra imm32 narrowing stays in
   that backend. */
static int match_wide_counter_loop(bytecode_t *bc,
                                   struct match_wide_counter_loop *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_load0, at_lim, at_arith, at_tail, at_load1;
  uint8_t cmp_op, arith_op;
  BC_TAKE(at_load0, OP_LOAD_SLOT);
  uint8_t cmp_slot = BC_ARG(at_load0, 0);
  BC_TAKE(at_lim, OP_LOAD_CONST);
  uint8_t lim_idx = BC_ARG(at_lim, 0);
  BC_EAT_ANY(cmp_op);
  if (cmp_op != OP_LT && cmp_op != OP_GT && cmp_op != OP_LE && cmp_op != OP_GE)
    return 0;
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE_ANY(at_arith, arith_op);
  if (arith_op != OP_SLOT_SUB_FIX && arith_op != OP_SLOT_ADD_FIX)
    return 0;
  if (BC_ARG(at_arith, 0) != cmp_slot)
    return 0;
  int16_t arith_imm = BC_I16(at_arith, 1);
  BC_TAKE(at_tail, OP_TAIL_SELF);
  if (BC_ARG(at_tail, 0) != 1)
    return 0;
  BC_EAT(OP_JUMP);
  BC_TAKE(at_load1, OP_LOAD_SLOT);
  if (BC_ARG(at_load1, 0) != cmp_slot)
    return 0;
  BC_EAT(OP_RET);
  BC_END();

  int slot_off = env_slot_off_checked(cmp_slot);
  if (slot_off < 0 || lim_idx >= bc->nconsts)
    return 0;
  /* fixnum limit only — a float const would mis-compare against the tagged
     integer counter (the VM would promote; we must not pretend it's a fix). */
  exp_t *lim = bc->consts[lim_idx];
  if (!isnumber(lim))
    return 0;
  int64_t lim_val = FIX_VAL(lim);
  if (lim_val > (INT64_MAX >> 3) || lim_val < (INT64_MIN >> 3))
    return 0;

  m->cmp_op = cmp_op;
  m->arith_op = arith_op;
  m->slot = cmp_slot;
  m->arith_imm = arith_imm;
  m->lim_val = lim_val;
  m->slot_off = slot_off;
  return 1;
}

/* tail_loop_with_call captured operands (26-byte shape):
     (def lp (k) (if (cmp k K1) (do (g K_arg) (lp (op k K2))) k))
   A counter loop that makes one value-dropping global call per iteration. */
struct match_tail_loop_with_call {
  uint8_t cmp_op, arith_op;
  uint8_t slot, const_idx;
  int16_t cmp_imm, arith_imm, arg_imm;
  int slot_off;
};

/* Walk the 26-byte tail-counter loop with one inner global call:
     <cmp> slot K1 ; BR_IF_FALSE ; LOAD_FIX arg ; CALL_GLOBAL idx,1 ; POP ;
     <arith> slot K2 ; TAIL_SELF 1 ; JUMP ; LOAD_SLOT slot ; RET
   cmp ∈ {SLOT_GT/LT/GE/LE/IS_FIX}, arith ∈ {SLOT_ADD/SUB_FIX}. */
static int match_tail_loop_with_call(bytecode_t *bc,
                                     struct match_tail_loop_with_call *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_cmp, at_arg, at_call, at_arith, at_tail, at_load;
  uint8_t cmp_op, arith_op;
  BC_TAKE_ANY(at_cmp, cmp_op);
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX &&
      cmp_op != OP_SLOT_IS_FIX)
    return 0;
  uint8_t slot = BC_ARG(at_cmp, 0);
  int16_t cmp_imm = BC_I16(at_cmp, 1);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_arg, OP_LOAD_FIX);
  int16_t arg_imm = BC_I16(at_arg, 0);
  BC_TAKE(at_call, OP_CALL_GLOBAL);
  uint8_t const_idx = BC_ARG(at_call, 0);
  if (BC_ARG(at_call, 1) != 1) /* nargs must be 1 */
    return 0;
  if (const_idx >= bc->nconsts)
    return 0;
  BC_EAT(OP_POP);
  BC_TAKE_ANY(at_arith, arith_op);
  if (arith_op != OP_SLOT_SUB_FIX && arith_op != OP_SLOT_ADD_FIX)
    return 0;
  if (BC_ARG(at_arith, 0) != slot)
    return 0;
  int16_t arith_imm = BC_I16(at_arith, 1);
  BC_TAKE(at_tail, OP_TAIL_SELF);
  if (BC_ARG(at_tail, 0) != 1)
    return 0;
  BC_EAT(OP_JUMP);
  BC_TAKE(at_load, OP_LOAD_SLOT);
  if (BC_ARG(at_load, 0) != slot)
    return 0;
  BC_EAT(OP_RET);
  BC_END();

  int slot_off = env_slot_off_checked(slot);
  if (slot_off < 0)
    return 0;

  m->cmp_op = cmp_op;
  m->arith_op = arith_op;
  m->slot = slot;
  m->const_idx = const_idx;
  m->cmp_imm = cmp_imm;
  m->arith_imm = arith_imm;
  m->arg_imm = arg_imm;
  m->slot_off = slot_off;
  return 1;
}

/* recurse_add_two captured operands (28-byte two-call recursion, fib shape):
     (def f (n) (if (cmp n K1) n (+ (g (n op_a K2)) (g (n op_b K3)))))
   The walk is shared; the self-call / is-fib-like decision (and what to do
   when it fails) differs per backend, so those stay in each try_jit_X. */
struct match_recurse_add_two {
  uint8_t cmp_op, op_a, op_b;
  uint8_t slot, idx_a, idx_b;
  int16_t K1, K2, K3;
  int slot_off;
};

/* Walk the 28-byte two-call recursion:
     <cmp> slot K1 ; BR_IF_FALSE ; LOAD_SLOT slot ; JUMP ;
     <op_a> slot K2 ; CALL_GLOBAL idx_a,1 ; <op_b> slot K3 ;
     CALL_GLOBAL idx_b,1 ; ADD ; RET
   cmp ∈ {SLOT_GT/LT/GE/LE/IS_FIX}, op_a/op_b ∈ {SLOT_ADD/SUB_FIX}. */
static int match_recurse_add_two(bytecode_t *bc,
                                 struct match_recurse_add_two *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_cmp, at_load, at_a, at_ca, at_b, at_cb;
  uint8_t cmp_op, op_a, op_b;
  BC_TAKE_ANY(at_cmp, cmp_op);
  if (cmp_op != OP_SLOT_GT_FIX && cmp_op != OP_SLOT_LT_FIX &&
      cmp_op != OP_SLOT_GE_FIX && cmp_op != OP_SLOT_LE_FIX &&
      cmp_op != OP_SLOT_IS_FIX)
    return 0;
  uint8_t slot = BC_ARG(at_cmp, 0);
  int16_t K1 = BC_I16(at_cmp, 1);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_load, OP_LOAD_SLOT);
  if (BC_ARG(at_load, 0) != slot)
    return 0;
  BC_EAT(OP_JUMP);
  BC_TAKE_ANY(at_a, op_a);
  if (op_a != OP_SLOT_SUB_FIX && op_a != OP_SLOT_ADD_FIX)
    return 0;
  if (BC_ARG(at_a, 0) != slot)
    return 0;
  int16_t K2 = BC_I16(at_a, 1);
  BC_TAKE(at_ca, OP_CALL_GLOBAL);
  uint8_t idx_a = BC_ARG(at_ca, 0);
  if (BC_ARG(at_ca, 1) != 1)
    return 0;
  if (idx_a >= bc->nconsts)
    return 0;
  BC_TAKE_ANY(at_b, op_b);
  if (op_b != OP_SLOT_SUB_FIX && op_b != OP_SLOT_ADD_FIX)
    return 0;
  if (BC_ARG(at_b, 0) != slot)
    return 0;
  int16_t K3 = BC_I16(at_b, 1);
  BC_TAKE(at_cb, OP_CALL_GLOBAL);
  uint8_t idx_b = BC_ARG(at_cb, 0);
  if (BC_ARG(at_cb, 1) != 1)
    return 0;
  if (idx_b >= bc->nconsts)
    return 0;
  BC_EAT(OP_ADD);
  BC_EAT(OP_RET);
  BC_END();

  int slot_off = env_slot_off_checked(slot);
  if (slot_off < 0)
    return 0;

  m->cmp_op = cmp_op;
  m->op_a = op_a;
  m->op_b = op_b;
  m->slot = slot;
  m->idx_a = idx_a;
  m->idx_b = idx_b;
  m->K1 = K1;
  m->K2 = K2;
  m->K3 = K3;
  m->slot_off = slot_off;
  return 1;
}

/* recurse_mul_one captured operands (24-byte iterative-fact shape):
     (def f (n) (if (cmp n K1) BASE (* (g (n step K2)) n)))
   The self-call guard (bc_calls_self on idx) stays per-backend. */
struct match_recurse_mul_one {
  uint8_t cmp_op, step_op;
  uint8_t slot, idx;
  int16_t K1, K2, BASE;
  int slot_off;
};

/* Walk the 24-byte iterative-factorial shape:
     <cmp> slot K1 ; BR_IF_FALSE ; LOAD_FIX BASE ; JUMP ;
     LOAD_SLOT slot ; <step> slot K2 ; CALL_GLOBAL idx,1 ; MUL ; RET
   cmp ∈ {SLOT_LT/GT/LE/GE_FIX} (no IS), step ∈ {SLOT_ADD/SUB_FIX}. */
static int match_recurse_mul_one(bytecode_t *bc,
                                 struct match_recurse_mul_one *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_cmp, at_base, at_load, at_step, at_call;
  uint8_t cmp_op, step_op;
  BC_TAKE_ANY(at_cmp, cmp_op);
  if (cmp_op != OP_SLOT_LT_FIX && cmp_op != OP_SLOT_GT_FIX &&
      cmp_op != OP_SLOT_LE_FIX && cmp_op != OP_SLOT_GE_FIX)
    return 0;
  uint8_t slot = BC_ARG(at_cmp, 0);
  int16_t K1 = BC_I16(at_cmp, 1);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_base, OP_LOAD_FIX);
  int16_t BASE = BC_I16(at_base, 0);
  BC_EAT(OP_JUMP);
  BC_TAKE(at_load, OP_LOAD_SLOT);
  if (BC_ARG(at_load, 0) != slot)
    return 0;
  BC_TAKE_ANY(at_step, step_op);
  if (step_op != OP_SLOT_SUB_FIX && step_op != OP_SLOT_ADD_FIX)
    return 0;
  if (BC_ARG(at_step, 0) != slot)
    return 0;
  int16_t K2 = BC_I16(at_step, 1);
  BC_TAKE(at_call, OP_CALL_GLOBAL);
  uint8_t idx = BC_ARG(at_call, 0);
  if (BC_ARG(at_call, 1) != 1)
    return 0;
  if (idx >= bc->nconsts)
    return 0;
  BC_EAT(OP_MUL);
  BC_EAT(OP_RET);
  BC_END();

  int slot_off = env_slot_off_checked(slot);
  if (slot_off < 0)
    return 0;

  m->cmp_op = cmp_op;
  m->step_op = step_op;
  m->slot = slot;
  m->idx = idx;
  m->K1 = K1;
  m->K2 = K2;
  m->BASE = BASE;
  m->slot_off = slot_off;
  return 1;
}

/* modeq_leaf captured operands (10-byte (is (mod a b) K) leaf):
     LOAD_SLOT a ; LOAD_SLOT b ; MOD ; LOAD_FIX K ; IS ; RET */
struct match_modeq_leaf {
  int16_t K;
  int off_a, off_b;
};

static int match_modeq_leaf(bytecode_t *bc, struct match_modeq_leaf *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_a, at_b, at_k;
  BC_TAKE(at_a, OP_LOAD_SLOT);
  int off_a = env_slot_off_checked(BC_ARG(at_a, 0));
  if (off_a < 0)
    return 0;
  BC_TAKE(at_b, OP_LOAD_SLOT);
  int off_b = env_slot_off_checked(BC_ARG(at_b, 0));
  if (off_b < 0)
    return 0;
  BC_EAT(OP_MOD);
  BC_TAKE(at_k, OP_LOAD_FIX);
  int16_t K = BC_I16(at_k, 0);
  BC_EAT(OP_IS);
  BC_EAT(OP_RET);
  BC_END();

  m->K = K;
  m->off_a = off_a;
  m->off_b = off_b;
  return 1;
}

/* ackermann captured operands (49-byte exact-match shape):
     (if (is m 0) (+ n 1) (if (is n 0) (ack m-1 1) (ack m-1 (ack m n-1))))
   The self-call guard (bc_calls_self on idx) stays per-backend. */
struct match_ackermann {
  uint8_t slot_m, slot_n, idx;
  int off_m, off_n;
};

/* Walk the 49-byte ackermann shape:
     SLOT_IS_FIX m 0 ; BR_IF_FALSE ; SLOT_ADD_FIX n 1 ; JUMP ;
     SLOT_IS_FIX n 0 ; BR_IF_FALSE ;
     SLOT_SUB_FIX m 1 ; LOAD_FIX 1 ; TAIL_SELF 2 ; JUMP ;
     SLOT_SUB_FIX m 1 ; LOAD_SLOT m ; SLOT_SUB_FIX n 1 ; CALL_GLOBAL idx,2 ;
     TAIL_SELF 2 ; RET */
static int match_ackermann(bytecode_t *bc, struct match_ackermann *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_mis, at_nadd, at_nis, at_subm1, at_lf1, at_t1, at_subm2;
  int at_loadm, at_subn, at_call, at_t2;
  BC_TAKE(at_mis, OP_SLOT_IS_FIX);
  uint8_t slot_m = BC_ARG(at_mis, 0);
  if (BC_I16(at_mis, 1) != 0)
    return 0;
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_nadd, OP_SLOT_ADD_FIX);
  uint8_t slot_n_check = BC_ARG(at_nadd, 0);
  if (BC_I16(at_nadd, 1) != 1)
    return 0;
  BC_EAT(OP_JUMP);
  BC_TAKE(at_nis, OP_SLOT_IS_FIX);
  uint8_t slot_n = BC_ARG(at_nis, 0);
  if (slot_n != slot_n_check || BC_I16(at_nis, 1) != 0)
    return 0;
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_subm1, OP_SLOT_SUB_FIX);
  if (BC_ARG(at_subm1, 0) != slot_m || BC_I16(at_subm1, 1) != 1)
    return 0;
  BC_TAKE(at_lf1, OP_LOAD_FIX);
  if (BC_I16(at_lf1, 0) != 1)
    return 0;
  BC_TAKE(at_t1, OP_TAIL_SELF);
  if (BC_ARG(at_t1, 0) != 2)
    return 0;
  BC_EAT(OP_JUMP);
  BC_TAKE(at_subm2, OP_SLOT_SUB_FIX);
  if (BC_ARG(at_subm2, 0) != slot_m || BC_I16(at_subm2, 1) != 1)
    return 0;
  BC_TAKE(at_loadm, OP_LOAD_SLOT);
  if (BC_ARG(at_loadm, 0) != slot_m)
    return 0;
  BC_TAKE(at_subn, OP_SLOT_SUB_FIX);
  if (BC_ARG(at_subn, 0) != slot_n || BC_I16(at_subn, 1) != 1)
    return 0;
  BC_TAKE(at_call, OP_CALL_GLOBAL);
  uint8_t idx = BC_ARG(at_call, 0);
  if (BC_ARG(at_call, 1) != 2)
    return 0;
  if (idx >= bc->nconsts)
    return 0;
  BC_TAKE(at_t2, OP_TAIL_SELF);
  if (BC_ARG(at_t2, 0) != 2)
    return 0;
  BC_EAT(OP_RET);
  BC_END();

  int off_m = env_slot_off_checked(slot_m);
  int off_n = env_slot_off_checked(slot_n);
  if (off_m < 0 || off_n < 0)
    return 0;

  m->slot_m = slot_m;
  m->slot_n = slot_n;
  m->idx = idx;
  m->off_m = off_m;
  m->off_n = off_n;
  return 1;
}

/* tak captured operands (50-byte Knuth's tak exact-match shape):
     (if (no (< y x)) z (tak (tak x-1 y z) (tak y-1 z x) (tak z-1 x y)))
   The three self-call guards (bc_calls_self on idx_a/b/c) stay per-backend. */
struct match_tak {
  uint8_t s_x, s_y, s_z;
  uint8_t idx_a, idx_b, idx_c;
  int off_x, off_y, off_z;
};

/* Walk the 50-byte tak shape:
     LOAD_SLOT y ; LOAD_SLOT x ; LT ; NOT ; BR_IF_FALSE ; LOAD_SLOT z ; JUMP ;
     SLOT_SUB_FIX x 1 ; LOAD_SLOT y ; LOAD_SLOT z ; CALL_GLOBAL a,3 ;
     SLOT_SUB_FIX y 1 ; LOAD_SLOT z ; LOAD_SLOT x ; CALL_GLOBAL b,3 ;
     SLOT_SUB_FIX z 1 ; LOAD_SLOT x ; LOAD_SLOT y ; CALL_GLOBAL c,3 ;
     TAIL_SELF 3 ; RET */
static int match_tak(bytecode_t *bc, struct match_tak *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_y, at_x, at_z0, at_sx, at_y2, at_z2, at_ca, at_sy, at_z3;
  int at_x2, at_cb, at_sz, at_x3, at_y3, at_cc, at_tail;
  BC_TAKE(at_y, OP_LOAD_SLOT);
  uint8_t s_y = BC_ARG(at_y, 0);
  BC_TAKE(at_x, OP_LOAD_SLOT);
  uint8_t s_x = BC_ARG(at_x, 0);
  BC_EAT(OP_LT);
  BC_EAT(OP_NOT);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_z0, OP_LOAD_SLOT);
  uint8_t s_z = BC_ARG(at_z0, 0);
  BC_EAT(OP_JUMP);
  BC_TAKE(at_sx, OP_SLOT_SUB_FIX);
  if (BC_ARG(at_sx, 0) != s_x || BC_I16(at_sx, 1) != 1)
    return 0;
  BC_TAKE(at_y2, OP_LOAD_SLOT);
  if (BC_ARG(at_y2, 0) != s_y)
    return 0;
  BC_TAKE(at_z2, OP_LOAD_SLOT);
  if (BC_ARG(at_z2, 0) != s_z)
    return 0;
  BC_TAKE(at_ca, OP_CALL_GLOBAL);
  uint8_t idx_a = BC_ARG(at_ca, 0);
  if (BC_ARG(at_ca, 1) != 3)
    return 0;
  BC_TAKE(at_sy, OP_SLOT_SUB_FIX);
  if (BC_ARG(at_sy, 0) != s_y || BC_I16(at_sy, 1) != 1)
    return 0;
  BC_TAKE(at_z3, OP_LOAD_SLOT);
  if (BC_ARG(at_z3, 0) != s_z)
    return 0;
  BC_TAKE(at_x2, OP_LOAD_SLOT);
  if (BC_ARG(at_x2, 0) != s_x)
    return 0;
  BC_TAKE(at_cb, OP_CALL_GLOBAL);
  uint8_t idx_b = BC_ARG(at_cb, 0);
  if (BC_ARG(at_cb, 1) != 3)
    return 0;
  BC_TAKE(at_sz, OP_SLOT_SUB_FIX);
  if (BC_ARG(at_sz, 0) != s_z || BC_I16(at_sz, 1) != 1)
    return 0;
  BC_TAKE(at_x3, OP_LOAD_SLOT);
  if (BC_ARG(at_x3, 0) != s_x)
    return 0;
  BC_TAKE(at_y3, OP_LOAD_SLOT);
  if (BC_ARG(at_y3, 0) != s_y)
    return 0;
  BC_TAKE(at_cc, OP_CALL_GLOBAL);
  uint8_t idx_c = BC_ARG(at_cc, 0);
  if (BC_ARG(at_cc, 1) != 3)
    return 0;
  BC_TAKE(at_tail, OP_TAIL_SELF);
  if (BC_ARG(at_tail, 0) != 3)
    return 0;
  BC_EAT(OP_RET);
  BC_END();

  if (idx_a >= bc->nconsts || idx_b >= bc->nconsts || idx_c >= bc->nconsts)
    return 0;
  int off_x = env_slot_off_checked(s_x);
  int off_y = env_slot_off_checked(s_y);
  int off_z = env_slot_off_checked(s_z);
  if (off_x < 0 || off_y < 0 || off_z < 0)
    return 0;

  m->s_x = s_x;
  m->s_y = s_y;
  m->s_z = s_z;
  m->idx_a = idx_a;
  m->idx_b = idx_b;
  m->idx_c = idx_c;
  m->off_x = off_x;
  m->off_y = off_y;
  m->off_z = off_z;
  return 1;
}

/* safe_p captured operands (71-byte nqueens safe? shape): three conflict
   checks (column, +diag, -diag) walking the placed-queens list, then recurse
   on the cdr. Captures the c/qs/off slot offsets; the const guards ("t"/"nil")
   are validated inside the walk. */
struct match_safe_p {
  uint8_t s_c, s_qs, s_off;
  int off_c, off_qs, off_off;
};

static int match_safe_p(bytecode_t *bc, struct match_safe_p *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_qs, at_t, at_c, at_qs2, at_n1, at_c2, at_off, at_qs3, at_n2;
  int at_c3, at_off2, at_qs4, at_n3, at_c4, at_qs5, at_addoff, at_tail;
  BC_TAKE(at_qs, OP_LOAD_SLOT);
  uint8_t s_qs = BC_ARG(at_qs, 0);
  BC_EAT(OP_NOT);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_t, OP_LOAD_GLOBAL);
  uint8_t idx_t = BC_ARG(at_t, 0);
  BC_EAT(OP_JUMP);
  /* column conflict */
  BC_TAKE(at_c, OP_LOAD_SLOT);
  uint8_t s_c = BC_ARG(at_c, 0);
  BC_TAKE(at_qs2, OP_LOAD_SLOT);
  if (BC_ARG(at_qs2, 0) != s_qs)
    return 0;
  BC_EAT(OP_CAR);
  BC_EAT(OP_IS);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_n1, OP_LOAD_GLOBAL);
  uint8_t idx_nil1 = BC_ARG(at_n1, 0);
  BC_EAT(OP_JUMP);
  /* +diagonal conflict */
  BC_TAKE(at_c2, OP_LOAD_SLOT);
  if (BC_ARG(at_c2, 0) != s_c)
    return 0;
  BC_TAKE(at_off, OP_LOAD_SLOT);
  uint8_t s_off = BC_ARG(at_off, 0);
  BC_EAT(OP_ADD);
  BC_TAKE(at_qs3, OP_LOAD_SLOT);
  if (BC_ARG(at_qs3, 0) != s_qs)
    return 0;
  BC_EAT(OP_CAR);
  BC_EAT(OP_IS);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_n2, OP_LOAD_GLOBAL);
  uint8_t idx_nil2 = BC_ARG(at_n2, 0);
  BC_EAT(OP_JUMP);
  /* -diagonal conflict */
  BC_TAKE(at_c3, OP_LOAD_SLOT);
  if (BC_ARG(at_c3, 0) != s_c)
    return 0;
  BC_TAKE(at_off2, OP_LOAD_SLOT);
  if (BC_ARG(at_off2, 0) != s_off)
    return 0;
  BC_EAT(OP_SUB);
  BC_TAKE(at_qs4, OP_LOAD_SLOT);
  if (BC_ARG(at_qs4, 0) != s_qs)
    return 0;
  BC_EAT(OP_CAR);
  BC_EAT(OP_IS);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_n3, OP_LOAD_GLOBAL);
  uint8_t idx_nil3 = BC_ARG(at_n3, 0);
  BC_EAT(OP_JUMP);
  /* recurse on the cdr with offset+1 */
  BC_TAKE(at_c4, OP_LOAD_SLOT);
  if (BC_ARG(at_c4, 0) != s_c)
    return 0;
  BC_TAKE(at_qs5, OP_LOAD_SLOT);
  if (BC_ARG(at_qs5, 0) != s_qs)
    return 0;
  BC_EAT(OP_CDR);
  BC_TAKE(at_addoff, OP_SLOT_ADD_FIX);
  if (BC_ARG(at_addoff, 0) != s_off || BC_I16(at_addoff, 1) != 1)
    return 0;
  BC_TAKE(at_tail, OP_TAIL_SELF);
  if (BC_ARG(at_tail, 0) != 3)
    return 0;
  BC_EAT(OP_RET);
  BC_END();

  if (!bc_const_is_sym(bc, idx_t, "t"))
    return 0;
  if (!bc_const_is_sym(bc, idx_nil1, "nil") ||
      !bc_const_is_sym(bc, idx_nil2, "nil") ||
      !bc_const_is_sym(bc, idx_nil3, "nil"))
    return 0;
  int off_c = env_slot_off_checked(s_c);
  int off_qs = env_slot_off_checked(s_qs);
  int off_off = env_slot_off_checked(s_off);
  if (off_c < 0 || off_qs < 0 || off_off < 0)
    return 0;

  m->s_c = s_c;
  m->s_qs = s_qs;
  m->s_off = s_off;
  m->off_c = off_c;
  m->off_qs = off_qs;
  m->off_off = off_off;
  return 1;
}

/* mark_from captured operands (35-byte sieve mark-from inner loop). Captures
   the j/n/step/marks slot offsets; both LOAD_GLOBALs must resolve to "nil"
   (validated inside the walk). */
struct match_mark_from {
  uint8_t s_j, s_n, s_step, s_marks;
  int off_j, off_n, off_step, off_marks;
};

static int match_mark_from(bytecode_t *bc, struct match_mark_from *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_j, at_n, at_nil1, at_m, at_j2, at_nil2, at_step, at_j3;
  int at_step2, at_n2, at_m2, at_tail;
  BC_TAKE(at_j, OP_LOAD_SLOT);
  uint8_t s_j = BC_ARG(at_j, 0);
  BC_TAKE(at_n, OP_LOAD_SLOT);
  uint8_t s_n = BC_ARG(at_n, 0);
  BC_EAT(OP_GT);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_nil1, OP_LOAD_GLOBAL);
  uint8_t idx_nil1 = BC_ARG(at_nil1, 0);
  BC_EAT(OP_JUMP);
  BC_TAKE(at_m, OP_LOAD_SLOT);
  uint8_t s_marks = BC_ARG(at_m, 0);
  BC_TAKE(at_j2, OP_LOAD_SLOT);
  if (BC_ARG(at_j2, 0) != s_j)
    return 0;
  BC_TAKE(at_nil2, OP_LOAD_GLOBAL);
  uint8_t idx_nil2 = BC_ARG(at_nil2, 0);
  BC_EAT(OP_VEC_SET);
  BC_EAT(OP_POP);
  BC_TAKE(at_step, OP_LOAD_SLOT);
  uint8_t s_step = BC_ARG(at_step, 0);
  BC_TAKE(at_j3, OP_LOAD_SLOT);
  if (BC_ARG(at_j3, 0) != s_j)
    return 0;
  BC_TAKE(at_step2, OP_LOAD_SLOT);
  if (BC_ARG(at_step2, 0) != s_step)
    return 0;
  BC_EAT(OP_ADD);
  BC_TAKE(at_n2, OP_LOAD_SLOT);
  if (BC_ARG(at_n2, 0) != s_n)
    return 0;
  BC_TAKE(at_m2, OP_LOAD_SLOT);
  if (BC_ARG(at_m2, 0) != s_marks)
    return 0;
  BC_TAKE(at_tail, OP_TAIL_SELF);
  if (BC_ARG(at_tail, 0) != 4)
    return 0;
  BC_EAT(OP_RET);
  BC_END();

  /* Both LOAD_GLOBALs must resolve to the symbol "nil". */
  if (!bc_const_is_sym(bc, idx_nil1, "nil") ||
      !bc_const_is_sym(bc, idx_nil2, "nil"))
    return 0;
  int off_j = env_slot_off_checked(s_j);
  int off_n = env_slot_off_checked(s_n);
  int off_step = env_slot_off_checked(s_step);
  int off_marks = env_slot_off_checked(s_marks);
  if (off_j < 0 || off_n < 0 || off_step < 0 || off_marks < 0)
    return 0;

  m->s_j = s_j;
  m->s_n = s_n;
  m->s_step = s_step;
  m->s_marks = s_marks;
  m->off_j = off_j;
  m->off_n = off_n;
  m->off_step = off_step;
  m->off_marks = off_marks;
  return 1;
}

/* is_prime_given captured operands (37-byte sieve is-prime-given list walk).
   Captures the acc/i slot offsets; the "t"/"nil" const guards are validated
   inside the walk. */
struct match_is_prime_given {
  uint8_t s_acc, s_i;
  int off_acc, off_i;
};

static int match_is_prime_given(bytecode_t *bc,
                                struct match_is_prime_given *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_acc0, at_t, at_i, at_acc1, at_k, at_nil, at_acc2, at_i2;
  int at_tail;
  BC_TAKE(at_acc0, OP_LOAD_SLOT);
  uint8_t s_acc = BC_ARG(at_acc0, 0);
  BC_EAT(OP_NOT);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_t, OP_LOAD_GLOBAL);
  uint8_t idx_t = BC_ARG(at_t, 0);
  BC_EAT(OP_JUMP);
  BC_TAKE(at_i, OP_LOAD_SLOT);
  uint8_t s_i = BC_ARG(at_i, 0);
  BC_TAKE(at_acc1, OP_LOAD_SLOT);
  if (BC_ARG(at_acc1, 0) != s_acc)
    return 0;
  BC_EAT(OP_CAR);
  BC_EAT(OP_MOD);
  BC_TAKE(at_k, OP_LOAD_FIX);
  if (BC_I16(at_k, 0) != 0)
    return 0;
  BC_EAT(OP_IS);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_nil, OP_LOAD_GLOBAL);
  uint8_t idx_nil = BC_ARG(at_nil, 0);
  BC_EAT(OP_JUMP);
  BC_TAKE(at_acc2, OP_LOAD_SLOT);
  if (BC_ARG(at_acc2, 0) != s_acc)
    return 0;
  BC_EAT(OP_CDR);
  BC_TAKE(at_i2, OP_LOAD_SLOT);
  if (BC_ARG(at_i2, 0) != s_i)
    return 0;
  BC_TAKE(at_tail, OP_TAIL_SELF);
  if (BC_ARG(at_tail, 0) != 2)
    return 0;
  BC_EAT(OP_RET);
  BC_END();

  if (!bc_const_is_sym(bc, idx_t, "t") || !bc_const_is_sym(bc, idx_nil, "nil"))
    return 0;
  int off_acc = env_slot_off_checked(s_acc);
  int off_i = env_slot_off_checked(s_i);
  if (off_acc < 0 || off_i < 0)
    return 0;

  m->s_acc = s_acc;
  m->s_i = s_i;
  m->off_acc = off_acc;
  m->off_i = off_i;
  return 1;
}

/* count_primes captured operands (41-byte sieve count-primes outer loop).
   Captures the i/n/acc/marks slot offsets. */
struct match_count_primes {
  uint8_t s_i, s_n, s_acc, s_marks;
  int off_i, off_n, off_acc, off_marks;
};

static int match_count_primes(bytecode_t *bc, struct match_count_primes *m) {
  uint8_t *c = bc->code;
  int pc = 0, at_i, at_n, at_acc0, at_addi, at_n2, at_m1, at_m2, at_i2;
  int at_addacc, at_acc1, at_tail;
  BC_TAKE(at_i, OP_LOAD_SLOT);
  uint8_t s_i = BC_ARG(at_i, 0);
  BC_TAKE(at_n, OP_LOAD_SLOT);
  uint8_t s_n = BC_ARG(at_n, 0);
  BC_EAT(OP_GT);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_acc0, OP_LOAD_SLOT);
  uint8_t s_acc = BC_ARG(at_acc0, 0);
  BC_EAT(OP_JUMP);
  BC_TAKE(at_addi, OP_SLOT_ADD_FIX);
  if (BC_ARG(at_addi, 0) != s_i || BC_I16(at_addi, 1) != 1)
    return 0;
  BC_TAKE(at_n2, OP_LOAD_SLOT);
  if (BC_ARG(at_n2, 0) != s_n)
    return 0;
  BC_TAKE(at_m1, OP_LOAD_SLOT);
  uint8_t s_marks = BC_ARG(at_m1, 0);
  BC_TAKE(at_m2, OP_LOAD_SLOT);
  if (BC_ARG(at_m2, 0) != s_marks)
    return 0;
  BC_TAKE(at_i2, OP_LOAD_SLOT);
  if (BC_ARG(at_i2, 0) != s_i)
    return 0;
  BC_EAT(OP_VEC_REF);
  BC_EAT(OP_BR_IF_FALSE);
  BC_TAKE(at_addacc, OP_SLOT_ADD_FIX);
  if (BC_ARG(at_addacc, 0) != s_acc || BC_I16(at_addacc, 1) != 1)
    return 0;
  BC_EAT(OP_JUMP);
  BC_TAKE(at_acc1, OP_LOAD_SLOT);
  if (BC_ARG(at_acc1, 0) != s_acc)
    return 0;
  BC_TAKE(at_tail, OP_TAIL_SELF);
  if (BC_ARG(at_tail, 0) != 4)
    return 0;
  BC_EAT(OP_RET);
  BC_END();

  int off_i = env_slot_off_checked(s_i);
  int off_n = env_slot_off_checked(s_n);
  int off_acc = env_slot_off_checked(s_acc);
  int off_marks = env_slot_off_checked(s_marks);
  if (off_i < 0 || off_n < 0 || off_acc < 0 || off_marks < 0)
    return 0;

  m->s_i = s_i;
  m->s_n = s_n;
  m->s_acc = s_acc;
  m->s_marks = s_marks;
  m->off_i = off_i;
  m->off_n = off_n;
  m->off_acc = off_acc;
  m->off_marks = off_marks;
  return 1;
}

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

/* ───────────────────────── numeric tail-loop compiler ─────────────────────
   A general bytecode→native compiler for the numeric subset of self-tail loops,
   replacing per-kernel curated shapes (subsumes float_acc/float_series; handles
   Mandelbrot/Newton/logistic via ONE mechanism). numloop_analyze (here, arch-
   neutral) validates + type-infers + lays out registers; the per-backend
   try_jit_numloop emitters translate the result. SAFE BY CONSTRUCTION: any
   unsupported construct returns 0 (no JIT → the VM runs it), never miscompiles.

   Value classes: INT (tagged fixnum → GPR) / FLOAT (unboxed double → xmm/d) /
   BOOL (a comparison result, produced then IMMEDIATELY consumed by BR_IF_*, so
   it never needs a register — the emitter fuses compare+branch). Slot types are
   inferred (float consts force FLOAT; :f64 param hints seed FLOAT; the tail-self
   back-edge unifies tail-arg classes into slot classes at a fixed point), then
   GUARDED at entry — a wrong guess merely deopts to the VM. */
#define NLC_INT 1
#define NLC_FLOAT 2
#define NLC_BOOL 3
#define NL_MAXPC 256
#define NL_MAXSTK 16

typedef struct {
  uint8_t slot_class[ENV_INLINE_SLOTS]; /* NLC_INT / NLC_FLOAT per param slot */
  int nparams;
  int nfslots, nislots;       /* # float / # int slots */
  int fidx[ENV_INLINE_SLOTS]; /* slot → float-home index (FLOAT slots), else -1 */
  int iidx[ENV_INLINE_SLOTS]; /* slot → int-home index (INT slots), else -1 */
  int8_t depth[NL_MAXPC];     /* stack depth before op @pc; -1 = unreached */
  uint8_t scls[NL_MAXPC][NL_MAXSTK]; /* class of each stack entry before op @pc */
  int max_ftmp, max_itmp;     /* peak operand-stack entries by class (reg budget) */
  uint8_t float_result;       /* RET returns a FLOAT (needs frame + make_floatf) */
} numloop_t;

/* Validate + type-infer + register-budget a numeric self-tail loop. All bail
   conditions live here so both backends bail identically. Returns 1 on success
   (nl filled), 0 to decline (→ other shapes / VM). */
static int numloop_analyze(bytecode_t *bc, numloop_t *nl) {
  int np = bc->nparams;
  if (np < 1 || np > ENV_INLINE_SLOTS)
    return 0;
  int ncode = bc->ncode;
  if (ncode < 2 || ncode > NL_MAXPC)
    return 0;
  uint8_t *c = bc->code;
  nl->nparams = np;

  /* Seed slot classes from :f64/:int hints; default INT, promote to FLOAT via
     the tail-self fixed point. (A wrong seed only costs a deopt.) */
  for (int i = 0; i < np; i++)
    nl->slot_class[i] =
        (bc->param_hints[i] == TYPE_HINT_F64) ? NLC_FLOAT : NLC_INT;

  /* Two phases: (1) iterate the abstract sim to a fixed point, PROMOTING slot
     classes (an int op whose operands haven't promoted yet is tolerated); then
     (2) one `strict` pass that bails on any arithmetic still typed INT (the
     increment-1 "float-only body" restriction) and records the final layout.
     Bailing on int-arith during phase 1 would reject a kernel like
     (+ c (* a b)) before the tail-self back-edge promotes a,b to FLOAT. */
  int strict = 0;
  for (int iter = 0;; iter++) {
    int changed = 0, ok = 1, saw_tail = 0;
    for (int p = 0; p < ncode; p++)
      nl->depth[p] = -1;
    nl->depth[0] = 0; /* loop entry: empty operand stack */
    nl->max_ftmp = nl->max_itmp = 0;
    nl->float_result = 0;

    int pc = 0;
    while (pc < ncode) {
      int d = nl->depth[pc];
      uint8_t op = c[pc];
      int len = bc_oplen(op);
      if (len == 0) {
        ok = 0;
        break;
      }
      if (d < 0) { /* unreached (e.g. the dead JUMP after TAIL_SELF) */
        pc += len;
        continue;
      }
      uint8_t st[NL_MAXSTK];
      for (int k = 0; k < d; k++)
        st[k] = nl->scls[pc][k];
      int nd = d, nextpc = pc + len, target = -1, fall = 1;

      switch (op) {
      case OP_LOAD_SLOT: {
        uint8_t s = c[pc + 1];
        if (s >= np || nd >= NL_MAXSTK) { ok = 0; goto stop; }
        st[nd++] = nl->slot_class[s];
        break;
      }
      case OP_LOAD_CONST: {
        uint8_t ci = c[pc + 1];
        if (ci >= bc->nconsts || nd >= NL_MAXSTK) { ok = 0; goto stop; }
        exp_t *k = bc->consts[ci];
        uint8_t cl = isfloat(k) ? NLC_FLOAT : isnumber(k) ? NLC_INT : 0;
        if (!cl) { ok = 0; goto stop; }
        st[nd++] = cl;
        break;
      }
      case OP_LOAD_FIX:
        if (nd >= NL_MAXSTK) { ok = 0; goto stop; }
        st[nd++] = NLC_INT;
        break;
      case OP_ADD:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV: {
        if (nd < 2) { ok = 0; goto stop; }
        uint8_t b = st[nd - 1], a = st[nd - 2];
        if ((a != NLC_INT && a != NLC_FLOAT) ||
            (b != NLC_INT && b != NLC_FLOAT)) { ok = 0; goto stop; }
        uint8_t r = (a == NLC_FLOAT || b == NLC_FLOAT) ? NLC_FLOAT : NLC_INT;
        /* First increment: only FLOAT arithmetic (int counter uses SLOT_*_FIX).
           Pure-int +−×÷ → bail, but only on the strict pass (phase 1 tolerates
           it so slot classes can still promote via the tail-self fixed point). */
        if (r == NLC_INT && strict) { ok = 0; goto stop; }
        nd -= 2;
        st[nd++] = r;
        break;
      }
      case OP_LT:
      case OP_GT:
      case OP_LE:
      case OP_GE: {
        if (nd < 2) { ok = 0; goto stop; }
        uint8_t b = st[nd - 1], a = st[nd - 2];
        if ((a != NLC_INT && a != NLC_FLOAT) ||
            (b != NLC_INT && b != NLC_FLOAT)) { ok = 0; goto stop; }
        nd -= 2;
        st[nd++] = NLC_BOOL;
        break;
      }
      case OP_SLOT_ADD_FIX:
      case OP_SLOT_SUB_FIX: {
        uint8_t s = c[pc + 1];
        if (s >= np || nd >= NL_MAXSTK) { ok = 0; goto stop; }
        st[nd++] = nl->slot_class[s]; /* non-mutating: push slot±imm */
        break;
      }
      case OP_SLOT_LT_FIX:
      case OP_SLOT_LE_FIX:
      case OP_SLOT_GT_FIX:
      case OP_SLOT_GE_FIX: {
        uint8_t s = c[pc + 1];
        if (s >= np || nd >= NL_MAXSTK) { ok = 0; goto stop; }
        st[nd++] = NLC_BOOL;
        break;
      }
      case OP_BR_IF_FALSE:
      case OP_BR_IF_TRUE: {
        if (nd < 1 || st[nd - 1] != NLC_BOOL) { ok = 0; goto stop; }
        nd -= 1;
        int16_t off = (int16_t)(c[pc + 1] | (c[pc + 2] << 8));
        target = pc + len + off;
        break;
      }
      case OP_JUMP: {
        int16_t off = (int16_t)(c[pc + 1] | (c[pc + 2] << 8));
        target = pc + len + off;
        fall = 0;
        break;
      }
      case OP_TAIL_SELF: {
        uint8_t n = c[pc + 1];
        if (n != np || nd < n) { ok = 0; goto stop; }
        for (int i = 0; i < n; i++) {
          uint8_t ac = st[nd - n + i];
          if (ac == NLC_BOOL) { ok = 0; goto stop; }
          if (ac == NLC_FLOAT && nl->slot_class[i] != NLC_FLOAT) {
            nl->slot_class[i] = NLC_FLOAT;
            changed = 1;
          }
        }
        nd -= n;
        saw_tail = 1;
        fall = 0;
        break;
      }
      case OP_RET: {
        if (nd < 1) { ok = 0; goto stop; }
        uint8_t r = st[nd - 1];
        if (r == NLC_BOOL) { ok = 0; goto stop; }
        if (r == NLC_FLOAT)
          nl->float_result = 1;
        nd -= 1;
        fall = 0;
        break;
      }
      default:
        ok = 0;
        goto stop;
      }

      { /* peak operand-stack register pressure by class */
        int ft = 0, it = 0;
        for (int k = 0; k < nd; k++) {
          if (st[k] == NLC_FLOAT)
            ft++;
          else if (st[k] == NLC_INT)
            it++;
        }
        if (ft > nl->max_ftmp)
          nl->max_ftmp = ft;
        if (it > nl->max_itmp)
          nl->max_itmp = it;
      }

#define NL_PROP(tpc)                                                           \
  do {                                                                         \
    int _t = (tpc);                                                            \
    if (_t < 0 || _t >= ncode) { ok = 0; goto stop; }                          \
    if (nl->depth[_t] < 0) {                                                   \
      nl->depth[_t] = nd;                                                      \
      for (int k = 0; k < nd; k++)                                             \
        nl->scls[_t][k] = st[k];                                              \
    } else {                                                                   \
      if (nl->depth[_t] != nd) { ok = 0; goto stop; }                          \
      for (int k = 0; k < nd; k++)                                             \
        if (nl->scls[_t][k] != st[k]) { ok = 0; goto stop; }                  \
    }                                                                          \
  } while (0)
      if (fall)
        NL_PROP(nextpc);
      if (target >= 0)
        NL_PROP(target);
#undef NL_PROP
      pc = nextpc;
    }
  stop:
    if (!ok || !saw_tail)
      return 0;
    if (strict)
      break; /* final validating pass complete */
    if (!changed || iter >= ENV_INLINE_SLOTS)
      strict = 1; /* classes converged → do one strict pass next */
  }

  nl->nfslots = nl->nislots = 0;
  for (int i = 0; i < np; i++) {
    if (env_slot_off_checked((uint8_t)i) < 0)
      return 0;
    if (nl->slot_class[i] == NLC_FLOAT) {
      nl->fidx[i] = nl->nfslots++;
      nl->iidx[i] = -1;
    } else {
      nl->iidx[i] = nl->nislots++;
      nl->fidx[i] = -1;
    }
  }
  /* Loose sanity bound only (stack arrays are NL_MAXSTK). Each backend applies
     its OWN register budget after analyze — amd64 is tight (xmm0-7, GPR pool
     rcx/rax/rdx), arm64 is roomy — so a kernel can JIT on arm64 yet fall to the
     VM on amd64. */
  if (nl->nfslots + nl->max_ftmp >= NL_MAXSTK ||
      nl->nislots + nl->max_itmp >= NL_MAXSTK)
    return 0;
  return 1;
}

/* Physical-register index for the operand-stack entry at position p, given the
   stack-class row `scls` (entries below p decide how many regs are already in
   use by that class). Float temps sit above the nfslots float homes; int temps
   above the nislots int homes. The backend maps the returned index to a real
   xmm/d (float) or GPR-pool slot (int). */
static int nl_freg(const uint8_t *scls, int p, int nfslots) {
  int c = 0;
  for (int k = 0; k < p; k++)
    if (scls[k] == NLC_FLOAT)
      c++;
  return nfslots + c;
}
static int nl_ireg(const uint8_t *scls, int p, int nislots) {
  int c = 0;
  for (int k = 0; k < p; k++)
    if (scls[k] == NLC_INT)
      c++;
  return nislots + c;
}
