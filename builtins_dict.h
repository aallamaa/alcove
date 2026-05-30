/* builtins_dict.h — Lisp-value hash-map (EXP_DICT) builtins: hash-map, assoc!,
 * dissoc!, get, contains?, and the polymorphic count. Sit on the dict.h engine
 * (set_get_keyval_dict etc.). FRAGMENT #included into alcove.c; cmd bodies
 * reached via lispProcList function pointers, prototypes in builtins.h.
 */
/* ---------- hash-map / dict ops ---------- */

const char doc_hashmap[] = "(hash-map [k v ...]) — build an EXP_DICT. Keys: "
                           "keyword/string/number. Same as {k v, ...}.";
exp_t *hashmapcmd(exp_t *e, env_t *env) {
  exp_t *ret = make_dict_exp();
  dict_t *d = (dict_t *)ret->ptr;
  exp_t *a = cdr(e);
  char tmp[32];
  while (a) {
    exp_t *kraw = EVAL(car(a), env);
    if (iserror(kraw)) {
      unrefexp(ret);
      unrefexp(e);
      return kraw;
    }
    if (!a->next) {
      unrefexp(kraw);
      unrefexp(ret);
      unrefexp(e);
      return error(ERROR_MISSING_PARAMETER, NULL, env,
                   "hash-map: odd number of forms (key without value)");
    }
    exp_t *v = EVAL(car(a->next), env);
    if (iserror(v)) {
      unrefexp(kraw);
      unrefexp(ret);
      unrefexp(e);
      return v;
    }
    char *ks = alc_key_to_cstr(kraw, tmp);
    if (!ks) {
      unrefexp(kraw);
      unrefexp(v);
      unrefexp(ret);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "hash-map: unsupported key type");
    }
    set_get_keyval_dict(d, ks, v);
    unrefexp(kraw);
    unrefexp(v);
    a = a->next->next;
  }
  unrefexp(e);
  return ret;
}

const char doc_assocbang[] = "(assoc! d k v) — set d[k]=v in place; returns d.";
exp_t *assocbangcmd(exp_t *e, env_t *env) {
  DICT_KV_SETUP("assoc!")
  exp_t *v = EVAL(cadddr(e), env);
  if (iserror(v))
    CLEAN_RETURN_2(k, d, v);

  if (!ks)
    CLEAN_RETURN_3(
        k, d, v,
        error(ERROR_ILLEGAL_VALUE, NULL, env, "assoc!: unsupported key type"));

  set_get_keyval_dict((dict_t *)d->ptr, ks, v);
  CLEAN_RETURN_2(k, v, d);
}

const char doc_dissocbang[] =
    "(dissoc! d k) — delete key k from d in place; returns d.";
exp_t *dissocbangcmd(exp_t *e, env_t *env) {
  DICT_KV_SETUP("dissoc!")
  if (ks)
    del_keyval_dict((dict_t *)d->ptr, ks);
  CLEAN_RETURN_1(k, d); /* d is not unref'd, it is returned */
}

const char doc_get[] = "(get d k [default]) — fetch d[k]. Works on hash-maps. "
                       "Returns default (or nil) when missing.";
exp_t *getcmd(exp_t *e, env_t *env) {
  DICT_KV_SETUP("get")
  exp_t *ret = NIL_EXP;
  if (ks) {
    keyval_t *kv = set_get_keyval_dict((dict_t *)d->ptr, ks, NULL);
    if (kv)
      ret = refexp(kv->val);
    else if (cdddr(e))
      ret = EVAL(cadddr(e), env);
  } else if (cdddr(e)) {
    ret = EVAL(cadddr(e), env);
  }
  CLEAN_RETURN_2(k, d, ret);
}

const char doc_containsp[] = "(contains? d k) — t if d has key k, else nil.";
exp_t *containspcmd(exp_t *e, env_t *env) {
  DICT_KV_SETUP("contains?")
  exp_t *ret = NIL_EXP;
  if (ks && set_get_keyval_dict((dict_t *)d->ptr, ks, NULL))
    ret = TRUE_EXP;
  unrefexp(k);
  unrefexp(d);
  unrefexp(e);
  return ret;
}

const char doc_keys[] = "(keys d) — list of keys in d (order undefined).";
DICT_ITER_CMD(keyscmd, "keys", alc_cstr_to_key((char *)k->key))

const char doc_vals[] = "(vals d) — list of values in d (order matches keys).";
DICT_ITER_CMD(valscmd, "vals", refexp(k->val))

const char doc_count[] = "(count x) — element count for hash-maps, sets, "
                         "deques, vectors, strings, blobs, and lists.";
exp_t *countcmd(exp_t *e, env_t *env) {
  exp_t *x = EVAL(cadr(e), env);
  if (iserror(x)) {
    unrefexp(e);
    return x;
  }
  int64_t n = 0;
  if (isdict(x) || isset(x)) {
    dict_t *d = (dict_t *)x->ptr;
    n = d ? (int64_t)d->ht[0].used : 0;
  } else if (islist(x))
    n = ((alc_list_t *)x->ptr)->len;
  else if (isblob(x))
    n = (int64_t)((alc_blob_t *)x->ptr)->len;
  else if (is_ptr(x) && x->type == EXP_VECTOR && x->ptr)
    n = vec_len(x);
  else if (isstring(x))
    n = (int64_t)strlen((char *)exp_text(x));
  else if (ispair(x)) {
    exp_t *p = x;
    while (p && istrue(p)) {
      n++;
      p = p->next;
    }
  } else {
    unrefexp(x);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "count: unsupported type");
  }
  unrefexp(x);
  unrefexp(e);
  return MAKE_FIX(n);
}
