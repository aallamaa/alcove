/* compiler_impl.h — bytecode compiler and VM execution implementation. FRAGMENT
 * #included into alcove.c (single TU). NOT standalone, NOT separately compiled.
 * `make tidy` lints it in context via alcove.c.
 */

static void emit_u8(compiler_t *c, uint8_t b) {
  if (c->failed)
    return;
  if (c->ncode + 1 > c->code_cap) {
    c->code_cap = c->code_cap ? c->code_cap * 2 : 64;
    c->code = xrealloc(c->code, c->code_cap);
  }
  c->code[c->ncode++] = b;
}
static void emit_i16(compiler_t *c, int16_t v) {
  emit_u8(c, (uint8_t)(v & 0xff));
  emit_u8(c, (uint8_t)((v >> 8) & 0xff));
}
/* Record "from the current code offset, source position is form e's" into the
   compiler's pc→loc table — coalescing consecutive forms on the same line so
   the table stays tiny. Cold (compile time only); the VM never reads it except
   when raising an error. No-op when the form carries no source position. */
static void emit_loc(compiler_t *c, exp_t *e) {
  int line = form_line(e);
  if (c->failed || !line || line == c->last_loc_line)
    return;
  if (c->nlocs + 1 > c->locs_cap) {
    int ncap = c->locs_cap ? c->locs_cap * 2 : 16;
    bc_loc_t *nl = realloc(c->locs, (size_t)ncap * sizeof(*nl));
    if (!nl)
      return; /* OOM: skip this entry — line info is best-effort, never fatal */
    c->locs = nl;
    c->locs_cap = ncap;
  }
  c->locs[c->nlocs].pc = c->ncode;
  c->locs[c->nlocs].line = line;
  c->locs[c->nlocs].col = form_col(e);
  c->nlocs++;
  c->last_loc_line = line;
}
static int add_const(compiler_t *c, exp_t *v) {
  /* de-dupe by pointer equality — rare wins but costs nothing */
  int i;
  for (i = 0; i < c->nconsts; i++)
    if (c->consts[i] == v)
      return i;
  /* OP_LOAD_CONST encodes the index as u8, so at most 256 distinct
     constants per lambda. Above that we bail rather than silently
     wrap — the tree-walker will still handle the body. */
  if (c->nconsts >= 256) {
    c->failed = 1;
    return -1;
  }
  if (c->nconsts + 1 > c->consts_cap) {
    c->consts_cap = c->consts_cap ? c->consts_cap * 2 : 8;
    c->consts = xrealloc(c->consts, c->consts_cap * sizeof(exp_t *));
  }
  c->consts[c->nconsts] = refexp(v);
  return c->nconsts++;
}
/* Emit `OP_LOAD_CONST <idx>` for `v`, interning the constant. No-op-safe if a
   prior emit failed; bails (c->failed) if the const pool overflowed. Cold —
   compiler only. */
ALCOVE_COLD static void emit_load_const(compiler_t *c, exp_t *v) {
  int k = add_const(c, v);
  if (c->failed)
    return;
  emit_u8(c, OP_LOAD_CONST);
  emit_u8(c, (uint8_t)k);
}
/* Emit the nil literal — the implicit result of empty bodies, else-less ifs,
   etc. Thin wrapper over emit_load_const(nil). */
ALCOVE_COLD static void emit_nil(compiler_t *c) {
  emit_load_const(c, nil_singleton);
}
/* Emit a NAMED slot binding for a let/with/for-counter local. Storing the
   name (as a const symbol) lets an OP_EVAL_AST sub-form resolve the local by
   symbol at runtime; plain OP_BIND_SLOT leaves a NULL key and is kept only for
   for's hidden, nameless end-value slot. */
static void emit_bind_named(compiler_t *c, int slot, exp_t *var) {
  int nk = add_const(c, var);
  if (c->failed)
    return;
  emit_u8(c, OP_BIND_SLOT_NAMED);
  emit_u8(c, (uint8_t)slot);
  emit_u8(c, (uint8_t)nk);
}
static int find_slot(compiler_t *c, const char *name) {
  /* Innermost (highest idx) binding wins so inner let shadows outer.
     NULL slot_names are "hidden" (e.g. for's end-value slot) — skipped. */
  int i;
  for (i = c->nslots - 1; i >= 0; i--) {
    if (!c->slot_names[i])
      continue;
    if (strcmp(c->slot_names[i], name) == 0)
      return i;
  }
  return -1;
}

static int op_for_head(const char *s);
static void compile_expr(compiler_t *c, exp_t *e, int tail);
static void patch_i16(compiler_t *c, int patch, int target);

/* Is `e` one of the native infix-operator symbols (+ - * / < > <= >= is iso
   isnt mod)? These are the ops the compiler/VM/JIT have fast paths for. */
static int is_native_op_symbol(exp_t *e) {
  if (!e || !is_ptr(e) || !issymbol(e))
    return 0;
  static const char *const ops[] = {"+",  "-",  "*",  "/",   "<",    ">",
                                    "<=", ">=", "is", "iso", "isnt", "mod"};
  const char *t = (const char *)exp_text(e);
  for (int i = 0; i < 12; i++)
    if (strcmp(t, ops[i]) == 0)
      return 1;
  return 0;
}
/* Infix -> prefix at COMPILE time: (A OP B) -> (OP A B) when OP is a native
   operator AND A is statically NON-CALLABLE — a sub-expression head ((fib ..)),
   a numeric literal, or a param carrying a :type hint. This is what lets infix
   numeric code compile to the native ops and JIT (e.g. (def fib (n :int) (if
   (n < 2) n ((fib (n - 1)) + (fib (n - 2)))))).
   It must NOT fire on a bare un-hinted symbol head: (my-fold + lst) is a real
   CALL passing + to a HOF, and infix-vs-call there is a RUNTIME value question
   — left to the generic call path + the runtime infix dispatch. The AST
   evaluator already computes the same result via that value dispatch, so the
   rewrite keeps AST==VM (verified by equiv_sweep). Returns a fresh owned
   (OP A B) (sharing A/B/op refs) or NULL. */
static exp_t *compile_infix_rewrite(compiler_t *c, exp_t *e) {
  if (!ispair(e))
    return NULL;
  exp_t *a = e->content, *r1 = e->next;
  if (!r1)
    return NULL;
  exp_t *op = r1->content, *r2 = r1->next;
  if (!r2 || r2->next) /* need exactly 3 elements: (A OP B) */
    return NULL;
  if (!is_native_op_symbol(op))
    return NULL;
  int noncallable = 0;
  if (a && is_ptr(a) && ispair(a))
    noncallable = 1; /* sub-expression head: (fib ..) etc. */
  else if (isnumber(a) || (a && is_ptr(a) && isfloat(a)))
    noncallable = 1; /* numeric literal */
  else if (a && is_ptr(a) && issymbol(a)) {
    int slot = find_slot(c, (char *)exp_text(a));
    if (slot >= 0)
      noncallable = 1; /* local variable or parameter is a known value, never a
                          function */
  }
  if (!noncallable)
    return NULL;
  exp_t *n2 = make_node(refexp(a));
  n2->next = make_node(refexp(r2->content));
  exp_t *h = make_node(refexp(op));
  h->next = n2;
  return h;
}

/* Returns OP_ADD..OP_NOT for pure-arithmetic/cmp symbols, -1 otherwise. */
static int op_for_head(const char *s) {
  if (!strcmp(s, "+"))
    return OP_ADD;
  if (!strcmp(s, "-"))
    return OP_SUB;
  if (!strcmp(s, "*"))
    return OP_MUL;
  if (!strcmp(s, "/"))
    return OP_DIV;
  if (!strcmp(s, "mod"))
    return OP_MOD;
  if (!strcmp(s, "<"))
    return OP_LT;
  if (!strcmp(s, ">"))
    return OP_GT;
  if (!strcmp(s, "<="))
    return OP_LE;
  if (!strcmp(s, ">="))
    return OP_GE;
  if (!strcmp(s, "is"))
    return OP_IS;
  if (!strcmp(s, "iso"))
    return OP_ISO;
  if (!strcmp(s, "no"))
    return OP_NOT;
  return -1;
}

/* Compile a sequence of body forms (e.g. a multi-expression let/with/
   when body): evaluate each in order, POP all but the last, and let the
   last keep the caller's tail position. Empty body compiles to nil. */
static void compile_body_seq(compiler_t *c, exp_t *body, int tail) {
  if (!body) {
    emit_nil(c);
    return;
  }
  int saw_any = 0;
  for (; body; body = body->next) {
    if (saw_any)
      emit_u8(c, OP_POP);
    int is_last = (body->next == NULL);
    compile_expr(c, body->content, is_last && tail);
    if (c->failed)
      return;
    saw_any = 1;
  }
}

static void compile_if(compiler_t *c, exp_t *form, int tail) {
  /* (if cond then else)  — only 2-way for phase 1 */
  exp_t *cond = cadr(form);
  exp_t *thn = caddr(form);
  exp_t *els = cadddr(form);
  if (cdddr(form) && cdddr(form)->next) {
    c->failed = 1;
    return;
  }
  compile_expr(c, cond, 0);
  if (c->failed)
    return;
  emit_u8(c, OP_BR_IF_FALSE);
  int patch_false = c->ncode;
  emit_i16(c, 0);
  compile_expr(c, thn, tail);
  if (c->failed)
    return;
  emit_u8(c, OP_JUMP);
  int patch_end = c->ncode;
  emit_i16(c, 0);
  int false_target = c->ncode;
  if (els)
    compile_expr(c, els, tail);
  else {
    /* (if cond then) with no else: result is nil. */
    emit_nil(c);
  }
  if (c->failed)
    return;
  int end_target = c->ncode;
  patch_i16(c, patch_false, false_target);
  patch_i16(c, patch_end, end_target);
}

static void compile_call(compiler_t *c, exp_t *form, int tail) {
  /* Emits one of three ops depending on context:
       - OP_TAIL_SELF: same-fn tail call, rebinds inline slots in place.
         Requires tail && self_name matches head && nlet_depth == 0
         (the inline-slot invariant).
       - OP_TAIL_CALL: other-fn tail call. VM tears down current env
         and jumps to the target lambda with O(1) C stack growth.
         Target must be resolvable at runtime as a lambda.
       - OP_CALL: regular non-tail call (and the fallback when the
         target might be an internal cmd — vm_invoke_values handles). */
  exp_t *head = car(form);
  int nargs = 0;
  exp_t *a;
  /* Slot-headed calls compile fine now that vm_invoke_values has a
     string-as-callable arm (ticket 6). The earlier blanket refusal
     was too conservative. */
  /* Self-tail emission. OP_TAIL_SELF rebinds the inline param slots in place
     and jumps to pc=0, re-running the WHOLE prologue (including any
     let/with-binding LOAD+BIND_SLOT_NAMED setup) from the fresh params. The
     l_tail_self handler first unrefs ALL inline slots (params + let-added) and
     resets n_inline=nparams, so the re-run prologue cleanly re-grows the let
     slots with no refcount drift, and the skipped trailing UNBIND_SLOT(s) are
     harmless. This makes it safe inside a let/with body — but ONLY when the
     lambda body cannot create an env-capturing closure or push a try handler
     (see body_capture_unsafe): in-place env mutation would otherwise corrupt
     an escaped closure or grow the handler stack unboundedly. When unsafe, we
     keep OP_TAIL_CALL (correct, just 2.9x slower in the let case). */
  int is_self_tail =
      tail && c->self_name && (c->nlet_depth == 0 || !c->capture_unsafe) &&
      issymbol(head) && strcmp(exp_text(head), c->self_name) == 0;
  /* Cross-function tail call is safe regardless of nlet_depth:
     OP_TAIL_CALL wholesale releases current env's inline slots. */
  int is_cross_tail = tail && !is_self_tail;
  /* Fused LOAD_GLOBAL+CALL: if head is a symbol that isn't a local
     slot and isn't the self-tail case, we can skip the LOAD_GLOBAL
     dispatch + PUSH/POP and call via the gcache directly. */
  int use_call_global = 0, global_idx = -1;
  if (!is_self_tail && !is_cross_tail && issymbol(head) &&
      find_slot(c, exp_text(head)) < 0) {
    global_idx = add_const(c, head);
    if (global_idx < 0) {
      c->failed = 1;
      return;
    }
    use_call_global = 1;
  }
  if (!is_self_tail && !use_call_global) {
    compile_expr(c, head, 0);
    if (c->failed)
      return;
  }
  for (a = form->next; a; a = a->next) {
    compile_expr(c, a->content, 0);
    if (c->failed)
      return;
    nargs++;
  }
  if (nargs > 255) {
    c->failed = 1;
    return;
  }
  if (is_self_tail) {
    emit_u8(c, OP_TAIL_SELF);
    emit_u8(c, (uint8_t)nargs);
  } else if (is_cross_tail) {
    emit_u8(c, OP_TAIL_CALL);
    emit_u8(c, (uint8_t)nargs);
  } else if (use_call_global) {
    emit_u8(c, OP_CALL_GLOBAL);
    emit_u8(c, (uint8_t)global_idx);
    emit_u8(c, (uint8_t)nargs);
  } else {
    emit_u8(c, OP_CALL);
    emit_u8(c, (uint8_t)nargs);
  }
}

/* Superinstruction fuse table — maps a plain binary op to its fused
   slot-op-fix variant. Returns 0 when no fuse exists for this op. */
static int fuse_slot_fix(int op) {
  switch (op) {
  case OP_ADD:
    return OP_SLOT_ADD_FIX;
  case OP_SUB:
    return OP_SLOT_SUB_FIX;
  case OP_LT:
    return OP_SLOT_LT_FIX;
  case OP_LE:
    return OP_SLOT_LE_FIX;
  case OP_GT:
    return OP_SLOT_GT_FIX;
  case OP_GE:
    return OP_SLOT_GE_FIX;
  case OP_IS:
    return OP_SLOT_IS_FIX;
  default:
    return 0;
  }
}

static void compile_arith(compiler_t *c, exp_t *form, int op) {
  /* Binary left-fold: (+ a b c d) → a b + c + d + */
  exp_t *a = form->next;
  if (!a || !a->next) {
    c->failed = 1;
    return;
  }
  if ((op == OP_LT || op == OP_GT || op == OP_LE || op == OP_GE) &&
      a->next->next) {
    c->failed = 1;
    return;
  }
  exp_t *arg1 = a->content;
  exp_t *arg2 = a->next->content;
  int is_binary = !a->next->next;

  /* Canonicalize (+ K slot) → (+ slot K) for commutative ops so the
     slot-fix peephole and JIT shape matchers see the canonical form.
     Binary-only — the peephole below doesn't handle >2 args anyway. */
  if (is_binary && (op == OP_ADD || op == OP_MUL || op == OP_IS) &&
      isnumber(arg1) && issymbol(arg2)) {
    exp_t *tmp = arg1;
    arg1 = arg2;
    arg2 = tmp;
  }

  /* Peephole: exactly 2 args, arg1 is a local slot symbol, arg2 is a
     fixnum fitting in int16. Emit one fused op instead of three. */
  if (is_binary) {
    int fused = fuse_slot_fix(op);
    if (fused && issymbol(arg1) && isnumber(arg2)) {
      int slot = find_slot(c, exp_text(arg1));
      int64_t v = FIX_VAL(arg2);
      if (slot >= 0 && v >= INT16_MIN && v <= INT16_MAX) {
        emit_u8(c, (uint8_t)fused);
        emit_u8(c, (uint8_t)slot);
        emit_i16(c, (int16_t)v);
        return;
      }
    }
  }

  compile_expr(c, arg1, 0);
  if (c->failed)
    return;
  compile_expr(c, arg2, 0);
  if (c->failed)
    return;
  emit_u8(c, (uint8_t)op);
  for (a = a->next->next; a; a = a->next) {
    compile_expr(c, a->content, 0);
    if (c->failed)
      return;
    emit_u8(c, (uint8_t)op);
  }
}

/* (= sym val) — only when sym resolves to a local slot. Global / car /
   cdr / string-index assignment stays in the tree-walker. */
static void compile_assign(compiler_t *c, exp_t *form, int tail) {
  (void)tail;
  exp_t *key = cadr(form);
  exp_t *val = caddr(form);
  if (!issymbol(key)) {
    c->failed = 1;
    return;
  }
  int slot = find_slot(c, exp_text(key));
  compile_expr(c, val, 0);
  if (c->failed)
    return;
  if (slot >= 0) {
    emit_u8(c, OP_STORE_SLOT);
    emit_u8(c, (uint8_t)slot);
    /* STORE_SLOT re-pushes the stored value so (= x v) returns v. */
    return;
  }
  /* Not a local slot: a captured free var (mutable closure) or a global.
     Store via a runtime env-chain walk that matches updatebang's `=`
     semantics. This is what lets mutable closures compile to bytecode
     instead of failing here and falling back to AST. A reserved-name
     target is rejected at runtime by OP_STORE_FREE (slot < 0 always,
     since reserved names can't bind to a slot), same as OP_SETQ_DYN. */
  int k = add_const(c, key);
  if (c->failed)
    return;
  emit_u8(c, OP_STORE_FREE);
  emit_u8(c, (uint8_t)k);
}

/* (let var val body ...) — single binding, evaluates body in extended
   scope. Destructuring (let (a b) val body) falls back to AST (var is a
   pair, not a symbol). Falls back if slot count would overflow. */
