/* pp.h — Source pretty-printer. FRAGMENT
 * #included into alcove.c (single TU). NOT standalone, NOT separately compiled.
 * `make tidy` lints it in context via alcove.c.
 */

/* ---------------- Source pretty-printer ----------------
   Used by (source fn) to render a lambda body the way a Lisper would
   write it: special forms get hanging indents, sub-forms break onto
   their own lines, atoms stay inline. Width is a soft limit; if a
   form's atom-only flat rendering fits under PP_WIDTH chars, we
   keep it on one line. */
#define PP_WIDTH 60
static void pp_indent(int n) {
  int i;
  for (i = 0; i < n; i++)
    putchar(' ');
}

/* Best-effort flat width estimate for an exp_t. Counts the chars
   print_node would emit, ignoring ANSI escapes. Returns INT_MAX-ish
   on cycles or very large structures so the pretty-printer falls
   back to multi-line. */
static int pp_flat_width(exp_t *e) {
  if (e == NULL)
    return 3; /* "nil" */
  if (isnumber(e)) {
    int64_t v = FIX_VAL(e);
    int w = (v < 0) ? 2 : 1;
    int64_t a = v < 0 ? -v : v;
    while (a >= 10) {
      a /= 10;
      w++;
    }
    return w;
  }
  if (ischar(e))
    return 4; /* "#\X" */
  if (!is_ptr(e))
    return 8;
  switch (e->type) {
  case EXP_SYMBOL:
  case EXP_STRING:
    return exp_text(e) ? (int)strlen((char *)exp_text(e)) +
                             (e->type == EXP_STRING ? 2 : 0)
                       : 3;
  case EXP_FLOAT:
    return 12; /* approx */
  case EXP_PAIR: {
    int w = 2; /* parens */
    exp_t *cur;
    int first = 1;
    for (cur = e; cur; cur = cur->next) {
      if (cur->type != EXP_PAIR) {
        w += 3 + pp_flat_width(cur); /* " . X" */
        break;
      }
      if (!first)
        w += 1;
      first = 0;
      w += pp_flat_width(cur->content);
      if (w > PP_WIDTH * 4)
        return PP_WIDTH * 4; /* short-circuit on big forms */
    }
    return w;
  }
  default:
    return 16;
  }
}

static void pp_form(exp_t *e, int indent);

/* Print one body form, then a newline. Used for the body lists in
   def/fn/let/do where each top-level form starts a fresh line. */
static void pp_body(exp_t *body, int indent) {
  while (body) {
    putchar('\n');
    pp_indent(indent);
    pp_form(body->content, indent);
    body = body->next;
  }
}

static void pp_form(exp_t *e, int indent) {
  if (!ispair(e) || !istrue(e)) {
    print_node(e);
    return;
  }

  exp_t *head = car(e);
  const char *s = (issymbol(head)) ? (const char *)exp_text(head) : NULL;

  /* If the whole form is small, stay on one line. */
  if (pp_flat_width(e) <= PP_WIDTH - indent) {
    print_node(e);
    return;
  }

  /* Special forms with hanging indents. */
  if (s) {
    /* (if cond then [else]) — cond on header line, then/else stacked
       and indented past `(if `. */
    if (!strcmp(s, "if")) {
      exp_t *cond = cadr(e), *th = caddr(e), *el = cadddr(e);
      int sub = indent + 4;
      printf("\x1B[33m(\x1B[1;35mif\x1B[22;39m ");
      pp_form(cond, sub);
      putchar('\n');
      pp_indent(sub);
      pp_form(th, sub);
      if (el) {
        putchar('\n');
        pp_indent(sub);
        pp_form(el, sub);
      }
      printf("\x1B[33m)\x1B[39m");
      return;
    }
    /* (def NAME PARAMS BODY...) and (defmacro NAME PARAMS BODY...) */
    if (!strcmp(s, "def") || !strcmp(s, "defmacro")) {
      exp_t *name = cadr(e), *params = caddr(e), *body = cdddr(e);
      printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m ", s);
      print_node(name);
      putchar(' ');
      print_node(params);
      pp_body(body, indent + 2);
      printf("\x1B[33m)\x1B[39m");
      return;
    }
    /* (fn PARAMS BODY...) and (mac PARAMS BODY...) */
    if (!strcmp(s, "fn") || !strcmp(s, "mac")) {
      exp_t *params = cadr(e), *body = cddr(e);
      printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m ", s);
      print_node(params);
      pp_body(body, indent + 2);
      printf("\x1B[33m)\x1B[39m");
      return;
    }
    /* (let VAR VAL BODY...) — VAR and VAL inline, body indented. */
    if (!strcmp(s, "let")) {
      exp_t *var = cadr(e), *val = caddr(e), *body = cdddr(e);
      printf("\x1B[33m(\x1B[1;35mlet\x1B[22;39m ");
      print_node(var);
      putchar(' ');
      pp_form(val, indent + 2);
      pp_body(body, indent + 2);
      printf("\x1B[33m)\x1B[39m");
      return;
    }
    /* (do BODY...), (and ...), (or ...), (when ...), (unless ...) */
    if (!strcmp(s, "do") || !strcmp(s, "and") || !strcmp(s, "or") ||
        !strcmp(s, "when") || !strcmp(s, "unless")) {
      printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m", s);
      pp_body(cdr(e), indent + 2);
      printf("\x1B[33m)\x1B[39m");
      return;
    }
    /* (cond (test body...) ...) — each clause on its own line. */
    if (!strcmp(s, "cond")) {
      printf("\x1B[33m(\x1B[1;35mcond\x1B[22;39m");
      exp_t *clauses = cdr(e);
      while (clauses) {
        putchar('\n');
        pp_indent(indent + 2);
        pp_form(clauses->content, indent + 2);
        clauses = clauses->next;
      }
      printf("\x1B[33m)\x1B[39m");
      return;
    }
  }

  /* General call form (HEAD ARG1 ARG2 ...).  Header inline; if too long,
     stack args under first arg with align indent. */
  printf("\x1B[33m(\x1B[39m");
  print_node(head); /* head atom */
  exp_t *args = cdr(e);
  int sub = indent + (s ? (int)strlen(s) : 1) + 2;
  /* Place first arg on header line, rest on new lines. */
  if (args) {
    putchar(' ');
    pp_form(args->content, sub);
    args = args->next;
  }
  while (args) {
    putchar('\n');
    pp_indent(sub);
    pp_form(args->content, sub);
    args = args->next;
  }
  printf("\x1B[33m)\x1B[39m");
}

