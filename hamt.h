/* hamt.h — persistent map (HAMT / EXP_HAMT): nodes, ops, builtins,
 * serializers. FRAGMENT #included into alcove.c (single TU — keeps the node
 * ops inlinable against the value model and refcounting). NOT standalone, NOT
 * separately compiled. (container_apply, the generic (container key) dispatch,
 * lives here too because it needs hamt_node_get; it will move to a containers
 * module later.) `make tidy` lints it in context via alcove.c.
 */
/* ---------- persistent map (HAMT) / EXP_HAMT ops ----------
   A Hash Array Mapped Trie: an immutable map where assoc/dissoc return a NEW
   map that shares unchanged subtrees with the old one (structural sharing),
   so updates are O(log32 n) time AND space. 5 hash bits per level (32-way
   fan-out), bitmap-compressed nodes. Nodes are reference-counted C structs
   (not exp_t) shared across map versions; the trie is an acyclic DAG so
   refcounting reclaims it precisely (no cycles). A node with bitmap==0 is a
   hash-collision bucket (linear list of entries that share a full 32-bit
   hash); otherwise each present slot holds either a key/value entry
   (slot.key != NULL) or a child node (slot.key == NULL). */
#define HAMT_BITS 5
#define HAMT_MASK 31u
/* hamt_node / hamt_slot / hamt_t are declared in alcove.h (so istrue and
   print_node, which precede this section, can see the layout). */

/* Hash consistent with isequal: equal values must hash equal. Mirrors the
   types isequal compares by value (number/char/float/string/symbol/blob);
   anything else falls back to pointer identity (matching isequal's default). */
static uint32_t hamt_hashkey(exp_t *k) {
  if (isnumber(k)) {
    int64_t v = FIX_VAL(k);
    return bernstein_hash((unsigned char *)&v, sizeof v);
  }
  if (ischar(k)) {
    uint32_t v = CHAR_VAL(k);
    return bernstein_hash((unsigned char *)&v, sizeof v);
  }
  if (isfloat(k)) {
    double v = k->f;
    return bernstein_hash((unsigned char *)&v, sizeof v);
  }
  if (isstring(k) || issymbol(k)) {
    const char *s = exp_text(k);
    return bernstein_hash((unsigned char *)s, strlen(s));
  }
  if (isblob(k)) {
    return bernstein_hash((unsigned char *)blob_bytes(k), blob_len(k));
  }
  return bernstein_hash((unsigned char *)&k,
                        sizeof(void *)); /* identity hash */
}

static hamt_node *hamt_node_ref(hamt_node *n) {
  if (n)
    n->nref++;
  return n;
}
static void hamt_node_unref(hamt_node *n);

/* A HAMT slot holds either a key/value entry (is_entry true) or a child node.
   These factor the repeated entry-vs-child ref/unref branch. The caller passes
   the same is_entry predicate it used in the surrounding code: `bitmap==0 ||
   s->key` inside whole-node walks (collision buckets are all entries), or just
   `s->key` inside the bitmap-node code where bitmap != 0. */
static inline void hamt_slot_ref(hamt_slot *s, int is_entry) {
  if (is_entry) {
    refexp(s->key);
    refexp(s->val);
  } else {
    hamt_node_ref(s->child);
  }
}
static inline void hamt_slot_unref(hamt_slot *s, int is_entry) {
  if (is_entry) {
    unrefexp(s->key);
    unrefexp(s->val);
  } else {
    hamt_node_unref(s->child);
  }
}

static void hamt_node_unref(hamt_node *n) {
  if (!n || --n->nref > 0)
    return;
  for (int i = 0; i < n->n; i++)
    hamt_slot_unref(&n->slots[i], n->bitmap == 0 || n->slots[i].key);
  free(n);
}

static hamt_node *hamt_node_alloc(int n, uint32_t bitmap) {
  hamt_node *node = (hamt_node *)memalloc(1, sizeof(hamt_node) +
                                                 (size_t)n * sizeof(hamt_slot));
  node->nref = 1;
  node->bitmap = bitmap;
  node->n = n;
  return node;
}

/* Compaction copy: a fresh bitmap-node with slot `pos` dropped (n-1 slots,
   bitmap `newbitmap`), every surviving slot copied with a fresh ref. Caller
   guarantees node is a bitmap node (bitmap != 0) with node->n > 1. Factors the
   two byte-identical dissoc shrink loops. */
