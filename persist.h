/* persist.h — per-type binary (de)serializers for savedb/loaddb: dump_exp_t
 * plus a dump/load pair for each value type (char/string/number/float/
 * symbol/pair/lambda/macro/blob/vec/set/dict/deque). FRAGMENT #included into
 * alcove.c at the serializers' original position (so the load_* calls to the
 * value constructors resolve via the alcove.h forward decls exactly as before).
 * The savedb/loaddb orchestration and the exp_tfuncList registration (in main)
 * stay in alcove.c. NOT standalone, NOT separately compiled.
 */
exp_t *dump_exp_t(exp_t *e, FILE *stream) { return __DUMP__(e, stream); }

exp_t *load_char(exp_t *e, FILE *stream) {
  /* Chars are tagged immediates — discard the placeholder exp_t that
     load_exp_t handed us and return a fresh tagged char. */
  int c = getc(stream);
  if (e)
    unrefexp(e);
  if (c == EOF)
    return NULL;
  return MAKE_CHAR(utf8_decode_stream(c, stream));
}

exp_t *dump_char(exp_t *e, FILE *stream) {
  unsigned short int t = EXP_CHAR;
  if (dumptype(stream, &t) <= 0)
    return NULL;
  char u[4];
  int k = utf8_encode((uint32_t)CHAR_VAL(e), u);
  if (fwrite(u, 1, (size_t)k, stream) != (size_t)k)
    return NULL;
  return e;
}

/* Cap on string lengths from a db.dump file. 16 MiB is plenty for any
   real symbol/string we'd persist; bigger values are almost certainly
   either corruption or a malicious header trying to wrap (length+1 -> 0
   then giant fread). The cap is checked before the alloc so neither
   the wrap nor the read happens on bad input. */
#define ALCOVE_LOAD_MAX_STRLEN ((size_t)1 << 24)

char *load_str(char **pptr, FILE *stream) {
  size_t length;
  char *ptr;
  if (loadsize_t(stream, &length) <= 0)
    return NULL;
  if (length > ALCOVE_LOAD_MAX_STRLEN) {
    *pptr = NULL;
    return NULL;
  }
  ptr = *pptr = memalloc(length + 1, sizeof(char));
  if (!ptr) {
    *pptr = NULL;
    return NULL;
  }
  if (fread(ptr, 1, length, stream) != length) {
    free(ptr);
    *pptr = NULL;
    return NULL;
  }
  *((char *)(ptr + length)) = '\0';
  return ptr;
}

exp_t *load_string(exp_t *e, FILE *stream) {
  if (load_str((char **)&(e->ptr), stream))
    return e;
  unrefexp(e); /* release placeholder on read failure */
  return NULL;
}

char *dump_str(char *ptr, FILE *stream) {
  size_t length = strlen(ptr);
  if (dumpsize_t(stream, &length) <= 0)
    return NULL;
  /* fwrite returns 0 on success for empty strings — guard with length > 0
     to avoid treating a successful empty write as a failure. */
  if (length > 0 && fwrite(ptr, 1, length, stream) != length)
    return NULL;
  return ptr;
}

/* Binary-safe variants — RESP keys can hold NULs, so the strlen-based
   dump_str path doesn't apply. dump_strn writes the size prefix followed
   by exactly n bytes; load_strn reads symmetrically and NUL-terminates
   the returned buffer for caller convenience (klen carries the real
   length, the trailing NUL is decorative). */
static int dump_strn(const char *ptr, size_t n, FILE *stream) {
  if (dumpsize_t(stream, &n) <= 0)
    return 0;
  return n == 0 || fwrite(ptr, 1, n, stream) == n;
}
static int load_strn(char **pptr, size_t *plen, FILE *stream) {
  size_t n;
  if (loadsize_t(stream, &n) <= 0)
    return 0;
  /* Sanity cap matches RESP_MAX_BULK so a corrupted dump can't drag
     us into a 16 EB allocation. */
  if (n > (size_t)(512u * 1024 * 1024))
    return 0;
  char *p = malloc(n + 1);
  if (!p)
    return 0;
  if (n > 0 && fread(p, 1, n, stream) != n) {
    free(p);
    return 0;
  }
  p[n] = 0;
  *pptr = p;
  *plen = n;
  return 1;
}

