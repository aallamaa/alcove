/* vector.h — dense numeric vector (EXP_VECTOR) storage + ops: allocation,
 * kind promotion (i64->f64->boxed), element get/set, the tensor bulk ops
 * (vec-dot/axpy/scale/relu/argmax/...), and the growable-window push/pop/
 * shift/unshift. FRAGMENT #included into alcove.c BEFORE the VM/JIT so the
 * static-inline accessors (vec_get_boxed/vec_read_double/...) stay inlined into
 * the tensor loops and JIT (single TU). The vec_*_at/vec_len macros live in
 * alcove.h; the dump/load serializers + the `vector` builtin stay in alcove.c.
 * NOT standalone, NOT separately compiled.
 */

/* VEC_SIMD — placed right before an elementwise F64 loop to tell the compiler
 * "no element of this loop aliases another across iterations, vectorize it."
 * It enables AVX/AVX-512/FMA packing (with -march=native the codegen targets
 * the build host) WITHOUT the `restrict` route, which would be UB for the
 * legal in-place aliasing calls we support (e.g. (vec-axpy! v a v) where the
 * source and destination vector are the SAME object — that is a self-update,
 * not iteration-to-iteration aliasing, and stays correct under ivdep).
 * Portable: empty on toolchains that lack the pragma, so it still builds and
 * runs everywhere. ONLY valid on loops with no loop-carried dependency — do
 * NOT use on reductions (vec-dot, vec-max, vec-argmax), whose accumulator is
 * carried across iterations and must not be reassociated without fast-math. */
#if defined(__clang__)
#  define VEC_SIMD _Pragma("clang loop vectorize(assume_safety)")
#elif defined(__GNUC__)
#  define VEC_SIMD _Pragma("GCC ivdep")
#else
#  define VEC_SIMD
#endif
/* ---------------- Vectors ----------------
   Mutable, O(1) random-access array. Storage layout:
     e->type    = EXP_VECTOR
     e->flags   = ... | vec_kind (bits 4-5: GEN / I64 / F64)
     e->ptr     = alc_vec_t* — 8-byte header {cap, hdr_pad} + cap cells
                  of 8 bytes each (kind determines cell interpretation)
     e->vec_win = {start, end} — live window; len = end - start
   For VEC_KIND_GEN each cell holds an owning exp_t* ref (released by
   the unrefexp walk above). I64/F64 cells are raw scalars — no
   refcount accounting. The slow-sieve trial-division benchmark wins
   from this because we use a sqrt-cutoff on smallest-first vector
   iteration instead of cdr-walking a largest-first cons list.
   alc_vec_t and the vec_* accessors are declared in alcove.h. */

/* Allocate the alc_vec_t storage block with `cap` cells of 8 bytes. The
   caller fills cells / sets exp_t->ptr / sets the window. Returns NULL
   on overflow or alloc failure. memalloc zero-initialises the payload,
   so cells outside the window read as zero (not garbage). */
static alc_vec_t *vec_alloc_storage(int64_t cap) {
  /* cap must fit a positive int32 (it is stored as v->cap). The bound is
     exclusive: cap == 2^31 would cast to INT32_MIN. */
  if (cap < 0 || cap >= ((int64_t)1 << 31))
    return NULL;
  /* Reject counts whose byte product would wrap size_t. On 64-bit this is
     unreachable (cap < 2^31 → bytes < 2^34 << SIZE_MAX), but on wasm32 a
     persisted u32 element count like 0x20000001 passes the cell-count bound
     yet (size_t)cap*8 truncates to a tiny block — the GEN init loop would then
     write hundreds of millions of pointers past it (heap overflow). */
  if ((uint64_t)cap > (SIZE_MAX - sizeof(alc_vec_t)) / 8u)
    return NULL;
  size_t bytes = sizeof(alc_vec_t) + (size_t)cap * 8u;
  alc_vec_t *v = (alc_vec_t *)memalloc(1, bytes);
  if (!v)
    return NULL;
  v->cap = (int32_t)cap;
  v->hdr_pad = 0;
  return v;
}

/* Commit a freshly-built replacement storage block: free the old buffer,
   install new_v as vexp's storage, and set the vec-kind bits to newkind. The
   alloc + element-conversion loop stays inline at each caller (so the typed
   tensor/tighten loops aren't out-of-lined); this is the shared free-old/
   swap-ptr/retag tail of vec_promote_i64_to_f64 / vec_promote_to_gen /
   vec_tighten. `old` is captured by the caller before the conversion (the
   caller still reads from it while filling new_v). */
static inline void vec_swap_storage(exp_t *vexp, alc_vec_t *old,
                                    alc_vec_t *new_v, unsigned newkind) {
  free(old);
  vexp->ptr = new_v;
  vexp->flags =
      (unsigned short)((vexp->flags & ~VEC_KIND_MASK) | newkind);
}

exp_t *make_vector(int64_t n, exp_t *fill) {
  /* Hard cap to keep `(size_t)n * 8` from wrapping. With int61 fixnums n
     can reach 2^60; n*8 wraps modulo SIZE_MAX and hands us a tiny alloc
     that the loop then writes terabytes past. 1<<31 cells (16 GiB at
     8B/cell) is well past any sane vector; anything bigger we refuse
     rather than guess. */
  alc_vec_t *v = vec_alloc_storage(n);
  if (!v)
    return NULL;
  /* Kind inference from fill's type. Numeric fillers get the typed fast
     path; anything else (incl. nil) falls back to GEN. vec-set! later
     auto-promotes to GEN on a type-mismatched write. */
  unsigned kind = VEC_KIND_GEN;
  if (isnumber(fill))
    kind = VEC_KIND_I64;
  else if (isfloat(fill))
    kind = VEC_KIND_F64;

  exp_t *e = make_nil();
  e->type = EXP_VECTOR;
  e->flags = (unsigned short)kind;
  e->ptr = v;
  e->vec_win.start = 0;
  e->vec_win.end = (int32_t)n;
  char *base = (char *)v + sizeof(alc_vec_t);
  if (kind == VEC_KIND_I64) {
    int64_t fix = FIX_VAL(fill);
    int64_t *cells = (int64_t *)base;
    for (int64_t i = 0; i < n; i++)
      cells[i] = fix;
  } else if (kind == VEC_KIND_F64) {
    double f = (double)fill->f;
    double *cells = (double *)base;
    for (int64_t i = 0; i < n; i++)
      cells[i] = f;
  } else {
    exp_t **cells = (exp_t **)base;
    for (int64_t i = 0; i < n; i++)
      cells[i] = refexp(fill);
  }
  return e;
}

/* Promote an I64 vec to F64 in place. Converts every cell (live window
   only; cells outside [start, end) are uninitialized garbage and stay
   that way). Tensor mutating ops call this when they need to store
   non-integer results into a vec that was constructed integer-typed —
   matches the pre-refactor behavior where `(vec 4 0)` then `(vec-scale!
   v 0.5)` ended up holding floats. Single-shard only; asserts
   FLAG_SHARED clear. Returns 1 on success. */
static int vec_promote_i64_to_f64(exp_t *vexp) {
  if (vec_kind(vexp) == VEC_KIND_F64)
    return 1;
  if (vec_kind(vexp) != VEC_KIND_I64)
    return 0;
  if (vexp->flags & FLAG_SHARED)
    return 0;
  alc_vec_t *old = (alc_vec_t *)vexp->ptr;
  alc_vec_t *new_v = vec_alloc_storage(old->cap);
  if (!new_v)
    return 0;
  int64_t *src = (int64_t *)((char *)old + sizeof(alc_vec_t));
  double *dst = (double *)((char *)new_v + sizeof(alc_vec_t));
  int32_t s = vexp->vec_win.start;
  int32_t en = vexp->vec_win.end;
  for (int32_t i = s; i < en; i++)
    dst[i] = (double)src[i];
  vec_swap_storage(vexp, old, new_v, VEC_KIND_F64);
  return 1;
}

/* Promote a typed (I64 or F64) vec to GEN in place. Boxes every live
   cell into a fresh heap exp_t. Single-shard only — callers must check
   FLAG_SHARED is clear (a shared typed vec must not realloc its
   payload). Returns 1 on success, 0 on alloc failure or shared-vec.

   Cells outside the live window [start, end) in the new buffer are left
   uninitialized — the gen-walk in unrefexp only touches indices in the
   window via vec_gen_at, so garbage outside is invisible. */
static int vec_promote_to_gen(exp_t *vexp) {
  if (vec_kind(vexp) == VEC_KIND_GEN)
    return 1;
  if (vexp->flags & FLAG_SHARED)
    return 0;

  alc_vec_t *old = (alc_vec_t *)vexp->ptr;
  int32_t start = vexp->vec_win.start;
  int64_t live = vexp->vec_win.end - start;
  alc_vec_t *new_v = vec_alloc_storage(old->cap);
  if (!new_v)
    return 0;
  exp_t **dst = (exp_t **)((char *)new_v + sizeof(alc_vec_t));
  char *oldbase = (char *)old + sizeof(alc_vec_t);
  if (vec_kind(vexp) == VEC_KIND_I64) {
    int64_t *src = (int64_t *)oldbase;
    for (int64_t i = 0; i < live; i++)
      dst[start + i] = MAKE_FIX(src[start + i]); /* tagged immediate */
  } else {                                       /* VEC_KIND_F64 */
    double *src = (double *)oldbase;
    for (int64_t i = 0; i < live; i++)
      dst[start + i] = make_floatf((expfloat)src[start + i]); /* fresh nref=1 */
  }
  vec_swap_storage(vexp, old, new_v, VEC_KIND_GEN);
  return 1;
}

/* Boxed read: returns an exp_t* (refcount-bumped or fresh) at index i,
   regardless of vec kind. Caller has bounds-checked. The boxed value
   for I64 is a tagged immediate (no refcount); for F64 it's a fresh
   EXP_FLOAT with nref=1; for GEN it's refexp() of the existing slot. */
