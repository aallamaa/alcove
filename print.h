/* print.h — print_node: the canonical value printer (REPL echo + (pr ...)),
 * with ANSI coloring and the self-representing forms (#[...] vectors, #b"..."
 * blobs, {k v} dicts, #{...} sets, HAMT via hamt_print, () lists). FRAGMENT
 * #included into alcove.c; reads container fields via the alcove.h types and
 * uses utf8.h (included earlier). The `str`/format buffer renderer
 * (exp_to_string_buf) and the source pretty-printer (pp_*) remain in alcove.c.
 * NOT standalone, NOT separately compiled.
 */
/* Depth guard: a cyclic container ((assoc! d "self" d), a deque pushed into
   itself, ...) used to recurse print_node to a C-stack overflow — the REPL
   merely ECHOING such a value killed the process. Cap the depth and print an
   ellipsis instead; 256 exceeds any real data's nesting yet is only a couple
   hundred stack frames. Same precedent as ALCOVE_DUMPABLE_MAX_DEPTH
   (dict.h). The wrapper owns the counter so every early return in the body
   (print_node_1) still unwinds it correctly. */
#define PRINT_NODE_MAX_DEPTH 256
static ALCOVE_TLS int print_node_depth = 0;
static void print_node_1(exp_t *node);
void print_node(exp_t *node) {
  if (print_node_depth >= PRINT_NODE_MAX_DEPTH) {
    printf("...");
    return;
  }
  print_node_depth++;
  print_node_1(node);
  print_node_depth--;
}
static void print_node_1(exp_t *node) {
  if (node == NULL) {
    printf("nil");
    return;
  }
  /* Tagged immediates — handle before any ->field access. */
  if (isnumber(node)) {
    printf("\x1B[92m%lld\x1B[39m", (long long)FIX_VAL(node));
    return;
  }
  if (ischar(node)) {
    uint32_t c = CHAR_VAL(node);
    if (c >= 0x80) {
      char u[4];
      int k = utf8_encode(c, u);
      printf("#\\");
      fwrite(u, 1, (size_t)k, stdout);
    } else if (c > 32 && c < 127) {
      printf("#\\%c", (char)c);
    } else {
      printf("#\\%u", c);
    }
    return;
  }
  if (!is_ptr(node)) {
    printf("<?imm %p>", (void *)node);
    return;
  }
  if (node->type == EXP_ERROR) {
    printf("\x1B[91mError: \x1B[39m%s\n", (char *)exp_text(node));
  } else if (node->type == EXP_TREE) {
    printf("[ ");
    if (node->content)
      print_node(node->content);
    printf("] ");
  } else if (node->type == EXP_PAIR) {
    if (istrue(node)) {
      printf("(");
      if (node->content)
        print_node(node->content);
      while ((node = node->next)) {
        if ispair (node) {
          printf(" ");
          print_node(node->content);
        } else {
          printf(" . ");
          print_node(node);
          break;
        }
      }
      printf(")");
    } else
      printf("nil");
  } else if (node->type == EXP_LAMBDA) {
    if (node->meta)
      printf("\x1B[92m#<procedure:%s@%08lx>\x1B[39m", (char *)node->meta,
             (long)node);
    else
      printf("\x1B[92m#<procedure@%08lx>\x1B[39m", (long)node);
    /*  if (node->content) print_node(node->content);
        printf(")");*/
  } else if (node->type == EXP_MACRO) {
    if (node->meta)
      printf("\x1B[92m#<macro:%s@%08lx>\x1B[39m", (char *)node->meta,
             (long)node);
    else
      printf("\x1B[92m#<macro@@%08lx>\x1B[39m", (long)node);
    /*  if (node->content) print_node(node->content);
        printf(")");*/
  }

  else if (node->type == EXP_SYMBOL)
    printf("\x1B[92m%s\x1B[39m", (char *)exp_text(node));
  else if (node->type == EXP_STRING)
    printf("\x1B[92m\"%s\"\x1B[39m", (char *)exp_text(node));
  else if (node->type == EXP_FLOAT)
    printf("\x1B[92m%g\x1B[39m", node->f);
  else if (node->type == EXP_RATIONAL) {
    alc_rat_t *r = (alc_rat_t *)node->ptr;
    printf("\x1B[92m%lld/%lld\x1B[39m", (long long)r->num, (long long)r->den);
  } else if (node->type == EXP_DECIMAL) {
    char db[48];
    dec_to_str((alc_dec_t *)node->ptr, db);
    printf("\x1B[92m%sm\x1B[39m", db);
  } else if (node->type == EXP_VECTOR) {
    int64_t n = vec_len(node);
    printf("#[");
    unsigned k = vec_kind(node);
    for (int64_t i = 0; i < n; i++) {
      if (i)
        printf(" ");
      if (k == VEC_KIND_GEN) {
        print_node(vec_gen_at(node, i));
      } else if (k == VEC_KIND_I64) {
        printf("\x1B[92m%lld\x1B[39m", (long long)vec_i64_at(node, i));
      } else { /* VEC_KIND_F64 */
        printf("\x1B[92m%lf\x1B[39m", vec_f64_at(node, i));
      }
    }
    printf("]");
  } else if (node->type == EXP_BLOB) {
    /* Show the content when it's printable text (b"..."), otherwise a
       hexdump-style view with an ASCII column (non-printable -> '.'), like
       the right column of `hexdump -C`. Binary view is capped so a large
       payload doesn't flood the REPL. */
    alc_blob_t *b = (alc_blob_t *)node->ptr;
    size_t n = b ? b->len : 0;
    const unsigned char *p = b ? (const unsigned char *)b->bytes : NULL;
    int printable = (n > 0);
    for (size_t i = 0; i < n; i++)
      if (p[i] < 0x20 || p[i] > 0x7e) {
        printable = 0;
        break;
      }
    if (n == 0) {
      printf("\x1B[92m#<blob 0 bytes>\x1B[39m");
    } else if (printable) {
      /* #b"..." — a dispatch-macro literal (Scheme/EDN style) so the form
         re-reads as the same blob. The `#` prefix keeps it distinct from the
         symbol `b` (a normal constituent). */
      printf("\x1B[92m#b\"");
      for (size_t i = 0; i < n; i++) {
        if (p[i] == '"' || p[i] == '\\')
          putchar('\\');
        putchar(p[i]);
      }
      printf("\"\x1B[39m");
    } else {
      size_t show = n < 64 ? n : 64;
      printf("\x1B[92m#<blob %zu: ", n);
      for (size_t i = 0; i < show; i++)
        printf("%02x ", p[i]);
      if (show < n)
        printf("... ");
      putchar('|');
      for (size_t i = 0; i < show; i++)
        putchar((p[i] >= 0x20 && p[i] <= 0x7e) ? (char)p[i] : '.');
      printf("|>\x1B[39m");
    }
  } else if (node->type == EXP_DICT) {
    /* Clojure-style {k v, k v} so the printed form re-reads as the same
       value. Iteration order is bucket-order, not insertion-order. */
    dict_t *d = (dict_t *)node->ptr;
    printf("{");
    int first = 1;
    if (d) {
      unsigned int i;
      for (i = 0; i < d->ht[0].size; i++) {
        keyval_t *k = d->ht[0].table[i];
        while (k) {
          if (!first)
            printf(", ");
          first = 0;
          if (((char *)k->key)[0] == ':')
            printf("\x1B[92m%s\x1B[39m", (char *)k->key);
          else
            printf("\x1B[92m\"%s\"\x1B[39m", (char *)k->key);
          printf(" ");
          print_node(k->val);
          k = k->next;
        }
      }
    }
    printf("}");
  } else if (node->type == EXP_SET) {
    dict_t *d = (dict_t *)node->ptr;
    printf("#{");
    int first = 1;
    if (d) {
      for (unsigned int i = 0; i < d->ht[0].size; i++) {
        keyval_t *k = d->ht[0].table[i];
        while (k) {
          if (!first)
            printf(" ");
          first = 0;
          print_node(k->val);
          k = k->next;
        }
      }
    }
    printf("}");
  } else if (node->type == EXP_HAMT) {
    void hamt_print(exp_t * m); /* defined with the HAMT ops */
    hamt_print(node);
  } else if (node->type == EXP_CONT) {
    printf("\x1B[92m#<continuation>\x1B[39m");
  } else if (node->type == EXP_WEAK) {
    printf("\x1B[92m#<weak%s>\x1B[39m", node->ptr ? "" : ":cleared");
  } else if (node->type == EXP_LIST) {
    alc_list_t *l = (alc_list_t *)node->ptr;
    printf("(");
    if (l) {
      alc_listnode_t *n = l->head;
      int first = 1;
      while (n) {
        if (!first)
          printf(" ");
        first = 0;
        print_node(n->val);
        n = n->next;
      }
    }
    printf(")");
  } else if (node->type == EXP_INTERNAL) {
    printf("\x1B[92m#<builtin>\x1B[39m");
  } else if (node->type == EXO_MACROINTERNAL) {
    printf("\x1B[92m#<macro-builtin>\x1B[39m");
  } else if (node->type == EXP_FFI) {
    printf("\x1B[92m#<ffi>\x1B[39m");
  } else if (node->type == EXP_PORT) {
    alc_port_t *p = (alc_port_t *)node->ptr;
    printf("#<port %s %c%s>", p && p->path ? p->path : "?",
           p ? p->mode : '?', p && p->closed ? " closed" : "");
  } else if (node->type >= EXP_MAXSIZE && node->type < ALCOVE_TYPE_CAP &&
             exp_tfuncList[node->type]) {
    /* Custom (foreign) module type: use its print hook, else a default
       #<name@ptr> from the registered type name. */
    if (exp_tfuncList[node->type]->print)
      exp_tfuncList[node->type]->print(node);
    else
      printf("\x1B[92m#<%s@%p>\x1B[39m",
             g_custom_types[node->type].name ? g_custom_types[node->type].name
                                             : "foreign",
             node->ptr);
  } else {
    printf("\x1B[92m#<type %d>\x1B[39m", node->type);
  }
  return;
}
