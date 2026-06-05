/* builtins_stdlib.h — the standard-library builtins: arithmetic (incl. the
 * MATH_CMD family), float math (FLOAT_UNARY_CMD), comparisons/min-max
 * (MINMAX_CMD), type predicates (PRED_CMD), string ops (STRING_CASE_CMD),
 * sequence/list ops (length/nth/map/filter/reduce/sort/...), I/O, and escape
 * continuations (call/cc). FRAGMENT #included into alcove.c after the shared
 * EVAL_ARG/CLEAN_RETURN macros and the family generators it relies on. The cmd
 * bodies are reached only via the lispProcList function pointers, so moving
 * them is perf-neutral; their prototypes stay in builtins.h. NOT standalone.
 */
/* ---------------- Standard-library builtins (math/seq/predicates)
   ---------------- Each follows the prn/expt pattern: walk e->next, EVAL each
   arg, type-check, produce result, unrefexp args + form, return owned ref. */

/* (mod a b) — integer modulo. Both args must be fixnums. */
const char doc_mod[] = "(mod a b) — remainder of a / b. Sign follows the "
                       "divisor for floats; truncation for ints.";
exp_t *modcmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *b = NULL, *ret = NULL;
  if (e->next && e->next->next) {
    a = EVAL(e->next->content, env);
    if (iserror(a)) {
      unrefexp(e);
      return a;
    }
    b = EVAL(e->next->next->content, env);
    if (iserror(b)) {
      unrefexp(a);
      unrefexp(e);
      return b;
    }
    if (isnumber(a) && isnumber(b) && FIX_VAL(b) != 0) {
      int64_t va = FIX_VAL(a), vb = FIX_VAL(b);
      ret = MAKE_FIX(va - (va / vb) * vb); /* C99 truncated div */
    } else if ((isnumber(a) || isfloat(a)) && (isnumber(b) || isfloat(b))) {
      double da = TO_DOUBLE(a);
      double db = TO_DOUBLE(b);
      if (db == 0.0)
        ret = error(ERROR_DIV_BY0, e, env, "mod by 0");
      else
        ret = make_floatf(fmod(da, db));
    } else {
      ret = error(ERROR_ILLEGAL_VALUE, e, env, "mod needs numeric operands");
    }
  } else {
    ret = error(ERROR_MISSING_PARAMETER, e, env, "(mod a b)");
  }
  unrefexp(a);
  unrefexp(b);
  unrefexp(e);
  return ret;
}

/* Bitwise ops on integers. Both args must be fixnums; floats are
   rejected. Shifts mask the count to 0..63 so (<< 1 64) == (<< 1 0). */
#define BITOP_AB(name, expr)                                                   \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *a = NULL, *b = NULL, *ret = NULL;                                   \
    if (e->next && e->next->next) {                                            \
      a = EVAL(e->next->content, env);                                         \
      if (iserror(a)) {                                                        \
        unrefexp(e);                                                           \
        return a;                                                              \
      }                                                                        \
      b = EVAL(e->next->next->content, env);                                   \
      if (iserror(b)) {                                                        \
        unrefexp(a);                                                           \
        unrefexp(e);                                                           \
        return b;                                                              \
      }                                                                        \
      if (isnumber(a) && isnumber(b)) {                                        \
        int64_t va = FIX_VAL(a), vb = FIX_VAL(b);                              \
        ret = MAKE_FIX(expr);                                                  \
      } else {                                                                 \
        ret = error(ERROR_ILLEGAL_VALUE, e, env,                               \
                    "bit op requires two integer args");                       \
      }                                                                        \
    } else {                                                                   \
      ret = error(ERROR_MISSING_PARAMETER, e, env, "bit op needs two args");   \
    }                                                                          \
    unrefexp(a);                                                               \
    unrefexp(b);                                                               \
    unrefexp(e);                                                               \
    return ret;                                                                \
  }

const char doc_bitand[] =
    "(bit-and a b) — bitwise AND on two integers. Alias: &.";
BITOP_AB(bitandcmd, va &vb)
const char doc_bitor[] = "(bit-or a b) — bitwise OR on two integers. Alias: |.";
BITOP_AB(bitorcmd, va | vb)
const char doc_bitxor[] =
    "(bit-xor a b) — bitwise XOR on two integers. Alias: ^.";
BITOP_AB(bitxorcmd, va ^ vb)
const char doc_shl[] =
    "(<< x n) — bitwise shift x left by n bits. Wraps inside int61.";
BITOP_AB(shlcmd, (int64_t)((uint64_t)va << (vb & 63)))
const char doc_shr[] =
    "(>> x n) — arithmetic shift x right by n bits (sign-preserving).";
BITOP_AB(shrcmd, va >> (vb & 63))
#undef BITOP_AB

/* (~ x) — bitwise NOT. ~0 is -1 (all bits set, fits cleanly in int61). */
const char doc_bitnot[] = "(~ x) — bitwise NOT (complement). Alias: bit-not.";
exp_t *bitnotcmd(exp_t *e, env_t *env) {
  exp_t *ret = NULL;
  EVAL_ARG_1(a);
  if (a) {
    if (isnumber(a)) {
      ret = MAKE_FIX(~FIX_VAL(a));
    } else {
      ret = error(ERROR_ILLEGAL_VALUE, e, env, "~ requires an integer arg");
    }
  } else {
    ret = error(ERROR_MISSING_PARAMETER, e, env, "(~ x) needs one arg");
  }
  CLEAN_RETURN_1(a, ret);
}

/* (abs x) — |x| for fixnum or float. */
const char doc_abs[] = "(abs x) — absolute value.";
exp_t *abscmd(exp_t *e, env_t *env) {
  exp_t *ret = NULL;
  EVAL_ARG_1(a);
  if (a) {
    if (isnumber(a)) {
      int64_t v = FIX_VAL(a);
      int64_t av = v < 0 ? -v : v;
      /* If negation overflows fixnum range (abs of most-negative fixnum),
         promote to float rather than silently wrapping to negative.
         Uses signed >>3 to match FIX_VAL's arithmetic-shift semantics. */
      if (v < 0 && !FIX_FITS(av))
        ret = make_floatf(-(expfloat)v);
      else
        ret = MAKE_FIX(av);
    } else if (isfloat(a)) {
      ret = make_floatf(a->f < 0 ? -a->f : a->f);
    } else
      ret = error(ERROR_ILLEGAL_VALUE, e, env, "abs: not a number");
  } else
    ret = error(ERROR_MISSING_PARAMETER, e, env, "(abs x)");
  CLEAN_RETURN_1(a, ret);
}

/* Helper for max/min: numeric "less than" between two values, promoting
   to double if either is a float. Returns 1 if a < b, 0 otherwise. -1
   on type error (caller checks). */
static int alc_numlt(exp_t *a, exp_t *b, int *err) {
  *err = 0;
  if (isnumber(a) && isnumber(b))
    return FIX_VAL(a) < FIX_VAL(b);
  if ((isnumber(a) || isfloat(a)) && (isnumber(b) || isfloat(b))) {
    double da = TO_DOUBLE(a);
    double db = TO_DOUBLE(b);
    return da < db;
  }
  *err = 1;
  return 0;
}

/* (max a b ...) — variadic; at least one arg required. */
#define MINMAX_CMD(name, is_lt, err_name)                                      \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *cur = e->next;                                                      \
    if (!cur) {                                                                \
      unrefexp(e);                                                             \
      return error(ERROR_MISSING_PARAMETER, e, env, "(" #name " ...)");        \
    }                                                                          \
    exp_t *best = EVAL(cur->content, env);                                     \
    if (iserror(best)) {                                                       \
      unrefexp(e);                                                             \
      return best;                                                             \
    }                                                                          \
    cur = cur->next;                                                           \
    while (cur) {                                                              \
      exp_t *v = EVAL(cur->content, env);                                      \
      if (iserror(v)) {                                                        \
        unrefexp(best);                                                        \
        unrefexp(e);                                                           \
        return v;                                                              \
      }                                                                        \
      int err;                                                                 \
      int lt = (is_lt) ? alc_numlt(v, best, &err) : alc_numlt(best, v, &err);  \
      if (err) {                                                               \
        unrefexp(best);                                                        \
        unrefexp(v);                                                           \
        unrefexp(e);                                                           \
        return error(ERROR_ILLEGAL_VALUE, e, env, err_name ": non-numeric");   \
      }                                                                        \
      if (lt) {                                                                \
        unrefexp(best);                                                        \
        best = v;                                                              \
      } else                                                                   \
        unrefexp(v);                                                           \
      cur = cur->next;                                                         \
    }                                                                          \
    unrefexp(e);                                                               \
    return best;                                                               \
  }

const char doc_max[] = "(max x ...) — largest of the args.";
MINMAX_CMD(maxcmd, 0, "max")

/* (min a b ...) */
const char doc_min[] = "(min x ...) — smallest of the args.";
MINMAX_CMD(mincmd, 1, "min")

/* (length x) — list length, string length, or 0 for nil. */
const char doc_length[] = "(length x) — element count of a list/string/vector.";
exp_t *lengthcmd(exp_t *e, env_t *env) {
  exp_t *ret = NULL;
  EVAL_ARG_1(a);
  if (a) {
    int64_t n = 0;
    if (isstring(a)) {
      const char *_t = exp_text(a);
      n = _t ? utf8_strlen(_t) : 0;
    } else if (a == nil_singleton)
      n = 0;
    else if (ispair(a)) {
      exp_t *cur = a;
      while (cur && cur->content) {
        n++;
        cur = cur->next;
      }
    } else {
      ret = error(ERROR_ILLEGAL_VALUE, e, env, "length: not a list/string");
      goto done;
    }
    ret = MAKE_FIX(n);
  } else
    ret = error(ERROR_MISSING_PARAMETER, e, env, "(length x)");
done:
  CLEAN_RETURN_1(a, ret);
}

/* ── generic sequence protocol ────────────────────────────────────────────
   coll_to_list coerces any supported sequence to a cons list the caller OWNS
   (unrefexp when done): an already-cons list / nil is returned via refexp (no
   copy); a vector, string (per codepoint), or deque is materialized into a
   fresh list. Returns NULL if `coll` isn't a sequence. Associative collections
   (set → members, dict/HAMT → (key value) entries) are handled by
   coll_assoc_to_list, defined after set.h/hamt.h since it needs their iterators.
   This is what makes seq/first/rest/map/filter/reduce/nth work uniformly across
   list, vector, string, deque, set, dict, and HAMT. */
static exp_t *coll_assoc_to_list(exp_t *coll); /* defined post-set.h/hamt.h */
static exp_t *coll_to_list(exp_t *coll) {
  if (!coll || coll == NIL_EXP || ispair(coll))
    return refexp(coll ? coll : NIL_EXP);
  exp_t *head = NULL, *tail = NULL;
  if (isvector(coll)) {
    int64_t n = vec_len(coll);
    for (int64_t i = 0; i < n; i++)
      list_append_owned(&head, &tail, vec_get_boxed(coll, i));
  } else if (isstring(coll)) {
    const char *s = exp_text(coll);
    size_t off = 0;
    while (s[off])
      list_append_owned(&head, &tail, make_char(utf8_decode_at(s, &off)));
  } else if (islist(coll)) { /* deque (EXP_LIST) */
    alc_list_t *l = (alc_list_t *)coll->ptr;
    for (alc_listnode_t *nd = l ? l->head : NULL; nd; nd = nd->next)
      list_append_owned(&head, &tail, refexp(nd->val));
  } else {
    return coll_assoc_to_list(coll); /* set/dict (or NULL if not a sequence) */
  }
  return head ? head : refexp(NIL_EXP);
}

const char doc_seq[] =
    "(seq coll) — list of coll's elements: list/vector/string/deque; set "
    "members; dict/hamt (key value) entries. nil for an empty/absent sequence.";
exp_t *seqcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(a);
  exp_t *s = coll_to_list(a);
  if (!s)
    CLEAN_RETURN_1(a, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "seq: argument is not a sequence"));
  CLEAN_RETURN_1(a, s);
}

const char doc_first[] = "(first coll) — first element of a sequence, or nil.";
exp_t *firstcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(a);
  exp_t *s = coll_to_list(a), *ret = NIL_EXP;
  if (s) {
    if (ispair(s) && s->content)
      ret = refexp(s->content);
    unrefexp(s);
  }
  CLEAN_RETURN_1(a, ret);
}

const char doc_rest[] =
    "(rest coll) — sequence of all but the first element (a list), or nil.";
exp_t *restcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(a);
  exp_t *s = coll_to_list(a), *ret = NIL_EXP;
  if (s) {
    /* rest shares the tail of the materialized list; refexp it and drop s */
    if (ispair(s) && s->next && s->next->content)
      ret = refexp(s->next);
    unrefexp(s);
  }
  CLEAN_RETURN_1(a, ret);
}

/* (nth i coll) — 0-indexed element; nil if out of range. Accepts either arg
   order (nth i coll) / (nth coll i) since a collection is never a number.
   Works across list/vector/string/deque via coll_to_list. */
const char doc_nth[] = "(nth i coll) — 0-based element of a list/string/vector/"
                       "deque (arg order is flexible). nil if out of range.";