static inline exp_t *vec_get_boxed(exp_t *vexp, int64_t i) {
  char *base = (char *)vexp->ptr + sizeof(alc_vec_t);
  int32_t off = vexp->vec_win.start + (int32_t)i;
  switch (vec_kind(vexp)) {
  case VEC_KIND_GEN:
    return refexp(((exp_t **)base)[off]);
  case VEC_KIND_I64:
    return MAKE_FIX(((int64_t *)base)[off]);
  case VEC_KIND_F64:
    return make_floatf((expfloat)((double *)base)[off]);
  }
  return NIL_EXP; /* unreachable — vec_kind only returns one of the three */
}

/* Boxed write: stores val into index i. Caller transferred its ref. On
   a kind match, val's ref is either kept (GEN) or released (I64/F64
   extract the scalar and drop the box). On a kind mismatch, the vec
   promotes to GEN first then retries. Returns 0 on alloc failure
   (promotion failed); the caller's ref to val is released either way.

   Quietly accepts a fixnum into an F64 vec (converts to double) — this
   matches the existing vec_elem_set_double semantics that the MLP
   relies on for scalar inits like `(vec-fill! v 0)`. */
static int vec_set_boxed(exp_t *vexp, int64_t i, exp_t *val) {
  char *base = (char *)vexp->ptr + sizeof(alc_vec_t);
  int32_t off = vexp->vec_win.start + (int32_t)i;
  unsigned k = vec_kind(vexp);
  if (k == VEC_KIND_GEN) {
    unrefexp(((exp_t **)base)[off]);
    ((exp_t **)base)[off] = val; /* ownership transferred */
    return 1;
  }
  if (k == VEC_KIND_I64 && isnumber(val)) {
    ((int64_t *)base)[off] = FIX_VAL(val);
    /* val is a tagged immediate — no ref to release. */
    return 1;
  }
  if (k == VEC_KIND_F64) {
    if (isfloat(val)) {
      ((double *)base)[off] = (double)val->f;
      unrefexp(val);
      return 1;
    }
    if (isnumber(val)) {
      ((double *)base)[off] = (double)FIX_VAL(val);
      return 1; /* tagged immediate, no unref */
    }
  }
  /* Kind mismatch — promote to GEN then retry. */
  if (!vec_promote_to_gen(vexp)) {
    unrefexp(val);
    return 0;
  }
  /* After promotion vexp is GEN; recurse handles the GEN store. */
  return vec_set_boxed(vexp, i, val);
}

/* Read element i as a double, regardless of kind. Sets *err on a GEN
   non-numeric element. Caller has bounds-checked. */
static inline double vec_read_double(exp_t *vexp, int64_t i, int *err) {
  char *base = (char *)vexp->ptr + sizeof(alc_vec_t);
  int32_t off = vexp->vec_win.start + (int32_t)i;
  switch (vec_kind(vexp)) {
  case VEC_KIND_F64:
    return ((double *)base)[off];
  case VEC_KIND_I64:
    return (double)((int64_t *)base)[off];
  case VEC_KIND_GEN: {
    exp_t *e = ((exp_t **)base)[off];
    if (isnumber(e))
      return (double)FIX_VAL(e);
    if (isfloat(e))
      return e->f;
    *err = 1;
    return 0.0;
  }
  }
  *err = 1;
  return 0.0;
}

/* Write a double into element i. If the vec is F64, raw store. I64
   truncates to int64. GEN goes through the in-place EXP_FLOAT fast path
   (preserved from the old vec_elem_set_double). Caller has bounds-
   checked. Returns 0 on failure (e.g., GEN promotion not possible). */
static inline int vec_write_double(exp_t *vexp, int64_t i, double x) {
  char *base = (char *)vexp->ptr + sizeof(alc_vec_t);
  int32_t off = vexp->vec_win.start + (int32_t)i;
  switch (vec_kind(vexp)) {
  case VEC_KIND_F64:
    ((double *)base)[off] = x;
    return 1;
  case VEC_KIND_I64:
    ((int64_t *)base)[off] = (int64_t)x;
    return 1;
  case VEC_KIND_GEN: {
    exp_t **cells = (exp_t **)base;
    exp_t *cur = cells[off];
    if (is_ptr(cur) && cur->type == EXP_FLOAT && cur->nref == 1 &&
        !(cur->flags & FLAG_SHARED)) {
      cur->f = (expfloat)x;
      return 1;
    }
    unrefexp(cur);
    cells[off] = make_floatf((expfloat)x);
    return 1;
  }
  }
  return 0;
}

/* Inspect a freshly-built GEN vec; if all cells are homogeneous numeric,
   replace its storage with a typed I64 or F64 buffer in place. Used by
   vectorcmd / the #[...] reader to pick the tightest kind for literals.
   Asserts start==0 (always true for fresh vectorcmd output). Returns 1
   if the vec was tightened, 0 if it stays GEN. */
static int vec_tighten(exp_t *vexp) {
  if (vec_kind(vexp) != VEC_KIND_GEN)
    return 1;
  int64_t n = vec_len(vexp);
  if (n == 0)
    return 0; /* nothing to infer from */
  exp_t **cells = vec_gen_cells(vexp);
  int all_fix = 1, all_num = 1;
  for (int64_t i = 0; i < n; i++) {
    exp_t *c = cells[i];
    if (!isnumber(c))
      all_fix = 0;
    if (!isnumber(c) && !isfloat(c)) {
      all_num = 0;
      break;
    }
  }
  if (!all_num)
    return 0;
  alc_vec_t *old = (alc_vec_t *)vexp->ptr;
  alc_vec_t *new_v = vec_alloc_storage(old->cap);
  if (!new_v)
    return 0;
  char *newbase = (char *)new_v + sizeof(alc_vec_t);
  unsigned newkind;
  if (all_fix) {
    int64_t *dst = (int64_t *)newbase;
    for (int64_t i = 0; i < n; i++)
      dst[i] = FIX_VAL(cells[i]); /* tagged immediates, no unref */
    newkind = VEC_KIND_I64;
  } else {
    double *dst = (double *)newbase;
    for (int64_t i = 0; i < n; i++) {
      exp_t *c = cells[i];
      dst[i] = isfloat(c) ? (double)c->f : (double)FIX_VAL(c);
      unrefexp(c); /* drop the box we no longer need */
    }
    newkind = VEC_KIND_F64;
  }
  vec_swap_storage(vexp, old, new_v, newkind);
  vexp->vec_win.start = 0;
  vexp->vec_win.end = (int32_t)n;
  return 1;
}

/* Tensor ops produce floats; an I64-kind output vec must first promote to F64
   so the result holds the math exactly (matches the pre-refactor per-cell
   EXP_FLOAT boxing). Promotion fails on a FLAG_SHARED vec — run `cleanup`
   (the call site's CLEAN_RETURN_n with the right arity, given the prebuilt
   error expr `_alc_e`) rather than silently truncating. `name` is the op name
   used in the error message. */
#define VEC_REQUIRE_FLOAT_WRITABLE(vexp, name, cleanup)                        \
  do {                                                                         \
    if (vec_kind(vexp) == VEC_KIND_I64 && !vec_promote_i64_to_f64(vexp)) {     \
      exp_t *_alc_e = error(ERROR_ILLEGAL_VALUE, e, env,                       \
                            name ": cannot promote shared I64 vec");           \
      cleanup;                                                                 \
    }                                                                          \
  } while (0)

const char doc_vec[] = "(vec n init) — fixed-size vector of n cells "
                       "initialised to init. (vec n) defaults init to nil.";
exp_t *veccmd(exp_t *e, env_t *env) {
  exp_t *fill = NIL_EXP;
  EVAL_ARG_1(nexp);
  if (e->next && e->next->next)
    fill = EVAL(e->next->next->content, env);
  if (iserror(fill))
    CLEAN_RETURN_1(nexp, fill);
  if (!isnumber(nexp))
    CLEAN_RETURN_2(nexp, fill,
                   error(ERROR_NUMBER_EXPECTED, e, env,
                         "(vec n init): n must be a number"));
  int64_t n = FIX_VAL(nexp);
  if (n < 0)
    n = 0;
  unrefexp(nexp);
  exp_t *ret = make_vector(n, fill);
  unrefexp(fill);
  if (!ret) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "(vec n ...): n is too large or alloc failed");
  }
  unrefexp(e);
  return ret;
}

const char doc_vecref[] =
    "(vec-ref v i) — read element i of vector v (O(1)). 0-based index.";
exp_t *vecrefcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(vexp, iexp);

  if (!isvector(vexp) || !(isnumber(iexp) || isfloat(iexp)))
    CLEAN_RETURN_2(
        vexp, iexp,
        error(ERROR_ILLEGAL_VALUE, e, env, "(vec-ref v i): bad args"));

  int64_t i = isnumber(iexp) ? FIX_VAL(iexp)
                             : (int64_t)iexp->f; /* float idx truncates */
  if (i < 0 || i >= vec_len(vexp))
    CLEAN_RETURN_2(
        vexp, iexp,
        error(ERROR_INDEX_OUT_OF_RANGE, e, env, "vec-ref: index out of range"));

  exp_t *ret = vec_get_boxed(vexp, i);
  CLEAN_RETURN_2(vexp, iexp, ret);
}

const char doc_vecset[] =
    "(vec-set! v i x) — store x into element i of vector v. Returns x.";