static hamt_node *hamt_node_without(hamt_node *node, int pos,
                                    uint32_t newbitmap) {
  hamt_node *c = hamt_node_alloc(node->n - 1, newbitmap);
  int j = 0;
  for (int i = 0; i < node->n; i++)
    if (i != pos) {
      c->slots[j] = node->slots[i];
      hamt_slot_ref(&c->slots[j], node->slots[i].key != NULL);
      j++;
    }
  return c;
}

/* Deep copy: new node owning fresh refs to every key/val/child. */
static hamt_node *hamt_node_copy(hamt_node *node) {
  hamt_node *c = hamt_node_alloc(node->n, node->bitmap);
  for (int i = 0; i < node->n; i++) {
    c->slots[i] = node->slots[i];
    hamt_slot_ref(&c->slots[i], node->bitmap == 0 || node->slots[i].key);
  }
  return c;
}

/* Build a node holding two distinct entries that collide at `shift`. */
static hamt_node *hamt_merge(exp_t *k1, exp_t *v1, uint32_t h1, exp_t *k2,
                             exp_t *v2, uint32_t h2, int shift) {
  if (shift >= 32) { /* out of hash bits → collision bucket */
    hamt_node *b = hamt_node_alloc(2, 0);
    b->slots[0].key = refexp(k1);
    b->slots[0].val = refexp(v1);
    b->slots[1].key = refexp(k2);
    b->slots[1].val = refexp(v2);
    return b;
  }
  uint32_t i1 = (h1 >> shift) & HAMT_MASK, i2 = (h2 >> shift) & HAMT_MASK;
  if (i1 == i2) {
    hamt_node *child = hamt_merge(k1, v1, h1, k2, v2, h2, shift + HAMT_BITS);
    hamt_node *node = hamt_node_alloc(1, 1u << i1);
    node->slots[0].key = NULL;
    node->slots[0].child = child;
    return node;
  }
  hamt_node *node = hamt_node_alloc(2, (1u << i1) | (1u << i2));
  int p1 = (i1 < i2) ? 0 : 1, p2 = 1 - p1;
  node->slots[p1].key = refexp(k1);
  node->slots[p1].val = refexp(v1);
  node->slots[p2].key = refexp(k2);
  node->slots[p2].val = refexp(v2);
  return node;
}

/* Lookup: returns the borrowed value for key, or NULL if absent. */
static exp_t *hamt_node_get(hamt_node *node, exp_t *key, uint32_t hash,
                            int shift) {
  if (!node)
    return NULL;
  if (node->bitmap == 0) { /* collision bucket */
    for (int i = 0; i < node->n; i++)
      if (isequal(node->slots[i].key, key))
        return node->slots[i].val;
    return NULL;
  }
  uint32_t bit = 1u << ((hash >> shift) & HAMT_MASK);
  if (!(node->bitmap & bit))
    return NULL;
  int pos = __builtin_popcount(node->bitmap & (bit - 1));
  if (node->slots[pos].key)
    return isequal(node->slots[pos].key, key) ? node->slots[pos].val : NULL;
  return hamt_node_get(node->slots[pos].child, key, hash, shift + HAMT_BITS);
}

/* assoc — returns an OWNED node (always a fresh path; the result owns one
   ref). *added set to 1 when key was not already present. node may be NULL
   (empty), producing a single-entry node. */
