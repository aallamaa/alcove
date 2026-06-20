/* builtins_control.h — control-flow builtins: cond, match (structural
 * pattern matching), and the generator builtins (yield-style lazy sequences).
 * FRAGMENT #included into alcove.c (single TU); cmd bodies reached via the
 * lispProcList function pointers, prototypes in builtins.h. NOT standalone.
 */
/* ---- cond ---------------------------------------------------------------- */

const char doc_cond[] =
    "(cond test1 expr1 test2 expr2 ... default) — Arc-style flat cond. "
    "Evaluates tests left-to-right; returns the expr paired with the first "
    "truthy test. A lone trailing element is the unconditional default. "
    "(cond) returns nil.";
exp_t *condcmd(exp_t *e, env_t *env) {
  int outer_tail = in_tail_position;
  exp_t *cur = cdr(e);
  while (cur) {
    if (!cur->next) {
      /* Lone trailing element: default */
      in_tail_position = outer_tail;
      exp_t *ret = EVAL(car(cur), env);
      unrefexp(e);
      return ret ? ret : NIL_EXP;
    }
    in_tail_position = 0;
    exp_t *test = EVAL(car(cur), env);
    if (iserror(test)) {
      in_tail_position = outer_tail;
      unrefexp(e);
      return test;
    }
    int truthy = istrue(test);
    unrefexp(test);
    if (truthy) {
      in_tail_position = outer_tail;
      exp_t *ret = EVAL(car(cur->next), env);
      unrefexp(e);
      return ret ? ret : NIL_EXP;
    }
    cur = cur->next ? cur->next->next : NULL;
  }
  in_tail_position = outer_tail;
  unrefexp(e);
  return NIL_EXP;
}

/* ---- match --------------------------------------------------------------- */

/* Forward declaration for mutual recursion in list pattern matching. */
static int alc_match_pat(exp_t *pat, exp_t *val, env_t *newenv, exp_t *e_err,
                         env_t *eval_env, exp_t **err);

static int alc_match_list_pats(exp_t *pats, exp_t *vals, env_t *newenv,
                               exp_t *e_err, env_t *eval_env, exp_t **err) {
  exp_t *p = pats, *v = vals;
  while (p && p->content) {
    if (!ispair(v) || !istrue(v))
      return 0; /* fewer values than patterns */
    if (!alc_match_pat(p->content, v->content, newenv, e_err, eval_env, err))
      return *err ? -1 : 0;
    if (*err)
      return -1;
    p = p->next;
    v = v ? v->next : NULL;
  }
  /* Exact length: value list must also be exhausted. */
  return (!v || v == NIL_EXP || !istrue(v)) ? 1 : 0;
}

/* Returns 1 on match (variables bound into newenv), 0 on no-match,
   -1 on structural error (sets *err). All refs borrowed. */