/* Emit N PARALLEL bindings (let/with semantics): every value is compiled while
   none of the new names is in scope yet — so a value referencing a name also
   bound here resolves to the OUTER binding, matching the tree-walker (which
   evaluates all values in the enclosing env). The incremental "bind as you go"
   approach would instead make later values see earlier ones (let* semantics) —
   a real AST-vs-VM divergence. Compile all values first (stack: v0..v(n-1)),
   then pop-bind v(n-1)->slot(n-1) down to v0->slot0, register names, run body.
 */
static void compile_parallel_let(compiler_t *c, exp_t **vars, exp_t **vals,
                                 int n, exp_t *body, int tail) {
  int start = c->nslots;
  if (start + n > ENV_INLINE_SLOTS) {
    c->failed = 1;
    return;
  }
  for (int i = 0; i < n; i++) {
    if (!issymbol(vars[i])) {
      c->failed = 1;
      return;
    }
    compile_expr(c, vals[i], 0); /* outer scope: new names not yet registered */
    if (c->failed)
      return;
  }
  for (int i = n - 1; i >= 0; i--) { /* stack top is v(n-1) → bind it first */
    c->slot_names[start + i] = (char *)exp_text(vars[i]);
    emit_bind_named(c, start + i, vars[i]);
    if (c->failed)
      return;
  }
  c->nslots = start + n;
  c->nlet_depth++;
  compile_body_seq(c, body, tail);
  if (c->failed)
    return;
  c->nlet_depth--;
  for (int i = n - 1; i >= 0; i--) {
    emit_u8(c, OP_UNBIND_SLOT);
    emit_u8(c, (uint8_t)(start + i));
  }
  c->nslots -= n;
}

static void compile_let(compiler_t *c, exp_t *form, int tail) {
  exp_t *first = cadr(form);
  let_shape_t shape = let_classify(first);

  if (shape == LET_SINGLE) {
    exp_t *var = first;
    exp_t *val = caddr(form);
    exp_t *body =
        form->next && form->next->next ? form->next->next->next : NULL;
    if (!body || c->nslots >= ENV_INLINE_SLOTS) {
      c->failed = 1;
      return;
    }
    int slot = c->nslots;
    compile_expr(c, val, 0);
    if (c->failed)
      return;
    emit_bind_named(c, slot, var);
    if (c->failed)
      return;
    c->slot_names[slot] = (char *)exp_text(var);
    c->nslots++;
    c->nlet_depth++;
    compile_body_seq(c, body, tail);
    if (c->failed)
      return;
    c->nlet_depth--;
    c->nslots--;
    /* Body's value is on the stack; the binding's owning ref is still in
       the slot. UNBIND_SLOT unrefs and NULLs it, leaving the result. */
    emit_u8(c, OP_UNBIND_SLOT);
    emit_u8(c, (uint8_t)slot);
    return;
  }

  /* FLAT (x 5 y 6) / CLOJURE ((x 5) (y 6)) — parallel bindings, then body.
     DESTRUCTURE / BAD defer to the AST. */
  if (shape != LET_FLAT && shape != LET_CLOJURE) {
    c->failed = 1;
    return;
  }
  exp_t *body = form->next ? form->next->next : NULL;
  if (!body) {
    c->failed = 1;
    return;
  }
  exp_t *vars[ENV_INLINE_SLOTS], *vals[ENV_INLINE_SLOTS];
  int n = 0;
  for (exp_t *p = (first && first->content) ? first : NULL; p && p->content;) {
    if (n >= ENV_INLINE_SLOTS) {
      c->failed = 1;
      return;
    }
    if (shape == LET_CLOJURE) {
      vars[n] = p->content->content; /* (name val) */
      vals[n] = p->content->next->content;
      p = p->next;
    } else {
      vars[n] = p->content; /* name val name val */
      if (!p->next) {
        c->failed = 1;
        return;
      }
      vals[n] = p->next->content;
      p = p->next->next;
    }
    n++;
  }
  compile_parallel_let(c, vars, vals, n, body, tail);
}

/* (with (v1 e1 v2 e2 ...) body) — N PARALLEL bindings then body: each value is
   evaluated against the enclosing env (a value doesn't see earlier v's in the
   same with), matching the tree-walker's withcmd. Routed through the shared
   parallel emitter so the VM matches the AST (the old incremental emit was
   sequential — a with whose value referenced a sibling binding diverged). */
static void compile_with(compiler_t *c, exp_t *form, int tail) {
  exp_t *pairs = cadr(form);
  exp_t *body = form->next ? form->next->next : NULL;
  if (!ispair(pairs) || !body) {
    c->failed = 1;
    return;
  }
  exp_t *vars[ENV_INLINE_SLOTS], *vals[ENV_INLINE_SLOTS];
  int n = 0;
  for (exp_t *p = pairs; p && p->content;) {
    if (n >= ENV_INLINE_SLOTS || !p->next) {
      c->failed = 1;
      return;
    }
    vars[n] = p->content;
    vals[n] = p->next->content;
    n++;
    p = p->next->next;
  }
  compile_parallel_let(c, vars, vals, n, body, tail);
}

/* (let* (v1 e1 v2 e2 ...) body ...) — sequential bindings: each val sees
   the slots bound by earlier pairs. The flat legacy form
   (let* v1 e1 ... single-body) is left to the tree-walker. */
static void compile_letstar(compiler_t *c, exp_t *form, int tail) {
  exp_t *first = cadr(form);
  if (!ispair(first)) { /* flat legacy form — defer to AST */
    c->failed = 1;
    return;
  }
  exp_t *body = form->next ? form->next->next : NULL;
  if (!body) {
    c->failed = 1;
    return;
  }
  int start_slot = c->nslots;
  int nbindings = 0;
  exp_t *p = first;
  while (p && p->content) {
    exp_t *var = p->content;
    exp_t *nxt = p->next;
    if (!nxt || !issymbol(var)) {
      c->failed = 1;
      return;
    }
    if (c->nslots >= ENV_INLINE_SLOTS) {
      c->failed = 1;
      return;
    }
    /* Compile val BEFORE registering this var's slot name, but AFTER
       earlier vars are visible — that gives let*'s sequential scope. */
    compile_expr(c, nxt->content, 0);
    if (c->failed)
      return;
    emit_bind_named(c, c->nslots, var);
    if (c->failed)
      return;
    c->slot_names[c->nslots] = (char *)exp_text(var);
    c->nslots++;
    nbindings++;
    p = nxt->next;
  }
  c->nlet_depth++;
  compile_body_seq(c, body, tail);
  if (c->failed)
    return;
  c->nlet_depth--;
  int i;
  for (i = nbindings - 1; i >= 0; i--) {
    emit_u8(c, OP_UNBIND_SLOT);
    emit_u8(c, (uint8_t)(start_slot + i));
  }
  c->nslots -= nbindings;
}

/* (when cond body ...) / (unless cond body ...). For `when`, the body runs
   when cond is truthy; for `unless`, when cond is falsey. The other case
   yields nil. `negate` selects unless semantics. */
static void compile_when_unless(compiler_t *c, exp_t *form, int tail,
                                int negate) {
  exp_t *cond = cadr(form);
  exp_t *body = form->next ? form->next->next : NULL;
  if (!cond) {
    c->failed = 1;
    return;
  }
  compile_expr(c, cond, 0);
  if (c->failed)
    return;
  emit_u8(c, OP_BR_IF_FALSE);
  int patch_false = c->ncode;
  emit_i16(c, 0);
  /* Fall-through path = cond truthy. For `when` that runs the body; for
     `unless` it yields nil. The false-branch target is the opposite. */
  exp_t *run = negate ? NULL : body;  /* truthy path */
  exp_t *skip = negate ? body : NULL; /* falsey path */
  if (run)
    compile_body_seq(c, run, tail);
  else {
    emit_nil(c);
  }
  if (c->failed)
    return;
  emit_u8(c, OP_JUMP);
  int patch_end = c->ncode;
  emit_i16(c, 0);
  int false_target = c->ncode;
  if (skip)
    compile_body_seq(c, skip, tail);
  else {
    emit_nil(c);
  }
  if (c->failed)
    return;
  int end_target = c->ncode;
  patch_i16(c, patch_false, false_target);
  patch_i16(c, patch_end, end_target);
}

/* Backpatch helper: write the i16 at code[patch..patch+1] as the relative
   offset from end-of-operand (patch+2) to `target`. Mirrors the open-coded
   fixups in compile_if/compile_when_unless. */
static void patch_i16(compiler_t *c, int patch, int target) {
  int16_t off = (int16_t)(target - (patch + 2));
  c->code[patch] = off & 0xff;
  c->code[patch + 1] = (off >> 8) & 0xff;
}

/* Cap on conditional branches we can backpatch per form. Each clause needs
   one fixup slot; 64 is far past any hand-written and/or/cond/case. Above it
   we bail to the AST tree-walker (c->failed) rather than truncate. */
#define COND_MAX_PATCHES 64

/* (and a b ... z) / (or a b ... z) — short-circuit, *value-preserving*.
   `and` returns the first falsy operand (else the last); `or` the first
   truthy (else the last). The deciding value must survive the branch, but
   OP_BR_IF_* pop their test — so DUP first: the branch pops one copy and,
   on short-circuit, jumps with the other still live; on fall-through we POP
   that survivor and evaluate the next operand. The final operand needs no
   guard and inherits the caller's tail flag, so a tail call inside
   `(and ... (f x))` keeps O(1)-stack TCO instead of bailing to AST. */
static void compile_and_or(compiler_t *c, exp_t *form, int tail, int is_or) {
  exp_t *args = cdr(form);
  if (!args) { /* (and) -> t ; (or) -> nil */
    int k = add_const(c, is_or ? NIL_EXP : TRUE_EXP);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
    return;
  }
  uint8_t br = is_or ? OP_BR_IF_TRUE : OP_BR_IF_FALSE;
  int patches[COND_MAX_PATCHES];
  int npatch = 0;
  for (exp_t *a = args; a; a = a->next) {
    int is_last = (a->next == NULL);
    compile_expr(c, a->content, is_last && tail);
    if (c->failed)
      return;
    if (is_last)
      break;
    if (npatch >= COND_MAX_PATCHES) {
      c->failed = 1;
      return;
    }
    emit_u8(c, OP_DUP);
    emit_u8(c, br);
    patches[npatch++] = c->ncode;
    emit_i16(c, 0);
    emit_u8(c, OP_POP); /* fall-through: drop the survivor, try next operand */
  }
  int end_target = c->ncode;
  for (int i = 0; i < npatch; i++)
    patch_i16(c, patches[i], end_target);
}

/* (cond t1 e1 t2 e2 ... [default]) — Arc-style flat cond. A lone trailing
   element is the unconditional default; with no default and every test
   falsy the result is nil. Tests compile in non-tail position; the selected
   expr (and the default) inherit the caller's tail flag. Uses only the
   existing popping branch — tests aren't returned, so no DUP needed. */
static void compile_cond(compiler_t *c, exp_t *form, int tail) {
  exp_t *cur = cdr(form);
  if (!cur) { /* (cond) -> nil */
    emit_nil(c);
    return;
  }
  int end_patches[COND_MAX_PATCHES];
  int n_end = 0;
  int had_default = 0;
  while (cur) {
    if (!cur->next) { /* lone trailing default */
      compile_expr(c, cur->content, tail);
      if (c->failed)
        return;
      had_default = 1;
      break;
    }
    compile_expr(c, cur->content, 0); /* test */
    if (c->failed)
      return;
    emit_u8(c, OP_BR_IF_FALSE);
    int patch_next = c->ncode;
    emit_i16(c, 0);
    compile_expr(c, cur->next->content, tail); /* paired expr */
    if (c->failed)
      return;
    if (n_end >= COND_MAX_PATCHES) {
      c->failed = 1;
      return;
    }
    emit_u8(c, OP_JUMP);
    end_patches[n_end++] = c->ncode;
    emit_i16(c, 0);
    patch_i16(c, patch_next, c->ncode); /* false -> next clause */
    cur = cur->next->next;
  }
  if (!had_default) { /* no clause matched -> nil */
    emit_nil(c);
  }
  int end_target = c->ncode;
  for (int i = 0; i < n_end; i++)
    patch_i16(c, end_patches[i], end_target);
}

/* (case key v1 e1 v2 e2 ... [default]) — Arc-style flat pairs. The vN are
   UNEVALUATED literals matched to the evaluated key via isequal (OP_IS,
   exactly as casecmd does). The key is evaluated once, then DUP'd per
   comparison; the selected expr (or trailing default) inherits the tail
   flag. Every path discards the saved key before leaving its value. */
static void compile_case(compiler_t *c, exp_t *form, int tail) {
  exp_t *key = cadr(form);
  if (!key) {
    c->failed = 1;
    return;
  }
  compile_expr(c, key, 0); /* stack: [key] */
  if (c->failed)
    return;
  exp_t *cur = cddr(form); /* first vN */
  int end_patches[COND_MAX_PATCHES];
  int n_end = 0;
  int had_default = 0;
  while (cur) {
    if (!cur->next) {     /* lone trailing default */
      emit_u8(c, OP_POP); /* discard saved key */
      compile_expr(c, cur->content, tail);
      if (c->failed)
        return;
      had_default = 1;
      break;
    }
    emit_u8(c, OP_DUP);                 /* [key, key] */
    int k = add_const(c, cur->content); /* literal vN */
    if (c->failed)
      return;
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k); /* [key, key, vN] */
    emit_u8(c, OP_IS);      /* [key, bool] (isequal) */
    emit_u8(c, OP_BR_IF_FALSE);
    int patch_next = c->ncode; /* false -> next clause, [key] */
    emit_i16(c, 0);
    emit_u8(c, OP_POP); /* matched: drop saved key, eval paired expr */
    compile_expr(c, cur->next->content, tail);
    if (c->failed)
      return;
    if (n_end >= COND_MAX_PATCHES) {
      c->failed = 1;
      return;
    }
    emit_u8(c, OP_JUMP);
    end_patches[n_end++] = c->ncode;
    emit_i16(c, 0);
    patch_i16(c, patch_next, c->ncode);
    cur = cur->next->next;
  }
  if (!had_default) { /* no match -> drop key, result nil */
    emit_u8(c, OP_POP);
    emit_nil(c);
  }
  int end_target = c->ncode;
  for (int i = 0; i < n_end; i++)
    patch_i16(c, end_patches[i], end_target);
}

/* (for counter start end body...) — counter iterates start..end inclusive.
   Matches AST forcmd semantics: the body's final expression of the
   final iteration becomes the for's return value; nil if the loop
   never runs or the body is empty.

   Stack discipline: a "current result" sits on the stack across
   iterations (initially nil). Each iter POPs it before running the
   body, and the body's last expression leaves the new result. Exit
   branch takes the BR_IF_FALSE path with the result on top. */
static void compile_for(compiler_t *c, exp_t *form, int tail) {
  (void)tail;
  exp_t *var_node = form->next;
  if (!var_node) {
    c->failed = 1;
    return;
  }
  exp_t *var = var_node->content;
  exp_t *start_node = var_node->next;
  if (!start_node) {
    c->failed = 1;
    return;
  }
  exp_t *end_node = start_node->next;
  if (!end_node) {
    c->failed = 1;
    return;
  }
  exp_t *body_node = end_node->next;

  if (!issymbol(var)) {
    c->failed = 1;
    return;
  }
  if (c->nslots + 2 > ENV_INLINE_SLOTS) {
    c->failed = 1;
    return;
  }

  int counter_slot = c->nslots;
  int end_slot = c->nslots + 1;

  compile_expr(c, start_node->content, 0);
  if (c->failed)
    return;
  emit_bind_named(c, counter_slot, var);
  if (c->failed)
    return;
  c->slot_names[counter_slot] = (char *)exp_text(var);
  c->nslots++;

  compile_expr(c, end_node->content, 0);
  if (c->failed)
    return;
  emit_u8(c, OP_BIND_SLOT); /* hidden end-value slot: nameless, NULL key */
  emit_u8(c, (uint8_t)end_slot);
  c->slot_names[end_slot] = NULL;
  c->nslots++;

  /* Seed the loop's "current result" with nil so an un-entered or
     empty-body for still returns something at exit. */
  int k_nil = add_const(c, nil_singleton);
  emit_u8(c, OP_LOAD_CONST);
  emit_u8(c, (uint8_t)k_nil);

  c->nlet_depth++;
  int loop_top = c->ncode;

  /* Fused slot-vs-slot compare: replaces LOAD_SLOT+LOAD_SLOT+LE with
     one dispatch. Saves 2 dispatches per iteration in the hot loop. */
  emit_u8(c, OP_SLOT_LE_SLOT);
  emit_u8(c, (uint8_t)counter_slot);
  emit_u8(c, (uint8_t)end_slot);
  emit_u8(c, OP_BR_IF_FALSE);
  int patch_exit = c->ncode;
  emit_i16(c, 0);

  if (body_node) {
    /* Replace previous iteration's result with this one's. */
    emit_u8(c, OP_POP);
    exp_t *b;
    for (b = body_node; b; b = b->next) {
      compile_expr(c, b->content, 0);
      if (c->failed)
        return;
      if (b->next)
        emit_u8(c, OP_POP); /* discard non-last body exprs */
    }
    /* Last body expr's value remains on stack as the new "current result". */
  }

  emit_u8(c, OP_LOAD_SLOT);
  emit_u8(c, (uint8_t)counter_slot);
  emit_u8(c, OP_LOAD_FIX);
  emit_i16(c, 1);
  emit_u8(c, OP_ADD);
  emit_u8(c, OP_STORE_SLOT);
  emit_u8(c, (uint8_t)counter_slot);
  emit_u8(c, OP_POP);

  emit_u8(c, OP_JUMP);
  int patch_jump = c->ncode;
  emit_i16(c, 0);

  int loop_end = c->ncode;

  patch_i16(c, patch_exit, loop_end);
  patch_i16(c, patch_jump, loop_top);

  c->nlet_depth--;

  emit_u8(c, OP_UNBIND_SLOT);
  emit_u8(c, (uint8_t)end_slot);
  emit_u8(c, OP_UNBIND_SLOT);
  emit_u8(c, (uint8_t)counter_slot);
  c->nslots -= 2;
  /* Result left on top of stack by the last iteration (or the seed nil). */
}

