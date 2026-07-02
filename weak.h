/* weak.h — weak references: (weak v) / (weak-get w) / (weak? x).
 *
 * A weak cell (EXP_WEAK) points at a target WITHOUT owning a reference —
 * the designed escape hatch for reference cycles the refcounter cannot
 * reclaim and (gc-cycles) deliberately leaves alone (a callback that
 * captures a dict holding that callback: hold the dict weakly and the
 * cycle never forms). When the target's last strong reference drops, every
 * weak cell pointing at it is nulled, and (weak-get) returns nil.
 *
 * Representation. The weak cell uses `ptr` for the borrowed target (NULL
 * once cleared) and `meta` to chain sibling weak cells of the SAME target
 * (also borrowed). `next` stays NULL — unrefexp_free's chain walk
 * decrements ->next for every type, so a borrowed pointer there would be
 * over-released. Targets carry FLAG_WEAK_REFERENT so the free paths pay a
 * registry lookup ONLY for objects that were ever weakly referenced; the
 * hot inline unrefexp path is untouched.
 *
 * Registry: a TLS open-addressed pointer map target → chain head. TLS for
 * the same reason as the arena itself (per-thread values, no contention);
 * create weak cells on the thread that owns the target.
 *
 * Immediates and the immortal singletons are never freed, so (weak 5) /
 * (weak nil) return an unregistered cell that simply never clears.
 *
 * Hooks (both out-of-line, cold):
 *   - unrefexp_free / gc_free_dead_cell call weak_on_target_free(e) when
 *     the flag is set: null every cell in the chain, drop the entry.
 *   - unrefexp_free's EXP_WEAK case calls weak_on_cell_free(w): unlink the
 *     dying cell from its target's chain (clearing the flag on the last).
 */

/* ---- the target → weak-chain registry (open addressing, tombstones) ---- */

typedef struct {
  exp_t *target; /* key; NULL = empty, (exp_t *)-1 = tombstone */
  exp_t *head;   /* first weak cell in the chain (linked via ->meta) */
} weak_slot_t;

static ALCOVE_TLS weak_slot_t *weak_tab = NULL;
static ALCOVE_TLS size_t weak_cap = 0;  /* power of two */
static ALCOVE_TLS size_t weak_used = 0; /* live + tombstones */

#define WEAK_TOMBSTONE ((exp_t *)(uintptr_t)-1)

static size_t weak_hash(exp_t *p) {
  /* Fibonacci hash on the pointer bits (low 3 are always zero). */
  return (size_t)(((uintptr_t)p >> 3) * 0x9E3779B97F4A7C15ull);
}

static weak_slot_t *weak_find(exp_t *target) {
  if (!weak_cap)
    return NULL;
  size_t mask = weak_cap - 1, i = weak_hash(target) & mask;
  while (weak_tab[i].target) {
    if (weak_tab[i].target == target)
      return &weak_tab[i];
    i = (i + 1) & mask;
  }
  return NULL;
}

static int weak_grow(void) {
  size_t ncap = weak_cap ? weak_cap * 2 : 64;
  weak_slot_t *nt = (weak_slot_t *)calloc(ncap, sizeof *nt);
  if (!nt)
    return 0;
  size_t mask = ncap - 1, live = 0;
  for (size_t i = 0; i < weak_cap; i++) {
    exp_t *t = weak_tab[i].target;
    if (!t || t == WEAK_TOMBSTONE)
      continue; /* tombstones die in the rehash */
    size_t j = weak_hash(t) & mask;
    while (nt[j].target)
      j = (j + 1) & mask;
    nt[j] = weak_tab[i];
    live++;
  }
  free(weak_tab);
  weak_tab = nt;
  weak_cap = ncap;
  weak_used = live;
  return 1;
}

/* Register cell as a weak referent of target, creating the slot if needed
   (cell->meta chains to the previous head). Returns 0 on OOM. */