/* Container (de)serializer prologues. The set/dict/deque dumps all write the
   type tag then a size_t element count, recurse per element with the same
   dumpability guard, and their loads all discard the placeholder, read a
   size_t count, and reject a corrupt/oversized count against the same cap.
   These macros factor that shared boilerplate; blob and vec keep their own
   headers (different wire shape: vec writes a kind byte + u32, blob a 512 MiB
   byte cap) so their exact bytes are untouched. */
#define DUMP_HEADER(e, n)                                                      \
  do {                                                                         \
    if (dumptype(stream, &(e)->type) <= 0)                                     \
      return NULL;                                                             \
    if (dumpsize_t(stream, &(n)) <= 0)                                         \
      return NULL;                                                             \
  } while (0)
#define DUMP_ELEM(v, stream)                                                   \
  do {                                                                         \
    if (!(v) || !__DUMPABLE__(v) || !__DUMP__((v), stream))                    \
      return NULL;                                                             \
  } while (0)
#define LOAD_DISCARD_AND_COUNT(e, n, cap)                                      \
  do {                                                                         \
    if (e)                                                                     \
      unrefexp(e);                                                             \
    (n) = 0;                                                                   \
    if (loadsize_t(stream, &(n)) <= 0 || (n) > (size_t)(cap))                  \
      return NULL;                                                             \
  } while (0)

exp_t *dump_set(exp_t *e, FILE *stream) {
  dict_t *d = (dict_t *)e->ptr;
  size_t n = d ? (size_t)d->ht[0].used : 0;
  DUMP_HEADER(e, n);
  if (!d)
    return e;
  for (unsigned int i = 0; i < d->ht[0].size; i++) {
    for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
      DUMP_ELEM(k->val, stream);
    }
  }
  return e;
}

exp_t *load_set(exp_t *e, FILE *stream) {
  size_t n;
  LOAD_DISCARD_AND_COUNT(e, n, 1u << 28);
  exp_t *ret = make_set_exp();
  dict_t *d = (dict_t *)ret->ptr;
  for (size_t i = 0; i < n; i++) {
    exp_t *val = load_exp_t(stream);
    if (!val || !set_insert_value(d, val)) {
      if (val)
        unrefexp(val);
      unrefexp(ret);
      return NULL;
    }
    unrefexp(val);
  }
  return ret;
}

/* EXP_DICT serializer. Format: type tag + entry count + per entry a
   bare string key (dump_str, no type tag — dict keys are always the
   canonicalized C-string from alc_key_to_cstr) followed by the value
   (__DUMP__, self-describing). Walks ht[0] only — dict_rehash is
   one-shot into ht[0], so used == entry count. The top-level dump path
   pre-checks dumpability recursively, so values here are dumpable; the
   per-value __DUMPABLE__ guard is defense in depth (matches dump_set). */
exp_t *dump_dict_value(exp_t *e, FILE *stream) {
  dict_t *d = (dict_t *)e->ptr;
  size_t n = d ? (size_t)d->ht[0].used : 0;
  DUMP_HEADER(e, n);
  if (!d)
    return e;
  for (unsigned int i = 0; i < d->ht[0].size; i++)
    for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
      if (!dump_str(k->key, stream))
        return NULL;
      DUMP_ELEM(k->val, stream);
    }
  return e;
}

exp_t *load_dict_value(exp_t *e, FILE *stream) {
  size_t n;
  LOAD_DISCARD_AND_COUNT(e, n, 1u << 28);
  exp_t *ret = make_dict_exp();
  dict_t *d = (dict_t *)ret->ptr;
  for (size_t i = 0; i < n; i++) {
    char *key = NULL;
    if (!load_str(&key, stream)) {
      unrefexp(ret);
      return NULL;
    }
    exp_t *val = load_exp_t(stream);
    if (!val) {
      free(key);
      unrefexp(ret);
      return NULL;
    }
    /* set_get_keyval_dict strdup's the key and refexp's the value, so we
       still own both: free our key copy and drop our load ref. */
    set_get_keyval_dict(d, key, val);
    free(key);
    unrefexp(val);
  }
  return ret;
}

