/* ffi.h — the libffi binding (ffi-fn / ffi-callback / ffi-struct / ffi-pack /
 * ffi-unpack / ffi-vfn): type resolution, dlopen, call/closure dispatch, and
 * the cmd entry points. Wraps the whole #ifdef ALCOVE_FFI ... #else (stub cmds
 * that error on FFI use) ... #endif, so non-FFI builds get the stubs. FRAGMENT
 * #included into alcove.c at its original position; the 3 tiny #ifdef
 * ALCOVE_FFI dispatch hooks in eval/VM stay in alcove.c. NOT standalone.
 */
#ifdef ALCOVE_FFI
#include <dlfcn.h>
#include <ffi.h>

#define ALC_FFI_MAX_ARGS 8
typedef enum {
  AFFI_VOID,
  AFFI_INT,
  AFFI_LONG,
  AFFI_DOUBLE,
  AFFI_STRING,
  AFFI_PTR,
  AFFI_STRUCT /* by-value aggregate; see AFFI_KIND_STRUCT descriptor */
} alc_ffi_tag_t;

/* An EXP_FFI value is one of: a bound C function (FN — ffi-fn), an alcove
   lambda exposed to C as a function pointer (CB — ffi-callback), or a
   by-value struct type descriptor (STRUCT — ffi-struct). memalloc zeroes the
   struct, so AFFI_KIND_FN == 0 keeps existing ffi-fn paths intact. */
typedef enum {
  AFFI_KIND_FN = 0,
  AFFI_KIND_CB,
  AFFI_KIND_STRUCT
} alc_ffi_kind_t;

typedef struct alc_ffi_t {
  void *fn; /* dlsym result (FN kind) */
  ffi_cif cif;
  ffi_type *rtype;
  unsigned int nargs; /* FN/CB: arg count. STRUCT: field count. */
  ffi_type *atypes[ALC_FFI_MAX_ARGS]; /* FN/CB args; STRUCT field types */
  uint8_t ret_tag;
  uint8_t arg_tags[ALC_FFI_MAX_ARGS]; /* FN/CB arg tags; STRUCT field tags */
  uint8_t kind;                       /* AFFI_KIND_FN | _CB | _STRUCT */
  uint8_t variadic;   /* FN: bound via ffi-vfn — nargs is the FIXED count and
                         the cif is prepped per call (ffi_prep_cif_var) since
                         the variadic arg types vary by call site. */
  char *display_name; /* "lib:fn" (FN) / "<callback>" / "<struct>" for errors */
  /* CB kind only — the libffi closure trampoline and the alcove fn it calls.
     `code` is the C-callable entry point; pass it where a ptr arg is wanted. */
  ffi_closure *closure;
  void *code;
  exp_t *cb_lambda; /* owned ref so the lambda outlives any C-side calls */
  /* FN kind only — for STRUCT-tagged args/return, the owning descriptor
     (so we know the size to validate/allocate). NULL for scalar slots. */
  exp_t *arg_structs[ALC_FFI_MAX_ARGS];
  exp_t *ret_struct;
  /* STRUCT kind only — the by-value aggregate layout. struct_type is the
     FFI_TYPE_STRUCT passed to ffi_prep_cif; elements is its NULL-terminated
     member array; offsets/struct_size are computed for pack/unpack. */
  ffi_type struct_type;
  ffi_type *elements[ALC_FFI_MAX_ARGS + 1];
  size_t offsets[ALC_FFI_MAX_ARGS];
  size_t struct_size;
  size_t struct_align; /* max field alignment — needed when nested in another
                          struct's layout. arg_structs[i] holds the nested
                          descriptor for a struct-typed field (else NULL). */
} alc_ffi_t;

/* Map a type-name string to (tag, ffi_type*). Returns 0 on success, -1
   on unknown type. */
static int alc_ffi_typeof(const char *name, alc_ffi_tag_t *tag,
                          ffi_type **out) {
  if (!strcmp(name, "void")) {
    *tag = AFFI_VOID;
    *out = &ffi_type_void;
    return 0;
  }
  if (!strcmp(name, "int")) {
    *tag = AFFI_INT;
    *out = &ffi_type_sint32;
    return 0;
  }
  if (!strcmp(name, "long") || !strcmp(name, "int64")) {
    *tag = AFFI_LONG;
    *out = &ffi_type_sint64;
    return 0;
  }
  if (!strcmp(name, "double")) {
    *tag = AFFI_DOUBLE;
    *out = &ffi_type_double;
    return 0;
  }
  if (!strcmp(name, "string") || !strcmp(name, "char*")) {
    *tag = AFFI_STRING;
    *out = &ffi_type_pointer;
    return 0;
  }
  if (!strcmp(name, "ptr") || !strcmp(name, "void*")) {
    *tag = AFFI_PTR;
    *out = &ffi_type_pointer;
    return 0;
  }
  return -1;
}

/* Resolve an ffi type spec used by ffi-fn for a return/arg type: either a
   type-name string (scalar) or a struct descriptor value from ffi-struct.
   On success sets the tag and ffi_type out-params plus *desc (the descriptor
   exp for a struct, else NULL) and returns 0; returns -1 on an invalid spec. */
static int alc_ffi_resolve_type(exp_t *spec, alc_ffi_tag_t *tag, ffi_type **out,
                                exp_t **desc) {
  *desc = NULL;
  if (isffi(spec)) {
    alc_ffi_t *d = (alc_ffi_t *)spec->ptr;
    if (d->kind != AFFI_KIND_STRUCT)
      return -1;
    *tag = AFFI_STRUCT;
    *out = &d->struct_type;
    *desc = spec;
    return 0;
  }
  if (isstring(spec))
    return alc_ffi_typeof((char *)exp_text(spec), tag, out);
  return -1;
}

/* Forward decl: ffi-fn's error paths free a partially-built binding via the
   full destructor (it may already hold refexp'd struct descriptors). */
