/* adr.h — Adder -> alcove S-expression transpiler, in C.
 *
 * Originally ported from a Python reader (adr.py, removed 2026-08 — it was
 * built and tested by nothing and had silently fallen behind this file).
 * One entry point:
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

#include "match.h"

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
  return match(c, ' ', '\t', '(', ')', '"', '\'', '`', ',', '[', ']', '{', '}');
}

static als_node *als_read_one(als_lr *r);

/* ---- Adder attribute (dot) sugar ----------------------------------------
   A '.' BETWEEN identifier characters inside one token is attribute syntax:
     read:   a.owner        -> (a "owner")            a.b.c -> ((a "b") "c")
     write:  a.owner = v     -> (assoc! a "owner" v)   (in als_line_node)
     method: a.speak(x y)    -> (speak a x y)          (in als_read_forms)
   A standalone `.` (dotted pair), a leading/trailing dot, an empty segment
   (a..b), a digit-adjacent dot (floats: 1.5, 100.0m, -3.14), or a token that
   starts with ':' (keyword), '"' (string), '#' (dispatch) or a digit all leave
   the token untouched — Adder-only sugar, the Alcove reader is unchanged. */
static int als_is_attr_token(const char *s, size_t n) {
  if (n < 3)
    return 0;
  ifmatch (s[0], ':', '.', '"', '#')
    return 0; /* keyword / leading dot / string / dispatch token */
  if (s[0] >= '0' && s[0] <= '9')
    return 0; /* a number-leading token is never attribute access */
  if (s[n - 1] == '.')
    return 0; /* trailing dot */
  int dots = 0;
  for (size_t k = 0; k < n; k++) {
    if (s[k] != '.')
      continue;
    char pv = s[k - 1], nx = s[k + 1]; /* k is interior (0 < k < n-1) here */
    if (pv == '.' || nx == '.')
      return 0; /* empty segment a..b */
    if ((pv >= '0' && pv <= '9') || (nx >= '0' && nx <= '9'))
      return 0; /* digit-adjacent -> float, not attribute */
    dots++;
  }
  return dots > 0;
}

/* Split a dotted token into its '.'-separated segments. Fills seg[]/seglen[]
   for up to `maxseg` segments and returns the total segment count. */
static int als_dot_segments(const char *s, size_t n, const char **seg,
                            size_t *seglen, int maxseg) {
  int c = 0;
  size_t start = 0;
  for (size_t k = 0; k <= n; k++) {
    if (k == n || s[k] == '.') {
      if (c < maxseg) {
        seg[c] = s + start;
        seglen[c] = k - start;
      }
      c++;
      start = k + 1;
    }
  }
  return c;
}

/* Build a string-literal atom node holding "seg" (with the quotes). */
static als_node *als_str_atom(const char *s, size_t n) {
  als_node *a = (als_node *)als_xcalloc(1, sizeof *a);
  a->atom = (char *)als_xmalloc(n + 3);
  a->atom[0] = '"';
  memcpy(a->atom + 1, s, n);
  a->atom[n + 1] = '"';
  a->atom[n + 2] = 0;
  return a;
}

/* Build the attribute READ-chain over the first `count` segments:
   [a] -> a ; [a,b] -> (a "b") ; [a,b,c] -> ((a "b") "c"). */
static als_node *als_read_chain(const char **seg, const size_t *seglen,
                                int count) {
  als_node *node = als_atom(seg[0], seglen[0]);
  for (int k = 1; k < count; k++) {
    als_node *L = als_list();
    als_push(L, node);
    als_push(L, als_str_atom(seg[k], seglen[k]));
    node = L;
  }
  return node;
}

#define ALS_MAXSEG 64

/* If `a` (a non-list atom) is an attribute token, replace it with its READ
   chain (freeing `a`); otherwise return it unchanged. */
static als_node *als_desugar_atom(als_node *a) {
  if (a->is_list || !a->atom)
    return a;
  size_t n = strlen(a->atom);
  if (!als_is_attr_token(a->atom, n))
    return a;
  const char *seg[ALS_MAXSEG];
  size_t seglen[ALS_MAXSEG];
  int c = als_dot_segments(a->atom, n, seg, seglen, ALS_MAXSEG);
  if (c < 2 || c > ALS_MAXSEG)
    return a; /* pathological — leave as a plain atom */
  als_node *chain = als_read_chain(seg, seglen, c);
  als_free(a);
  return chain;
}

