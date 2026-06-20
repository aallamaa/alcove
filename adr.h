/* adr.h — Adder -> alcove S-expression transpiler, in C.
 *
 * A self-contained port of adr.py. One entry point:
 *
 *     char *als_to_sexpr(const char *src);   // malloc'd; caller frees
 *
 * It turns the whitespace/`:`-block syntax into ordinary alcove
 * s-expression text, which alcove's existing reader then parses. No
 * alcove headers are needed here; this is pure string -> string.
 *
 * Reader rules (see adder-spec.md):
 *   - bare word = symbol; "..." string (escapes verbatim); numbers ride
 *     through verbatim.
 *   - inline (...) are normal lists and nest.
 *   - `name(a b)` == `name (a b)` ; `name()` == `(name)`.
 *   - a line of one atom  -> that value; one list -> as-is;
 *     many forms -> (f f ...).
 *   - a line ending in `:` opens a block; the more-indented lines below
 *     are appended as further elements of that line's list.
 *   - `'x` -> (quote x). `# ...` is a comment (not `#\` , not in str).
 *   - true->t, false->nil; head macro->defmacro, head set->=.
 */
#ifndef ALCOVE_ALS_H
#define ALCOVE_ALS_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- checked allocation ----
   This header is self-contained (no alcove.h), so it can't use the engine's
   xrealloc/graceful_shutdown. OOM in the transpiler is fatal anyway — abort
   rather than leak-then-deref-NULL on a self-assigning `p = realloc(p, n)`. */
static void *als_xmalloc(size_t n) {
  void *p = malloc(n);
  if (!p && n) {
    fputs("adder transpiler: out of memory\n", stderr);
    abort();
  }
  return p;
}
static void *als_xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q && n) {
    fputs("adder transpiler: out of memory\n", stderr);
    abort();
  }
  return q;
}
static void *als_xcalloc(size_t count, size_t size) {
  void *p = calloc(count, size);
  if (!p && count && size) {
    fputs("adder transpiler: out of memory\n", stderr);
    abort();
  }
  return p;
}
static char *als_xstrdup(const char *s) {
  char *p = strdup(s);
  if (!p) {
    fputs("adder transpiler: out of memory\n", stderr);
    abort();
  }
  return p;
}

/* ---- growable byte buffer ---- */
typedef struct {
  char *p;
  size_t len, cap;
} als_buf;

static void als_buf_init(als_buf *b) {
  b->cap = 256;
  b->len = 0;
  b->p = (char *)als_xmalloc(b->cap);
  b->p[0] = 0;
}
static void als_buf_putn(als_buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    while (b->len + n + 1 > b->cap)
      b->cap *= 2;
    b->p = (char *)als_xrealloc(b->p, b->cap);
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = 0;
}
static void als_buf_puts(als_buf *b, const char *s) {
  als_buf_putn(b, s, strlen(s));
}
static void als_buf_putc(als_buf *b, char c) { als_buf_putn(b, &c, 1); }

/* ---- form model: ATOM (raw text) or LIST (children) ---- */
typedef struct als_node {
  int is_list;
  char *atom; /* when !is_list — owned */
  struct als_node **kid;
  int n, cap;
} als_node;

static als_node *als_atom(const char *s, size_t n) {
  als_node *x = (als_node *)als_xcalloc(1, sizeof *x);
  /* alcove-target literal mapping */
  if (n == 4 && !strncmp(s, "true", 4)) {
    x->atom = als_xstrdup("t");
  } else if (n == 5 && !strncmp(s, "false", 5)) {
    x->atom = als_xstrdup("nil");
  } else {
    x->atom = (char *)als_xmalloc(n + 1);
    memcpy(x->atom, s, n);
    x->atom[n] = 0;
  }
  return x;
}
static als_node *als_list(void) {
  als_node *x = (als_node *)als_xcalloc(1, sizeof *x);
  x->is_list = 1;
  return x;
}
static void als_push(als_node *L, als_node *c) {
  if (L->n == L->cap) {
    L->cap = L->cap ? L->cap * 2 : 4;
    L->kid = (als_node **)als_xrealloc(L->kid, L->cap * sizeof *L->kid);
  }
  L->kid[L->n++] = c;
}
static void als_free(als_node *x) {
  if (!x)
    return;
  if (x->is_list) {
    for (int i = 0; i < x->n; i++)
      als_free(x->kid[i]);
    free(x->kid);
  } else
    free(x->atom);
  free(x);
}

