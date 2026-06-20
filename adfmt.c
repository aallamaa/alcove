/* adfmt.c — `adder fmt`: a faithful, comment-preserving formatter for Adder.
 *
 * Unlike adr.h (which LOWERS Adder into Alcove s-exprs: true->t, f(x)->(f x),
 * a = b -> (= a b), [..]->(fn..)), this reads Adder into a SURFACE-PRESERVING
 * tree — every token kept verbatim — then re-emits it in a canonical layout:
 *   - definitions:  def name(params): <body>   (name glued to params, colon)
 *   - calls:        name(args)                  (no space before '(')
 *   - special forms with a body (if/when/for/let/while/...): head ...:  + block
 *   - short forms stay INLINE; only forms large enough to read better when
 *     broken are split across indented lines (the indentation feature).
 *   - line comments (#...) and blank-line grouping are preserved.
 *
 * Correctness is gated externally: transpiling the original and the formatted
 * source through adr.h must yield identical s-exprs (layout-only change), and
 * the formatter must be idempotent.
 *
 * Standalone build: cc -O2 -o adfmt adfmt.c
 * (Phase 3 wires the same engine into the `adder fmt` subcommand.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ alloc */
static void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) { fputs("adfmt: out of memory\n", stderr); exit(2); }
  return p;
}
static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) { fputs("adfmt: out of memory\n", stderr); exit(2); }
  return q;
}
static char *xstrndup(const char *s, size_t n) {
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

/* ------------------------------------------------------------- byte buffer */
typedef struct { char *p; size_t len, cap; } buf;
static void buf_init(buf *b) { b->cap = 256; b->len = 0; b->p = xmalloc(b->cap); b->p[0] = 0; }
static void buf_putn(buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) { while (b->len + n + 1 > b->cap) b->cap *= 2; b->p = xrealloc(b->p, b->cap); }
  memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0;
}
static void buf_puts(buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
static void buf_putc(buf *b, char c) { buf_putn(b, &c, 1); }

/* --------------------------------------------------------------- CST nodes
 * kind: ATOM (verbatim token) or a bracketed group. The group's `open`
 * remembers the surface bracket: '(' paren, '[' arc-lambda, plus flags for
 * #[ (vec), { (map), #{ (set). `call` marks a group that was glued to the
 * previous name (f(args)); the printer also re-sugars plain (f a b) calls.
 * `block` marks a form whose body came from / should use the `:` indentation.
 * lead/trail carry comments; `blank` = a blank line preceded this form. */
typedef struct node {
  int is_list;
  char *tok;            /* atom: verbatim text (owned) */
  char open;            /* list: '(' or '[' */
  int vec, set, map;    /* #[..], #{..}, {..} */
  int call;             /* glued call group f(args) */
  struct node **kid; int n, cap;
  char *lead;           /* full-line comment(s) above (owned, may be NULL) */
  char *trail;          /* trailing # comment (owned, may be NULL) */
  int blank;            /* blank line(s) immediately before */
  int block;            /* indentation block (had ':' + indented children) */
  int hdr_n;            /* for a block: # of head-line kids (rest = body) */
} node;

static node *mk_atom(const char *s, size_t n) {
  node *x = xmalloc(sizeof *x); memset(x, 0, sizeof *x);
  x->tok = xstrndup(s, n); return x;
}
static node *mk_list(char open) {
  node *x = xmalloc(sizeof *x); memset(x, 0, sizeof *x);
  x->is_list = 1; x->open = open; return x;
}
static void push(node *L, node *c) {
  if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 4; L->kid = xrealloc(L->kid, L->cap * sizeof *L->kid); }
  L->kid[L->n++] = c;
}
static void freenode(node *x) {
  if (!x) return;
  for (int i = 0; i < x->n; i++) freenode(x->kid[i]);
  free(x->kid); free(x->tok); free(x->lead); free(x->trail); free(x);
}

/* ------------------------------------------------------- inline tokenizer
 * One line's code text -> a flat list of surface forms. Brackets nest; every
 * token is kept verbatim (no true->t, no infix/call lowering). Call sugar
 * f(args) is recorded as a `call` group so the printer reproduces it. */
typedef struct { const char *s; size_t i, n; } lex;

static int is_delim(char c) {
  return c == ' ' || c == '\t' || c == '(' || c == ')' || c == '[' ||
         c == ']' || c == '{' || c == '}' || c == '"' || c == '\'' ||
         c == '`' || c == ',';
}
static node *read_one(lex *r);
static void read_forms(lex *r, char term, node *out);

