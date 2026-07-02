/* watch.h — (watch! obj fn) / (unwatch! obj) / (watched? obj):
 * POST-modification hooks on the mutable containers (Django post_save /
 * Clojure add-watch flavor — observation, not veto; a validator layer can
 * come later if veto is ever wanted).
 *
 * After a structural mutation of a watched dict/deque/vector/set —
 * assoc!/dissoc!, push-*!/pop-*!, vec-set!, set-add!/set-del! — every
 * watcher is called as (fn obj ev), newest first, where ev is a plist:
 *   (:op assoc! :key "score" :old 10 :new 20)
 * :key/:old/:new are nil where the operation has none (a push has no key,
 * a pop has no :new). The mutation has ALREADY happened when the watcher
 * runs; a watcher error propagates as the mutator's return value (the
 * write persists — same contract as Django receivers / Clojure watches).
 * The bulk numeric ops (vec-add!/vec-scale!/... tensor family) do NOT
 * notify: they are the hot path the language exists for.
 *
 * Cost model mirrors weak.h: watched objects carry FLAG_WATCHED, so an
 * unwatched object pays exactly one flag test inside the (non-JIT)
 * mutator builtin — the refcount and JIT hot paths are untouched.
 * Registry: TLS open-addressed obj → owned list of watcher closures.
 * Reentrancy: FLAG_WATCHED is cleared on obj for the duration of its own
 * dispatch, so a watcher mutating its own object does not re-fire
 * (mutating a DIFFERENT watched object fires normally).
 *
 * A watcher closure that captures its own object forms a cycle the
 * refcounter cannot see ends of — hold the object weakly in the closure
 * ((weak obj), weak.h) or (unwatch! obj) explicitly.
 *
 * FRAGMENT #included into alcove.c before vector.h so every mutator
 * fragment (vector.h, builtins_dict.h, set.h, deque.h) sees watch_notify.
 */

typedef struct {
  exp_t *target; /* key; NULL = empty, (exp_t *)-1 = tombstone */
  exp_t *fns;    /* owned Lisp list of watcher closures, newest first */
} watch_slot_t;

static ALCOVE_TLS watch_slot_t *watch_tab = NULL;
static ALCOVE_TLS size_t watch_cap = 0;
static ALCOVE_TLS size_t watch_used = 0;

#define WATCH_TOMBSTONE ((exp_t *)(uintptr_t)-1)

static size_t watch_hash(exp_t *p) {
  return (size_t)(((uintptr_t)p >> 3) * 0x9E3779B97F4A7C15ull);
}

static watch_slot_t *watch_find(exp_t *target) {
  if (!watch_cap)
    return NULL;
  size_t mask = watch_cap - 1, i = watch_hash(target) & mask;
  while (watch_tab[i].target) {
    if (watch_tab[i].target == target)
      return &watch_tab[i];
    i = (i + 1) & mask;
  }
  return NULL;
}

static int watch_grow(void) {
  size_t ncap = watch_cap ? watch_cap * 2 : 32;
  watch_slot_t *nt = (watch_slot_t *)calloc(ncap, sizeof *nt);
  if (!nt)
    return 0;
  size_t mask = ncap - 1, live = 0;
  for (size_t i = 0; i < watch_cap; i++) {
    exp_t *t = watch_tab[i].target;
    if (!t || t == WATCH_TOMBSTONE)
      continue;
    size_t j = watch_hash(t) & mask;
    while (nt[j].target)
      j = (j + 1) & mask;
    nt[j] = watch_tab[i];
    live++;
  }
  free(watch_tab);
  watch_tab = nt;
  watch_cap = ncap;
  watch_used = live;
  return 1;
}

/* Register fn (owned ref transferred on success) as a watcher of target. */
static int watch_insert(exp_t *target, exp_t *fn) {
  if (!watch_cap || (watch_used + 1) * 10 > watch_cap * 7)
    if (!watch_grow())
      return 0;
  size_t mask = watch_cap - 1, i = watch_hash(target) & mask;
  watch_slot_t *slot = NULL, *tomb = NULL;
  while (watch_tab[i].target) {
    if (watch_tab[i].target == target) {
      slot = &watch_tab[i];
      break;
    }
    if (watch_tab[i].target == WATCH_TOMBSTONE && !tomb)
      tomb = &watch_tab[i];
    i = (i + 1) & mask;
  }
  if (!slot) {
    slot = tomb ? tomb : &watch_tab[i];
    if (!tomb)
      watch_used++;
    slot->target = target;
    slot->fns = NULL;
  }
  exp_t *cell = make_node(fn); /* cell owns fn */
  cell->next = slot->fns;
  slot->fns = cell;
  return 1;
}