void alc_ffi_free(void *ptr);

/* Process-wide cache of dlopen handles keyed by lib name. dlopen with
   the same name on Linux returns the same handle anyway, but caching
   avoids re-resolving on each (ffi-fn) call. The mutex serializes the
   list mutation under multi-thread builds; it is held across dlopen
   itself so a concurrent caller never observes a half-linked entry,
   and to avoid duplicate inserts under a TOCTOU race. dlopen is rare
   (one-time per lib name), so the contention cost is negligible. */
#if !ALCOVE_SINGLE_THREADED
static pthread_mutex_t g_ffi_libs_mtx = PTHREAD_MUTEX_INITIALIZER;
#define FFI_LIBS_LOCK() pthread_mutex_lock(&g_ffi_libs_mtx)
#define FFI_LIBS_UNLOCK() pthread_mutex_unlock(&g_ffi_libs_mtx)
#else
#define FFI_LIBS_LOCK() ((void)0)
#define FFI_LIBS_UNLOCK() ((void)0)
#endif
static struct ffi_lib_cache {
  char *name;
  void *h;
  struct ffi_lib_cache *next;
} *g_ffi_libs = NULL;
static void *alc_ffi_dlopen(const char *name) {
  struct ffi_lib_cache *c;
  FFI_LIBS_LOCK();
  for (c = g_ffi_libs; c; c = c->next)
    if (!strcmp(c->name, name)) {
      void *h = c->h;
      FFI_LIBS_UNLOCK();
      return h;
    }
  /* An empty lib name means "this process": dlopen(NULL) resolves symbols
     already linked into alcove (libc, libm, libffi, and alcove's own
     exported functions). Lets scripts/tests bind well-known symbols
     (strlen, abs, …) without naming a platform-specific shared object.
     Otherwise: RTLD_LOCAL keeps the lib's symbols out of the global
     namespace — reduces accidental shadowing if multiple libs export the
     same name. RTLD_NOW resolves all symbols at load so dlsym failures
     surface promptly. */
  void *h = dlopen(name[0] ? name : NULL, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    FFI_LIBS_UNLOCK();
    return NULL;
  }
  c = (struct ffi_lib_cache *)memalloc(1, sizeof(*c));
  c->name = strdup(name);
  c->h = h;
  c->next = g_ffi_libs;
  g_ffi_libs = c;
  FFI_LIBS_UNLOCK();
  return h;
}

/* ---- FFI self-test fixtures ----
   Tiny exported C functions the test suite (test.alc) binds to via the ""
   (process-self) library, so the whole FFI surface — scalar calls, callbacks,
   and struct-by-value — has automated regression coverage with no external
   .so. They must NOT be static (dlsym resolves them) and the binary is linked
   with -rdynamic so Linux exports them too (macOS exports by default). Not a
   public API; the alc_ffi_selftest_ prefix keeps them out of the way. */
long alc_ffi_selftest_add(long a, long b) { return a + b; }
long alc_ffi_selftest_apply2(long (*fn)(long, long), long a, long b) {
  return fn(a, b);
}
long alc_ffi_selftest_sum_map(long (*fn)(long), long n) {
  long s = 0;
  for (long i = 0; i < n; i++)
    s += fn(i);
  return s;
}
double alc_ffi_selftest_apply_d(double (*fn)(double), double x) {
  return fn(x);
}
typedef struct {
  double x, y;
} alc_ffi_selftest_point;
double alc_ffi_selftest_pt_norm2(alc_ffi_selftest_point p) {
  return p.x * p.x + p.y * p.y;
}
alc_ffi_selftest_point alc_ffi_selftest_pt_make(double x, double y) {
  alc_ffi_selftest_point p = {x, y};
  return p;
}
/* Nested-struct fixture: a segment of two points (struct-in-struct). */
typedef struct {
  alc_ffi_selftest_point a, b;
} alc_ffi_selftest_seg;
double alc_ffi_selftest_seg_len2(alc_ffi_selftest_seg s) {
  double dx = s.b.x - s.a.x, dy = s.b.y - s.a.y;
  return dx * dx + dy * dy;
}
/* Variadic fixtures — sum `count` trailing args. Return a value (no stdout)
   so the test suite can assert on them. */
long alc_ffi_selftest_vsum(int count, ...) {
  va_list ap;
  va_start(ap, count);
  long s = 0;
  for (int i = 0; i < count; i++)
    s += va_arg(ap, long);
  va_end(ap);
  return s;
}
double alc_ffi_selftest_vsumd(int count, ...) {
  va_list ap;
  va_start(ap, count);
  double s = 0;
  for (int i = 0; i < count; i++)
    s += va_arg(ap, double);
  va_end(ap);
  return s;
}

/* Shared binder for (ffi-fn ...) and (ffi-vfn ...). When variadic, the given
   arg types are the FIXED prefix (>=1 required, per C's named-param rule),
   the cif is prepped per call in alc_ffi_call, and any extra call args have
   their types inferred from their alcove runtime type. */