/* read a "..." string token verbatim (quotes + escapes). r->i at the '"'. */
static node *read_string(lex *r) {
  size_t start = r->i++;
  while (r->i < r->n) {
    if (r->s[r->i] == '\\') { r->i += 2; continue; }
    if (r->s[r->i] == '"') { r->i++; break; }
    r->i++;
  }
  return mk_atom(r->s + start, r->i - start);
}

static node *read_one(lex *r) {
  char c = r->s[r->i];
  if (c == '(') { r->i++; node *L = mk_list('('); read_forms(r, ')', L); return L; }
  if (c == '[') { r->i++; node *L = mk_list('['); read_forms(r, ']', L); return L; }
  if (c == '{') { r->i++; node *L = mk_list('{'); L->map = 1; read_forms(r, '}', L); return L; }
  if (c == '"') return read_string(r);
  /* #[ vec, #{ set, #\ char, #b" blob, # word */
  if (c == '#' && r->i + 1 < r->n) {
    char d = r->s[r->i + 1];
    if (d == '[') { r->i += 2; node *L = mk_list('['); L->vec = 1; read_forms(r, ']', L); return L; }
    if (d == '{') { r->i += 2; node *L = mk_list('{'); L->set = 1; read_forms(r, '}', L); return L; }
    if (d == '\\' && r->i + 2 < r->n) { /* #\X — keep whole UTF-8 codepoint */
      unsigned char lead = (unsigned char)r->s[r->i + 2];
      size_t clen = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
      if (r->i + 2 + clen > r->n) clen = r->n - (r->i + 2);
      node *a = mk_atom(r->s + r->i, 2 + clen); r->i += 2 + clen; return a;
    }
    if (d == 'b' && r->i + 2 < r->n && r->s[r->i + 2] == '"') {
      size_t start = r->i; r->i += 2; /* keep #b, then the string verbatim */
      r->i++; /* opening quote */
      while (r->i < r->n) { if (r->s[r->i] == '\\') { r->i += 2; continue; } if (r->s[r->i] == '"') { r->i++; break; } r->i++; }
      return mk_atom(r->s + start, r->i - start);
    }
  }
  /* quote / quasiquote / unquote: keep the prefix glued to the next form */
  if (c == '\'' || c == '`' || c == ',') {
    size_t start = r->i++;
    if (c == ',' && r->i < r->n && r->s[r->i] == '@') r->i++;
    while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\t')) r->i++;
    /* wrap: a 1-kid list with a synthetic prefix atom so the printer reglues */
    node *q = mk_list('\''); q->call = 0;
    q->tok = xstrndup(r->s + start, (c == ',' && r->s[start + 1] == '@') ? 2 : 1);
    push(q, read_one(r));
    return q;
  }
  /* bare token up to a delimiter */
  size_t start = r->i;
  while (r->i < r->n && !is_delim(r->s[r->i])) r->i++;
  if (r->i == start) r->i++; /* lone delimiter we don't special-case */
  return mk_atom(r->s + start, r->i - start);
}

static void read_forms(lex *r, char term, node *out) {
  int sep_comma = (term == '}'); /* commas separate map/set entries */
  for (;;) {
    while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\t' ||
                           (sep_comma && r->s[r->i] == ',')))
      r->i++;
    if (r->i >= r->n) return;
    char c = r->s[r->i];
    if (term && c == term) { r->i++; return; }
    node *f = read_one(r);
    /* name/result glued to '(' : a CALL `f(args)`. fn(params)/def-headers are
       also call groups here — the printer renders the glued sugar uniformly. */
    while (f && r->i < r->n && r->s[r->i] == '(') { /* f(a)(b) and (expr)(a) chains */
      r->i++;
      node *call = mk_list('('); call->call = 1;
      push(call, f);              /* head (atom name or a prior result list) */
      read_forms(r, ')', call);   /* args */
      f = call;
    }
    push(out, f);
  }
}

/* ------------------------------------------------------- comment splitting
 * Return the byte offset of the start of a `#` line-comment in `line`, or -1.
 * Skips #\, #[, #{, #b" and anything inside a "..." string. */
