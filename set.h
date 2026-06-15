/* set.h — hash-set (EXP_SET) ops: the type-tagged canonical key encoder
 * (set_key_for_value), value clone/insert, the set constructors and
 * add/del/has?/union/intersection/difference/->list builtins. EXP_SET is a
 * dict_t with canonical-string keys, so this sits on dict.h. FRAGMENT
 * #included into alcove.c (single TU). NOT standalone/separately compiled.
 */
/* ---------- hash-set / EXP_SET ops ---------- */

static void set_key_hex_append(char **buf, size_t *len, size_t *cap,
                               const unsigned char *bytes, size_t n) {
  static const char hexdigits[] = "0123456789abcdef";
  if (*len + n * 2 + 1 > *cap) {
    while (*len + n * 2 + 1 > *cap)
      *cap *= 2;
    char *p = realloc(*buf, *cap);
    if (!p)
      graceful_shutdown("Fatal error: Out of memory");
    *buf = p;
  }
  for (size_t i = 0; i < n; i++) {
    (*buf)[(*len)++] = hexdigits[bytes[i] >> 4];
    (*buf)[(*len)++] = hexdigits[bytes[i] & 0x0f];
  }
  (*buf)[*len] = 0;
}

static void set_key_put(char **buf, size_t *len, size_t *cap, const char *s) {
  str_buf_put(buf, len, cap, s, strlen(s));
}

static char *set_key_for_value(exp_t *v) {
  char tmp[128];
  size_t cap = 64, len = 0;
  char *buf = memalloc(cap, 1);
  buf[0] = 0;

  if (!v || v == NIL_EXP || (is_ptr(v) && ispair(v) && !istrue(v))) {
    set_key_put(&buf, &len, &cap, "N:");
    return buf;
  }
  if (v == TRUE_EXP) {
    set_key_put(&buf, &len, &cap, "T:");
    return buf;
  }
  if (isnumber(v)) {
    snprintf(tmp, sizeof tmp, "I:%lld", (long long)FIX_VAL(v));
    set_key_put(&buf, &len, &cap, tmp);
    return buf;
  }
  if (ischar(v)) {
    snprintf(tmp, sizeof tmp, "C:%u", (unsigned int)CHAR_VAL(v));
    set_key_put(&buf, &len, &cap, tmp);
    return buf;
  }
  if (isfloat(v)) {
    uint64_t bits = 0;
    memcpy(&bits, &v->f, sizeof bits);
    snprintf(tmp, sizeof tmp, "F:%016llx", (unsigned long long)bits);
    set_key_put(&buf, &len, &cap, tmp);
    return buf;
  }
  if (isrational(v)) { /* reduced+normalized -> canonical key */
    alc_rat_t *r = (alc_rat_t *)v->ptr;
    snprintf(tmp, sizeof tmp, "R:%lld/%lld", (long long)r->num,
             (long long)r->den);
    set_key_put(&buf, &len, &cap, tmp);
    return buf;
  }
  if (isstring(v) || issymbol(v)) {
    set_key_put(&buf, &len, &cap, isstring(v) ? "S:" : "Y:");
    {
      const char *_t = exp_text(v);
      set_key_hex_append(&buf, &len, &cap, (const unsigned char *)_t,
                         strlen(_t));
    }
    return buf;
  }
  if (isblob(v)) {
    alc_blob_t *b = (alc_blob_t *)v->ptr;
    set_key_put(&buf, &len, &cap, "B:");
    if (b && b->len)
      set_key_hex_append(&buf, &len, &cap, (const unsigned char *)b->bytes,
                         b->len);
    return buf;
  }

  free(buf);
  return NULL;
}

static exp_t *set_value_clone(exp_t *v) {
  if (!v || v == NIL_EXP || (is_ptr(v) && ispair(v) && !istrue(v)))
    return refexp(NIL_EXP);
  if (v == TRUE_EXP)
    return refexp(TRUE_EXP);
  if (isnumber(v) || ischar(v))
    return refexp(v);
  if (isfloat(v))
    return make_floatf(v->f);
  if (isrational(v)) {
    alc_rat_t *r = (alc_rat_t *)v->ptr;
    return make_rational(r->num, r->den);
  }
  if (isstring(v)) {
    const char *_t = exp_text(v);
    return make_string((char *)_t, strlen((char *)_t));
  }
  if (issymbol(v)) {
    const char *_t = exp_text(v);
    return make_symbol((char *)_t, strlen((char *)_t));
  }
  if (isblob(v)) {
    alc_blob_t *b = (alc_blob_t *)v->ptr;
    return make_blob((b && b->len) ? b->bytes : "", b ? b->len : 0);
  }
  return NULL;
}

static int set_insert_value(dict_t *d, exp_t *v) {
  char *ks = set_key_for_value(v);
  if (!ks)
    return 0;
  exp_t *stored = set_value_clone(v);
  if (!stored) {
    free(ks);
    return 0;
  }
  set_get_keyval_dict(d, ks, stored);
  unrefexp(stored);
  free(ks);
  return 1;
}

const char doc_set[] =
    "(set x ...) — build an EXP_SET with unique scalar elements.";
exp_t *setcmd(exp_t *e, env_t *env) {
  exp_t *ret = make_set_exp();
  dict_t *d = (dict_t *)ret->ptr;
  for (exp_t *a = cdr(e); a; a = a->next) {
    exp_t *v = EVAL(car(a), env);
    if (iserror(v)) {
      unrefexp(ret);
      unrefexp(e);
      return v;
    }
    if (!set_insert_value(d, v)) {
      unrefexp(v);
      unrefexp(ret);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "set: unsupported element type");
    }
    unrefexp(v);
  }
  unrefexp(e);
  return ret;
}