static exp_t *ffi_bind_impl(exp_t *e, env_t *env, int variadic) {
  const char *who = variadic ? "ffi-vfn" : "ffi-fn";
  exp_t *cur = e->next;
  exp_t *libname = NULL, *fnname = NULL, *rtype = NULL;
  exp_t *atypes[ALC_FFI_MAX_ARGS] = {0};
  int n_a = 0;
  exp_t *err = NULL;
  exp_t *ret = NULL; /* declared up here so the `goto cleanup`
                        above doesn't jump over its init */

  if (!cur || !cur->next || !cur->next->next) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "(%s lib name return-type arg-type ...)", who);
    goto cleanup;
  }
  libname = EVAL(cur->content, env);
  if (iserror(libname)) {
    err = libname;
    libname = NULL;
    goto cleanup;
  }
  cur = cur->next;
  fnname = EVAL(cur->content, env);
  if (iserror(fnname)) {
    err = fnname;
    fnname = NULL;
    goto cleanup;
  }
  cur = cur->next;
  rtype = EVAL(cur->content, env);
  if (iserror(rtype)) {
    err = rtype;
    rtype = NULL;
    goto cleanup;
  }
  cur = cur->next;
  while (cur && n_a < ALC_FFI_MAX_ARGS) {
    atypes[n_a] = EVAL(cur->content, env);
    if (iserror(atypes[n_a])) {
      err = atypes[n_a];
      atypes[n_a] = NULL;
      goto cleanup;
    }
    n_a++;
    cur = cur->next;
  }
  /* If we hit the cap and there are still more arg types, refuse rather
     than silently truncate — a binding with the wrong arity reads stack
     garbage at call time. Bump ALC_FFI_MAX_ARGS if a real use case needs
     more than 8 args. */
  if (cur) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "%s: too many arg types (max %d supported)", who,
                ALC_FFI_MAX_ARGS);
    goto cleanup;
  }
  if (variadic && n_a < 1) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "ffi-vfn: need at least one fixed arg type before the "
                "variadic part (e.g. the format string)");
    goto cleanup;
  }

  if (!isstring(libname) || !isstring(fnname)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "%s: lib and name must be strings",
                who);
    goto cleanup;
  }
  void *h = alc_ffi_dlopen((char *)exp_text(libname));
  if (!h) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "%s: dlopen failed (%s)", who,
                dlerror());
    goto cleanup;
  }
  void *sym = dlsym(h, (char *)exp_text(fnname));
  if (!sym) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "%s: dlsym failed for %s", who,
                (char *)exp_text(fnname));
    goto cleanup;
  }

  alc_ffi_t *f = (alc_ffi_t *)memalloc(1, sizeof(alc_ffi_t));
  f->fn = sym;
  f->nargs = (unsigned int)n_a; /* variadic: this is the FIXED count */
  f->variadic = (uint8_t)variadic;
  alc_ffi_tag_t rt;
  ffi_type *rt_ffi;
  exp_t *rdesc;
  if (alc_ffi_resolve_type(rtype, &rt, &rt_ffi, &rdesc) < 0) {
    free(f);
    err = error(ERROR_ILLEGAL_VALUE, e, env, "%s: unknown return type", who);
    goto cleanup;
  }
  f->ret_tag = rt;
  f->rtype = rt_ffi;
  if (rdesc) /* struct-by-value return: hold the descriptor for sizing */
    f->ret_struct = refexp(rdesc);
  for (int i = 0; i < n_a; i++) {
    alc_ffi_tag_t at;
    ffi_type *at_ffi;
    exp_t *adesc;
    if (alc_ffi_resolve_type(atypes[i], &at, &at_ffi, &adesc) < 0) {
      alc_ffi_free(f);
      err = error(ERROR_ILLEGAL_VALUE, e, env,
                  "%s: unknown/invalid arg type at slot %d", who, i);
      goto cleanup;
    }
    f->arg_tags[i] = at;
    f->atypes[i] = at_ffi;
    if (adesc) /* struct-by-value arg: hold the descriptor for sizing */
      f->arg_structs[i] = refexp(adesc);
  }
  /* Non-variadic: prep the cif now (fixed signature). Variadic: the cif is
     built per call in alc_ffi_call once the variadic arg types are known. */
  if (!variadic && ffi_prep_cif(&f->cif, FFI_DEFAULT_ABI, f->nargs, f->rtype,
                                f->atypes) != FFI_OK) {
    alc_ffi_free(f);
    err = error(ERROR_ILLEGAL_VALUE, e, env, "%s: ffi_prep_cif failed", who);
    goto cleanup;
  }
  size_t dnlen =
      strlen((char *)exp_text(libname)) + strlen((char *)exp_text(fnname)) + 2;
  f->display_name = (char *)memalloc(dnlen, 1);
  snprintf(f->display_name, dnlen, "%s:%s", (char *)exp_text(libname),
           (char *)exp_text(fnname));

  INIT_TYPED(ret, EXP_FFI, f);

cleanup:
  unrefexp(libname);
  unrefexp(fnname);
  unrefexp(rtype);
  for (int i = 0; i < n_a; i++)
    unrefexp(atypes[i]);
  unrefexp(e);
  if (err)
    return err;
  return ret;
}

/* (ffi-fn lib name return-type arg-type ...) */
exp_t *ffifncmd(exp_t *e, env_t *env) { return ffi_bind_impl(e, env, 0); }

/* (ffi-vfn lib name return-type fixed-arg-type ...) — a variadic C function.
   The given arg types are the FIXED prefix; extra args supplied at the call
   are passed with types inferred from their alcove value (fixnum→long,
   float→double, char→int, string/nil→pointer). */
exp_t *ffivfncmd(exp_t *e, env_t *env) { return ffi_bind_impl(e, env, 1); }

/* libffi closure trampoline: C code calls this with the native args; we
   marshal them to alcove values, invoke the bound lambda, and marshal the
   result back into `ret`. user_data is the owning alc_ffi_t (kind CB).
   Registered via ffi_prep_closure_loc in fficallbackcmd. Portable across
   every libffi target — no arch-specific code here (libffi owns the
   executable-memory + ABI details, including macOS hardened-runtime). */