static int comment_at(const char *line) {
  int in_str = 0; size_t i = 0, n = strlen(line);
  while (i < n) {
    char c = line[i];
    if (in_str) {
      if (c == '\\') { i += 2; continue; }
      if (c == '"') in_str = 0;
      i++; continue;
    }
    if (c == '"') { in_str = 1; i++; continue; }
    if (c == '#') {
      char d = i + 1 < n ? line[i + 1] : 0;
      if (d == '\\') { /* #\X char literal — skip #, \, and the whole codepoint */
        i += 2;
        if (i < n) { unsigned char l = (unsigned char)line[i];
          i += l >= 0xF0 ? 4 : l >= 0xE0 ? 3 : l >= 0xC0 ? 2 : 1; }
        continue;
      }
      if (d == '[' || d == '{' ||
          (d == 'b' && i + 2 < n && line[i + 2] == '"')) { i += 2; continue; }
      return (int)i;
    }
    i++;
  }
  return -1;
}

/* a line that "opens a block": ends with ':' (not inside a string/paren). */
static int opens_block(const char *body) {
  size_t n = strlen(body);
  return n > 0 && body[n - 1] == ':';
}
/* an inline `head: tail` colon (a ':' not at end, outside strings/brackets). */
static int inline_colon(const char *body) {
  int in_str = 0, depth = 0; size_t n = strlen(body);
  for (size_t i = 0; i < n; i++) {
    char c = body[i];
    if (in_str) { if (c == '\\') { i++; continue; } if (c == '"') in_str = 0; continue; }
    if (c == '"') { in_str = 1; continue; }
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') depth--;
    else if (c == ':' && depth == 0 && i + 1 < n &&
             i > 0 && body[i - 1] != ' ' && body[i - 1] != '\t' &&
             (body[i + 1] == ' ' || body[i + 1] == '\t'))
      return (int)i; /* `head: body` — not a :keyword (those are space/<letter>) */
  }
  return -1;
}

/* parse one code line's text into the node it denotes (lone atom -> value;
   lone list -> as-is; many -> a synthetic call list). */
static node *line_node(const char *text) {
  lex r = {text, 0, strlen(text)};
  node *forms = mk_list('('); forms->open = 'L'; /* 'L' = bare line list */
  read_forms(&r, 0, forms);
  if (forms->n == 1) { node *only = forms->kid[0]; forms->kid[0] = NULL; freenode(forms); return only; }
  return forms;
}

/* ------------------------------------------------------------ block reader
 * Reads whole source into a roots list, honoring indentation `:` blocks and
 * inline `head: body`, attaching comments and blank-line markers. elif/else
 * are kept as their own block forms (the printer renders them as siblings;
 * adr.h handles their semantic attachment). */
