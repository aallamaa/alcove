/* dict.h — the open-chaining hash table (dict_t): Bernstein hash, create/
 * destroy, incremental-safe rehash, the central set_get_keyval_dict add/lookup
 * (also the env binding store), timestamp variants (RESP TTLs), and delete.
 * FRAGMENT #included into alcove.c early; callers before this point (env, ...)
 * reach it through the forward decls in alcove.h. The Lisp-value dict builtins,
 * key-encoding, and set ops live further down. NOT standalone/separately
 * compiled.
 */
static unsigned int bernstein_seed = 3102;

/* Bernstein Hash Function */
unsigned int bernstein_hash(unsigned char *key, int len) {
  unsigned int hash = bernstein_seed;
  int i;
  for (i = 0; i < len; ++i)
    hash = (hash + (hash << 5)) ^ key[i];
  return hash;
}

/* Bernstein Hash Function not key sensitive*/
unsigned int bernstein_uhash(unsigned char *key, int len) {
  unsigned int hash = bernstein_seed;
  int i;
  for (i = 0; i < len; ++i)
    hash = (hash + (hash << 5)) ^ tolower(key[i]);
  return hash;
}

static void init_kvht(kvht_t *kvht) {
  kvht->table = NULL;
  kvht->size = 0;
  kvht->sizemask = 0;
  kvht->used = 0;
}

dict_t *create_dict() {
  dict_t *d;
  d = memalloc(1, sizeof(dict_t));
  d->meta = NULL;
  init_kvht(&d->ht[0]);
  init_kvht(&d->ht[1]);
  return d;
}

int destroy_dict(dict_t *d) {
  // check if in use
  keyval_t *ckv;
  keyval_t *pkv;
  unsigned int i, j;
  for (i = 0; i < 2; i++) {
    for (j = 0; j < d->ht[i].size; j++) {
      ckv = d->ht[i].table[j];
      while (ckv) {
        pkv = ckv;
        ckv = pkv->next;
        free(pkv->key);
        unrefexp(pkv->val);
        free(pkv);
      }
    }
    if (d->ht[i].table)
      free(d->ht[i].table);
  }
  // FREE META?
  free(d);
  return 1;
}

/* Recursively verify e and EVERY nested element has a registered dump fn.
   __DUMPABLE__ alone is shallow: a vector or set IS dumpable, but a dict /
   deque element inside it is not, and __DUMP__ would then fail mid-record —
   after the type tag + count are already written — aborting the whole
   savedb and truncating the file (every persisted variable silently lost).
   The top-level dump paths use this to skip a non-round-trippable variable
   cleanly (with a warning) before writing any bytes. Depth-limited so a
   pathological / cyclic structure is treated as not-dumpable (skipped)
   rather than overflowing the stack. */
#define ALCOVE_DUMPABLE_MAX_DEPTH 512
static int is_fully_dumpable(exp_t *e, int depth) {
  if (depth > ALCOVE_DUMPABLE_MAX_DEPTH)
    return 0;
  if (!e || e == NIL_EXP || is_imm(e))
    return 1; /* nil + tagged fixnum/char always round-trip */
  if (!__DUMPABLE__(e))
    return 0;
  /* A multi-arity (defn) lambda stores clause lambdas in `content`, not a
     param list, and has no single body — the source-form serializer can't
     round-trip it. Treat it as non-dumpable so savedb skips it (with a
     warning) instead of writing a record that reloads as a broken lambda. */
  if (e->type == EXP_LAMBDA && (e->flags & FLAG_MULTI))
    return 0;
  if (isvector(e)) {
    if (vec_kind(e) != VEC_KIND_GEN)
      return 1; /* I64/F64 cells are raw scalars */
    int64_t n = vec_len(e);
    for (int64_t i = 0; i < n; i++)
      if (!is_fully_dumpable(vec_gen_at(e, i), depth + 1))
        return 0;
    return 1;
  }
  if (isset(e) || isdict(e)) {
    dict_t *sd = (dict_t *)e->ptr;
    if (sd)
      DICT_FOREACH(sd, k, 0, 0)
        if (!is_fully_dumpable(k->val, depth + 1))
          return 0;
    return 1;
  }
  if (islist(e)) {
    alc_list_t *l = (alc_list_t *)e->ptr;
    if (l)
      for (alc_listnode_t *node = l->head; node; node = node->next)
        if (!is_fully_dumpable(node->val, depth + 1))
          return 0;
    return 1;
  }
  if (ispair(e)) {
    exp_t *p = e;
    while (p && ispair(p) && istrue(p)) {
      if (!is_fully_dumpable(p->content, depth + 1))
        return 0;
      p = p->next;
    }
    if (p && !ispair(p) && !is_fully_dumpable(p, depth + 1))
      return 0; /* improper tail */
    return 1;
  }
  /* Remaining dumpable scalars (string/symbol/float/blob) and lambda/macro,
     whose bodies are always code (dumpable) — already passed __DUMPABLE__. */
  return 1;
}