exp_t *nthcmd(exp_t *e, env_t *env) {
  exp_t *ret = NIL_EXP;
  EVAL_ARG_2(a, b);
  exp_t *idxv = isnumber(a) ? a : (isnumber(b) ? b : NULL);
  exp_t *collv = isnumber(a) ? b : a;
  if (idxv && collv) {
    exp_t *seq = coll_to_list(collv);
    if (!seq)
      CLEAN_RETURN_2(a, b,
                     error(ERROR_ILLEGAL_VALUE, NULL, env,
                           "nth: not a sequence"));
    int64_t idx = FIX_VAL(idxv);
    exp_t *cur = seq;
    while (idx > 0 && ispair(cur) && cur->next) {
      cur = cur->next;
      idx--;
    }
    if (idx == 0 && ispair(cur) && cur->content)
      ret = refexp(cur->content);
    unrefexp(seq);
  }
  CLEAN_RETURN_2(a, b, ret);
}

/* Apply a reserved builtin to already-evaluated VALUE args by name, quoting each
   so the builtin's own EVAL passes them through unchanged. Returns the builtin's
   result (owned), or an error if `name` isn't a builtin. Used by conj/into to
   reuse the tested per-type add ops instead of re-implementing container
   internals. The synthesized form is consumed by the builtin (consume-e). */
static exp_t *apply_builtin(const char *name, env_t *env, int n, exp_t **args) {
  keyval_t *kv = set_get_keyval_dict(reserved_symbol, (char *)name, NULL);
  if (!kv || !isinternal((exp_t *)kv->val))
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "conj/into: missing %s", name);
  exp_t *form = make_node(refexp((exp_t *)kv->val)), *cur = form;
  for (int i = 0; i < n; i++)
    cur = cur->next = make_node(make_quote(refexp(args[i])));
  return evaluate(form, env);
}

/* conj one element into coll, dispatched by type: mutable collections
   (vector/set/deque/dict) are updated IN PLACE and coll is returned; immutable
   ones (list/hamt) return a fresh collection. For dict/hamt, `x` is a (key
   value) 2-list. Returns the (owned) resulting collection, or an error. The
   caller's `coll` ref is consumed only when a fresh collection is returned
   (it does `coll = conj_one(coll, ...)`); for in-place updates the same object
   comes back (an extra ref), so the caller must unref the return either way. */
static exp_t *conj_one(exp_t *coll, exp_t *x, env_t *env) {
  if (isvector(coll) || isset(coll) || islist(coll)) {
    const char *op =
        isvector(coll) ? "vec-push!" : isset(coll) ? "set-add!" : "push-right!";
    exp_t *args[2] = {coll, x};
    exp_t *r = apply_builtin(op, env, 2, args); /* mutates coll */
    if (iserror(r))
      return r; /* propagate, don't swallow the echo */
    unrefexp(r);
    return refexp(coll);
  }
  if (isdict(coll) || ishamt(coll)) {
    if (!ispair(x) || !car(x) || !cadr(x))
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "conj: map entry must be a (key value) list");
    exp_t *args[3] = {coll, car(x), cadr(x)};
    if (isdict(coll)) {
      exp_t *r = apply_builtin("assoc!", env, 3, args); /* mutates */
      if (iserror(r))
        return r;
      unrefexp(r);
      return refexp(coll);
    }
    return apply_builtin("hamt-assoc", env, 3, args); /* fresh hamt */
  }
  /* list / nil → prepend (immutable): (cons x coll), a fresh list. */
  if (ispair(coll) || coll == NIL_EXP) {
    exp_t *args[2] = {x, coll};
    return apply_builtin("cons", env, 2, args);
  }
  return error(ERROR_ILLEGAL_VALUE, NULL, env, "conj: not a collection");
}

const char doc_conj[] =
    "(conj coll x ...) — add elements to coll. Mutable collections "
    "(vector/set/deque/dict) are updated in place; list/hamt return a fresh "
    "collection (list prepends). For dict/hamt each x is a (key value) list.";
exp_t *conjcmd(exp_t *e, env_t *env) {
  if (!e->next)
    CLEAN_RETURN_1(NIL_EXP, error(ERROR_MISSING_PARAMETER, e, env,
                                  "(conj coll x ...)"));
  exp_t *coll = EVAL(e->next->content, env);
  if (iserror(coll)) {
    unrefexp(e);
    return coll;
  }
  for (exp_t *p = e->next->next; p; p = p->next) {
    exp_t *x = EVAL(p->content, env);
    if (iserror(x)) {
      unrefexp(coll);
      unrefexp(e);
      return x;
    }
    exp_t *nc = conj_one(coll, x, env);
    unrefexp(x);
    unrefexp(coll); /* drop the old/echo ref; nc is the new owned coll */
    coll = nc;
    if (iserror(coll)) {
      unrefexp(e);
      return coll;
    }
  }
  unrefexp(e);
  return coll;
}

const char doc_into[] =
    "(into dest src) — conj every element of (seq src) into dest, left to "
    "right. (into [] s) / (into #{} s) / (into {} entries) convert collections.";
exp_t *intocmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(dest, src);
  if (!dest || !e->next->next)
    CLEAN_RETURN_2(dest, src,
                   error(ERROR_MISSING_PARAMETER, e, env, "(into dest src)"));
  exp_t *seq = coll_to_list(src);
  if (!seq)
    CLEAN_RETURN_2(dest, src,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "into: src is not a sequence"));
  exp_t *coll = refexp(dest);
  for (exp_t *c = seq; ispair(c) && c->content; c = c->next) {
    exp_t *nc = conj_one(coll, c->content, env);
    unrefexp(coll);
    coll = nc;
    if (iserror(coll)) {
      unrefexp(seq);
      CLEAN_RETURN_2(dest, src, coll);
    }
  }
  unrefexp(seq);
  CLEAN_RETURN_2(dest, src, coll);
}

/* ── user types: type-of + defstruct ──────────────────────────────────────
   A struct instance is a dict carrying its type name under the string key
   "__type__"; type-of reads it (else returns the built-in type name). */
static const char *type_name_of(exp_t *a) {
  if (a == NULL || a == NIL_EXP)
    return "nil";
  if (a == TRUE_EXP)
    return "bool";
  if (isnumber(a))
    return "int";
  if (ischar(a))
    return "char";
  if (isfloat(a))
    return "float";
  if (isstring(a))
    return "string";
  if (issymbol(a))
    return ((const char *)exp_text(a))[0] == ':' ? "keyword" : "symbol";
  if (ispair(a))
    return "list";
  if (isvector(a))
    return "vector";
  if (isset(a))
    return "set";
  if (isdict(a))
    return "dict";
  if (ishamt(a))
    return "hamt";
  if (islist(a))
    return "deque";
  if (isblob(a))
    return "blob";
  if (islambda(a) || isinternal(a))
    return "fn";
  if (iserror(a))
    return "error";
  return "unknown";
}

const char doc_typeof[] =
    "(type-of x) — type tag as a symbol: int/float/string/symbol/keyword/char/"
    "bool/nil/list/vector/set/dict/hamt/deque/fn; or a defstruct's type name.";
exp_t *typeofcmd(exp_t *e, env_t *env) {
  /* Evaluate by hand (not EVAL_ARG_1): an error value is a thing whose type we
     report as 'error, not something to re-raise — (type-of (try ...)) → 'error. */
  exp_t *a = e->next ? EVAL(e->next->content, env) : refexp(NIL_EXP);
  if (iserror(a))
    CLEAN_RETURN_1(a, make_symbol("error", 5));
  if (isdict(a) && a->ptr) {
    keyval_t *kv = set_get_keyval_dict((dict_t *)a->ptr, "__type__", NULL);
    if (kv && kv->val)
      CLEAN_RETURN_1(a, refexp(kv->val));
  }
  const char *t = type_name_of(a);
  CLEAN_RETURN_1(a, make_symbol((char *)t, (int)strlen(t)));
}

/* (defstruct NAME field...) — define a record type. Expands (by building source
   and evaluating it) to: a constructor NAME, a predicate NAME?, and per-field
   accessors NAME-field. Instances are dicts tagged with "__type__" → NAME, so
   (type-of inst) is NAME. Top-level (defines globals). */
const char doc_defstruct[] =
    "(defstruct point x y) — record type: constructor (point 3 4), predicate "
    "(point? v), accessors (point-x v); (type-of v) is 'point.";
exp_t *defstructcmd(exp_t *e, env_t *env) {
  exp_t *nm = cadr(e);
  if (!nm || !is_ptr(nm) || !issymbol(nm)) {
    exp_t *err = error(ERROR_ILLEGAL_VALUE, e, env, "(defstruct name field...)");
    unrefexp(e);
    return err;
  }
  /* Each field must be a PLAIN symbol. A keyword (:x) or other token would be
     emitted verbatim into the generated (def NAME (fields...) ...) param list,
     where :x reads as a JIT type-hint and the whole expansion fails opaquely. */
  for (exp_t *f = cddr(e); f && f->content; f = f->next)
    if (!issymbol(f->content) ||
        ((const char *)exp_text(f->content))[0] == ':') {
      exp_t *err = error(ERROR_ILLEGAL_VALUE, e, env,
                         "defstruct: field names must be plain symbols");
      unrefexp(e);
      return err;
    }
  char nbuf[128];
  snprintf(nbuf, sizeof nbuf, "%s", (const char *)exp_text(nm));
  char *src = malloc(8192);
  if (!src) {
    exp_t *err = error(ERROR_ILLEGAL_VALUE, e, env, "defstruct: out of memory");
    unrefexp(e);
    return err;
  }
  int o = 0, ok = 1;
#define DS_ADD(...)                                                            \
  do {                                                                         \
    int _n = snprintf(src + o, 8192 - o, __VA_ARGS__);                         \
    if (_n < 0 || _n >= 8192 - o)                                              \
      ok = 0;                                                                  \
    else                                                                       \
      o += _n;                                                                 \
  } while (0)
  DS_ADD("(do (def %s (", nbuf);
  for (exp_t *f = cddr(e); f && f->content; f = f->next)
    if (issymbol(f->content))
      DS_ADD("%s ", (const char *)exp_text(f->content));
  DS_ADD(") (hash-map \"__type__\" (quote %s)", nbuf);
  for (exp_t *f = cddr(e); f && f->content; f = f->next)
    if (issymbol(f->content)) {
      const char *fn = (const char *)exp_text(f->content);
      DS_ADD(" \"%s\" %s", fn, fn);
    }
  DS_ADD("))");
  DS_ADD(" (def %s? (o) (and (dict? o) (is (type-of o) (quote %s))))", nbuf,
         nbuf);
  for (exp_t *f = cddr(e); f && f->content; f = f->next)
    if (issymbol(f->content)) {
      const char *fn = (const char *)exp_text(f->content);
      DS_ADD(" (def %s-%s (o) (get o \"%s\"))", nbuf, fn, fn);
    }
  DS_ADD(")");
#undef DS_ADD
  exp_t *ret;
  if (!ok) {
    ret = error(ERROR_ILLEGAL_VALUE, e, env, "defstruct: too large");
  } else {
    exp_t *r = alcove_eval_string(src);
    if (iserror(r))
      ret = r;
    else {
      unrefexp(r);
      ret = make_symbol(nbuf, (int)strlen(nbuf));
    }
  }
  free(src);
  unrefexp(e);
  return ret;
}

/* ── multimethods: defmulti / defmethod ───────────────────────────────────
   A multimethod NAME is an ordinary fn that dispatches: it applies a dispatch
   fn to its args to get a value, looks that value up in a per-multimethod dict
   NAME__m, and applies the found method to the args. defmethod adds an entry.
   Implemented by constructing the expansion forms (they embed the arbitrary
   dispatch fn / method body, so build-by-string won't do) and evaluating. */
static exp_t *sx_sym(const char *s) { return make_symbol((char *)s, (int)strlen(s)); }
static exp_t *sx_lst(int n, ...) { /* build a proper list from n owned exps */
  va_list ap;
  va_start(ap, n);
  exp_t *head = NULL, *tail = NULL;
  for (int i = 0; i < n; i++) {
    exp_t *node = make_node(va_arg(ap, exp_t *));
    if (tail)
      tail->next = node;
    else
      head = node;
    tail = node;
  }
  va_end(ap);
  return head ? head : refexp(NIL_EXP);
}

const char doc_defmulti[] =
    "(defmulti area type-of) — define a multimethod: calling (area x ...) "
    "applies the dispatch fn to the args and runs the matching defmethod.";
exp_t *defmulticmd(exp_t *e, env_t *env) {
  exp_t *nm = cadr(e), *disp = caddr(e);
  if (!nm || !is_ptr(nm) || !issymbol(nm) || !disp) {
    exp_t *err = error(ERROR_ILLEGAL_VALUE, e, env, "(defmulti name dispatch-fn)");
    unrefexp(e);
    return err;
  }
  char mname[160];
  snprintf(mname, sizeof mname, "%s__m", (const char *)exp_text(nm));
  exp_t *ret = make_symbol((char *)exp_text(nm), (int)strlen(exp_text(nm)));
  /* (do (= NAME__m (hash-map))           ; = binds a VALUE; def would read the
         (def NAME (. args)               ;   (hash-map) as a param list
              (apply (get NAME__m (apply DISP args)) args))) */
  exp_t *d1 = sx_lst(3, sx_sym("="), sx_sym(mname), sx_lst(1, sx_sym("hash-map")));
  exp_t *dispcall = sx_lst(3, sx_sym("apply"), refexp(disp), sx_sym("args"));
  exp_t *getcall = sx_lst(3, sx_sym("get"), sx_sym(mname), dispcall);
  exp_t *body = sx_lst(3, sx_sym("apply"), getcall, sx_sym("args"));
  exp_t *params = sx_lst(2, sx_sym("."), sx_sym("args"));
  exp_t *d2 = sx_lst(4, sx_sym("def"), refexp(nm), params, body);
  exp_t *form = sx_lst(3, sx_sym("do"), d1, d2);
  exp_t *r = evaluate(form, env);
  unrefexp(e);
  if (iserror(r)) {
    unrefexp(ret);
    return r;
  }
  unrefexp(r);
  return ret;
}