/* EXP_LIST (deque) serializer. Format: type tag + element count + each
   element (__DUMP__) in head->tail order. Load rebuilds with
   alc_list_push_right (appends to tail), preserving order. */
exp_t *dump_deque_value(exp_t *e, FILE *stream) {
  alc_list_t *l = (alc_list_t *)e->ptr;
  size_t n = l ? (size_t)l->len : 0;
  DUMP_HEADER(e, n);
  if (!l)
    return e;
  for (alc_listnode_t *node = l->head; node; node = node->next)
    DUMP_ELEM(node->val, stream);
  return e;
}

exp_t *load_deque_value(exp_t *e, FILE *stream) {
  size_t n;
  LOAD_DISCARD_AND_COUNT(e, n, 1u << 28);
  exp_t *ret = make_list_exp();
  alc_list_t *l = (alc_list_t *)ret->ptr;
  for (size_t i = 0; i < n; i++) {
    exp_t *val = load_exp_t(stream);
    if (!val) {
      unrefexp(ret);
      return NULL;
    }
    alc_list_push_right(l, val); /* takes ownership of val */
  }
  return ret;
}

/* EXP_BLOB serializer — alc_blob_t is {len, bytes[]} (binary-safe).
   Format: type tag (already written by dispatch wrapper for load,
   we write it here for dump symmetric with dump_string), then size_t
   len, then exactly len bytes. */
exp_t *dump_blob(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  alc_blob_t *b = (alc_blob_t *)e->ptr;
  size_t n = b->len;
  if (dumpsize_t(stream, &n) <= 0)
    return NULL;
  if (n > 0 && fwrite(b->bytes, 1, n, stream) != n)
    return NULL;
  return e;
}
exp_t *load_blob(exp_t *e, FILE *stream) {
  /* load_exp_t allocated a placeholder via make_nil(); discard it and
     return a fresh blob — matches the load_char/load_string pattern. */
  if (e)
    unrefexp(e);
  size_t n;
  if (loadsize_t(stream, &n) <= 0)
    return NULL;
  if (n > (size_t)(512u * 1024 * 1024))
    return NULL;
  /* make_blob copies the input; using a stack buffer for tiny blobs
     would be a micro-opt but the heap path is fine for cold load. */
  char *buf = (n > 0) ? malloc(n) : NULL;
  if (n > 0 && (!buf || fread(buf, 1, n, stream) != n)) {
    free(buf);
    return NULL;
  }
  exp_t *blob = make_blob(buf ? buf : "", n);
  free(buf);
  return blob;
}

exp_t *dump_string(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  if (dump_str(exp_text(e), stream))
    return e;
  else
    return NULL;
}

/* EXP_NUMBER (tagged fixnum) — write 8 raw bytes (int64 untagged value).
   Like load_char, the placeholder allocated by load_exp_t is discarded
   because tagged immediates aren't heap exp_t*. */
exp_t *dump_number(exp_t *e, FILE *stream) {
  unsigned short int t = EXP_NUMBER;
  int64_t v = FIX_VAL(e);
  if (dumptype(stream, &t) <= 0)
    return NULL;
  if (fwrite(&v, sizeof(v), 1, stream) != 1)
    return NULL;
  return e;
}
exp_t *load_number(exp_t *e, FILE *stream) {
  int64_t v;
  if (e)
    unrefexp(e);
  if (fread(&v, sizeof(v), 1, stream) != 1)
    return NULL;
  return MAKE_FIX(v);
}

/* EXP_FLOAT — heap exp_t with `f` field (double). 8 raw bytes. */
exp_t *dump_float(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  if (fwrite(&e->f, sizeof(e->f), 1, stream) != 1)
    return NULL;
  return e;
}
exp_t *load_float(exp_t *e, FILE *stream) {
  if (fread(&e->f, sizeof(e->f), 1, stream) != 1) {
    unrefexp(e); /* release placeholder on read failure */
    return NULL;
  }
  return e;
}