/* Recursively desugar bare attribute-read atoms in a form tree, in place.
   Method calls (glued paren) and assignment writes (assoc!) are lowered
   earlier by the caller; this pass only rewrites the remaining bare reads. */
static void als_desugar_dots(als_node *node) {
  if (!node || !node->is_list)
    return;
  for (int i = 0; i < node->n; i++) {
    if (node->kid[i]->is_list)
      als_desugar_dots(node->kid[i]);
    else
      node->kid[i] = als_desugar_atom(node->kid[i]);
  }
}

/* Top-level entry: desugar a node that may itself be a bare atom. */
static als_node *als_desugar_node(als_node *node) {
  if (!node)
    return node;
  if (node->is_list) {
    als_desugar_dots(node);
    return node;
  }
  return als_desugar_atom(node);
}

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
      /* Adder attribute method call: a.speak(x y) -> (speak a x y). The LAST
         dotted segment is the generic-function name; the earlier segments are
         the receiver read-chain (a.b.speak(x) -> (speak (a "b") x)). A dotted
         token NOT glued to '(' is a field read, handled by the desugar walk. */
      {
        const char *seg[ALS_MAXSEG];
        size_t seglen[ALS_MAXSEG];
        size_t flen = f->atom ? strlen(f->atom) : 0;
        int c = (flen && als_is_attr_token(f->atom, flen))
                    ? als_dot_segments(f->atom, flen, seg, seglen, ALS_MAXSEG)
                    : 0;
        if (c >= 2 && c <= ALS_MAXSEG) {
          als_node *call = als_list();
          als_push(call, als_atom(seg[c - 1], seglen[c - 1])); /* generic fn */
          als_push(call, als_read_chain(seg, seglen, c - 1));  /* receiver */
          for (int k = 0; k < args->n; k++)
            als_push(call, args->kid[k]);
          args->n = 0; /* kids moved into call */
          als_free(args);
          als_free(f);
          /* trailing chained call groups a.speak(x)(y), mirroring plain path */
          while (r->i < r->n && r->s[r->i] == '(') {
            r->i++;
            als_node *more = als_list();
            als_read_forms(r, ')', more);
            als_node *outer = als_list();
            als_push(outer, call);
            for (int k = 0; k < more->n; k++)
              als_push(outer, more->kid[k]);
            more->n = 0;
            als_free(more);
            call = outer;
          }
          als_push(out, call);
          continue;
        }
      }
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
        r->i = (r->i + 1 < r->n) ? r->i + 2 : r->i + 1;
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
  /* comprehension sugar: #l[..]/#g[..] -> (lfor..)/(gfor..) and
     #s{..}/#d{..} -> (sfor..)/(dfor..). alcove's own reader expands these
     identically; we lower them here so the body isn't misread by the
     indentation reader (a bare [..] is an arc-lambda and {..} is a
     hash-map, so the dispatch prefix must be consumed as one form). The
     opener is fixed per letter: [ for the sequence-shaped l/g, { for the
     collection-shaped s/d. */
  if (c == '#' && r->i + 2 < r->n) {
    char d = r->s[r->i + 1], op = r->s[r->i + 2];
    if ((d == 'l' || d == 'g') && op == '[') {
      r->i += 3; /* consume #l[ / #g[ */
      als_node *comp = als_list();
      als_push(comp, als_atom((d == 'l') ? "lfor" : "gfor", 4));
      als_read_forms(r, ']', comp);
      return comp;
    }
    if ((d == 's' || d == 'd') && op == '{') {
      r->i += 3; /* consume #s{ / #d{ */
      als_node *comp = als_list();
      als_push(comp, als_atom((d == 's') ? "sfor" : "dfor", 4));
      als_read_forms(r, '}', comp);
      return comp;
    }
  }
  /* blob literal #b"..." -> (string->blob "..."). This is the form the printer
     emits for a printable blob, so a printed blob re-reads in Adder. */
  if (c == '#' && r->i + 2 < r->n && r->s[r->i + 1] == 'b' &&
      r->s[r->i + 2] == '"') {
    r->i += 2;             /* consume `#b`; r->i now at the opening '"' */
    size_t start = r->i++; /* keep the string (with quotes/escapes) verbatim */
    while (r->i < r->n) {
      if (r->s[r->i] == '\\') {
        r->i = (r->i + 1 < r->n) ? r->i + 2 : r->i + 1;
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
    return als_desugar_node(only); /* lone atom -> value; lone list -> as-is */
  }
  /* Infix assignment: `lhs = rhs...` -> (= lhs rhs...), Python-style. Fires
     only when `=` is the SECOND form on the line, so the prefix form `= place
     val` (where `=` is first) is untouched. A multi-token RHS is wrapped:
     `a = + b c` -> (= a (+ b c)). A dotted LHS is an attribute WRITE:
     `a.owner = v` -> (assoc! a "owner" v); `a.b.c = v` -> (assoc! (a "b") "c"
     v). */
  if (forms->n >= 3 && !forms->kid[1]->is_list && forms->kid[1]->atom &&
      !strcmp(forms->kid[1]->atom, "=")) {
    /* build the RHS first (single token, or a wrapped (rhs...) list) */
    als_node *rhs;
    if (forms->n == 3) {
      rhs = forms->kid[2]; /* single-token RHS */
    } else {
      rhs = als_list();
      for (int i = 2; i < forms->n; i++)
        als_push(rhs, forms->kid[i]); /* (rhs...) */
    }
    als_node *lhs = forms->kid[0];
    const char *seg[ALS_MAXSEG];
    size_t seglen[ALS_MAXSEG];
    size_t llen = (!lhs->is_list && lhs->atom) ? strlen(lhs->atom) : 0;
    int c = (llen && als_is_attr_token(lhs->atom, llen))
                ? als_dot_segments(lhs->atom, llen, seg, seglen, ALS_MAXSEG)
                : 0;
    als_node *asn = als_list();
    if (c >= 2 && c <= ALS_MAXSEG) { /* attribute write via assoc! */
      als_push(asn, als_atom("assoc!", 6));
      als_push(asn, als_read_chain(seg, seglen, c - 1)); /* receiver chain */
      als_push(asn, als_str_atom(seg[c - 1], seglen[c - 1])); /* "field" */
      als_push(asn, rhs);
      als_free(lhs);                /* kid[0] consumed */
      als_free(forms->kid[1]);      /* the unused `=` atom */
    } else {                        /* plain infix assignment -> (= lhs rhs) */
      als_push(asn, forms->kid[1]); /* the `=` atom, as the head */
      als_push(asn, lhs);           /* lhs */
      als_push(asn, rhs);
    }
    forms->n = 0; /* remaining kids moved into asn/rhs; free the shell only */
    als_free(forms);
    return als_desugar_node(asn);
  }
  return als_desugar_node(forms); /* many -> (f f ...) */
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
    /* A bare `;` (outside a string; a `#\;` char literal was handled above) is
       the *Alcove* line-comment char. Adder's own comment char is `#`, but a
       `;` must not pass through into the transpiled s-expr: there it would
       start a comment that swallows the rest of the line — including a closing
       paren — leaving the form unterminated, so the reader runs to EOF and
       silently drops the whole form (and any following input up to a `)`).
       Treat it as a comment here too, matching the alcove reader's
       unconditional `;` rule. */
    if (c == ';')
      break;
    /* A line comment is `#` followed by a space, tab, or end of line:
       `# like this`. `#!` is also a comment (so `#!/usr/bin/env adder`
       shebang scripts run; the alcove reader has the matching `#!` rule).
       A `#` glued to any other character is a dispatch token (#[ vector,
       #{ set, #b"..." blob) and passes through to the reader untouched.
       This one rule replaces a per-token exception list — see the matching
       rule in als_read_one. */
    if (c == '#' && (i + 1 >= n || match(line[i + 1], ' ', '\t', '!')))
      break;
    out[o++] = c;
    if (c == '"')
      in_str = 1;
    i++;
  }
  out[o] = 0;
  return out;
}