const char doc_defmethod[] =
    "(defmethod area :circle (c) ...) — add a method to multimethod `area` for "
    "the dispatch value :circle (matched against (dispatch-fn args)).";
exp_t *defmethodcmd(exp_t *e, env_t *env) {
  /* e = (defmethod NAME dval params body...) */
  exp_t *nm = cadr(e), *dval = caddr(e), *params = cadddr(e);
  /* body = forms after params. e = (defmethod NAME dval params body...);
     cdddr(e) is the (params body...) tail, so cdr of it is (body...). */
  exp_t *bodyforms = cdddr(e) ? cdr(cdddr(e)) : NULL;
  if (!nm || !is_ptr(nm) || !issymbol(nm) || !dval || !params) {
    exp_t *err = error(ERROR_ILLEGAL_VALUE, e, env,
                       "(defmethod name dispatch-val params body...)");
    unrefexp(e);
    return err;
  }
  char mname[160];
  snprintf(mname, sizeof mname, "%s__m", (const char *)exp_text(nm));
  /* method fn: (fn params body...) */
  exp_t *fnform = make_node(sx_sym("fn"));
  fnform->next = make_node(refexp(params));
  exp_t *tl = fnform->next;
  for (exp_t *b = bodyforms; b && b->content; b = b->next)
    tl = tl->next = make_node(refexp(b->content));
  /* (assoc! NAME__m dval fnform) — dval is used as-is: the user writes a quoted
     symbol ('circle), keyword (:hi), or literal, which assoc! evaluates to the
     dispatch key. (Re-quoting would store the (quote circle) FORM as the key.) */
  exp_t *form = sx_lst(4, sx_sym("assoc!"), sx_sym(mname), refexp(dval), fnform);
  exp_t *r = evaluate(form, env);
  unrefexp(e);
  return r; /* the method fn (assoc! returns the value), or an error */
}

/* (reverse list) — non-destructive; returns a new list. */
const char doc_reverse[] =
    "(reverse xs) — list with elements in reverse order.";
exp_t *reversecmd(exp_t *e, env_t *env) {
  exp_t *acc = NIL_EXP;
  EVAL_ARG_1(a);
  if (a) {
    /* Reject non-list args before walking — same fix pattern as appendcmd:
       a tagged immediate (fixnum, char) passes the `cur != NULL` check
       and segfaults on the deref of cur->content. nil/empty is fine. */
    if (NOT_A_LIST(a))
      CLEAN_RETURN_1(a, error(ERROR_ILLEGAL_VALUE, NULL, env,
                              "reverse: argument is not a list"));
    exp_t *cur = a;
    while (ispair(cur) && cur->content) {
      exp_t *node = make_node(refexp(cur->content));
      node->next = (acc == NIL_EXP) ? NULL : acc;
      acc = node;
      cur = cur->next;
    }
  }
  CLEAN_RETURN_1(a, acc);
}

/* (append list1 list2 ...) — flat concat into a new list (cars are
   shared with inputs but the cons spine is fresh). */
const char doc_append[] = "(append xs ys ...) — concatenate lists.";
exp_t *appendcmd(exp_t *e, env_t *env) {
  exp_t *head = NULL, *tail = NULL;
  exp_t *cur_arg = e->next;
  while (cur_arg) {
    exp_t *list = EVAL(cur_arg->content, env);
    if (iserror(list)) {
      if (head)
        unrefexp(head);
      unrefexp(e);
      return list;
    }
    /* nil/empty (which lispers freely pass to append) is a no-op. Anything
       that isn't a heap pair is a hard error — without this guard, a
       tagged fixnum like (append 10 ...) walks `cur->content` which
       dereferences the tag bits and segfaults. */
    if (NOT_A_LIST(list)) {
      if (head)
        unrefexp(head);
      unrefexp(list);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "append: argument is not a list");
    }
    exp_t *cur = list;
    while (ispair(cur) && cur->content) {
      exp_t *node = make_node(refexp(cur->content));
      if (!head) {
        head = node;
        tail = node;
      } else {
        tail->next = node;
        tail = node;
      }
      cur = cur->next;
    }
    unrefexp(list);
    cur_arg = cur_arg->next;
  }
  unrefexp(e);
  return head ? head : NIL_EXP;
}

/* Type predicates — return t/nil. */
#define PRED_CMD(name, pred)                                                   \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *a = NULL, *ret = NIL_EXP;                                           \
    if (e->next) {                                                             \
      a = EVAL(e->next->content, env);                                         \
      if (iserror(a)) {                                                        \
        unrefexp(e);                                                           \
        return a;                                                              \
      }                                                                        \
      if (pred)                                                                \
        ret = TRUE_EXP;                                                        \
    }                                                                          \
    unrefexp(a);                                                               \
    unrefexp(e);                                                               \
    return ret;                                                                \
  }
/* Type-predicate cmds, expanded from the PRED_CMD macro above. Each
   takes zero or one arg and returns t / nil (no-arg form is nil). */
const char doc_numberp[] = "(number? x) — t if x is a fixnum or float.";
const char doc_stringp[] = "(string? x) — t if x is a string.";
const char doc_symbolp[] = "(symbol? x) — t if x is a symbol.";
const char doc_pairp[] = "(pair? x) — t if x is a non-empty pair (cons cell).";
const char doc_fnp[] = "(fn? x) — t if x is callable (lambda or builtin).";
const char doc_vecp[] = "(vec? x) — t if x is a vector.";
const char doc_blobp[] = "(blob? x) — t if x is a blob.";
const char doc_dictp[] = "(dict? x) — t if x is a hash-map.";
const char doc_dequep[] = "(deque? x) — t if x is a deque.";
const char doc_setp[] = "(set? x) — t if x is a hash-set.";
PRED_CMD(numberpcmd, (isnumber(a) || isfloat(a)))
PRED_CMD(stringpcmd, isstring(a))
PRED_CMD(symbolpcmd, issymbol(a))
PRED_CMD(pairpcmd, (ispair(a) && a->content))
PRED_CMD(fnpcmd, (islambda(a) || isinternal(a) || isffi(a)))
PRED_CMD(vecpcmd, isvector(a))
PRED_CMD(blobpcmd, isblob(a))
PRED_CMD(dictpcmd, isdict(a))
PRED_CMD(dequepcmd, islist(a))
PRED_CMD(setpcmd, isset(a))
/* Introspection predicates — let tests assert on internal optimizations.
   compiled?: lambda body compiled to bytecode (vs AST fallback).
   jit?: bytecode also has native code installed (only in JIT builds).
   inline?: symbol/string text stored inline (FLAG_INLINE_TXT). Guarded by
   is_ptr first — a tagged immediate (fixnum/char) has no flags word. */
PRED_CMD(compiledpcmd, (islambda(a) && (a->flags & FLAG_COMPILED) && a->bc))
PRED_CMD(jitpcmd,
         (islambda(a) && (a->flags & FLAG_COMPILED) && a->bc && a->bc->jit))
PRED_CMD(inlinepcmd, (is_ptr(a) && (a->flags & FLAG_INLINE_TXT)))
#undef PRED_CMD

const char doc_compiledp[] =
    "(compiled? fn) — t if fn's body is compiled to bytecode (not AST).";
const char doc_jitp[] =
    "(jit? fn) — t if fn has native JIT code installed (JIT builds only).";
const char doc_inlinep[] =
    "(inline? x) — t if x is a symbol/string whose text is stored inline "
    "(<= 7 chars) rather than heap-allocated.";
const char doc_expflags[] =
    "(exp-flags x) — integer flags word of x (0 for tagged immediates). "
    "Introspection/testing: bit 2 (4) = compiled, bit 6 (64) = inline-text.";
/* Value-returning flags accessor — the user-facing counterpart to inspect,
   for assertions on the raw flags word. */
exp_t *expflagscmd(exp_t *e, env_t *env) {
  exp_t *a = e->next ? EVAL(e->next->content, env) : refexp(NIL_EXP);
  if (iserror(a)) {
    unrefexp(e);
    return a;
  }
  int f = is_ptr(a) ? a->flags : 0;
  unrefexp(a);
  unrefexp(e);
  return MAKE_FIX(f);
}

/* (exit) / (exit code) — terminate the process. */
const char doc_exit[] = "(exit) or (exit code) — terminate the process. "
                        "Default exit code is 0. Alias: quit.";
exp_t *exitcmd(exp_t *e, env_t *env) {
  int code = 0;
  if (e->next) {
    exp_t *a = EVAL(e->next->content, env);
    if (isnumber(a))
      code = (int)FIX_VAL(a);
    unrefexp(a);
  }
  unrefexp(e);
  (void)env;
  exit(code);
}

/* (random n) — pseudo-random fixnum in [0, n). Seeded once from time. */
const char doc_random[] =
    "(random n) — uniform fixnum in [0, n). (random) gives a 64-bit value.";
exp_t *randomcmd(exp_t *e, env_t *env) {
  static int seeded = 0;
  if (!seeded) {
    srand((unsigned)gettimeusec());
    seeded = 1;
  }
  int64_t n = 0;
  if (e->next) {
    exp_t *a = EVAL(e->next->content, env);
    if (isnumber(a))
      n = FIX_VAL(a);
    unrefexp(a);
  }
  unrefexp(e);
  return MAKE_FIX(
      n <= 0 ? 0
             : (int64_t)((double)rand() / ((double)RAND_MAX + 1) * (double)n));
}

/* (map fn list) — non-destructive; returns a new list of (fn x) values. */
const char doc_map[] = "(map fn xs ...) — apply fn to corresponding elements "
                       "of one or more lists; returns a new list.";
exp_t *mapcmd(exp_t *e, env_t *env) {
  exp_t *head = NULL, *tail = NULL;
  EVAL_ARG_2(fn, xs);
  /* Require the list-arg FORM (arity), but a NULL VALUE is the empty list,
     not a missing arg: (cdr <1-elem>) yields C NULL, which must filter/map
     as () — not error. (!fn short-circuits if e->next is missing.) */
  if (!fn || !e->next->next)
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_MISSING_PARAMETER, e, env, "(map fn list)"));
  exp_t *xseq = coll_to_list(xs);
  if (!xseq)
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "map: second argument is not a sequence"));

  exp_t *cur = xseq;
  while (ispair(cur) && cur->content) {
    exp_t *res = alc_apply1(fn, cur->content, env);
    if (res && iserror(res)) {
      if (head)
        unrefexp(head);
      unrefexp(xseq);
      CLEAN_RETURN_2(fn, xs, res);
    }
    if (!res)
      res = NIL_EXP;
    exp_t *node = make_node(res);
    if (!head) {
      head = node;
      tail = node;
    } else {
      tail->next = node;
      tail = node;
    }
    cur = cur->next;
  }
  unrefexp(xseq);
  CLEAN_RETURN_2(fn, xs, head ? head : NIL_EXP);
}
/* (filter pred list) — keep elements where (pred x) is truthy. */
const char doc_filter[] = "(filter pred xs) — list of elements of xs for which "
                          "(pred elem) is truthy.";
exp_t *filtercmd(exp_t *e, env_t *env) {
  exp_t *head = NULL, *tail = NULL;
  EVAL_ARG_2(fn, xs);
  if (!fn || !e->next->next) /* NULL list value = empty list, not missing arg */
    CLEAN_RETURN_2(
        fn, xs, error(ERROR_MISSING_PARAMETER, e, env, "(filter pred list)"));
  exp_t *xseq = coll_to_list(xs);
  if (!xseq)
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "filter: second argument is not a sequence"));

  exp_t *cur = xseq;
  while (ispair(cur) && cur->content) {
    exp_t *res = alc_apply1(fn, cur->content, env);
    if (res && iserror(res)) {
      if (head)
        unrefexp(head);
      unrefexp(xseq);
      CLEAN_RETURN_2(fn, xs, res);
    }
    int keep = (res != NULL && res != NIL_EXP);
    if (res)
      unrefexp(res);
    if (keep) {
      exp_t *node = make_node(refexp(cur->content));
      if (!head) {
        head = node;
        tail = node;
      } else {
        tail->next = node;
        tail = node;
      }
    }
    cur = cur->next;
  }
  unrefexp(xseq);
  CLEAN_RETURN_2(fn, xs, head ? head : NIL_EXP);
}
/* (reduce fn init list) — left fold: ((fn (fn init x0) x1) x2 ...). */
const char doc_reduce[] = "(reduce fn init xs) — left fold: (fn (fn (fn init "
                          "x0) x1) x2)... Returns init for empty xs.";
