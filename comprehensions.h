/* comprehensions.h — the Hy-flavored comprehension family:
 *   (lfor var coll [pred] expr)        -> list
 *   (sfor var coll [pred] expr)        -> set
 *   (dfor var coll [pred] kexpr vexpr) -> hash-map
 *   (gfor var coll [pred] expr)        -> lazy generator
 *
 * Positional clauses (no keywords), matching the language's Arc lineage:
 * bind var to each element of coll (a list), keep the elements where pred
 * is true (omit pred to keep all), and accumulate expr's value into the
 * named shape. lfor/sfor/dfor share one eager driver modeled on eachcmd
 * (env binding, empty-list guard, error propagation). gfor SYNTHESIZES
 * the equivalent (map! (fn (var) expr) [(filter! (fn (var) pred))]
 * (iter! coll)) pipeline and evaluates it, so laziness comes from the
 * existing generator machinery instead of a second implementation.
 *
 * These are special forms reached via the compiler's OP_EVAL_AST fallback
 * (see CLAUDE.md: builtins reached only through the AST fallback need no
 * compile_expr dispatch). FRAGMENT #included into alcove.c after set.h —
 * needs make_dict_exp/make_set_exp/set_insert_value/alc_key_to_cstr.
 */

enum { COMPR_LIST, COMPR_SET, COMPR_DICT };

static exp_t *compr_run(exp_t *e, env_t *env, int kind, const char *usage) {
  exp_t *a_var = e->next;
  exp_t *a_coll = a_var ? a_var->next : NULL;
  exp_t *rest = a_coll ? a_coll->next : NULL;
  int nrest = 0;
  for (exp_t *r = rest; r; r = r->next)
    nrest++;
  int need = (kind == COMPR_DICT) ? 2 : 1; /* dfor takes kexpr AND vexpr */
  if (!a_var || !a_coll || nrest < need || nrest > need + 1) {
    exp_t *err = error(ERROR_MISSING_PARAMETER, e, env, "%s", usage);
    unrefexp(e);
    return err;
  }
  exp_t *varname = a_var->content;
  exp_t *ret = NULL;
  if (!issymbol(varname)) {
    ret = error(ERROR_ILLEGAL_VALUE, e, env,
                "comprehension: first arg must be a symbol");
    unrefexp(e);
    return ret;
  }
  CHECK_RESERVED_BIND(varname, ret, "in comprehension", {
    unrefexp(e);
    return ret;
  });
  int haspred = (nrest == need + 1);
  exp_t *pred = haspred ? rest->content : NULL;
  exp_t *r1 = haspred ? rest->next : rest;
  exp_t *x1 = r1->content;                              /* expr, or dfor key */
  exp_t *x2 = (kind == COMPR_DICT) ? r1->next->content : NULL; /* dfor val */

  exp_t *coll = EVAL(a_coll->content, env);
  if (coll == NULL)
    coll = NIL_EXP;
  if (iserror(coll)) {
    unrefexp(e);
    return coll;
  }
  if (!ispair(coll)) {
    ret = error(ERROR_ILLEGAL_VALUE, e, env,
                "comprehension: coll must be a list");
    unrefexp(coll);
    unrefexp(e);
    return ret;
  }

  env_t *nenv = make_env(env);
  if (!nenv->d)
    nenv->d = create_dict();
  exp_t *head = NULL, *tail = NULL; /* COMPR_LIST accumulator */
  exp_t *res = NULL;                /* COMPR_SET / COMPR_DICT accumulator */
  if (kind == COMPR_DICT)
    res = make_dict_exp();
  else if (kind == COMPR_SET)
    res = make_set_exp();

  /* Same guard as eachcmd: the empty list is a pair with NULL content;
     a list CONTAINING nil stores NIL_EXP (non-NULL), so it iterates. */
  for (exp_t *w = coll; w && w->content; w = w->next) {
    set_get_keyval_dict(nenv->d, exp_text(varname), car(w));
    if (pred) {
      exp_t *p = EVAL(pred, nenv);
      if (p && iserror(p)) {
        ret = p;
        goto fail;
      }
      int keep = (p && p != NIL_EXP);
      unrefexp(p);
      if (!keep)
        continue;
    }
    exp_t *v1 = EVAL(x1, nenv);
    if (v1 && iserror(v1)) {
      ret = v1;
      goto fail;
    }
    if (!v1) /* raw-NULL nil: normalize so a nil element stays a real cell */
      v1 = NIL_EXP;
    switch (kind) {
    case COMPR_LIST: {
      exp_t *cell = make_node(v1); /* cell owns v1 */
      if (!head)
        head = tail = cell;
      else {
        tail->next = cell;
        tail = cell;
      }
      break;
    }
    case COMPR_SET:
      if (!set_insert_value((dict_t *)res->ptr, v1)) {
        unrefexp(v1);
        ret = error(ERROR_ILLEGAL_VALUE, e, env,
                    "sfor: unsupported set element type");
        goto fail;
      }
      unrefexp(v1);
      break;
    case COMPR_DICT: {
      char tmp[32];
      char *ks = alc_key_to_cstr(v1, tmp);
      if (!ks) {
        unrefexp(v1);
        ret = error(ERROR_ILLEGAL_VALUE, e, env,
                    "dfor: key must be a keyword/string/number");
        goto fail;
      }
      exp_t *v2 = EVAL(x2, nenv);
      if (v2 && iserror(v2)) {
        unrefexp(v1);
        ret = v2;
        goto fail;
      }
      if (!v2)
        v2 = NIL_EXP;
      /* insert copies the key, so ks may point into v1's text or tmp */
      set_get_keyval_dict((dict_t *)res->ptr, ks, v2);
      unrefexp(v2);
      unrefexp(v1);
      break;
    }
    }
  }
  if (kind == COMPR_LIST)
    ret = head ? head : refexp(NIL_EXP);
  else
    ret = res;
  goto done;
fail:
  if (head)
    unrefexp(head);
  if (res)
    unrefexp(res);
done:
  destroy_env(nenv);
  unrefexp(coll);
  unrefexp(e);
  return ret;
}