/* Net bracket depth of one COMMENT-STRIPPED line: >0 = brackets left open,
   <0 = a closer with no opener. All three pairs count, since `(f 1`, `#[1 2`
   and `{:a 1` are equally unfinished. Strings and `#\X` char literals are
   skipped — the value byte of `#\(` is not a bracket. Input has already been
   through als_strip_comment, so there are no comments left to skip.

   *in_str_io carries the string state IN and OUT, because a string literal
   may span physical lines: test.alc has assert names that do. Such a line
   looks wildly unbalanced on its own (the brackets are text inside the
   string), so the caller must not join it or judge its depth. */
static int als_bracket_depth(const char *t, int *in_str_io) {
  int d = 0, in_str = *in_str_io;
  for (size_t k = 0; t[k];) {
    char c = t[k];
    if (in_str) {
      if (c == '\\' && t[k + 1])
        k += 2;
      else {
        if (c == '"')
          in_str = 0;
        k++;
      }
      continue;
    }
    if (c == '#' && t[k + 1] == '\\') {
      k += t[k + 2] ? 3 : 2;
      continue;
    }
    if (c == '"')
      in_str = 1;
    else ifmatch (c, '(', '[', '{')
      d++;
    else ifmatch (c, ')', ']', '}')
      d--;
    k++;
  }
  *in_str_io = in_str;
  return d;
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
  for (size_t i = strlen(t); i > 0 && match(t[i - 1], ' ', '\t');)
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
    if (t[i] == ':' && match(t[i + 1], ' ', '\t')) {
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
  /* First syntax error the TRANSPILER itself diagnosed (0 = none). These are
     mistakes the alcove reader can never see, because the text we emit for
     them — a (raise 'syntax-error ...) — is perfectly valid s-expr syntax.
     Without this channel `check-syntax` and the LSP would call a stray `]`
     clean and only the runtime would complain. */
  int err_line;
  char *err_msg; /* owned; freed by als_map_free */
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
/* Record the FIRST transpiler-diagnosed syntax error; later ones are noise. */
static void als_map_err(als_map *m, int line, const char *msg) {
  if (!m || m->err_line)
    return;
  m->err_line = line;
  m->err_msg = als_xstrdup(msg);
}
static int als_map_lookup(const als_map *m, int gen_line) {
  if (!m || gen_line < 1 || gen_line > m->n)
    return 0;
  return m->line[gen_line - 1];
}
static void als_map_free(als_map *m) {
  if (m) {
    free(m->line);
    free(m->err_msg);
    m->err_msg = NULL;
    m->err_line = 0;
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
  return strncmp(s, kw, kwlen) == 0 && match(s[kwlen], '\0', ' ', '\t');
}

/* Emit `(raise 'syntax-error "adder line N: msg")` as a top-level form AND
   record it in the map. Two channels on purpose: the raise is what a plain
   run reports (loudly, with a source-mapped caret), the map entry is what
   check-syntax / the LSP read — they parse the GENERATED text, in which the
   raise is perfectly valid syntax and would otherwise look clean. */
static void als_syntax_error(als_node *roots, als_map *map, int line,
                             const char *what) {
  char msg[192], quoted[196];
  snprintf(msg, sizeof msg, "adder line %d: %s", line, what);
  snprintf(quoted, sizeof quoted, "\"%s\"", msg);
  als_node *err = als_list();
  als_push(err, als_atom("raise", 5));
  als_push(err, als_atom("'syntax-error", 13));
  als_push(err, als_atom(quoted, strlen(quoted)));
  als_push(roots, err);
  als_map_push(map, line);
  als_map_err(map, line, msg);
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
  /* Branch bodies of a block `if` are wrapped in a synthesized (do ...) so a
     multi-statement arm stays ONE arm (alcove's `if` is Arc-style multi-arg).
     A one-statement arm needs no wrapper, but we only know the count once the
     block is closed — so record every wrapper and collapse the singletons at
     the end. Keeps (if c (return x)) exact instead of (if c (do (return x))).
   */
  struct als_wrap {
    als_node *parent;
    int idx;
  } *wraps = NULL;
  int nwrap = 0, cwrap = 0;
  /* Indentation character this source committed to (' ' or '\t'), and whether
     we have already reported a mix — see the check in the line loop. */
  char indent_style = 0;
  int mix_reported = 0;
#define ALS_NOTE_WRAP(p, i)                                                    \
  do {                                                                         \
    if (nwrap == cwrap) {                                                      \
      cwrap = cwrap ? cwrap * 2 : 16;                                          \
      wraps = (struct als_wrap *)als_xrealloc(wraps, cwrap * sizeof *wraps);   \
    }                                                                          \
    wraps[nwrap].parent = (p);                                                 \
    wraps[nwrap].idx = (i);                                                    \
    nwrap++;                                                                   \
  } while (0)

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
    int line_start = cur_line; /* this LOGICAL line's first physical line */

    /* ---- implicit line joining ----------------------------------------
       A line that ends with brackets still open continues onto the next
       physical line, exactly as inside `(`/`[`/`{` in Python. Without this
       the transpiler emitted one form per physical line, so `prn (list 1`
       / `2)` produced `(prn (list 1))` and `(2 ))` — two malformed roots,
       silently. Continuation lines contribute no indentation: the logical
       line's block level is the FIRST line's, and the joined text is what
       the block/indent engine below sees. */
    int extra_lines = 0, depth = 0, str_after = 0;
    for (;;) {
      char *probe = als_strip_comment(raw);
      str_after = 0;
      depth = als_bracket_depth(probe, &str_after);
      free(probe);
      if (i > slen) /* no more input to pull in */
        break;
      if (!str_after && depth <= 0) /* logical line is complete */
        break;
      size_t j2 = i;
      while (j2 < slen && src[j2] != '\n')
        j2++;
      size_t s2 = i;
      /* Inside a string literal the newline and the next line's leading
         whitespace are string CONTENT: join with '\n' and keep every byte.
         Outside one, the continuation contributes no indentation, so drop
         it and join with a space. Getting this backwards silently rewrites
         the user's string. */
      if (!str_after)
        while (s2 < j2 && (src[s2] == ' ' || src[s2] == '\t'))
          s2++;
      size_t add = j2 - s2, rl = strlen(raw);
      raw = (char *)als_xrealloc(raw, rl + add + 2);
      raw[rl] = str_after ? '\n' : ' ';
      memcpy(raw + rl + 1, src + s2, add);
      raw[rl + add + 1] = 0;
      i = j2 + 1;
      extra_lines++;
    }
    cur_line += extra_lines;

    /* Whatever bracket depth survives the join is a real error: >0 means the
       source ran out while a bracket was open, <0 a closer with no opener.
       Both used to fall through and emit malformed s-expr text, which the
       alcove reader then rejected somewhere else entirely ("call to macro
       char ) unknown!"), pointing at the wrong line. */
    {
      if (str_after) { /* ran out of input mid-string */
        als_syntax_error(roots, map, line_start,
                         "unterminated string literal — reached end of input");
        free(raw);
        continue;
      }
      int d = depth;
      if (d != 0) {
        als_syntax_error(roots, map, line_start,
                         d > 0 ? "unterminated ( [ or { — reached end of input"
                               : "unexpected ) ] or } with no matching opener");
        free(raw);
        continue;
      }
    }

    char *nocom = als_strip_comment(raw);
    free(raw);
    /* indent = leading spaces/tabs of nocom */
    int indent = 0;
    while (nocom[indent] == ' ' || nocom[indent] == '\t')
      indent++;
    /* Indentation is COLUMN-counted, one column per byte, so a tab is worth
       exactly 1 — the same as a space. That is self-consistent only while a
       file sticks to one character: mix them and a tab-indented line scores
       BELOW a space-indented one at the same visual depth, silently popping
       the block stack and re-parenting the line. Reject the mix (Python's
       TabError, same reasoning) rather than pick a tab width — any width we
       chose would still disagree with somebody's editor. */
    /* Blank/whitespace-only lines carry no indentation: skip them, or a file
       indented with spaces trips on one stray tab-only line. */
    int blank_line = 1;
    for (int k = indent; nocom[k]; k++)
      if (!match(nocom[k], ' ', '\t', '\r')) {
        blank_line = 0;
        break;
      }
    if (indent > 0 && !blank_line && !mix_reported) {
      int has_sp = 0, has_tab = 0;
      for (int k = 0; k < indent; k++) {
        if (nocom[k] == '\t')
          has_tab = 1;
        else
          has_sp = 1;
      }
      if (!indent_style && !(has_sp && has_tab))
        indent_style = has_tab ? '\t' : ' ';
      if ((has_sp && has_tab) ||
          (indent_style && (indent_style == ' ' ? has_tab : has_sp))) {
        als_syntax_error(roots, map, line_start,
                         "indentation mixes tabs and spaces; pick one");
        mix_reported = 1; /* one diagnostic per transpile, not one per line */
      }
    }
    /* if_stack is indexed by indentation COLUMN (not nesting depth), so clamp
       the index — pathologically deep indentation (hostile / fuzzed input)
       must not write out of bounds. Real source never nears 256 columns. */
    int iidx = indent < MAXD ? indent : MAXD - 1;
    /* trim both ends into `body` */
    char *body = als_xstrdup(nocom + indent);
    for (size_t k = strlen(body);
         k > 0 && (match(body[k - 1], ' ', '\t', '\r'));)
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
        ALS_NOTE_WRAP(target, target->n - 1);
        if (sp < MAXD) {
          ind_stack[sp] = indent;
          node_stack[sp] = do_node;
          sp++;
        }
        if (is_else)
          if_stack[iidx] = NULL; /* else terminates the chain */
      } else {
        /* No `if` opens a block at this column. Silently dropping the clause
           would be the worst outcome: the arm's body is MORE indented, so it
           would attach to whatever block is open and run as part of the THEN
           branch — the exact wrong-branch bug this syntax is meant to avoid.
           Emit a raise so the mistake is loud instead. (The arm's body still
           parses into the enclosing block — a top-level error is reported and
           execution continues — but the diagnostic names the line and column
           problem, and the process exits non-zero.) */
        char what[96];
        snprintf(what, sizeof what,
                 "`%s:` has no matching `if` at this indentation",
                 is_else ? "else" : "elif");
        als_syntax_error(roots, map, line_start, what);
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
    /* Where an indented block body gets appended. Normally the node itself
       (when/unless/def/... all take an implicit body sequence), but `if` does
       NOT: alcove's `if` is Arc-style multi-arg, so a two-statement then-body
       would silently become (if c THEN ELSE). Give block `if` its own (do ...)
       so the branch is a statement sequence, exactly like the else branch. */
    als_node *body_target = node;
    if (block && node->is_list && node->n > 0 && !node->kid[0]->is_list) {
      const char *head = node->kid[0]->atom;
      if (head && (strcmp(head, "if") == 0 || strcmp(head, "when") == 0 ||
                   strcmp(head, "unless") == 0))
        if_stack[iidx] = node;
      if (head && strcmp(head, "if") == 0) {
        body_target = als_list();
        als_push(body_target, als_atom("do", 2));
        als_push(node, body_target);
        ALS_NOTE_WRAP(node, node->n - 1);
      }
    }

    if (sp > 0) {
      als_push(node_stack[sp - 1], node);
    } else {
      als_push(roots, node);         /* defer emit until tree is complete */
      als_map_push(map, line_start); /* this root → its Adder start line */
    }

    if (block && sp < MAXD) {
      ind_stack[sp] = indent;
      node_stack[sp] = body_target;
      sp++;
    }
  }

  /* Collapse (do X) -> X for the synthesized branch wrappers. An empty
     wrapper (a `pass`-less arm) is left alone: (do) is a valid nil. */
  for (int k = 0; k < nwrap; k++) {
    als_node *p = wraps[k].parent, *d = p->kid[wraps[k].idx];
    if (d->is_list && d->n == 2) {
      p->kid[wraps[k].idx] = d->kid[1];
      als_free(d->kid[0]); /* the "do" atom */
      free(d->kid);
      free(d);
    }
  }
  free(wraps);
#undef ALS_NOTE_WRAP

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