static hamt_node *hamt_node_assoc(hamt_node *node, exp_t *key, exp_t *val,
                                  uint32_t hash, int shift, int *added) {
  if (!node) {
    uint32_t idx = (hash >> shift) & HAMT_MASK;
    hamt_node *nn = hamt_node_alloc(1, 1u << idx);
    nn->slots[0].key = refexp(key);
    nn->slots[0].val = refexp(val);
    *added = 1;
    return nn;
  }
  if (node->bitmap == 0) { /* collision bucket */
    for (int i = 0; i < node->n; i++)
      if (isequal(node->slots[i].key, key)) {
        hamt_node *c = hamt_node_copy(node);
        unrefexp(c->slots[i].val);
        c->slots[i].val = refexp(val);
        *added = 0;
        return c;
      }
    hamt_node *c = hamt_node_alloc(node->n + 1, 0);
    for (int i = 0; i < node->n; i++) {
      c->slots[i].key = refexp(node->slots[i].key);
      c->slots[i].val = refexp(node->slots[i].val);
    }
    c->slots[node->n].key = refexp(key);
    c->slots[node->n].val = refexp(val);
    *added = 1;
    return c;
  }
  uint32_t bit = 1u << ((hash >> shift) & HAMT_MASK);
  int pos = __builtin_popcount(node->bitmap & (bit - 1));
  if (!(node->bitmap & bit)) { /* empty slot → insert entry, keeping order */
    hamt_node *c = hamt_node_alloc(node->n + 1, node->bitmap | bit);
    for (int i = 0; i < pos; i++) {
      c->slots[i] = node->slots[i];
      hamt_slot_ref(&c->slots[i], node->slots[i].key != NULL);
    }
    c->slots[pos].key = refexp(key);
    c->slots[pos].val = refexp(val);
    for (int i = pos; i < node->n; i++) {
      c->slots[i + 1] = node->slots[i];
      hamt_slot_ref(&c->slots[i + 1], node->slots[i].key != NULL);
    }
    *added = 1;
    return c;
  }
  if (node->slots[pos].key) {                 /* present entry */
    if (isequal(node->slots[pos].key, key)) { /* replace value */
      hamt_node *c = hamt_node_copy(node);
      unrefexp(c->slots[pos].val);
      c->slots[pos].val = refexp(val);
      *added = 0;
      return c;
    }
    /* different key, same slot → split into a child node */
    exp_t *ek = node->slots[pos].key, *ev = node->slots[pos].val;
    hamt_node *child =
        hamt_merge(ek, ev, hamt_hashkey(ek), key, val, hash, shift + HAMT_BITS);
    hamt_node *c = hamt_node_copy(node);
    unrefexp(c->slots[pos].key);
    unrefexp(c->slots[pos].val);
    c->slots[pos].key = NULL;
    c->slots[pos].child = child;
    *added = 1;
    return c;
  }
  /* present child → recurse */
  hamt_node *newchild = hamt_node_assoc(node->slots[pos].child, key, val, hash,
                                        shift + HAMT_BITS, added);
  hamt_node *c = hamt_node_copy(node);
  hamt_node_unref(c->slots[pos].child);
  c->slots[pos].child = newchild;
  return c;
}

/* dissoc — returns an OWNED node (or NULL if the node becomes empty). When
   the key is absent, returns hamt_node_ref(node) and leaves *removed 0. */
static hamt_node *hamt_node_dissoc(hamt_node *node, exp_t *key, uint32_t hash,
                                   int shift, int *removed) {
  if (!node) {
    *removed = 0;
    return NULL;
  }
  if (node->bitmap == 0) { /* collision bucket */
    for (int i = 0; i < node->n; i++)
      if (isequal(node->slots[i].key, key)) {
        *removed = 1;
        if (node->n == 1)
          return NULL;
        hamt_node *c = hamt_node_alloc(node->n - 1, 0);
        int j = 0;
        for (int k = 0; k < node->n; k++)
          if (k != i) {
            c->slots[j].key = refexp(node->slots[k].key);
            c->slots[j].val = refexp(node->slots[k].val);
            j++;
          }
        return c;
      }
    *removed = 0;
    return hamt_node_ref(node);
  }
  uint32_t bit = 1u << ((hash >> shift) & HAMT_MASK);
  if (!(node->bitmap & bit)) {
    *removed = 0;
    return hamt_node_ref(node);
  }
  int pos = __builtin_popcount(node->bitmap & (bit - 1));
  if (node->slots[pos].key) { /* entry */
    if (!isequal(node->slots[pos].key, key)) {
      *removed = 0;
      return hamt_node_ref(node);
    }
    *removed = 1;
    if (node->n == 1)
      return NULL;
    return hamt_node_without(node, pos, node->bitmap & ~bit);
  }
  /* child → recurse */
  hamt_node *newchild = hamt_node_dissoc(node->slots[pos].child, key, hash,
                                         shift + HAMT_BITS, removed);
  if (!*removed) {
    hamt_node_unref(newchild);
    return hamt_node_ref(node);
  }
  if (newchild == NULL) { /* child emptied → drop the slot */
    if (node->n == 1)
      return NULL;
    return hamt_node_without(node, pos, node->bitmap & ~bit);
  }
  hamt_node *c = hamt_node_copy(node);
  hamt_node_unref(c->slots[pos].child);
  c->slots[pos].child = newchild;
  return c;
}

