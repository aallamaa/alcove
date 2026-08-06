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
  case EXP_LAMBDA:
  case EXP_MACRO:
    return 1;
  default:
    return 0;
  }
}

/* The env a lambda/macro captured, or NULL. Lives on the WRAPPER node
   (e->next->meta), set by fncmd/defcmd/defmacrocmd and released by
   unrefexp_free's destroy_env. */
static env_t *gc_captured_env(exp_t *e) {
  if (e->type != EXP_LAMBDA && e->type != EXP_MACRO)
    return NULL;
  return (e->next && e->next->meta) ? (env_t *)e->next->meta : NULL;
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
  case EXP_LAMBDA:
  case EXP_MACRO:
    /* Owned exp_t children, mirroring unrefexp_free + bytecode_free. A
       COMPILED lambda unions e->content with e->bc, so its params list and
       its whole CONSTANT POOL are owned through bc instead — miss those and
       the pool's cells look unreferenced. An AST lambda owns content
       directly. Both own the wrapper/body node.
       The captured env is NOT an exp_t: it is a node in the env domain
       (gc_visit_env_edges). Reporting its bindings here instead would
       over-count them against the lambda, and an over-count frees live
       objects. */
    if (e->flags & FLAG_COMPILED) {
      bytecode_t *bc = e->bc;
      if (bc) {
        if (bc->content)
          fn(bc->content, ud);
        for (int ci = 0; ci < bc->nconsts; ci++)
          if (bc->consts[ci])
            fn(bc->consts[ci], ud);
      }
    } else if (e->content) {
      fn(e->content, ud);
    }
    if (e->next)
      fn(e->next, ud);
    break;
  default:
    break;
  }
}

/* Owned exp_t children of an env, mirroring destroy_env exactly: the inline
   slots, the overflow dict's values, and callingfnc. The parent (root) is an
   ENV ref, walked separately. */
static void gc_visit_env_edges(env_t *v, gc_edge_fn fn, void *ud) {
  for (int i = 0; i < v->n_inline; i++)
    if (v->inline_vals[i])
      fn(v->inline_vals[i], ud);
  if (v->d) {
    DICT_FOREACH(v->d, kv, 0, 1)
    if (kv->val)
      fn((exp_t *)kv->val, ud);
  }
  if (v->callingfnc)
    fn(v->callingfnc, ud);
}

/* ---- collector state + phase callbacks ---- */

#define GC_LIVE 1u
#define GC_WALK 2u
#define GC_MARK 4u
#define GC_DEAD 8u

/* ---- env nodes -----------------------------------------------------------
   A closure cycle runs through TWO refcount domains: container -> lambda ->
   env_t -> (env binding) -> container. env_t is not an exp_t and does not
   live in the exp_t arena, so the cell sweep alone can never see the second
   half, which is why closures were conservatively kept.

   Envs are therefore modelled as collector nodes in their own right — but
   only the ones DISCOVERED through a lambda's capture. That is deliberate:
   we cannot enumerate live envs (destroy_env abandons non-top arena slots
   without a liveness bit, so a walk of the arena would read dead slots), and
   an env we never discover is simply never a candidate, i.e. treated as a
   root. Conservative in the safe direction. */
typedef struct {
  env_t *env;
  uint32_t interns; /* refs from within the candidate set */
  uint8_t state;    /* GC_* bits, same meaning as for cells */
} gc_envnode_t;

typedef struct {
  exp_t **sorted;    /* sorted chunk bases */
  int64_t nchunks;   /* chunks (and sorted length) */
  int64_t ncells;    /* nchunks * EXP_BUMP_CHUNK */
  uint32_t *interns; /* per-cell internal (arena-owned) reference count */
  uint8_t *state;    /* per-cell GC_* flag bits */
  exp_t **stack;     /* mark stack; each cell pushed at most once */
  int64_t sp;
  gc_envnode_t *envs; /* discovered env nodes */
  int64_t nenv, envcap;
  env_t **estack; /* mark stack for env nodes */
  int64_t esp;
  int failed; /* an allocation failed — abandon the collection, free nothing */
} gc_ctx_t;

static int64_t gc_env_index(gc_ctx_t *g, env_t *e) {
  for (int64_t i = 0; i < g->nenv; i++)
    if (g->envs[i].env == e)
      return i;
  return -1;
}

/* Register an env as a candidate node. Returns its index, or -1 if we could
   not (allocation failure) — the caller then abandons the whole collection
   rather than proceeding with an incomplete edge picture. */
static int64_t gc_env_add(gc_ctx_t *g, env_t *e) {
  int64_t at = gc_env_index(g, e);
  if (at >= 0)
    return at;
  if (g->nenv == g->envcap) {
    int64_t cap = g->envcap ? g->envcap * 2 : 32;
    gc_envnode_t *grown =
        (gc_envnode_t *)realloc(g->envs, (size_t)cap * sizeof *grown);
    if (!grown) {
      g->failed = 1;
      return -1;
    }
    g->envs = grown;
    g->envcap = cap;
  }
  g->envs[g->nenv].env = e;
  g->envs[g->nenv].interns = 0;
  g->envs[g->nenv].state = GC_LIVE;
  return g->nenv++;
}