/* Drop a target's entire watcher list (unwatch! and the free paths). */
static void watch_on_target_free(exp_t *target) {
  watch_slot_t *s = watch_find(target);
  if (!s)
    return;
  if (s->fns)
    unrefexp(s->fns);
  s->target = WATCH_TOMBSTONE;
  s->fns = NULL;
}

/* Build (:op opsym :key k :old o :new n) — every arg borrowed, NULL = nil. */
static exp_t *watch_make_event(const char *op, exp_t *k, exp_t *old,
                               exp_t *nw) {
  exp_t *ev = make_node(make_symbol(":op", 3));
  exp_t *t = ev;
  t->next = make_node(make_symbol((char *)op, strlen(op)));
  t = t->next;
  t->next = make_node(make_symbol(":key", 4));
  t = t->next;
  t->next = make_node(k ? refexp(k) : NIL_EXP);
  t = t->next;
  t->next = make_node(make_symbol(":old", 4));
  t = t->next;
  t->next = make_node(old ? refexp(old) : NIL_EXP);
  t = t->next;
  t->next = make_node(make_symbol(":new", 4));
  t = t->next;
  t->next = make_node(nw ? refexp(nw) : NIL_EXP);
  return ev;
}

/* POST-notify obj's watchers. Call ONLY when (obj->flags & FLAG_WATCHED).
   k/old/nw are borrowed and may be NULL. Returns NULL normally, or the
   first watcher ERROR (ownership transferred to the caller, who should
   return it as the mutator's result — the mutation itself persists). */
static exp_t *watch_notify(exp_t *obj, const char *op, exp_t *k, exp_t *old,
                           exp_t *nw, env_t *env) {
  watch_slot_t *s = watch_find(obj);
  if (!s || !s->fns)
    return NULL;
  exp_t *ev = watch_make_event(op, k, old, nw);
  obj->flags &= (unsigned short)~FLAG_WATCHED; /* reentrancy guard */
  exp_t *err = NULL;
  for (exp_t *c = s->fns; c && c->content; c = c->next) {
    /* Synthesize (fn obj 'ev): the closure and the container self-evaluate,
       the event list must be quoted. The form takes its own refs. */
    exp_t *call = make_node(refexp(c->content));
    call->next = make_node(refexp(obj));
    call->next->next = make_node(make_quote(refexp(ev)));
    exp_t *r = EVAL(call, env);
    unrefexp(call);
    if (r && iserror(r)) {
      err = r;
      break;
    }
    unrefexp(r);
  }
  unrefexp(ev);
  /* Restore the flag unless a watcher unwatched its own object. */
  if (watch_find(obj))
    obj->flags |= FLAG_WATCHED;
  return err;
}

/* ---- builtins ---- */

const char doc_watch[] =
    "(watch! obj fn) — call (fn obj ev) AFTER each structural mutation of "
    "obj (a hash-map, deque, vector, or set): assoc!/dissoc!, push-*!/"
    "pop-*!, vec-set!, set-add!/set-del!. ev is a plist (:op sym :key k "
    ":old o :new n) with nil where an op has no such field. Watchers stack "
    "(newest runs first); a watcher error becomes the mutator's return "
    "value (the write persists). A watcher mutating its own obj does not "
    "re-fire. Bulk numeric vec ops do not notify. Returns obj. Capture obj "
    "weakly in fn ((weak obj)) or the pair leaks — see (weak).";
exp_t *watchcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(obj, fn);
  if (!is_ptr(obj) ||
      !(isdict(obj) || islist(obj) || isvector(obj) || isset(obj)))
    CLEAN_RETURN_2(obj, fn,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "watch!: obj must be a hash-map/deque/vector/set"));
  if (!is_ptr(fn) || !(fn->type == EXP_LAMBDA || fn->type == EXP_INTERNAL))
    CLEAN_RETURN_2(obj, fn, error(ERROR_ILLEGAL_VALUE, e, env,
                                  "watch!: fn must be a function"));
  if (!watch_insert(obj, refexp(fn)))
    CLEAN_RETURN_2(obj, fn,
                   error(ERROR_ILLEGAL_VALUE, e, env, "watch!: out of memory"));
  obj->flags |= FLAG_WATCHED;
  unrefexp(fn);
  unrefexp(e);
  return obj;
}

const char doc_unwatch[] =
    "(unwatch! obj) — remove ALL watchers from obj. Returns obj.";
exp_t *unwatchcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(obj);
  if (is_ptr(obj)) {
    watch_on_target_free(obj);
    obj->flags &= (unsigned short)~FLAG_WATCHED;
  }
  unrefexp(e);
  return obj;
}

const char doc_watchedp[] = "(watched? obj) — t if obj has any watcher.";
exp_t *watchedpcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(obj);
  exp_t *ret =
      (is_ptr(obj) && (obj->flags & FLAG_WATCHED)) ? TRUE_EXP : NIL_EXP;
  CLEAN_RETURN_1(obj, refexp(ret));
}