static void alc_ffi_closure_dispatch(ffi_cif *cif, void *ret, void **args,
                                     void *user) {
  (void)cif;
  alc_ffi_t *cb = (alc_ffi_t *)user;
  exp_t *argv[ALC_FFI_MAX_ARGS];
  unsigned int i;
  for (i = 0; i < cb->nargs; i++) {
    switch (cb->arg_tags[i]) {
    case AFFI_INT:
      argv[i] = MAKE_FIX((int64_t)*(int32_t *)args[i]);
      break;
    case AFFI_LONG:
      argv[i] = MAKE_FIX(*(int64_t *)args[i]);
      break;
    case AFFI_DOUBLE:
      argv[i] = make_floatf(*(double *)args[i]);
      break;
    case AFFI_STRING: {
      const char *s = *(const char **)args[i];
      argv[i] = s ? make_string((char *)s, (int)strnlen(s, 1u << 24)) : NIL_EXP;
      break;
    }
    case AFFI_PTR:
      argv[i] = MAKE_FIX((int64_t)(uintptr_t)*(void **)args[i]);
      break;
    default:
      argv[i] = NIL_EXP;
      break;
    }
  }
  /* Re-enter the evaluator from C. vm_invoke_values borrows fn and consumes
     the argv refs. Use the global env as the resolution root — the lambda
     carries its own captured env (next->meta) for its free vars. Save and
     restore the tail-position flag around the nested invocation. */
  int saved_tail = in_tail_position;
  in_tail_position = 0;
  exp_t *r =
      vm_invoke_values(cb->cb_lambda, (int)cb->nargs, argv, g_global_env);
  in_tail_position = saved_tail;

  /* Integral/pointer returns go through ffi_arg (the closure return slot is
     register-width); double has its own slot. String return is rejected at
     bind time (the buffer's lifetime can't outlive this call). */
  switch (cb->ret_tag) {
  case AFFI_VOID:
    break;
  case AFFI_INT:
  case AFFI_LONG:
    *(ffi_arg *)ret =
        (ffi_arg)(r ? (isfloat(r) ? (int64_t)r->f
                                  : (isnumber(r) || ischar(r) ? FIX_VAL(r) : 0))
                    : 0);
    break;
  case AFFI_DOUBLE:
    *(double *)ret = (r && (isfloat(r) || isnumber(r))) ? TO_DOUBLE(r) : 0.0;
    break;
  case AFFI_PTR:
    *(ffi_arg *)ret =
        (ffi_arg)(uintptr_t)((r && isnumber(r)) ? (void *)(uintptr_t)FIX_VAL(r)
                                                : NULL);
    break;
  default:
    *(ffi_arg *)ret = 0;
    break;
  }
  if (r)
    unrefexp(r);
}

/* (ffi-callback ret-type (arg-types...) fn) — wrap an alcove lambda in a
   libffi closure so it can be passed to C as a function pointer. */
exp_t *fficallbackcmd(exp_t *e, env_t *env) {
  exp_t *cur = e->next;
  exp_t *rtype = NULL, *atlist = NULL, *fn = NULL, *err = NULL, *ret = NULL;
  if (!cur || !cur->next || !cur->next->next) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "(ffi-callback ret-type (arg-types...) fn)");
    goto cleanup;
  }
  rtype = EVAL(cur->content, env);
  if (iserror(rtype)) {
    err = rtype;
    rtype = NULL;
    goto cleanup;
  }
  cur = cur->next;
  atlist = EVAL(cur->content, env);
  if (iserror(atlist)) {
    err = atlist;
    atlist = NULL;
    goto cleanup;
  }
  cur = cur->next;
  fn = EVAL(cur->content, env);
  if (iserror(fn)) {
    err = fn;
    fn = NULL;
    goto cleanup;
  }

  if (!isstring(rtype)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-callback: ret-type must be a string");
    goto cleanup;
  }
  if (!islambda(fn)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-callback: third arg must be a function");
    goto cleanup;
  }
  if (atlist && atlist != NIL_EXP && !ispair(atlist)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-callback: arg-types must be a list of type strings");
    goto cleanup;
  }
  alc_ffi_tag_t rt;
  ffi_type *rt_ffi;
  if (alc_ffi_typeof((char *)exp_text(rtype), &rt, &rt_ffi) < 0) {
    err =
        error(ERROR_ILLEGAL_VALUE, e, env,
              "ffi-callback: unknown return type %s", (char *)exp_text(rtype));
    goto cleanup;
  }
  if (rt == AFFI_STRING) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-callback: string return not supported (buffer lifetime)");
    goto cleanup;
  }
  alc_ffi_t *f = (alc_ffi_t *)memalloc(1, sizeof(alc_ffi_t));
  f->kind = AFFI_KIND_CB;
  f->ret_tag = rt;
  f->rtype = rt_ffi;
  int n = 0;
  for (exp_t *p = atlist; p && ispair(p); p = p->next) {
    if (n >= ALC_FFI_MAX_ARGS) {
      free(f);
      err =
          error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-callback: too many arg types (max %d)", ALC_FFI_MAX_ARGS);
      goto cleanup;
    }
    exp_t *tn = p->content;
    alc_ffi_tag_t at;
    ffi_type *at_ffi;
    if (!isstring(tn)) {
      free(f);
      err = error(ERROR_ILLEGAL_VALUE, e, env,
                  "ffi-callback: arg-type must be a string");
      goto cleanup;
    }
    if (alc_ffi_typeof((char *)exp_text(tn), &at, &at_ffi) < 0) {
      free(f);
      err = error(ERROR_ILLEGAL_VALUE, e, env,
                  "ffi-callback: unknown arg type %s", (char *)exp_text(tn));
      goto cleanup;
    }
    f->arg_tags[n] = at;
    f->atypes[n] = at_ffi;
    n++;
  }
  f->nargs = (unsigned int)n;
  if (ffi_prep_cif(&f->cif, FFI_DEFAULT_ABI, f->nargs, f->rtype, f->atypes) !=
      FFI_OK) {
    free(f);
    err =
        error(ERROR_ILLEGAL_VALUE, e, env, "ffi-callback: ffi_prep_cif failed");
    goto cleanup;
  }
  /* ffi_closure_alloc returns executable memory + writes the callable entry
     into f->code; ffi_prep_closure_loc binds the cif + dispatcher to it. */
  f->closure = ffi_closure_alloc(sizeof(ffi_closure), &f->code);
  if (!f->closure) {
    free(f);
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-callback: closure alloc failed");
    goto cleanup;
  }
  if (ffi_prep_closure_loc(f->closure, &f->cif, alc_ffi_closure_dispatch, f,
                           f->code) != FFI_OK) {
    ffi_closure_free(f->closure);
    free(f);
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-callback: ffi_prep_closure_loc failed");
    goto cleanup;
  }
  f->cb_lambda = refexp(fn);
  f->display_name = strdup("<callback>");
  INIT_TYPED(ret, EXP_FFI, f);