/* Sentinel const index meaning "no handler" / "no finally" in OP_PUSH_HANDLER.
 */
#define HANDLER_NONE 0xff

/* (try body handler [finally]) compiled to the heap-handler-stack model — ONLY
   when the try is in TAIL POSITION of the compiled lambda. We emit:
       OP_PUSH_HANDLER handler_idx finally_idx
       <body, compiled in tail position>
   The body's value (or tail call) flows out; OP_RET / the VM's error router
   runs the handler and/or finally and unwinds the handler stack (see vm_run).
   Because the try is in tail position there is no code after the body in this
   lambda, so no explicit pop is needed — OP_RET unwinds. The handler and
   finally forms are stored in the const pool and evaluated LAZILY by the VM
   (matching trycmd's AST semantics: a nil-literal handler means no-catch;
   handler/finally eval can itself error and that surfaces).

   NOT in tail position → return 0 so compile_expr bails the whole lambda to AST
   (trycmd). That keeps the risky path narrow: only tail-position try (the deep-
   recursion shape) uses the new machinery; every other try keeps today's exact,
   already-correct behavior. */
static int compile_try(compiler_t *c, exp_t *form, int tail) {
  if (!tail)
    return 0; /* non-tail try → bail to AST */
  /* (try body handler [finally]) — need at least body + handler. */
  exp_t *body = cadr(form);
  exp_t *handler = caddr(form);
  if (!form->next || !form->next->next)
    return 0; /* malformed → let trycmd report it */
  exp_t *finally = cdddr(form) ? cadddr(form) : NULL;
  if (cdddr(form) && cdr(cdddr(form)))
    return 0; /* too many args → AST path reports / handles */

  /* Store handler form in the const pool. A literal nil handler = no-catch;
     encode it as the nil const so the VM applies the same rule as trycmd. */
  int h_idx = add_const(c, handler ? handler : nil_singleton);
  if (c->failed || h_idx >= HANDLER_NONE)
    return 0;
  int f_idx = HANDLER_NONE;
  if (finally) {
    f_idx = add_const(c, finally);
    if (c->failed || f_idx >= HANDLER_NONE)
      return 0;
  }
  emit_u8(c, OP_PUSH_HANDLER);
  emit_u8(c, (uint8_t)h_idx);
  emit_u8(c, (uint8_t)f_idx);
  /* Committed to the VM path now (OP_PUSH_HANDLER emitted). Body in TAIL
     position: its tail call trampolines, so deep try-per-level recursion runs
     in O(1) C stack. If the body fails to compile, c->failed is set and we
     return 1 anyway — the caller must NOT fall through to OP_EVAL_AST (that
     would leave a dangling OP_PUSH_HANDLER); c->failed bails the whole lambda
     to the AST path, which is correct and safe. */
  compile_expr(c, body, 1);
  return 1;
}

/* Special-form ids for compile_expr's head dispatch. The strcmp ladder it used
   to be is now a sorted-table lookup + switch: one bsearch (log n) instead of
   up to ~30 sequential strcmps for every call head, and the table is the single
   source of the recognized-form name set. SF_NONE → not a compiled special form
   (fall through to op_for_head / the reserved_symbol tier, exactly as a
   non-matching ladder arm did). Multiple spellings can map to one id
   (=/setf → SF_ASSIGN, max/min → SF_MAXMIN). */
enum sform_id {
  SF_NONE = 0,
  SF_IF,
  SF_TRY,
  SF_LET,
  SF_LETSTAR,
  SF_WITH,
  SF_WHEN,
  SF_UNLESS,
  SF_AND,
  SF_OR,
  SF_COND,
  SF_CASE,
  SF_FOR,
  SF_ASSIGN,
  SF_SETQ,
  SF_DO,
  SF_CONS,
  SF_CAR,
  SF_CDR,
  SF_LIST,
  SF_VEC_REF,
  SF_VEC_SET,
  SF_VEC_LEN,
  SF_VEC,
  SF_SQRT_INT,
  SF_LENGTH,
  SF_ABS,
  SF_MAXMIN,
  SF_QUOTE
};
/* MUST stay sorted by name (bsearch). */
static const struct {
  const char *name;
  int id;
} sform_table[] = {
    {"=", SF_ASSIGN},          {"abs", SF_ABS},         {"and", SF_AND},
    {"car", SF_CAR},           {"case", SF_CASE},       {"cdr", SF_CDR},
    {"cond", SF_COND},         {"cons", SF_CONS},       {"do", SF_DO},
    {"for", SF_FOR},           {"if", SF_IF},           {"length", SF_LENGTH},
    {"let", SF_LET},           {"let*", SF_LETSTAR},    {"list", SF_LIST},
    {"max", SF_MAXMIN},        {"min", SF_MAXMIN},      {"or", SF_OR},
    {"quote", SF_QUOTE},       {"setf", SF_ASSIGN},     {"setq", SF_SETQ},
    {"sqrt-int", SF_SQRT_INT}, {"try", SF_TRY},         {"unless", SF_UNLESS},
    {"vec", SF_VEC},           {"vec-len", SF_VEC_LEN}, {"vec-ref", SF_VEC_REF},
    {"vec-set!", SF_VEC_SET},  {"when", SF_WHEN},       {"with", SF_WITH}};
static int sform_cmp(const void *k, const void *e) {
  return strcmp((const char *)k, ((const typeof(sform_table[0]) *)e)->name);
}
static int sform_lookup(const char *s) {
  const void *hit =
      bsearch(s, sform_table, sizeof sform_table / sizeof sform_table[0],
              sizeof sform_table[0], sform_cmp);
  return hit ? ((const typeof(sform_table[0]) *)hit)->id : SF_NONE;
}

static void compile_expr(compiler_t *c, exp_t *e, int tail) {
  if (c->failed)
    return;
  emit_loc(c,
           e); /* stamp pc→source line for this form (cold; error-path only) */
  /* Tagged fixnum literal: if it fits in int16, inline; else const pool. */
  if (isnumber(e)) {
    int64_t v = FIX_VAL(e);
    if (v >= INT16_MIN && v <= INT16_MAX) {
      emit_u8(c, OP_LOAD_FIX);
      emit_i16(c, (int16_t)v);
    } else {
      int k = add_const(c, e);
      emit_u8(c, OP_LOAD_CONST);
      emit_u8(c, (uint8_t)k);
    }
    return;
  }
  if (!is_ptr(e)) {
    /* tagged char or other immediate */
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
    return;
  }
  if (isstring(e) || isfloat(e)) {
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
    return;
  }
  if (e == nil_singleton || e == true_singleton) {
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_CONST);
    emit_u8(c, (uint8_t)k);
    return;
  }
  if (issymbol(e)) {
    /* A keyword (:foo) self-evaluates — mirror evaluate()'s keyword arm
       (the `exp_text(e)[0] == ':'` check there). Without this, a keyword used
       as a value in a compiled body — e.g. (def f () :hello) or any body whose
       tail expression is a bare keyword — would emit OP_LOAD_GLOBAL and fail at
       runtime with "Unbound variable :hello". As an argument it already worked
       (it round-trips through the const pool); this fixes the value/return
       position. */
    const char *nm = exp_text(e);
    if (nm[0] == ':') {
      int k = add_const(c, e);
      emit_u8(c, OP_LOAD_CONST);
      emit_u8(c, (uint8_t)k);
      return;
    }
    int slot = find_slot(c, exp_text(e));
    if (slot >= 0) {
      emit_u8(c, OP_LOAD_SLOT);
      emit_u8(c, (uint8_t)slot);
      return;
    }
    /* Global / builtin. Runtime lookup via the constant (the symbol
       itself is the key — lookup will cache on it via meta). */
    int k = add_const(c, e);
    emit_u8(c, OP_LOAD_GLOBAL);
    emit_u8(c, (uint8_t)k);
    return;
  }
  if (!ispair(e)) {
    c->failed = 1;
    return;
  }

  /* Call form. Dispatch on head. */
  exp_t *head = car(e);
  /* Infix -> prefix when the head is statically non-callable (see
     compile_infix_rewrite). Done before special-form dispatch — but those have
     a keyword head, never a hinted param / sub-expression / literal, so they're
     untouched. */
  {
    exp_t *pfx = compile_infix_rewrite(c, e);
    if (pfx) {
      compile_expr(c, pfx, tail);
      unrefexp(pfx);
      return;
    }
  }
  if (issymbol(head)) {
    const char *s = exp_text(head);
    switch (sform_lookup(s)) {
    case SF_IF: {
      compile_if(c, e, tail);
      return;
      break;
    }
    case SF_TRY: {
      /* Only tail-position try uses the handler-stack VM path. A non-tail try
         (compile_try returns 0) falls through to the reserved-symbol
         OP_EVAL_AST path below — just THAT sub-form defers to trycmd while the
         enclosing lambda stays compiled (and keeps its own tail call), exactly
         as before this change. So non-tail try keeps today's exact behavior. */
      if (compile_try(c, e, tail))
        return;
      break;
    }
    case SF_LET: {
      compile_let(c, e, tail);
      return;
      break;
    }
    case SF_LETSTAR: {
      compile_letstar(c, e, tail);
      return;
      break;
    }
    case SF_WITH: {
      compile_with(c, e, tail);
      return;
      break;
    }
    case SF_WHEN: {
      compile_when_unless(c, e, tail, 0);
      return;
      break;
    }
    case SF_UNLESS: {
      compile_when_unless(c, e, tail, 1);
      return;
      break;
    }
    case SF_AND: {
      compile_and_or(c, e, tail, 0);
      return;
      break;
    }
    case SF_OR: {
      compile_and_or(c, e, tail, 1);
      return;
      break;
    }
    case SF_COND: {
      compile_cond(c, e, tail);
      return;
      break;
    }
    case SF_CASE: {
      compile_case(c, e, tail);
      return;
      break;
    }
    case SF_FOR: {
      compile_for(c, e, tail);
      return;
      break;
    }
    case SF_ASSIGN: {
      compile_assign(c, e, tail);
      return;
      break;
    }
    case SF_SETQ: {
      exp_t *args = cdr(e);
      if (!args) { /* (setq) -> nil; let the tree-walker handle it */
        c->failed = 1;
        return;
      }
      for (exp_t *a = args; a; a = cddr(a)) {
        exp_t *sym = car(a);
        if (!issymbol(sym) || !cdr(a)) { /* malformed: defer to evaluator */
          c->failed = 1;
          return;
        }
        compile_expr(c, cadr(a), 0);
        if (c->failed)
          return;
        int slot = find_slot(c, exp_text(sym));
        if (slot >= 0) {
          emit_u8(c, OP_STORE_SLOT);
          emit_u8(c, (uint8_t)slot);
        } else {
          int k = add_const(c, sym);
          if (c->failed)
            return;
          emit_u8(c, OP_SETQ_DYN);
          emit_u8(c, (uint8_t)k);
        }
        /* Each pair leaves its value on the stack; setq's result is the
           last pair's value, so discard the earlier ones. */
        if (cddr(a))
          emit_u8(c, OP_POP);
      }
      return;
      break;
    }
    case SF_DO: {
      /* Sequential eval, return last value. Same shape as the body
         walk in compile_lambda — emit each expr, POP between, last
         one keeps tail position. (do) with no exprs evaluates to nil. */
      exp_t *b = e->next;
      if (!b) {
        emit_nil(c);
        return;
      }
      int saw_any = 0;
      for (; b; b = b->next) {
        if (saw_any)
          emit_u8(c, OP_POP);
        int is_last = (b->next == NULL);
        compile_expr(c, b->content, is_last && tail);
        if (c->failed)
          return;
        saw_any = 1;
      }
      return;
      break;
    }
    case SF_CONS: {
      exp_t *a = cadr(e), *b = caddr(e);
      if (!a || !b) {
        c->failed = 1;
        return;
      }
      compile_expr(c, a, 0);
      if (c->failed)
        return;
      compile_expr(c, b, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_CONS);
      return;
      break;
    }
    case SF_CAR: {
      exp_t *a = cadr(e);
      if (!a) {
        c->failed = 1;
        return;
      }
      compile_expr(c, a, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_CAR);
      return;
      break;
    }
    case SF_CDR: {
      exp_t *a = cadr(e);
      if (!a) {
        c->failed = 1;
        return;
      }
      compile_expr(c, a, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_CDR);
      return;
      break;
    }
    case SF_LIST: {
      int n = 0;
      exp_t *a;
      for (a = e->next; a; a = a->next) {
        compile_expr(c, a->content, 0);
        if (c->failed)
          return;
        n++;
        if (n > 255) {
          c->failed = 1;
          return;
        }
      }
      emit_u8(c, OP_LIST);
      emit_u8(c, (uint8_t)n);
      return;
      break;
    }
    case SF_VEC_REF: {
      exp_t *v = cadr(e), *i = caddr(e);
      if (!v || !i) {
        c->failed = 1;
        return;
      }
      compile_expr(c, v, 0);
      if (c->failed)
        return;
      compile_expr(c, i, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_VEC_REF);
      return;
      break;
    }
    case SF_VEC_SET: {
      exp_t *v = cadr(e), *i = caddr(e), *x = cadddr(e);
      if (!v || !i || !x) {
        c->failed = 1;
        return;
      }
      compile_expr(c, v, 0);
      if (c->failed)
        return;
      compile_expr(c, i, 0);
      if (c->failed)
        return;
      compile_expr(c, x, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_VEC_SET);
      return;
      break;
    }
    case SF_VEC_LEN: {
      exp_t *v = cadr(e);
      if (!v) {
        c->failed = 1;
        return;
      }
      compile_expr(c, v, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_VEC_LEN);
      return;
      break;
    }
    case SF_VEC: {
      exp_t *n = cadr(e), *init = caddr(e);
      if (!n) {
        c->failed = 1;
        return;
      }
      compile_expr(c, n, 0);
      if (c->failed)
        return;
      if (init)
        compile_expr(c, init, 0);
      else {
        emit_nil(c);
      }
      if (c->failed)
        return;
      emit_u8(c, OP_VEC_NEW);
      return;
      break;
    }
    case SF_SQRT_INT: {
      exp_t *a = cadr(e);
      if (!a) {
        c->failed = 1;
        return;
      }
      compile_expr(c, a, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_SQRT_INT);
      return;
      break;
    }
    case SF_LENGTH: {
      exp_t *a = cadr(e);
      if (!a) {
        c->failed = 1;
        return;
      }
      compile_expr(c, a, 0);
      if (c->failed)
        return;
      emit_u8(c, OP_LENGTH);
      return;
      break;
    }
    /* abs (unary) / max,min (binary) get native opcodes instead of the
       OP_EVAL_AST tree-walk — the common arity. Wrong/variadic arity falls
       through to OP_EVAL_AST below (where abscmd/maxcmd/mincmd handle it,
       including variadic max/min over >2 args). */
    case SF_ABS: {
      exp_t *a = cadr(e);
      if (a && !cddr(e)) {
        compile_expr(c, a, 0);
        if (c->failed)
          return;
        emit_u8(c, OP_ABS);
        return;
      }
      break;
    }
    case SF_MAXMIN: {
      exp_t *a = cadr(e), *b = caddr(e);
      if (a && b && !cdddr(e)) {
        compile_expr(c, a, 0);
        if (c->failed)
          return;
        compile_expr(c, b, 0);
        if (c->failed)
          return;
        emit_u8(c, !strcmp(s, "max") ? OP_NMAX : OP_NMIN);
        return;
      }
      break;
    }
    case SF_QUOTE: {
      exp_t *q = cadr(e);
      int k = add_const(c, q ? q : nil_singleton);
      emit_u8(c, OP_LOAD_CONST);
      emit_u8(c, (uint8_t)k);
      return;
      break;
    }
    default:
      break;
    }
    int op = op_for_head(s);
    if (op >= 0) {
      if (op == OP_NOT) {
        /* Unary: (no x) */
        if (!e->next) {
          c->failed = 1;
          return;
        }
        compile_expr(c, e->next->content, 0);
        if (c->failed)
          return;
        emit_u8(c, OP_NOT);
        return;
      }
      compile_arith(c, e, op);
      return;
    }
    /* A head that resolves to an EXP_INTERNAL we haven't whitelisted above is a
       builtin with no native opcode. Two sub-cases:
         - TAIL_AWARE special forms (match, for-gen, …) and closure-creating
           forms (fn/lambda/def/defn/defc/defmacro/macro): keep the
       wholesale-AST fallback. TAIL_AWARE forms need the tree-walker's own
       tail-marker trampoline for their tail recursion; closure-creators would
       capture THIS compiled frame's env (whose inline slots die on return) and
           dangle. Both must run with the whole lambda as AST.
         - everything else (max, abs, string-*, reverse, map, …): a value
           sub-expression, never the tail. Emit OP_EVAL_AST so just THIS form
           defers to the tree-walker while the enclosing lambda stays compiled
           (and keeps its tail call). Fixes the deep-recursion segfault any
           non-whitelisted builtin in a hot body used to cause.
       OP_EVAL_AST resolves the sub-form's free vars BY NAME via the
       tree-walker; params (param_keys) plus let/with/for-counter locals (now
       bound with OP_BIND_SLOT_NAMED) plus captures (parent chain) are all
       name-resolvable, so it is safe even inside a let/with/for body. User
       lambdas (not in reserved_symbol) fall through to compile_call. */
    keyval_t *kv = set_get_keyval_dict(reserved_symbol, (char *)s, NULL);
    if (kv && isinternal(kv->val)) {
      /* Applicative module builtin (alcove_register_cmd, non-tail-aware): emit
         a real OP_CALL_GLOBAL so a hot loop calling it isn't a per-call AST
         tree-walk. Try the fast compile; if an argument won't compile (e.g. a
         (fn ...) literal that would mis-capture this frame, or a const-pool
         overflow), roll the emit + consts back and fall through to OP_EVAL_AST
         — identical behavior to before, just for that call. tail=0: a builtin
         call never self-TCOs, and OP_TAIL_CALL rejects non-lambdas. */
      if (kv->val->flags & FLAG_APPLICATIVE) {
        int save_ncode = c->ncode, save_nconsts = c->nconsts;
        compile_call(c, e, 0);
        if (!c->failed)
          return;
        c->failed = 0;
        for (int z = save_nconsts; z < c->nconsts; z++)
          unrefexp(c->consts[z]);
        c->nconsts = save_nconsts;
        c->ncode = save_ncode;
        /* fall through to the OP_EVAL_AST emit below */
      } else if ((kv->val->flags & FLAG_TAIL_AWARE) || !strcmp(s, "fn") ||
                 !strcmp(s, "lambda") || !strcmp(s, "def") ||
                 !strcmp(s, "defn") || !strcmp(s, "defc") ||
                 !strcmp(s, "defmacro") || !strcmp(s, "macro")) {
        c->failed = 1;
        return;
      }
      int k = add_const(c, e);
      if (c->failed)
        return;
      emit_u8(c, OP_EVAL_AST);
      emit_u8(c, (uint8_t)k);
      return;
    }
    /* A head bound to a user macro (defmacro) — not a reserved form and not
       shadowed by a local — must be EXPANDED at compile time and the expansion
       compiled, exactly as evaluate() does in the tree-walker. Without this the
       compiler emits a call to the macro object, which fails at runtime with
       "not a lambda" (OP_TAIL_CALL/Bytecode call). find_slot() < 0 ensures a
       local of the same name still shadows the macro, matching lexical lookup.
     */
    if (find_slot(c, (char *)s) < 0 && g_global_env && g_global_env->d) {
      keyval_t *mkv = set_get_keyval_dict(g_global_env->d, (char *)s, NULL);
      if (mkv && mkv->val && ismacro(mkv->val)) {
        exp_t *expanded = expandmacro(e, mkv->val, g_global_env);
        if (!expanded || iserror(expanded)) {
          if (expanded)
            unrefexp(expanded);
          c->failed = 1;
          return;
        }
        compile_expr(c, expanded,
                     tail); /* recurses: nested macros expand too */
        unrefexp(expanded);
        return;
      }
    }
    compile_call(c, e, tail);
    return;
  }
  /* Complex head — fall back. */
  c->failed = 1;
}