exp_t *reducecmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(fn, acc, xs);
  /* Require all three arg FORMS (arity), but NULL VALUES are nil/empty —
     a NULL init or a NULL list (e.g. from (cdr <1-elem>)) must fold as
     init/() rather than error. (!fn short-circuits the form derefs.) */
  if (!fn || !e->next->next || !e->next->next->next)
    CLEAN_RETURN_3(
        fn, acc, xs,
        error(ERROR_MISSING_PARAMETER, e, env, "(reduce fn init list)"));
  if (!acc)
    acc = NIL_EXP; /* NULL init seed → nil */
  exp_t *xseq = coll_to_list(xs);
  if (!xseq)
    CLEAN_RETURN_3(fn, acc, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "reduce: third argument is not a sequence"));

  /* Fast path: detect a simple 6-byte binary-arithmetic lambda
     (fn (a b) (op a b)) — bytecode is LOAD_SLOT 0, LOAD_SLOT 1, OP, RET.
     Common across reduce-sum, reduce-product, reduce-max, etc. We
     inline the tagged-fixnum operation directly, skipping the
     vm_invoke_values per-element env churn. Gives ~10x on listsum-style
     workloads. Falls back to the general path on non-fixnum or when
     the lambda has any other shape. */
  int fast_op = 0; /* 0=none, 1=add, 2=sub, 3=mul */
  if (islambda(fn) && (fn->flags & FLAG_COMPILED) && fn->bc &&
      fn->bc->ncode == 6) {
    uint8_t *c = fn->bc->code;
    if (c[0] == OP_LOAD_SLOT && c[1] == 0 && c[2] == OP_LOAD_SLOT &&
        c[3] == 1 && c[5] == OP_RET) {
      if (c[4] == OP_ADD)
        fast_op = 1;
      else if (c[4] == OP_SUB)
        fast_op = 2;
      else if (c[4] == OP_MUL)
        fast_op = 3;
    }
  }

  exp_t *cur = xseq;
  if (fast_op) {
    while (ispair(cur) && cur->content) {
      exp_t *x = cur->content;
      if (isnumber(acc) && isnumber(x)) {
        int64_t a = FIX_VAL(acc), b = FIX_VAL(x);
        int64_t r = (fast_op == 1)   ? (a + b)
                    : (fast_op == 2) ? (a - b)
                                     : (a * b);
        acc = MAKE_FIX(r);
      } else {
        acc = alc_apply2(fn, acc, refexp(x), env);
        if (acc && iserror(acc)) {
          unrefexp(xseq);
          CLEAN_RETURN_2(fn, xs, acc);
        }
        if (!acc)
          acc = NIL_EXP;
      }
      cur = cur->next;
    }
  } else {
    while (ispair(cur) && cur->content) {
      acc = alc_apply2(fn, acc, refexp(cur->content), env);
      if (acc && iserror(acc)) {
        unrefexp(xseq);
        CLEAN_RETURN_2(fn, xs, acc);
      }
      if (!acc)
        acc = NIL_EXP;
      cur = cur->next;
    }
  }
  unrefexp(xseq);
  CLEAN_RETURN_2(fn, xs, acc);
}

/* (any? pred list) — return t as soon as (pred x) is truthy for any
   element of list, nil if none match. Walks in C with one
   vm_invoke_values per element instead of recursive bytecode. */
const char doc_any[] = "(any? pred xs) — t if pred is truthy for at least one "
                       "element. Short-circuits.";
exp_t *anypcmd(exp_t *e, env_t *env) {
  exp_t *ret = NIL_EXP;
  EVAL_ARG_2(fn, xs);
  if (!fn || !e->next->next) /* NULL list value = empty list, not missing arg */
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_MISSING_PARAMETER, e, env, "(any? pred list)"));
  if (NOT_A_LIST(xs))
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "any?: second argument is not a list"));

  exp_t *cur = xs;
  while (ispair(cur) && cur->content) {
    exp_t *res = alc_apply1(fn, cur->content, env);
    if (res && iserror(res))
      CLEAN_RETURN_2(fn, xs, res);
    int truthy = (res != NULL && res != NIL_EXP);
    if (res)
      unrefexp(res);
    if (truthy) {
      ret = TRUE_EXP;
      break;
    }
    cur = cur->next;
  }
  CLEAN_RETURN_2(fn, xs, ret);
}
/* (all? pred list) — return t if (pred x) is truthy for every
   element, nil at the first failure. Empty list → t. */
const char doc_all[] = "(all? pred xs) — t if pred is truthy for every element "
                       "(vacuously t for empty). Short-circuits.";
exp_t *allpcmd(exp_t *e, env_t *env) {
  exp_t *ret = TRUE_EXP;
  EVAL_ARG_2(fn, xs);
  if (!fn || !e->next->next) /* NULL list value = empty list, not missing arg */
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_MISSING_PARAMETER, e, env, "(all? pred list)"));
  if (NOT_A_LIST(xs))
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "all?: second argument is not a list"));

  exp_t *cur = xs;
  while (ispair(cur) && cur->content) {
    exp_t *res = alc_apply1(fn, cur->content, env);
    if (res && iserror(res))
      CLEAN_RETURN_2(fn, xs, res);
    int truthy = (res != NULL && res != NIL_EXP);
    if (res)
      unrefexp(res);
    if (!truthy) {
      ret = NIL_EXP;
      break;
    }
    cur = cur->next;
  }
  CLEAN_RETURN_2(fn, xs, ret);
}

/* (apply fn args-list) — call fn with each element of args-list as
   separate args. Implemented by re-using vm_invoke_values for compiled
   lambdas; falls back to AST invoke otherwise. */
const char doc_apply[] = "(apply fn args) — call fn with the elements of the "
                         "list args as its arguments.";
exp_t *applycmd(exp_t *e, env_t *env) {
  exp_t *fn = NULL, *args = NULL, *ret = NULL;
  if (e->next && e->next->next) {
    fn = EVAL(e->next->content, env);
    if (iserror(fn)) {
      unrefexp(e);
      return fn;
    }
    args = EVAL(e->next->next->content, env);
    if (iserror(args)) {
      unrefexp(fn);
      unrefexp(e);
      return args;
    }
    /* The 2nd arg must be a proper list; a tagged immediate (e.g. (apply + 5))
       would otherwise be walked as a cons and deref a non-pointer → SEGV. */
    if (args != NIL_EXP && !ispair(args)) {
      exp_t *err = error(ERROR_ILLEGAL_VALUE, e, env,
                         "(apply fn args): args must be a list");
      unrefexp(fn);
      unrefexp(args);
      unrefexp(e);
      return err;
    }
    /* Materialize args as an exp_t** so vm_invoke_values can take it. */
    int n = 0;
    exp_t *c = args;
    while (c && c->content) {
      n++;
      c = c->next;
    }
    exp_t **argv = (n > 0) ? memalloc(n, sizeof(exp_t *)) : NULL;
    int i = 0;
    c = args;
    while (c && c->content && i < n) {
      argv[i++] = refexp(c->content);
      c = c->next;
    }
    /* alc_apply_n dispatches lambda / builtin / continuation and consumes argv
       (errors + frees argv for a non-callable) — so apply works with a builtin
       like type-of, not just lambdas. */
    ret = alc_apply_n(fn, n, argv, env);
    free(argv);
  } else
    ret = error(ERROR_MISSING_PARAMETER, e, env, "(apply fn args)");
  unrefexp(fn);
  unrefexp(args);
  unrefexp(e);
  return ret;
}

/* Call any callable (lambda or builtin) with pre-evaluated args.
   Builtins receive their canonical `e` list; lambdas use vm_invoke_values.
   The caller keeps ownership of `fn`; argv ownership is transferred. */
static exp_t *alc_apply_n(exp_t *fn, int nargs, exp_t **argv, env_t *env) {
  if (islambda(fn))
    return vm_invoke_values(fn, nargs, argv, env);
  if (isinternal(fn)) {
    exp_t *head = make_node(refexp(fn));
    exp_t *cur = head;
    for (int i = 0; i < nargs; i++)
      cur = cur->next = make_node(argv[i]);
    int was_tail = in_tail_position;
    in_tail_position = 0;
    exp_t *ret = fn->fnc(head, env);
    in_tail_position = was_tail;
    return ret;
  }
  if (iscont(fn)) { /* escape continuation invoked via apply/map/etc. */
    exp_t *payload = nargs > 0 ? refexp(argv[0]) : refexp(NIL_EXP);
    for (int i = 0; i < nargs; i++)
      unrefexp(argv[i]);
    return make_cont_escape((int64_t)(intptr_t)fn->meta, payload, env);
  }
  for (int i = 0; i < nargs; i++)
    unrefexp(argv[i]);
  return error(ERROR_ILLEGAL_VALUE, fn, env, "not a callable");
}
static exp_t *alc_apply1(exp_t *fn, exp_t *arg, env_t *env) {
  exp_t *argv[1] = {refexp(arg)};
  return alc_apply_n(fn, 1, argv, env);
}
static exp_t *alc_apply2(exp_t *fn, exp_t *a, exp_t *b, env_t *env) {
  exp_t *argv[2] = {a, b};
  return alc_apply_n(fn, 2, argv, env);
}

/* ---- New stdlib additions -------------------------------------------- */

/* (list? x) — t if x is nil or a proper list (all cdrs end with nil). */
const char doc_listp[] = "(list? x) — t if x is nil or a proper list.";
exp_t *listpcmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NIL_EXP;
  if (e->next) {
    a = EVAL(e->next->content, env);
    if (iserror(a)) {
      unrefexp(e);
      return a;
    }
    exp_t *cur = a;
    while (ispair(cur) && cur->content)
      cur = cur->next;
    if (!cur || cur == NIL_EXP)
      ret = TRUE_EXP;
  }
  unrefexp(a);
  unrefexp(e);
  return ret;
}

/* (null? x) — t if x is nil (empty list / false). Complements pair?. */
const char doc_nullp[] = "(null? x) — t if x is nil.";
exp_t *nullpcmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NIL_EXP;
  if (e->next) {
    a = EVAL(e->next->content, env);
    if (iserror(a)) {
      unrefexp(e);
      return a;
    }
    if (!a || a == NIL_EXP)
      ret = TRUE_EXP;
  }
  unrefexp(a);
  unrefexp(e);
  return ret;
}

static int64_t gensym_counter = 0;
static exp_t *make_gensym(void) {
  char buf[32];
  int n = snprintf(buf, sizeof buf, "G%lld", (long long)gensym_counter++);
  return make_symbol(buf, n);
}

/* (gensym) — return a fresh symbol G0, G1, G2, … unique per session. */
const char doc_gensym[] = "(gensym) — unique symbol each call: G0, G1, …";
exp_t *gensymcmd(exp_t *e, env_t *env) {
  unrefexp(e);
  (void)env;
  return make_gensym();
}

const char doc_withgensyms[] =
    "(with-gensyms (s ...) body ...) — bind each name to a fresh unique "
    "symbol, then evaluate body forms in that scope. Used inside defmacro "
    "to avoid variable capture: "
    "(with-gensyms (tmp) `(let ,tmp ,x ,tmp)).";
exp_t *withgensymscmd(exp_t *e, env_t *env) {
  exp_t *names_node = e->next;
  exp_t *body_start = names_node ? names_node->next : NULL;
  /* names_node->content is either a pair (non-empty list) or nil (()) */
  if (!names_node || !body_start ||
      (!ispair(names_node->content) && istrue(names_node->content))) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, NULL, env,
                 "with-gensyms: expected (name-list body...)");
  }
  env_t *newenv = make_env(env);
  if (!newenv->d)
    newenv->d = create_dict();
  exp_t *names = names_node->content;
  while (names && ispair(names) && istrue(names)) {
    exp_t *nm = names->content;
    if (!issymbol(nm)) {
      destroy_env(newenv);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "with-gensyms: names must be symbols");
    }
    exp_t *gs = make_gensym();
    set_get_keyval_dict(newenv->d, exp_text(nm), gs);
    unrefexp(gs);
    names = names->next;
  }
  exp_t *ret = NIL_EXP;
  for (exp_t *b = body_start; b; b = b->next) {
    if (ret != NIL_EXP)
      unrefexp(ret);
    ret = EVAL(b->content, newenv);
    if (iserror(ret))
      break;
  }
  destroy_env(newenv);
  unrefexp(e);
  return ret;
}

/* (take n xs) — first n elements of list xs. */
const char doc_take[] = "(take n xs) — list of the first n elements of xs.";
exp_t *takecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(n, xs);
  if (!n || !isnumber(n))
    CLEAN_RETURN_2(n, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "take: first arg must be a number"));
  int64_t count = FIX_VAL(n);
  exp_t *ret = NIL_EXP, *tail = NULL;
  exp_t *cur = xs;
  for (int64_t i = 0; i < count && ispair(cur) && cur->content; i++) {
    list_append_owned(&ret, &tail, refexp(cur->content));
    cur = cur->next;
  }
  CLEAN_RETURN_2(n, xs, ret);
}

/* (drop n xs) — xs with the first n elements removed. */
const char doc_drop[] = "(drop n xs) — xs without its first n elements.";
exp_t *dropcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(n, xs);
  if (!n || !isnumber(n))
    CLEAN_RETURN_2(n, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "drop: first arg must be a number"));
  int64_t count = FIX_VAL(n);
  exp_t *cur = xs;
  for (int64_t i = 0; i < count && ispair(cur) && cur->content; i++)
    cur = cur->next;
  exp_t *ret = NIL_EXP, *tail = NULL;
  while (ispair(cur) && cur->content) {
    list_append_owned(&ret, &tail, refexp(cur->content));
    cur = cur->next;
  }
  CLEAN_RETURN_2(n, xs, ret);
}

/* (range start end) / (range start end step) — list of integers. */
const char doc_range[] =
    "(range start end) or (range start end step) — list of integers "
    "from start (inclusive) to end (exclusive).";
