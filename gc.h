/* gc.h — (gc-cycles): an ON-DEMAND trial-deletion cycle collector.
 *
 * Alcove's refcounting cannot reclaim reference cycles (a settled non-goal:
 * no tracing GC, no hot-path tax). Cycles can only be closed through the
 * MUTATING containers — (assoc! d k d), (push-right! q q), (vec-set! v i v)
 * — since pairs are immutable after construction and HAMTs are persistent.
 * This collector reclaims exactly those cycles, only when explicitly asked,
 * with ZERO cost on the allocation/refcount hot paths.
 *
 * Algorithm (Bacon–Rajan trial deletion, whole-arena, stop-the-world):
 *   0. Enumerate every cell via the chunk registry (exp_chunk_bases in
 *      alcove.c). live := nref > 0 (freelist + bump-tail cells are 0).
 *   1. COUNT: for each live WALKABLE cell, visit its owned edges and count
 *      internal references per target cell (a side array — no exp_t change).
 *   2. ROOTS + MARK: a live cell is a root if it is UNWALKABLE (lambda, env-
 *      held, FFI, generator, ...), an immortal singleton, or its nref exceeds
 *      its internal count (someone outside the arena graph — C stack, VM
 *      stack, env binding, keyspace, another thread — still holds it). Mark
 *      everything reachable from roots through walkable edges.
 *   3. SWEEP: live ∧ walkable ∧ unmarked = cyclic garbage. Release each dead
 *      cell's edges to NON-dead targets (normal unrefexp — those are real
 *      counted refs being dropped on behalf of the dying owner), then free
 *      each dead cell's own payload WITHOUT recursing and push it to the
 *      freelist. Dead→dead edges vanish with their owners, matched by never
 *      decrementing them.
 *
 * SAFETY MODEL — why partial coverage is sound: an edge the collector does
 * not know about (any unwalked type, C stack, bytecode constants, envs, the
 * lock-free keyspace, other threads) is simply never counted, which makes its
 * target look externally referenced, which makes it a root: unknown = kept.
 * The one obligation is the mirror image: every edge the COUNT phase does
 * count must be a genuinely owned (refcounted) reference, must be traversed
 * identically by MARK, and must be either released or dissolved by SWEEP.
 * gc_visit_edges is that single shared edge enumerator; it mirrors the
 * ownership model of unrefexp_free's type switch (alcove.c) exactly.
 *
 * v1 walks: EXP_PAIR / EXP_TREE / EXP_PAIR_CIRCULAR (content + next),
 * EXP_DICT / EXP_SET (keyval vals), EXP_LIST (deque node vals), and
 * EXP_VECTOR of kind GEN (owning element slots). Closures are NOT walked, so
 * a cycle threaded through a captured env (callback↔dict) is not collected —
 * the lambda↔env self-cycle is already broken by env_break_self_cycle
 * (env.h); wider env cycles remain future work.
 *
 * THREADING: collects the CALLING thread's arena only. Cross-arena and
 * keyspace references are external (kept) by the safety model above, but the
 * walk itself takes no locks — run it single-threaded, or with reactors
 * quiesced; do not call it from a --threads RESP callback.
 */

/* ---- cell indexing over the chunk registry ---- */

static int gc_base_cmp(const void *a, const void *b) {
  exp_t *x = *(exp_t *const *)a, *y = *(exp_t *const *)b;
  return (x > y) - (x < y);
}

/* Map a pointer to its dense cell index (chunk*256+slot over the SORTED
   base array), or -1 if it is not a cell in this thread's arena. */
static int64_t gc_cell_index(exp_t **sorted, int64_t nchunks, exp_t *p) {
  if (!is_ptr(p))
    return -1;
  int64_t lo = 0, hi = nchunks - 1, hit = -1;
  while (lo <= hi) {
    int64_t mid = lo + (hi - lo) / 2;
    if (sorted[mid] <= p) {
      hit = mid;
      lo = mid + 1;
    } else
      hi = mid - 1;
  }
  if (hit < 0)
    return -1;
  size_t off = (size_t)((char *)p - (char *)sorted[hit]);
  if (off >= EXP_BUMP_CHUNK * sizeof(exp_t) || off % sizeof(exp_t))
    return -1;
  return hit * EXP_BUMP_CHUNK + (int64_t)(off / sizeof(exp_t));
}

/* ---- the shared edge enumerator (see the safety model above) ---- */

static int gc_walkable(exp_t *e) {
  switch (e->type) {
  case EXP_PAIR:
  case EXP_TREE:
  case EXP_PAIR_CIRCULAR:
  case EXP_DICT:
  case EXP_SET:
  case EXP_LIST:
    return 1;
  case EXP_VECTOR:
    return vec_kind(e) == VEC_KIND_GEN;
  default:
    return 0;
  }
}