const char doc_lfor[] =
    "(lfor var coll expr) / (lfor var coll pred expr) — list comprehension: "
    "bind var to each element of list coll, keep expr's value where pred is "
    "true (3-arg form keeps all). (lfor x (range 0 10) (odd x) (* x x)) is "
    "(1 9 25 49 81). Family: sfor (set), dfor (hash-map), gfor (lazy).";
exp_t *lforcmd(exp_t *e, env_t *env) {
  return compr_run(e, env, COMPR_LIST,
                   "(lfor var coll expr) or (lfor var coll pred expr)");
}

const char doc_sfor[] =
    "(sfor var coll expr) / (sfor var coll pred expr) — set comprehension: "
    "like lfor but accumulates into a set (duplicates collapse; elements "
    "must be scalar set members).";
exp_t *sforcmd(exp_t *e, env_t *env) {
  return compr_run(e, env, COMPR_SET,
                   "(sfor var coll expr) or (sfor var coll pred expr)");
}

const char doc_dfor[] =
    "(dfor var coll kexpr vexpr) / (dfor var coll pred kexpr vexpr) — "
    "hash-map comprehension: like lfor but each kept element contributes "
    "the entry kexpr -> vexpr (keys: keyword/string/number).";
exp_t *dforcmd(exp_t *e, env_t *env) {
  return compr_run(
      e, env, COMPR_DICT,
      "(dfor var coll kexpr vexpr) or (dfor var coll pred kexpr vexpr)");
}

/* gfor: build (map! (fn (var) expr) [inner]) where inner is (iter! coll)
   or (filter! (fn (var) pred) (iter! coll)), then evaluate it. The
   synthesized form takes its own refs on the reused subforms and is
   released after EVAL. */
static exp_t *compr_list2(exp_t *a, exp_t *b) { /* (a b) — owns both */
  exp_t *n = make_node(a);
  n->next = make_node(b);
  return n;
}

const char doc_gfor[] =
    "(gfor var coll expr) / (gfor var coll pred expr) — generator "
    "comprehension: the lazy twin of lfor. Nothing runs until the generator "
    "is pulled (next! / collect! / for-gen). Sugar for (map! (fn (var) "
    "expr) (filter! (fn (var) pred) (iter! coll))).";
exp_t *gforcmd(exp_t *e, env_t *env) {
  exp_t *a_var = e->next;
  exp_t *a_coll = a_var ? a_var->next : NULL;
  exp_t *rest = a_coll ? a_coll->next : NULL;
  int nrest = 0;
  for (exp_t *r = rest; r; r = r->next)
    nrest++;
  if (!a_var || !a_coll || nrest < 1 || nrest > 2) {
    exp_t *err = error(ERROR_MISSING_PARAMETER, e, env,
                       "(gfor var coll expr) or (gfor var coll pred expr)");
    unrefexp(e);
    return err;
  }
  exp_t *varname = a_var->content;
  exp_t *ret = NULL;
  if (!issymbol(varname)) {
    ret = error(ERROR_ILLEGAL_VALUE, e, env,
                "gfor: first arg must be a symbol");
    unrefexp(e);
    return ret;
  }
  CHECK_RESERVED_BIND(varname, ret, "in comprehension", {
    unrefexp(e);
    return ret;
  });
  exp_t *pred = (nrest == 2) ? rest->content : NULL;
  exp_t *expr = (nrest == 2) ? rest->next->content : rest->content;

  /* (iter! coll) */
  exp_t *inner = compr_list2(make_symbol("iter!", 5), refexp(a_coll->content));
  if (pred) { /* (filter! (fn (var) pred) inner) */
    exp_t *pfn = compr_list2(make_symbol("fn", 2), make_node(refexp(varname)));
    pfn->next->next = make_node(refexp(pred));
    exp_t *fl = compr_list2(make_symbol("filter!", 7), pfn);
    fl->next->next = make_node(inner);
    inner = fl;
  }
  /* (map! (fn (var) expr) inner) */
  exp_t *mfn = compr_list2(make_symbol("fn", 2), make_node(refexp(varname)));
  mfn->next->next = make_node(refexp(expr));
  exp_t *synth = compr_list2(make_symbol("map!", 4), mfn);
  synth->next->next = make_node(inner);

  ret = EVAL(synth, env);
  unrefexp(synth);
  unrefexp(e);
  return ret;
}