static node *parse(const char *src) {
  enum { MAXD = 256 };
  int ind_stack[MAXD]; node *node_stack[MAXD]; int sp = 0;
  node *roots = mk_list('R');
  buf pend; buf_init(&pend);   /* pending leading-comment block */
  int pend_blank = 0;

  size_t i = 0, slen = strlen(src);
  while (i <= slen) {
    size_t j = i; while (j < slen && src[j] != '\n') j++;
    char *raw = xstrndup(src + i, j - i);
    i = j + 1;
    /* strip trailing \r */
    size_t rl = strlen(raw); while (rl && (raw[rl-1] == '\r')) raw[--rl] = 0;

    int cpos = comment_at(raw);
    char *code = cpos < 0 ? xstrndup(raw, strlen(raw)) : xstrndup(raw, cpos);
    char *comment = cpos < 0 ? NULL : xstrndup(raw + cpos, strlen(raw + cpos));
    /* trim code both ends */
    int indent = 0; while (code[indent] == ' ' || code[indent] == '\t') indent++;
    char *body = xstrndup(code + indent, strlen(code + indent));
    for (size_t k = strlen(body); k && (body[k-1]==' '||body[k-1]=='\t'); ) body[--k] = 0;
    /* trim trailing ws from comment */
    if (comment) for (size_t k = strlen(comment); k && (comment[k-1]==' '||comment[k-1]=='\t'); ) comment[--k] = 0;

    if (body[0] == 0) {
      if (comment) {            /* full-line comment -> pending lead block */
        if (pend.len) buf_putc(&pend, '\n');
        buf_puts(&pend, comment);
      } else {                  /* blank line */
        pend_blank = 1;
      }
      free(raw); free(code); free(comment); free(body);
      continue;
    }

    int block = opens_block(body);
    int icolon = block ? -1 : inline_colon(body);
    node *nd;
    if (icolon >= 0) {
      char *tail = xstrndup(body + icolon + 1, strlen(body + icolon + 1));
      char *tt = tail; while (*tt==' '||*tt=='\t') tt++;
      body[icolon] = 0;
      nd = line_node(body);
      if (!nd->is_list) { node *L = mk_list('('); push(L, nd); nd = L; }
      nd->hdr_n = nd->n;        /* head-line forms; the body_part follows */
      push(nd, line_node(tt));
      nd->block = 1;            /* inline head: body — printer may keep inline */
      free(tail);
    } else {
      char *b2 = xstrndup(body, block ? strlen(body) - 1 : strlen(body));
      for (size_t k = strlen(b2); k && (b2[k-1]==' '||b2[k-1]=='\t'); ) b2[--k] = 0;
      nd = line_node(b2);
      if (block && !nd->is_list) { node *L = mk_list('('); push(L, nd); nd = L; }
      if (block) { nd->block = 1; nd->hdr_n = nd->n; } /* indented body appends later */
      free(b2);
    }
    /* attach trivia */
    if (pend.len) { nd->lead = xstrndup(pend.p, pend.len); pend.len = 0; pend.p[0] = 0; }
    if (comment) nd->trail = xstrndup(comment, strlen(comment));
    nd->blank = pend_blank; pend_blank = 0;

    /* indentation nesting: pop to parents shallower than this line */
    while (sp > 0 && indent <= ind_stack[sp - 1]) sp--;
    if (sp > 0) push(node_stack[sp - 1], nd); else push(roots, nd);

    if ((block || icolon < 0 ? block : 0) && sp < MAXD) {
      ind_stack[sp] = indent; node_stack[sp] = nd; sp++;
    }
    free(raw); free(code); free(comment); free(body);
  }
  /* trailing comment block with no following form -> a comment-only root */
  if (pend.len) { node *c = mk_list('C'); c->lead = xstrndup(pend.p, pend.len); push(roots, c); }
  free(pend.p);
  return roots;
}

/* ------------------------------------------------------------- the printer */
#define INDENT_STEP 2
#define WIDTH 80           /* break forms wider than this */

static int is_def_head(const char *s) {
  return s && (!strcmp(s, "def") || !strcmp(s, "defn") || !strcmp(s, "defc") ||
               !strcmp(s, "defmacro") || !strcmp(s, "macro"));
}
static int is_fn_head(const char *s) { return s && (!strcmp(s, "fn") || !strcmp(s, "lambda")); }
/* if-family: must NOT inline-collapse — adr.h attaches elif/else only to an
   if/when/unless that stays an open `:`-block. */
static int is_if_family(const char *s) {
  return s && (!strcmp(s, "if") || !strcmp(s, "when") || !strcmp(s, "unless") ||
               !strcmp(s, "elif") || !strcmp(s, "else"));
}
static const char *head_tok(const node *x) {
  if (x->is_list && x->n > 0 && !x->kid[0]->is_list) return x->kid[0]->tok;
  return NULL;
}
/* (= a b) -> render as the Adder-preferred infix `a = b` (statement level). */
static int is_infix_assign(const node *x) {
  return x->is_list && !x->call && x->n == 3 && !x->kid[0]->is_list &&
         x->kid[0]->tok && !strcmp(x->kid[0]->tok, "=");
}
/* NOTE on operator infix `(n < 0)` / `(n is 0)`: it reads better, but adr.h does
   NOT lower it (unlike `a = b`), so it stays a RUNTIME infix dispatch and the
   compiler can't fuse it into the SLOT_<cmp>_FIX superinstruction the JIT loop
   matcher needs — it deoptimizes hot loops (verified: +117 jit-loop test fails).
   So it's deliberately NOT applied here. A future `--infix` opt-in could enable
   it for code where readability outweighs loop perf. */
static int g_infix_ops = 0; /* off: operator infix would deopt hot loops */
static int is_infix_op(const char *s) {
  if (!g_infix_ops || !s) return 0;
  static const char *ops[] = {"<",">","<=",">=","is","iso","+","-","*","/","mod",0};
  for (int k = 0; ops[k]; k++) if (!strcmp(s, ops[k])) return 1;
  return 0;
}
static int g_quoted = 0; /* set while emitting inside a quote/quasiquote */
static int is_infix_op_form(const node *x) {
  return !g_quoted && x->is_list && !x->call && x->open == '(' && x->n == 3 &&
         !x->kid[0]->is_list && is_infix_op(x->kid[0]->tok);
}