/* Conservative compile-time predicate: does this body subtree contain any
   construct that could (at runtime) create a closure capturing the current
   call env, OR push a try handler? Such a body is "capture-unsafe": running
   OP_TAIL_SELF (which mutates env->inline_vals in place and jumps to pc=0)
   would corrupt an escaped closure's captured bindings, and would re-push a
   try handler every iteration. We therefore disable the let-body OP_TAIL_SELF
   relaxation (below) when this returns true, falling back to OP_TAIL_CALL.

   The denylist symbols are the only env-capturing / handler-pushing forms:
     - fn/lambda/def/defn/defc/defmacro/macro: build a closure over env. In a
       directly-compiled body they already force a full-lambda AST bail
       (compile_expr line ~6281), but they can ALSO ride along inside an
       OP_EVAL_AST sub-form (e.g. (map (fn (x) ...) xs)), where the fn IS
       compiled-out yet still captures env at AST-eval time. Scanning the raw
       AST catches both.
     - try: compiles to OP_PUSH_HANDLER, which snapshots env onto the handler
       stack; a TAIL_SELF jump-to-pc=0 would re-push it unboundedly.
   Anything not in this list (arith, if/do/cond/case/and/or, user fn calls,
   let/with/for, value-only OP_EVAL_AST builtins like reverse/map-of-no-fn)
   provably does not retain env, so in-place rebinding is safe. */
static int body_capture_unsafe(exp_t *e) {
  if (!e || !is_ptr(e))
    return 0;
  if (issymbol(e))
    return 0;
  if (!ispair(e))
    return 0;
  exp_t *head = car(e);
  if (issymbol(head)) {
    const char *s = exp_text(head);
    if (!strcmp(s, "fn") || !strcmp(s, "lambda") || !strcmp(s, "def") ||
        !strcmp(s, "defn") || !strcmp(s, "defc") || !strcmp(s, "defmacro") ||
        !strcmp(s, "macro") || !strcmp(s, "try"))
      return 1;
  }
  /* Recurse over every element of this list (head + args, and nested
     lists). content holds the element; next is the rest of the list. */
  for (exp_t *p = e; p; p = p->next) {
    if (body_capture_unsafe(p->content))
      return 1;
  }
  return 0;
}

int compile_lambda(exp_t *fn, int is_closure, const uint8_t *param_hints,
                   uint8_t ret_hint) {
  if (!fn || !islambda(fn))
    return 0;
  if (g_no_compile)
    return 0; /* --interpret: force AST tree-walker (differential testing) */
  if (fn->flags & FLAG_COMPILED)
    return 1; /* idempotent */
  exp_t *params = fn->content;
  exp_t *body = fn->next->content;
  compiler_t c = {0};
  c.self_name = (const char *)fn->meta; /* may be NULL for anon fn */
  c.param_hints = param_hints; /* borrowed; for infix->prefix rewrite */
  /* One-shot body pre-pass: decide whether the let-body OP_TAIL_SELF
     relaxation is permitted for this whole lambda (order-independent — a
     capturing form anywhere, incl. a let val re-run each iteration, must
     veto it). */
  for (exp_t *bb = body; bb; bb = bb->next) {
    if (body_capture_unsafe(bb->content)) {
      c.capture_unsafe = 1;
      break;
    }
  }

  /* Register params into slots 0..N-1 matching env->inline_slots.
     Rest params (dot notation or bare-symbol wrap) fall back to AST.
     Empty params `()` are the nil_singleton sentinel — a pair with NULL
     content — so the `p->content` guard (matching var2env) terminates
     the loop with nparams == 0 instead of misreading it as a malformed
     param and failing. Without it, no 0-arg function would ever compile. */
  exp_t *p;
  for (p = params; p && p->content; p = p->next) {
    if (c.nparams >= ENV_INLINE_SLOTS) {
      c.failed = 1;
      break;
    }
    if (!is_ptr(p->content) || !issymbol(p->content)) {
      c.failed = 1;
      break;
    }
    /* Dot marker means rest params — AST eval handles collection. */
    if (strcmp((char *)exp_text(p->content), ".") == 0) {
      c.failed = 1;
      break;
    }
    c.slot_names[c.nparams++] = (char *)exp_text(p->content);
  }
  c.nslots = c.nparams;

  /* Walk body list: each expression, pop between, except the last. */
  exp_t *b;
  int saw_any = 0;
  for (b = body; b && !c.failed; b = b->next) {
    if (saw_any)
      emit_u8(&c, OP_POP);
    int is_last = (b->next == NULL);
    compile_expr(&c, b->content, is_last);
    saw_any = 1;
  }
  if (!saw_any) {
    c.failed = 1;
  }
  if (!c.failed)
    emit_u8(&c, OP_RET);

  if (c.failed) {
    int i;
    for (i = 0; i < c.nconsts; i++)
      unrefexp(c.consts[i]);
    free(c.consts);
    free(c.code);
    free(c.locs);
    return 0;
  }
  bytecode_t *bc = calloc(1, sizeof(bytecode_t));
  /* Migrate the params list off fn->content onto bc->content. After
     `fn->bc = bc` below, the union assignment overwrites fn->content
     with bc, so this is an ownership *transfer* — bytecode_free will
     unrefexp(bc->content) at end of life. Don't refexp here. */
  bc->content = fn->content;
  bc->code = c.code;
  bc->ncode = c.ncode;
  bc->consts = c.consts;
  bc->nconsts = c.nconsts;
  /* Cache param info so vm_invoke_values doesn't re-walk fn->content
     on every call. The keys are borrowed pointers into the param-
     symbol ->ptr fields, kept alive by the lambda's ref on its
     header. */
  bc->nparams = (uint8_t)c.nparams;
  for (int pi = 0; pi < c.nparams; pi++) {
    bc->param_keys[pi] = c.slot_names[pi];
    bc->param_hints[pi] = param_hints ? param_hints[pi] : TYPE_HINT_NONE;
  }
  bc->ret_hint = ret_hint;
  bc->self_name = (const char *)fn->meta; /* borrowed; NULL for anon */
  bc->no_gcache = (uint8_t)(is_closure != 0);
  /* Eager-allocate the global-resolution cache (instead of lazily on first
     miss) so concurrent reactors sharing this bytecode — a registered RESP
     command under --threads — never race on `if (!bc->gcache) bc->gcache =
     calloc(...)`: that lazy init can leak a duplicate allocation and, on a weak
     memory model (arm64), publish the pointer before its zero-fill is visible,
     letting a peer read a garbage {val,gen} and refexp() a wild pointer. With
     it pre-zeroed here, only the per-slot {val,gen} writes remain and a torn
     read merely misses the cache and re-resolves. One calloc per compiled
     non-closure that has consts; the runtime fast path is unchanged. */
  if (!bc->no_gcache && bc->nconsts > 0)
    bc->gcache = calloc((size_t)bc->nconsts, sizeof(gcache_entry));
  bc->locs = c.locs; /* transfer the pc→source-location table (may be NULL) */
  bc->nlocs = c.nlocs;
  fn->bc = bc;
  fn->flags |= FLAG_COMPILED;
#ifdef ALCOVE_JIT
  /* Don't JIT closures: the JIT's global-call helpers cache via gcache,
     which is unsafe for a closure's captured free vars. Bytecode (with
     no_gcache fresh lookups) is still far faster than AST eval. */
  if (!is_closure)
    jit_compile(bc); /* opportunistic; no-op for shapes we don't recognize */
#endif
  return 1;
}

/* "Callable index" — the family of types that support (container i) as a
   read, mirroring the (string i) sugar. Keeping the membership test and the
   element fetch in one place means every call site (the AST evaluator's two
   head paths, vm_invoke_values, and the OP_TAIL_CALL fallback) stays in sync,
   and a new indexable type is a one-line addition here rather than four. */
static inline int isindexable(exp_t *e) {
  return isstring(e) || isvector(e) || isblob(e);
}
/* Keyed containers answer (m k) as a key lookup (Clojure-style), distinct
   from the integer indexing of isindexable: dict/hamt -> value (nil if
   absent), set -> the member (nil if absent). */
static inline int iskeyed(exp_t *e) {
  return isdict(e) || isset(e) || ishamt(e);
}
/* List-like: a non-empty pair chain (EXP_PAIR) or a deque (EXP_LIST). These
   answer (lst i) as nth — 0-based, nil out of range — so a list reads
   index-style like a vector, just O(n). nil is excluded on purpose: calling
   nil as a function stays a loud error (it is almost always a bug), rather
   than silently indexing the empty list to nil. */
static inline int islistlike(exp_t *e) {
  return e != NIL_EXP && (ispair(e) || islist(e));
}
/* Any value that supports (container arg) as a read. */
static inline int iscallable_container(exp_t *e) {
  return isindexable(e) || iskeyed(e) || islistlike(e);
}
/* Apply a callable container to one already-evaluated argument, consuming
   `arg`'s ref. Indexable -> element by integer index; keyed -> value/member
   by key. Returns an owned result (nil for an absent key) or an error.
   Defined after the HAMT ops since it needs hamt_node_get. */
static exp_t *container_apply(exp_t *c, exp_t *arg, env_t *env);
/* Fetch element i (0-based) of an indexable container. Returns an owned ref
   (vector cell) or a fresh immediate (string -> char, blob -> byte fixnum),
   or NULL when i is out of range / negative. Caller guarantees isindexable(c)
   and that i came from a validated integer. */
static exp_t *index_get(exp_t *c, int64_t i) {
  if (i < 0)
    return NULL;
  if (isstring(c)) {
    uint32_t cp;
    return utf8_index(exp_text(c), i, &cp) ? make_char(cp) : NULL;
  }
  if (isvector(c))
    return (i < vec_len(c)) ? vec_get_boxed(c, i) : NULL;
  /* blob: one byte, 0..255, as a fixnum (matches blob-ref). */
  return ((size_t)i < blob_len(c))
             ? MAKE_FIX((int64_t)(unsigned char)blob_bytes(c)[i])
             : NULL;
}

/* Run one popped try's finally form (entry) in its snapshot env. Returns the
   finally's OWNED error value if it errored or escaped (so the caller can let
   it take priority), else NULL (normal finally value is discarded — like
   trycmd). */
static exp_t *vm_run_finally(const vm_handler_t *entry) {
  if (!entry->finally_form)
    return NULL;
  env_t *e = vm_handler_make_env(entry);
  exp_t *fret = EVAL(entry->finally_form, e);
  destroy_env(e);
  if (fret && iserror(fret))
    return fret; /* error OR escape token — let caller decide priority */
  if (fret)
    unrefexp(fret);
  return NULL;
}

/* Release a popped handler entry's snapshot refs (root chain + slot values). */
static void vm_handler_release(vm_handler_t *h) {
  for (int i = 0; i < h->n_inline; i++)
    unrefexp(h->inline_vals[i]);
  if (h->root)
    destroy_env(h->root);
}

/* Unwind the heap handler stack from its current top down to `handler_base`,
   resolving an in-flight value `v` (OWNED). Two regimes, both running every
   popped try's finally exactly once:
     - while v is a REAL error (catchable): each popped try gets to catch it —
       run its handler form (nil handler = no-catch → propagate; handler that
       re-raises → its error continues to the next-out handler). Once a handler
       returns a normal value, v becomes that value and we switch to the
       finally-only regime for the remaining (outer) handlers, whose protected
       bodies completed normally with v.
     - while v is normal OR an is_cont_escape token: handlers are NOT run (no
       catchable error reached them); only their finally runs. An escape token
       passes straight through to handler_base. A finally that itself errors/
       escapes takes priority over v (matches trycmd's finally rule).
   Returns the resolved OWNED value to hand back to the VM (which either returns
   it to its C caller, or — if still an error and handler_base is this vm_run's
   entry depth — propagates it out). Each entry's handler/finally run in a
   short-lived env rebuilt from that try's snapshotted bindings. */
static exp_t *vm_unwind_handlers(exp_t *v, int handler_base) {
  while (g_handler_sp > handler_base) {
    vm_handler_t *entry = &g_handlers[g_handler_sp - 1];
    if (v && iserror(v) && !is_cont_escape(v)) {
      /* Catchable error: this try may handle it. Evaluate the handler form
         lazily (a nil-literal handler means no-catch), exactly like trycmd.
         Eval AND apply within one temp-env scope: the handler closure captures
         this env, so it must outlive the apply, then tear down LIFO. */
      env_t *he = vm_handler_make_env(entry);
      exp_t *handler = EVAL(entry->handler_form, he);
      if (!handler || handler == NIL_EXP) {
        /* no-catch: propagate v past this try (finally still runs) */
        if (handler)
          unrefexp(handler);
      } else if (iserror(handler)) {
        /* handler eval itself failed/escaped — that replaces v */
        unrefexp(v);
        v = handler;
      } else {
        bt_clear(); /* error handled here — drop its captured backtrace */
        exp_t *res = alc_apply1(handler, v, he);
        unrefexp(handler);
        unrefexp(v);
        v = res ? res : NIL_EXP;
        /* v is now whatever the handler produced: a normal value (caught), or
           a re-raised error (continues to the next-out handler). */
      }
      destroy_env(he);
    }
    /* finally runs on EVERY path (normal, caught, re-raised, escape-through).
     */
    exp_t *ferr = vm_run_finally(entry);
    if (ferr) {
      /* finally errored/escaped: it takes priority over the current value. */
      if (v)
        unrefexp(v);
      v = ferr;
    }
    vm_handler_release(entry);
    g_handler_sp--;
  }
  return v ? v : NIL_EXP;
}

/* Bounded Levenshtein edit distance between `a` and `b`. Early-exits with
   `max+1` once a whole row exceeds `max` (or the length gap alone does), so
   the common "not close" case is cheap. Names are short; bail on long ones. */
static int alc_edit_distance(const char *a, const char *b, int max) {
  int la = (int)strlen(a), lb = (int)strlen(b);
  int gap = la > lb ? la - lb : lb - la;
  if (gap > max)
    return max + 1;
  if (lb >= 64 || la >= 64)
    return max + 1;
  int prev[64], cur[64];
  for (int j = 0; j <= lb; j++)
    prev[j] = j;
  for (int i = 1; i <= la; i++) {
    cur[0] = i;
    int rowmin = cur[0];
    for (int j = 1; j <= lb; j++) {
      int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      int del = prev[j] + 1, ins = cur[j - 1] + 1, sub = prev[j - 1] + cost;
      int m = del < ins ? del : ins;
      if (sub < m)
        m = sub;
      cur[j] = m;
      if (m < rowmin)
        rowmin = m;
    }
    if (rowmin > max)
      return max + 1;
    memcpy(prev, cur, sizeof(int) * (lb + 1));
  }
  return prev[lb];
}