cleanup:
  unrefexp(rtype);
  unrefexp(atlist);
  unrefexp(fn);
  unrefexp(e);
  return err ? err : ret;
}

/* (ffi-struct field-type-str...) — define a by-value C struct type. Fields
   are scalar type names (int long double ptr). Computes the ABI layout
   (offsets + size) with the standard scalar-aggregate alignment rule, which
   matches the C ABI on every libffi target for non-packed scalar members —
   so no dependency on ffi_get_struct_offsets (libffi 3.3+). */
exp_t *ffistructcmd(exp_t *e, env_t *env) {
  exp_t *cur = e->next;
  exp_t *err = NULL, *ret = NULL;
  exp_t *fields[ALC_FFI_MAX_ARGS] = {0};
  int nf = 0;
  while (cur && nf < ALC_FFI_MAX_ARGS) {
    fields[nf] = EVAL(cur->content, env);
    if (iserror(fields[nf])) {
      err = fields[nf];
      fields[nf] = NULL;
      goto cleanup;
    }
    nf++;
    cur = cur->next;
  }
  if (cur) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-struct: too many fields (max %d)", ALC_FFI_MAX_ARGS);
    goto cleanup;
  }
  if (nf == 0) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "ffi-struct: need at least one field type");
    goto cleanup;
  }
  alc_ffi_t *f = (alc_ffi_t *)memalloc(1, sizeof(alc_ffi_t));
  f->kind = AFFI_KIND_STRUCT;
  size_t off = 0, align = 1;
  for (int i = 0; i < nf; i++) {
    alc_ffi_tag_t t;
    ffi_type *ft;
    size_t fsize, falign;
    if (ishamt(fields[i])) { /* defensive: not a valid field */
      alc_ffi_free(f);
      err =
          error(ERROR_ILLEGAL_VALUE, e, env, "ffi-struct: invalid field type");
      goto cleanup;
    }
    if (isffi(fields[i]) &&
        ((alc_ffi_t *)fields[i]->ptr)->kind == AFFI_KIND_STRUCT) {
      /* nested struct field: reuse its ffi_type + computed size/alignment,
         and hold an owning ref to the nested descriptor for pack/unpack. */
      alc_ffi_t *nd = (alc_ffi_t *)fields[i]->ptr;
      t = AFFI_STRUCT;
      ft = &nd->struct_type;
      fsize = nd->struct_size;
      falign = nd->struct_align;
      f->arg_structs[i] = refexp(fields[i]);
    } else if (isstring(fields[i]) &&
               alc_ffi_typeof((char *)exp_text(fields[i]), &t, &ft) == 0 &&
               t != AFFI_VOID && t != AFFI_STRING) {
      fsize = ft->size;
      falign = ft->alignment;
    } else {
      alc_ffi_free(f);
      err = error(ERROR_ILLEGAL_VALUE, e, env,
                  "ffi-struct: field must be int/long/double/ptr or a struct "
                  "descriptor");
      goto cleanup;
    }
    f->arg_tags[i] = (uint8_t)t;
    f->atypes[i] = ft;
    f->elements[i] = ft;
    off = (off + falign - 1) & ~(falign - 1);
    f->offsets[i] = off;
    off += fsize;
    if (falign > align)
      align = falign;
  }
  f->elements[nf] = NULL;
  f->nargs = (unsigned int)nf;
  f->struct_size = (off + align - 1) & ~(align - 1);
  f->struct_align = align;
  /* Leave size/alignment 0 so ffi_prep_cif computes the authoritative
     in-cif layout when this descriptor is used as an ffi-fn arg/return; our
     manual offsets/struct_size drive pack/unpack and match it for scalars. */
  f->struct_type.size = 0;
  f->struct_type.alignment = 0;
  f->struct_type.type = FFI_TYPE_STRUCT;
  f->struct_type.elements = f->elements;
  f->display_name = strdup("<struct>");
  INIT_TYPED(ret, EXP_FFI, f);
cleanup:
  for (int i = 0; i < nf; i++)
    unrefexp(fields[i]);
  unrefexp(e);
  return err ? err : ret;
}

/* (ffi-pack struct-desc vals...) — pack scalar field values into a blob laid
   out per the descriptor's ABI layout. */