static int alc_match_pat(exp_t *pat, exp_t *val, env_t *newenv, exp_t *e_err,
                         env_t *eval_env, exp_t **err) {
  *err = NULL;

  /* Wildcard `_` */
  if (issymbol(pat) && strcmp((char *)exp_text(pat), "_") == 0)
    return 1;

  /* Literal nil — matches nil/empty-list */
  if (!pat || pat == NIL_EXP || !istrue(pat))
    return (!val || val == NIL_EXP || !istrue(val)) ? 1 : 0;

  /* Literal t — matches t */
  if (pat == TRUE_EXP)
    return (val == TRUE_EXP) ? 1 : 0;

  /* Fixnum literal */
  if (isnumber(pat))
    return (isnumber(val) && FIX_VAL(pat) == FIX_VAL(val)) ? 1 : 0;

  /* Float literal */
  if (isfloat(pat))
    return (isfloat(val) && pat->f == val->f) ? 1 : 0;

  /* String literal */
  if (isstring(pat))
    return (isstring(val) &&
            strcmp((char *)exp_text(pat), (char *)exp_text(val)) == 0)
               ? 1
               : 0;

  /* Plain symbol: capture binding — var2env_bind is forward-declared static
     at file scope above; the in-body extern was UB (C11 §6.2.2p7). */
  if (issymbol(pat)) {
    var2env_bind((char *)exp_text(pat), refexp(val ? val : NIL_EXP), newenv);
    return 1;
  }

  /* Compound pattern: must be a pair */
  if (!ispair(pat) || !istrue(pat))
    return 0;

  exp_t *head = pat->content;
  exp_t *rest = pat->next;

  if (issymbol(head)) {
    const char *hn = (const char *)exp_text(head);

    /* (quote sym) — match a specific symbol */
    if (strcmp(hn, "quote") == 0) {
      if (!rest || !rest->content) {
        *err = error(ERROR_ILLEGAL_VALUE, e_err, eval_env,
                     "match: (quote sym) needs an argument");
        return -1;
      }
      exp_t *q = rest->content;
      return (issymbol(q) && issymbol(val) &&
              strcmp((char *)exp_text(q), (char *)exp_text(val)) == 0)
                 ? 1
                 : 0;
    }

    /* (? pred) — guard: call pred on val */
    if (strcmp(hn, "?") == 0) {
      if (!rest || !rest->content) {
        *err = error(ERROR_ILLEGAL_VALUE, e_err, eval_env,
                     "match: (? pred) needs a predicate");
        return -1;
      }
      exp_t *pred = EVAL(rest->content, eval_env);
      if (!pred || iserror(pred)) {
        *err = pred ? pred : NIL_EXP;
        return -1;
      }
      exp_t *r = alc_apply1(pred, val ? val : NIL_EXP, eval_env);
      unrefexp(pred);
      if (!r || iserror(r)) {
        *err = r ? r : NIL_EXP;
        return -1;
      }
      int ok = istrue(r);
      unrefexp(r);
      return ok ? 1 : 0;
    }

    /* (cons ph pt) — pair pattern */
    if (strcmp(hn, "cons") == 0) {
      if (!rest || !rest->next) {
        *err = error(ERROR_ILLEGAL_VALUE, e_err, eval_env,
                     "match: (cons head tail) needs two sub-patterns");
        return -1;
      }
      if (!ispair(val) || !istrue(val))
        return 0;
      int r = alc_match_pat(rest->content, val->content, newenv, e_err,
                            eval_env, err);
      if (r <= 0)
        return r;
      return alc_match_pat(rest->next->content, val->next ? val->next : NIL_EXP,
                           newenv, e_err, eval_env, err);
    }

    /* (list p1 p2 ...) — exact-length list pattern */
    if (strcmp(hn, "list") == 0)
      return alc_match_list_pats(rest, val, newenv, e_err, eval_env, err);

    /* (vec p1 p2 ...) — exact-length vector pattern */
    if (strcmp(hn, "vec") == 0) {
      if (!isvector(val))
        return 0;
      int64_t vlen = vec_len(val);
      exp_t *p = rest;
      int64_t pi = 0;
      while (p && p->content) {
        if (pi >= vlen)
          return 0;
        /* vec_get_boxed returns an owned ref for all three vec kinds. */
        exp_t *cell = vec_get_boxed(val, pi);
        int r = alc_match_pat(p->content, cell, newenv, e_err, eval_env, err);
        unrefexp(cell);
        if (r <= 0)
          return r;
        p = p->next;
        pi++;
      }
      return (pi == vlen) ? 1 : 0;
    }
  }
  return 0; /* unrecognised compound pattern = no-match */
}

const char doc_match[] =
    "(match expr pat1 result1 pat2 result2 ... default) — structural pattern "
    "matching. Evaluates expr once, then tries each pat left-to-right. The "
    "first matching pat evaluates its result with pattern variables in scope. "
    "A lone trailing element is the unconditional default; (match x) is nil. "
    "Patterns: _ wildcard; symbol x binds; nil/t/number/string literal; "
    "(list p...) exact list; (cons h t) pair; (vec p...) vector; "
    "(quote sym) symbol literal; (? pred) guard predicate.";
exp_t *matchcmd(exp_t *e, env_t *env) {
  int outer_tail = in_tail_position;
  if (!e->next) {
    unrefexp(e);
    return NIL_EXP;
  }
  in_tail_position = 0;
  exp_t *val = EVAL(e->next->content, env);
  if (!val || iserror(val)) {
    in_tail_position = outer_tail;
    unrefexp(e);
    return val ? val : NIL_EXP;
  }
  exp_t *cur = e->next->next;
  exp_t *ret = NIL_EXP;
  while (cur) {
    if (!cur->next) {
      /* Default: lone trailing element */
      in_tail_position = outer_tail;
      ret = EVAL(car(cur), env);
      break;
    }
    exp_t *pat = car(cur);
    exp_t *rform = car(cur->next);
    env_t *newenv = make_env(env);
    exp_t *merr = NULL;
    int m = alc_match_pat(pat, val, newenv, e, env, &merr);
    if (merr) {
      destroy_env(newenv);
      unrefexp(val);
      unrefexp(e);
      in_tail_position = outer_tail;
      return merr;
    }
    if (m > 0) {
      in_tail_position = outer_tail;
      ret = EVAL(rform, newenv);
      destroy_env(newenv);
      break;
    }
    destroy_env(newenv);
    cur = cur->next ? cur->next->next : NULL;
  }
  unrefexp(val);
  unrefexp(e);
  in_tail_position = outer_tail;
  return ret ? ret : NIL_EXP;
}