const char doc_hashset[] = "(hash-set x ...) — alias for set.";
exp_t *hashsetcmd(exp_t *e, env_t *env) { return setcmd(e, env); }

#define SET_VALUE_SETUP(err_name)                                              \
  exp_t *s = EVAL(cadr(e), env);                                               \
  if (iserror(s)) {                                                            \
    unrefexp(e);                                                               \
    return s;                                                                  \
  }                                                                            \
  if (!isset(s)) {                                                             \
    unrefexp(s);                                                               \
    unrefexp(e);                                                               \
    return error(ERROR_ILLEGAL_VALUE, NULL, env,                               \
                 err_name ": first arg must be a set");                        \
  }                                                                            \
  exp_t *v = EVAL(caddr(e), env);                                              \
  if (iserror(v)) {                                                            \
    unrefexp(s);                                                               \
    unrefexp(e);                                                               \
    return v;                                                                  \
  }                                                                            \
  char *ks = set_key_for_value(v);

const char doc_setaddbang[] =
    "(set-add! s x) — add x to set s in place; returns s.";
exp_t *setaddbangcmd(exp_t *e, env_t *env) {
  SET_VALUE_SETUP("set-add!")
  if (!ks)
    CLEAN_RETURN_2(s, v,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "set-add!: unsupported element type"));
  exp_t *stored = set_value_clone(v);
  if (!stored) {
    free(ks);
    CLEAN_RETURN_2(s, v,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "set-add!: unsupported element type"));
  }
  set_get_keyval_dict((dict_t *)s->ptr, ks, stored);
  unrefexp(stored);
  free(ks);
  CLEAN_RETURN_1(v, s);
}

const char doc_setdelbang[] =
    "(set-del! s x) — remove x from set s in place; returns s.";
exp_t *setdelbangcmd(exp_t *e, env_t *env) {
  SET_VALUE_SETUP("set-del!")
  if (ks) {
    del_keyval_dict((dict_t *)s->ptr, ks);
    free(ks);
  }
  CLEAN_RETURN_1(v, s);
}

const char doc_sethasp[] = "(set-has? s x) — t if s contains x, else nil.";
exp_t *sethaspcmd(exp_t *e, env_t *env) {
  SET_VALUE_SETUP("set-has?")
  exp_t *ret = NIL_EXP;
  if (ks && set_get_keyval_dict((dict_t *)s->ptr, ks, NULL))
    ret = TRUE_EXP;
  if (ks)
    free(ks);
  CLEAN_RETURN_2(s, v, ret);
}

static exp_t *set_copy_exp(exp_t *src) {
  exp_t *ret = make_set_exp();
  dict_t *rd = (dict_t *)ret->ptr;
  dict_t *sd = (dict_t *)src->ptr;
  if (sd)
    DICT_FOREACH(sd, k, 0, 0)
      set_insert_value(rd, k->val);
  return ret;
}

static int set_contains_key(exp_t *s, char *key) {
  return set_get_keyval_dict((dict_t *)s->ptr, key, NULL) != NULL;
}

const char doc_setunion[] =
    "(set-union a b) — new set with elements of a or b.";
exp_t *setunioncmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(a, b);
  if (!isset(a) || !isset(b))
    CLEAN_RETURN_2(
        a, b,
        error(ERROR_ILLEGAL_VALUE, NULL, env, "set-union: args must be sets"));
  exp_t *ret = set_copy_exp(a);
  dict_t *rd = (dict_t *)ret->ptr;
  dict_t *bd = (dict_t *)b->ptr;
  if (bd)
    DICT_FOREACH(bd, k, 0, 0)
      set_insert_value(rd, k->val);
  CLEAN_RETURN_2(a, b, ret);
}

const char doc_setintersection[] =
    "(set-intersection a b) — new set with elements common to a and b.";
exp_t *setintersectioncmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(a, b);
  if (!isset(a) || !isset(b))
    CLEAN_RETURN_2(a, b,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "set-intersection: args must be sets"));
  exp_t *ret = make_set_exp();
  dict_t *rd = (dict_t *)ret->ptr;
  dict_t *ad = (dict_t *)a->ptr;
  if (ad)
    DICT_FOREACH(ad, k, 0, 0)
      if (set_contains_key(b, k->key))
        set_insert_value(rd, k->val);
  CLEAN_RETURN_2(a, b, ret);
}

const char doc_setdifference[] =
    "(set-difference a b) — new set with elements in a but not b.";
exp_t *setdifferencecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(a, b);
  if (!isset(a) || !isset(b))
    CLEAN_RETURN_2(a, b,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "set-difference: args must be sets"));
  exp_t *ret = make_set_exp();
  dict_t *rd = (dict_t *)ret->ptr;
  dict_t *ad = (dict_t *)a->ptr;
  if (ad)
    DICT_FOREACH(ad, k, 0, 0)
      if (!set_contains_key(b, k->key))
        set_insert_value(rd, k->val);
  CLEAN_RETURN_2(a, b, ret);
}

const char doc_setlist[] =
    "(set->list s) — list of set elements (order undefined).";
exp_t *setlistcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(s);
  if (!isset(s))
    CLEAN_RETURN_1(s, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "set->list: arg must be a set"));
  exp_t *ret = NIL_EXP, *tail = NULL;
  dict_t *d = (dict_t *)s->ptr;
  if (d)
    DICT_FOREACH(d, k, 0, 0)
      list_append_owned(&ret, &tail, set_value_clone(k->val));
  CLEAN_RETURN_1(s, ret);
}