static void gc_count_edge(exp_t *child, void *ud) {
  gc_ctx_t *g = (gc_ctx_t *)ud;
  int64_t ci = gc_cell_index(g->sorted, g->nchunks, child);
  if (ci >= 0)
    g->interns[ci]++;
}

/* Mark an ENV node and queue it. Kept separate from gc_mark_edge because the
   two domains have separate tables; an env is never an exp_t cell. */
static void gc_mark_env(gc_ctx_t *g, env_t *v) {
  int64_t ei = gc_env_index(g, v);
  if (ei < 0 || (g->envs[ei].state & GC_MARK))
    return;
  g->envs[ei].state |= GC_MARK;
  if (g->envs[ei].state & GC_WALK)
    g->estack[g->esp++] = v;
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
    return;        /* dies with us — its ref dissolves, never decremented */
  unrefexp(child); /* a real counted ref, dropped on the dying owner's
                      behalf; may legitimately cascade-free live subtrees
                      whose only holders were dead cells */
}

/* Free a dead env's OWN structure. Children and parent were released above,
   so this only reclaims the dict and the arena slot — the tail of
   destroy_env after its unref loop, with the refcount dance skipped because
   the env is provably unreachable. */
static void gc_free_dead_env(env_t *v) {
  if (v->d) {
    /* destroy_dict would unref the values; they were handled by
       gc_release_edge, so free the table structure only. */
    for (int i = 0; i < 2; i++) {
      if (!v->d->ht[i].table)
        continue;
      for (unsigned long j = 0; j < v->d->ht[i].size; j++) {
        keyval_t *kv = v->d->ht[i].table[j];
        while (kv) {
          keyval_t *nx = kv->next;
          free(kv->key);
          free(kv);
          kv = nx;
        }
      }
      free(v->d->ht[i].table);
    }
    free(v->d);
    v->d = NULL;
  }
  v->n_inline = 0;
  v->has_closure = 0;
  v->callingfnc = NULL;
  v->root = NULL;
  v->nref = 0;
  shard_t *sh = current_shard;
  if (v >= sh->arena && v < sh->arena_end) {
    if (v + 1 == sh->arena_sp)
      sh->arena_sp = v; /* LIFO top: hand the slot back */
  } else {
    free(v);
  }
}

/* Free a dead cell's OWN payload without recursing into children (edges were
   handled by gc_release_edge), then push the cell to the freelist. Mirrors
   unrefexp_free's payload logic for the walkable types only — dead ⊆
   walkable by construction. */