/* ---- generators ---------------------------------------------------------- */

/* Internal dict keys for generator state (chosen to avoid user collisions). */
#define GK_KIND "\x01gk" /* fixnum kind: 0=list 1=range 2=map 3=filter */
#define GK_CUR "\x01gc"  /* cursor: list node (list), current int (range) */
#define GK_END "\x01ge"  /* end value (range) */
#define GK_STP "\x01gs"  /* step (range) */
#define GK_FN "\x01gf"   /* function (map/filter) */
#define GK_IN "\x01gi"   /* inner generator (map/filter) */
#define GK_LIST 0
#define GK_RANGE 1
#define GK_MAP 2
#define GK_FILTER 3

static exp_t *gen_dict_get(exp_t *d, const char *k) {
  keyval_t *kv = set_get_keyval_dict((dict_t *)d->ptr, (char *)k, NULL);
  return kv ? kv->val : NULL;
}
static void gen_dict_set(exp_t *d, const char *k, exp_t *v) {
  set_get_keyval_dict((dict_t *)d->ptr, (char *)k, v);
}
static int gen_kind(exp_t *g) {
  exp_t *k = gen_dict_get(g, GK_KIND);
  return (k && isnumber(k)) ? (int)FIX_VAL(k) : -1;
}
static int isgen(exp_t *g) { return isdict(g) && gen_kind(g) >= 0; }

/* Advance generator g by one step. Returns next value (owned ref) or GEN_DONE.
   Mutates g's internal state in-place. */
static exp_t *alc_gen_step(exp_t *g, env_t *env) {
  /* A plain closure is a generator: each step calls it with no args, and
     it returns the next value (or *done* to signal exhaustion). This lets
     users write custom generators as ordinary 0-arg closures — counters,
     fib, zip, etc. — that compose with map!/filter!/for-each!/collect!. */
  if (islambda(g)) {
    exp_t *v = alc_apply_n(g, 0, NULL, env); /* borrows g, no argv to own */
    return v ? v : GEN_DONE;
  }
  if (!isgen(g))
    return GEN_DONE;
  switch (gen_kind(g)) {

  case GK_LIST: {
    exp_t *cur = gen_dict_get(g, GK_CUR);
    if (!cur || !ispair(cur) || !istrue(cur))
      return GEN_DONE;
    exp_t *val = refexp(cur->content ? cur->content : NIL_EXP);
    /* refexp next BEFORE dict-set: the dict will unref the old cursor
       (cascade-freeing cur), which would also decrement cur->next.
       Our explicit ref keeps next alive until dict_set stores its own. */
    exp_t *rest = refexp(cur->next ? cur->next : NIL_EXP);
    gen_dict_set(g, GK_CUR, rest);
    unrefexp(rest); /* release our protection ref; dict holds its own */
    return val;
  }

  case GK_RANGE: {
    exp_t *ecur = gen_dict_get(g, GK_CUR);
    exp_t *eend = gen_dict_get(g, GK_END);
    exp_t *estp = gen_dict_get(g, GK_STP);
    if (!ecur || !eend || !estp || !isnumber(ecur))
      return GEN_DONE;
    int64_t cur = FIX_VAL(ecur), end = FIX_VAL(eend), stp = FIX_VAL(estp);
    int done = (stp > 0) ? (cur >= end) : (cur <= end);
    if (done)
      return GEN_DONE;
    gen_dict_set(g, GK_CUR, MAKE_FIX(cur + stp));
    return MAKE_FIX(cur);
  }

  case GK_MAP: {
    exp_t *inner = gen_dict_get(g, GK_IN);
    exp_t *fn = gen_dict_get(g, GK_FN);
    if (!inner || !fn)
      return GEN_DONE;
    /* refexp fn before the recursive step: user code inside alc_gen_step
       could drop the last external ref to g, freeing the dict and fn. */
    refexp(fn);
    exp_t *v = alc_gen_step(inner, env);
    if (!v || isgen_done(v)) {
      unrefexp(fn);
      return GEN_DONE;
    }
    exp_t *r = alc_apply1(fn, v, env);
    unrefexp(fn);
    unrefexp(v);
    return r ? r : NIL_EXP;
  }

  case GK_FILTER: {
    exp_t *inner = gen_dict_get(g, GK_IN);
    exp_t *pred = gen_dict_get(g, GK_FN);
    if (!inner || !pred)
      return GEN_DONE;
    /* refexp pred across the loop: inner step may drop the last ref to g. */
    refexp(pred);
    for (;;) {
      exp_t *v = alc_gen_step(inner, env);
      if (!v || isgen_done(v)) {
        unrefexp(pred);
        return GEN_DONE;
      }
      exp_t *ok = alc_apply1(pred, v, env);
      if (iserror(ok)) {
        unrefexp(pred);
        unrefexp(v);
        return ok;
      }
      int pass = istrue(ok);
      unrefexp(ok);
      if (pass) {
        unrefexp(pred);
        return v;
      }
      unrefexp(v);
    }
  }
  }
  return GEN_DONE;
}