/* "did you mean?" support: scan the builtins (reserved_symbol) + the global
   env chain for the symbol name closest to `name` by edit distance, within a
   small length-scaled threshold. Returns a BORROWED pointer to the best
   candidate's stored key (stable for the process), or NULL if nothing close.
   Exact (distance-0) matches are skipped — they wouldn't be "unbound". */
static const char *alc_suggest_symbol(const char *name, env_t *env) {
  if (!name)
    return NULL;
  int nl = (int)strlen(name);
  if (nl <= 2)
    return NULL; /* too short — suggestions would be noise */
  int thr = (nl <= 4) ? 1 : 2;
  int best = thr + 1;
  const char *bestname = NULL;
#define ALC_CONSIDER(K)                                                        \
  do {                                                                         \
    const char *_k = (K);                                                      \
    if (_k && *_k) {                                                           \
      int _d = alc_edit_distance(name, _k, thr);                               \
      if (_d >= 1 && _d < best) {                                              \
        best = _d;                                                             \
        bestname = _k;                                                         \
      }                                                                        \
    }                                                                          \
  } while (0)
  /* builtins */
  if (reserved_symbol) {
    for (unsigned h = 0; h < 2; h++)
      for (unsigned j = 0; j < reserved_symbol->ht[h].size; j++)
        for (keyval_t *kv = reserved_symbol->ht[h].table[j]; kv; kv = kv->next)
          ALC_CONSIDER((const char *)kv->key);
  }
  /* global env chain (inline slots + dict at each frame) */
  for (env_t *cur = env; cur; cur = cur->root) {
    for (int i = 0; i < cur->n_inline; i++)
      ALC_CONSIDER(cur->inline_keys[i]);
    if (cur->d)
      for (unsigned h = 0; h < 2; h++)
        for (unsigned j = 0; j < cur->d->ht[h].size; j++)
          for (keyval_t *kv = cur->d->ht[h].table[j]; kv; kv = kv->next)
            ALC_CONSIDER((const char *)kv->key);
  }
#undef ALC_CONSIDER
  return (best <= thr) ? bestname : NULL;
}

/* Exact-arithmetic fallback for the VM's inline add/sub/mul/div ops when an
   operand is a rational or decimal (not a fixnum/float). The inline ops know
   fixnum and float shapes; coercing the tower to double would diverge from the
   AST evaluator's EXACT result (1/2, 1.5m). Route through the very same builtin
   (pluscmd/minuscmd/multiplycmd/dividecmd) the AST uses, via alc_apply_n, so
   the compiled and interpreted results are byte-identical. Consumes a and b.
   Returns an owned value or an error exp. The builtin internals are looked up
   once and cached (reserved_symbol never rebinds these). */
static exp_t *vm_arith_tower(char opchar, exp_t *a, exp_t *b, env_t *env) {
  static exp_t *cache[4]; /* +, -, *, / */
  int slot = opchar == '+' ? 0 : opchar == '-' ? 1 : opchar == '*' ? 2 : 3;
  if (!cache[slot]) {
    const char *nm = slot == 0 ? "+" : slot == 1 ? "-" : slot == 2 ? "*" : "/";
    keyval_t *kv = set_get_keyval_dict(reserved_symbol, (char *)nm, NULL);
    cache[slot] = kv ? (exp_t *)kv->val : NULL;
  }
  if (!cache[slot]) {
    unrefexp(a);
    unrefexp(b);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "Illegal value in operation");
  }
  exp_t *argv[2] = {a, b}; /* alc_apply_n consumes both refs */
  return alc_apply_n(cache[slot], 2, argv, env);
}

/* ---- infix dispatch: (VALUE op VALUE) -> (op VALUE VALUE) -----------------
   When the head of a 3-element form evaluates to a NON-callable value (a number
   etc.) and the operator position holds one of these binary builtins, evaluate
   it as infix. Gating on "head is non-callable" is what makes this safe: a HOF
   call like (apply + xs) has a function head, so it never becomes infix. This
   lives only in the cold not-a-function dispatch branch, so hot calls pay
   nothing. Both the AST evaluator and the VM funnel through is_infix_op +
   infix_apply, so the two tiers stay byte-identical (equiv_sweep/jit-fuzz). */
static const char *const g_infix_names[] = {
    "+", "-", "*", "/", "<", ">", "<=", ">=", "is", "iso", "isnt", "mod"};
#define N_INFIX_OPS ((int)(sizeof g_infix_names / sizeof g_infix_names[0]))
static exp_t *g_infix_ops[N_INFIX_OPS];
/* If v is one of the cached binary-operator builtins, return its index in
   g_infix_names; else -1. Identity check against the (lazily cached) internals
   reserved_symbol holds — those never rebind. */
static int infix_op_index(exp_t *v) {
  if (!v || !is_ptr(v) || !isinternal(v))
    return -1;
  if (!g_infix_ops[0])
    for (int i = 0; i < N_INFIX_OPS; i++) {
      keyval_t *kv =
          set_get_keyval_dict(reserved_symbol, (char *)g_infix_names[i], NULL);
      g_infix_ops[i] = kv ? (exp_t *)kv->val : NULL;
    }
  for (int i = 0; i < N_INFIX_OPS; i++)
    if (v == g_infix_ops[i])
      return i;
  return -1;
}
/* Wrap an evaluated value as a form element: self-evaluating values pass
   through, but a symbol or pair would re-resolve / be applied, so quote it.
   Returns an owned ref (the new node content). Mirrors alc_apply_n. */
static exp_t *infix_wrap_value(exp_t *v) {
  if (v && is_ptr(v) && (issymbol(v) || ispair(v)))
    return make_quote(refexp(v));
  return refexp(v);
}
/* Apply infix operator (by index) to (head_val, rhs_val): build the real form
   (OP-SYMBOL head rhs) and evaluate it, so the operator's own builtin sees the
   operator SYMBOL in head position (cmpcmd decodes < > <= >= from it) — exactly
   as a hand-written (op a b) would. head_val/rhs_val borrowed; returns owned.
 */
static exp_t *infix_apply(int op_idx, exp_t *head_val, exp_t *rhs_val,
                          env_t *env) {
  exp_t *opsym = make_symbol((char *)g_infix_names[op_idx],
                             (int)strlen(g_infix_names[op_idx]));
  exp_t *form = make_node(opsym);
  form->next = make_node(infix_wrap_value(head_val));
  form->next->next = make_node(infix_wrap_value(rhs_val));
  return evaluate(form, env); /* consumes form + the wrapped value refs */
}
/* True when `v` is a real function — a builtin (internal) or a lambda/closure —
   i.e. something alc_apply_n can call as `(v a b)`. Used to extend infix beyond
   the fixed operator set: `a f b` -> (f a b) for ANY function f, while leaving
   strings/dicts (callable only as indexers) and special-form-only values out.
 */
static int infix_is_fn(exp_t *v) {
  return v && is_ptr(v) && (isinternal(v) || islambda(v));
}
/* General infix: apply an arbitrary binary function VALUE as (op head rhs).
   alc_apply_n quote-protects symbol/list arg values, so head_val/rhs_val pass
   through safely. op/head_val/rhs_val borrowed; returns an owned result. */
static exp_t *infix_apply_fn(exp_t *op, exp_t *head_val, exp_t *rhs_val,
                             env_t *env) {
  exp_t *argv[2] = {refexp(head_val), refexp(rhs_val)};
  return alc_apply_n(op, 2, argv, env); /* consumes argv; not op */
}
/* Container-head infix: a string/dict/etc. is callable as an indexer, so a form
   like (s starts-with? "t") would otherwise try to index s. When the form is
   exactly (container op rhs) and `op` (the ALREADY-evaluated first arg) is an
   operator or a function, evaluate it as infix (op container rhs) and return
   the owned result with *matched=1; otherwise *matched=0 and the caller indexes
   the container by op as before. container/op borrowed; rhs is e's 3rd element,
   evaluated here. */
static exp_t *ast_container_infix(exp_t *container, exp_t *op, exp_t *e,
                                  env_t *env, int *matched) {
  *matched = 0;
  if (!e->next || !e->next->next || e->next->next->next)
    return NULL; /* not exactly (container op rhs) */
  int oi = infix_op_index(op);
  if (oi < 0 && !infix_is_fn(op))
    return NULL; /* op isn't an operator or function — it's a real index */
  *matched = 1;
  int outer_tail = in_tail_position;
  in_tail_position = 0;
  exp_t *rhs = EVAL(e->next->next->content, env);
  in_tail_position = outer_tail;
  if (iserror(rhs))
    return rhs;
  exp_t *res = oi >= 0 ? infix_apply(oi, container, rhs, env)
                       : infix_apply_fn(op, container, rhs, env);
  unrefexp(rhs);
  return res;
}

/* Bytecode dispatch loop. Entered with `env` already populated (params
   in inline slots). Returns an owned exp_t* (or NULL).
   OP_TAIL_CALL re-enters via goto tail_reentry with a fresh fn —
   `fn_owned` tracks whether we took ownership of the post-tail fn (so
   we can unref it on final return or error). */