static void gc_free_dead_cell(exp_t *e) {
  if (e->flags & FLAG_WEAK_REFERENT)
    weak_on_target_free(e); /* same contract as unrefexp_free's hook */
  if (e->flags & FLAG_WATCHED)
    watch_on_target_free(e);
  switch (e->type) {
  case EXP_LAMBDA:
  case EXP_MACRO:
    /* meta is the self-name header (owned, plain malloc). The captured env
       was released in the sweep and content/next were edges, so all that is
       left is the bytecode STRUCTURE — freed here without bytecode_free's
       unrefs, which would double-release the pool cells gc_release_edge
       already handled. */
    if ((e->flags & FLAG_COMPILED) && e->bc) {
      bytecode_t *bc = e->bc;
      free(bc->consts);
      free(bc->gcache);
      free(bc->code);
      free(bc->locs);
#ifdef ALCOVE_JIT
      if (bc->jit_mem)
        munmap(bc->jit_mem, bc->jit_size);
#endif
      free(bc);
      e->bc = NULL;
    }
    free(e->meta);
    e->meta = NULL;
    break;
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
  g.envcap = 32;
  g.envs = (gc_envnode_t *)malloc((size_t)g.envcap * sizeof *g.envs);
  if (!g.sorted || !g.interns || !g.state || !g.stack || !g.envs) {
    free(g.sorted);
    free(g.interns);
    free(g.state);
    free(g.stack);
    free(g.envs);
    free(g.estack);
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
      ifmatch (p, nil_singleton, true_singleton, gen_done_singleton)
        continue; /* immortal: pre-marked as a root below, never walked */
      if (gc_walkable(p)) {
        g.state[ci] |= GC_WALK;
        gc_visit_edges(p, gc_count_edge, &g);
        /* Discover the captured env (and its parent chain). Registering is
           what makes the second half of a closure cycle visible at all. */
        for (env_t *v = gc_captured_env(p); v; v = v->root) {
          int64_t ei = gc_env_add(&g, v);
          if (ei < 0)
            goto gc_abandon;
          g.envs[ei].interns++; /* the lambda's / child env's ref to v */
          if (g.envs[ei].state & GC_WALK)
            break; /* chain already counted from a previous lambda */
          g.envs[ei].state |= GC_WALK;
          gc_visit_env_edges(v, gc_count_edge, &g);
        }
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
  /* Sized only now: the env set is closed after phase 1, and gc_mark_env
     pushes each env at most once. Sizing it off g.ncells earlier would have
     been a guess — a deep parent chain can put more envs in the table than
     there are cells. */
  g.estack = (env_t **)malloc((size_t)(g.nenv ? g.nenv : 1) * sizeof *g.estack);
  if (!g.estack)
    goto gc_abandon;

  /* Env roots: an env referenced from OUTSIDE the candidate set (a live call
     frame, a closure we declined to walk) has nref above the refs we counted,
     so it and everything it binds stay. An undiscovered env was never a
     candidate and is a root by construction. */
  for (int64_t ei = 0; ei < g.nenv; ei++)
    if (!(g.envs[ei].state & GC_MARK) &&
        (uint32_t)g.envs[ei].env->nref > g.envs[ei].interns)
      gc_mark_env(&g, g.envs[ei].env);

  /* Marking alternates between the two domains until both stacks drain: a
     cell can reach an env (a lambda's capture) and an env can reach cells
     (its bindings). */
  while (g.sp > 0 || g.esp > 0) {
    while (g.sp > 0) {
      exp_t *p = g.stack[--g.sp];
      gc_visit_edges(p, gc_mark_edge, &g);
      env_t *cap = gc_captured_env(p);
      if (cap)
        gc_mark_env(&g, cap);
    }
    while (g.esp > 0) {
      env_t *v = g.estack[--g.esp];
      gc_visit_env_edges(v, gc_mark_edge, &g);
      if (v->root)
        gc_mark_env(&g, v->root);
    }
  }

  /* Phase 3: sweep — flag the dead set first (gc_release_edge consults it),
     then release outbound edges, then free payloads. */
  int64_t freed = 0;
  for (int64_t ci = 0; ci < g.ncells; ci++)
    if ((g.state[ci] & (GC_LIVE | GC_WALK | GC_MARK)) == (GC_LIVE | GC_WALK))
      g.state[ci] |= GC_DEAD;
  for (int64_t ei = 0; ei < g.nenv; ei++)
    if ((g.envs[ei].state & (GC_LIVE | GC_WALK | GC_MARK)) ==
        (GC_LIVE | GC_WALK))
      g.envs[ei].state |= GC_DEAD;
  for (int64_t c = 0; c < g.nchunks; c++)
    for (int i = 0; i < EXP_BUMP_CHUNK; i++)
      if (g.state[c * EXP_BUMP_CHUNK + i] & GC_DEAD)
        gc_visit_edges(g.sorted[c] + i, gc_release_edge, &g);
  /* A dead lambda's ref to a SURVIVING env is a real ref that must be
     dropped; to a dead env it dissolves with the cycle. Same rule as
     gc_release_edge, one domain over. */
  for (int64_t c = 0; c < g.nchunks; c++)
    for (int i = 0; i < EXP_BUMP_CHUNK; i++) {
      if (!(g.state[c * EXP_BUMP_CHUNK + i] & GC_DEAD))
        continue;
      exp_t *p = g.sorted[c] + i;
      env_t *cap = gc_captured_env(p);
      if (!cap)
        continue;
      int64_t ei = gc_env_index(&g, cap);
      if (ei < 0 || !(g.envs[ei].state & GC_DEAD))
        destroy_env(cap);
      p->next->meta = NULL; /* never leave a dangling capture behind */
    }
  for (int64_t ei = 0; ei < g.nenv; ei++)
    if (g.envs[ei].state & GC_DEAD)
      gc_visit_env_edges(g.envs[ei].env, gc_release_edge, &g);
  for (int64_t ei = 0; ei < g.nenv; ei++) {
    if (!(g.envs[ei].state & GC_DEAD))
      continue;
    env_t *v = g.envs[ei].env;
    int64_t pi = v->root ? gc_env_index(&g, v->root) : -1;
    if (v->root && (pi < 0 || !(g.envs[pi].state & GC_DEAD)))
      destroy_env(v->root); /* surviving parent: drop this env's ref */
    gc_free_dead_env(v);
  }
  for (int64_t c = 0; c < g.nchunks; c++) {
    for (int i = 0; i < EXP_BUMP_CHUNK; i++) {
      if (g.state[c * EXP_BUMP_CHUNK + i] & GC_DEAD) {
        gc_free_dead_cell(g.sorted[c] + i);
        freed++;
      }
    }
  }

gc_done:
  free(g.sorted);
  free(g.interns);
  free(g.state);
  free(g.stack);
  free(g.envs);
  free(g.estack);
  return make_integeri(freed);

gc_abandon:
  /* Ran out of memory building the env node set. With an incomplete edge
     picture every conclusion is unsafe, so free NOTHING and report 0. */
  freed = 0;
  goto gc_done;
}