static void emit_inline(const node *x, buf *o);
static void emit_seq(node **kid, int from, int n, buf *o) {
  for (int k = from; k < n; k++) { if (k > from) buf_putc(o, ' '); emit_inline(kid[k], o); }
}

/* one form, fully inline (single line). Faithful: call sugar only where the
   source used it (x->call); `(= a b)` becomes infix `a = b`. */
static void emit_inline(const node *x, buf *o) {
  if (!x->is_list) { buf_puts(o, x->tok); return; }
  if (x->open == '\'') { /* quote/quasiquote/unquote — its body is literal data */
    int save = g_quoted; g_quoted = 1;
    buf_puts(o, x->tok); emit_inline(x->kid[0], o); g_quoted = save; return;
  }
  if (is_infix_op_form(x)) { /* (op a b) -> (a op b) */
    buf_putc(o, '('); emit_inline(x->kid[1], o);
    buf_putc(o, ' '); buf_puts(o, x->kid[0]->tok); buf_putc(o, ' ');
    emit_inline(x->kid[2], o); buf_putc(o, ')'); return;
  }
  if (x->vec) { buf_puts(o, "#["); emit_seq(x->kid, 0, x->n, o); buf_putc(o, ']'); return; }
  if (x->set) { buf_puts(o, "#{"); emit_seq(x->kid, 0, x->n, o); buf_putc(o, '}'); return; }
  if (x->map) { buf_putc(o, '{'); emit_seq(x->kid, 0, x->n, o); buf_putc(o, '}'); return; }
  if (x->open == '[') { buf_putc(o, '['); emit_seq(x->kid, 0, x->n, o); buf_putc(o, ']'); return; }
  /* NB: infix `a = b` is applied only at STATEMENT level (emit_form); nested
     (= a b) — e.g. inside a quasiquote or as a call arg — stays prefix, because
     `lhs = rhs` only round-trips through adr.h's line-level infix rule. */
  if (x->call) { /* head(args) — source-glued call, no space */
    emit_inline(x->kid[0], o); buf_putc(o, '(');
    emit_seq(x->kid, 1, x->n, o); buf_putc(o, ')'); return;
  }
  if (x->open == 'L') { emit_seq(x->kid, 0, x->n, o); return; } /* bare line */
  buf_putc(o, '('); emit_seq(x->kid, 0, x->n, o); buf_putc(o, ')');
}
static size_t inline_width(const node *x) { buf t; buf_init(&t); emit_inline(x, &t); size_t w = t.len; free(t.p); return w; }

static void emit_form(const node *x, int col, buf *o);
static void put_indent(buf *o, int col) { for (int k = 0; k < col; k++) buf_putc(o, ' '); }
static void put_comment_block(const char *lead, int col, buf *o) {
  const char *p = lead;
  while (p && *p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    put_indent(o, col); buf_putn(o, p, len); buf_putc(o, '\n');
    if (!nl) break;
    p = nl + 1;
  }
}

/* render a block form's HEADER (kids 0..hdr_n) into `hdr`, normalizing a
   def/fn header to `def name(params)` / `fn(params)` regardless of whether the
   source glued the params. Returns the body-start index. */