typedef void (*gc_edge_fn)(exp_t *child, void *ud);

static void gc_visit_edges(exp_t *e, gc_edge_fn fn, void *ud) {
  switch (e->type) {
  case EXP_PAIR:
  case EXP_TREE:
  case EXP_PAIR_CIRCULAR:
    /* Both are owned: unrefexp_free recurses on content and chain-
       decrements next. meta is NOT an edge (symbol cache / wrapper env). */
    if (e->content)
      fn(e->content, ud);
    if (e->next)
      fn(e->next, ud);
    break;
  case EXP_DICT:
  case EXP_SET: {
    dict_t *d = (dict_t *)e->ptr;
    if (!d)
      break;
    for (int i = 0; i < 2; i++) {
      if (!d->ht[i].table)
        continue;
      for (unsigned long j = 0; j < d->ht[i].size; j++)
        for (keyval_t *kv = d->ht[i].table[j]; kv; kv = kv->next)
          if (kv->val)
            fn(kv->val, ud); /* keys are char*, not edges */
    }
    break;
  }
  case EXP_LIST: {
    alc_list_t *l = (alc_list_t *)e->ptr;
    if (!l)
      break;
    for (alc_listnode_t *n = l->head; n; n = n->next)
      if (n->val)
        fn(n->val, ud);
    break;
  }
  case EXP_VECTOR: {
    if (!e->ptr || vec_kind(e) != VEC_KIND_GEN)
      break;
    int64_t n = vec_len(e);
    for (int64_t i = 0; i < n; i++)
      if (vec_gen_at(e, i))
        fn(vec_gen_at(e, i), ud);
    break;
  }
  default:
    break;
  }
}

/* ---- collector state + phase callbacks ---- */

#define GC_LIVE 1u
#define GC_WALK 2u
#define GC_MARK 4u
#define GC_DEAD 8u

typedef struct {
  exp_t **sorted;    /* sorted chunk bases */
  int64_t nchunks;   /* chunks (and sorted length) */
  int64_t ncells;    /* nchunks * EXP_BUMP_CHUNK */
  uint32_t *interns; /* per-cell internal (arena-owned) reference count */
  uint8_t *state;    /* per-cell GC_* flag bits */
  exp_t **stack;     /* mark stack; each cell pushed at most once */
  int64_t sp;
} gc_ctx_t;

static void gc_count_edge(exp_t *child, void *ud) {
  gc_ctx_t *g = (gc_ctx_t *)ud;
  int64_t ci = gc_cell_index(g->sorted, g->nchunks, child);
  if (ci >= 0)
    g->interns[ci]++;
}

static void gc_mark_edge(exp_t *child, void *ud) {
  gc_ctx_t *g = (gc_ctx_t *)ud;
  int64_t ci = gc_cell_index(g->sorted, g->nchunks, child);
  if (ci < 0 || (g->state[ci] & GC_MARK) || !(g->state[ci] & GC_LIVE))
    return;
  g->state[ci] |= GC_MARK;
  if (g->state[ci] & GC_WALK)
    g->stack[g->sp++] = child; /* only walkable cells have out-edges */
}

static void gc_release_edge(exp_t *child, void *ud) {
  gc_ctx_t *g = (gc_ctx_t *)ud;
  int64_t ci = gc_cell_index(g->sorted, g->nchunks, child);
  if (ci >= 0 && (g->state[ci] & GC_DEAD))
    return;         /* dies with us — its ref dissolves, never decremented */
  unrefexp(child);  /* a real counted ref, dropped on the dying owner's
                       behalf; may legitimately cascade-free live subtrees
                       whose only holders were dead cells */
}

/* Free a dead cell's OWN payload without recursing into children (edges were
   handled by gc_release_edge), then push the cell to the freelist. Mirrors
   unrefexp_free's payload logic for the walkable types only — dead ⊆
   walkable by construction. */
static void gc_free_dead_cell(exp_t *e) {
  switch (e->type) {
  case EXP_DICT:
  case EXP_SET: {
    dict_t *d = (dict_t *)e->ptr;
    if (d) {
      for (int i = 0; i < 2; i++) {
        if (d->ht[i].table) {
          for (unsigned long j = 0; j < d->ht[i].size; j++) {
            keyval_t *kv = d->ht[i].table[j];
            while (kv) {
              keyval_t *nx = kv->next;
              free(kv->key);
              free(kv);
              kv = nx;
            }
          }
          free(d->ht[i].table);
        }
      }
      free(d);
    }
    break;
  }
  case EXP_LIST: {
    alc_list_t *l = (alc_list_t *)e->ptr;
    if (l) {
      alc_listnode_t *n = l->head;
      while (n) {
        alc_listnode_t *nx = n->next;
        free(n);
        n = nx;
      }
      free(l);
    }
    break;
  }
  case EXP_VECTOR:
    free(e->ptr);
    break;
  default: /* PAIR / TREE / PAIR_CIRCULAR: no owned malloc payload */
    break;
  }
  e->nref = 0;
  e->next = exp_freelist;
  exp_freelist = e;
}