static exp_t *make_gen_dict(int kind) {
  exp_t *d = make_nil();
  d->type = EXP_DICT;
  d->ptr = create_dict();
  gen_dict_set(d, GK_KIND, MAKE_FIX(kind));
  return d;
}

const char doc_gendone[] = "(*gen-done*) — returns the generator exhaustion "
                           "sentinel. Generators return this when exhausted.";
exp_t *gendone_cmd(exp_t *e, env_t *env) {
  unrefexp(e);
  (void)env;
  return GEN_DONE;
}

const char doc_gendonep[] =
    "(gen-done? x) — t if x is the generator exhaustion "
    "sentinel (*gen-done*), nil otherwise.";
exp_t *gendonep_cmd(exp_t *e, env_t *env) {
  if (!e->next) {
    unrefexp(e);
    return NIL_EXP;
  }
  exp_t *v = EVAL(e->next->content, env);
  exp_t *ret = (v && isgen_done(v)) ? TRUE_EXP : NIL_EXP;
  unrefexp(v);
  unrefexp(e);
  return ret;
}

const char doc_genlist[] =
    "(gen-list lst) — returns a generator that yields "
    "each element of lst in order. Exhaustion returns *gen-done*.";
exp_t *genlist_cmd(exp_t *e, env_t *env) {
  if (!e->next) {
    unrefexp(e);
    return NIL_EXP;
  }
  exp_t *lst = EVAL(e->next->content, env);
  if (iserror(lst)) {
    unrefexp(e);
    return lst;
  }
  exp_t *g = make_gen_dict(GK_LIST);
  gen_dict_set(g, GK_CUR, lst ? lst : NIL_EXP);
  unrefexp(lst);
  unrefexp(e);
  return g;
}

const char doc_genrange[] =
    "(gen-range end) / (gen-range start end) / (gen-range start end step) — "
    "yields integers from start (default 0) up to but not including end, "
    "incrementing by step (default 1; negative step counts down).";