exp_t *vecsetcmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(vexp, iexp, valexp);

  if (!isvector(vexp) || !(isnumber(iexp) || isfloat(iexp)))
    CLEAN_RETURN_3(
        vexp, iexp, valexp,
        error(ERROR_ILLEGAL_VALUE, e, env, "(vec-set! v i val): bad args"));

  int64_t i = isnumber(iexp) ? FIX_VAL(iexp)
                             : (int64_t)iexp->f; /* float idx truncates */
  if (i < 0 || i >= vec_len(vexp))
    CLEAN_RETURN_3(vexp, iexp, valexp,
                   error(ERROR_INDEX_OUT_OF_RANGE, e, env,
                         "vec-set!: index out of range"));

  /* Refcount the return value before vec_set_boxed eats valexp's ref. */
  exp_t *ret = refexp(valexp);
  if (!vec_set_boxed(vexp, i, valexp)) {
    unrefexp(ret);
    CLEAN_RETURN_2(vexp, iexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-set!: alloc failure or shared vec promote"));
  }
  /* valexp ownership consumed by vec_set_boxed — don't clean it. */
  CLEAN_RETURN_2(vexp, iexp, ret);
}

const char doc_veclen[] = "(vec-len v) — number of cells in vector v.";
exp_t *veclencmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(
        vexp, error(ERROR_ILLEGAL_VALUE, e, env, "(vec-len v): not a vector"));
  int64_t n = vec_len(vexp);
  CLEAN_RETURN_1(vexp, MAKE_FIX(n));
}

/* ---------- tensor bulk ops ----------
   These take vec-of-floats (or vec-of-fixnums; we coerce) and operate
   on the underlying doubles, eliminating per-element boxing. The
   interpreter would otherwise allocate an EXP_FLOAT per multiply and
   per add — these ops collapse the hot inner loops of an MLP forward/
   backward pass into a single C-level walk.

   Coercion rule: each element must be a number (float or fixnum); any
   other type errors. The mutating ops (-set! / -axpy! / -scale! / -add!
   / -fill! / -relu!) replace each output slot with a fresh EXP_FLOAT
   (we don't try to reuse the old slot's exp_t in place — refcount
   tracking would be racy and the alloc savings are marginal compared
   to the multiply count). */

/* Resolve the live cell array for a VEC_KIND_GEN vec — pointer to the
   first valid cell (accounting for vec_win.start). Cached once outside
   the hot loop so the compiler doesn't reload vexp->ptr / vec_win.start
   on every iteration (unrefexp / make_floatf are non-aliasing in
   practice, but the compiler can't prove it). */
static inline exp_t **vec_gen_cells(exp_t *vexp) {
  return (exp_t **)((char *)vexp->ptr + sizeof(alc_vec_t)) +
         vexp->vec_win.start;
}

/* Read cell[i] (within the resolved cell view) as a double. Returns
   0.0 + sets *err on a non-numeric GEN element. Used by the GEN-GEN
   fast path in tensor ops; typed kinds skip this and read raw. */
static inline double gen_cell_as_double(exp_t **cells, int64_t i, int *err) {
  exp_t *e = cells[i];
  if (isnumber(e))
    return (double)FIX_VAL(e);
  if (isfloat(e))
    return e->f;
  *err = 1;
  return 0.0;
}

const char doc_vecdot[] =
    "(vec-dot a b) — sum of a[i]*b[i] over both vectors (must be equal "
    "length, numeric elements). Returns a float.";
exp_t *vecdotcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(aexp, bexp);
  if (!isvector(aexp) || !isvector(bexp))
    CLEAN_RETURN_2(aexp, bexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-dot a b): both args must be vectors"));
  int64_t na = vec_len(aexp), nb = vec_len(bexp);
  if (na != nb)
    CLEAN_RETURN_2(aexp, bexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-dot: length mismatch (%lld vs %lld)",
                         (long long)na, (long long)nb));
  int err = 0;
  double s = 0.0;
  unsigned ka = vec_kind(aexp), kb = vec_kind(bexp);
  /* Hoisted fast paths cover the practical cases:
       F64-F64: raw double dot product (MLP hot path);
       GEN-GEN: hoisted exp_t* cells + boxed read (heterogeneous vecs
                like sieve-fast or untyped data);
       I64-F64 / F64-I64: integer→double convert on the typed side.
     Anything else (GEN mixed with typed, etc.) goes through the per-
     element kind switch — same correctness, slower. */
  if (ka == VEC_KIND_F64 && kb == VEC_KIND_F64) {
    double *acells = VEC_F64_CELLS(aexp);
    double *bcells = VEC_F64_CELLS(bexp);
    for (int64_t i = 0; i < na; i++)
      s += acells[i] * bcells[i];
  } else if (ka == VEC_KIND_GEN && kb == VEC_KIND_GEN) {
    exp_t **acells = vec_gen_cells(aexp);
    exp_t **bcells = vec_gen_cells(bexp);
    for (int64_t i = 0; i < na; i++)
      s += gen_cell_as_double(acells, i, &err) *
           gen_cell_as_double(bcells, i, &err);
  } else if (ka == VEC_KIND_I64 && kb == VEC_KIND_F64) {
    int64_t *acells = (int64_t *)((char *)aexp->ptr + sizeof(alc_vec_t)) +
                      aexp->vec_win.start;
    double *bcells = VEC_F64_CELLS(bexp);
    for (int64_t i = 0; i < na; i++)
      s += (double)acells[i] * bcells[i];
  } else if (ka == VEC_KIND_F64 && kb == VEC_KIND_I64) {
    double *acells = VEC_F64_CELLS(aexp);
    int64_t *bcells = (int64_t *)((char *)bexp->ptr + sizeof(alc_vec_t)) +
                      bexp->vec_win.start;
    for (int64_t i = 0; i < na; i++)
      s += acells[i] * (double)bcells[i];
  } else {
    /* GEN mixed with typed, or I64-I64 — kind-uniform fallback. */
    for (int64_t i = 0; i < na; i++)
      s += vec_read_double(aexp, i, &err) * vec_read_double(bexp, i, &err);
  }
  if (err)
    CLEAN_RETURN_2(
        aexp, bexp,
        error(ERROR_NUMBER_EXPECTED, e, env, "vec-dot: non-numeric element"));
  exp_t *ret = make_floatf((expfloat)s);
  CLEAN_RETURN_2(aexp, bexp, ret);
}

const char doc_vecaxpy[] =
    "(vec-axpy! y a x) — in place y[i] += a * x[i]. Returns y.";
exp_t *vecaxpycmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(yexp, aexp, xexp);
  if (!isvector(yexp) || !isvector(xexp) || !(isnumber(aexp) || isfloat(aexp)))
    CLEAN_RETURN_3(yexp, aexp, xexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-axpy! y a x): y, x vecs and a scalar"));
  int64_t ny = vec_len(yexp);
  if (ny != vec_len(xexp))
    CLEAN_RETURN_3(
        yexp, aexp, xexp,
        error(ERROR_ILLEGAL_VALUE, e, env, "vec-axpy!: length mismatch"));
  double a = isfloat(aexp) ? aexp->f : (double)FIX_VAL(aexp);
  VEC_REQUIRE_FLOAT_WRITABLE(yexp, "vec-axpy!",
                             CLEAN_RETURN_3(yexp, aexp, xexp, _alc_e));
  int err = 0;
  if (vec_kind(yexp) == VEC_KIND_F64 && vec_kind(xexp) == VEC_KIND_F64) {
    double *ycells = VEC_F64_CELLS(yexp);
    double *xcells = VEC_F64_CELLS(xexp);
    VEC_SIMD
    for (int64_t i = 0; i < ny; i++)
      ycells[i] += a * xcells[i];
  } else {
    for (int64_t i = 0; i < ny; i++) {
      double yv = vec_read_double(yexp, i, &err);
      double xv = vec_read_double(xexp, i, &err);
      if (err)
        break;
      vec_write_double(yexp, i, yv + a * xv);
    }
  }
  if (err)
    CLEAN_RETURN_3(
        yexp, aexp, xexp,
        error(ERROR_NUMBER_EXPECTED, e, env, "vec-axpy!: non-numeric element"));
  exp_t *ret = refexp(yexp);
  CLEAN_RETURN_3(yexp, aexp, xexp, ret);
}

const char doc_vecscale[] = "(vec-scale! v a) — in place v[i] *= a. Returns v.";
exp_t *vecscalecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(vexp, aexp);
  if (!isvector(vexp) || !(isnumber(aexp) || isfloat(aexp)))
    CLEAN_RETURN_2(
        vexp, aexp,
        error(ERROR_ILLEGAL_VALUE, e, env, "(vec-scale! v a): vec + scalar"));
  double a = isfloat(aexp) ? aexp->f : (double)FIX_VAL(aexp);
  VEC_REQUIRE_FLOAT_WRITABLE(vexp, "vec-scale!",
                             CLEAN_RETURN_2(vexp, aexp, _alc_e));
  int err = 0;
  int64_t n = vec_len(vexp);
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells = VEC_F64_CELLS(vexp);
    VEC_SIMD
    for (int64_t i = 0; i < n; i++)
      cells[i] *= a;
  } else {
    for (int64_t i = 0; i < n; i++) {
      double vi = vec_read_double(vexp, i, &err);
      if (err)
        break;
      vec_write_double(vexp, i, vi * a);
    }
  }
  if (err)
    CLEAN_RETURN_2(vexp, aexp,
                   error(ERROR_NUMBER_EXPECTED, e, env,
                         "vec-scale!: non-numeric element"));
  exp_t *ret = refexp(vexp);
  CLEAN_RETURN_2(vexp, aexp, ret);
}

const char doc_veccopy[] =
    "(vec-copy! dst src) — overwrite dst with elements from src (must be "
    "equal length). Returns dst.";