/* ---- inline reader: one comment/colon-stripped line -> forms ---- */
typedef struct {
  const char *s;
  size_t i, n;
} als_lr;

static int als_is_delim(char c) {
  return c == ' ' || c == '\t' || c == '(' || c == ')' || c == '"' ||
         c == '\'' || c == '`' || c == ',' || c == '[' || c == ']' ||
         c == '{' || c == '}';
}

static als_node *als_read_one(als_lr *r);

/* read forms until end (term==0) or until `term` char consumed.
   Inside a brace container ({…} map, #{…} set — both close with '}') a comma
   is an entry separator and counts as whitespace; everywhere else ',' stays
   the unquote reader macro, so the quasiquote/unquote meta-syntax is intact. */
static void als_read_forms(als_lr *r, char term, als_node *out) {
  int sep_comma = (term == '}');
  for (;;) {
    while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\t' ||
                           (sep_comma && r->s[r->i] == ',')))
      r->i++;
    if (r->i >= r->n)
      return;
    char c = r->s[r->i];
    if (term && c == term) {
      r->i++;
      return;
    }
    als_node *f = als_read_one(r);
    /* call sugar: a symbol atom immediately followed by '(' */
    if (f && !f->is_list && r->i < r->n && r->s[r->i] == '(') {
      r->i++; /* consume ( */
      als_node *args = als_list();
      als_read_forms(r, ')', args);
      /* ALT2: a name glued to `(...)` is a CALL of ANY arity -> (name args...),
         EXCEPT in a binder context, where `(...)` is a PARAMETER LIST: after a
         name-binder (def/defn/defc/defmacro/macro -> def f(x):), or when `name`
         is itself fn/lambda glued to its own params (fn(x):). */
      als_node *prev = out->n > 0 ? out->kid[out->n - 1] : NULL;
      int binder =
          prev && !prev->is_list && prev->atom &&
          (!strcmp(prev->atom, "def") || !strcmp(prev->atom, "defn") ||
           !strcmp(prev->atom, "defc") || !strcmp(prev->atom, "defmacro") ||
           !strcmp(prev->atom, "macro") || !strcmp(prev->atom, "fn") ||
           !strcmp(prev->atom, "lambda"));
      int self_binder =
          f->atom && (!strcmp(f->atom, "fn") || !strcmp(f->atom, "lambda"));
      if (binder || self_binder) { /* def/fn header: name + param list */
        als_push(out, f);          /* name */
        als_push(out, args);       /* (params...) — may be empty */
      } else {                     /* name(args...) -> (name args...) */
        als_node *call = als_list();
        als_push(call, f);
        for (int i = 0; i < args->n; i++)
          als_push(call, args->kid[i]);
        args->n = 0; /* kids moved into call; free only the shell */
        als_free(args);
        /* Chained call: f(a)(b)(c) -> (((f a) b) c). Each '(' that follows the
           previous ')' WITH NO WHITESPACE re-heads the call with the next arg
           group. A space or newline before '(' breaks the chain (the next
           group is a separate form). Empty groups are fine: f()() -> ((f)). */
        while (r->i < r->n && r->s[r->i] == '(') {
          r->i++; /* consume ( */
          als_node *more = als_list();
          als_read_forms(r, ')', more);
          als_node *outer = als_list();
          als_push(outer, call); /* prior result becomes the head */
          for (int i = 0; i < more->n; i++)
            als_push(outer, more->kid[i]);
          more->n = 0; /* kids moved into outer */
          als_free(more);
          call = outer;
        }
        als_push(out, call);
      }
      continue;
    }
    als_push(out, f);
  }
}