exp_t *vm_run(exp_t *fn, env_t *env) {
#define VM_STACK_MAX 256
  exp_t *stack[VM_STACK_MAX];
  bytecode_t *bc;
  uint8_t *code;
  exp_t **consts;
  int sp;
  int pc;
  int fn_owned = 0;
  /* This vm_run owns only handlers pushed at or above this depth. On final
     return (OP_RET / error / escape) it unwinds back to here, running each
     popped try's finally. Set ONCE at entry — NOT reset on tail_reentry, so
     handlers pushed by a try-per-level recursion accumulate across trampolines
     (correct: they're all live until the base case returns). */
  int handler_base = g_handler_sp;

  /* Stack-overflow guard: a nested vm_run (a non-tail call into another
     compiled body) is a fresh C frame; bail with a catchable error before the
     C stack overflows. Placed before tail_reentry so the in-frame tail
     trampoline (which adds no C frame) is never charged. Nothing is pushed yet
     (sp would be 0) and fn is borrowed (fn_owned == 0), so just unwind any
     enclosing handlers with the error — an outer (try ...) still catches it. */
  if (stack_guard_exhausted()) {
    exp_t *_err = error(ERROR_ILLEGAL_VALUE, fn, env,
                        "stack overflow: recursion too deep (use tail "
                        "recursion, or raise the OS stack limit)");
    return vm_unwind_handlers(_err, handler_base);
  }

tail_reentry:
  bc = fn->bc;
  code = bc->code;
  consts = bc->consts;
  sp = 0;
  pc = 0;

/* Map the faulting code offset to its source line/col so the error reports the
   precise failing form (the VM path; cold). error() already cleared g_err_line
   via the lambda's (absent) line, so a miss leaves the top-level-form fallback.
 */
#define RUNTIME_ERR_LOC                                                        \
  do {                                                                         \
    int _el, _ec;                                                              \
    if (bc_loc_at(bc, pc, &_el, &_ec)) {                                       \
      g_err_line = _el;                                                        \
      g_err_col = _ec;                                                         \
      errmeta_set_loc(_err, _el, _ec); /* per-error copy for introspection */  \
    }                                                                          \
  } while (0)
/* RUNTIME_ERR_C carries the machine-readable error CLASS. The class must
   match what the AST tier raises for the same failure (div-by-zero,
   missing-parameter, index-out-of-range, ...) — handlers dispatch on
   (error-code e), and the equiv sweep compares printed OUTPUT, so a code
   divergence between tiers is invisible to it (this hid the div-by-zero
   class turning into illegal-value through every compiled call). */
#define RUNTIME_ERR_C(errnum, msg)                                             \
  do {                                                                         \
    exp_t *_err = error(errnum, fn, env, msg);                                 \
    RUNTIME_ERR_LOC;                                                           \
    int _i;                                                                    \
    for (_i = 0; _i < sp; _i++)                                                \
      unrefexp(stack[_i]);                                                     \
    if (fn_owned)                                                              \
      unrefexp(fn);                                                            \
    return vm_unwind_handlers(_err, handler_base);                             \
  } while (0)
#define RUNTIME_ERR(msg) RUNTIME_ERR_C(ERROR_ILLEGAL_VALUE, msg)
/* Like RUNTIME_ERR but with two printf args — for arity errors, so the VM
   wording matches the AST path's "too few/many arguments to NAME"
   (class: missing-parameter, same as the AST arity path). */
#define RUNTIME_ERR_FMT(fmt, a1, a2)                                           \
  do {                                                                         \
    exp_t *_err = error(ERROR_MISSING_PARAMETER, fn, env, fmt, a1, a2);        \
    RUNTIME_ERR_LOC;                                                           \
    int _i;                                                                    \
    for (_i = 0; _i < sp; _i++)                                                \
      unrefexp(stack[_i]);                                                     \
    if (fn_owned)                                                              \
      unrefexp(fn);                                                            \
    return vm_unwind_handlers(_err, handler_base);                             \
  } while (0)
/* Like RUNTIME_ERR but for an unbound symbol: names the symbol and appends a
   "did you mean 'X'?" hint when a near-match exists. `symexp` is the symbol. */
#define RUNTIME_ERR_UNBOUND(symexp)                                            \
  do {                                                                         \
    const char *_nm = (const char *)exp_text(symexp);                          \
    const char *_sg = alc_suggest_symbol(_nm, env);                            \
    exp_t *_err =                                                              \
        _sg ? error(ERROR_UNBOUND_VARIABLE, fn, env,                           \
                    "Unbound variable %s (did you mean '%s'?)", _nm, _sg)      \
            : error(ERROR_UNBOUND_VARIABLE, fn, env, "Unbound variable %s",    \
                    _nm);                                                      \
    RUNTIME_ERR_LOC;                                                           \
    int _i;                                                                    \
    for (_i = 0; _i < sp; _i++)                                                \
      unrefexp(stack[_i]);                                                     \
    if (fn_owned)                                                              \
      unrefexp(fn);                                                            \
    return vm_unwind_handlers(_err, handler_base);                             \
  } while (0)
/* Propagate an ALREADY-BUILT error object (e.g. one returned by a builtin the
   VM delegated to) through the handler stack — like RUNTIME_ERR but without
   synthesizing a new error/message. */
#define VM_PROPAGATE_ERR(errexp)                                               \
  do {                                                                         \
    exp_t *_err = (errexp);                                                    \
    RUNTIME_ERR_LOC;                                                           \
    int _i;                                                                    \
    for (_i = 0; _i < sp; _i++)                                                \
      unrefexp(stack[_i]);                                                     \
    if (fn_owned)                                                              \
      unrefexp(fn);                                                            \
    return vm_unwind_handlers(_err, handler_base);                             \
  } while (0)
#define PUSH(v)                                                                \
  do {                                                                         \
    if (sp >= VM_STACK_MAX)                                                    \
      RUNTIME_ERR("VM stack overflow");                                        \
    stack[sp++] = (v);                                                         \
  } while (0)
#define POP() (stack[--sp])
/* Return `v` (OWNED) from this vm_run, routing it through any handlers this run
   accumulated (catch / finally / escape-through). Cheap no-handler fast path.
 */
#define VM_RETURN(v)                                                           \
  return ((g_handler_sp > handler_base)                                        \
              ? vm_unwind_handlers((v), handler_base)                          \
              : (v))
#define READ_U8 (code[pc++])
#define READ_I16 (pc += 2, (int16_t)(code[pc - 2] | (code[pc - 1] << 8)))

  /* Threaded dispatch via GCC/Clang computed goto: each op ends with a
     direct indirect branch to the next op's label. Lets the CPU's
     branch predictor learn per-op successor patterns — measurably
     faster than a single switch-based jump-table on hot loops. */
  static const void *const dispatch[OP_MAX] = {
      [OP_HALT] = &&l_halt,
      [OP_RET] = &&l_ret,
      [OP_POP] = &&l_pop,
      [OP_DUP] = &&l_dup,
      [OP_EVAL_AST] = &&l_eval_ast,
      [OP_BIND_SLOT_NAMED] = &&l_bind_slot_named,
      [OP_LOAD_FIX] = &&l_load_fix,
      [OP_LOAD_CONST] = &&l_load_const,
      [OP_LOAD_SLOT] = &&l_load_slot,
      [OP_LOAD_GLOBAL] = &&l_load_global,
      [OP_STORE_SLOT] = &&l_store_slot,
      [OP_BIND_SLOT] = &&l_bind_slot,
      [OP_UNBIND_SLOT] = &&l_unbind_slot,
      [OP_ADD] = &&l_add,
      [OP_SUB] = &&l_sub,
      [OP_MUL] = &&l_mul,
      [OP_DIV] = &&l_div,
      [OP_MOD] = &&l_mod,
      [OP_LT] = &&l_lt,
      [OP_GT] = &&l_gt,
      [OP_LE] = &&l_le,
      [OP_GE] = &&l_ge,
      [OP_IS] = &&l_is,
      [OP_ISO] = &&l_iso,
      [OP_NOT] = &&l_not,
      [OP_JUMP] = &&l_jump,
      [OP_BR_IF_FALSE] = &&l_br_if_false,
      [OP_BR_IF_TRUE] = &&l_br_if_true,
      [OP_CALL] = &&l_call,
      [OP_CALL_GLOBAL] = &&l_call_global,
      [OP_TAIL_SELF] = &&l_tail_self,
      [OP_TAIL_CALL] = &&l_tail_call,
      [OP_CONS] = &&l_cons,
      [OP_CAR] = &&l_car,
      [OP_CDR] = &&l_cdr,
      [OP_LIST] = &&l_list,
      [OP_SLOT_ADD_FIX] = &&l_slot_add_fix,
      [OP_SLOT_SUB_FIX] = &&l_slot_sub_fix,
      [OP_SLOT_LT_FIX] = &&l_slot_lt_fix,
      [OP_SLOT_LE_FIX] = &&l_slot_le_fix,
      [OP_SLOT_GT_FIX] = &&l_slot_gt_fix,
      [OP_SLOT_GE_FIX] = &&l_slot_ge_fix,
      [OP_SLOT_IS_FIX] = &&l_slot_is_fix,
      [OP_SLOT_LE_SLOT] = &&l_slot_le_slot,
      [OP_VEC_REF] = &&l_vec_ref,
      [OP_VEC_SET] = &&l_vec_set,
      [OP_VEC_LEN] = &&l_vec_len,
      [OP_VEC_NEW] = &&l_vec_new,
      [OP_SQRT_INT] = &&l_sqrt_int,
      [OP_ABS] = &&l_abs,
      [OP_NMAX] = &&l_max,
      [OP_NMIN] = &&l_min,
      [OP_LENGTH] = &&l_length,
      [OP_SETQ_DYN] = &&l_setq_dyn,
      [OP_STORE_FREE] = &&l_store_free,
      [OP_PUSH_HANDLER] = &&l_push_handler,
  };
#ifndef NDEBUG
  /* Catches "added an opcode but forgot to initialize dispatch[]" —
     a designated-init gap silently leaves a slot NULL and would jump
     to 0 on that op. One-time cost at vm_run entry; NDEBUG strips it. */
  {
    int _i;
    for (_i = 0; _i < OP_MAX; _i++)
      assert(dispatch[_i] != NULL);
  }
#endif
#define NEXT goto *dispatch[code[pc++]]

  NEXT;

l_halt:
  RUNTIME_ERR("Bytecode: OP_HALT reached (compiler bug)");

l_ret: {
  exp_t *r = POP();
  while (sp > 0)
    unrefexp(POP());
  if (fn_owned)
    unrefexp(fn);
  /* Normal completion: unwind any handlers this vm_run accumulated (running
     each try's finally) before returning the value. For a try-per-level
     recursion every level's finally runs here as the base value flows out. */
  if (g_handler_sp > handler_base)
    return vm_unwind_handlers(r, handler_base);
  return r;
}

l_pop:
  unrefexp(POP());
  NEXT;

l_dup: {
  exp_t *t = stack[sp - 1];
  PUSH(refexp(t));
  NEXT;
}

l_eval_ast: {
  /* Run one stored raw sub-form through the tree-walker — the escape hatch for
     builtins with no native opcode. in_tail_position must be 0: the VM owns TCO
     and this form is a value sub-expression (never the tail), so a stray tail
     marker would be mis-handled. EVAL consumes the ref it's handed, so pass a
     fresh one; the const keeps its own. lookup() walks inline slots + the
     parent chain, so locals and captures resolve as in full-AST mode; nested
     user-lambda calls re-enter the compiled path via invoke(). */
  uint8_t idx = READ_U8;
  int saved_tail = in_tail_position;
  in_tail_position = 0;
  exp_t *r = EVAL(refexp(consts[idx]), env);
  in_tail_position = saved_tail;
  if (r && iserror(r)) {
    while (sp > 0)
      unrefexp(POP());
    if (fn_owned)
      unrefexp(fn);
    /* Route through this vm_run's handler stack (catchable error or
       escape-through). No active handler → returns r unchanged. */
    if (g_handler_sp > handler_base)
      return vm_unwind_handlers(r, handler_base);
    return r;
  }
  if (!r)
    r = NIL_EXP;
  PUSH(r);
  NEXT;
}

l_push_handler: {
  /* Push a try handler entry: {handler-form, finally-form, env}. The forms live
     in the const pool (borrowed, outlive the call). Index 0xff = none. Emitted
     only for a tail-position try (compile_try); the protected body follows in
     tail position and trampolines, so handlers accumulate on this heap stack
     (O(n) for try-per-level recursion) instead of the C stack. The VM's error
     router (RUNTIME_ERR / OP_CALL* / OP_EVAL_AST error paths) and OP_RET unwind
     these via vm_unwind_handlers. */
  uint8_t h_idx = READ_U8;
  uint8_t f_idx = READ_U8;
  exp_t *hform =
      consts[h_idx]; /* nil_singleton encodes a nil (no-catch) handler */
  exp_t *fform = (f_idx == HANDLER_NONE) ? NULL : consts[f_idx];
  if (!vm_handler_push(hform, fform, env)) {
    /* OOM growing the handler stack: surface as a (catchable) runtime error,
       but there's no room to register THIS handler — route through whatever is
       already active. */
    RUNTIME_ERR("try: out of memory growing handler stack");
  }
  NEXT;
}

l_load_fix: {
  int16_t v = READ_I16;
  PUSH(MAKE_FIX((int64_t)v));
  NEXT;
}
l_load_const: {
  uint8_t idx = READ_U8;
  PUSH(refexp(consts[idx]));
  NEXT;
}
l_load_slot: {
  uint8_t idx = READ_U8;
  PUSH(refexp(env->inline_vals[idx]));
  NEXT;
}
l_load_global: {
  uint8_t idx = READ_U8;
  /* Per-bytecode global cache. The gcache slot stores the last lookup
     result + the generation it was cached at. If alcove_global_gen
     still matches, we skip the env walk + strcmp entirely. fib spends
     ~78% of its time here without this cache. */
  /* Hit-path is unchanged from the no-closure original (zero added cost):
     closures never allocate gcache (the store below is gated on
     !no_gcache), so bc->gcache stays NULL for them and this never hits. */
  if (bc->gcache && bc->gcache[idx].gen == alcove_global_gen) {
    PUSH(refexp(bc->gcache[idx].val));
  } else {
    int is_global;
    exp_t *v = lookup_scoped(consts[idx], env, &is_global);
    if (!v)
      RUNTIME_ERR_UNBOUND(consts[idx]);
    /* Only memoize truly-global resolutions. A local free var (OP_STORE_FREE
       target read back via OP_LOAD_GLOBAL) must NOT be cached — the gcache is
       keyed by global-gen and would serve it stale to a later call. */
    if (!bc->no_gcache && is_global) {
      if (!bc->gcache)
        bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
      bc->gcache[idx].val = v; /* not refcounted by us; bound globally */
      bc->gcache[idx].gen = alcove_global_gen;
    }
    PUSH(v);
  }
  NEXT;
}

l_store_slot: {
  /* (= local val): replace an existing slot's value. */
  uint8_t idx = READ_U8;
  exp_t *v = POP();
  unrefexp(env->inline_vals[idx]);
  env->inline_vals[idx] = v;
  /* Leave the updated value on the stack as the expression's result.
     (= ...) returns the assigned value. */
  PUSH(refexp(v));
  NEXT;
}
l_setq_dyn: {
  /* (setq sym val) for a non-local target: walk the env chain at
     runtime (nearest existing binding, else create top-level). */
  uint8_t idx = READ_U8;
  exp_t *v = POP();
  /* Reject (setq <reserved> v) — consistent with the AST setqcmd path. */
  {
    exp_t *_rerr = NULL;
    REJECT_RESERVED_ASSIGN(consts[idx], _rerr, {
      unrefexp(v);
      for (int _i = 0; _i < sp; _i++)
        unrefexp(stack[_i]);
      if (fn_owned)
        unrefexp(fn);
      VM_RETURN(_rerr);
    });
  }
  /* setq_store_symbol takes its own ref on v (refexp into the binding)
     and does not consume ours — same contract setqcmd relies on — so
     our popped ref becomes the on-stack result. */
  setq_store_symbol(consts[idx], env, v);
  PUSH(v);
  NEXT;
}
l_store_free: {
  /* (= sym val) / (setf sym val) for a non-slot target: a captured free
     var (mutable closure) or a global. Same shape as SETQ_DYN but uses
     assign_store_symbol, which creates in the CURRENT env on not-found
     (matching updatebang) rather than the root env. */
  uint8_t idx = READ_U8;
  exp_t *v = POP();
  {
    exp_t *_rerr = NULL;
    REJECT_RESERVED_ASSIGN(consts[idx], _rerr, {
      unrefexp(v);
      for (int _i = 0; _i < sp; _i++)
        unrefexp(stack[_i]);
      if (fn_owned)
        unrefexp(fn);
      VM_RETURN(_rerr);
    });
  }
  /* Borrows v (refexp into the binding); our popped ref is the result.
     Returns 1 if it REFUSED a global write under the RESP callback guard —
     in that case the binding was NOT written, so v's ref is still ours to
     drop, and we raise the read-only error (same unwind shape as
     REJECT_RESERVED_ASSIGN above). */
  if (assign_store_symbol(consts[idx], env, v)) {
    exp_t *_rerr = resp_cb_readonly_error(env);
    unrefexp(v);
    for (int _i = 0; _i < sp; _i++)
      unrefexp(stack[_i]);
    if (fn_owned)
      unrefexp(fn);
    VM_RETURN(_rerr);
  }
  PUSH(v);
  NEXT;
}
l_bind_slot: {
  /* let/with/for entry: allocate a new inline slot and bump n_inline
     if this is a fresh position. The compiler resolves these names to
     slot indices at compile time, so symbolic lookup never needs to
     find them — but lookup() / updatebang() / dir() walk inline_keys
     in [0, n_inline), so we must write a sentinel here. NULL means
     "skip on symbolic walk"; the slot is still reachable by index. */
  uint8_t idx = READ_U8;
  exp_t *v = POP();
  /* Slot is fresh (compiler guarantees no prior BIND at same idx
     without intervening UNBIND). No old value to unref. */
  env->inline_vals[idx] = v;
  env->inline_keys[idx] = NULL;
  if (idx >= env->n_inline)
    env->n_inline = idx + 1;
  NEXT;
}
l_bind_slot_named: {
  /* Like OP_BIND_SLOT, but records the slot's NAME so an OP_EVAL_AST sub-form
     can resolve this let/with/for-counter local by name (symbolic lookup skips
     NULL-keyed slots). The name points into a const symbol, which outlives the
     frame. lookup() scans inline slots innermost-first, so a same-named inner
     binding correctly shadows an outer one. */
  uint8_t idx = READ_U8;
  uint8_t name_idx = READ_U8;
  exp_t *v = POP();
  env->inline_vals[idx] = v;
  env->inline_keys[idx] = (char *)exp_text(consts[name_idx]);
  if (idx >= env->n_inline)
    env->n_inline = idx + 1;
  NEXT;
}
l_unbind_slot: {
  /* let/with exit: release the binding. destroy_env would catch any
     leftover refs via n_inline, but we explicitly clear here so the
     slot is reusable for subsequent lets. Clear the key too — a freed slot
     must not stay name-visible to a later OP_EVAL_AST symbolic lookup. */
  uint8_t idx = READ_U8;
  unrefexp(env->inline_vals[idx]);
  env->inline_vals[idx] = NULL;
  env->inline_keys[idx] = NULL;
  if (idx + 1 == env->n_inline)
    env->n_inline = idx;
  NEXT;
}

/* Numeric binary op helpers. COERCE_TO_DOUBLE implicitly references
   `a` and `b` in its error path so both operands get unref'd before
   we jump to the error return — not reusable outside BIN_ARITH /
   CMP_OP sites that name their operands `a` and `b`. */
#define COERCE_TO_DOUBLE(v, out, opname)                                       \
  do {                                                                         \
    if (isnumber(v))                                                           \
      (out) = (double)FIX_VAL(v);                                              \
    else if (isfloat(v)) {                                                     \
      (out) = (v)->f;                                                          \
      unref_float(v); /* v is known float here — skip the general dispatch */  \
    } else {                                                                   \
      unrefexp(a);                                                             \
      unrefexp(b);                                                             \
      RUNTIME_ERR(opname);                                                     \
    }                                                                          \
  } while (0)
#define BIN_ARITH(op, opchar, opname)                                          \
  do {                                                                         \
    exp_t *b = POP(), *a = POP();                                              \
    if (isnumber(a) && isnumber(b)) {                                          \
      int64_t _r = FIX_VAL(a);                                                 \
      if (fix_op_ovf((opchar), &_r, FIX_VAL(b)) || !FIX_FITS(_r))              \
        RUNTIME_ERR("integer overflow (no implicit float; use a float, "       \
                    "rational, or decimal)"); /* identical to the AST msg */   \
      PUSH(MAKE_FIX(_r));                                                      \
    } else if (isrational(a) || isdecimal(a) || isrational(b) ||               \
               isdecimal(b)) {                                                 \
      /* exact tower: defer to the AST builtin so VM == AST byte-for-byte */   \
      exp_t *_tr = vm_arith_tower((opchar), a, b, env); /* consumes a,b */     \
      if (_tr && iserror(_tr))                                                 \
        VM_PROPAGATE_ERR(_tr);                                                 \
      PUSH(_tr ? _tr : NIL_EXP);                                               \
    } else {                                                                   \
      double da, db;                                                           \
      COERCE_TO_DOUBLE(a, da, "Illegal value in " opname);                     \
      COERCE_TO_DOUBLE(b, db, "Illegal value in " opname);                     \
      PUSH(make_floatf(da op db));                                             \
    }                                                                          \
  } while (0)

l_add:
  BIN_ARITH(+, '+', "+");
  NEXT;
l_sub:
  BIN_ARITH(-, '-', "-");
  NEXT;
l_mul:
  BIN_ARITH(*, '*', "*");
  NEXT;
l_div: {
  exp_t *b = POP(), *a = POP();
  if (isnumber(a) && isnumber(b)) {
    int64_t bb = FIX_VAL(b);
    if (bb == 0)
      RUNTIME_ERR_C(ERROR_DIV_BY0, "Illegal division by 0");
    PUSH(MAKE_FIX(FIX_VAL(a) / bb));
  } else if (isrational(a) || isdecimal(a) || isrational(b) || isdecimal(b)) {
    /* exact tower division: defer to the AST builtin (VM == AST). */
    exp_t *tr = vm_arith_tower('/', a, b, env); /* consumes a,b */
    if (tr && iserror(tr))
      VM_PROPAGATE_ERR(tr);
    PUSH(tr ? tr : NIL_EXP);
  } else {
    double da, db;
    COERCE_TO_DOUBLE(a, da, "Illegal value in /");
    COERCE_TO_DOUBLE(b, db, "Illegal value in /");
    if (db == 0)
      RUNTIME_ERR_C(ERROR_DIV_BY0, "Illegal division by 0");
    PUSH(make_floatf(da / db));
  }
  NEXT;
}
l_mod: {
  /* Truncated modulo (C99 %), matches modcmd. Lifts (mod a b) from
     a builtin-call-back-to-AST round-trip into one VM dispatch.
     Float operands fall back to fmod for parity with what users
     expect from mathematical modulo. */
  exp_t *b = POP(), *a = POP();
  if (isnumber(a) && isnumber(b)) {
    int64_t bb = FIX_VAL(b);
    if (bb == 0)
      RUNTIME_ERR_C(ERROR_DIV_BY0, ERR_MODULO_BY_ZERO);
    int64_t va = FIX_VAL(a);
    PUSH(MAKE_FIX(va - (va / bb) * bb));
  } else {
    double da, db;
    COERCE_TO_DOUBLE(a, da, "Illegal value in mod");
    COERCE_TO_DOUBLE(b, db, "Illegal value in mod");
    if (db == 0.0)
      RUNTIME_ERR_C(ERROR_DIV_BY0, ERR_MODULO_BY_ZERO);
    PUSH(make_floatf(fmod(da, db)));
  }
  NEXT;
}

/* Integer compares on two fixnums skip the cast-to-double step
   (which would overflow at 61-bit boundaries). Mixed-type paths
   have a/b already unref'd by COERCE_TO_DOUBLE — fixnums are
   immediates and never need unref either, so no trailing cleanup. */
#define CMP_OP(intcmp, flcmp)                                                  \
  do {                                                                         \
    exp_t *b = POP(), *a = POP();                                              \
    int r;                                                                     \
    if ((isnumber(a) || isfloat(a)) && (isnumber(b) || isfloat(b))) {          \
      if (isnumber(a) && isnumber(b))                                          \
        r = FIX_VAL(a) intcmp FIX_VAL(b);                                      \
      else {                                                                   \
        double da = TO_DOUBLE(a);                                              \
        double db = TO_DOUBLE(b);                                              \
        r = da flcmp db;                                                       \
      }                                                                        \
      /* a, b are known number-or-float here — skip the general dispatch. */   \
      unref_number_or_float(a);                                                \
      unref_number_or_float(b);                                                \
    } else {                                                                   \
      double d;                                                                \
      if (!alc_pair_cmp(a, b, &d)) {                                           \
        unrefexp(a);                                                           \
        unrefexp(b);                                                           \
        RUNTIME_ERR(ERR_COMPARE_INCOMPAT);                                     \
      }                                                                        \
      r = d flcmp 0;                                                           \
      unrefexp(a);                                                             \
      unrefexp(b);                                                             \
    }                                                                          \
    PUSH(r ? TRUE_EXP : NIL_EXP);                                              \
  } while (0)

l_lt:
  CMP_OP(<, <);
  NEXT;
l_gt:
  CMP_OP(>, >);
  NEXT;
l_le:
  CMP_OP(<=, <=);
  NEXT;
l_ge:
  CMP_OP(>=, >=);
  NEXT;

l_is: {
  exp_t *b = POP(), *a = POP();
  int r = isequal(a, b);
  unrefexp(a);
  unrefexp(b);
  PUSH(r ? TRUE_EXP : NIL_EXP);
  NEXT;
}
l_iso: {
  exp_t *b = POP(), *a = POP();
  int r = isoequal(a, b);
  unrefexp(a);
  unrefexp(b);
  PUSH(r ? TRUE_EXP : NIL_EXP);
  NEXT;
}
l_not: {
  exp_t *a = POP();
  int r = !istrue(a);
  unrefexp(a);
  PUSH(r ? TRUE_EXP : NIL_EXP);
  NEXT;
}

l_jump: {
  int16_t off = READ_I16;
  pc += off;
  /* loop back-edge (negative offset): the runaway-budget checkpoint */
  if (off < 0) {
    int _b = budget_check();
    if (_b == 1)
      RUNTIME_ERR("interrupted: time limit exceeded");
    if (_b == 2)
      RUNTIME_ERR("interrupted: memory limit exceeded");
  }
  NEXT;
}
l_br_if_false: {
  int16_t off = READ_I16;
  exp_t *a = POP();
  if (!istrue(a))
    pc += off;
  unrefexp(a);
  NEXT;
}
l_br_if_true: {
  int16_t off = READ_I16;
  exp_t *a = POP();
  if (istrue(a))
    pc += off;
  unrefexp(a);
  NEXT;
}

l_tail_self: {
  /* Self-tail: rebind inline slots from the top of the operand
     stack, keep keys as-is (same fn → same params), jump to PC 0. */
  uint8_t n = READ_U8;
  if (n != bc->nparams)
    RUNTIME_ERR_FMT("too %s arguments to %s", n < bc->nparams ? "few" : "many",
                    fn->meta ? (const char *)fn->meta : "function");
  int base = sp - n;
  int i;
  for (i = 0; i < env->n_inline; i++)
    unrefexp(env->inline_vals[i]);
  env->n_inline = n <= ENV_INLINE_SLOTS ? n : ENV_INLINE_SLOTS;
  for (i = 0; i < env->n_inline; i++)
    env->inline_vals[i] = stack[base + i];
  for (; i < n; i++)
    unrefexp(stack[base + i]);
  sp = base;
  pc = 0;
  /* self-tail loop back-edge: the runaway-budget checkpoint */
  {
    int _b = budget_check();
    if (_b == 1)
      RUNTIME_ERR("interrupted: time limit exceeded");
    if (_b == 2)
      RUNTIME_ERR("interrupted: memory limit exceeded");
  }
  NEXT;
}

l_call: {
  uint8_t n = READ_U8;
  int base = sp - n;
  exp_t *callee = stack[base - 1];
  exp_t *ret = vm_invoke_values(callee, n, &stack[base], env);
  sp = base - 1;
  unrefexp(callee);
  if (ret && iserror(ret)) {
    while (sp > 0)
      unrefexp(POP());
    if (fn_owned)
      unrefexp(fn);
    VM_RETURN(ret);
  }
  if (!ret)
    ret = NIL_EXP;
  PUSH(ret);
  NEXT;
}

l_call_global: {
  /* Fused LOAD_GLOBAL + CALL. The callee is never pushed to the
     operand stack — we resolve via the gcache directly. */
  uint8_t idx = READ_U8;
  uint8_t n = READ_U8;
  exp_t *callee;
  /* Hit-path unchanged from the original (see OP_LOAD_GLOBAL): closures
     never allocate gcache, so this never hits for them. */
  if (bc->gcache && bc->gcache[idx].gen == alcove_global_gen) {
    callee = refexp(bc->gcache[idx].val);
  } else {
    int is_global;
    callee = lookup_scoped(consts[idx], env, &is_global);
    if (!callee)
      RUNTIME_ERR_UNBOUND(consts[idx]);
    /* Only cache global resolutions (see OP_LOAD_GLOBAL): a locally-bound
       callee must not be memoized against the global generation. */
    if (!bc->no_gcache && is_global) {
      if (!bc->gcache)
        bc->gcache = calloc(bc->nconsts, sizeof(gcache_entry));
      bc->gcache[idx].val = callee;
      bc->gcache[idx].gen = alcove_global_gen;
    }
  }
  int base = sp - n;
  exp_t *ret = vm_invoke_values(callee, n, &stack[base], env);
  sp = base;
  unrefexp(callee);
  if (ret && iserror(ret)) {
    while (sp > 0)
      unrefexp(POP());
    if (fn_owned)
      unrefexp(fn);
    VM_RETURN(ret);
  }
  if (!ret)
    ret = NIL_EXP;
  PUSH(ret);
  NEXT;
}

l_tail_call: {
  /* Cross-function tail call: release the current env's bindings,
     rebind with new fn's params, and `goto tail_reentry` so the
     same vm_run invocation runs the new bytecode. O(1) C stack
     growth across tail hops.
     If the target isn't compiled, fall back to vm_invoke_values —
     we lose TCO for that hop but stay correct. */
  uint8_t n = READ_U8;
  int base = sp - n;
  exp_t *new_fn = stack[base - 1];

  /* String-as-callable and escape continuations: dispatch via
     vm_invoke_values (it has the string-index arm and the EXP_CONT escape
     arm). A continuation invoked in tail position must yield its escape
     token, not be rejected as "not a lambda". Same fallback shape as the
     !FLAG_COMPILED branch. */
  int is_ffi_callee = 0;
#ifdef ALCOVE_FFI
  is_ffi_callee = isffi(new_fn);
#endif
  (void)is_ffi_callee;
  if (!islambda(new_fn)) {
    /* ANYTHING that is not a plain lambda — a callable container (string/vector
       index, dict/set key), an escape continuation, an ffi-fn, a builtin held
       in a variable, an infix (value op value) head, or a genuinely
       non-callable value — defers to vm_invoke_values, the single authority on
       callee dispatch. It has every arm (and produces the right error for a
       truly-bad callee). Routing all non-lambda callees here is deliberate:
       it makes tail position handle exactly what non-tail OP_CALL and the AST
       evaluator handle, so a callee shape can never be accepted in one place
       and rejected as "not a lambda" in another (the regression that
       motivated this). One C-stack frame for the hop; lambdas keep full TCO
       below. */
    exp_t *ret = vm_invoke_values(new_fn, n, &stack[base], env);
    sp = base - 1;
    unrefexp(new_fn);
    while (sp > 0)
      unrefexp(POP());
    if (fn_owned)
      unrefexp(fn);
    VM_RETURN(ret);
  }

  if (!(new_fn->flags & FLAG_COMPILED) ||
      (new_fn->next && new_fn->next->meta &&
       (env_t *)new_fn->next->meta != g_global_env)) {
    /* Non-compiled target, OR a real CLOSURE that captured a NON-global env:
       the in-place TCO reuse below keeps the CURRENT env and only rebinds its
       slots, so such a closure's free variables — which live in its captured
       (local) env, not this frame's chain — would resolve wrong. Dispatch
       through vm_invoke_values instead (one C-stack frame), which parents the
       new env to the captured env. Top-level defs capture the GLOBAL env,
       which the current frame's root chain already reaches, so they keep
       full TCO; self-recursion keeps TCO via OP_TAIL_SELF. Only cross-function
       tail calls into local closures pay this one-frame hop. */
    exp_t *ret = vm_invoke_values(new_fn, n, &stack[base], env);
    sp = base - 1;
    unrefexp(new_fn);
    while (sp > 0)
      unrefexp(POP());
    if (fn_owned)
      unrefexp(fn);
    VM_RETURN(ret);
  }

  if (n != new_fn->bc->nparams)
    RUNTIME_ERR_FMT("too %s arguments to %s",
                    n < new_fn->bc->nparams ? "few" : "many",
                    new_fn->meta ? (const char *)new_fn->meta : "function");

  /* Compiled target: stash args, unwind, rebind, jump. */
  if (n > ENV_INLINE_SLOTS) {
    int i;
    for (i = 0; i < n; i++)
      unrefexp(stack[base + i]);
    sp = base - 1;
    unrefexp(new_fn);
    RUNTIME_ERR("OP_TAIL_CALL: too many args");
  }
  exp_t *args_buf[ENV_INLINE_SLOTS];
  {
    int i;
    for (i = 0; i < n; i++)
      args_buf[i] = stack[base + i];
  }
  sp = base - 1; /* drop args */
  /* stack[base-1] (new_fn slot) is above sp; we've taken ownership */

  /* Release current env's inline slots + any dict. */
  {
    int i;
    for (i = 0; i < env->n_inline; i++)
      unrefexp(env->inline_vals[i]);
  }
  env->n_inline = 0;
  if (env->d) {
    destroy_dict(env->d);
    env->d = NULL;
  }

  /* Bind new args to new_fn's params. lambda_params handles the
     content/bc union overload (compiled lambdas migrate params to
     bc->content). */
  exp_t *p = lambda_params(new_fn);
  int i = 0;
  while (p && i < n) {
    if (!is_ptr(p->content) || !issymbol(p->content)) {
      int j;
      for (j = i; j < n; j++)
        unrefexp(args_buf[j]);
      unrefexp(new_fn);
      /* Build error BEFORE potentially freeing fn — error() takes a refexp
         on the id argument, so fn must still be live when it is passed. */
      exp_t *_tc_err =
          error(ERROR_ILLEGAL_VALUE, fn, env, "OP_TAIL_CALL: bad param");
      if (fn_owned)
        unrefexp(fn);
      VM_RETURN(_tc_err);
    }
    if (env->n_inline < ENV_INLINE_SLOTS) {
      env->inline_keys[env->n_inline] = (char *)exp_text(p->content);
      env->inline_vals[env->n_inline] = args_buf[i];
      env->n_inline++;
    } else {
      if (!env->d)
        env->d = create_dict();
      set_get_keyval_dict(env->d, exp_text(p->content), args_buf[i]);
      unrefexp(args_buf[i]);
    }
    p = p->next;
    i++;
  }
  while (i < n)
    unrefexp(args_buf[i++]);

  /* Swap in new_fn and re-enter. The TCO reuses this frame, so update its
     backtrace name to the function we're tail-calling into (a tail chain shows
     the function actually running, not the one that started the chain). */
  if (fn_owned)
    unrefexp(fn);
  fn = new_fn;
  fn_owned = 1;
  if (g_calldepth >= 1 && g_calldepth <= ALC_BT_MAX)
    g_callstack[g_calldepth - 1] =
        new_fn->meta ? (const char *)new_fn->meta : "<anonymous>";
  /* cross-function tail loop back-edge: the runaway-budget checkpoint */
  {
    int _b = budget_check();
    if (_b == 1)
      RUNTIME_ERR("interrupted: time limit exceeded");
    if (_b == 2)
      RUNTIME_ERR("interrupted: memory limit exceeded");
  }
  goto tail_reentry;
}

l_cons: {
  /* (cons a b): make_node(a) takes ownership of a; b becomes ->next.
     For (cons a nil) we drop the explicit nil tail to match conscmd. */
  exp_t *b = POP(), *a = POP();
  exp_t *pair = make_node(a); /* transfers a's ref into pair->content */
  if (istrue(b))
    pair->next = b; /* transfers b's ref */
  else {
    unrefexp(b);
    pair->next = NULL;
  }
  PUSH(pair);
  NEXT;
}
l_car: {
  exp_t *p = POP();
  exp_t *v = car(p); /* borrowed (via macro guard) */
  PUSH(refexp(v));
  unrefexp(p);
  NEXT;
}
l_cdr: {
  exp_t *p = POP();
  exp_t *v = cdr(p); /* borrowed */
  PUSH(refexp(v));
  unrefexp(p);
  NEXT;
}
l_list: {
  /* (list a0 ... aN-1) → fresh list. Args own their refs; we transfer
     into the new pair chain. */
  uint8_t n = READ_U8;
  if (n == 0) {
    PUSH(NIL_EXP);
    NEXT;
  }
  int base = sp - n;
  exp_t *head = make_node(stack[base]);
  exp_t *cur = head;
  int i;
  for (i = 1; i < n; i++) {
    cur = cur->next = make_node(stack[base + i]);
  }
  sp = base;
  PUSH(head);
  NEXT;
}

/* Fused LOAD_SLOT + LOAD_FIX + op. Saves two dispatches and two
   stack round-trips per fired op — the hot arithmetic shapes on
   fib / countdown / etc. are all of this form. Fixnum slot is the
   fast path; float falls back to the same semantics as the 3-op
   sequence via COERCE_TO_DOUBLE. */
#define SLOT_FIX_NUMERIC(body_int, body_flt, opname)                           \
  do {                                                                         \
    uint8_t idx = READ_U8;                                                     \
    int16_t imm = READ_I16;                                                    \
    exp_t *a = env->inline_vals[idx];                                          \
    if (isnumber(a)) {                                                         \
      body_int;                                                                \
    } else if (isfloat(a)) {                                                   \
      double da = a->f;                                                        \
      (void)da;                                                                \
      body_flt;                                                                \
    } else                                                                     \
      RUNTIME_ERR("Illegal value in " opname);                                 \
  } while (0)

/* Overflow-checked fixnum slot±imm: error rather than wrap or implicit float.
 */
#define SLOT_FIX_CHECKED(opchar)                                               \
  do {                                                                         \
    int64_t _r = FIX_VAL(a);                                                   \
    if (fix_op_ovf((opchar), &_r, (int64_t)imm) || !FIX_FITS(_r))              \
      RUNTIME_ERR("integer overflow (no implicit float; use a float, "         \
                  "rational, or decimal)");                                    \
    PUSH(MAKE_FIX(_r));                                                        \
  } while (0)
l_slot_add_fix:
  SLOT_FIX_NUMERIC(SLOT_FIX_CHECKED('+'), PUSH(make_floatf(da + (double)imm)),
                   "+");
  NEXT;
l_slot_sub_fix:
  SLOT_FIX_NUMERIC(SLOT_FIX_CHECKED('-'), PUSH(make_floatf(da - (double)imm)),
                   "-");
  NEXT;
l_slot_lt_fix:
  SLOT_FIX_NUMERIC(PUSH(FIX_VAL(a) < imm ? TRUE_EXP : NIL_EXP),
                   PUSH(da < (double)imm ? TRUE_EXP : NIL_EXP), "<");
  NEXT;
l_slot_le_fix:
  SLOT_FIX_NUMERIC(PUSH(FIX_VAL(a) <= imm ? TRUE_EXP : NIL_EXP),
                   PUSH(da <= (double)imm ? TRUE_EXP : NIL_EXP), "<=");
  NEXT;
l_slot_gt_fix:
  SLOT_FIX_NUMERIC(PUSH(FIX_VAL(a) > imm ? TRUE_EXP : NIL_EXP),
                   PUSH(da > (double)imm ? TRUE_EXP : NIL_EXP), ">");
  NEXT;
l_slot_ge_fix:
  SLOT_FIX_NUMERIC(PUSH(FIX_VAL(a) >= imm ? TRUE_EXP : NIL_EXP),
                   PUSH(da >= (double)imm ? TRUE_EXP : NIL_EXP), ">=");
  NEXT;
l_slot_is_fix: {
  /* (is slot K) with a fixnum immediate is exactly a tagged-pointer bit
     compare — no numeric coercion, no error path: a non-fixnum slot value
     (float / char / heap ptr) simply isn't bit-equal to MAKE_FIX(imm) and
     yields nil, matching isequal's fixnum-vs-other behavior. */
  uint8_t idx = READ_U8;
  int16_t imm = READ_I16;
  exp_t *a = env->inline_vals[idx];
  PUSH(a == MAKE_FIX((int64_t)imm) ? TRUE_EXP : NIL_EXP);
  NEXT;
}

l_slot_le_slot: {
  /* Hot-path superinst for `for`: reads two slots, pushes t/nil for
     (slot_a <= slot_b). Fuses LOAD_SLOT+LOAD_SLOT+LE into one dispatch. */
  uint8_t idx_a = READ_U8;
  uint8_t idx_b = READ_U8;
  exp_t *a = env->inline_vals[idx_a];
  exp_t *b = env->inline_vals[idx_b];
  if (isnumber(a) && isnumber(b)) {
    PUSH(FIX_VAL(a) <= FIX_VAL(b) ? TRUE_EXP : NIL_EXP);
  } else if ((isnumber(a) || isfloat(a)) && (isnumber(b) || isfloat(b))) {
    double da = TO_DOUBLE(a);
    double db = TO_DOUBLE(b);
    PUSH(da <= db ? TRUE_EXP : NIL_EXP);
  } else {
    RUNTIME_ERR("Illegal value in <=");
  }
  NEXT;
}

#undef SLOT_FIX_NUMERIC

l_vec_ref: {
  exp_t *iexp = POP(), *vexp = POP();
  if (!isvector(vexp) || !(isnumber(iexp) || isfloat(iexp))) {
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-ref: bad args");
  }
  /* A float index truncates to its integer part (matches the AST vec-ref
     and ordinary integer division semantics). This also absorbs a value
     that arrived as a float instead of a fixnum — e.g. (/ a b) taking the
     float path on a 32-bit target — where the truncated result is still
     the correct index. */
  int64_t i = isnumber(iexp) ? FIX_VAL(iexp) : (int64_t)iexp->f;
  if (i < 0 || i >= vec_len(vexp)) {
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR_C(ERROR_INDEX_OUT_OF_RANGE, "vec-ref: index out of range");
  }
  exp_t *r = vec_get_boxed(vexp, i);
  unrefexp(iexp);
  unrefexp(vexp);
  PUSH(r);
  NEXT;
}
l_vec_set: {
  exp_t *valexp = POP(), *iexp = POP(), *vexp = POP();
  if (!isvector(vexp) || !(isnumber(iexp) || isfloat(iexp))) {
    unrefexp(valexp);
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-set!: bad args");
  }
  int64_t i = isnumber(iexp) ? FIX_VAL(iexp)
                             : (int64_t)iexp->f; /* float idx truncates */
  if (i < 0 || i >= vec_len(vexp)) {
    unrefexp(valexp);
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR_C(ERROR_INDEX_OUT_OF_RANGE, "vec-set!: index out of range");
  }
  /* Push the returned value before vec_set_boxed consumes valexp. */
  exp_t *r = refexp(valexp);
  if (!vec_set_boxed(vexp, i, valexp)) {
    unrefexp(r);
    unrefexp(iexp);
    unrefexp(vexp);
    RUNTIME_ERR("vec-set!: alloc failure or shared vec promote");
  }
  PUSH(r);
  unrefexp(iexp);
  unrefexp(vexp);
  NEXT;
}
l_vec_len: {
  exp_t *vexp = POP();
  if (!isvector(vexp)) {
    unrefexp(vexp);
    RUNTIME_ERR("vec-len: not a vector");
  }
  int64_t n = vec_len(vexp);
  unrefexp(vexp);
  PUSH(MAKE_FIX(n));
  NEXT;
}
l_vec_new: {
  extern exp_t *make_vector(int64_t n, exp_t *fill);
  exp_t *initexp = POP(), *nexp = POP();
  if (!isnumber(nexp)) {
    unrefexp(initexp);
    unrefexp(nexp);
    RUNTIME_ERR("vec: n must be a number");
  }
  int64_t n = FIX_VAL(nexp);
  if (n < 0)
    n = 0;
  exp_t *vec = make_vector(n, initexp);
  unrefexp(initexp);
  unrefexp(nexp);
  if (!vec)
    RUNTIME_ERR("(vec n ...): n is too large or alloc failed");
  PUSH(vec);
  NEXT;
}
l_sqrt_int: {
  exp_t *nexp = POP();
  if (!isnumber(nexp)) {
    unrefexp(nexp);
    RUNTIME_ERR("sqrt-int: not a number");
  }
  int64_t n = FIX_VAL(nexp);
  int64_t r = (n < 0) ? 0 : (int64_t)sqrt((double)n);
  /* Cast to uint64_t before multiplying to avoid signed overflow UB when
     r is near INT64_MAX (same guard used in sqrtintcmd tree-walker path). */
  while ((uint64_t)(r + 1) * (uint64_t)(r + 1) <= (uint64_t)n)
    r++;
  while ((uint64_t)r * (uint64_t)r > (uint64_t)n)
    r--;
  unrefexp(nexp);
  PUSH(MAKE_FIX(r));
  NEXT;
}
l_abs: {
  /* (abs a) — mirrors abscmd. Fixnum: |v|, erroring on FIXMIN overflow
     (explicit over implicit — no float promotion). Float: fabs. A fixnum
     operand is an immediate (no ref); a float is heap (drop ref after read). */
  exp_t *a = POP();
  if (isnumber(a)) {
    int64_t v = FIX_VAL(a);
    int64_t av = v < 0 ? -v : v;
    if (v < 0 && !FIX_FITS(av))
      RUNTIME_ERR("integer overflow (no implicit float; use a float, "
                  "rational, or decimal)");
    PUSH(MAKE_FIX(av));
  } else if (isfloat(a)) {
    expfloat f = a->f;
    unrefexp(a);
    PUSH(make_floatf(f < 0 ? -f : f));
  } else {
    unrefexp(a);
    RUNTIME_ERR("abs: not a number");
  }
  NEXT;
}
l_max: {
  /* (max a b) — value-preserving (returns the actual larger operand, keeping
     its int/float type), via alc_numlt (= maxcmd's comparator). Keep the
     winner's ref, drop the loser's. Equal → keep a (matches maxcmd's fold). */
  exp_t *b = POP(), *a = POP();
  int err;
  int a_lt_b = alc_numlt(a, b, &err);
  if (err) {
    unrefexp(a);
    unrefexp(b);
    RUNTIME_ERR("max: non-numeric");
  }
  if (a_lt_b) {
    unrefexp(a);
    PUSH(b);
  } else {
    unrefexp(b);
    PUSH(a);
  }
  NEXT;
}
l_min: {
  /* (min a b) — symmetric to OP_MAX; keep the smaller. Equal → keep a. */
  exp_t *b = POP(), *a = POP();
  int err;
  int b_lt_a = alc_numlt(b, a, &err);
  if (err) {
    unrefexp(a);
    unrefexp(b);
    RUNTIME_ERR("min: non-numeric");
  }
  if (b_lt_a) {
    unrefexp(a);
    PUSH(b);
  } else {
    unrefexp(b);
    PUSH(a);
  }
  NEXT;
}
l_length: {
  /* (length x) — must mirror lengthcmd's semantics or compiled bodies
     silently miscompile string/vector/blob/list args to 0. NULL and
     nil_singleton are length 0 (empty list). */
  exp_t *xs = POP();
  int64_t n = 0;
  if (xs == NULL || xs == nil_singleton) {
    n = 0;
  } else if (isstring(xs)) {
    {
      const char *_t = exp_text(xs);
      n = _t ? utf8_strlen(_t) : 0;
    }
  } else if (ispair(xs)) {
    exp_t *cur = xs;
    while (is_ptr(cur) && cur->type == EXP_PAIR) {
      n++;
      cur = cur->next;
    }
  } else if (is_ptr(xs) && xs->type == EXP_VECTOR && xs->ptr) {
    n = vec_len(xs);
  } else if (isblob(xs)) {
    n = xs->ptr ? (int64_t)((alc_blob_t *)xs->ptr)->len : 0;
  } else if (islist(xs)) {
    n = xs->ptr ? (int64_t)((alc_list_t *)xs->ptr)->len : 0;
  } else {
    unrefexp(xs);
    RUNTIME_ERR("length: not a list/string/vector/blob");
  }
  unrefexp(xs);
  PUSH(MAKE_FIX(n));
  NEXT;
}

#undef BIN_ARITH
#undef CMP_OP
#undef COERCE_TO_DOUBLE
#undef NEXT
#undef PUSH
#undef POP
#undef READ_U8
#undef READ_I16
#undef RUNTIME_ERR
}