exp_t *rangecmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(arg1, arg2, arg3);
  /* Arity is on the FORMS (e->next chain), not the values — a present arg
     that evaluates to a NULL value is still "supplied" and must hit the
     type check, exactly as the original goto-bad arity test did. */
  if (!e->next || !e->next->next)
    CLEAN_RETURN_3(arg1, arg2, arg3,
                   error(ERROR_MISSING_PARAMETER, e, env,
                         "(range start end [step])"));
  if (!isnumber(arg1) || !isnumber(arg2) || (arg3 && !isnumber(arg3)))
    CLEAN_RETURN_3(
        arg1, arg2, arg3,
        error(ERROR_ILLEGAL_VALUE, NULL, env, "range: args must be integers"));
  int64_t start = FIX_VAL(arg1), end = FIX_VAL(arg2);
  int64_t step = arg3 ? FIX_VAL(arg3) : (start <= end ? 1 : -1);
  if (step == 0)
    CLEAN_RETURN_3(
        arg1, arg2, arg3,
        error(ERROR_ILLEGAL_VALUE, NULL, env, "range: step cannot be 0"));
  exp_t *ret = NIL_EXP, *tail = NULL;
  for (int64_t i = start; step > 0 ? i < end : i > end; i += step)
    list_append_owned(&ret, &tail, MAKE_FIX(i));
  CLEAN_RETURN_3(arg1, arg2, arg3, ret);
}

/* (zip xs ys) — list of (x y) pairs from two lists. Stops at shorter. */
const char doc_zip[] =
    "(zip xs ys) — list of (x y) pairs; stops at the shorter list.";
exp_t *zipcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(xs, ys);
  exp_t *ret = NIL_EXP, *tail = NULL;
  exp_t *cx = xs, *cy = ys;
  while (ispair(cx) && cx->content && ispair(cy) && cy->content) {
    exp_t *pair = make_node(refexp(cx->content));
    pair->next = make_node(refexp(cy->content));
    list_append_owned(&ret, &tail, pair);
    cx = cx->next;
    cy = cy->next;
  }
  CLEAN_RETURN_2(xs, ys, ret);
}

/* flatten helper (non-recursive, uses a stack to avoid C stack growth) */
static void flatten_into(exp_t *x, exp_t **ret, exp_t **tail) {
  if (!x || x == NIL_EXP)
    return;
  if (!ispair(x) || !x->content) {
    list_append_owned(ret, tail, refexp(x));
    return;
  }
  exp_t *cur = x;
  while (ispair(cur) && cur->content) {
    exp_t *v = cur->content;
    if (ispair(v) && v->content)
      flatten_into(v, ret, tail);
    else if (v && v != NIL_EXP)
      list_append_owned(ret, tail, refexp(v));
    cur = cur->next;
  }
}

/* (flatten xs) — recursively flatten nested lists into a flat list. */
const char doc_flatten[] = "(flatten xs) — recursively flatten nested lists.";
exp_t *flattencmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(xs);
  exp_t *ret = NIL_EXP, *tail = NULL;
  flatten_into(xs, &ret, &tail);
  CLEAN_RETURN_1(xs, ret);
}

/* sort helpers */
typedef struct {
  exp_t **arr;
  int n;
} sort_ctx;

static int sort_cmp_default(const void *a, const void *b) {
  exp_t *x = *(exp_t **)a, *y = *(exp_t **)b;
  if (isnumber(x) && isnumber(y)) {
    int64_t dx = FIX_VAL(x), dy = FIX_VAL(y);
    return dx < dy ? -1 : dx > dy ? 1 : 0;
  }
  if (isfloat(x) && isfloat(y))
    return x->f < y->f ? -1 : x->f > y->f ? 1 : 0;
  if (isnumber(x) && isfloat(y)) {
    double dx = (double)FIX_VAL(x);
    return dx < y->f ? -1 : dx > y->f ? 1 : 0;
  }
  if (isfloat(x) && isnumber(y)) {
    double dy = (double)FIX_VAL(y);
    return x->f < dy ? -1 : x->f > dy ? 1 : 0;
  }
  if (isstring(x) && isstring(y))
    return strcmp((char *)exp_text(x), (char *)exp_text(y));
  return 0;
}

/* (sort xs) — sort list with default < ordering (numbers, strings). */
const char doc_sort[] =
    "(sort xs) — sort list using default ordering (numbers by value, "
    "strings lexicographically).";
exp_t *sortcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(xs);
  if (!xs || xs == NIL_EXP)
    CLEAN_RETURN_1(xs, NIL_EXP);
  if (!ispair(xs))
    CLEAN_RETURN_1(
        xs, error(ERROR_ILLEGAL_VALUE, NULL, env, "sort: arg must be a list"));
  int n = 0;
  for (exp_t *c = xs; ispair(c) && c->content; c = c->next)
    n++;
  exp_t **arr = memalloc(n, sizeof *arr);
  int i = 0;
  for (exp_t *c = xs; ispair(c) && c->content; c = c->next)
    arr[i++] = c->content;
  qsort(arr, n, sizeof *arr, sort_cmp_default);
  exp_t *ret = NIL_EXP, *tail = NULL;
  for (i = 0; i < n; i++)
    list_append_owned(&ret, &tail, refexp(arr[i]));
  free(arr);
  CLEAN_RETURN_1(xs, ret);
}

/* (sort-by key-fn xs) — sort list by (key-fn element). */
const char doc_sortby[] = "(sort-by key-fn xs) — sort xs by (key-fn element).";

typedef struct {
  exp_t *val;
  exp_t *key;
} sortby_pair;

static int sort_cmp_by(const void *a, const void *b) {
  const sortby_pair *pa = (const sortby_pair *)a;
  const sortby_pair *pb = (const sortby_pair *)b;
  return sort_cmp_default(&pa->key, &pb->key);
}

exp_t *sortbycmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(fn, xs);
  if (!fn || !e->next->next) /* NULL list value = empty list, not missing arg */
    CLEAN_RETURN_2(
        fn, xs, error(ERROR_MISSING_PARAMETER, e, env, "(sort-by key-fn xs)"));
  if (xs == NIL_EXP)
    CLEAN_RETURN_2(fn, xs, NIL_EXP);
  if (!ispair(xs))
    CLEAN_RETURN_2(fn, xs,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "sort-by: second arg must be a list"));
  int n = 0;
  for (exp_t *c = xs; ispair(c) && c->content; c = c->next)
    n++;
  sortby_pair *pairs = memalloc(n, sizeof *pairs);
  int i = 0;
  for (exp_t *c = xs; ispair(c) && c->content; c = c->next) {
    pairs[i].val = c->content;
    pairs[i].key = alc_apply1(fn, c->content, env);
    if (!pairs[i].key)
      pairs[i].key = NIL_EXP;
    if (iserror(pairs[i].key)) {
      exp_t *err = pairs[i].key;
      for (int j = 0; j < i; j++)
        unrefexp(pairs[j].key);
      free(pairs);
      CLEAN_RETURN_2(fn, xs, err);
    }
    i++;
  }
  qsort(pairs, n, sizeof *pairs, sort_cmp_by);
  exp_t *ret = NIL_EXP, *tail = NULL;
  for (i = 0; i < n; i++) {
    list_append_owned(&ret, &tail, refexp(pairs[i].val));
    unrefexp(pairs[i].key);
  }
  free(pairs);
  CLEAN_RETURN_2(fn, xs, ret);
}

/* (string-contains? s sub) — t if s contains substring sub. */
const char doc_stringcontainsp[] =
    "(string-contains? s sub) — t if string s contains substring sub.";
exp_t *stringcontainspcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(s, sub);
  if (!isstring(s) || !isstring(sub))
    CLEAN_RETURN_2(s, sub,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "string-contains?: args must be strings"));
  exp_t *ret =
      strstr((char *)exp_text(s), (char *)exp_text(sub)) ? TRUE_EXP : NIL_EXP;
  CLEAN_RETURN_2(s, sub, ret);
}

/* (string-index s sub) — 0-based index of first occurrence, or nil. */
const char doc_stringindex[] =
    "(string-index s sub) — index of first occurrence of sub in s, or nil.";
exp_t *stringindexcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(s, sub);
  if (!isstring(s) || !isstring(sub))
    CLEAN_RETURN_2(s, sub,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "string-index: args must be strings"));
  char *base = (char *)exp_text(s);
  char *found = strstr(base, (char *)exp_text(sub));
  exp_t *ret = found ? MAKE_FIX(utf8_count_bytes(base, (size_t)(found - base)))
                     : NIL_EXP;
  CLEAN_RETURN_2(s, sub, ret);
}

/* (string-replace s old new) — replace all occurrences of old with new. */
const char doc_stringreplace[] =
    "(string-replace s old new) — replace all occurrences of old with new.";
exp_t *stringreplacecmd(exp_t *e, env_t *env) {
  exp_t *s = NULL, *old = NULL, *nw = NULL;
  if (!e->next || !e->next->next || !e->next->next->next)
    goto bad;
  s = EVAL(e->next->content, env);
  if (iserror(s)) {
    unrefexp(e);
    return s;
  }
  old = EVAL(e->next->next->content, env);
  if (iserror(old)) {
    unrefexp(s);
    unrefexp(e);
    return old;
  }
  nw = EVAL(e->next->next->next->content, env);
  if (iserror(nw)) {
    unrefexp(s);
    unrefexp(old);
    unrefexp(e);
    return nw;
  }
  if (!isstring(s) || !isstring(old) || !isstring(nw)) {
    unrefexp(s);
    unrefexp(old);
    unrefexp(nw);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "string-replace: args must be strings");
  }
  {
    const char *haystack = (char *)exp_text(s);
    const char *needle = (char *)exp_text(old);
    const char *replacement = (char *)exp_text(nw);
    size_t nlen = strlen(needle), rlen = strlen(replacement);
    size_t cap = 64, len = 0;
    char *buf = memalloc(cap, 1);
    const char *p = haystack;
    if (nlen == 0) {
      str_buf_put(&buf, &len, &cap, p, strlen(p));
    } else {
      const char *found;
      while ((found = strstr(p, needle)) != NULL) {
        str_buf_put(&buf, &len, &cap, p, (size_t)(found - p));
        str_buf_put(&buf, &len, &cap, replacement, rlen);
        p = found + nlen;
      }
      str_buf_put(&buf, &len, &cap, p, strlen(p));
    }
    exp_t *ret = make_string(buf, (int)len);
    free(buf);
    unrefexp(s);
    unrefexp(old);
    unrefexp(nw);
    unrefexp(e);
    return ret;
  }
bad: {
  exp_t *err =
      error(ERROR_MISSING_PARAMETER, e, env, "(string-replace s old new)");
  unrefexp(s);
  unrefexp(old);
  unrefexp(e);
  return err;
}
}

/* (error? x) — t if x is an error value. */
const char doc_errorp[] = "(error? x) — t if x is an error value.";
exp_t *errorpcmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NIL_EXP;
  if (e->next) {
    a = EVAL(e->next->content, env);
    /* Don't propagate: we want to inspect the error, not re-raise it. */
    if (a && iserror(a))
      ret = TRUE_EXP;
  }
  if (a)
    unrefexp(a);
  unrefexp(e);
  return ret;
}

/* (error-message x) — string message from an error value, or nil. */
const char doc_errormessage[] =
    "(error-message x) — extract the message string from an error value.";
exp_t *errormessagecmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NIL_EXP;
  if (e->next) {
    a = EVAL(e->next->content, env);
    if (a && iserror(a) && a->ptr) {
      const char *_t = exp_text(a);
      ret = make_string((char *)_t, (int)strlen((char *)_t));
    }
  }
  if (a)
    unrefexp(a);
  unrefexp(e);
  return ret;
}

/* (try body-expr handler) — evaluate body; on error call (handler err).
   Unlike normal propagation, the error is caught here and not re-raised.
   handler receives the error exp_t and may call (error-message e) on it. */
const char doc_try[] =
    "(try body handler) — evaluate body; if it signals an error call "
    "(handler err). Returns body's value on success or handler's value. "
    "(try body handler finally-expr) — like above but always evaluates "
    "finally-expr last; its value is discarded. "
    "(try body nil finally-expr) — no catch; run body, always run finally "
    "(errors from body propagate after finally runs).";
exp_t *trycmd(exp_t *e, env_t *env) {
  if (!e->next || !e->next->next) {
    unrefexp(e);
    return error(ERROR_MISSING_PARAMETER, NULL, env, "(try body handler)");
  }
  exp_t *finally_form =
      e->next->next->next ? e->next->next->next->content : NULL;
  exp_t *result = EVAL(e->next->content, env);
  exp_t *ret;
  if (!result || !iserror(result)) {
    ret = result ? result : NIL_EXP;
  } else if (is_cont_escape(result)) {
    /* A call/cc escape passing through this try: it belongs to the matching
       call/cc frame, NOT to this handler. Do NOT run the handler — propagate
       the token unchanged. (finally still runs below, on every exit path.) */
    ret = result;
  } else {
    /* Error path: evaluate the handler, then apply it to the error value.
       Only a literal nil handler means "no catch" — NOT any falsey value.
       (A lambda is the normal handler; istrue(lambda) is false, so a
       truthiness test here would reject every real handler.) */
    exp_t *handler = EVAL(e->next->next->content, env);
    if (!handler || handler == NIL_EXP) {
      ret = result; /* nil handler = no catch; propagate the body error */
    } else if (iserror(handler)) {
      unrefexp(result); /* handler eval itself failed — surface that error */
      ret = handler;
    } else {
      ret = alc_apply1(handler, result, env);
      unrefexp(handler);
      unrefexp(result);
      if (!ret)
        ret = NIL_EXP;
    }
  }
  if (finally_form) {
    exp_t *fret = EVAL(finally_form, env);
    if (fret && iserror(fret) && !iserror(ret)) {
      unrefexp(ret);
      ret = fret;
    } else
      unrefexp(fret);
  }
  unrefexp(e);
  return ret ? ret : NIL_EXP;
}