exp_t *ffipackcmd(exp_t *e, env_t *env) {
  exp_t *cur = e->next;
  exp_t *err = NULL, *ret = NULL, *desc = NULL;
  exp_t *vals[ALC_FFI_MAX_ARGS] = {0};
  int nv = 0;
  if (!cur) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "(ffi-pack struct-desc vals...)");
    goto cleanup;
  }
  desc = EVAL(cur->content, env);
  if (iserror(desc)) {
    err = desc;
    desc = NULL;
    goto cleanup;
  }
  cur = cur->next;
  while (cur && nv < ALC_FFI_MAX_ARGS) {
    vals[nv] = EVAL(cur->content, env);
    if (iserror(vals[nv])) {
      err = vals[nv];
      vals[nv] = NULL;
      goto cleanup;
    }
    nv++;
    cur = cur->next;
  }
  if (cur) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-pack: too many values (max %d)", ALC_FFI_MAX_ARGS);
    goto cleanup;
  }
  if (!isffi(desc) || ((alc_ffi_t *)desc->ptr)->kind != AFFI_KIND_STRUCT) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-pack: first arg must be an ffi-struct descriptor");
    goto cleanup;
  }
  alc_ffi_t *d = (alc_ffi_t *)desc->ptr;
  if ((unsigned int)nv != d->nargs) {
    err = error(ERROR_MISSING_PARAMETER, e, env,
                "ffi-pack: expected %u fields, got %d", d->nargs, nv);
    goto cleanup;
  }
  char *buf = (char *)memalloc(d->struct_size ? d->struct_size : 1, 1);
  for (int i = 0; i < nv; i++) {
    exp_t *v = vals[i];
    void *slot = buf + d->offsets[i];
    switch (d->arg_tags[i]) {
    case AFFI_INT:
      if (!(isnumber(v) || isfloat(v) || ischar(v)))
        goto badfield;
      *(int32_t *)slot = isnumber(v) ? (int32_t)FIX_VAL(v)
                         : ischar(v) ? (int32_t)CHAR_VAL(v)
                                     : (int32_t)v->f;
      break;
    case AFFI_LONG:
      if (!(isnumber(v) || isfloat(v) || ischar(v)))
        goto badfield;
      *(int64_t *)slot = isnumber(v) ? FIX_VAL(v)
                         : ischar(v) ? (int64_t)CHAR_VAL(v)
                                     : (int64_t)v->f;
      break;
    case AFFI_DOUBLE:
      if (!(isnumber(v) || isfloat(v) || ischar(v)))
        goto badfield;
      *(double *)slot = isfloat(v)  ? v->f
                        : ischar(v) ? (double)CHAR_VAL(v)
                                    : (double)FIX_VAL(v);
      break;
    case AFFI_PTR:
      if (!(isnumber(v) || v == NIL_EXP))
        goto badfield;
      *(void **)slot = (v == NIL_EXP) ? NULL : (void *)(uintptr_t)FIX_VAL(v);
      break;
    case AFFI_STRUCT: {
      /* nested struct field: v is a blob packed to the nested layout. */
      alc_ffi_t *nd =
          d->arg_structs[i] ? (alc_ffi_t *)d->arg_structs[i]->ptr : NULL;
      if (!nd || !isblob(v) || blob_len(v) != nd->struct_size)
        goto badfield;
      memcpy(slot, blob_bytes(v), nd->struct_size);
      break;
    }
    default:
      goto badfield;
    }
    continue;
  badfield:
    free(buf);
    err =
        error(ERROR_ILLEGAL_VALUE, e, env, "ffi-pack: field %d wrong type", i);
    goto cleanup;
  }
  ret = make_blob(buf, d->struct_size);
  free(buf);
cleanup:
  unrefexp(desc);
  for (int i = 0; i < nv; i++)
    unrefexp(vals[i]);
  unrefexp(e);
  return err ? err : ret;
}

/* (ffi-unpack struct-desc blob) — inverse of ffi-pack: read each field into
   a list of alcove values. */
exp_t *ffiunpackcmd(exp_t *e, env_t *env) {
  exp_t *err = NULL, *ret = NULL, *desc = NULL, *blob = NULL;
  if (!e->next || !e->next->next) {
    err =
        error(ERROR_MISSING_PARAMETER, e, env, "(ffi-unpack struct-desc blob)");
    goto cleanup;
  }
  desc = EVAL(e->next->content, env);
  if (iserror(desc)) {
    err = desc;
    desc = NULL;
    goto cleanup;
  }
  blob = EVAL(e->next->next->content, env);
  if (iserror(blob)) {
    err = blob;
    blob = NULL;
    goto cleanup;
  }
  if (!isffi(desc) || ((alc_ffi_t *)desc->ptr)->kind != AFFI_KIND_STRUCT) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-unpack: first arg must be an ffi-struct descriptor");
    goto cleanup;
  }
  if (!isblob(blob)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-unpack: second arg must be a blob");
    goto cleanup;
  }
  alc_ffi_t *d = (alc_ffi_t *)desc->ptr;
  if (blob_len(blob) < d->struct_size) {
    err = error(ERROR_ILLEGAL_VALUE, e, env,
                "ffi-unpack: blob too small (%zu < %zu)", blob_len(blob),
                d->struct_size);
    goto cleanup;
  }
  const char *buf = blob_bytes(blob);
  exp_t *head = NULL, *tail = NULL;
  for (unsigned int i = 0; i < d->nargs; i++) {
    const void *slot = buf + d->offsets[i];
    exp_t *v;
    switch (d->arg_tags[i]) {
    case AFFI_INT:
      v = MAKE_FIX((int64_t)*(const int32_t *)slot);
      break;
    case AFFI_LONG:
      v = MAKE_FIX(*(const int64_t *)slot);
      break;
    case AFFI_DOUBLE:
      v = make_floatf(*(const double *)slot);
      break;
    case AFFI_PTR:
      v = MAKE_FIX((int64_t)(uintptr_t)*(void *const *)slot);
      break;
    case AFFI_STRUCT: { /* nested struct → blob of its bytes (ffi-unpack again)
                         */
      alc_ffi_t *nd =
          d->arg_structs[i] ? (alc_ffi_t *)d->arg_structs[i]->ptr : NULL;
      v = nd ? make_blob(slot, nd->struct_size) : NIL_EXP;
      break;
    }
    default:
      v = NIL_EXP;
      break;
    }
    exp_t *node = make_node(v);
    if (!head) {
      head = node;
      tail = node;
    } else {
      tail->next = node;
      tail = node;
    }
  }
  ret = head ? head : NIL_EXP;
cleanup:
  unrefexp(desc);
  unrefexp(blob);
  unrefexp(e);
  return err ? err : ret;
}