const char doc_gc_cycles[] =
    "(gc-cycles) — reclaim reference CYCLES the refcounter cannot (a dict "
    "that contains itself, two deques holding each other, ...). On-demand "
    "and stop-the-world for the calling thread's heap: zero cost unless "
    "called. Returns the number of cells reclaimed. Cycles through closure "
    "captures are not collected (kept alive, never corrupted). Not "
    "concurrency-safe: call it single-threaded or with reactors quiesced, "
    "never from a --threads RESP callback. Audit with (heap-stats).";
exp_t *gccyclescmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  gc_ctx_t g = {0};
  g.nchunks = g_exp_chunks;
  g.ncells = g.nchunks * EXP_BUMP_CHUNK;
  if (g.ncells == 0)
    return make_integeri(0);
  g.sorted = (exp_t **)malloc((size_t)g.nchunks * sizeof *g.sorted);
  g.interns = (uint32_t *)calloc((size_t)g.ncells, sizeof *g.interns);
  g.state = (uint8_t *)calloc((size_t)g.ncells, sizeof *g.state);
  g.stack = (exp_t **)malloc((size_t)g.ncells * sizeof *g.stack);
  if (!g.sorted || !g.interns || !g.state || !g.stack) {
    free(g.sorted);
    free(g.interns);
    free(g.state);
    free(g.stack);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "gc-cycles: out of memory");
  }
  memcpy(g.sorted, exp_chunk_bases, (size_t)g.nchunks * sizeof *g.sorted);
  qsort(g.sorted, (size_t)g.nchunks, sizeof *g.sorted, gc_base_cmp);

  /* Phase 0+1: classify cells; count internal references. */
  for (int64_t c = 0; c < g.nchunks; c++) {
    for (int i = 0; i < EXP_BUMP_CHUNK; i++) {
      exp_t *p = g.sorted[c] + i;
      if (p->nref <= 0)
        continue; /* freelist tenant or never-allocated bump tail */
      int64_t ci = c * EXP_BUMP_CHUNK + i;
      g.state[ci] |= GC_LIVE;
      if (p == nil_singleton || p == true_singleton ||
          p == gen_done_singleton)
        continue; /* immortal: pre-marked as a root below, never walked */
      if (gc_walkable(p)) {
        g.state[ci] |= GC_WALK;
        gc_visit_edges(p, gc_count_edge, &g);
      }
    }
  }

  /* Phase 2: seed roots, then mark. */
  for (int64_t c = 0; c < g.nchunks; c++) {
    for (int i = 0; i < EXP_BUMP_CHUNK; i++) {
      int64_t ci = c * EXP_BUMP_CHUNK + i;
      if (!(g.state[ci] & GC_LIVE) || (g.state[ci] & GC_MARK))
        continue;
      exp_t *p = g.sorted[c] + i;
      int immortal = (p == nil_singleton || p == true_singleton ||
                      p == gen_done_singleton);
      if (immortal || !(g.state[ci] & GC_WALK) ||
          (uint32_t)p->nref > g.interns[ci]) {
        g.state[ci] |= GC_MARK;
        if (g.state[ci] & GC_WALK)
          g.stack[g.sp++] = p;
      }
    }
  }
  while (g.sp > 0)
    gc_visit_edges(g.stack[--g.sp], gc_mark_edge, &g);

  /* Phase 3: sweep — flag the dead set first (gc_release_edge consults it),
     then release outbound edges, then free payloads. */
  int64_t freed = 0;
  for (int64_t ci = 0; ci < g.ncells; ci++)
    if ((g.state[ci] & (GC_LIVE | GC_WALK | GC_MARK)) == (GC_LIVE | GC_WALK))
      g.state[ci] |= GC_DEAD;
  for (int64_t c = 0; c < g.nchunks; c++)
    for (int i = 0; i < EXP_BUMP_CHUNK; i++)
      if (g.state[c * EXP_BUMP_CHUNK + i] & GC_DEAD)
        gc_visit_edges(g.sorted[c] + i, gc_release_edge, &g);
  for (int64_t c = 0; c < g.nchunks; c++) {
    for (int i = 0; i < EXP_BUMP_CHUNK; i++) {
      if (g.state[c * EXP_BUMP_CHUNK + i] & GC_DEAD) {
        gc_free_dead_cell(g.sorted[c] + i);
        freed++;
      }
    }
  }

  free(g.sorted);
  free(g.interns);
  free(g.state);
  free(g.stack);
  return make_integeri(freed);
}