/* (source fn) — print the lambda's defining source: header + body.
   The output reads back as alcove code.  For named lambdas (def'd /
   defmacro'd) the leading form is `def` or `defmacro` and includes
   the name; for anonymous lambdas it's `fn` or `mac`.  Closures get
   a "; closure over <env>" comment so the user knows the body's
   free vars resolve against a captured environment. */
#ifdef ALCOVE_ALS
/* ---- adder source rendering ------------------------------
   Render a lambda/macro as Adder: drop outer parens at
   statement position, open `:`-blocks for body-bearing special forms,
   ladder trailing list/cons builders, shorten (quote x) to 'x. */
static int als_len(exp_t *x) {
  int n = 0;
  for (exp_t *p = x; p && ispair(p) && istrue(p); p = p->next)
    n++;
  return n;
}
static int als_harity(const char *s) {
  if (!s)
    return 0;
  if (!strcmp(s, "def") || !strcmp(s, "defmacro") || !strcmp(s, "mac") ||
      !strcmp(s, "let"))
    return 3;
  if (!strcmp(s, "for"))
    return 4;
  if (!strcmp(s, "fn") || !strcmp(s, "with") || !strcmp(s, "each") ||
      !strcmp(s, "if") || !strcmp(s, "when") || !strcmp(s, "unless") ||
      !strcmp(s, "while") || !strcmp(s, "case"))
    return 2;
  if (!strcmp(s, "do"))
    return 1;
  return 0;
}
static int als_is_builder(const char *s) {
  return s && (!strcmp(s, "list") || !strcmp(s, "cons") ||
               !strcmp(s, "append") || !strcmp(s, "quasiquote"));
}
/* a list whose head is list/cons/append/quasiquote */
static int als_builder_node(exp_t *e) {
  return e && ispair(e) && istrue(e) && issymbol(e->content) &&
         als_is_builder((char *)exp_text(e->content));
}
static void als_expr(exp_t *x);
static int als_is_quote(exp_t *x) {
  return x && ispair(x) && istrue(x) && als_len(x) == 2 &&
         issymbol(x->content) && !strcmp((char *)exp_text(x->content), "quote");
}
static void als_expr(exp_t *x) { /* argument position: keep parens */
  if (!x || !ispair(x) || !istrue(x)) {
    print_node(x);
    return;
  }
  if (als_is_quote(x)) {
    putchar('\'');
    als_expr(x->next->content);
    return;
  }
  putchar('(');
  int first = 1;
  for (exp_t *p = x; p && ispair(p) && istrue(p); p = p->next) {
    if (!first)
      putchar(' ');
    als_expr(p->content);
    first = 0;
  }
  putchar(')');
}
static void als_stmt(exp_t *x, int ind) { /* statement position */
  if (!x || !ispair(x) || !istrue(x)) {
    print_node(x);
    return;
  }
  int len = als_len(x);
  exp_t *head = x->content;
  const char *hs = issymbol(head) ? (const char *)exp_text(head) : NULL;
  if (als_is_quote(x)) {
    putchar('\'');
    als_expr(x->next->content);
    return;
  }
  if (len == 1 && hs) {
    printf("%s()", hs);
    return;
  }
  int ha = als_harity(hs), k = 0;
  if (ha && len > ha) {
    k = ha;
  } else if (als_is_builder(hs)) {
    /* ladder the maximal trailing run of nested builder calls */
    int last_non = 0, i = 0;
    for (exp_t *p = x; p && ispair(p) && istrue(p); p = p->next, i++)
      if (!als_builder_node(p->content))
        last_non = i;
    if (last_non + 1 < len)
      k = last_non + 1;
  }
  if (k > 0) { /* header line + indented child statements */
    int i = 0;
    for (exp_t *p = x; p && ispair(p) && istrue(p) && i < k; p = p->next, i++) {
      if (i)
        putchar(' ');
      als_expr(p->content);
    }
    putchar(':');
    i = 0;
    for (exp_t *p = x; p && ispair(p) && istrue(p); p = p->next, i++) {
      if (i < k)
        continue;
      putchar('\n');
      pp_indent(ind + 2);
      als_stmt(p->content, ind + 2);
    }
    return;
  }
  int first = 1; /* plain statement: unwrapped head + operands */
  for (exp_t *p = x; p && ispair(p) && istrue(p); p = p->next) {
    if (!first)
      putchar(' ');
    als_expr(p->content);
    first = 0;
  }
}
#endif /* ALCOVE_ALS */