static int weak_insert(exp_t *target, exp_t *cell) {
  if (!weak_cap || (weak_used + 1) * 10 > weak_cap * 7)
    if (!weak_grow())
      return 0;
  size_t mask = weak_cap - 1, i = weak_hash(target) & mask;
  weak_slot_t *slot = NULL, *tomb = NULL;
  while (weak_tab[i].target) {
    if (weak_tab[i].target == target) {
      slot = &weak_tab[i];
      break;
    }
    if (weak_tab[i].target == WEAK_TOMBSTONE && !tomb)
      tomb = &weak_tab[i];
    i = (i + 1) & mask;
  }
  if (!slot) {
    slot = tomb ? tomb : &weak_tab[i];
    if (!tomb)
      weak_used++;
    slot->target = target;
    slot->head = NULL;
  }
  cell->meta = (keyval_t *)slot->head; /* borrowed sibling link */
  slot->head = cell;
  return 1;
}

/* The target is being freed: null every weak cell pointing at it. Called
   from unrefexp_free and from the gc-cycles sweep (gc_free_dead_cell),
   gated on FLAG_WEAK_REFERENT. */
static void weak_on_target_free(exp_t *target) {
  weak_slot_t *s = weak_find(target);
  if (!s)
    return;
  exp_t *c = s->head;
  while (c) {
    exp_t *sib = (exp_t *)c->meta;
    c->ptr = NULL;
    c->meta = NULL;
    c = sib;
  }
  s->target = WEAK_TOMBSTONE;
  s->head = NULL;
}

/* A weak cell is being freed: unlink it from its target's chain (the cell
   may already be cleared — then there is nothing to do). */
static void weak_on_cell_free(exp_t *cell) {
  if (!cell->ptr)
    return;
  weak_slot_t *s = weak_find((exp_t *)cell->ptr);
  if (!s)
    return;
  exp_t **link = &s->head;
  while (*link && *link != cell)
    link = (exp_t **)&(*link)->meta;
  if (*link)
    *link = (exp_t *)cell->meta;
  if (!s->head) { /* last weak ref gone — untag the target, drop the slot */
    ((exp_t *)cell->ptr)->flags &= (unsigned short)~FLAG_WEAK_REFERENT;
    s->target = WEAK_TOMBSTONE;
  }
}

/* ---- builtins ---- */

const char doc_weak[] =
    "(weak v) — a weak reference to v: it does NOT keep v alive. Read it "
    "with (weak-get w), which returns v while v has strong references and "
    "nil after v is freed. The designed escape hatch for reference cycles "
    "(hold the back-pointer weakly and the cycle never forms — see also "
    "gc-cycles). Immediates and nil/t never free, so their weak refs never "
    "clear. Same-thread only, like the rest of the value heap.";
exp_t *weakcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  MAKE_TYPED(w, EXP_WEAK, v);
  if (is_ptr(v) && !is_immortal(v)) {
    if (!weak_insert(v, w)) {
      w->ptr = NULL;
      unrefexp(w);
      CLEAN_RETURN_1(v, error(ERROR_ILLEGAL_VALUE, e, env,
                              "weak: out of memory"));
    }
    v->flags |= FLAG_WEAK_REFERENT;
  }
  /* Drop the strong ref EVAL gave us — the cell holds v only weakly. If v
     was a temporary this frees it right here and the hook has already
     nulled w, so (weak-get) correctly answers nil. */
  CLEAN_RETURN_1(v, w);
}

const char doc_weak_get[] =
    "(weak-get w) — the value a weak cell points at, or nil if the target "
    "has been freed (or w holds nil).";
exp_t *weakgetcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(w);
  if (!is_ptr(w) || w->type != EXP_WEAK)
    CLEAN_RETURN_1(w, error(ERROR_ILLEGAL_VALUE, e, env,
                            "weak-get: argument must be a weak reference"));
  exp_t *ret = w->ptr ? refexp((exp_t *)w->ptr) : refexp(NIL_EXP);
  CLEAN_RETURN_1(w, ret);
}

const char doc_weakp[] = "(weak? x) — t if x is a weak reference cell.";
exp_t *weakpcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(x);
  exp_t *ret = (is_ptr(x) && x->type == EXP_WEAK) ? TRUE_EXP : NIL_EXP;
  CLEAN_RETURN_1(x, refexp(ret));
}