/* ---------- escape continuations (call/cc) ----------
   alcove's evaluator is a recursive C tree-walker (plus a bytecode VM), so a
   FULL re-entrant continuation — resumable more than once, or downward —
   would require capturing the C stack, which isn't portable here. We provide
   the widely-useful subset: ONE-SHOT, UPWARD (escape) continuations.
   (call/cc f) calls f with a continuation k; invoking (k v) abandons the
   in-progress work and makes the call/cc form return v. k is valid only
   during that call/cc's dynamic extent; invoking it afterward is an error.

   Mechanism: invoking k yields an EXP_ERROR-tagged escape token (errnum
   ERROR_CONT_ESCAPE) carrying the continuation id (in `meta`) and the payload
   (in `next`). It propagates up exactly like an error — every iserror
   short-circuit in evaluate/invoke/vm_run/builtins carries it — until the
   matching call/cc frame catches it. No setjmp; rides the existing
   error-propagation plumbing (same model as try/catch). */
static int64_t g_cont_id = 0;

static exp_t *make_cont(int64_t id) {
  exp_t *k = make_nil(); /* content == next == NULL */
  k->type = EXP_CONT;
  k->meta = (struct keyval_t *)(intptr_t)id; /* id lives in the unused meta */
  return k;
}
/* Build an escape token for continuation `id` carrying `payload`. Consumes
   the caller's `payload` ref (error() takes its own via the id parameter). */
static exp_t *make_cont_escape(int64_t id, exp_t *payload, env_t *env) {
  exp_t *tok = error(ERROR_CONT_ESCAPE, payload, env,
                     "call/cc continuation invoked outside its extent");
  tok->meta = (struct keyval_t *)(intptr_t)id;
  unrefexp(payload);
  return tok;
}
/* is_cont_escape() is defined in alcove.h (next to iserror) so the bytecode VM
   can route escape tokens through try handlers too. */

/* Invoke continuation `cont` with `arg` (consumed) → an escape token. */
static exp_t *apply_cont(exp_t *cont, exp_t *arg, env_t *env) {
  return make_cont_escape((int64_t)(intptr_t)cont->meta, arg, env);
}
/* Evaluate a (k arg) call form (arg optional, defaults nil). */
static exp_t *eval_cont_call(exp_t *cont, exp_t *e, env_t *env) {
  exp_t *arg = e->next ? EVAL(e->next->content, env) : refexp(NIL_EXP);
  if (iserror(arg)) /* arg eval failed, or itself escaped — propagate */
    return arg;
  return apply_cont(cont, arg, env);
}

const char doc_callcc[] =
    "(call/cc f) — call f with an escape continuation k; invoking (k v) makes "
    "this call/cc return v, abandoning the work in between. ESCAPE-ONLY: k is "
    "valid only during call/cc's dynamic extent (one-shot, upward) — calling "
    "it later errors. Not a full re-entrant continuation.";
exp_t *callcccmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(f);
  if (!(islambda(f) || isinternal(f)))
    CLEAN_RETURN_1(f, error(ERROR_ILLEGAL_VALUE, e, env,
                            "call/cc: argument must be a function"));
  int64_t id = ++g_cont_id;
  exp_t *k = make_cont(id);
  exp_t *r = alc_apply1(f, k, env); /* alc_apply1 takes its own ref to k */
  exp_t *ret;
  if (r && is_cont_escape(r) && (int64_t)(intptr_t)r->meta == id) {
    ret = refexp(r->next ? r->next : NIL_EXP); /* payload = the escape value */
    unrefexp(r);
  } else {
    ret = r ? r : NIL_EXP; /* normal return, or an escape/error bound for an
                              OUTER frame — propagate unchanged */
  }
  unrefexp(k);
  CLEAN_RETURN_1(f, ret);
}

/* ---- End new stdlib additions ---------------------------------------- */

const char doc_odd[] = "(odd x) — t if integer x is odd, nil otherwise.";
exp_t *oddcmd(exp_t *e, env_t *env) {
  exp_t *ret;
  if (e->next && isnumber(e->next->content))
    ret = ((FIX_VAL(e->next->content) & 1) ? TRUE_EXP : NIL_EXP);
  else
    ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in operation");
  unrefexp(e);
  return ret;
}

const char doc_do[] = "(do expr ...) — evaluate exprs in order; returns the "
                      "value of the last one.";
exp_t *docmd(exp_t *e, env_t *env) {
  /* Tail-aware: propagates in_tail_position to the final expression so
     a tail call inside (do ... (f x)) actually gets TCO. Returns the
     last expression's value (not nil — that was a pre-existing bug). */
  int outer_tail = in_tail_position;
  exp_t *cur = cdr(e);
  exp_t *ret = NULL;
  while (cur) {
    if (ret)
      unrefexp(ret);
    in_tail_position = (cur->next == NULL) ? outer_tail : 0;
    ret = EVAL(car(cur), env);
    if (ret && iserror(ret)) {
      in_tail_position = outer_tail;
      unrefexp(e);
      return ret;
    }
    cur = cdr(cur);
  }
  in_tail_position = outer_tail;
  unrefexp(e);
  return ret ? ret : NIL_EXP;
}

const char doc_when[] = "(when test expr ...) — if test is truthy, evaluate "
                        "the body in order; else nil.";
exp_t *whencmd(exp_t *e, env_t *env) {
  /* Tail position: when is TAIL_AWARE. The condition must NOT be
     evaluated in tail position — if it is, a user-fn call inside it
     returns a tail marker, and istrue() reports the marker as true
     (it's a non-empty pair), so the body fires when the condition was
     actually nil. Only the last body form inherits the outer tail. */
  int outer_tail = in_tail_position;
  exp_t *val = cadr(e);
  exp_t *cur = cddr(e);
  in_tail_position = 0;
  exp_t *ret = EVAL(val, env);
  if iserror (ret) {
    in_tail_position = outer_tail;
    unrefexp(e);
    return ret;
  }
  /* Body runs only when test is truthy AND there's a body to run
     ((when test) with no body forms gives cur=NULL — the per-arg
     tail-flag read `cur->next` would segfault). */
  int body_ran = 0;
  if (istrue(ret) && cur)
    do {
      unrefexp(ret);
      body_ran = 1;
      in_tail_position = (cur->next == NULL) ? outer_tail : 0;
      ret = EVAL(car(cur), env);
    } while ((cur = cdr(cur)) && !(ret && iserror(ret)));
  in_tail_position = outer_tail;
  if (ret && iserror(ret)) {
    unrefexp(e);
    return ret;
  }
  /* Return the last body expression's value (matches docstring and
     Arc semantics). The pre-existing code unconditionally returned
     NIL_EXP, so (when t 42) gave nil. If the body never ran (falsey
     test, or no body forms), discard ret (which holds the test value
     or last body iteration's truthy ret) and return NIL_EXP. */
  unrefexp(e);
  if (!body_ran) {
    unrefexp(ret);
    return NIL_EXP;
  }
  return ret ? ret : NIL_EXP;
}

const char doc_unless[] =
    "(unless test expr ...) — if test is falsey, evaluate "
    "the body in order; else nil.";
exp_t *unlesscmd(exp_t *e, env_t *env) {
  int outer_tail = in_tail_position;
  exp_t *val = cadr(e);
  exp_t *cur = cddr(e);
  in_tail_position = 0;
  exp_t *ret = EVAL(val, env);
  if iserror (ret) {
    in_tail_position = outer_tail;
    unrefexp(e);
    return ret;
  }
  int body_ran = 0;
  if (!istrue(ret) && cur)
    do {
      unrefexp(ret);
      body_ran = 1;
      in_tail_position = (cur->next == NULL) ? outer_tail : 0;
      ret = EVAL(car(cur), env);
    } while ((cur = cdr(cur)) && !(ret && iserror(ret)));
  in_tail_position = outer_tail;
  if (ret && iserror(ret)) {
    unrefexp(e);
    return ret;
  }
  unrefexp(e);
  if (!body_ran) {
    unrefexp(ret);
    return NIL_EXP;
  }
  return ret ? ret : NIL_EXP;
}

const char doc_while[] = "(while test expr ...) — re-evaluate body while test "
                         "stays truthy. Returns nil.";
exp_t *whilecmd(exp_t *e, env_t *env) {
  exp_t *val = cadr(e);
  exp_t *cur = cddr(e);
  exp_t *curi = cur;
  exp_t *ret = NULL;
  while (istrue(ret = EVAL(val, env)) && (!iserror(ret))) {
    cur = curi;
    do {
      unrefexp(ret);
      ret = EVAL(car(cur), env);
    } while ((cur = cdr(cur)) && !(ret && iserror(ret)));
    /* A body error / call-cc escape must propagate, not be discarded by the
       next condition eval — otherwise the loop swallows it (and a persistent
       error like a fired escape spins forever). */
    if (ret && iserror(ret))
      break;
  }
  if (ret && iserror(ret)) {
    unrefexp(e);
    return ret;
  } else {
    unrefexp(ret);
    unrefexp(e);
    return NIL_EXP;
  }
}

const char doc_repeat[] = "(repeat n expr ...) — run body n times, returning "
                          "the last expression's value.";
exp_t *repeatcmd(exp_t *e, env_t *env) {
  exp_t *val = EVAL(cadr(e), env);
  exp_t *cur = cddr(e);
  exp_t *curi = cur;
  exp_t *ret = NULL;
  int64_t counter = 0;
  if iserror (val) {
    unrefexp(e);
    return val;
  }
  if (isnumber(val))
    counter = FIX_VAL(val);
  else {
    ret =
        error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value for repeat counter");
    unrefexp(val);
    unrefexp(e);
    return ret;
  }
  unrefexp(val);
  while (counter-- > 0) {
    cur = curi;
    do {
      if (ret)
        unrefexp(ret);
      ret = EVAL(car(cur), env);
    } while ((cur = cdr(cur)) && !(ret && iserror(ret)));
  }
  if (ret && iserror(ret)) {
    unrefexp(e);
    return ret;
  } else {
    if (ret)
      unrefexp(ret);
    unrefexp(e);
    return NIL_EXP;
  }
}

const char doc_and[] = "(and expr ...) — short-circuit AND. (and) is t. "
                       "Returns the last truthy or first falsey value.";
exp_t *andcmd(exp_t *e, env_t *env) {
  /* (and) → t (vacuous), per doc. The previous loop EVAL'd car(NULL)
     and ended up returning nil for the empty case.
     Tail position: andcmd is TAIL_AWARE; only the last arg inherits
     the outer tail flag. Earlier args are NOT in tail position — if
     they were, a user-fn call inside one would return a tail marker
     and the trampoline check (ispair+content) would treat it as
     truthy, causing premature short-circuit. */
  int outer_tail = in_tail_position;
  exp_t *cur = cdr(e);
  exp_t *ret = TRUE_EXP;
  while (cur) {
    if (ret != TRUE_EXP)
      unrefexp(ret);
    in_tail_position = (cur->next == NULL) ? outer_tail : 0;
    ret = EVAL(car(cur), env);
    if (iserror(ret))
      goto finish;
    if (!istrue(ret))
      goto finish;
    cur = cdr(cur);
  }
finish:
  in_tail_position = outer_tail;
  unrefexp(e);
  return ret;
}

const char doc_or[] = "(or expr ...) — short-circuit OR. (or) is nil. Returns "
                      "the first truthy value, else nil.";
exp_t *orcmd(exp_t *e, env_t *env) {
  /* See andcmd: only the last arg inherits in_tail_position; earlier
     args must clear it so user-fn calls inside them return real
     values, not tail markers (which the trampoline-check would treat
     as truthy and short-circuit early).
     Loop must guard cur — (or) with no args yields cur=NULL on
     entry, and a do/while body would deref cur->next before the
     condition check. (or) → nil per doc. */
  int outer_tail = in_tail_position;
  exp_t *cur = cdr(e);
  exp_t *ret = NIL_EXP;
  while (cur) {
    if (ret != NIL_EXP)
      unrefexp(ret);
    in_tail_position = (cur->next == NULL) ? outer_tail : 0;
    ret = EVAL(car(cur), env);
    if iserror (ret)
      goto finish;
    if (istrue(ret))
      goto finish;
    cur = cdr(cur);
  }
finish:
  in_tail_position = outer_tail;
  unrefexp(e);
  return ret;
}

const char doc_no[] = "(no x) — t if x is nil or empty list/string, nil "
                      "otherwise. The canonical \"is falsey?\" test.";
exp_t *nocmd(exp_t *e, env_t *env) {
  exp_t *cur = cdr(e);
  exp_t *tmpexp = EVAL(car(cur), env);
  if iserror (tmpexp)
    goto finish;
  if (istrue(cur = tmpexp))
    tmpexp = NIL_EXP;
  else
    tmpexp = TRUE_EXP;
  unrefexp(cur);
finish:
  unrefexp(e);
  return tmpexp;
}