exp_t *veccopycmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(dexp, sexp);
  if (!isvector(dexp) || !isvector(sexp))
    CLEAN_RETURN_2(dexp, sexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-copy! dst src): both must be vectors"));
  int64_t n = vec_len(dexp);
  if (n != vec_len(sexp))
    CLEAN_RETURN_2(
        dexp, sexp,
        error(ERROR_ILLEGAL_VALUE, e, env, "vec-copy!: length mismatch"));
  unsigned kd = vec_kind(dexp), ks = vec_kind(sexp);
  if (kd == ks && kd != VEC_KIND_GEN) {
    /* Same typed kind — raw memcpy of cells (8 bytes each, all kinds). */
    char *dst =
        (char *)dexp->ptr + sizeof(alc_vec_t) + 8u * dexp->vec_win.start;
    char *src =
        (char *)sexp->ptr + sizeof(alc_vec_t) + 8u * sexp->vec_win.start;
    memcpy(dst, src, (size_t)n * 8u);
  } else if (kd == VEC_KIND_GEN && ks == VEC_KIND_GEN) {
    exp_t **dcells = vec_gen_cells(dexp);
    exp_t **scells = vec_gen_cells(sexp);
    for (int64_t i = 0; i < n; i++) {
      unrefexp(dcells[i]);
      dcells[i] = refexp(scells[i]);
    }
  } else {
    /* Mixed kinds — fall back to box/unbox per element. */
    for (int64_t i = 0; i < n; i++) {
      exp_t *boxed = vec_get_boxed(sexp, i);
      if (!vec_set_boxed(dexp, i, boxed))
        CLEAN_RETURN_2(
            dexp, sexp,
            error(ERROR_ILLEGAL_VALUE, e, env, "vec-copy!: alloc failure"));
    }
  }
  exp_t *ret = refexp(dexp);
  CLEAN_RETURN_2(dexp, sexp, ret);
}

/* Elementwise in-place binary op y[i] OP= x[i] over two equal-length vectors.
   The F64/F64 fast path uses the native ASSIGN_OP (SIMD-friendly); the generic
   path reads/writes doubles with BIN_OP. The binary sibling of VEC_ACTIVATION. */
#define VEC_BINOP_INPLACE(cmdname, docname, ASSIGN_OP, BIN_OP)                  \
  exp_t *cmdname(exp_t *e, env_t *env) {                                       \
    EVAL_ARG_2(yexp, xexp);                                                    \
    if (!isvector(yexp) || !isvector(xexp))                                    \
      CLEAN_RETURN_2(yexp, xexp,                                               \
                     error(ERROR_ILLEGAL_VALUE, e, env,                        \
                           "(" docname " y x): both must be vectors"));        \
    int64_t ny = vec_len(yexp);                                               \
    if (ny != vec_len(xexp))                                                   \
      CLEAN_RETURN_2(yexp, xexp,                                               \
                     error(ERROR_ILLEGAL_VALUE, e, env,                        \
                           docname ": length mismatch"));                      \
    VEC_REQUIRE_FLOAT_WRITABLE(yexp, docname,                                  \
                               CLEAN_RETURN_2(yexp, xexp, _alc_e));            \
    int err = 0;                                                              \
    if (vec_kind(yexp) == VEC_KIND_F64 && vec_kind(xexp) == VEC_KIND_F64) {   \
      double *ycells = VEC_F64_CELLS(yexp);                                   \
      double *xcells = VEC_F64_CELLS(xexp);                                   \
      VEC_SIMD                                                                \
      for (int64_t i = 0; i < ny; i++)                                        \
        ycells[i] ASSIGN_OP xcells[i];                                        \
    } else {                                                                  \
      for (int64_t i = 0; i < ny; i++) {                                      \
        double yv = vec_read_double(yexp, i, &err);                           \
        double xv = vec_read_double(xexp, i, &err);                           \
        if (err)                                                              \
          break;                                                             \
        vec_write_double(yexp, i, yv BIN_OP xv);                              \
      }                                                                       \
    }                                                                         \
    if (err)                                                                  \
      CLEAN_RETURN_2(yexp, xexp,                                              \
                     error(ERROR_NUMBER_EXPECTED, e, env,                     \
                           docname ": non-numeric element"));                 \
    exp_t *ret = refexp(yexp);                                                \
    CLEAN_RETURN_2(yexp, xexp, ret);                                          \
  }

const char doc_vecadd[] = "(vec-add! y x) — in place y[i] += x[i]. Returns y.";
VEC_BINOP_INPLACE(vecaddcmd, "vec-add!", +=, +)

/* Data-parallel masked counter: count[i] += (src[i] <= limit) ? 1 : 0, in one
   SIMD pass. The building block for vectorized escape-time fractals — one call
   advances the iteration count of every still-bounded pixel at once. A NaN src
   (an escaped point whose z overflowed) compares false, so it stops counting,
   exactly matching a scalar escape loop. */
const char doc_veccountle[] =
    "(vec-count-le! count src limit) — in place count[i] += (src[i] <= limit ? 1 "
    ": 0), one SIMD pass (NaN src counts as 0). Returns count.";
exp_t *veccountlecmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(cexp, sexp, limexp);
  if (!isvector(cexp) || !isvector(sexp))
    CLEAN_RETURN_3(cexp, sexp, limexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-count-le! count src limit): count, src vectors"));
  if (!isnumber(limexp) && !isfloat(limexp))
    CLEAN_RETURN_3(cexp, sexp, limexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-count-le!: limit must be a number"));
  int64_t n = vec_len(cexp);
  if (n != vec_len(sexp))
    CLEAN_RETURN_3(cexp, sexp, limexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-count-le!: length mismatch"));
  double lim = isfloat(limexp) ? limexp->f : (double)FIX_VAL(limexp);
  VEC_REQUIRE_FLOAT_WRITABLE(cexp, "vec-count-le!",
                             CLEAN_RETURN_3(cexp, sexp, limexp, _alc_e));
  int err = 0;
  if (vec_kind(cexp) == VEC_KIND_F64 && vec_kind(sexp) == VEC_KIND_F64) {
    double *cc = VEC_F64_CELLS(cexp);
    double *sc = VEC_F64_CELLS(sexp);
    VEC_SIMD
    for (int64_t i = 0; i < n; i++)
      cc[i] += (sc[i] <= lim) ? 1.0 : 0.0;
  } else {
    for (int64_t i = 0; i < n; i++) {
      double cv = vec_read_double(cexp, i, &err);
      double sv = vec_read_double(sexp, i, &err);
      if (err)
        break;
      vec_write_double(cexp, i, cv + ((sv <= lim) ? 1.0 : 0.0));
    }
  }
  if (err)
    CLEAN_RETURN_3(
        cexp, sexp, limexp,
        error(ERROR_NUMBER_EXPECTED, e, env, "vec-count-le!: non-numeric"));
  exp_t *ret = refexp(cexp);
  CLEAN_RETURN_3(cexp, sexp, limexp, ret);
}

const char doc_vecfill[] = "(vec-fill! v a) — in place v[i] = a. Returns v.";
exp_t *vecfillcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(vexp, aexp);
  if (!isvector(vexp) || !(isnumber(aexp) || isfloat(aexp)))
    CLEAN_RETURN_2(
        vexp, aexp,
        error(ERROR_ILLEGAL_VALUE, e, env, "(vec-fill! v a): vec + scalar"));
  double a = isfloat(aexp) ? aexp->f : (double)FIX_VAL(aexp);
  VEC_REQUIRE_FLOAT_WRITABLE(vexp, "vec-fill!",
                             CLEAN_RETURN_2(vexp, aexp, _alc_e));
  int64_t n = vec_len(vexp);
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells = VEC_F64_CELLS(vexp);
    for (int64_t i = 0; i < n; i++)
      cells[i] = a;
  } else {
    for (int64_t i = 0; i < n; i++)
      vec_write_double(vexp, i, a);
  }
  exp_t *ret = refexp(vexp);
  CLEAN_RETURN_2(vexp, aexp, ret);
}

const char doc_vecrelu[] =
    "(vec-relu! v) — in place v[i] = max(0, v[i]). Returns v.";
exp_t *vecrelucmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(vexp, error(ERROR_ILLEGAL_VALUE, e, env,
                               "(vec-relu! v): not a vector"));
  VEC_REQUIRE_FLOAT_WRITABLE(vexp, "vec-relu!", CLEAN_RETURN_1(vexp, _alc_e));
  int err = 0;
  int64_t n = vec_len(vexp);
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells = VEC_F64_CELLS(vexp);
    VEC_SIMD
    for (int64_t i = 0; i < n; i++)
      if (cells[i] < 0.0)
        cells[i] = 0.0;
  } else {
    for (int64_t i = 0; i < n; i++) {
      double vi = vec_read_double(vexp, i, &err);
      if (err)
        break;
      if (vi < 0.0)
        vec_write_double(vexp, i, 0.0);
    }
  }
  if (err)
    CLEAN_RETURN_1(vexp, error(ERROR_NUMBER_EXPECTED, e, env,
                               "vec-relu!: non-numeric element"));
  exp_t *ret = refexp(vexp);
  CLEAN_RETURN_1(vexp, ret);
}

const char doc_vecargmax[] =
    "(vec-argmax v) — index of the largest element. Empty vec -> -1.";
exp_t *vecargmaxcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(vexp, error(ERROR_ILLEGAL_VALUE, e, env,
                               "(vec-argmax v): not a vector"));
  int64_t best = -1;
  double bestv = 0.0;
  int err = 0;
  int64_t n = vec_len(vexp);
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells = VEC_F64_CELLS(vexp);
    for (int64_t i = 0; i < n; i++)
      if (best < 0 || cells[i] > bestv) {
        best = i;
        bestv = cells[i];
      }
  } else {
    for (int64_t i = 0; i < n; i++) {
      double x = vec_read_double(vexp, i, &err);
      if (err)
        break;
      if (best < 0 || x > bestv) {
        best = i;
        bestv = x;
      }
    }
  }
  if (err)
    CLEAN_RETURN_1(vexp, error(ERROR_NUMBER_EXPECTED, e, env,
                               "vec-argmax: non-numeric element"));
  CLEAN_RETURN_1(vexp, MAKE_FIX(best));
}

const char doc_vecmax[] =
    "(vec-max v) — largest element as a float. Empty vec is an error.";