exp_t *genrange_cmd(exp_t *e, env_t *env) {
  exp_t *cur = cdr(e);
  if (!cur) {
    unrefexp(e);
    return NIL_EXP;
  }
  EVAL_ARG_1(a);
  int64_t start = 0, end = 0, step = 1;
  exp_t *b = NULL, *c = NULL;
  if (cdr(cur)) {
    b = EVAL(car(cdr(cur)), env);
    if (!b) {
      unrefexp(a);
      unrefexp(e);
      return NIL_EXP;
    }
    if (iserror(b)) {
      unrefexp(a);
      unrefexp(e);
      return b;
    }
    if (cdr(cdr(cur))) {
      c = EVAL(car(cdr(cdr(cur))), env);
      if (!c) {
        unrefexp(a);
        unrefexp(b);
        unrefexp(e);
        return NIL_EXP;
      }
      if (iserror(c)) {
        unrefexp(a);
        unrefexp(b);
        unrefexp(e);
        return c;
      }
    }
  }
  if (!b) {
    /* (gen-range end) */
    if (!isnumber(a)) {
      unrefexp(a);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "gen-range: integer expected");
    }
    end = FIX_VAL(a);
  } else if (!c) {
    /* (gen-range start end) */
    if (!isnumber(a) || !isnumber(b)) {
      unrefexp(a);
      unrefexp(b);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "gen-range: integers expected");
    }
    start = FIX_VAL(a);
    end = FIX_VAL(b);
  } else {
    /* (gen-range start end step) */
    if (!isnumber(a) || !isnumber(b) || !isnumber(c)) {
      unrefexp(a);
      unrefexp(b);
      unrefexp(c);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "gen-range: integers expected");
    }
    start = FIX_VAL(a);
    end = FIX_VAL(b);
    step = FIX_VAL(c);
    unrefexp(c);
  }
  unrefexp(a);
  unrefexp(b);
  if (step == 0) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "gen-range: step cannot be 0");
  }
  exp_t *g = make_gen_dict(GK_RANGE);
  gen_dict_set(g, GK_CUR, MAKE_FIX(start));
  gen_dict_set(g, GK_END, MAKE_FIX(end));
  gen_dict_set(g, GK_STP, MAKE_FIX(step));
  unrefexp(e);
  return g;
}

const char doc_gennext[] = "(gen-next! g) — advance generator g and return its "
                           "next value, or *gen-done* when exhausted.";
exp_t *gennext_cmd(exp_t *e, env_t *env) {
  if (!e->next) {
    unrefexp(e);
    return GEN_DONE;
  }
  exp_t *g = EVAL(e->next->content, env);
  if (!g || iserror(g)) {
    unrefexp(e);
    return g ? g : NIL_EXP;
  }
  exp_t *ret = alc_gen_step(g, env);
  unrefexp(g);
  unrefexp(e);
  return ret ? ret : GEN_DONE;
}

const char doc_gencollect[] =
    "(gen-collect g) — drain generator g into a list and return it.";
exp_t *gencollect_cmd(exp_t *e, env_t *env) {
  if (!e->next) {
    unrefexp(e);
    return NIL_EXP;
  }
  exp_t *g = EVAL(e->next->content, env);
  if (!g || iserror(g)) {
    unrefexp(e);
    return g ? g : NIL_EXP;
  }
  exp_t *head = NIL_EXP, *tail = NULL;
  for (;;) {
    exp_t *v = alc_gen_step(g, env);
    if (!v || isgen_done(v))
      break;
    if (iserror(v)) {
      unrefexp(head); /* NIL_EXP is immortal — unrefexp handles it safely */
      unrefexp(g);
      unrefexp(e);
      return v;
    }
    list_append_owned(&head, &tail, v);
  }
  unrefexp(g);
  unrefexp(e);
  return head;
}

const char doc_genmap[] =
    "(gen-map fn g) — return a generator that yields (fn x) for each x from g.";
exp_t *genmap_cmd(exp_t *e, env_t *env) {
  if (!e->next || !e->next->next) {
    unrefexp(e);
    return NIL_EXP;
  }
  EVAL_ARG_1(fn);
  exp_t *g = EVAL(e->next->next->content, env);
  if (!fn || iserror(fn)) {
    unrefexp(g);
    unrefexp(e);
    return fn ? fn : NIL_EXP;
  }
  if (!g || iserror(g)) {
    unrefexp(fn);
    unrefexp(e);
    return g ? g : NIL_EXP;
  }
  exp_t *out = make_gen_dict(GK_MAP);
  gen_dict_set(out, GK_FN, fn);
  gen_dict_set(out, GK_IN, g);
  unrefexp(fn);
  unrefexp(g);
  unrefexp(e);
  return out;
}

const char doc_genfilter[] =
    "(gen-filter pred g) — return a generator yielding only elements of g "
    "for which (pred x) is truthy.";
exp_t *genfilter_cmd(exp_t *e, env_t *env) {
  if (!e->next || !e->next->next) {
    unrefexp(e);
    return NIL_EXP;
  }
  EVAL_ARG_1(pred);
  exp_t *g = EVAL(e->next->next->content, env);
  if (!pred || iserror(pred)) {
    unrefexp(g);
    unrefexp(e);
    return pred ? pred : NIL_EXP;
  }
  if (!g || iserror(g)) {
    unrefexp(pred);
    unrefexp(e);
    return g ? g : NIL_EXP;
  }
  exp_t *out = make_gen_dict(GK_FILTER);
  gen_dict_set(out, GK_FN, pred);
  gen_dict_set(out, GK_IN, g);
  unrefexp(pred);
  unrefexp(g);
  unrefexp(e);
  return out;
}