int isequal(exp_t *cur1, exp_t *cur2) {
  /* borrow ref to cur1 and cur2 */
  int ret = 0;
  /* Fast path: any two tagged immediates compare by bit-pattern equality.
     Fixnum 5 == Fixnum 5, char 'a' == char 'a', and cross-type never equal. */
  if (cur1 == cur2)
    return 1;
  if (!is_ptr(cur1) || !is_ptr(cur2))
    return 0;
  if (cur1->type == cur2->type) {
    if (isfloat(cur1))
      ret = (cur1->f == cur2->f);
    else if (issymbol(cur1) || isstring(cur1))
      ret = (strcmp(exp_text(cur1), exp_text(cur2)) == 0);
    else if (iserror(cur1))
      ret = (cur1->s64 == cur2->s64);
    else if (isblob(cur1)) {
      alc_blob_t *a = (alc_blob_t *)cur1->ptr;
      alc_blob_t *b = (alc_blob_t *)cur2->ptr;
      ret = (a && b && a->len == b->len &&
             (a->len == 0 || memcmp(a->bytes, b->bytes, a->len) == 0));
    } else
      /* Dict/list: pointer identity. Deep equality would require walking
         every entry/node and is rarely what Redis-style users want. */
      ret = (cur1 == cur2);
  }
  return ret;
}

static int hamt_iso(exp_t *a, exp_t *b); /* deep map equality; HAMT section */

/* Recursion-depth guard: two distinct cyclic collections (e.g. a dict that
   contains itself under different identities) would recurse forever and blow
   the C stack. Depth tracks NESTING (lists/vectors/dicts walk their spine in a
   loop and only recurse per element), so a few thousand levels is far beyond any
   real data while staying well under an 8 MB stack — even with ASan's fat frames.
   Hitting the cap means "give up — treat as not equal" rather than crash. */
#define ISO_MAX_DEPTH 2000
static ALCOVE_TLS int g_iso_depth = 0;

int isoequal(exp_t *cur1, exp_t *cur2) {
  /* borrow ref to cur1 and cur2 */
  int ret = 0;
  exp_t *cur1n;
  exp_t *cur2n;

  if (cur1 == cur2)
    return 1;
  if (!is_ptr(cur1) || !is_ptr(cur2))
    return 0;
  if (g_iso_depth >= ISO_MAX_DEPTH)
    return 0; /* runaway recursion (cyclic structure) — bail out */
  g_iso_depth++;
  if (cur1->type == cur2->type) {
    if (ispair(cur1)) {
      cur1n = cur1;
      cur2n = cur2;
      ret = 1;
      /* Walk both lists in lockstep WHILE BOTH ARE PAIRS, comparing cars.
         Advance via ->next only after confirming a pair, so an improper
         (dotted) tail — e.g. (cons 1 2), whose ->next is a non-pointer
         fixnum immediate — is never dereferenced as a pair (was a segv). */
      while (ret && ispair(cur1n) && ispair(cur2n)) {
        ret = isoequal(cur1n->content, cur2n->content);
        cur1n = cur1n->next;
        cur2n = cur2n->next;
      }
      /* Compare the tails: NULL/NULL for proper lists, or the dotted
         final values for improper lists (e.g. the 2 in (cons 1 2)). */
      if (ret)
        ret = isoequal(cur1n, cur2n);
    } else if (isvector(cur1)) {
      /* Element-wise deep equality — doc_iso promises vector recursion.
         vec_get_boxed returns an owned ref, so release each after compare. */
      int64_t n1 = vec_len(cur1), n2 = vec_len(cur2);
      if (n1 != n2)
        ret = 0;
      else {
        ret = 1;
        for (int64_t i = 0; i < n1 && ret; i++) {
          exp_t *a = vec_get_boxed(cur1, i);
          exp_t *b = vec_get_boxed(cur2, i);
          ret = isoequal(a, b);
          unrefexp(a);
          unrefexp(b);
        }
      }
    } else if (ishamt(cur1)) {
      ret = hamt_iso(cur1, cur2); /* same entries (deep), order-independent */
    } else if (isdict(cur1)) {
      /* same number of keys, and every key maps to an iso-equal value */
      dict_t *d1 = (dict_t *)cur1->ptr, *d2 = (dict_t *)cur2->ptr;
      if (!d1 || !d2)
        ret = (d1 == d2);
      else if (d1->ht[0].used != d2->ht[0].used)
        ret = 0;
      else {
        ret = 1;
        DICT_FOREACH(d1, k, 0, 0) {
          keyval_t *k2 = set_get_keyval_dict(d2, (char *)k->key, NULL);
          if (ret && (!k2 || !isoequal(k->val, k2->val)))
            ret = 0;
        }
      }
    } else if (isset(cur1)) {
      /* same number of members, and every member of cur1 is in cur2 (members
         are the keys, so matching keys is enough — no value compare needed) */
      dict_t *d1 = (dict_t *)cur1->ptr, *d2 = (dict_t *)cur2->ptr;
      if (!d1 || !d2)
        ret = (d1 == d2);
      else if (d1->ht[0].used != d2->ht[0].used)
        ret = 0;
      else {
        ret = 1;
        DICT_FOREACH(d1, k, 0, 0) {
          if (ret && !set_get_keyval_dict(d2, (char *)k->key, NULL))
            ret = 0;
        }
      }
    } else
      ret = isequal(cur1, cur2);
  }
  g_iso_depth--;
  return ret;
}

const char doc_is[] =
    "(is a b) — pointer/value identity. Same fixnum or same heap object. "
    "Aliases: eq, eq?.";
EQUALITY_CMD(iscmd, isequal)

const char doc_iso[] = "(iso a b) — structural (deep) equality. Recurses into "
                       "pairs/strings/vectors.";
EQUALITY_CMD(isocmd, isoequal)

const char doc_in[] =
    "(in val a b c ...) — t if (is val) matches any of the rest.";
exp_t *incmd(exp_t *e, env_t *env) {
  exp_t *cur = cdr(e);
  exp_t *val = EVAL(cadr(e), env);
  exp_t *val2 = NULL;

  if iserror (val) {
    unrefexp(e);
    return val;
  }
  int ret = 0;
  while ((cur = cdr(cur))) {
    if (val2)
      unrefexp(val2);
    val2 = EVAL(car(cur), env);
    if iserror (val2) {
      unrefexp(e);
      unrefexp(val);
      return val2;
    }
    if ((ret = isoequal(val, val2)))
      break;
  }

  cur = (ret ? TRUE_EXP : NIL_EXP);
  unrefexp(val);
  if (val2)
    unrefexp(val2);
  unrefexp(e);
  return cur;
}

const char doc_case[] =
    "(case key v1 e1 v2 e2 ... default) — Arc-style flat pairs (NOT (val expr) "
    "clauses). First v that matches key returns its e; trailing odd element is "
    "the default.";
exp_t *casecmd(exp_t *e, env_t *env) {
  /* Tail position: case is TAIL_AWARE. The discriminant must NOT be
     in tail position (it's used for matching, not returned). The
     selected body expression IS the return value, so it inherits the
     outer tail flag. Same trap as when/and/or: a tail marker from a
     user-fn call would mis-compare via isequal. */
  int outer_tail = in_tail_position;
  exp_t *cur = cdr(e);
  in_tail_position = 0;
  exp_t *val = EVAL(cadr(e), env);
  if iserror (val) {
    in_tail_position = outer_tail;
    unrefexp(e);
    return val;
  }
  exp_t *ret = NULL;
  while ((cur = cdr(cur)))
    if (cur->next) {
      if (isequal(val, car(cur))) {
        ret = cadr(cur);
        break;
      } else
        cur = cdr(cur);
    } else
      ret = car(cur);
  unrefexp(val);
  in_tail_position = outer_tail;
  cur = EVAL(ret, env);
  unrefexp(e);
  return cur;
}

const char doc_for[] =
    "(for var start end body ...) — iterate var from start to end inclusive, "
    "evaluating body. Returns last body value.";
exp_t *forcmd(exp_t *e, env_t *env) {
  env_t *newenv = make_env(env);
  exp_t *ret = NULL;
  exp_t *curvar;
  exp_t *curval;
  exp_t *curin;
  exp_t *lastidx = NULL;
  exp_t *retval = NULL;

  if ((curvar = e->next)) {
    if ((curval = curvar->next)) {
      CHECK_RESERVED_BIND(curvar->content, ret, "in for", {
        destroy_env(newenv);
        unrefexp(e);
        return ret;
      });
      if (!(newenv->d))
        newenv->d = create_dict();

      if (issymbol(curvar->content)) {
        if ((retval = EVAL(curval->content, env)) == NULL)
          retval = NIL_EXP;
        if (isnumber(retval)) {
          if (!curval->next)
            lastidx = NIL_EXP;
          if (curval->next &&
              (lastidx = EVAL(curval->next->content, env)) == NULL)
            lastidx = NIL_EXP;
          if (isnumber(lastidx)) {
            curin = curval->next->next;
          } else {
            if iserror (lastidx)
              ret = refexp(lastidx);
            else
              ret = error(ERROR_ILLEGAL_VALUE, e, env,
                          "Illegal value (not integer) in for counter");
            goto error;
          }
        } else {
          if iserror (retval)
            ret = refexp(retval);
          else
            ret = error(ERROR_ILLEGAL_VALUE, e, env,
                        "Illegal value (not integer) in for counter");
          goto error;
        }

      } else {
        ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in for");
        goto error;
      }
      {
        int64_t counter = FIX_VAL(retval);
        int64_t idx = FIX_VAL(lastidx) + 1;
        while (counter < idx) {
          /* Rebind the loop variable to a fresh tagged fixnum. */
          set_get_keyval_dict(newenv->d, exp_text(curvar->content),
                              MAKE_FIX(counter));
          curval = curin;
          while (curval) {
            if (ret)
              unrefexp(ret);
            ret = EVAL(curval->content, newenv);
            if (iserror(ret))
              goto error;
            /* NULL is treated as nil — some builtins (historically prn,
               others may return NULL too) didn't return NIL_EXP. We
               normalize here so the post-loop "if (!ret)" doesn't
               misread a clean iteration as "missing parameter". */
            if (!ret)
              ret = NIL_EXP;
            curval = curval->next;
          }
          counter++;
        }
      }
    }
  }
error:
  destroy_env(newenv);
  if (!ret)
    ret = error(ERROR_MISSING_PARAMETER, e, env, "Missing parameter in for");
  if (lastidx)
    unrefexp(lastidx);
  if (retval)
    unrefexp(retval);
  unrefexp(e);
  return ret;
}

const char doc_each[] = "(each var coll body ...) — bind var to each element "
                        "of coll (list/string/vector) and run body.";