exp_t *vecmaxcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(
        vexp, error(ERROR_ILLEGAL_VALUE, e, env, "(vec-max v): not a vector"));
  int64_t n = vec_len(vexp);
  if (n == 0)
    CLEAN_RETURN_1(vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env, "vec-max: empty vector"));
  int err = 0;
  double m;
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells = VEC_F64_CELLS(vexp);
    m = cells[0];
    for (int64_t i = 1; i < n; i++)
      if (cells[i] > m)
        m = cells[i];
  } else {
    m = vec_read_double(vexp, 0, &err);
    for (int64_t i = 1; i < n; i++) {
      double x = vec_read_double(vexp, i, &err);
      if (err)
        break;
      if (x > m)
        m = x;
    }
  }
  if (err)
    CLEAN_RETURN_1(vexp, error(ERROR_NUMBER_EXPECTED, e, env,
                               "vec-max: non-numeric element"));
  CLEAN_RETURN_1(vexp, make_floatf((expfloat)m));
}

/* ---------- numeric / NN tensor ops ----------
   Elementwise in-place (mul!/sub!), elementwise activations (exp!/sigmoid!/
   tanh!/softmax!), and reductions (sum/min/argmin). Each takes the F64
   fast-path (raw double loop, SIMD-vectorised where there's no transcendental
   call or loop-carried dependency) and a generic vec_read_double/write_double
   fallback for I64/GEN vectors. The in-place ops promote the target to F64 via
   VEC_REQUIRE_FLOAT_WRITABLE; vec-min mirrors vec-max, vec-argmin vec-argmax. */

const char doc_vecmul[] = "(vec-mul! y x) — in place y[i] *= x[i] (Hadamard "
                          "product). Returns y.";
VEC_BINOP_INPLACE(vecmulcmd, "vec-mul!", *=, *)

const char doc_vecsub[] = "(vec-sub! y x) — in place y[i] -= x[i]. Returns y.";
VEC_BINOP_INPLACE(vecsubcmd, "vec-sub!", -=, -)

const char doc_vecsum[] =
    "(vec-sum v) — sum of all elements, as a float. Empty vec -> 0.";
exp_t *vecsumcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(
        vexp, error(ERROR_ILLEGAL_VALUE, e, env, "(vec-sum v): not a vector"));
  int err = 0;
  double s = 0.0;
  int64_t n = vec_len(vexp);
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells = VEC_F64_CELLS(vexp);
    for (int64_t i = 0; i < n; i++)
      s += cells[i];
  } else {
    for (int64_t i = 0; i < n; i++) {
      s += vec_read_double(vexp, i, &err);
      if (err)
        break;
    }
  }
  if (err)
    CLEAN_RETURN_1(vexp, error(ERROR_NUMBER_EXPECTED, e, env,
                               "vec-sum: non-numeric element"));
  CLEAN_RETURN_1(vexp, make_floatf((expfloat)s));
}

const char doc_vecmin[] =
    "(vec-min v) — smallest element as a float. Empty vec is an error.";
exp_t *vecmincmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(
        vexp, error(ERROR_ILLEGAL_VALUE, e, env, "(vec-min v): not a vector"));
  int64_t n = vec_len(vexp);
  if (n == 0)
    CLEAN_RETURN_1(vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env, "vec-min: empty vector"));
  int err = 0;
  double m;
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells = VEC_F64_CELLS(vexp);
    m = cells[0];
    for (int64_t i = 1; i < n; i++)
      if (cells[i] < m)
        m = cells[i];
  } else {
    m = vec_read_double(vexp, 0, &err);
    for (int64_t i = 1; i < n; i++) {
      double x = vec_read_double(vexp, i, &err);
      if (err)
        break;
      if (x < m)
        m = x;
    }
  }
  if (err)
    CLEAN_RETURN_1(vexp, error(ERROR_NUMBER_EXPECTED, e, env,
                               "vec-min: non-numeric element"));
  CLEAN_RETURN_1(vexp, make_floatf((expfloat)m));
}

const char doc_vecargmin[] =
    "(vec-argmin v) — index of the smallest element. Empty vec -> -1.";
exp_t *vecargmincmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(vexp, error(ERROR_ILLEGAL_VALUE, e, env,
                               "(vec-argmin v): not a vector"));
  int64_t best = -1;
  double bestv = 0.0;
  int err = 0;
  int64_t n = vec_len(vexp);
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells = VEC_F64_CELLS(vexp);
    for (int64_t i = 0; i < n; i++)
      if (best < 0 || cells[i] < bestv) {
        best = i;
        bestv = cells[i];
      }
  } else {
    for (int64_t i = 0; i < n; i++) {
      double x = vec_read_double(vexp, i, &err);
      if (err)
        break;
      if (best < 0 || x < bestv) {
        best = i;
        bestv = x;
      }
    }
  }
  if (err)
    CLEAN_RETURN_1(vexp, error(ERROR_NUMBER_EXPECTED, e, env,
                               "vec-argmin: non-numeric element"));
  CLEAN_RETURN_1(vexp, MAKE_FIX(best));
}

/* In-place elementwise activation: apply `fn` to every element. Promotes to
   F64. `name` is for the error message. Shared by exp!/sigmoid!/tanh!. */
#define VEC_ACTIVATION(cmdname, docname, fnexpr)                               \
  exp_t *cmdname(exp_t *e, env_t *env) {                                       \
    EVAL_ARG_1(vexp);                                                          \
    if (!isvector(vexp))                                                       \
      CLEAN_RETURN_1(vexp, error(ERROR_ILLEGAL_VALUE, e, env,                  \
                                 "(" docname " v): not a vector"));            \
    VEC_REQUIRE_FLOAT_WRITABLE(vexp, docname, CLEAN_RETURN_1(vexp, _alc_e));   \
    int err = 0;                                                              \
    int64_t n = vec_len(vexp);                                                \
    if (vec_kind(vexp) == VEC_KIND_F64) {                                     \
      double *cells = VEC_F64_CELLS(vexp);                                    \
      for (int64_t i = 0; i < n; i++) {                                       \
        double x = cells[i];                                                  \
        cells[i] = (fnexpr);                                                  \
      }                                                                       \
    } else {                                                                  \
      for (int64_t i = 0; i < n; i++) {                                       \
        double x = vec_read_double(vexp, i, &err);                            \
        if (err)                                                              \
          break;                                                             \
        vec_write_double(vexp, i, (fnexpr));                                  \
      }                                                                       \
    }                                                                         \
    if (err)                                                                  \
      CLEAN_RETURN_1(vexp, error(ERROR_NUMBER_EXPECTED, e, env,               \
                                 docname ": non-numeric element"));           \
    exp_t *ret = refexp(vexp);                                                \
    CLEAN_RETURN_1(vexp, ret);                                                \
  }
const char doc_vecexp[] = "(vec-exp! v) — in place v[i] = exp(v[i]). Returns v.";
VEC_ACTIVATION(vecexpcmd, "vec-exp!", exp(x))
const char doc_vecsigmoid[] =
    "(vec-sigmoid! v) — in place v[i] = 1/(1+exp(-v[i])). Returns v.";
VEC_ACTIVATION(vecsigmoidcmd, "vec-sigmoid!", 1.0 / (1.0 + exp(-x)))
const char doc_vectanh[] =
    "(vec-tanh! v) — in place v[i] = tanh(v[i]). Returns v.";
VEC_ACTIVATION(vectanhcmd, "vec-tanh!", tanh(x))

const char doc_vecsoftmax[] =
    "(vec-softmax! v) — in place numerically-stable softmax: v[i] = "
    "exp(v[i]-max)/sum. Returns v.";
exp_t *vecsoftmaxcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(vexp, error(ERROR_ILLEGAL_VALUE, e, env,
                               "(vec-softmax! v): not a vector"));
  VEC_REQUIRE_FLOAT_WRITABLE(vexp, "vec-softmax!", CLEAN_RETURN_1(vexp, _alc_e));
  int64_t n = vec_len(vexp);
  if (n == 0) { /* softmax of nothing is a no-op */
    exp_t *ret = refexp(vexp);
    CLEAN_RETURN_1(vexp, ret);
  }
  int err = 0;
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells = VEC_F64_CELLS(vexp);
    double m = cells[0];
    for (int64_t i = 1; i < n; i++)
      if (cells[i] > m)
        m = cells[i];
    double s = 0.0;
    for (int64_t i = 0; i < n; i++) {
      cells[i] = exp(cells[i] - m);
      s += cells[i];
    }
    double inv = s > 0.0 ? 1.0 / s : 0.0;
    VEC_SIMD
    for (int64_t i = 0; i < n; i++)
      cells[i] *= inv;
  } else {
    double m = vec_read_double(vexp, 0, &err);
    for (int64_t i = 1; i < n && !err; i++) {
      double x = vec_read_double(vexp, i, &err);
      if (x > m)
        m = x;
    }
    double s = 0.0;
    for (int64_t i = 0; i < n && !err; i++) {
      double x = exp(vec_read_double(vexp, i, &err) - m);
      vec_write_double(vexp, i, x);
      s += x;
    }
    double inv = s > 0.0 ? 1.0 / s : 0.0;
    for (int64_t i = 0; i < n && !err; i++)
      vec_write_double(vexp, i, vec_read_double(vexp, i, &err) * inv);
  }
  if (err)
    CLEAN_RETURN_1(vexp, error(ERROR_NUMBER_EXPECTED, e, env,
                               "vec-softmax!: non-numeric element"));
  exp_t *ret = refexp(vexp);
  CLEAN_RETURN_1(vexp, ret);
}

/* ---------- matrix ops (dense, row-major, over flat F64 vectors) ----------
   A matrix is a plain vector holding rows back-to-back; the shapes are derived
   from the vector lengths so the call stays minimal. Result is always a fresh
   F64 vector. F64 inputs hit a raw-double path (the inner loop is an axpy →
   SIMD); other kinds go through vec_read_double. */