static als_node *als_read_one(als_lr *r) {
  char c = r->s[r->i];
  if (c == '{') {
    /* hash-map literal {k v, k v} -> (hash-map k v k v). This is the form the
       printer emits for a dict; als_read_forms treats the ',' separators as
       whitespace (see its sep_comma note). `#{…}` (set) is handled in the `#`
       dispatch below. */
    r->i++;
    als_node *m = als_list();
    als_push(m, als_atom("hash-map", 8));
    als_read_forms(r, '}', m);
    return m;
  }
  if (c == '(') {
    r->i++;
    als_node *L = als_list();
    als_read_forms(r, ')', L);
    return L;
  }
  /* arc-lambda [body...] -> (fn (_) (body...)). alcove's own reader reads a
     bare `[...]` as a one-argument lambda whose implicit parameter is `_` and
     whose body is the single call form spelled inside the brackets (so
     `[* _ _]` is `(fn (_) (* _ _))`). We lower it here so the indentation
     reader sees ONE form: without this, `[`, the body tokens, and `]` are read
     as separate atoms, and the infix-`=` rule then wraps a multi-token RHS in
     parens — turning `f = [* _ _]` into `(= f ([* _ _]))`, a zero-arg call of
     the lambda. The `#[` vector case below still matches first via the `#`. */
  if (c == '[') {
    r->i++; /* consume [ */
    als_node *body = als_list();
    als_read_forms(r, ']', body); /* the (tok tok ...) call */
    als_node *lam = als_list();
    als_push(lam, als_atom("fn", 2));
    als_node *params = als_list();
    als_push(params, als_atom("_", 1));
    als_push(lam, params); /* (_) */
    als_push(lam, body);   /* (tok tok ...) */
    return lam;
  }
  if (c == '"') { /* string: keep quotes + escapes verbatim */
    size_t start = r->i++;
    while (r->i < r->n) {
      if (r->s[r->i] == '\\') {
        r->i += 2;
        continue;
      }
      if (r->s[r->i] == '"') {
        r->i++;
        break;
      }
      r->i++;
    }
    return als_atom(r->s + start, r->i - start);
  }
  if (c == '\'') {
    r->i++;
    while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\t'))
      r->i++;
    als_node *q = als_list();
    als_push(q, als_atom("quote", 5));
    als_push(q, als_read_one(r));
    return q;
  }
  if (c == '`') {
    r->i++;
    while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\t'))
      r->i++;
    als_node *q = als_list();
    als_push(q, als_atom("quasiquote", 10));
    als_push(q, als_read_one(r));
    return q;
  }
  if (c == ',') {
    r->i++; /* consume ',' */
    int splice = r->i < r->n && r->s[r->i] == '@';
    if (splice)
      r->i++; /* consume '@' */
    while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\t'))
      r->i++;
    als_node *q = als_list();
    als_push(
        q, als_atom(splice ? "unquote-splicing" : "unquote", splice ? 16 : 7));
    als_push(q, als_read_one(r));
    return q;
  }
  /* alcove char literal #\X — emit `#\` plus the full character that
     follows. X may be a multi-byte UTF-8 codepoint (#\é, #\世, #\😀), so
     take the whole sequence rather than a fixed 3 bytes; otherwise the
     trailing continuation bytes leak out as stray tokens. */
  if (c == '#' && r->i + 2 < r->n && r->s[r->i + 1] == '\\') {
    unsigned char lead = (unsigned char)r->s[r->i + 2];
    size_t clen = 1;
    if (lead >= 0xF0)
      clen = 4;
    else if (lead >= 0xE0)
      clen = 3;
    else if (lead >= 0xC0)
      clen = 2;
    if (r->i + 2 + clen > r->n) /* clamp to available bytes */
      clen = r->n - (r->i + 2);
    als_node *a = als_atom(r->s + r->i, 2 + clen);
    r->i += 2 + clen;
    return a;
  }
  /* vector literal #[a b c] -> (vector a b c). alcove's own reader expands
     #[...] the same way; we lower it here so the indentation reader's
     line/atom logic doesn't choke on the brackets. */
  if (c == '#' && r->i + 1 < r->n && r->s[r->i + 1] == '[') {
    r->i += 2; /* consume #[ */
    als_node *vec = als_list();
    als_push(vec, als_atom("vector", 6));
    als_read_forms(r, ']', vec);
    return vec;
  }
  /* set literal #{a b c} -> (hash-set a b c). alcove's reader expands #{...}
     the same way; lowering it here keeps the printed form of a set readable
     in Adder too. */
  if (c == '#' && r->i + 1 < r->n && r->s[r->i + 1] == '{') {
    r->i += 2; /* consume #{ */
    als_node *set = als_list();
    als_push(set, als_atom("hash-set", 8));
    als_read_forms(r, '}', set);
    return set;
  }
  /* blob literal #b"..." -> (string->blob "..."). This is the form the printer
     emits for a printable blob, so a printed blob re-reads in Adder. */
  if (c == '#' && r->i + 2 < r->n && r->s[r->i + 1] == 'b' &&
      r->s[r->i + 2] == '"') {
    r->i += 2;             /* consume `#b`; r->i now at the opening '"' */
    size_t start = r->i++; /* keep the string (with quotes/escapes) verbatim */
    while (r->i < r->n) {
      if (r->s[r->i] == '\\') {
        r->i += 2;
        continue;
      }
      if (r->s[r->i] == '"') {
        r->i++;
        break;
      }
      r->i++;
    }
    als_node *blob = als_list();
    als_push(blob, als_atom("string->blob", 12));
    als_push(blob, als_atom(r->s + start, r->i - start));
    return blob;
  }
  size_t start = r->i;
  while (r->i < r->n && !als_is_delim(r->s[r->i]))
    r->i++;
  if (r->i == start) /* lone delimiter we don't special-case: take 1 */
    r->i++;
  return als_atom(r->s + start, r->i - start);
}