exp_t *eachcmd(exp_t *e, env_t *env) {
  env_t *newenv = make_env(env);
  exp_t *curvar;
  exp_t *curval;
  exp_t *curin;
  exp_t *retval = NULL;
  exp_t *tmpexp = NULL;
  exp_t *ret = NULL;

  if ((curvar = e->next)) {
    if ((curval = curvar->next)) {
      CHECK_RESERVED_BIND(curvar->content, ret, "in each", {
        destroy_env(newenv);
        unrefexp(e);
        return ret;
      });
      if (!(newenv->d))
        newenv->d = create_dict();

      if (issymbol(curvar->content)) {
        if ((retval = EVAL(curval->content, env)) == NULL)
          retval = NIL_EXP;
        if (ispair(retval)) {
          curin = curval->next;
          tmpexp = retval;
          while (retval) {
            set_get_keyval_dict(newenv->d, exp_text(curvar->content),
                                car(retval));
            curval = curin;
            while (curval) {
              ret = EVAL(curval->content, newenv);
              if iserror (ret)
                goto finish;
              unrefexp(ret);
              curval = curval->next;
            }
            retval = retval->next;
          }
          ret = NULL;
          goto finish;
        } else {
          if iserror (retval)
            ret = refexp(retval);
          else
            ret = error(ERROR_ILLEGAL_VALUE, e, env,
                        "Illegal value (not list) in each");
          goto finish;
        }

      } else {
        ret = error(ERROR_ILLEGAL_VALUE, e, env, "Illegal value in each");
        goto finish;
      }
    }
  }

  ret = error(ERROR_MISSING_PARAMETER, e, env, "Missing parameter in each");
finish:
  destroy_env(newenv);
  /* only the head (tmpexp) owns a ref; retval is a borrowed walker */
  if (tmpexp)
    unrefexp(tmpexp);
  else if (retval)
    unrefexp(retval);
  unrefexp(e);
  return ret;
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
const char doc_time[] = "(time expr) — evaluate expr, print elapsed wall time, "
                        "and return the value.";
exp_t *timecmd(exp_t *e, env_t *env) {
  unrefexp(e);
  return make_integeri(gettimeusec());
}
#pragma GCC diagnostic warning "-Wunused-parameter"

#pragma GCC diagnostic ignored "-Wunused-parameter"
/* Map common type tags to their string names for inspect output. */
static const char *inspect_type_name(int t) {
  switch (t) {
  case EXP_SYMBOL:
    return "symbol";
  case EXP_NUMBER:
    return "number";
  case EXP_FLOAT:
    return "float";
  case EXP_STRING:
    return "string";
  case EXP_CHAR:
    return "char";
  case EXP_BOOLEAN:
    return "boolean";
  case EXP_VECTOR:
    return "vector";
  case EXP_ERROR:
    return "error";
  case EXP_PAIR:
    return "pair";
  case EXP_LAMBDA:
    return "lambda";
  case EXP_INTERNAL:
    return "builtin";
  case EXP_MACRO:
    return "macro";
  case EXP_BLOB:
    return "blob";
  case EXP_DICT:
    return "dict";
  case EXP_LIST:
    return "deque";
  case EXP_SET:
    return "set";
  case EXO_MACROINTERNAL:
    return "macro-builtin";
  case EXP_FFI:
    return "ffi";
  case EXP_TREE:
    return "tree";
  case EXP_PAIR_CIRCULAR:
    return "pair-circular";
  default:
    return "?";
  }
}

/* Display the contents of an exp_t — basic type/flag/ref info, plus
   type-specific details (lambda gets arity + params + compile/JIT status,
   string gets the value + length, etc). Caller retains the ref. */
static void inspect_value(exp_t *v) {
  if (!v) {
    printf("\x1B[96m<NULL>\x1B[39m\n");
    return;
  }
  if (!is_ptr(v)) {
    if (isnumber(v))
      printf("\x1B[96m<imm fixnum %lld>\x1B[39m\n", (long long)FIX_VAL(v));
    else if (ischar(v))
      printf("\x1B[96m<imm char %u>\x1B[39m\n", CHAR_VAL(v));
    else
      printf("\x1B[96m<imm 0x%lx>\x1B[39m\n", (long)(intptr_t)v);
    return;
  }
  printf("\x1B[96mtype:\t%d (%s)\nflag:\t%d%s%s\nref:\t%d\x1B[39m\n", v->type,
         inspect_type_name(v->type), v->flags,
         (v->flags & FLAG_COMPILED) ? " COMPILED" : "",
         (v->flags & FLAG_TAIL_AWARE) ? " TAIL_AWARE" : "", v->nref);
  if (v->type == EXP_LAMBDA) {
    if (v->meta)
      printf("\x1B[96mname:\t%s\x1B[39m\n", (char *)v->meta);
    exp_t *params = lambda_params(v);
    int arity = 0;
    exp_t *p;
    for (p = params; p; p = p->next)
      arity++;
    printf("\x1B[96marity:\t%d\nparams:\t(", arity);
    int first = 1;
    for (p = params; p; p = p->next) {
      if (!first)
        printf(" ");
      first = 0;
      if (issymbol(p->content))
        printf("%s", (char *)exp_text(p->content));
    }
    printf(")\x1B[39m\n");
    if (v->flags & FLAG_COMPILED) {
      if (v->bc) {
        printf("\x1B[96mbytecode: %d bytes, %d consts", v->bc->ncode,
               v->bc->nconsts);
#ifdef ALCOVE_JIT
        if (v->bc->jit)
          printf(", jit installed");
        else
          printf(", jit not installed");
#endif
        printf(" (use (disasm fn) to see ops)\x1B[39m\n");
      }
    } else {
      printf("\x1B[96mbody:\truns as AST (compile_lambda failed or not yet "
             "attempted)\x1B[39m\n");
    }
  } else if (v->type == EXP_MACRO && v->meta) {
    printf("\x1B[96mname:\t%s\x1B[39m\n", (char *)v->meta);
  } else if (v->type == EXP_STRING && exp_text(v)) {
    printf("\x1B[96mvalue:\t\"%s\"\nlen:\t%zu\x1B[39m\n", (char *)exp_text(v),
           strlen((char *)exp_text(v)));
  } else if (v->type == EXP_FLOAT) {
    printf("\x1B[96mvalue:\t%g\x1B[39m\n", v->f);
  } else if (v->type == EXP_SYMBOL && exp_text(v)) {
    printf("\x1B[96msym:\t%s\x1B[39m\n", (char *)exp_text(v));
  }
}

/* (inspect val) — evaluates val, prints type info + type-specific details. */
const char doc_inspect[] = "(inspect x) — print internal representation: type, "
                           "refcount, and (for lambdas) compile/JIT status.";
exp_t *inspectcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(arg);
  inspect_value(arg);
  CLEAN_RETURN_1(arg, NULL);
}

/* (dir)              — list user/local bindings, alphabetically.
   (dir "sub")        — apropos-style substring filter (CL/Clojure-style).
   (dir nil t)        — also include builtins from reserved_symbol.
   (dir "sub" t)      — substring filter + builtins.
   Walks env chain inner→outer, dedupes by name (inner wins, matching
   shadowing semantics), then sorts. Prints name + kind + (for lambdas)
   the parameter list. */
typedef struct dir_entry_t {
  const char *name;
  exp_t *val;
} dir_entry_t;

static int dir_entry_cmp(const void *a, const void *b) {
  return strcmp(((const dir_entry_t *)a)->name, ((const dir_entry_t *)b)->name);
}
static int dir_match(const char *name, const char *needle) {
  return (!needle || !*needle) ? 1 : (strstr(name, needle) != NULL);
}
static int dir_seen(dir_entry_t *arr, int n, const char *name) {
  int i;
  for (i = 0; i < n; i++)
    if (strcmp(arr[i].name, name) == 0)
      return 1;
  return 0;
}
static void dir_grow(dir_entry_t **arr, int *n, int *cap, const char *name,
                     exp_t *val) {
  if (dir_seen(*arr, *n, name))
    return;
  if (*n >= *cap) {
    *cap = *cap ? *cap * 2 : 32;
    *arr = realloc(*arr, sizeof(dir_entry_t) * (*cap));
  }
  (*arr)[(*n)++] = (dir_entry_t){name, val};
}
static void dir_collect_dict(dict_t *d, const char *needle, dir_entry_t **arr,
                             int *n, int *cap) {
  if (!d)
    return;
  int h;
  size_t i;
  keyval_t *k;
  for (h = 0; h < 2; h++) {
    if (!d->ht[h].size)
      continue;
    for (i = 0; i < d->ht[h].size; i++) {
      for (k = d->ht[h].table[i]; k; k = k->next) {
        if (!k->key)
          continue;
        if (!dir_match((const char *)k->key, needle))
          continue;
        dir_grow(arr, n, cap, (const char *)k->key, k->val);
      }
    }
  }
}

const char doc_dir[] =
    "(dir) — list every name bound in the current environment chain.";
exp_t *dircmd(exp_t *e, env_t *env) {
  const char *needle = NULL;
  int show_builtins = 0;
  EVAL_ARG_2(needle_arg, flag_arg);
  if (istrue(flag_arg))
    show_builtins = 1;
  /* needle: accept symbol or string; nil / other → no filter */
  if (is_ptr(needle_arg) && (issymbol(needle_arg) || isstring(needle_arg)))
    needle = (const char *)exp_text(needle_arg);

  dir_entry_t *arr = NULL;
  int n = 0, cap = 0;

  /* Walk env chain inner→outer so inner shadows in dir_grow's dedup. */
  env_t *cur;
  for (cur = env; cur; cur = cur->root) {
    int i;
    for (i = 0; i < cur->n_inline; i++) {
      const char *k = cur->inline_keys[i];
      if (!k || !dir_match(k, needle))
        continue;
      dir_grow(&arr, &n, &cap, k, cur->inline_vals[i]);
    }
    dir_collect_dict(cur->d, needle, &arr, &n, &cap);
  }
  if (show_builtins)
    dir_collect_dict(reserved_symbol, needle, &arr, &n, &cap);

  if (n > 1)
    qsort(arr, n, sizeof(dir_entry_t), dir_entry_cmp);

  int i;
  for (i = 0; i < n; i++) {
    exp_t *v = arr[i].val;
    const char *kind;
    if (!is_ptr(v))
      kind = "imm";
    else if (v->type == EXP_LAMBDA)
      kind = "lambda";
    else if (v->type == EXP_MACRO)
      kind = "macro";
    else if (v->type == EXP_INTERNAL)
      kind = "builtin";
    else if (v->type == EXP_SYMBOL)
      kind = "symbol";
    else if (v->type == EXP_NUMBER)
      kind = "fixnum";
    else if (v->type == EXP_FLOAT)
      kind = "float";
    else if (v->type == EXP_STRING)
      kind = "string";
    else if (v->type == EXP_PAIR)
      kind = "pair";
    else
      kind = "?";
    printf("  %-20s  %-8s", arr[i].name, kind);
    if (is_ptr(v) && v->type == EXP_LAMBDA) {
      /* Lambda: print parameter list. */
      exp_t *params = lambda_params(v);
      if (params) {
        printf("  (");
        int first = 1;
        exp_t *p;
        for (p = params; p; p = p->next) {
          if (!first)
            printf(" ");
          first = 0;
          if (issymbol(p->content))
            printf("%s", (char *)exp_text(p->content));
        }
        printf(")");
      }
    } else if (isnumber(v) || ischar(v) || isfloat(v) || isstring(v)) {
      /* Atomic value: show it. */
      printf("  ");
      print_node(v);
    }
    printf("\n");
  }

  free(arr);
  unrefexp(needle_arg);
  unrefexp(flag_arg);
  unrefexp(e);
  return NULL;
}

/* (web?) — t if the interpreter was built with -DALCOVE_WEB (running
   in a browser via Emscripten), nil otherwise. Lets .alc code branch
   on whether features that need a browser (Canvas, Web Audio, fetch)
   are available, or whether native-only features (libffi, the JIT,
   readline) are wired up. */
const char doc_webp[] =
    "(web?) — t if running in the WASM build, nil otherwise.";
exp_t *webpcmd(exp_t *e, env_t *env) {
  (void)e;
  (void)env;
#ifdef ALCOVE_WEB
  return TRUE_EXP;
#else
  return NIL_EXP;
#endif
}

/* Host-identity queries. Values come from compile-time macros, so they are
   constant for the life of the binary (and stay correct under our per-machine
   recompile-on-install model). platform/arch return symbols so a script reads
   (is (platform) 'linux); dylib-suffix returns the shared-library extension as
   a string, ready to splice into an FFI path. WASM reports as 'web. */
const char doc_platform[] =
    "(platform) — host OS as a symbol: web | darwin | linux | freebsd | unknown.";
exp_t *platformcmd(exp_t *e, env_t *env) {
  (void)e;
  (void)env;
#if defined(ALCOVE_WEB)
  return make_symbol("web", 3);
#elif defined(__APPLE__)
  return make_symbol("darwin", 6);
#elif defined(__linux__)
  return make_symbol("linux", 5);
#elif defined(__FreeBSD__)
  return make_symbol("freebsd", 7);
#else
  return make_symbol("unknown", 7);
#endif
}

const char doc_arch[] =
    "(arch) — host CPU as a symbol: arm64 | amd64 | x86 | wasm | unknown.";
exp_t *archcmd(exp_t *e, env_t *env) {
  (void)e;
  (void)env;
#if defined(__wasm__) || defined(__wasm32__) || defined(__EMSCRIPTEN__)
  return make_symbol("wasm", 4);
#elif defined(__aarch64__) || defined(__arm64__)
  return make_symbol("arm64", 5);
#elif defined(__x86_64__)
  return make_symbol("amd64", 5);
#elif defined(__i386__)
  return make_symbol("x86", 3);
#else
  return make_symbol("unknown", 7);
#endif
}

const char doc_dylibsuffix[] =
    "(dylib-suffix) — shared-library file extension for this host: "
    "\".dylib\" on macOS, \".wasm\" on web, \".so\" elsewhere. Splice into an "
    "FFI path: (string-concat \"./libfoo\" (dylib-suffix)).";
exp_t *dylibsuffixcmd(exp_t *e, env_t *env) {
  (void)e;
  (void)env;
#if defined(ALCOVE_WEB)
  return make_string(".wasm", 5);
#elif defined(__APPLE__)
  return make_string(".dylib", 6);
#else
  return make_string(".so", 3);
#endif
}

/* (sleep-ms N) — block the caller for N milliseconds, then return nil.
   On native, calls usleep(); on the WASM build, calls emscripten_sleep()
   which, with -sASYNCIFY=1, suspends the WASM stack so the browser can
   paint and process events. This is the *only* reliable way for a
   synchronous .alc game loop to yield to the browser — JS-side
   setTimeout in an addFunction'd callback can't unwind the alcove
   eval frames behind it. */
/* Monotonic millisecond clock — for frame pacing / elapsed-time measurement,
   unaffected by NTP/DST steps (unlike (time), which is wall-clock µs). The
   epoch is arbitrary; only differences are meaningful. */
const char doc_nowms[] =
    "(now-ms) — fixnum milliseconds from a monotonic clock (arbitrary epoch; "
    "use differences). For wall-clock time use (time) (µs since Unix epoch).";
exp_t *nowmscmd(exp_t *e, env_t *env) {
  (void)e;
  (void)env;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return make_integeri((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

const char doc_sleepms[] = "(sleep-ms N) — sleep N milliseconds. On web, "
                           "yields to the browser via Asyncify.";
exp_t *sleepmscmd(exp_t *e, env_t *env) {
  int ms = alcove_arg_int(e, env, 0);
  if (ms > 0) {
#ifdef ALCOVE_WEB
    emscripten_sleep((unsigned)ms);
#else
    usleep((unsigned)ms * 1000U);
#endif
  }
  return NIL_EXP;
}

/* (disasm fn)  — evaluates fn, expects a compiled lambda, prints its
   bytecode op-by-op plus the JIT install status. Useful for verifying
   what bytecode the compiler produces (no more ad-hoc fprintf in C). */
const char doc_disasm[] = "(disasm fn) — print the bytecode of a compiled "
                          "function (or note that it's not compiled).";
exp_t *disasmcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(arg);
  if (!arg || !islambda(arg)) {
    printf("\x1B[96m(disasm): not a lambda\x1B[39m\n");
  } else if (!(arg->flags & FLAG_COMPILED) || !arg->bc) {
    printf("\x1B[96m(disasm): lambda is not compiled (runs as AST)\x1B[39m\n");
  } else {
    disasm_bytecode(arg->bc);
  }
  CLEAN_RETURN_1(arg, NULL);
}