/* Evaluate the optional 4th argument of the 4-ary vector ops (bias / scale /
   extra operand), or NIL_EXP if absent. Owned result (unref it); on an eval
   error the error exp is returned (iserror true). Centralizes the otherwise
   open-coded `e->next->next->next->next` walk used by mat-vec!, mat-vec-t!,
   vec-ger!, and vec<-blob. */
static exp_t *eval_opt_arg4(exp_t *e, env_t *env) {
  if (e->next && e->next->next && e->next->next->next &&
      e->next->next->next->next)
    return EVAL(e->next->next->next->next->content, env);
  return NIL_EXP;
}

const char doc_matvec[] =
    "(mat-vec A x) — matrix·vector. A is a flat row-major m×n matrix, x has "
    "length n; n = (len x), m = (len A)/n. Returns a fresh length-m F64 vector "
    "y where y[i] = sum_j A[i*n+j]*x[j].";
exp_t *matveccmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(aexp, xexp);
  if (!isvector(aexp) || !isvector(xexp))
    CLEAN_RETURN_2(aexp, xexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(mat-vec A x): both must be vectors"));
  int64_t alen = vec_len(aexp), n = vec_len(xexp);
  if (n <= 0 || alen % n != 0)
    CLEAN_RETURN_2(aexp, xexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "mat-vec: (len A)=%lld must be a positive multiple of "
                         "(len x)=%lld",
                         (long long)alen, (long long)n));
  int64_t m = alen / n;
  exp_t *zero = make_floatf(0.0);
  exp_t *out = make_vector(m, zero);
  unrefexp(zero);
  if (!out)
    CLEAN_RETURN_2(aexp, xexp,
                   error(ERROR_ILLEGAL_VALUE, e, env, "mat-vec: alloc failed"));
  int err = 0;
  double *y = VEC_F64_CELLS(out);
  if (vec_kind(aexp) == VEC_KIND_F64 && vec_kind(xexp) == VEC_KIND_F64) {
    double *A = VEC_F64_CELLS(aexp);
    double *X = VEC_F64_CELLS(xexp);
    for (int64_t i = 0; i < m; i++) {
      double s = 0.0;
      const double *row = A + i * n;
      for (int64_t j = 0; j < n; j++)
        s += row[j] * X[j];
      y[i] = s;
    }
  } else {
    for (int64_t i = 0; i < m && !err; i++) {
      double s = 0.0;
      for (int64_t j = 0; j < n; j++)
        s += vec_read_double(aexp, i * n + j, &err) *
             vec_read_double(xexp, j, &err);
      y[i] = s;
    }
  }
  if (err) {
    unrefexp(out);
    CLEAN_RETURN_2(aexp, xexp, error(ERROR_NUMBER_EXPECTED, e, env,
                                     "mat-vec: non-numeric element"));
  }
  CLEAN_RETURN_2(aexp, xexp, out);
}

const char doc_matvecbang[] =
    "(mat-vec! out A x [bias]) — in-place matrix·vector. A is a flat row-major "
    "m×n matrix, x has length n, out a length-m F64 vector that receives "
    "out[i] = sum_j A[i*n+j]*x[j] (+ bias[i] if a length-m bias vec is given). "
    "No allocation — the fused dense-layer primitive (one C call replaces a "
    "per-row vec-dot loop; ~8x faster). Returns out.";
exp_t *matvecbangcmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(outexp, aexp, xexp);
  exp_t *bexp = eval_opt_arg4(e, env);
  if (iserror(bexp))
    CLEAN_RETURN_3(outexp, aexp, xexp, bexp);
  int has_bias = (bexp != NIL_EXP);
  if (!isvector(outexp) || !isvector(aexp) || !isvector(xexp) ||
      (has_bias && !isvector(bexp)))
    CLEAN_RETURN_4(outexp, aexp, xexp, bexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(mat-vec! out A x [bias]): out, A, x [, bias] must "
                         "be vectors"));
  int64_t alen = vec_len(aexp), n = vec_len(xexp), m = vec_len(outexp);
  if (n <= 0 || m <= 0 || alen != m * n)
    CLEAN_RETURN_4(outexp, aexp, xexp, bexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "mat-vec!: shape mismatch — (len A)=%lld must equal "
                         "(len out)=%lld * (len x)=%lld",
                         (long long)alen, (long long)m, (long long)n));
  if (has_bias && vec_len(bexp) != m)
    CLEAN_RETURN_4(outexp, aexp, xexp, bexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "mat-vec!: bias length %lld != (len out)=%lld",
                         (long long)vec_len(bexp), (long long)m));
  VEC_REQUIRE_FLOAT_WRITABLE(outexp, "mat-vec!",
                             CLEAN_RETURN_4(outexp, aexp, xexp, bexp, _alc_e));
  int err = 0;
  double *y = VEC_F64_CELLS(outexp);
  if (vec_kind(aexp) == VEC_KIND_F64 && vec_kind(xexp) == VEC_KIND_F64 &&
      (!has_bias || vec_kind(bexp) == VEC_KIND_F64)) {
    double *A = VEC_F64_CELLS(aexp);
    double *X = VEC_F64_CELLS(xexp);
    double *B = has_bias ? VEC_F64_CELLS(bexp) : NULL;
    for (int64_t i = 0; i < m; i++) {
      double s = B ? B[i] : 0.0;
      const double *row = A + i * n;
      for (int64_t j = 0; j < n; j++)
        s += row[j] * X[j];
      y[i] = s;
    }
  } else {
    for (int64_t i = 0; i < m && !err; i++) {
      double s = has_bias ? vec_read_double(bexp, i, &err) : 0.0;
      for (int64_t j = 0; j < n; j++)
        s += vec_read_double(aexp, i * n + j, &err) *
             vec_read_double(xexp, j, &err);
      y[i] = s;
    }
  }
  if (err)
    CLEAN_RETURN_4(outexp, aexp, xexp, bexp,
                   error(ERROR_NUMBER_EXPECTED, e, env,
                         "mat-vec!: non-numeric element"));
  exp_t *ret = refexp(outexp);
  CLEAN_RETURN_4(outexp, aexp, xexp, bexp, ret);
}

const char doc_matvectbang[] =
    "(mat-vec-t! out A v [bias]) — in-place TRANSPOSED matrix·vector. A is a "
    "flat row-major m×n matrix, v length m, out length n; computes Aᵀ·v: "
    "out[j] = sum_i A[i*n+j]*v[i] (+ bias[j]) without transposing A. The fused "
    "input-gradient primitive for a dense layer's backward pass. Returns out.";
exp_t *matvectbangcmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(outexp, aexp, vexp);
  exp_t *bexp = eval_opt_arg4(e, env);
  if (iserror(bexp))
    CLEAN_RETURN_3(outexp, aexp, vexp, bexp);
  int has_bias = (bexp != NIL_EXP);
  if (!isvector(outexp) || !isvector(aexp) || !isvector(vexp) ||
      (has_bias && !isvector(bexp)))
    CLEAN_RETURN_4(outexp, aexp, vexp, bexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(mat-vec-t! out A v [bias]): out, A, v [, bias] must "
                         "be vectors"));
  int64_t alen = vec_len(aexp), m = vec_len(vexp), n = vec_len(outexp);
  if (m <= 0 || n <= 0 || alen != m * n)
    CLEAN_RETURN_4(outexp, aexp, vexp, bexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "mat-vec-t!: shape mismatch — (len A)=%lld must equal "
                         "(len v)=%lld * (len out)=%lld",
                         (long long)alen, (long long)m, (long long)n));
  if (has_bias && vec_len(bexp) != n)
    CLEAN_RETURN_4(outexp, aexp, vexp, bexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "mat-vec-t!: bias length %lld != (len out)=%lld",
                         (long long)vec_len(bexp), (long long)n));
  VEC_REQUIRE_FLOAT_WRITABLE(outexp, "mat-vec-t!",
                             CLEAN_RETURN_4(outexp, aexp, vexp, bexp, _alc_e));
  int err = 0;
  double *y = VEC_F64_CELLS(outexp);
  if (vec_kind(aexp) == VEC_KIND_F64 && vec_kind(vexp) == VEC_KIND_F64 &&
      (!has_bias || vec_kind(bexp) == VEC_KIND_F64)) {
    double *A = VEC_F64_CELLS(aexp);
    double *V = VEC_F64_CELLS(vexp);
    double *B = has_bias ? VEC_F64_CELLS(bexp) : NULL;
    for (int64_t j = 0; j < n; j++)
      y[j] = B ? B[j] : 0.0;
    /* Accumulate row by row so the inner walk over A is sequential. */
    for (int64_t i = 0; i < m; i++) {
      double vi = V[i];
      const double *row = A + i * n;
      VEC_SIMD
      for (int64_t j = 0; j < n; j++)
        y[j] += vi * row[j];
    }
  } else {
    for (int64_t j = 0; j < n; j++)
      y[j] = has_bias ? vec_read_double(bexp, j, &err) : 0.0;
    for (int64_t i = 0; i < m && !err; i++) {
      double vi = vec_read_double(vexp, i, &err);
      for (int64_t j = 0; j < n; j++)
        y[j] += vi * vec_read_double(aexp, i * n + j, &err);
    }
  }
  if (err)
    CLEAN_RETURN_4(outexp, aexp, vexp, bexp,
                   error(ERROR_NUMBER_EXPECTED, e, env,
                         "mat-vec-t!: non-numeric element"));
  exp_t *ret = refexp(outexp);
  CLEAN_RETURN_4(outexp, aexp, vexp, bexp, ret);
}

const char doc_vecger[] =
    "(vec-ger! A alpha u v) — in-place rank-1 update of a flat row-major m×n "
    "matrix A: A[i*n+j] += alpha*u[i]*v[j], with u length m, v length n. The "
    "fused weight-update / outer-product primitive (BLAS ger). Returns A.";