void alc_ffi_free(void *ptr) {
  alc_ffi_t *f = (alc_ffi_t *)ptr;
  if (!f)
    return;
  if (f->kind == AFFI_KIND_CB) {
    if (f->closure)
      ffi_closure_free(f->closure);
    if (f->cb_lambda)
      unrefexp(f->cb_lambda);
  }
  /* FN bindings hold a ref to the descriptor of each struct-by-value
     arg/return (and STRUCT/CB kinds leave these NULL). */
  for (int i = 0; i < ALC_FFI_MAX_ARGS; i++)
    if (f->arg_structs[i])
      unrefexp(f->arg_structs[i]);
  if (f->ret_struct)
    unrefexp(f->ret_struct);
  if (f->display_name)
    free(f->display_name);
  free(f);
}

/* Marshal alcove args → C, ffi_call, marshal return. */
/* Infer the ffi type of a variadic call argument from its alcove runtime
   type, applying C's default argument promotions. alcove integers are 64-bit,
   so a fixnum is passed as a long — use %ld in format strings; floats become
   double; chars become int; strings and nil become pointers. Returns -1 if
   the value can't be passed as a vararg. */
static int alc_ffi_infer(exp_t *a, alc_ffi_tag_t *tag, ffi_type **out) {
  if (isnumber(a)) {
    *tag = AFFI_LONG;
    *out = &ffi_type_sint64;
    return 0;
  }
  if (isfloat(a)) {
    *tag = AFFI_DOUBLE;
    *out = &ffi_type_double;
    return 0;
  }
  if (ischar(a)) {
    *tag = AFFI_INT;
    *out = &ffi_type_sint32;
    return 0;
  }
  if (isstring(a)) {
    *tag = AFFI_STRING;
    *out = &ffi_type_pointer;
    return 0;
  }
  if (a == NIL_EXP) {
    *tag = AFFI_PTR;
    *out = &ffi_type_pointer;
    return 0;
  }
  return -1;
}