/* EXP_SYMBOL — same length-prefixed bytes as a string; on load we just
   stash the name into e->ptr. Symbol identity (eq?) isn't preserved
   across runs but iso-equality / lookup-by-name works fine. */
exp_t *dump_symbol(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  if (dump_str(exp_text(e), stream))
    return e;
  return NULL;
}
exp_t *load_symbol(exp_t *e, FILE *stream) {
  if (load_str((char **)&(e->ptr), stream))
    return e;
  unrefexp(e); /* release placeholder on read failure */
  return NULL;
}

/* EXP_PAIR — a cons cell. content=car, next=cdr (alcove uses `next`
   as the cdr field for its linked-list representation). Both children
   may be NULL (e.g., nil = (PAIR, NULL, NULL)). We use a 1-byte flag
   to record which children are present so improper pairs (a . b) and
   the empty list both round-trip. Recurses via __DUMP__ so any element
   whose type has a registered dump fn is preserved; mixed-type lists
   work transparently. */
exp_t *dump_pair(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  uint8_t flags = (e->content ? 1 : 0) | (e->next ? 2 : 0);
  if (fwrite(&flags, 1, 1, stream) != 1)
    return NULL;
  if ((flags & 1) && !__DUMP__(e->content, stream))
    return NULL;
  if ((flags & 2) && !__DUMP__(e->next, stream))
    return NULL;
  return e;
}
exp_t *load_pair(exp_t *e, FILE *stream) {
  uint8_t flags;
  if (fread(&flags, 1, 1, stream) != 1)
    return NULL;
  if (flags & 1) {
    e->content = load_exp_t(stream);
    if (!e->content)
      return NULL; /* propagate sub-read failure */
  }
  if (flags & 2) {
    e->next = load_exp_t(stream);
    if (!e->next)
      return NULL; /* propagate sub-read failure */
  }
  return e;
}

/* EXP_LAMBDA — persisted as source: name + params tree + body tree.
   On load we reconstruct the lambda exp_t with the same shape defcmd
   builds, then call compile_lambda so the bytecode VM (and JIT, where
   the shape matches) sees the function. JIT pages don't survive a
   restart but get re-installed at compile time on the new arch.

   Limitations: closures over locals don't survive (alcove doesn't seem
   to support lexical closures over let/with bindings beyond the
   enclosing global env). Recursive references resolve fine because the
   loader installs the lambda into the global env under its persisted
   name before the body is ever called. */
/* dump_lambda ≡ dump_macro (byte-identical) and load_lambda/load_macro differ
   only in the trailing compile_lambda. dump_callable / load_callable hold that
   shared body; the thin per-type wrappers below stay so the exp_tfuncList
   dispatch table still has one fn per type. */
static exp_t *dump_callable(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  /* Name (empty string for anonymous fns; preserves shape on the wire). */
  const char *name = e->meta ? (const char *)e->meta : "";
  if (!dump_str((char *)name, stream))
    return NULL;
  /* Flags: bit0 = has params; bit1 = has body. */
  exp_t *params = lambda_params(e);
  uint8_t flags = 0;
  if (params)
    flags |= 1;
  if (e->next && e->next->content)
    flags |= 2;
  if (fwrite(&flags, 1, 1, stream) != 1)
    return NULL;
  if ((flags & 1) && !__DUMP__(params, stream))
    return NULL;
  if ((flags & 2) && !__DUMP__(e->next->content, stream))
    return NULL;
  return e;
}
/* compile != 0 → compile_lambda after rebuild (lambdas → bytecode VM/JIT);
   compile == 0 → leave AST-only (macros, run by the macro expander). */