const char doc_forgen[] =
    "(for-gen var gen body ...) — iterate generator gen, binding each yielded "
    "value to var and evaluating body forms. Returns nil.";
exp_t *forgencmd(exp_t *e, env_t *env) {
  if (!e->next || !e->next->next || !e->next->next->next) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, NULL, env,
                 "(for-gen var gen body ...)");
  }
  exp_t *varname = e->next->content;
  if (!issymbol(varname)) {
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "for-gen: first arg must be a symbol");
  }
  exp_t *g = EVAL(e->next->next->content, env);
  if (!g || iserror(g)) {
    unrefexp(e);
    return g ? g : NIL_EXP;
  }
  exp_t *body_start = e->next->next->next;
  env_t *loop_env = make_env(env);
  int was_tail = in_tail_position;
  in_tail_position = 0;
  exp_t *ret = NIL_EXP;
  for (;;) {
    exp_t *v = alc_gen_step(g, env);
    if (!v || isgen_done(v))
      break;
    if (iserror(v)) {
      ret = v;
      break;
    }
    /* Rebind the loop variable each iteration. */
    if (!loop_env->d)
      loop_env->d = create_dict();
    set_get_keyval_dict(loop_env->d, (char *)exp_text(varname), v);
    unrefexp(v);
    /* Evaluate body forms. */
    exp_t *bc = body_start;
    while (bc) {
      if (ret)
        unrefexp(ret);
      ret = EVAL(bc->content, loop_env);
      if (ret && iserror(ret))
        goto done;
      bc = bc->next;
    }
  }
done:
  if (ret && iserror(ret)) { /* propagate */
  } else {
    unrefexp(ret);
    ret = NIL_EXP;
  }
  in_tail_position = was_tail;
  destroy_env(loop_env);
  unrefexp(g);
  unrefexp(e);
  return ret;
}

/* Bind a single name (sym->ptr) to val in env; takes ownership of val. */
static void var2env_bind(char *name, exp_t *val, env_t *env) {
  if (env->n_inline < ENV_INLINE_SLOTS) {
    env->inline_keys[env->n_inline] = name;
    env->inline_vals[env->n_inline] = val;
    env->n_inline++;
  } else {
    if (!env->d)
      env->d = create_dict();
    set_get_keyval_dict(env->d, (char *)name, val);
    unrefexp(val);
  }
}