/* one line's text -> the node it denotes (pragmatic lone-atom rule) */
static als_node *als_line_node(const char *text) {
  als_lr r = {text, 0, strlen(text)};
  als_node *forms = als_list();
  als_read_forms(&r, 0, forms);
  if (forms->n == 1) {
    als_node *only = forms->kid[0];
    forms->kid[0] = NULL;
    als_free(forms);
    return only; /* lone atom -> value; lone list -> as-is */
  }
  /* Infix assignment: `lhs = rhs...` -> (= lhs rhs...), Python-style. Fires
     only when `=` is the SECOND form on the line, so the prefix form `= place
     val` (where `=` is first) is untouched. A multi-token RHS is wrapped:
     `a = + b c` -> (= a (+ b c)). */
  if (forms->n >= 3 && !forms->kid[1]->is_list && forms->kid[1]->atom &&
      !strcmp(forms->kid[1]->atom, "=")) {
    als_node *asn = als_list();
    als_push(asn, forms->kid[1]); /* the `=` atom, as the head */
    als_push(asn, forms->kid[0]); /* lhs */
    if (forms->n == 3) {
      als_push(asn, forms->kid[2]); /* single-token RHS */
    } else {
      als_node *rhs = als_list();
      for (int i = 2; i < forms->n; i++)
        als_push(rhs, forms->kid[i]);
      als_push(asn, rhs); /* (rhs...) */
    }
    forms->n = 0; /* all kids moved into asn; free only the forms shell */
    als_free(forms);
    return asn;
  }
  return forms; /* many -> (f f ...) */
}

/* ---- comment / colon handling ---- */

/* copy `line` minus a `#` comment (not `#\`, not inside a string).
   Plain while-loop with explicit index advancement: every read is guarded by
   the loop's `i < n` plus an explicit `i + k < n`, so it's both correct and
   easy for the static analyzer to prove in-bounds. */