static exp_t *load_callable(exp_t *e, FILE *stream, int compile) {
  char *name = NULL;
  if (!load_str(&name, stream)) {
    unrefexp(e); /* release placeholder on read failure */
    return NULL;
  }
  uint8_t flags;
  if (fread(&flags, 1, 1, stream) != 1) {
    free(name);
    unrefexp(e);
    return NULL;
  }
  exp_t *params = (flags & 1) ? load_exp_t(stream) : NULL;
  exp_t *body = (flags & 2) ? load_exp_t(stream) : NULL;
  /* Mirror defcmd's lambda shape: e->content = params, e->next is a
     wrapping node whose content is the body list. */
  e->content = params;
  e->next = make_node(body);
  if (name && name[0]) {
    e->meta = (keyval_t *)name; /* take ownership */
  } else {
    free(name);
    e->meta = NULL;
  }
  /* Silent fallback to AST eval if compile_lambda can't compile (e.g.,
     body uses an unsupported form). The lambda still works either way.
     Persisted lambdas are top-level (no captured env survives a dump). */
  if (compile)
    compile_lambda(e, 0, NULL, TYPE_HINT_NONE); /* hints aren't persisted */
  return e;
}

/* EXP_LAMBDA — persisted as source: name + params tree + body tree.
   On load we reconstruct the lambda exp_t with the same shape defcmd
   builds, then call compile_lambda so the bytecode VM (and JIT, where
   the shape matches) sees the function. JIT pages don't survive a
   restart but get re-installed at compile time on the new arch.

   Limitations: closures over locals don't survive (alcove doesn't seem
   to support lexical closures over let/with bindings beyond the
   enclosing global env). Recursive references resolve fine because the
   loader installs the lambda into the global env under its persisted
   name before the body is ever called. */
exp_t *dump_lambda(exp_t *e, FILE *stream) { return dump_callable(e, stream); }
exp_t *load_lambda(exp_t *e, FILE *stream) {
  return load_callable(e, stream, 1);
}

/* EXP_MACRO — same on-wire shape as EXP_LAMBDA (defmacrocmd builds an
   identical exp_t structure, just with type=EXP_MACRO). The only load-
   side difference is that macros are AST-evaluated, so we skip
   compile_lambda. Source-form persistence: the macro body is preserved
   exactly and re-installed at load time. */
exp_t *dump_macro(exp_t *e, FILE *stream) { return dump_callable(e, stream); }
exp_t *load_macro(exp_t *e, FILE *stream) {
  return load_callable(e, stream, 0);
}

/* Forward decls for the vec helpers defined alongside make_vector. */
static exp_t *vec_get_boxed(exp_t *vexp, int64_t i);
static exp_t **vec_gen_cells(exp_t *vexp);
static alc_vec_t *vec_alloc_storage(int64_t cap);
static int vec_tighten(exp_t *vexp);

/* Set by alcove_load_unified to the version read from the file header;
   read by load_vec to choose between v1/v2 record layouts. Single
   loader at a time — there's no concurrent loadu path. Initialised to
   2 (current version) so any dump path that bypasses the header-read
   still writes/reads v2-compatible records. */
static int alcove_load_dump_version = 2;

/* EXP_VECTOR — v2 record (dump_vec always writes v2):
     [u8 kind][u32 len][payload]
       kind == VEC_KIND_GEN >> 4: len × __DUMP__(exp_t*)
       kind == VEC_KIND_I64 >> 4: len × int64_t (raw little-endian)
       kind == VEC_KIND_F64 >> 4: len × double  (raw little-endian)

   v1 dumps are still read by load_vec via load_vec_v1 (boxed cells,
   reconstructs as GEN). The kind on the wire is the bit pattern shifted
   right by 4 so it stays compact (0/1/2) and stable across future flag
   layouts. */
exp_t *dump_vec(exp_t *e, FILE *stream) {
  if (dumptype(stream, &e->type) <= 0)
    return NULL;
  unsigned k = vec_kind(e);
  uint8_t kind_tag = (uint8_t)(k >> 4);
  if (fwrite(&kind_tag, 1, 1, stream) != 1)
    return NULL;
  uint32_t n = (uint32_t)vec_len(e);
  if (fwrite(&n, sizeof(n), 1, stream) != 1)
    return NULL;
  if (k == VEC_KIND_GEN) {
    for (uint32_t i = 0; i < n; i++) {
      exp_t *cell = vec_gen_at(e, i);
      /* Defense in depth: the top-level dump pre-checks dumpability
         recursively (is_fully_dumpable), so a non-dumpable cell shouldn't
         reach here — but guard anyway to match dump_set and fail cleanly. */
      if (!__DUMPABLE__(cell) || !__DUMP__(cell, stream))
        return NULL;
    }
  } else if (k == VEC_KIND_I64) {
    int64_t *cells =
        (int64_t *)((char *)e->ptr + sizeof(alc_vec_t)) + e->vec_win.start;
    if (n > 0 && fwrite(cells, sizeof(int64_t), n, stream) != n)
      return NULL;
  } else { /* VEC_KIND_F64 */
    double *cells = VEC_F64_CELLS(e);
    if (n > 0 && fwrite(cells, sizeof(double), n, stream) != n)
      return NULL;
  }
  return e;
}

