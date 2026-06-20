/* env.h — environment (env_t) lifecycle: ref_env, make_env (arena-bump
 * allocation off the current shard), the self-closure cycle detector/breaker,
 * and destroy_env. FRAGMENT #included into alcove.c after the shard globals it
 * uses (current_shard) — the hot inline make_env/ref_env stay inlined into the
 * call path (single TU). The eval-coupled `lookup` resolver and the shard/
 * reactor runtime stay in alcove.c. NOT standalone, NOT separately compiled.
 */

/* Walk dict buckets. `d` is the dict_t*, `kv` the loop-variable name bound to
   each keyval_t* in turn, and [ht_lo, ht_hi] the inclusive ht[] index range to
   walk: 0,0 for ht[0] only (the set/value walks); 0,1 for both ht[0] and ht[1]
   (env scans both halves of an in-progress rehash). MACRO form so the hot
   set-algebra bulk loops inline identically to the hand-rolled walk. Defined
   here (the first-#included container fragment) so dict.h/set.h see it too;
   used ONLY within the container fragments (env/dict/set). */
#define DICT_FOREACH(d, kv, ht_lo, ht_hi)                                      \
  for (unsigned int _alc_h = (ht_lo); _alc_h <= (unsigned int)(ht_hi);         \
       _alc_h++)                                                               \
    for (unsigned long _alc_b = 0; _alc_b < (d)->ht[_alc_h].size; _alc_b++)    \
      for (keyval_t *kv = (d)->ht[_alc_h].table[_alc_b]; kv; kv = kv->next)

/* Type-guard for builtins: if `var` fails predicate `pred`, build an error
   from the trailing error(...) args (bound to `_alc_e`) and run `cleanup` —
   the call site's CLEAN_RETURN_n with the right arity, e.g.
   REQUIRE_TYPE(b, isblob, CLEAN_RETURN_1(b, _alc_e),
                ERROR_ILLEGAL_VALUE, NULL, env, "blob-ref: not a blob").
   Factors the `if (!isX(v)) CLEAN_RETURN(error(...))` guards across the
   container fragments. Used ONLY within these fragments. */
#define REQUIRE_TYPE(var, pred, cleanup, ...)                                  \
  do {                                                                         \
    if (!pred(var)) {                                                          \
      exp_t *_alc_e = error(__VA_ARGS__);                                      \
      cleanup;                                                                 \
    }                                                                          \
  } while (0)

/* Two-argument both-must-be-strings guard (the binary-string builtins:
   string-contains?/index, starts-with?/ends-with?, setenv, rename-file). Same
   shape as REQUIRE_TYPE — `cleanup` runs the caller's CLEAN_RETURN_n with
   _alc_e, the trailing args are the error() call. */
#define REQUIRE_2_STRINGS(a, b, cleanup, ...)                                  \
  do {                                                                         \
    if (!isstring(a) || !isstring(b)) {                                        \
      exp_t *_alc_e = error(__VA_ARGS__);                                      \
      cleanup;                                                                 \
    }                                                                          \
  } while (0)

inline env_t *ref_env(env_t *env) {
  if (env) {
    REFCOUNT_INC(&env->nref);
  }
  return env;
}

inline env_t *make_env(env_t *rootenv) {
  /* No memset here: destroy_env leaves reused arena slots with the
     fields that could carry stale state (callingfnc, d, n_inline)
     already cleared. Fresh arena slots (first use) are BSS-zeroed.
     Heap-fallback slots come from memalloc which calloc's. Saves a
     ~128-byte store per call — the biggest per-call cost on fib. */
  env_t *newenv;
  shard_t *sh = current_shard;
  if (sh->arena_sp < sh->arena_end) {
    newenv = sh->arena_sp++;
  } else {
    newenv = memalloc(1, sizeof(env_t));
  }
  newenv->root = ref_env(rootenv);
  newenv->nref = 1;
  return newenv;
}

/* True for a closure that captured THIS env (its body wrapper's meta points
   back at env) — the lambda→env half of a (def/let f (fn ...)) cycle.
   FLAG_SHARED closures are excluded: severing their refs non-atomically
   would be unsafe under the multi-thread build, so we leave them alone. */
static inline int is_self_closure(exp_t *v, env_t *env) {
  return v && is_ptr(v) && (v->type == EXP_LAMBDA || v->type == EXP_MACRO) &&
         !(v->flags & FLAG_SHARED) && v->next && (env_t *)v->next->meta == env;
}