exp_t *vecgercmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(aexp, alphaexp, uexp);
  exp_t *vexp = eval_opt_arg4(e, env);
  if (iserror(vexp))
    CLEAN_RETURN_3(aexp, alphaexp, uexp, vexp);
  if (!isvector(aexp) || !(isnumber(alphaexp) || isfloat(alphaexp)) ||
      !isvector(uexp) || !isvector(vexp))
    CLEAN_RETURN_4(aexp, alphaexp, uexp, vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-ger! A alpha u v): A,u,v vecs and alpha scalar"));
  int64_t alen = vec_len(aexp), m = vec_len(uexp), n = vec_len(vexp);
  if (m <= 0 || n <= 0 || alen != m * n)
    CLEAN_RETURN_4(aexp, alphaexp, uexp, vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-ger!: shape mismatch — (len A)=%lld must equal "
                         "(len u)=%lld * (len v)=%lld",
                         (long long)alen, (long long)m, (long long)n));
  double alpha = isfloat(alphaexp) ? alphaexp->f : (double)FIX_VAL(alphaexp);
  VEC_REQUIRE_FLOAT_WRITABLE(aexp, "vec-ger!",
                             CLEAN_RETURN_4(aexp, alphaexp, uexp, vexp, _alc_e));
  int err = 0;
  if (vec_kind(aexp) == VEC_KIND_F64 && vec_kind(uexp) == VEC_KIND_F64 &&
      vec_kind(vexp) == VEC_KIND_F64) {
    double *A = VEC_F64_CELLS(aexp);
    double *U = VEC_F64_CELLS(uexp);
    double *V = VEC_F64_CELLS(vexp);
    for (int64_t i = 0; i < m; i++) {
      double au = alpha * U[i];
      double *row = A + i * n;
      VEC_SIMD
      for (int64_t j = 0; j < n; j++)
        row[j] += au * V[j];
    }
  } else {
    for (int64_t i = 0; i < m && !err; i++) {
      double au = alpha * vec_read_double(uexp, i, &err);
      for (int64_t j = 0; j < n; j++) {
        double a = vec_read_double(aexp, i * n + j, &err);
        vec_write_double(aexp, i * n + j, a + au * vec_read_double(vexp, j, &err));
      }
    }
  }
  if (err)
    CLEAN_RETURN_4(aexp, alphaexp, uexp, vexp,
                   error(ERROR_NUMBER_EXPECTED, e, env,
                         "vec-ger!: non-numeric element"));
  exp_t *ret = refexp(aexp);
  CLEAN_RETURN_4(aexp, alphaexp, uexp, vexp, ret);
}

const char doc_vecfromblob[] =
    "(vec-from-blob! v b off [scale]) — bulk-load bytes into an F64 vector: "
    "v[i] = b[off+i] * scale (scale defaults to 1.0; bytes read as unsigned "
    "0..255). One C loop instead of a per-element blob-ref/vec-set! pair — "
    "the fast path for decoding packed u8 datasets (images, audio) into "
    "tensors. Returns v.";
exp_t *vecfromblobcmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(vexp, bexp, offexp);
  exp_t *sexp = eval_opt_arg4(e, env);
  if (iserror(sexp))
    CLEAN_RETURN_3(vexp, bexp, offexp, sexp);
  int has_scale = (sexp != NIL_EXP);
  if (!isvector(vexp) || !isblob(bexp) || !isnumber(offexp) ||
      (has_scale && !(isnumber(sexp) || isfloat(sexp))))
    CLEAN_RETURN_4(vexp, bexp, offexp, sexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-from-blob! v b off [scale]): vec, blob, "
                         "integer offset [, numeric scale]"));
  int64_t off = FIX_VAL(offexp);
  int64_t n = vec_len(vexp);
  alc_blob_t *bb = (alc_blob_t *)bexp->ptr;
  if (off < 0 || off + n > (int64_t)bb->len)
    CLEAN_RETURN_4(vexp, bexp, offexp, sexp,
                   error(ERROR_INDEX_OUT_OF_RANGE, e, env,
                         "vec-from-blob!: bytes [%lld..%lld) outside blob of "
                         "%lld bytes",
                         (long long)off, (long long)(off + n),
                         (long long)bb->len));
  double scale = !has_scale  ? 1.0
                 : isfloat(sexp) ? sexp->f
                                 : (double)FIX_VAL(sexp);
  VEC_REQUIRE_FLOAT_WRITABLE(vexp, "vec-from-blob!",
                             CLEAN_RETURN_4(vexp, bexp, offexp, sexp, _alc_e));
  const unsigned char *src = (const unsigned char *)bb->bytes + off;
  if (vec_kind(vexp) == VEC_KIND_F64) {
    double *cells = VEC_F64_CELLS(vexp);
    VEC_SIMD
    for (int64_t i = 0; i < n; i++)
      cells[i] = scale * (double)src[i];
  } else {
    for (int64_t i = 0; i < n; i++)
      vec_write_double(vexp, i, scale * (double)src[i]);
  }
  exp_t *ret = refexp(vexp);
  CLEAN_RETURN_4(vexp, bexp, offexp, sexp, ret);
}

const char doc_matmul[] =
    "(mat-mul A B k) — matrix·matrix. A is a flat row-major m×k matrix, B a "
    "flat k×n matrix (k is the shared inner dimension); m = (len A)/k, n = "
    "(len B)/k. Returns a fresh row-major m×n F64 vector C[i*n+j] = "
    "sum_p A[i*k+p]*B[p*n+j].";
exp_t *matmulcmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(aexp, bexp, kexp);
  if (!isvector(aexp) || !isvector(bexp) || !isnumber(kexp))
    CLEAN_RETURN_3(aexp, bexp, kexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(mat-mul A B k): vectors A, B and integer k"));
  int64_t k = FIX_VAL(kexp);
  int64_t alen = vec_len(aexp), blen = vec_len(bexp);
  if (k <= 0 || alen % k != 0 || blen % k != 0)
    CLEAN_RETURN_3(aexp, bexp, kexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "mat-mul: k=%lld must be positive and divide both "
                         "(len A)=%lld and (len B)=%lld",
                         (long long)k, (long long)alen, (long long)blen));
  int64_t m = alen / k, n = blen / k;
  exp_t *zero = make_floatf(0.0);
  exp_t *out = make_vector(m * n, zero); /* zero-initialised — accumulated into */
  unrefexp(zero);
  if (!out)
    CLEAN_RETURN_3(aexp, bexp, kexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "mat-mul: result %lldx%lld too large",
                         (long long)m, (long long)n));
  int err = 0;
  double *C = VEC_F64_CELLS(out);
  if (vec_kind(aexp) == VEC_KIND_F64 && vec_kind(bexp) == VEC_KIND_F64) {
    double *A = VEC_F64_CELLS(aexp);
    double *B = VEC_F64_CELLS(bexp);
    /* i,p,j order: the inner j loop is C[i*n+..] += aip*B[p*n+..] — a contiguous
       axpy that vectorises; C was zero-initialised above. */
    for (int64_t i = 0; i < m; i++) {
      double *crow = C + i * n;
      const double *arow = A + i * k;
      for (int64_t p = 0; p < k; p++) {
        double aip = arow[p];
        const double *brow = B + p * n;
        VEC_SIMD
        for (int64_t j = 0; j < n; j++)
          crow[j] += aip * brow[j];
      }
    }
  } else {
    /* Same i,p,j accumulation order as the F64 path (accumulate into the
       zero-init C) so F64-kind and generic-numeric inputs round identically;
       stop on the first non-numeric element. */
    for (int64_t i = 0; i < m && !err; i++)
      for (int64_t p = 0; p < k && !err; p++) {
        double aip = vec_read_double(aexp, i * k + p, &err);
        for (int64_t j = 0; j < n && !err; j++)
          C[i * n + j] += aip * vec_read_double(bexp, p * n + j, &err);
      }
  }
  if (err) {
    unrefexp(out);
    CLEAN_RETURN_3(aexp, bexp, kexp, error(ERROR_NUMBER_EXPECTED, e, env,
                                           "mat-mul: non-numeric element"));
  }
  CLEAN_RETURN_3(aexp, bexp, kexp, out);
}

/* ---------- deque ops ----------
   vec-push! / vec-pop! at the back; vec-unshift! / vec-shift! at the
   front. Amortised O(1) via the cap/start/end window — pops bump the
   window without shifting; pushes grow only when the window hits a
   boundary. Growth: 1.5x cap on realloc; slide-left when start >=
   cap/4 (recovers headroom without reallocating); recenter on
   unshift-grow so subsequent unshifts don't realloc. */

/* Slide the live window down to position 0. Used when push hits the
   back boundary but there's room at the front (start > 0). For typed
   kinds the move is over raw int64/double bytes; for GEN the same
   bytewise move is correct because the cells before start are
   uninitialised garbage (the window is the source of truth for which
   cells are owned). */
static void vec_slide_left(exp_t *vexp) {
  int32_t start = vexp->vec_win.start;
  if (start == 0)
    return;
  int32_t live = vexp->vec_win.end - start;
  char *base = (char *)vexp->ptr + sizeof(alc_vec_t);
  if (live > 0)
    memmove(base, base + (size_t)start * 8u, (size_t)live * 8u);
  vexp->vec_win.start = 0;
  vexp->vec_win.end = live;
}

/* Reallocate to `new_cap` cells, copying the live window to start at
   `front_pad`. Old buffer freed; vexp->ptr replaced. Returns 0 on
   alloc failure (vexp unchanged in that case). */