static int emit_block_header(const node *x, buf *hdr, int hn_in) {
  const char *h = head_tok(x);
  int hn = hn_in > 0 ? hn_in : (x->hdr_n > 0 ? x->hdr_n : x->n);
  /* head line was itself a glued call: fn(params): / fn(): / name(args): —
     render head(args) (kid[0] + the call args up to the header boundary). */
  if (x->call) {
    emit_inline(x->kid[0], hdr); buf_putc(hdr, '(');
    emit_seq(x->kid, 1, hn, hdr); buf_putc(hdr, ')');
    return hn;
  }
  if (is_def_head(h) && hn >= 2) {
    buf_puts(hdr, h); buf_putc(hdr, ' ');
    if (x->kid[1]->call) { emit_inline(x->kid[1], hdr); return hn; } /* name(params) glued */
    /* def name (params): two separate forms -> glue them. A bare-symbol params
       form is a REST parameter (def f xs ...) — keep it unparenthesized. */
    emit_inline(x->kid[1], hdr);                       /* name */
    if (hn >= 3) { node *p = x->kid[2];
      if (p->is_list && !p->call) { buf_putc(hdr, '('); emit_seq(p->kid, 0, p->n, hdr); buf_putc(hdr, ')'); }
      else { buf_putc(hdr, ' '); emit_inline(p, hdr); } /* rest param: name xs */
      for (int k = 3; k < hn; k++) { buf_putc(hdr, ' '); emit_inline(x->kid[k], hdr); }
    }
    return hn;
  }
  if (is_fn_head(h) && hn >= 2) {
    buf_puts(hdr, "fn"); node *p = x->kid[1]; buf_putc(hdr, '(');
    if (p->is_list && !p->call) emit_seq(p->kid, 0, p->n, hdr); else emit_inline(p, hdr);
    buf_putc(hdr, ')');
    for (int k = 2; k < hn; k++) { buf_putc(hdr, ' '); emit_inline(x->kid[k], hdr); }
    return hn;
  }
  /* generic block head: render the head-line forms verbatim */
  for (int k = 0; k < hn; k++) { if (k) buf_putc(hdr, ' '); emit_inline(x->kid[k], hdr); }
  return hn;
}

/* How many leading kids of a form with this head are the "header" (signature /
   condition / binding); the rest are the indentable body. -1 = not a body form.
   Ported from alc2adr.py's HEADER_ARITY so a FLAT s-expr (an Alcove file, or an
   inlined Adder form) converts to indented Adder when it's wide enough to read
   better. Block rule re-appends the body children in order, so it round-trips. */
static int header_arity(const char *h) {
  if (!h) return -1;
  struct { const char *n; int a; } t[] = {
    {"def",3},{"defn",3},{"defc",3},{"defmacro",3},{"macro",3},{"mac",3},
    {"fn",2},{"lambda",2},{"with",2},{"each",2},{"let",3},{"let*",3},
    {"if",2},{"when",2},{"unless",2},{"while",2},{"for",4},{"case",2},{"do",1},{0,0}};
  for (int k = 0; t[k].n; k++) if (!strcmp(h, t[k].n)) return t[k].a;
  return -1;
}

/* A form's header arity (kids before the body) when it is a body-bearing form
   that should render with Adder header sugar (`head …: body`) — whether inline
   or broken. Returns 0 for a plain form (rendered as one inline expression). A
   source `:`-block, or a flat paren form whose head is in HEADER_ARITY with at
   least one body kid, qualifies. */
static int header_hn(const node *x) {
  if (x->block) return x->hdr_n > 0 ? x->hdr_n : x->n;
  if (!x->is_list || x->open != '(' || x->call) return 0;
  int ha = header_arity(head_tok(x));
  return (ha >= 0 && x->n > ha) ? ha : 0;
}
/* Should this header form BREAK its body across indented lines (vs inline
   `head: body`)? Yes when wider than WIDTH, >1 body form, a body form is itself
   a header form, or it's the if-family (elif/else need an open block). */
static int wants_break(const node *x, int col, int hn) {
  if (is_if_family(head_tok(x))) return 1;
  if (x->n - hn > 1) return 1;
  if (col + inline_width(x) > WIDTH) return 1;
  for (int k = hn; k < x->n; k++)
    if (x->kid[k]->is_list && header_hn(x->kid[k])) return 1;
  return 0;
}

/* emit one form at indentation `col`, choosing inline vs `:`-block. */
static void emit_form(const node *x, int col, buf *o) {
  if (x->open == 'C') { put_comment_block(x->lead, col, o); return; } /* comment-only */
  if (x->blank) buf_putc(o, '\n');
  if (x->lead) put_comment_block(x->lead, col, o);

  int hn = header_hn(x);
  if (hn == 0) { /* plain form: one inline line */
    put_indent(o, col);
    if (is_infix_assign(x)) { /* statement-level (= a b) -> a = b */
      emit_inline(x->kid[1], o); buf_puts(o, " = "); emit_inline(x->kid[2], o);
    } else emit_inline(x, o);
    if (x->trail) { buf_putc(o, ' '); buf_puts(o, x->trail); }
    buf_putc(o, '\n');
    return;
  }

  /* header form: render `head …` then either inline `: body` or a broken block. */
  buf hdr; buf_init(&hdr);
  int bodyfrom = emit_block_header(x, &hdr, hn);

  if (!wants_break(x, col, bodyfrom) &&
      !x->kid[bodyfrom]->lead && !x->kid[bodyfrom]->trail) {
    /* inline `header: body` (single short body form) */
    put_indent(o, col); buf_puts(o, hdr.p); buf_puts(o, ": ");
    emit_inline(x->kid[bodyfrom], o);
    if (x->trail) { buf_putc(o, ' '); buf_puts(o, x->trail); }
    buf_putc(o, '\n'); free(hdr.p); return;
  }

  put_indent(o, col); buf_puts(o, hdr.p); buf_putc(o, ':');
  if (x->trail) { buf_putc(o, ' '); buf_puts(o, x->trail); }
  buf_putc(o, '\n');
  for (int k = bodyfrom; k < x->n; k++) emit_form(x->kid[k], col + INDENT_STEP, o);
  free(hdr.p);
}