/* Reclaim a self-referential closure cycle that manual refcounting cannot.
   A closure created with (def/let f (fn ...)) inside a function body captures
   its frame (f->next->meta == env, an owned ref) while the frame owns f (its
   name binding) — a 2-node strong cycle. When the ONLY refs still keeping env
   alive are such closures, each owned SOLELY by env, sever the closure→env
   edges so env — and then, via the normal dict/inline unref below, the
   closures — can be freed.

   `residual` is env->nref AFTER the frame's own ref was dropped. We collect
   only when residual == (count of solely-env-owned self-closures): any other
   holder of env (an anonymous escaped closure that captured env but isn't
   bound here, or a live child env) makes residual exceed that count, and any
   self-closure with an extra referrer (e.g. it was returned) has nref != 1 —
   both cases bail, leaving live data untouched. Returns 1 if it severed the
   cycle (env is now collectible), 0 to leave the early-break intact. */
static inline int env_break_self_cycle(env_t *env, int residual) {
  int self_refs = 0;
  for (int i = 0; i < env->n_inline; i++) {
    exp_t *v = env->inline_vals[i];
    if (!is_self_closure(v, env))
      continue;
    if (v->nref != 1)
      return 0; /* owned elsewhere too → still live */
    self_refs++;
  }
  if (env->d)
    DICT_FOREACH(env->d, k, 0, 1) {
      if (!is_self_closure(k->val, env))
        continue;
      if (k->val->nref != 1)
        return 0;
      self_refs++;
    }
  /* Every remaining ref to env must be a self-closure back-ref. */
  if (self_refs == 0 || self_refs != residual)
    return 0;
  /* Sever each closure→env edge. unrefexp's closure-free path keys on
     next->meta to release the captured env; NULLing it makes the dict/inline
     unref below free the closure without re-entering destroy_env(env). */
  for (int i = 0; i < env->n_inline; i++)
    if (is_self_closure(env->inline_vals[i], env))
      env->inline_vals[i]->next->meta = NULL;
  if (env->d)
    DICT_FOREACH(env->d, k, 0, 1)
  if (is_self_closure(k->val, env))
    k->val->next->meta = NULL;
  return 1;
}

inline void *destroy_env(env_t *env) {
  /* Iterative release — each env holds a ref to its parent via
     make_env/ref_env. Recursing would blow the C stack on deep call chains.
     Also scrubs the fields that would carry stale state into a reused
     arena slot, so make_env can skip the wholesale memset.
     Cache the shard pointer once: TLS reads on each loop iteration
     showed up as a measurable hit on nqueens-vec. */
  shard_t *sh = current_shard;
  while (env) {
    env_t *parent = env->root;
    int residual = REFCOUNT_DEC(&env->nref);
    /* residual > 0 normally means "still referenced, stop". But a closure
       defined in this env can hold the env's only outstanding ref via a
       refcount cycle (see env_break_self_cycle); if so, sever it and fall
       through to free env. Otherwise honor the early-break. */
    /* has_closure gates the scan: a self-closure can only exist if some
       lambda/macro captured this env (the flag's set sites), so when it's
       clear env_break_self_cycle would provably return 0 — skip it. This is
       the common case on every non-closure frame teardown. */
    if (residual > 0 &&
        (!env->has_closure || !env_break_self_cycle(env, residual)))
      break;
    {
      int i;
      for (i = 0; i < env->n_inline; i++)
        unrefexp(env->inline_vals[i]);
    }
    if (env->d)
      destroy_dict(env->d);
    if (env->callingfnc)
      unrefexp(env->callingfnc);
    /* Arena envs: roll the bump pointer back (LIFO) and scrub the
       fields that would carry stale state into the slot's next tenant,
       so make_env can skip the wholesale memset.
       Heap-fallback envs return to free() — no scrub needed. */
    if (env >= sh->arena && env < sh->arena_end) {
      env->n_inline = 0;
      env->has_closure =
          0; /* scrub: make_env skips the memset, relies on this */
      env->d = NULL;
      env->callingfnc = NULL;
      if (env + 1 == sh->arena_sp)
        sh->arena_sp = env;
    } else {
      free(env);
    }
    env = parent;
  }
  return NULL;
}