/* Invoke a callee with already-evaluated args. Takes ownership of
   argv[i] values. Used by OP_CALL. No refexp on fn: the caller's
   operand stack holds fn for the duration of this call, so its lifetime
   is already guaranteed — skipping the atomic pair is measurable on
   call-heavy benchmarks. */
static exp_t *multi_pick(exp_t *clauses,
                         int n); /* defined below, before invoke */

static exp_t *vm_invoke_values(exp_t *fn, int nargs, exp_t **argv, env_t *env) {
  /* Multi-arity (defn) reached from compiled code: dispatch on arg count to
     the matching clause lambda, then run it normally. Before the param-list
     reads below. */
  /* Hot path first: a plain compiled lambda is by far the common callee, so
     test islambda once up front and jump straight to the bind+run code,
     skipping the rare-callee probes (container / continuation / ffi) below.
     Those probes are only worth running when fn is NOT a lambda. */
  if (islambda(fn)) {
    if (fn->flags & FLAG_MULTI) {
      exp_t *chosen = multi_pick(fn->content, nargs);
      if (!chosen) {
        for (int i = 0; i < nargs; i++)
          unrefexp(argv[i]);
        return error(ERROR_MISSING_PARAMETER, fn, env,
                     "no matching clause for %d argument(s)", nargs);
      }
      return vm_invoke_values(chosen, nargs, argv, env);
    }
    goto bind_lambda;
  }
  /* fn is not a lambda — the rare callee shapes, each of which returns. */
  /* String-as-callable: (s i) returns the indexed char. The AST
     evaluator handles this in two places (literal string head and the
     symbol-lookup path added in ticket 5). The bytecode VM compiles
     (sym args...) as OP_CALL_GLOBAL → vm_invoke_values, so we need the
     same arm here or compiled bodies miscompile string-index reads. */
  if (iscallable_container(fn)) {
    /* (container arg) read — indexable: element by int index; keyed
       (dict/hamt/set): value/member by key. The AST evaluator has the same
       arm on its two head paths; this keeps compiled bodies in sync. */
    int i;
    /* (s op rhs) infix on a container LHS -> (op s rhs): a 2-arg call whose
       first arg is an operator/function is infix, not indexing. Matches the AST
       container arms (ast_container_infix). argv[0]=op, argv[1]=rhs. */
    if (nargs == 2) {
      int oi = infix_op_index(argv[0]);
      if (oi >= 0 || infix_is_fn(argv[0])) {
        exp_t *r = oi >= 0 ? infix_apply(oi, fn, argv[1], env)
                           : infix_apply_fn(argv[0], fn, argv[1], env);
        unrefexp(argv[0]);
        unrefexp(argv[1]);
        return r;
      }
    }
    if (nargs != 1) {
      for (i = 0; i < nargs; i++)
        unrefexp(argv[i]);
      return error(ERROR_MISSING_PARAMETER, fn, env,
                   "index: expected exactly 1 arg, got %d", nargs);
    }
    return container_apply(fn, argv[0],
                           env); /* consumes argv[0]; caller unrefs fn */
  }
  if (iscont(fn)) {
    /* (k v) reached from compiled code: produce the escape token. */
    exp_t *payload = nargs > 0 ? refexp(argv[0]) : refexp(NIL_EXP);
    int i;
    for (i = 0; i < nargs; i++)
      unrefexp(argv[i]);
    return make_cont_escape((int64_t)(intptr_t)fn->meta, payload, env);
  }
#ifdef ALCOVE_FFI
  if (isffi(fn)) {
    /* An ffi-fn value called from compiled bytecode. The AST evaluator
       dispatches FFI in evaluate(); the VM funnels (sym args...) through
       OP_CALL_GLOBAL → here, so without this arm an FFI call inside any
       compiled function body errors ("not a lambda") instead of running.
       Args are already evaluated; alc_ffi_call consumes their refs. */
    return alc_ffi_call((alc_ffi_t *)fn->ptr, nargs, argv);
  }
#endif
  if (isinternal(fn)) {
    /* An applicative builtin called from compiled bytecode (FLAG_APPLICATIVE —
       see compile_expr's fast path). If it registered a values fast-path
       (lispCmdV in ->meta), call it directly with the evaluated argv — no call
       form synthesized, so nothing to leak. Otherwise alc_apply_n wraps the
       args in the canonical (fn args...) form and calls fn->fnc (which must
       consume that form). Both consume the argv refs. */
    /* Sandbox gate, hoisted above both arms: alc_apply_n routes through
       invoke_internal (gated), but the values fast-path calls fv() directly, so
       refuse a FLAG_UNSAFE builtin here to keep the "single gate" invariant on
       every compiled path. Consume the owned argv refs first. */
    if ((g_safe_mode || g_in_client_cmd) && (fn->flags & FLAG_UNSAFE)) {
      int i;
      for (i = 0; i < nargs; i++)
        unrefexp(argv[i]);
      return error(ERROR_ILLEGAL_VALUE, fn, env,
                   "operation not permitted in this context "
                   "(sandboxed: OS / filesystem / FFI / code-loading)");
    }
    if (fn->meta) {
      lispCmdV *fv = (lispCmdV *)(void *)fn->meta;
      int was_tail = in_tail_position;
      in_tail_position = 0;
      exp_t *ret = fv(nargs, argv, env);
      in_tail_position = was_tail;
      return ret;
    }
    return alc_apply_n(fn, nargs, argv, env);
  }
  if (!islambda(fn)) {
    /* Infix: a non-callable head with exactly 2 args whose operator value is a
       binary builtin -> (op head rhs). e.g. (1 + 2), or (a + b) where a is a
       number. argv[0] is the evaluated operator, argv[1] the rhs. */
    int idx;
    if (nargs == 2 && (idx = infix_op_index(argv[0])) >= 0) {
      exp_t *r = infix_apply(idx, fn, argv[1], env);
      unrefexp(argv[0]);
      unrefexp(argv[1]);
      return r;
    }
    /* General infix: a non-callable head with a FUNCTION in the operator slot
       -> (op head rhs). e.g. (s starts-with? "t") -> (starts-with? s "t"). */
    if (nargs == 2 && infix_is_fn(argv[0])) {
      exp_t *r = infix_apply_fn(argv[0], fn, argv[1], env);
      unrefexp(argv[0]);
      unrefexp(argv[1]);
      return r;
    }
    int i;
    for (i = 0; i < nargs; i++)
      unrefexp(argv[i]);
    return error(ERROR_ILLEGAL_VALUE, fn, env, "call: head is not a function");
  }
bind_lambda:; /* a plain (non-MULTI) lambda jumps straight here */
  /* Honor closure capture (see invoke()). For top-level fns this is
     just global so behavior is unchanged. */
  env_t *captured = (env_t *)fn->next->meta;
  env_t *newenv = make_env(captured ? captured : env);
  /* callingfnc stays NULL — OP_CALL is always non-tail from our side. */

  /* Fast path: compiled lambdas have their param keys cached in
     bc->param_keys, so we skip the per-call walk over fn->content
     (a cons-list traversal) and the per-param type check. ~30 cycles
     saved per call on the typical 2-3 arg case — material on call-
     heavy benchmarks like nqueens (~1M calls). */
  if ((fn->flags & FLAG_COMPILED) && fn->bc && fn->bc->nparams == nargs &&
      nargs <= ENV_INLINE_SLOTS) {
    int i;
    for (i = 0; i < nargs; i++) {
      newenv->inline_keys[i] = fn->bc->param_keys[i];
      newenv->inline_vals[i] = argv[i];
    }
    newenv->n_inline = nargs;
  } else {
    /* Slow-path bind. Verify expected param count up-front for compiled
       fns: silently running the body with too few args used to fail
       later as a misleading "unbound variable". */
    if ((fn->flags & FLAG_COMPILED) && fn->bc && fn->bc->nparams != nargs) {
      int i;
      for (i = 0; i < nargs; i++)
        unrefexp(argv[i]);
      destroy_env(newenv);
      /* Same wording as the AST path (var2env) so arity errors read identically
         whether the callee was reached interpreted or compiled. */
      return error(ERROR_ILLEGAL_VALUE, fn, env, "too %s arguments to %s",
                   nargs < fn->bc->nparams ? "few" : "many",
                   fn->meta ? (const char *)fn->meta : "function");
    }
    exp_t *p = lambda_params(fn);
    int i = 0;
    while (p && p->content) {
      if (!is_ptr(p->content) || !issymbol(p->content)) {
        int j;
        for (j = i; j < nargs; j++)
          unrefexp(argv[j]);
        destroy_env(newenv);
        return error(ERROR_ILLEGAL_VALUE, fn, env, "Bytecode call: bad param");
      }
      /* Rest param — collect remaining argv into a list and bind. */
      if (strcmp((char *)exp_text(p->content), ".") == 0) {
        if (!p->next || !p->next->content || !issymbol(p->next->content)) {
          int j;
          for (j = i; j < nargs; j++)
            unrefexp(argv[j]);
          destroy_env(newenv);
          return error(ERROR_ILLEGAL_VALUE, fn, env,
                       "rest param: symbol expected after '.'");
        }
        exp_t *rest_head = NIL_EXP, *rest_tail = NULL;
        for (; i < nargs; i++)
          list_append_owned(&rest_head, &rest_tail, argv[i]);
        var2env_bind((char *)exp_text(p->next->content), rest_head, newenv);
        p = NULL; /* done */
        break;
      }
      if (i >= nargs) {
        int j;
        for (j = i; j < nargs; j++)
          unrefexp(argv[j]);
        destroy_env(newenv);
        return error(ERROR_MISSING_PARAMETER, fn, env,
                     "too few arguments to %s",
                     fn->meta ? (const char *)fn->meta : "function");
      }
      var2env_bind((char *)exp_text(p->content), argv[i], newenv);
      p = p->next;
      i++;
    }
    while (i < nargs)
      unrefexp(argv[i++]);
  }

  exp_t *ret;
  bt_push(fn->meta ? (const char *)fn->meta : NULL); /* backtrace frame */
  if (fn->flags & FLAG_COMPILED) {
#ifdef ALCOVE_JIT
    if (fn->bc->jit) {
      ret = fn->bc->jit(newenv);
      if (!ret)
        ret = vm_run(fn, newenv); /* JIT deopt → bytecode */
    } else
#endif
      ret = vm_run(fn, newenv);
  } else {
    exp_t *body = fn->next->content;
    ret = NULL;
    while (body) {
      if (ret)
        unrefexp(ret);
      ret = evaluate(refexp(body->content), newenv);
      if (ret && iserror(ret))
        break;
      body = body->next;
    }
  }
  bt_pop();
  destroy_env(newenv);
  return ret;
}

static exp_t *multi_pick(exp_t *clauses, int n) {
  for (exp_t *c = clauses; c && c->content; c = c->next) {
    exp_t *L = c->content;
    int fixed = 0, variadic = 0;
    for (exp_t *p = lambda_params(L); p && p->content; p = p->next) {
      if (issymbol(p->content) &&
          strcmp((char *)exp_text(p->content), ".") == 0) {
        variadic = 1;
        break;
      }
      fixed++;
    }
    if (variadic ? (n >= fixed) : (n == fixed))
      return L;
  }
  return NULL;
}