char *adder_format(const char *src) {
  node *roots = parse(src);
  buf o; buf_init(&o);
  for (int k = 0; k < roots->n; k++) emit_form(roots->kid[k], 0, &o);
  freenode(roots);
  return o.p;
}

/* ----------------------------------------------------------------- CLI ----
 * Shared by the standalone `adfmt` binary and the `adder fmt` subcommand.
 *   adfmt [opts] [files...]      no files / "-"  -> stdin -> stdout
 *   --write / -w   format files IN PLACE (default for files is stdout)
 *   --check        exit 1 if any file is not already formatted (CI); no writes
 *   --infix        emit operator infix (n < 0) — readable but deopts hot loops
 *   --diff         print a unified-ish before/after marker per changed file
 */
static char *read_all(FILE *f) {
  buf in; buf_init(&in);
  char tmp[4096]; size_t r;
  while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) buf_putn(&in, tmp, r);
  return in.p;
}

static int adfmt_usage(FILE *o) {
  fputs(
"usage: adder fmt [options] [files...]\n"
"  (no files, or \"-\")   read stdin, write formatted Adder to stdout\n"
"  files...              read each file\n"
"options:\n"
"  -w, --write           rewrite each file in place (else print to stdout)\n"
"      --check           exit non-zero if any file isn't already formatted\n"
"                        (prints the unformatted paths; makes no changes)\n"
"      --infix           use operator infix `(n < 0)` (NB: deoptimizes hot\n"
"                        loops — the compiler can't fuse a runtime-infix cmp)\n"
"  -h, --help            this help\n", o);
  return 2;
}

int adfmt_cli_main(int argc, char **argv) {
  int write_inplace = 0, check = 0, nfiles = 0;
  const char *files[4096];
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "-w") || !strcmp(a, "--write")) write_inplace = 1;
    else if (!strcmp(a, "--check")) check = 1;
    else if (!strcmp(a, "--infix")) g_infix_ops = 1;
    else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { adfmt_usage(stdout); return 0; }
    else if (a[0] == '-' && a[1] && strcmp(a, "-")) { fprintf(stderr, "adfmt: unknown option %s\n", a); return adfmt_usage(stderr); }
    else if (nfiles < (int)(sizeof files / sizeof *files)) files[nfiles++] = a;
  }

  /* stdin -> stdout when no real files given */
  if (nfiles == 0 || (nfiles == 1 && !strcmp(files[0], "-"))) {
    if (write_inplace || check) { fputs("adfmt: --write/--check need file arguments\n", stderr); return 2; }
    char *src = read_all(stdin);
    char *out = adder_format(src);
    fputs(out, stdout);
    free(out); free(src);
    return 0;
  }

  int rc = 0;
  for (int i = 0; i < nfiles; i++) {
    FILE *f = fopen(files[i], "rb");
    if (!f) { perror(files[i]); rc = 2; continue; }
    char *src = read_all(f); fclose(f);
    char *out = adder_format(src);
    int changed = strcmp(src, out) != 0;
    if (check) {
      if (changed) { printf("%s\n", files[i]); rc = 1; }
    } else if (write_inplace) {
      if (changed) {
        FILE *w = fopen(files[i], "wb");
        if (!w) { perror(files[i]); rc = 2; }
        else { fputs(out, w); fclose(w); fprintf(stderr, "formatted %s\n", files[i]); }
      }
    } else {
      fputs(out, stdout);
    }
    free(out); free(src);
  }
  return rc;
}

#ifndef ADFMT_NO_MAIN
int main(int argc, char **argv) { return adfmt_cli_main(argc, argv); }
#endif