static char *als_strip_comment(const char *line) {
  size_t n = strlen(line);
  char *out = (char *)malloc(n + 1);
  size_t o = 0;
  int in_str = 0;
  size_t i = 0;
  while (i < n) {
    char c = line[i];
    if (in_str) {
      out[o++] = c;
      if (c == '\\' && i + 1 < n) {
        out[o++] = line[i + 1]; /* keep the escape pair verbatim */
        i += 2;
        continue;
      }
      if (c == '"')
        in_str = 0;
      i++;
      continue;
    }
    /* Char literal #\X: copy `#\` and the value byte verbatim, so a value of
       `#`, `"`, `;`, or a space isn't mistaken for a comment or a string
       opener. (A multi-byte #\é leaks only continuation bytes 0x80-0xBF,
       never '#'/'"'/';', so copying one byte here is enough.) */
    if (c == '#' && i + 1 < n && line[i + 1] == '\\') {
      out[o++] = '#';
      out[o++] = '\\';
      if (i + 2 < n) {
        out[o++] = line[i + 2];
        i += 3;
      } else {
        i += 2; /* dangling `#\` at end of line — copy what's there */
      }
      continue;
    }
    /* A line comment is `#` followed by a space, tab, or end of line:
       `# like this`. `#!` is also a comment (so `#!/usr/bin/env adder`
       shebang scripts run; the alcove reader has the matching `#!` rule).
       A `#` glued to any other character is a dispatch token (#[ vector,
       #{ set, #b"..." blob) and passes through to the reader untouched.
       This one rule replaces a per-token exception list — see the matching
       rule in adr.py / als_read_one. */
    if (c == '#' && (i + 1 >= n || line[i + 1] == ' ' || line[i + 1] == '\t' ||
                     line[i + 1] == '!'))
      break;
    out[o++] = c;
    if (c == '"')
      in_str = 1;
    i++;
  }
  out[o] = 0;
  return out;
}

/* does the trimmed text open a block? returns 1 and trims the ':' */
static int als_opens_block(char *t) {
  size_t n = strlen(t);
  if (n == 0 || t[n - 1] != ':')
    return 0;
  int in_str = 0;
  for (size_t i = 0; i + 1 < n; i++) {
    if (t[i] == '\\' && in_str) {
      i++;
      continue;
    }
    if (t[i] == '"')
      in_str = !in_str;
  }
  if (in_str)
    return 0;
  t[n - 1] = 0;
  /* rstrip */
  for (size_t i = strlen(t); i > 0 && (t[i - 1] == ' ' || t[i - 1] == '\t');)
    t[--i] = 0;
  return 1;
}

/* Find a standalone inline-block ':' — a colon followed by whitespace, outside
   any string — at which `head: body` splits into a head form and a one-line
   inline body (Pythonic `if cond: stmt`). Returns its index, or -1.
   A ':' glued to a constituent (`:keyword`, `{:.2f}`) has no following space
   and is left alone; a trailing ':' is the block opener handled by
   als_opens_block before this is consulted; a ':' inside a string is skipped.
 */
static int als_inline_colon(const char *t) {
  int in_str = 0;
  for (size_t i = 0; t[i]; i++) {
    if (in_str) {
      if (t[i] == '\\' && t[i + 1])
        i++;
      else if (t[i] == '"')
        in_str = 0;
      continue;
    }
    if (t[i] == '"') {
      in_str = 1;
      continue;
    }
    if (t[i] == ':' && (t[i + 1] == ' ' || t[i + 1] == '\t')) {
      /* require real content after the colon — else it's trailing junk */
      for (size_t j = i + 1; t[j]; j++)
        if (t[j] != ' ' && t[j] != '\t')
          return (int)i;
      return -1;
    }
  }
  return -1;
}

/* head-symbol remap: macro -> defmacro. Assignment is `setf` (the built-in
   alias of `=`), which needs no remap; `set` is intentionally NOT remapped so
   it remains the set constructor, matching Alcove — (set 1 2 3) builds a set.
 */
