/* deque.h — doubly-linked deque (EXP_LIST) ops: push/pop/peek both ends and
 * the deque constructor. FRAGMENT #included into alcove.c (single TU). The
 * EXP_LIST dump/load serializers stay in the persistence cluster. NOT
 * standalone, NOT separately compiled.
 */
/* ---------- deque / EXP_LIST ops ---------- */

/* Helper: append node to tail (no refcounting; caller already owns val ref). */
static void alc_list_push_right(alc_list_t *l, exp_t *val) {
  alc_listnode_t *n = (alc_listnode_t *)memalloc(1, sizeof(alc_listnode_t));
  n->val = val;
  n->prev = l->tail;
  if (l->tail)
    l->tail->next = n;
  else
    l->head = n;
  l->tail = n;
  l->len++;
}

static void alc_list_push_left(alc_list_t *l, exp_t *val) {
  alc_listnode_t *n = (alc_listnode_t *)memalloc(1, sizeof(alc_listnode_t));
  n->val = val;
  n->next = l->head;
  if (l->head)
    l->head->prev = n;
  else
    l->tail = n;
  l->head = n;
  l->len++;
}

const char doc_deque[] = "(deque [x ...]) — fresh doubly-linked deque "
                         "populated with given elements.";
exp_t *dequecmd(exp_t *e, env_t *env) {
  exp_t *ret = make_list_exp();
  alc_list_t *l = (alc_list_t *)ret->ptr;
  exp_t *a = cdr(e);
  while (a) {
    exp_t *v = EVAL(car(a), env);
    if (iserror(v)) {
      unrefexp(ret);
      unrefexp(e);
      return v;
    }
    alc_list_push_right(l, v); /* takes ownership of v */
    a = a->next;
  }
  unrefexp(e);
  return ret;
}

#define DEQUE_PUSH_CMD(name, err_name, push_fn)                                \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *d = EVAL(cadr(e), env);                                             \
    if (iserror(d)) {                                                          \
      unrefexp(e);                                                             \
      return d;                                                                \
    }                                                                          \
    if (!islist(d)) {                                                          \
      unrefexp(d);                                                             \
      unrefexp(e);                                                             \
      return error(ERROR_ILLEGAL_VALUE, NULL, env,                             \
                   err_name ": first arg must be a deque");                    \
    }                                                                          \
    exp_t *v = EVAL(caddr(e), env);                                            \
    if (iserror(v)) {                                                          \
      unrefexp(d);                                                             \
      unrefexp(e);                                                             \
      return v;                                                                \
    }                                                                          \
    push_fn((alc_list_t *)d->ptr, v);                                          \
    unrefexp(e);                                                               \
    return d;                                                                  \
  }

#define DEQUE_POP_CMD(name, err_name, HEAD, TAIL, NEXT, PREV)                  \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *d = EVAL(cadr(e), env);                                             \
    if (iserror(d)) {                                                          \
      unrefexp(e);                                                             \
      return d;                                                                \
    }                                                                          \
    if (!islist(d)) {                                                          \
      unrefexp(d);                                                             \
      unrefexp(e);                                                             \
      return error(ERROR_ILLEGAL_VALUE, NULL, env,                             \
                   err_name ": arg must be a deque");                          \
    }                                                                          \
    alc_list_t *l = (alc_list_t *)d->ptr;                                      \
    exp_t *ret = NIL_EXP;                                                      \
    if (l->HEAD) {                                                             \
      alc_listnode_t *n = l->HEAD;                                             \
      ret = n->val;                                                            \
      l->HEAD = n->NEXT;                                                       \
      if (l->HEAD)                                                             \
        l->HEAD->PREV = NULL;                                                  \
      else                                                                     \
        l->TAIL = NULL;                                                        \
      l->len--;                                                                \
      free(n);                                                                 \
    }                                                                          \
    unrefexp(d);                                                               \
    unrefexp(e);                                                               \
    return ret;                                                                \
  }

#define DEQUE_PEEK_CMD(name, err_name, HEAD)                                   \
  exp_t *name(exp_t *e, env_t *env) {                                          \
    exp_t *d = EVAL(cadr(e), env);                                             \
    if (iserror(d)) {                                                          \
      unrefexp(e);                                                             \
      return d;                                                                \
    }                                                                          \
    if (!islist(d)) {                                                          \
      unrefexp(d);                                                             \
      unrefexp(e);                                                             \
      return error(ERROR_ILLEGAL_VALUE, NULL, env,                             \
                   err_name ": arg must be a deque");                          \
    }                                                                          \
    alc_list_t *l = (alc_list_t *)d->ptr;                                      \
    exp_t *ret = (l->HEAD) ? refexp(l->HEAD->val) : NIL_EXP;                   \
    unrefexp(d);                                                               \
    unrefexp(e);                                                               \
    return ret;                                                                \
  }

const char doc_pushrightbang[] =
    "(push-right! d x) — RPUSH; append x to right end of deque, returns d.";
DEQUE_PUSH_CMD(pushrightbangcmd, "push-right!", alc_list_push_right)

const char doc_pushleftbang[] =
    "(push-left! d x) — LPUSH; prepend x to left end of deque, returns d.";
DEQUE_PUSH_CMD(pushleftbangcmd, "push-left!", alc_list_push_left)

const char doc_poprightbang[] =
    "(pop-right! d) — RPOP; remove and return rightmost element, or nil.";
DEQUE_POP_CMD(poprightbangcmd, "pop-right!", tail, head, prev, next)

const char doc_popleftbang[] =
    "(pop-left! d) — LPOP; remove and return leftmost element, or nil.";
DEQUE_POP_CMD(popleftbangcmd, "pop-left!", head, tail, next, prev)

const char doc_peekleft[] =
    "(peek-left d) — leftmost element (no mutation), or nil.";
DEQUE_PEEK_CMD(peekleftcmd, "peek-left", head)

const char doc_peekright[] =
    "(peek-right d) — rightmost element (no mutation), or nil.";
DEQUE_PEEK_CMD(peekrightcmd, "peek-right", tail)