/* Visit every entry (depth-first). Returns 0 to stop early. */
typedef int (*hamt_visit_fn)(exp_t *key, exp_t *val, void *ctx);
static int hamt_node_foreach(hamt_node *node, hamt_visit_fn fn, void *ctx) {
  if (!node)
    return 1;
  for (int i = 0; i < node->n; i++) {
    if (node->bitmap == 0 || node->slots[i].key) {
      if (!fn(node->slots[i].key, node->slots[i].val, ctx))
        return 0;
    } else if (!hamt_node_foreach(node->slots[i].child, fn, ctx)) {
      return 0;
    }
  }
  return 1;
}

/* Deep map equality (for `iso`): same count, and every key in `a` maps to an
   iso-equal value in `b`. With equal counts that's a bijection → equal. */
typedef struct {
  exp_t *other;
  int ok;
} hamt_iso_ctx;
static int hamt_iso_visit(exp_t *key, exp_t *val, void *ctx) {
  hamt_iso_ctx *c = (hamt_iso_ctx *)ctx;
  hamt_t *b = (hamt_t *)c->other->ptr;
  exp_t *bv = hamt_node_get(b->root, key, hamt_hashkey(key), 0);
  if (!bv || !isoequal(val, bv)) {
    c->ok = 0;
    return 0; /* stop the walk */
  }
  return 1;
}
static int hamt_iso(exp_t *a, exp_t *b) {
  hamt_t *ha = (hamt_t *)a->ptr, *hb = (hamt_t *)b->ptr;
  if (ha->count != hb->count)
    return 0;
  hamt_iso_ctx ctx = {b, 1};
  hamt_node_foreach(ha->root, hamt_iso_visit, &ctx);
  return ctx.ok;
}

/* Wrap a (root,count) into a fresh EXP_HAMT value. Takes ownership of root. */
static exp_t *hamt_wrap(hamt_node *root, int64_t count) {
  hamt_t *h = (hamt_t *)memalloc(1, sizeof(hamt_t));
  h->root = root;
  h->count = count;
  MAKE_TYPED(m, EXP_HAMT, h);
  return m;
}

void hamt_free(void *ptr) {
  hamt_t *h = (hamt_t *)ptr;
  if (!h)
    return;
  hamt_node_unref(h->root);
  free(h);
}

static int hamt_print_one(exp_t *k, exp_t *v, void *ctx) {
  int *first = (int *)ctx;
  if (!*first)
    printf(", ");
  *first = 0;
  print_node(k);
  printf(" ");
  print_node(v);
  return 1;
}
/* Rendered like a map literal: {k v, k v}. Called from print_node. */
void hamt_print(exp_t *m) {
  hamt_t *h = (hamt_t *)m->ptr;
  int first = 1;
  printf("{");
  if (h)
    hamt_node_foreach(h->root, hamt_print_one, &first);
  printf("}");
}

/* savedb/loaddb persistence: serialize as type-tag, entry count, then each
   (key, value) pair recursively via the generic dump/load. Mirrors the deque
   serializer. */
typedef struct {
  FILE *s;
  int ok;
} hamt_dump_ctx;
static int hamt_dump_visit(exp_t *key, exp_t *val, void *ctx) {
  hamt_dump_ctx *c = (hamt_dump_ctx *)ctx;
  if (!key || !__DUMPABLE__(key) || !__DUMP__(key, c->s) || !val ||
      !__DUMPABLE__(val) || !__DUMP__(val, c->s)) {
    c->ok = 0;
    return 0;
  }
  return 1;
}
exp_t *dump_hamt_value(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  hamt_t *h = (hamt_t *)e->ptr;
  size_t n = h ? (size_t)h->count : 0;
  if (dumpsize_t(stream, &n) <= 0)
    return NULL;
  if (!h || !h->root)
    return e;
  hamt_dump_ctx ctx = {stream, 1};
  hamt_node_foreach(h->root, hamt_dump_visit, &ctx);
  return ctx.ok ? e : NULL;
}
exp_t *load_hamt_value(exp_t *e, FILE *stream) {
  if (e)
    unrefexp(e);
  size_t n = 0;
  if (loadsize_t(stream, &n) <= 0 || n > (size_t)(1u << 28))
    return NULL;
  hamt_node *root = NULL;
  int64_t count = 0;
  for (size_t i = 0; i < n; i++) {
    exp_t *k = load_exp_t(stream);
    if (!k) {
      hamt_node_unref(root);
      return NULL;
    }
    exp_t *v = load_exp_t(stream);
    if (!v) {
      unrefexp(k);
      hamt_node_unref(root);
      return NULL;
    }
    int added = 0;
    hamt_node *nr = hamt_node_assoc(root, k, v, hamt_hashkey(k), 0, &added);
    hamt_node_unref(root);
    root = nr;
    count += added;
    unrefexp(k);
    unrefexp(v);
  }
  return hamt_wrap(root, count);
}