static void als_head_remap(als_node *node) {
  if (!node->is_list || node->n == 0)
    return;
  als_node *h = node->kid[0];
  if (h->is_list || !h->atom)
    return;
  if (!strcmp(h->atom, "macro")) {
    free(h->atom);
    h->atom = als_xstrdup("defmacro");
  }
}

/* ---- source map: generated s-expr line -> original Adder line ----
   als_to_sexpr emits exactly ONE generated line per top-level form, so the map
   is a flat array: generated line N (1-based) came from Adder line
   map->line[N-1]. Lets an error in transpiled .adr code point at the user's
   real source line. */
typedef struct {
  int *line;
  int n, cap;
} als_map;
static void als_map_push(als_map *m, int adder_line) {
  if (!m)
    return;
  if (m->n == m->cap) {
    m->cap = m->cap ? m->cap * 2 : 16;
    m->line = (int *)als_xrealloc(m->line, (size_t)m->cap * sizeof *m->line);
  }
  m->line[m->n++] = adder_line;
}
static int als_map_lookup(const als_map *m, int gen_line) {
  if (!m || gen_line < 1 || gen_line > m->n)
    return 0;
  return m->line[gen_line - 1];
}
static void als_map_free(als_map *m) {
  if (m) {
    free(m->line);
    m->line = NULL;
    m->n = m->cap = 0;
  }
}

/* ---- serialize node -> s-expression text ---- */
static void als_emit(als_node *x, als_buf *b) {
  if (!x)
    return;
  if (!x->is_list) {
    als_buf_puts(b, x->atom);
    return;
  }
  als_buf_putc(b, '(');
  for (int i = 0; i < x->n; i++) {
    if (i)
      als_buf_putc(b, ' ');
    als_emit(x->kid[i], b);
  }
  als_buf_putc(b, ')');
}

/* Check if trimmed body starts with a keyword (null-terminated kwlen chars). */
static int als_starts_with(const char *s, const char *kw, size_t kwlen) {
  return strncmp(s, kw, kwlen) == 0 &&
         (s[kwlen] == '\0' || s[kwlen] == ' ' || s[kwlen] == '\t');
}

/* ---- top level: src -> s-expr string (+ optional source map) ----
   When `map` is non-NULL it is filled with one entry per emitted top-level
   line, each the 1-based Adder source line that form began on. */