static exp_t *alc_ffi_call(alc_ffi_t *f, int nargs, exp_t **args) {
  /* Variadic: f->nargs is the FIXED count — require at least that many and at
     most the slot cap. Non-variadic: exact arity. */
  int bad_arity = f->variadic
                      ? (nargs < (int)f->nargs || nargs > ALC_FFI_MAX_ARGS)
                      : ((unsigned int)nargs != f->nargs);
  if (bad_arity) {
    int i;
    for (i = 0; i < nargs; i++)
      unrefexp(args[i]);
    return error(ERROR_MISSING_PARAMETER, NULL, NULL,
                 "ffi: wrong arg count for %s (expected %s%u, got %d)",
                 f->display_name ? f->display_name : "?",
                 f->variadic ? ">=" : "", f->nargs, nargs);
  }
  /* Slot storage. Avoid stack discipline issues by using one union per arg. */
  union {
    int32_t i;
    int64_t l;
    double d;
    const char *s;
    void *p;
  } slots[ALC_FFI_MAX_ARGS];
  void *avalues[ALC_FFI_MAX_ARGS];
  /* Per-call arg tags + ffi types. For non-variadic and the fixed prefix of a
     variadic call these come from the binding; extra variadic args are
     inferred from their value. */
  uint8_t tags[ALC_FFI_MAX_ARGS];
  ffi_type *atvec[ALC_FFI_MAX_ARGS];
  for (int i = 0; i < nargs; i++) {
    if (i < (int)f->nargs) {
      tags[i] = f->arg_tags[i];
      atvec[i] = f->atypes[i];
    } else {
      alc_ffi_tag_t t;
      if (alc_ffi_infer(args[i], &t, &atvec[i]) < 0) {
        for (int j = 0; j < nargs; j++)
          unrefexp(args[j]);
        return error(ERROR_ILLEGAL_VALUE, NULL, NULL,
                     "ffi: %s: variadic arg %d cannot be passed (need "
                     "number/float/char/string/nil)",
                     f->display_name ? f->display_name : "?", i);
      }
      tags[i] = (uint8_t)t;
    }
  }
  /* Type-mismatched args used to silently coerce to 0/NULL — calling
     a strlen-binding with a number then crashed in C. Now we refuse
     up front with a clear error so the caller knows which slot is
     wrong instead of seeing a SIGSEGV deep in libc. */
  for (int i = 0; i < nargs; i++) {
    exp_t *a = args[i];
    int ok = 0;
    switch (tags[i]) {
    case AFFI_INT:
    case AFFI_LONG:
      /* Chars (tagged immediates) are a natural fit for int args:
         shims that take ASCII codes via C's `int` convention should
         accept (gfx-text-set i (s i)) without forcing the caller to
         hand-convert. The numeric value of a char is its codepoint. */
      ok = isnumber(a) || isfloat(a) || ischar(a);
      break;
    case AFFI_DOUBLE:
      ok = isnumber(a) || isfloat(a) || ischar(a);
      break;
    case AFFI_STRING:
      ok = isstring(a);
      break;
    case AFFI_PTR:
      /* A raw address (fixnum), nil (NULL), an ffi-callback (its code pointer),
         a blob (its bytes), or an f64 vector (its cells — zero-copy, so a C shim
         can read/write a whole buffer alcove computed). */
      ok = isnumber(a) || a == NIL_EXP ||
           (isffi(a) && ((alc_ffi_t *)a->ptr)->kind == AFFI_KIND_CB) ||
           isblob(a) || (isvector(a) && vec_kind(a) == VEC_KIND_F64);
      break;
    case AFFI_STRUCT: {
      /* A struct-by-value arg is a blob packed to the declared layout. */
      alc_ffi_t *d =
          f->arg_structs[i] ? (alc_ffi_t *)f->arg_structs[i]->ptr : NULL;
      ok = d && isblob(a) && blob_len(a) == d->struct_size;
      break;
    }
    default:
      ok = 1;
      break;
    }
    if (!ok) {
      int j;
      for (j = 0; j < nargs; j++)
        unrefexp(args[j]);
      return error(ERROR_ILLEGAL_VALUE, NULL, NULL,
                   "ffi: %s: arg %d wrong type for tag %d",
                   f->display_name ? f->display_name : "?", i, (int)tags[i]);
    }
  }
  for (int i = 0; i < nargs; i++) {
    exp_t *a = args[i];
    switch (tags[i]) {
    case AFFI_INT:
      slots[i].i = isnumber(a) ? (int32_t)FIX_VAL(a)
                   : ischar(a) ? (int32_t)CHAR_VAL(a)
                               : (int32_t)a->f;
      avalues[i] = &slots[i].i;
      break;
    case AFFI_LONG:
      slots[i].l = isnumber(a) ? FIX_VAL(a)
                   : ischar(a) ? (int64_t)CHAR_VAL(a)
                               : (int64_t)a->f;
      avalues[i] = &slots[i].l;
      break;
    case AFFI_DOUBLE:
      slots[i].d = isfloat(a)  ? a->f
                   : ischar(a) ? (double)CHAR_VAL(a)
                               : (double)FIX_VAL(a);
      avalues[i] = &slots[i].d;
      break;
    case AFFI_STRING:
      slots[i].s = (const char *)exp_text(a);
      avalues[i] = &slots[i].s;
      break;
    case AFFI_PTR:
      if (a == NIL_EXP)
        slots[i].p = NULL;
      else if (isffi(a))
        slots[i].p = ((alc_ffi_t *)a->ptr)->code; /* callback fn pointer */
      else if (isblob(a))
        slots[i].p = (void *)blob_bytes(a); /* blob's raw bytes */
      else if (isvector(a) && vec_kind(a) == VEC_KIND_F64)
        slots[i].p = (void *)VEC_F64_CELLS(a); /* f64 vector's cells (zero-copy) */
      else
        slots[i].p = (void *)(uintptr_t)FIX_VAL(a); /* integer address */
      avalues[i] = &slots[i].p;
      break;
    case AFFI_STRUCT:
      /* Pass the packed bytes directly; libffi copies struct_size bytes per
         the cif. (Validated as a correctly-sized blob in the check above.) */
      avalues[i] = (void *)(uintptr_t)blob_bytes(a);
      break;
    default:
      slots[i].l = 0;
      avalues[i] = &slots[i].l;
      break;
    }
  }
  /* Variadic calls build their cif here, now that the per-call arg types are
     known (ffi_prep_cif_var needs the full type vector and the fixed count).
     Non-variadic calls reuse the cif prepped at bind time. */
  ffi_cif vcif;
  ffi_cif *cif = &f->cif;
  if (f->variadic) {
    if (ffi_prep_cif_var(&vcif, FFI_DEFAULT_ABI, f->nargs, (unsigned int)nargs,
                         f->rtype, atvec) != FFI_OK) {
      for (int j = 0; j < nargs; j++)
        unrefexp(args[j]);
      return error(ERROR_ILLEGAL_VALUE, NULL, NULL,
                   "ffi: %s: ffi_prep_cif_var failed",
                   f->display_name ? f->display_name : "?");
    }
    cif = &vcif;
  }
  union {
    int32_t i;
    int64_t l;
    double d;
    void *p;
  } rval;
  /* Struct-by-value return needs a buffer sized to the struct (at least
     ffi_arg, libffi's minimum return slot). Scalars use the union above. */
  void *rvalue = &rval;
  char *struct_buf = NULL;
  size_t struct_ret_size = 0;
  if (f->ret_tag == AFFI_STRUCT && f->ret_struct) {
    struct_ret_size = ((alc_ffi_t *)f->ret_struct->ptr)->struct_size;
    size_t bufsz =
        struct_ret_size < sizeof(ffi_arg) ? sizeof(ffi_arg) : struct_ret_size;
    struct_buf = (char *)memalloc(bufsz ? bufsz : 1, 1);
    rvalue = struct_buf;
  }
  ffi_call(cif, FFI_FN(f->fn), rvalue, avalues);
  exp_t *ret = NIL_EXP;
  switch (f->ret_tag) {
  case AFFI_VOID:
    ret = NIL_EXP;
    break;
  case AFFI_INT:
    ret = MAKE_FIX((int64_t)rval.i);
    break;
  case AFFI_LONG:
    ret = MAKE_FIX(rval.l);
    break;
  case AFFI_DOUBLE:
    ret = make_floatf(rval.d);
    break;
  case AFFI_STRING: {
    /* strlen on an arbitrary returned pointer is a footgun — if the C
       function returned a non-string or a non-NUL-terminated buffer
       we'd OOB-read the heap. Use strnlen with a generous cap so
       runaway strings get truncated instead of crashing. */
    if (!rval.p) {
      ret = NIL_EXP;
    } else {
      size_t len = strnlen((const char *)rval.p, 1u << 24);
      ret = make_string((char *)rval.p, (int)len);
    }
    break;
  }
  case AFFI_PTR:
    ret = MAKE_FIX((int64_t)(uintptr_t)rval.p);
    break;
  case AFFI_STRUCT:
    /* Hand the returned struct bytes back as a blob (ffi-unpack reads it). */
    ret = make_blob(struct_buf, struct_ret_size);
    break;
  }
  if (struct_buf)
    free(struct_buf);
  for (int i = 0; i < nargs; i++)
    unrefexp(args[i]);
  return ret;
}
#else  /* !ALCOVE_FFI */
exp_t *ffifncmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-fn: alcove built without libffi (install libffi-dev "
               "and rebuild).");
}
exp_t *ffivfncmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-vfn: alcove built without libffi.");
}
exp_t *fficallbackcmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-callback: alcove built without libffi (install "
               "libffi-dev and rebuild).");
}
exp_t *ffistructcmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-struct: alcove built without libffi.");
}
exp_t *ffipackcmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-pack: alcove built without libffi.");
}
exp_t *ffiunpackcmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "ffi-unpack: alcove built without libffi.");
}
void alc_ffi_free(void *ptr) {
  (void)ptr;
} /* called from unrefexp; no FFI exp can exist */
#endif /* ALCOVE_FFI */