int dump_dict(dict_t *d, FILE *stream) {
  // check if in use
  keyval_t *ckv;
  keyval_t *pkv;
  unsigned int i, j;
  for (i = 0; i < 2; i++) {
    for (j = 0; j < d->ht[i].size; j++) {
      ckv = d->ht[i].table[j];
      while (ckv) {
        pkv = ckv;
        ckv = pkv->next;
        if (pkv->timestamp > 0) {
          /* timestamp encoding: 0 = neutral, > 0 = persist mark
             (gettimeusec() of mark time, only nonzeroness matters here),
             < 0 = absolute-µs expire-at (RESP TTLs, never persisted). */
          if (!__DUMPABLE__(pkv->val)) {
            fprintf(stderr,
                    "savedb: skipping %s — type %d has no dump fn registered\n",
                    (char *)pkv->key, TYPEOF_E(pkv->val));
            continue;
          }
          if (__DUMP__(pkv->val, stream)) {
            dump_str(pkv->key, stream);
          }
        }
      }
    }
  }
  return 1;
}

/* Single-shot rehash. Doubles ht[0] in place and re-links every chain.
   Triggered when used >= size — without this every dict (incl. the
   global env) was permanently capped at 32 buckets, so worst-case
   chains were O(n/32) strcmps per lookup. The ht[1] machinery in
   create_dict was reserved for incremental rehash (Redis-style) but
   never wired; this is the simpler one-shot version. */
static void dict_rehash(dict_t *d, unsigned int new_size) {
  if (new_size == 0 || (new_size & (new_size - 1)) != 0)
    return; /* power of 2 */
  keyval_t **new_table = memalloc(new_size, sizeof(keyval_t *));
  if (!new_table)
    return; /* OOM: stay at current size, performance only */
  unsigned int new_mask = new_size - 1;
  unsigned int j;
  for (j = 0; j < d->ht[0].size; j++) {
    keyval_t *k = d->ht[0].table[j];
    while (k) {
      keyval_t *next = k->next;
      unsigned int h = bernstein_hash((unsigned char *)k->key, strlen(k->key));
      unsigned int slot = h & new_mask;
      k->next = new_table[slot];
      new_table[slot] = k;
      k = next;
    }
  }
  free(d->ht[0].table);
  d->ht[0].table = new_table;
  d->ht[0].size = new_size;
  d->ht[0].sizemask = new_mask;
}

keyval_t *set_get_keyval_dict(dict_t *d, char *key, exp_t *val) {
  unsigned int h = bernstein_hash((unsigned char *)key, strlen(key));
  keyval_t *k = NULL;
  if (d->ht[0].size) {
    if ((k = d->ht[0].table[h & (d->ht[0].sizemask)])) {
      while ((k->next) && (strcmp(key, k->key) != 0))
        k = k->next;
      if (val) {
        if (strcmp(key, k->key) == 0)
          unrefexp(k->val);
        else {
          k = k->next = memalloc(1, sizeof(keyval_t));
          d->ht[0].used++;
          k->key = strdup(key);
        }
      }
    } else if (val) {
      k = d->ht[0].table[h & (d->ht[0].sizemask)] =
          memalloc(1, sizeof(keyval_t));
      d->ht[0].used++;
      k->key = strdup(key);
    }
  }

  else if (val) {
    d->ht[0].size = DICT_KVHT_INITIAL_SIZE;
    d->ht[0].sizemask = DICT_KVHT_INITIAL_SIZE - 1;
    d->ht[0].table = memalloc(d->ht[0].size, sizeof(keyval_t *));
    k = d->ht[0].table[h & d->ht[0].sizemask] = memalloc(1, sizeof(keyval_t));
    d->ht[0].used++;
    k->key = strdup(key);
  };

  if (val) {
    k->val = refexp(val);
    /* Grow when load factor hits 1.0. Doubling keeps amortized O(1)
       per insert and avoids the historical 32-bucket cap that turned
       large global envs into linked-list scans. */
    if (d->ht[0].used >= d->ht[0].size)
      dict_rehash(d, d->ht[0].size * 2);
  } else {
    if (k && (strcmp(key, k->key) != 0))
      return NULL;
  }
  return k;
}

exp_t *set_keyval_dict_timestamp(dict_t *d, char *key, int64_t timestamp) {
  keyval_t *k = set_get_keyval_dict(d, key, NULL);
  if (k) {
    k->timestamp = timestamp;
    return refexp(k->val);
  }
  return NULL;
}

int64_t get_keyval_dict_timestamp(dict_t *d, char *key) {
  keyval_t *k = set_get_keyval_dict(d, key, NULL);
  if (k) {
    return k->timestamp;
  }
  return 0;
}

/* Returns 1 if a matching entry was removed, 0 otherwise. */
int del_keyval_dict(dict_t *d, char *key) {
  unsigned int h = bernstein_hash((unsigned char *)key, strlen(key));
  keyval_t *p = NULL;
  keyval_t *k;
  if (d->ht[0].size) {
    if ((k = d->ht[0].table[h & (d->ht[0].sizemask)])) {
      while ((k->next) && (strcmp(key, k->key) != 0)) {
        p = k;
        k = k->next;
      };
      if (strcmp(key, k->key) == 0) {
        unrefexp(k->val);
        free(k->key);
        d->ht[0].used--;
        if (p)
          p->next = k->next;
        else
          d->ht[0].table[h & (d->ht[0].sizemask)] = k->next;
        free(k);
        return 1;
      }
    }
  }
  return 0;
}