char *als_to_sexpr_mapped(const char *src, als_map *map) {
  /* split into lines (keep leading whitespace for indent calc) */
  size_t slen = strlen(src);
  als_buf out;
  als_buf_init(&out);
  int cur_line = 0; /* 1-based Adder source line of the current iteration */

  /* indentation stack: parent list to append children into */
  enum { MAXD = 256 };
  int ind_stack[MAXD];
  als_node *node_stack[MAXD];
  /* if_stack: tracks the 'if' node at each indent for elif/else attachment */
  als_node *if_stack[MAXD];
  int sp = 0;
  memset(if_stack, 0, sizeof(if_stack));
  als_node *roots = als_list();

  size_t i = 0;
  while (i <= slen) {
    cur_line++; /* at the top so every `continue` still advances the line count
                 */
    size_t j = i;
    while (j < slen && src[j] != '\n')
      j++;
    /* raw line src[i..j) */
    size_t rawlen = j - i;
    char *raw = (char *)malloc(rawlen + 1);
    memcpy(raw, src + i, rawlen);
    raw[rawlen] = 0;
    i = j + 1;

    char *nocom = als_strip_comment(raw);
    free(raw);
    /* indent = leading spaces/tabs of nocom */
    int indent = 0;
    while (nocom[indent] == ' ' || nocom[indent] == '\t')
      indent++;
    /* if_stack is indexed by indentation COLUMN (not nesting depth), so clamp
       the index — pathologically deep indentation (hostile / fuzzed input)
       must not write out of bounds. Real source never nears 256 columns. */
    int iidx = indent < MAXD ? indent : MAXD - 1;
    /* trim both ends into `body` */
    char *body = als_xstrdup(nocom + indent);
    for (size_t k = strlen(body);
         k > 0 &&
         (body[k - 1] == ' ' || body[k - 1] == '\t' || body[k - 1] == '\r');)
      body[--k] = 0;
    free(nocom);
    if (body[0] == 0) { /* blank */
      free(body);
      continue;
    }

    int block = als_opens_block(body);

    /* Detect elif/else blocks — they extend the preceding 'if' node
       at the same indent level rather than creating a new top-level form. */
    int is_else = block && strcmp(body, "else") == 0;
    int is_elif = block && als_starts_with(body, "elif", 4);

    if (is_else || is_elif) {
      /* Pop stack back to the level of the matching if */
      while (sp > 0 && indent <= ind_stack[sp - 1])
        sp--;
      als_node *target = if_stack[iidx];
      if (target) {
        if (is_elif) {
          /* Append the elif condition to the existing if node */
          char *cond_text = body + 4; /* skip "elif" */
          while (*cond_text == ' ' || *cond_text == '\t')
            cond_text++;
          als_node *cond = als_line_node(cond_text);
          als_push(target, cond);
        }
        /* Insert a fresh (do ...) for the branch body */
        als_node *do_node = als_list();
        als_push(do_node, als_atom("do", 2));
        als_push(target, do_node);
        if (sp < MAXD) {
          ind_stack[sp] = indent;
          node_stack[sp] = do_node;
          sp++;
        }
        if (is_else)
          if_stack[iidx] = NULL; /* else terminates the chain */
      }
      free(body);
      continue;
    }

    /* Inline block `head: body` (no trailing ':', so block==0): parse the head
       and the one-line body separately and nest the body inside the head form.
       Splitting (rather than stripping the ':') keeps an unparenthesized body
       grouped — `if c: return y` → (if c (return y)), not (if c return y). */
    int icolon = block ? -1 : als_inline_colon(body);
    als_node *node;
    if (icolon >= 0) {
      const char *body_part = body + icolon + 1;
      while (*body_part == ' ' || *body_part == '\t')
        body_part++;
      body[icolon] = 0; /* terminate the head part at the ':' */
      node = als_line_node(body);
      if (!node->is_list) {
        als_node *L = als_list();
        als_push(L, node);
        node = L;
      }
      als_push(node, als_line_node(body_part));
    } else {
      node = als_line_node(body);
      if (block && !node->is_list) {
        als_node *L = als_list();
        als_push(L, node);
        node = L;
      }
    }
    free(body);
    als_head_remap(node);

    while (sp > 0 && indent <= ind_stack[sp - 1])
      sp--;

    /* Track if/when/unless nodes for subsequent elif/else attachment. */
    if_stack[iidx] = NULL;
    if (block && node->is_list && node->n > 0 && !node->kid[0]->is_list) {
      const char *head = node->kid[0]->atom;
      if (head && (strcmp(head, "if") == 0 || strcmp(head, "when") == 0 ||
                   strcmp(head, "unless") == 0))
        if_stack[iidx] = node;
    }

    if (sp > 0) {
      als_push(node_stack[sp - 1], node);
    } else {
      als_push(roots, node);       /* defer emit until tree is complete */
      als_map_push(map, cur_line); /* this root → its Adder start line */
    }

    if (block && sp < MAXD) {
      ind_stack[sp] = indent;
      node_stack[sp] = node;
      sp++;
    }
  }

  for (int k = 0; k < roots->n; k++) {
    als_emit(roots->kid[k], &out);
    als_buf_putc(&out, '\n');
  }
  als_free(roots);
  return out.p;
}

/* Back-compat entry point: transpile without building a source map. */
char *als_to_sexpr(const char *src) { return als_to_sexpr_mapped(src, NULL); }

#endif /* ALCOVE_ALS_H */