const char doc_hamt[] =
    "(hamt k v ...) — build a persistent (immutable) map from key/value "
    "pairs. assoc/dissoc return new maps sharing structure with the old.";
exp_t *hamtcmd(exp_t *e, env_t *env) {
  /* Evaluate alternating key/value args into a fresh persistent map. */
  hamt_node *root = NULL;
  int64_t count = 0;
  exp_t *cur = e->next;
  exp_t *err = NULL;
  while (cur) {
    if (!cur->next) {
      err = error(ERROR_MISSING_PARAMETER, e, env,
                  "hamt: odd number of args (need key/value pairs)");
      break;
    }
    exp_t *k = EVAL(cur->content, env);
    if (iserror(k)) {
      err = k;
      break;
    }
    exp_t *v = EVAL(cur->next->content, env);
    if (iserror(v)) {
      unrefexp(k);
      err = v;
      break;
    }
    int added = 0;
    hamt_node *nr = hamt_node_assoc(root, k, v, hamt_hashkey(k), 0, &added);
    hamt_node_unref(root);
    root = nr;
    count += added;
    unrefexp(k);
    unrefexp(v);
    cur = cur->next->next;
  }
  unrefexp(e);
  if (err) {
    hamt_node_unref(root);
    return err;
  }
  return hamt_wrap(root, count);
}

const char doc_hamtassoc[] =
    "(hamt-assoc m k v) — new map with k→v added/updated; m is unchanged.";
exp_t *hamtassoccmd(exp_t *e, env_t *env) {
  EVAL_ARG_3(m, k, v);
  REQUIRE_TYPE(m, ishamt, CLEAN_RETURN_3(m, k, v, _alc_e), ERROR_ILLEGAL_VALUE,
               e, env, "hamt-assoc: not a hamt");
  hamt_t *h = (hamt_t *)m->ptr;
  int added = 0;
  hamt_node *nr = hamt_node_assoc(h->root, k, v, hamt_hashkey(k), 0, &added);
  exp_t *ret = hamt_wrap(nr, h->count + added);
  CLEAN_RETURN_3(m, k, v, ret);
}

const char doc_hamtget[] =
    "(hamt-get m k [default]) — value for k, or default (nil) if absent.";
exp_t *hamtgetcmd(exp_t *e, env_t *env) {
  exp_t *m = NULL, *k = NULL, *dflt = NULL, *err = NULL, *ret = NULL;
  if (!e->next || !e->next->next) {
    err = error(ERROR_MISSING_PARAMETER, e, env, "(hamt-get m k [default])");
    goto done;
  }
  m = EVAL(e->next->content, env);
  if (iserror(m)) {
    err = m;
    m = NULL;
    goto done;
  }
  k = EVAL(e->next->next->content, env);
  if (iserror(k)) {
    err = k;
    k = NULL;
    goto done;
  }
  if (e->next->next->next) {
    dflt = EVAL(e->next->next->next->content, env);
    if (iserror(dflt)) {
      err = dflt;
      dflt = NULL;
      goto done;
    }
  }
  if (!ishamt(m)) {
    err = error(ERROR_ILLEGAL_VALUE, e, env, "hamt-get: not a hamt");
    goto done;
  }
  {
    hamt_t *h = (hamt_t *)m->ptr;
    exp_t *v = hamt_node_get(h->root, k, hamt_hashkey(k), 0);
    ret = v ? refexp(v) : refexp(dflt ? dflt : NIL_EXP);
  }
done:
  unrefexp(m);
  unrefexp(k);
  unrefexp(dflt);
  unrefexp(e);
  return err ? err : ret;
}

const char doc_hamtdissoc[] =
    "(hamt-dissoc m k) — new map without k; m is unchanged.";