static int vec_realloc(exp_t *vexp, int32_t new_cap, int32_t front_pad) {
  alc_vec_t *old = (alc_vec_t *)vexp->ptr;
  int32_t start = vexp->vec_win.start;
  int32_t live = vexp->vec_win.end - start;
  if (new_cap < live + front_pad)
    return 0;
  alc_vec_t *new_v = vec_alloc_storage(new_cap);
  if (!new_v)
    return 0;
  if (live > 0) {
    char *src = (char *)old + sizeof(alc_vec_t) + (size_t)start * 8u;
    char *dst = (char *)new_v + sizeof(alc_vec_t) + (size_t)front_pad * 8u;
    memcpy(dst, src, (size_t)live * 8u);
  }
  free(old);
  vexp->ptr = new_v;
  vexp->vec_win.start = front_pad;
  vexp->vec_win.end = front_pad + live;
  return 1;
}

/* Ensure there's room at the back for one more push. Either slides
   the window left (recovers headroom) or grows 1.5x. */
static int vec_grow_back(exp_t *vexp) {
  alc_vec_t *v = (alc_vec_t *)vexp->ptr;
  int32_t cap = v->cap;
  /* Slide left only when it actually reclaims back-room — that requires
     start > 0. The `>= cap/4` part is a heuristic to avoid sliding for
     trivial gains, but for small caps cap/4 truncates to 0; combined with
     start == 0 (a full vector with no front headroom) the slide is a
     no-op, so we'd return "grew" without room and the caller would write
     past the buffer. Guarding on start > 0 forces a realloc in that case. */
  if (vexp->vec_win.start > 0 && vexp->vec_win.start >= cap / 4) {
    vec_slide_left(vexp);
    return 1;
  }
  /* int64 to avoid signed-overflow UB when cap is near INT32_MAX; fail the
     grow cleanly (vec-push then errors) if 1.5x would exceed the int32 cap. */
  int64_t grown = cap < 4 ? 8 : (int64_t)cap + (cap >> 1);
  if (grown >= ((int64_t)1 << 31))
    return 0;
  return vec_realloc(vexp, (int32_t)grown, 0);
}

/* Ensure there's room at the front for one more unshift. Always
   reallocates and recenters — front-grow without recentering would
   force a fresh realloc on every subsequent unshift. */
static int vec_grow_front(exp_t *vexp) {
  alc_vec_t *v = (alc_vec_t *)vexp->ptr;
  int32_t cap = v->cap;
  int32_t live = vexp->vec_win.end - vexp->vec_win.start;
  /* int64 to avoid signed-overflow UB near INT32_MAX; fail cleanly if the
     grown capacity would not fit a positive int32. */
  int64_t grown = cap < 4 ? 8 : (int64_t)cap + (cap >> 1);
  if (grown >= ((int64_t)1 << 31))
    return 0;
  int32_t new_cap = (int32_t)grown;
  int32_t front_pad = (new_cap - live) / 2;
  return vec_realloc(vexp, new_cap, front_pad);
}

const char doc_vecpush[] =
    "(vec-push! v x) — append x to the back of v (amortised O(1)). "
    "Returns x.";
exp_t *vecpushcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(vexp, valexp);
  if (!isvector(vexp))
    CLEAN_RETURN_2(
        vexp, valexp,
        error(ERROR_ILLEGAL_VALUE, e, env, "(vec-push! v x): v must be a vec"));
  if (vexp->flags & FLAG_SHARED)
    CLEAN_RETURN_2(vexp, valexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-push!: cannot mutate a shared vec"));
  if (vexp->vec_win.end == (int32_t)vec_cap(vexp)) {
    if (!vec_grow_back(vexp))
      CLEAN_RETURN_2(
          vexp, valexp,
          error(ERROR_ILLEGAL_VALUE, e, env, "vec-push!: grow failed"));
  }
  /* Extend the window by one. The new slot is uninitialised; for GEN
     we pre-write NIL so vec_set_boxed's unrefexp doesn't read garbage. */
  int32_t off = vexp->vec_win.end - vexp->vec_win.start; /* new logical idx */
  vexp->vec_win.end++;
  if (vec_kind(vexp) == VEC_KIND_GEN) {
    exp_t **cells = (exp_t **)((char *)vexp->ptr + sizeof(alc_vec_t));
    cells[vexp->vec_win.start + off] = refexp(NIL_EXP);
  }
  exp_t *ret = refexp(valexp);
  if (!vec_set_boxed(vexp, off, valexp)) {
    vexp->vec_win.end--; /* roll back */
    exp_t *err = error(ERROR_ILLEGAL_VALUE, e, env, "vec-push!: write failed");
    unrefexp(ret);
    unrefexp(vexp);
    unrefexp(e);
    return err;
  }
  /* valexp consumed by vec_set_boxed. */
  unrefexp(vexp);
  unrefexp(e);
  return ret;
}

const char doc_vecpop[] = "(vec-pop! v) — remove and return the last "
                          "element of v (O(1)). Errors on empty.";
exp_t *vecpopcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(
        vexp, error(ERROR_ILLEGAL_VALUE, e, env, "(vec-pop! v): not a vec"));
  if (vexp->flags & FLAG_SHARED)
    CLEAN_RETURN_1(vexp, error(ERROR_ILLEGAL_VALUE, e, env,
                               "vec-pop!: cannot mutate a shared vec"));
  int64_t n = vec_len(vexp);
  if (n == 0)
    CLEAN_RETURN_1(vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env, "vec-pop!: empty vec"));
  exp_t *ret = vec_get_boxed(vexp, n - 1);
  /* For GEN, the slot we're abandoning owns a ref — drop it. Typed
     kinds hold raw scalars, no ref accounting. */
  if (vec_kind(vexp) == VEC_KIND_GEN) {
    exp_t **cells = vec_gen_cells(vexp);
    unrefexp(cells[n - 1]);
  }
  vexp->vec_win.end--;
  CLEAN_RETURN_1(vexp, ret);
}

const char doc_vecunshift[] =
    "(vec-unshift! v x) — prepend x to the front of v (amortised O(1)). "
    "Returns x.";
exp_t *vecunshiftcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(vexp, valexp);
  if (!isvector(vexp))
    CLEAN_RETURN_2(vexp, valexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "(vec-unshift! v x): v must be a vec"));
  if (vexp->flags & FLAG_SHARED)
    CLEAN_RETURN_2(vexp, valexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "vec-unshift!: cannot mutate a shared vec"));
  if (vexp->vec_win.start == 0) {
    if (!vec_grow_front(vexp))
      CLEAN_RETURN_2(
          vexp, valexp,
          error(ERROR_ILLEGAL_VALUE, e, env, "vec-unshift!: grow failed"));
  }
  vexp->vec_win.start--;
  if (vec_kind(vexp) == VEC_KIND_GEN) {
    exp_t **cells = (exp_t **)((char *)vexp->ptr + sizeof(alc_vec_t));
    cells[vexp->vec_win.start] = refexp(NIL_EXP);
  }
  exp_t *ret = refexp(valexp);
  if (!vec_set_boxed(vexp, 0, valexp)) {
    vexp->vec_win.start++; /* roll back window */
    /* Release the NIL sentinel placed above for GEN kind */
    if (vec_kind(vexp) == VEC_KIND_GEN) {
      exp_t **cells = (exp_t **)((char *)vexp->ptr + sizeof(alc_vec_t));
      unrefexp(cells[vexp->vec_win.start]);
      cells[vexp->vec_win.start] = NULL;
    }
    unrefexp(ret);
    /* CLEAN_RETURN_2 evaluates error() before touching vexp/valexp/e, so
       no use-after-free; the old hand-rolled path had that ordering bug. */
    CLEAN_RETURN_2(
        vexp, valexp,
        error(ERROR_ILLEGAL_VALUE, NULL, env, "vec-unshift!: write failed"));
  }
  unrefexp(vexp);
  unrefexp(e);
  return ret;
}

const char doc_vecshift[] = "(vec-shift! v) — remove and return the first "
                            "element of v (O(1)). Errors on empty.";
exp_t *vecshiftcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(vexp);
  if (!isvector(vexp))
    CLEAN_RETURN_1(
        vexp, error(ERROR_ILLEGAL_VALUE, e, env, "(vec-shift! v): not a vec"));
  if (vexp->flags & FLAG_SHARED)
    CLEAN_RETURN_1(vexp, error(ERROR_ILLEGAL_VALUE, e, env,
                               "vec-shift!: cannot mutate a shared vec"));
  if (vec_len(vexp) == 0)
    CLEAN_RETURN_1(vexp,
                   error(ERROR_ILLEGAL_VALUE, e, env, "vec-shift!: empty vec"));
  exp_t *ret = vec_get_boxed(vexp, 0);
  if (vec_kind(vexp) == VEC_KIND_GEN) {
    exp_t **cells = vec_gen_cells(vexp);
    unrefexp(cells[0]);
  }
  vexp->vec_win.start++;
  CLEAN_RETURN_1(vexp, ret);
}

/* the `vector` constructor builtin — kept with the vector ops it builds. */
const char doc_vector[] = "(vector x ...) — build an EXP_VECTOR populated with "
                          "the given elements. Same as #[x ...].";
exp_t *vectorcmd(exp_t *e, env_t *env) {
  /* Two-pass: count elements (so we can size the vector once), then
     evaluate-and-store. Cheaper than growing a list intermediary. */
  long n = 0;
  for (exp_t *p = cdr(e); p; p = p->next)
    n++;
  exp_t *ret = make_vector(n, NIL_EXP);
  if (!ret) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "vector: alloc failed");
  }
  long i = 0;
  for (exp_t *p = cdr(e); p; p = p->next, i++) {
    exp_t *v = EVAL(car(p), env);
    if (iserror(v)) {
      unrefexp(ret);
      unrefexp(e);
      return v;
    }
    /* make_vector pre-filled with NIL (refcount bump per slot); release
       the placeholder before overwriting. */
    unrefexp(vec_gen_at(ret, i));
    vec_gen_at(ret, i) = v; /* take ownership */
  }
  /* Now that all elements are known, tighten to the narrowest kind:
     all-fixnum → I64, all-numeric → F64, anything else stays GEN. */
  vec_tighten(ret);
  unrefexp(e);
  return ret;
}