/* v1 reader: [u32 len][__DUMP__ × len]. Reconstructs as a GEN vec, then
   calls vec_tighten() to specialise to I64/F64 when the cells are
   homogeneously numeric. Pre-refactor dumps preserve their original
   semantics this way — e.g., mlp's train-y stays integer-comparable
   for `(is label 0)`, while train-X[k] becomes F64 for the tensor-op
   fast path. */
static exp_t *load_vec_v1(FILE *stream) {
  uint32_t n;
  if (fread(&n, sizeof(n), 1, stream) != 1)
    return NULL;
  exp_t *v = make_vector((int64_t)n, NIL_EXP);
  if (!v)
    return NULL;
  for (uint32_t i = 0; i < n; i++) {
    unrefexp(vec_gen_at(v, i));
    exp_t *cell = load_exp_t(stream);
    if (!cell) {
      v->vec_win.end = (int32_t)i;
      unrefexp(v);
      return NULL;
    }
    vec_gen_at(v, i) = cell;
  }
  vec_tighten(v);
  return v;
}

/* v2 reader: [u8 kind][u32 len][payload]. */
static exp_t *load_vec_v2(FILE *stream) {
  uint8_t kind_tag;
  if (fread(&kind_tag, 1, 1, stream) != 1)
    return NULL;
  if (kind_tag > 2)
    return NULL;
  uint32_t n;
  if (fread(&n, sizeof(n), 1, stream) != 1)
    return NULL;
  /* Pre-size storage; we'll overwrite cells per-kind below. */
  alc_vec_t *v = vec_alloc_storage((int64_t)n);
  if (!v)
    return NULL;
  exp_t *e = make_nil();
  e->type = EXP_VECTOR;
  e->flags = (unsigned short)((unsigned)kind_tag << 4);
  e->ptr = v;
  e->vec_win.start = 0;
  e->vec_win.end = (int32_t)n;
  if (kind_tag == 0) { /* GEN */
    exp_t **cells = (exp_t **)((char *)v + sizeof(alc_vec_t));
    /* Pre-init to nil so a partial load leaves the live prefix safe to
       walk during unrefexp(e). */
    for (uint32_t i = 0; i < n; i++)
      cells[i] = refexp(NIL_EXP);
    for (uint32_t i = 0; i < n; i++) {
      unrefexp(cells[i]);
      exp_t *cell = load_exp_t(stream);
      if (!cell) {
        e->vec_win.end = (int32_t)i;
        unrefexp(e);
        return NULL;
      }
      cells[i] = cell;
    }
  } else if (kind_tag == 1) { /* I64 */
    int64_t *cells = (int64_t *)((char *)v + sizeof(alc_vec_t));
    if (n > 0 && fread(cells, sizeof(int64_t), n, stream) != n) {
      e->vec_win.end = 0;
      unrefexp(e);
      return NULL;
    }
  } else { /* F64 */
    double *cells = (double *)((char *)v + sizeof(alc_vec_t));
    if (n > 0 && fread(cells, sizeof(double), n, stream) != n) {
      e->vec_win.end = 0;
      unrefexp(e);
      return NULL;
    }
  }
  return e;
}

exp_t *load_vec(exp_t *e, FILE *stream) {
  if (e)
    unrefexp(e);
  return alcove_load_dump_version >= 2 ? load_vec_v2(stream)
                                       : load_vec_v1(stream);
}