exp_t *var2env(exp_t *e, exp_t *var, exp_t *val, env_t *env, int evalexp) {
  /* borrow references */
  exp_t *curvar = var;
  exp_t *retvar;
  exp_t *curval = val;
  /* Name the callee in arity errors. e is the call form (name args...); its
     operator is a symbol for a named call, a fn literal for an anonymous one.
   */
  const char *fname = (e && is_ptr(e->content) && issymbol(e->content))
                          ? (const char *)exp_text(e->content)
                          : "function";

  /* Empty params `()` are represented by nil_singleton — a pair with
     NULL content/next. Without the content check, the loop would enter
     once and either bind NULL as a key or hit "missing parameter" if
     no args were passed. The content check makes 0-arg defs work. */
  while (curvar && curvar->content) {
    /* Rest-param marker: (a b . rest) reads as (a b . rest) — detect the
       dot symbol and collect remaining args into a list for the next param. */
    if (issymbol(curvar->content) &&
        strcmp((char *)exp_text(curvar->content), ".") == 0) {
      if (!curvar->next || !curvar->next->content ||
          !issymbol(curvar->next->content))
        return error(ERROR_ILLEGAL_VALUE, e, env,
                     "rest param: symbol expected after '.'");
      char *rest_name = (char *)exp_text(curvar->next->content);
      exp_t *rest_head = NIL_EXP, *rest_tail = NULL;
      while (curval) {
        exp_t *rv = evalexp ? EVAL(curval->content, env->root)
                            : refexp(curval->content);
        if (!rv)
          rv = NIL_EXP;
        if (evalexp && iserror(rv))
          return rv;
        list_append_owned(&rest_head, &rest_tail, rv);
        curval = curval->next;
      }
      var2env_bind(rest_name, rest_head, env);
      return NULL;
    }

    /* Default/optional param: (name default-expr) where the 2nd element is
       NOT a symbol — e.g. (y 10), (y (+ a 1)). Distinguished from a
       destructuring pattern (a b), whose elements are all symbols. The
       default is evaluated in `env` (the frame being built), so it can
       reference params bound earlier in this same list. */
    int is_default = ispair(curvar->content) &&
                     issymbol(car(curvar->content)) && cadr(curvar->content) &&
                     !issymbol(cadr(curvar->content));
    if ((curval)) {
      if ((retvar = (evalexp ? EVAL(curval->content, env->root)
                             : refexp(curval->content)))) {
        if (evalexp && iserror(retvar)) {
          return retvar;
        }
      } else
        retvar = NIL_EXP;
      if (is_default) {
        /* Arg supplied — bind the name to it; the default is unused. */
        var2env_bind((char *)exp_text(car(curvar->content)), retvar, env);
      } else if (issymbol(curvar->content)) {
        var2env_bind((char *)exp_text(curvar->content), retvar, env);
      } else if (ispair(curvar->content) && istrue(curvar->content)) {
        /* Destructuring param: (def f ((x y) z) body) binds the first arg
           as a list, extracting x and y from its elements. */
        exp_t *subpat = curvar->content;
        exp_t *subval = retvar;
        while (subpat && subpat->content) {
          exp_t *nm = subpat->content;
          if (!issymbol(nm)) {
            unrefexp(retvar);
            return error(ERROR_ILLEGAL_VALUE, e, env,
                         "destructuring param: symbol expected");
          }
          int have_val = subval && ispair(subval) && istrue(subval);
          var2env_bind((char *)exp_text(nm),
                       refexp(have_val ? subval->content : NIL_EXP), env);
          subpat = subpat->next;
          if (have_val)
            subval = subval->next;
        }
        unrefexp(retvar);
      } else {
        /* Not a symbol, a (name default) pair, or a destructuring pattern:
           a malformed parameter (e.g. a bare `10`). def/fn/defn reject this
           at definition via build_clean_params; this guards any other path
           (and stops a bad param from silently swallowing an argument and
           short-circuiting the too-many-args check below). */
        unrefexp(retvar);
        return error(ERROR_ILLEGAL_VALUE, e, env,
                     "illegal parameter in %s (must be a symbol, a "
                     "(name default) pair, or a destructuring pattern)",
                     fname);
      }
      curval = curval->next;
    } else if (is_default) {
      /* Arg omitted — evaluate the default in the frame being built. */
      exp_t *dv = EVAL(cadr(curvar->content), env);
      if (!dv)
        dv = NIL_EXP;
      if (iserror(dv))
        return dv;
      var2env_bind((char *)exp_text(car(curvar->content)), dv, env);
    } else
      return error(ERROR_MISSING_PARAMETER, e, env, "too few arguments to %s",
                   fname);
    curvar = curvar->next;
  }
  /* Leftover args = too many (a fixed-arity callee with no rest param). The
     rest-param branch consumes all args and returns early, so reaching here
     with args remaining is a genuine arity error — matches the compiled path
     (vm_invoke_values), which already rejects this. */
  if (curval && curval->content)
    return error(ERROR_MISSING_PARAMETER, e, env, "too many arguments to %s",
                 fname);
  return NULL;
}
/* Build a tail-call trampoline marker. Args are evaluated in env (the
   caller's frame, where local bindings still live) and attached to the
   marker directly so the outer invoke can rebind without re-evaluating.
   Marker layout:
     type    = EXP_PAIR
     flags   = FLAG_TAILREC
     content = the lambda to invoke (owned ref)
     next    = list of pre-evaluated arg nodes (each node content = value) */
static exp_t *make_tail_marker(exp_t *fn, exp_t *call_form, env_t *env) {
  /* Args are themselves not in tail position. */
  int saved_tail = in_tail_position;
  in_tail_position = 0;
  exp_t *marker = make_nil();
  marker->flags |= FLAG_TAILREC;
  marker->content = refexp(fn);
  exp_t *tail = marker;
  exp_t *src = call_form->next;
  while (src) {
    exp_t *v = EVAL(src->content, env);
    if (v && iserror(v)) {
      unrefexp(marker);
      in_tail_position = saved_tail;
      return v;
    }
    tail = tail->next = make_node(v);
    src = src->next;
  }
  in_tail_position = saved_tail;
  return marker;
}