exp_t *hamtdissoccmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(m, k);
  REQUIRE_TYPE(m, ishamt, CLEAN_RETURN_2(m, k, _alc_e), ERROR_ILLEGAL_VALUE, e,
               env, "hamt-dissoc: not a hamt");
  hamt_t *h = (hamt_t *)m->ptr;
  int removed = 0;
  hamt_node *nr = hamt_node_dissoc(h->root, k, hamt_hashkey(k), 0, &removed);
  exp_t *ret = hamt_wrap(nr, h->count - removed);
  CLEAN_RETURN_2(m, k, ret);
}

const char doc_hamtcount[] = "(hamt-count m) — number of entries in the map.";
exp_t *hamtcountcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(m);
  REQUIRE_TYPE(m, ishamt, CLEAN_RETURN_1(m, _alc_e), ERROR_ILLEGAL_VALUE, e,
               env, "hamt-count: not a hamt");
  int64_t c = ((hamt_t *)m->ptr)->count;
  CLEAN_RETURN_1(m, MAKE_FIX(c));
}

const char doc_hamtcontainsp[] =
    "(hamt-contains? m k) — t if k is present, else nil.";
exp_t *hamtcontainspcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(m, k);
  REQUIRE_TYPE(m, ishamt, CLEAN_RETURN_2(m, k, _alc_e), ERROR_ILLEGAL_VALUE, e,
               env, "hamt-contains?: not a hamt");
  hamt_t *h = (hamt_t *)m->ptr;
  exp_t *v = hamt_node_get(h->root, k, hamt_hashkey(k), 0);
  CLEAN_RETURN_2(m, k, refexp(v ? TRUE_EXP : NIL_EXP));
}

/* Apply a callable container to one already-evaluated argument, consuming
   arg's ref (see the forward decl near vm_run). Indexable string/vector/blob:
   element by integer index (a float index truncates), error if non-number or
   out of range. Keyed: dict and hamt return the value at the key (nil if
   absent), set returns the member (nil if absent) — Clojure's (m k) / (s e).
   Returns an owned result or an error exp. */
static exp_t *container_apply(exp_t *c, exp_t *arg, env_t *env) {
  if (isindexable(c)) {
    if (!isnumber(arg) && !isfloat(arg)) {
      unrefexp(arg);
      return error(ERROR_NUMBER_EXPECTED, c, env,
                   "index: arg must be a number");
    }
    int64_t i = isnumber(arg) ? FIX_VAL(arg) : (int64_t)arg->f;
    unrefexp(arg);
    exp_t *r = index_get(c, i);
    return r ? r
             : error(ERROR_INDEX_OUT_OF_RANGE, c, env,
                     "index: %lld out of range", (long long)i);
  }
  if (ishamt(c)) {
    hamt_t *h = (hamt_t *)c->ptr;
    exp_t *v = h ? hamt_node_get(h->root, arg, hamt_hashkey(arg), 0) : NULL;
    exp_t *ret = refexp(v ? v : NIL_EXP);
    unrefexp(arg);
    return ret;
  }
  if (isset(c)) {
    /* Sets canonicalize members with a type tag (set_key_for_value, malloc'd)
       so 2 and "2" are distinct — must use the same encoder as set-has?. */
    char *ks = set_key_for_value(arg);
    keyval_t *kv =
        (ks && c->ptr) ? set_get_keyval_dict((dict_t *)c->ptr, ks, NULL) : NULL;
    free(ks);
    exp_t *ret = refexp(kv ? arg : NIL_EXP); /* the member, or nil if absent */
    unrefexp(arg);
    return ret;
  }
  /* dict — canonical string key (keyword/string/number; else a miss -> nil). */
  {
    char tmp[32];
    char *ks = alc_key_to_cstr(arg, tmp);
    keyval_t *kv =
        (ks && c->ptr) ? set_get_keyval_dict((dict_t *)c->ptr, ks, NULL) : NULL;
    exp_t *ret = refexp(kv ? kv->val : NIL_EXP);
    unrefexp(arg);
    return ret;
  }
}

static int hamt_collect_keys(exp_t *key, exp_t *val, void *ctx) {
  (void)val;
  exp_t **acc = (exp_t **)ctx; /* acc[0]=head, acc[1]=tail */
  exp_t *node = make_node(refexp(key));
  if (!acc[0]) {
    acc[0] = node;
    acc[1] = node;
  } else {
    acc[1]->next = node;
    acc[1] = node;
  }
  return 1;
}
const char doc_hamtkeys[] =
    "(hamt-keys m) — list of the map's keys (unordered).";