const char doc_source[] = "(source fn) — print the original (params) + body "
                          "for a user-defined function.";
exp_t *sourcecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(arg);
  if (!arg || !is_ptr(arg) || (!islambda(arg) && !ismacro(arg))) {
    printf("\x1B[96m(source): not a lambda or macro\x1B[39m\n");
    CLEAN_RETURN_1(arg, NULL);
  }
  int is_macro = ismacro(arg);
  /* Only flag as a "closure" if the captured env is non-global. Top-
     level def'd lambdas always capture g_global_env; that's not a
     real closure, just the default scope. */
  env_t *cap = (env_t *)(arg->next ? arg->next->meta : NULL);
  int captured = (cap && cap != g_global_env) ? 1 : 0;
#ifdef ALCOVE_ALS
  /* adder rendering: `def NAME (params):` then the body as
     indented statements (no outer parens, `:`-blocks, 'quote). */
  {
    const char *kw =
        arg->meta ? (is_macro ? "defmacro" : "def") : (is_macro ? "mac" : "fn");
    printf("\x1B[1;35m%s\x1B[22;39m ", kw);
    if (arg->meta)
      printf("\x1B[36m%s\x1B[39m ", (char *)arg->meta);
    exp_t *params = lambda_params(arg);
    if (params && ispair(params) && istrue(params))
      als_expr(params);
    else
      printf("()"); /* no-arg list, not the symbol nil */
    printf(":");
    /* defmacrocmd stores the macro's single body form directly at
       arg->next->content; defcmd/fncmd store a list-of-forms there. */
    exp_t *bd = arg->next ? arg->next->content : NULL;
    if (is_macro) {
      printf("\n  ");
      als_stmt(bd, 2);
    } else {
      for (exp_t *b = bd; b; b = b->next) {
        printf("\n  ");
        als_stmt(b->content, 2);
      }
    }
    if (captured)
      printf("  \x1B[90m; closure over env %p\x1B[39m", (void *)cap);
    printf("\n");
    unrefexp(arg);
    unrefexp(e);
    return NULL;
  }
#endif
  /* Print the header inline (def NAME PARAMS or fn PARAMS), then
     pretty-print each body form on its own indented line. */
  if (arg->meta) {
    printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m \x1B[36m%s\x1B[39m ",
           is_macro ? "defmacro" : "def", (char *)arg->meta);
  } else {
    printf("\x1B[33m(\x1B[1;35m%s\x1B[22;39m ", is_macro ? "mac" : "fn");
  }
  print_node(lambda_params(arg)); /* params list */
  exp_t *body = arg->next ? arg->next->content : NULL;
  while (body) {
    printf("\n  ");
    pp_form(body->content, 2);
    body = body->next;
  }
  printf("\x1B[33m)\x1B[39m");
  if (captured)
    printf("  \x1B[90m; closure over env %p\x1B[39m", (void *)cap);
  printf("\n");
  unrefexp(arg);
  unrefexp(e);
  return NULL;
}
#pragma GCC diagnostic warning "-Wunused-parameter"