exp_t *hamtkeyscmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(m);
  REQUIRE_TYPE(m, ishamt, CLEAN_RETURN_1(m, _alc_e), ERROR_ILLEGAL_VALUE, e,
               env, "hamt-keys: not a hamt");
  exp_t *acc[2] = {NULL, NULL};
  hamt_node_foreach(((hamt_t *)m->ptr)->root, hamt_collect_keys, acc);
  CLEAN_RETURN_1(m, acc[0] ? acc[0] : NIL_EXP);
}

const char doc_hamtp[] = "(hamt? x) — t if x is a persistent map, else nil.";
exp_t *hamtpcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(x);
  CLEAN_RETURN_1(x, refexp(ishamt(x) ? TRUE_EXP : NIL_EXP));
}

static int hamt_collect_vals(exp_t *key, exp_t *val, void *ctx) {
  (void)key;
  exp_t **acc = (exp_t **)ctx;
  exp_t *node = make_node(refexp(val));
  if (!acc[0]) {
    acc[0] = node;
    acc[1] = node;
  } else {
    acc[1]->next = node;
    acc[1] = node;
  }
  return 1;
}
const char doc_hamtvals[] =
    "(hamt-vals m) — list of the map's values (unordered).";
exp_t *hamtvalscmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(m);
  REQUIRE_TYPE(m, ishamt, CLEAN_RETURN_1(m, _alc_e), ERROR_ILLEGAL_VALUE, e,
               env, "hamt-vals: not a hamt");
  exp_t *acc[2] = {NULL, NULL};
  hamt_node_foreach(((hamt_t *)m->ptr)->root, hamt_collect_vals, acc);
  CLEAN_RETURN_1(m, acc[0] ? acc[0] : NIL_EXP);
}

static int hamt_collect_kv(exp_t *key, exp_t *val, void *ctx) {
  exp_t **acc =
      (exp_t **)ctx; /* flat: k1 v1 k2 v2 ... (round-trips via hamt) */
  exp_t *kn = make_node(refexp(key));
  exp_t *vn = make_node(refexp(val));
  kn->next = vn;
  if (!acc[0]) {
    acc[0] = kn;
  } else {
    acc[1]->next = kn;
  }
  acc[1] = vn;
  return 1;
}
const char doc_hamtlist[] =
    "(hamt->list m) — flat list (k1 v1 k2 v2 ...) of entries (unordered); "
    "round-trips via (apply hamt (hamt->list m)).";
exp_t *hamtlistcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(m);
  REQUIRE_TYPE(m, ishamt, CLEAN_RETURN_1(m, _alc_e), ERROR_ILLEGAL_VALUE, e,
               env, "hamt->list: not a hamt");
  exp_t *acc[2] = {NULL, NULL};
  hamt_node_foreach(((hamt_t *)m->ptr)->root, hamt_collect_kv, acc);
  CLEAN_RETURN_1(m, acc[0] ? acc[0] : NIL_EXP);
}

typedef struct {
  hamt_node *root;
  int64_t count;
} hamt_merge_acc;
static int hamt_merge_one(exp_t *key, exp_t *val, void *ctx) {
  hamt_merge_acc *a = (hamt_merge_acc *)ctx;
  int added = 0;
  hamt_node *nr =
      hamt_node_assoc(a->root, key, val, hamt_hashkey(key), 0, &added);
  hamt_node_unref(a->root);
  a->root = nr;
  a->count += added;
  return 1;
}
const char doc_hamtmerge[] =
    "(hamt-merge a b) — new map with all of a's and b's entries; on a key in "
    "both, b's value wins. a and b are unchanged.";
exp_t *hamtmergecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(a, b);
  if (!ishamt(a) || !ishamt(b))
    CLEAN_RETURN_2(
        a, b, error(ERROR_ILLEGAL_VALUE, e, env, "hamt-merge: need two hamts"));
  hamt_t *ha = (hamt_t *)a->ptr, *hb = (hamt_t *)b->ptr;
  hamt_merge_acc acc;
  acc.root = hamt_node_ref(ha->root); /* start from a (shares a's nodes) */
  acc.count = ha->count;
  hamt_node_foreach(hb->root, hamt_merge_one, &acc);
  exp_t *ret = hamt_wrap(acc.root, acc.count);
  CLEAN_RETURN_2(a, b, ret);
}
