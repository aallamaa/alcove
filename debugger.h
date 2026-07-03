/* debugger.h — debugger implementation. FRAGMENT
 * #included into alcove.c (single TU). NOT standalone, NOT separately compiled.
 * `make tidy` lints it in context via alcove.c.
 */

/* ---- Debugger implementation (see the globals block near bt_clear) --------
   Everything here is reached only when g_debug is set. The debug REPL reads
   commands from stdin and writes to stderr (so it never tangles with the
   program's own stdout); --debug runs a FILE, leaving stdin free for commands.
 */
/* A form's line for DISPLAY/breakpoints: form_line() routed through the Adder
   source map (display_line is identity for Alcove, generated→.adr for Adder),
   so `bt`/`break <line>` use the line the user actually wrote in both dialects.
 */
static int dbg_disp_line(exp_t *e) {
  int ln = form_line(e);
  return ln ? display_line(ln) : 0;
}
static void dbg_push(const char *name, exp_t *form) {
  if (g_dbg_depth >= 0 && g_dbg_depth < ALC_BT_MAX) {
    g_dbg_frames[g_dbg_depth].name = name ? name : "<anonymous>";
    g_dbg_frames[g_dbg_depth].env = NULL;
    g_dbg_frames[g_dbg_depth].form = form;
    g_dbg_frames[g_dbg_depth].line = dbg_disp_line(form);
  }
  g_dbg_depth++;
}
static void dbg_pop(void) {
  if (g_dbg_depth > 0)
    g_dbg_depth--;
}
/* Index of the current (innermost) live frame in g_dbg_frames, clamped to the
   array bound. g_dbg_depth may exceed ALC_BT_MAX (dbg_push stores only the
   first ALC_BT_MAX frames but still counts depth), so deriving an index
   straight from g_dbg_depth-1 reads/writes out of bounds past 128 frames —
   reachable by ordinary deep recursion under --debug. Returns -1 when there is
   no frame. */
static inline int dbg_cur(void) {
  int d = g_dbg_depth < ALC_BT_MAX ? g_dbg_depth : ALC_BT_MAX;
  return d > 0 ? d - 1 : -1;
}
/* True if NAME has a break-on-function set. Checked by invoke_body AFTER args
   are bound, so the stop lands on the first BODY form (with the callee's params
   in scope), not on an argument expression evaluated in the caller. */
static int dbg_fn_breakpointed(const char *name) {
  if (!name)
    return 0;
  for (int i = 0; i < g_dbg_nbp_fn; i++)
    if (strcmp(g_dbg_bp_fn[i], name) == 0)
      return 1;
  return 0;
}
/* Render one value to a freshly-malloc'd C string (caller frees). */
static char *dbg_value_str(exp_t *v) {
  /* cap must start non-zero: str_buf_put grows by doubling (0*2 == 0 loops). */
  size_t len = 0, cap = 64;
  char *buf = malloc(cap);
  if (!buf)
    return strdup("");
  buf[0] = 0;
  exp_to_string_buf(v, &buf, &len, &cap);
  return buf;
}
/* Read+evaluate one expression string in `env` (the `p` command). */
static exp_t *dbg_eval_in_env(const char *src, env_t *env) {
  FILE *s = fmemopen((void *)src, strlen(src), "r");
  if (!s)
    return NIL_EXP;
  exp_t *form = reader(s, 0, 0);
  exp_t *r;
  if (!form || iserror(form))
    r = form ? form : refexp(NIL_EXP);
  else
    r = evaluate(form,
                 env); /* takes ownership of form (reader gave an owned ref) */
  fclose(s);
  return r ? r : refexp(NIL_EXP); /* always an owned ref — caller unrefs */
}
static void dbg_show_backtrace(FILE *out) {
  int top = g_dbg_depth < ALC_BT_MAX ? g_dbg_depth : ALC_BT_MAX;
  for (int i = top - 1; i >= 0; i--) {
    char *fs =
        g_dbg_frames[i].form ? dbg_value_str(g_dbg_frames[i].form) : NULL;
    int sel = (i == g_dbg_sel);
    fprintf(out, "  %s%c#%d%s %s%-16s%s line %s%d%s   %s%s%s\n",
            dc(sel ? DBGC_SEL : ""), sel ? '*' : ' ', top - 1 - i, dc(DBGC_RST),
            dc(DBGC_FN), g_dbg_frames[i].name ? g_dbg_frames[i].name : "?",
            dc(DBGC_RST), dc(DBGC_NUM), g_dbg_frames[i].line, dc(DBGC_RST),
            dc(DBGC_FORM), fs ? fs : "", dc(DBGC_RST));
    free(fs);
  }
  if (g_dbg_depth > ALC_BT_MAX)
    fprintf(out, "   … (%d deeper frames)\n", g_dbg_depth - ALC_BT_MAX);
}
static void dbg_show_locals(FILE *out, env_t *env) {
  if (!env) {
    fprintf(out, "  (no frame env)\n");
    return;
  }
  int n = 0;
  for (int i = 0; i < env->n_inline; i++) {
    if (!env->inline_keys[i])
      continue;
    char *vs = dbg_value_str(env->inline_vals[i]);
    fprintf(out, "  %s%s%s = %s\n", dc(DBGC_VAR), env->inline_keys[i],
            dc(DBGC_RST), vs);
    free(vs);
    n++;
  }
  if (env->d)
    fprintf(out, "  (+ more bindings in this frame's overflow dict)\n");
  if (n == 0 && !env->d)
    fprintf(out, "  (no locals)\n");
}
/* The interactive debug prompt. Returns when a resume command is given; sets
   g_dbg_mode for the kind of resume (run / step-into / next). */
static void debug_repl(exp_t *form, env_t *env) {
  g_dbg_active = 1; /* suppress the per-form hook while we run commands */
  g_dbg_color = isatty(fileno(stderr));
  int have_form = form && form != NIL_EXP;
  int cur = dbg_cur(); /* clamped innermost-frame index (-1 if none) */
  if (have_form && cur >= 0) {
    g_dbg_frames[cur].env = env;
    g_dbg_frames[cur].form = form;
    int ln = dbg_disp_line(form);
    if (ln)
      g_dbg_frames[cur].line = ln;
  }
  g_dbg_sel = cur >= 0 ? cur : 0;
  const char *fn = cur >= 0 ? g_dbg_frames[cur].name : "(top)";
  if (!have_form) {
    fprintf(
        stderr,
        "\n%s-- debugger ready.%s set breakpoints (%sbreak <fn|line>%s), then "
        "'%sc%s' to run; '%shelp%s' for commands.\n",
        dc(DBGC_HDR), dc(DBGC_RST), dc(DBGC_FN), dc(DBGC_RST), dc(DBGC_FN),
        dc(DBGC_RST), dc(DBGC_FN), dc(DBGC_RST));
  } else {
    char *fs = dbg_value_str(form);
    fprintf(stderr, "\n%s-- break in %s%s%s, line %s%d%s:\n   %s%s%s\n",
            dc(DBGC_HDR), dc(DBGC_FN), fn ? fn : "?", dc(DBGC_RST),
            dc(DBGC_NUM), dbg_disp_line(form), dc(DBGC_RST), dc(DBGC_FORM), fs,
            dc(DBGC_RST));
    free(fs);
  }
  for (;;) {
    env_t *fe = (g_dbg_depth > 0 && g_dbg_frames[g_dbg_sel].env)
                    ? g_dbg_frames[g_dbg_sel].env
                    : env;
    char *line = dbg_read_command(fe);
    if (!line) { /* EOF on the command stream → detach and continue */
      g_debug = 0;
      g_dbg_mode = 0;
      break;
    }
    char *c = line;
    while (*c == ' ' || *c == '\t')
      c++;
    if (!*c) {
      free(line);
      continue;
    }
    /* command word + rest */
    char *rest = c;
    while (*rest && *rest != ' ' && *rest != '\t')
      rest++;
    char *arg = rest;
    while (*arg == ' ' || *arg == '\t')
      arg++;
    size_t wlen = (size_t)(rest - c);
    int resume = 0; /* a step/next/continue/quit command ends the REPL */
#define DBG_IS(w) (wlen == strlen(w) && strncmp(c, w, wlen) == 0)
    if (DBG_IS("c") || DBG_IS("continue")) {
      g_dbg_mode = 0;
      resume = 1;
    } else if (DBG_IS("s") || DBG_IS("step")) {
      g_dbg_mode = 1;
      resume = 1;
    } else if (DBG_IS("n") || DBG_IS("next")) {
      g_dbg_mode = 2;
      g_dbg_next_depth = g_dbg_depth;
      resume = 1;
    } else if (DBG_IS("q") || DBG_IS("quit")) {
      g_debug = 0; /* detach: run to completion without stopping */
      g_dbg_mode = 0;
      resume = 1;
    } else if (DBG_IS("bt") || DBG_IS("backtrace") || DBG_IS("where")) {
      dbg_show_backtrace(stderr);
    } else if (DBG_IS("frame") || DBG_IS("f")) {
      int idx = atoi(arg); /* 0 = innermost, as printed by bt */
      int top = g_dbg_depth < ALC_BT_MAX ? g_dbg_depth : ALC_BT_MAX;
      if (idx >= 0 && idx < top)
        g_dbg_sel = top - 1 - idx;
      fprintf(stderr, "  frame #%d: %s%s%s (line %s%d%s)\n",
              top - 1 - g_dbg_sel, dc(DBGC_FN), g_dbg_frames[g_dbg_sel].name,
              dc(DBGC_RST), dc(DBGC_NUM), g_dbg_frames[g_dbg_sel].line,
              dc(DBGC_RST));
    } else if (DBG_IS("up")) {
      if (g_dbg_sel > 0)
        g_dbg_sel--;
      fprintf(stderr, "  frame: %s%s%s\n", dc(DBGC_FN),
              g_dbg_frames[g_dbg_sel].name, dc(DBGC_RST));
    } else if (DBG_IS("down")) {
      int top = g_dbg_depth < ALC_BT_MAX ? g_dbg_depth : ALC_BT_MAX;
      if (g_dbg_sel < top - 1)
        g_dbg_sel++;
      fprintf(stderr, "  frame: %s%s%s\n", dc(DBGC_FN),
              g_dbg_frames[g_dbg_sel].name, dc(DBGC_RST));
    } else if (DBG_IS("locals")) {
      env_t *fe = g_dbg_frames[g_dbg_sel].env;
      dbg_show_locals(stderr, fe ? fe : env);
    } else if (DBG_IS("p") || DBG_IS("print")) {
      if (!*arg) {
        fprintf(stderr, "  usage: p <expr>\n");
      } else {
        env_t *fe = g_dbg_frames[g_dbg_sel].env;
        exp_t *r = dbg_eval_in_env(arg, fe ? fe : env);
        char *vs = dbg_value_str(r);
        fprintf(stderr, "  %s%s%s\n", dc(DBGC_FORM), vs, dc(DBGC_RST));
        free(vs);
        unrefexp(r);
      }
    } else if (DBG_IS("break") || DBG_IS("b")) {
      if (!*arg) {
        fprintf(stderr, "  usage: break <function|line>\n");
      } else {
        int isnum = 1;
        for (char *p = arg; *p; p++)
          if (*p < '0' || *p > '9') {
            isnum = 0;
            break;
          }
        if (isnum && g_dbg_nbp_line < DBG_BP_MAX) {
          g_dbg_bp_line[g_dbg_nbp_line++] = atoi(arg);
          fprintf(stderr, "  breakpoint at line %s%s%s\n", dc(DBGC_NUM), arg,
                  dc(DBGC_RST));
        } else if (!isnum && g_dbg_nbp_fn < DBG_BP_MAX) {
          g_dbg_bp_fn[g_dbg_nbp_fn++] = strdup(arg);
          fprintf(stderr, "  breakpoint at function %s%s%s\n", dc(DBGC_FN), arg,
                  dc(DBGC_RST));
        }
      }
    } else if (DBG_IS("return")) {
      if (!g_dbg_in_error_break) {
        fprintf(stderr, "  return works only at a break-on-error (recovers the "
                        "failing expression with a value)\n");
      } else {
        env_t *fe = g_dbg_frames[g_dbg_sel].env;
        g_dbg_replace = dbg_eval_in_env(*arg ? arg : "nil", fe ? fe : env);
        g_dbg_did_replace = 1;
        resume = 1; /* error() returns this value in place of the error */
      }
    } else if (DBG_IS("h") || DBG_IS("help") || DBG_IS("?")) {
      fprintf(stderr,
              "  commands: bt | frame N | up | down | locals | p <expr>\n"
              "            step(s) | next(n) | continue(c) | break <fn|line>\n"
              "            return <expr> (recover at a break-on-error) | "
              "quit(q)\n");
    } else {
      fprintf(stderr, "  unknown command '%s' (try 'help')\n", c);
    }
#undef DBG_IS
    free(line);
    if (resume)
      break;
  }
  g_dbg_active = 0;
}
/* Per-form hook installed in evaluate(); decides whether to stop. */
static void dbg_hook(exp_t *e, env_t *env) {
  if (g_dbg_active)
    return; /* re-entrant call from a `p`/locals evaluation — don't stop */
  int ln =
      dbg_disp_line(e); /* Adder-mapped, so `break <line>` matches source */
  int cur = dbg_cur();
  if (cur >= 0) {
    g_dbg_frames[cur].env = env;
    g_dbg_frames[cur].form = e;
    if (ln)
      g_dbg_frames[cur].line = ln;
  }
  int stop = 0;
  if (g_dbg_mode == 1)
    stop = 1; /* step into */
  else if (g_dbg_mode == 2 && g_dbg_depth <= g_dbg_next_depth)
    stop = 1; /* next: back at or above the launching depth */
  else if (ln)
    for (int i = 0; i < g_dbg_nbp_line; i++)
      if (g_dbg_bp_line[i] == ln) {
        stop = 1;
        break;
      }
  if (stop)
    debug_repl(e, env);
}
/* Break-on-raise: an uncaught error just dropped us here at the raise site
   (frames live). Show it, then open the debugger at the failing frame. */
static exp_t *dbg_error_break(exp_t *err, env_t *env) {
  int cur = dbg_cur();
  exp_t *eform = (err->next && is_ptr(err->next) && ispair(err->next))
                     ? err->next
                     : (cur >= 0 ? g_dbg_frames[cur].form : NIL_EXP);
  env_t *fe = (cur >= 0 && g_dbg_frames[cur].env) ? g_dbg_frames[cur].env : env;
  g_dbg_color = isatty(fileno(stderr));
  fprintf(stderr,
          "\n%s** error raised: %s%s\n   (debugger — bt / locals / p <expr>; "
          "'c' propagates, '%sreturn <expr>%s' recovers with a value, 'q' "
          "detaches)\n",
          dc(DBGC_ERR), (const char *)err->ptr, dc(DBGC_RST), dc(DBGC_FN),
          dc(DBGC_ERR));
  fprintf(stderr, "%s", dc(DBGC_RST));
  g_dbg_did_replace = 0;
  g_dbg_replace = NULL;
  g_dbg_in_error_break = 1; /* enable the `return` recovery command */
  debug_repl(eform, fe);
  g_dbg_in_error_break = 0;
  if (g_dbg_did_replace)
    return g_dbg_replace ? g_dbg_replace
                         : refexp(NIL_EXP); /* owned replacement */
  return err;
}

const char doc_break[] = "(break) — drop into the interactive debugger here "
                         "(gdb-style: bt, frame N, "
                         "locals, p <expr>, step/next/continue, break "
                         "<fn|line>). Full backtrace and "
                         "source lines require running under `alcove --debug`.";
exp_t *breakcmd(exp_t *e, env_t *env) {
  g_track_lines = 1;
  g_debug = 1;
  debug_repl(e,
             env); /* e == the (break) call form; alive until we unref below */
  unrefexp(e);
  return NIL_EXP;
}

const char doc_allow_unsafe[] =
    "(allow-unsafe \"name\") — permanently clear the sandbox restriction on "
    "the "
    "named builtin (shell, file ops, ffi-*, load, ...) so it may run from RESP "
    "client callbacks and under --safe. For trusted init/setup code; "
    "allow-unsafe "
    "is itself sandboxed, so client code can't grant itself access. Returns t, "
    "or "
    "nil if there is no such builtin.";
exp_t *allowunsafecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(name);
  if (!isstring(name) && !issymbol(name))
    CLEAN_RETURN_1(name,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "allow-unsafe: name must be a string or symbol"));
  keyval_t *kv =
      set_get_keyval_dict(reserved_symbol, (char *)exp_text(name), NULL);
  exp_t *ret = NIL_EXP;
  if (kv && kv->val && isinternal((exp_t *)kv->val)) {
    ((exp_t *)kv->val)->flags &=
        ~FLAG_UNSAFE; /* grant: no longer host-escape-gated */
    ret = TRUE_EXP;
  }
  CLEAN_RETURN_1(name, refexp(ret));
}

/* Thin wrapper: push one backtrace frame around the whole AST invocation, so
   the many internal return paths need no per-exit bookkeeping. (Cross-function
   tail calls inside invoke_body reuse the frame and keep this name — a TCO
   collapse.) */
exp_t *invoke(exp_t *e, exp_t *fn, env_t *env) {
  bt_push(fn->meta ? (const char *)fn->meta : NULL);
  if (g_debug)
    dbg_push(fn->meta ? (const char *)fn->meta : NULL, e);
  exp_t *r = invoke_body(e, fn, env);
  if (g_debug)
    dbg_pop();
  bt_pop();
  return r;
}

static exp_t *invoke_body(exp_t *e, exp_t *fn, env_t *env) {
  /* e->content = fn name, e->next = args list,
     fn->content = params list, fn->next->content = body list.

     We hold a ref on `fn` across the invocation so that its header
     symbols (whose ->ptr we borrow into env->inline_keys) can never be
     freed while the env is live. Tail calls reuse the frame via a
     trampoline loop — O(1) C stack for tail recursion. */

  /* Stack-overflow guard: bail with a catchable error before deep non-tail
     recursion blows the C stack. Cleanup mirrors the multi-arity error path
     below — e is owned (unref it), fn is borrowed (the caller releases it). */
  if (stack_guard_exhausted()) {
    exp_t *er = error(ERROR_ILLEGAL_VALUE, e, env,
                      "stack overflow: recursion too deep (use tail recursion, "
                      "or raise the OS stack limit)");
    unrefexp(e);
    return er;
  }

  /* Multi-arity (defn): dispatch on argument count to the matching clause,
     then run that ordinary clause lambda through the normal path. Must come
     before any read of fn->content as a param list. */
  if (fn->flags & FLAG_MULTI) {
    int n = 0;
    for (exp_t *a = e->next; a; a = a->next)
      n++;
    exp_t *chosen = multi_pick(fn->content, n);
    if (!chosen) {
      exp_t *er = error(ERROR_MISSING_PARAMETER, e, env,
                        "no matching clause for %d argument(s)", n);
      unrefexp(e);
      return er;
    }
    return invoke_body(e, chosen, env); /* consumes e; same backtrace frame */
  }

  /* Nested invokes inherit but don't export tail-position: the CALLEE
     decides for its own body. Save/restore around the call. */
  int outer_tail = in_tail_position;
  in_tail_position = 0;

  env_t *newenv;
  exp_t *ret = NULL;
  refexp(fn);
  /* Set when we re-enter via a cross-function tail marker (below): the marker's
     arg nodes already hold EVALUATED values (make_tail_marker pre-evaluated
     them in the caller's frame), so they must be rebound as-is, never
     re-evaluated. Re-evaluating a value that happens to be a pair/symbol (e.g.
     a computed list) would (mis)interpret it as a call. */
  int marker_args = 0;

tailrec: {
  exp_t *body = fn->next->content;
  /* Closure: if fn captured an env at creation, use it as the new
     call frame's parent so let/with bindings from the defining scope
     resolve before walking up to global. Args themselves must be
     evaluated in the CALLER's env (where their free vars live) — so
     we eval first, bind into newenv with evalexp=false. */
  env_t *captured = (env_t *)fn->next->meta;
  /* Pre-evaluate args in the caller's env. Build a fresh list of
     pre-evaluated values so var2env can bind them without re-eval. */
  exp_t *evald_args = NULL, *evald_tail = NULL;
  {
    exp_t *src;
    for (src = e->next; src; src = src->next) {
      /* marker_args: the node already holds an evaluated value — take a ref,
         don't re-evaluate it (see the marker rebind in the cross-function tail
         path below). */
      exp_t *v = marker_args ? refexp(src->content) : EVAL(src->content, env);
      if (v && iserror(v)) {
        /* clean up partial evald list */
        if (evald_args)
          unrefexp(evald_args);
        unrefexp(fn);
        unrefexp(e);
        in_tail_position = outer_tail;
        return v;
      }
      exp_t *node = make_node(v ? v : NIL_EXP);
      if (!evald_args) {
        evald_args = node;
        evald_tail = node;
      } else {
        evald_tail->next = node;
        evald_tail = node;
      }
    }
  }
  marker_args = 0; /* consumed; a further tail hop re-sets it if needed */
  newenv = make_env(captured ? captured : env);
  newenv->callingfnc = refexp(e);
  exp_t *params = lambda_params(fn);
  if ((ret = var2env(e, params, evald_args, newenv, false))) {
    if (evald_args)
      unrefexp(evald_args);
    destroy_env(newenv);
    unrefexp(fn);
    unrefexp(e);
    in_tail_position = outer_tail;
    return ret;
  }
  if (evald_args)
    unrefexp(evald_args);

  /* Compiled body: cross-function tail calls lose TCO here (internal
     OP_TAIL_SELF still applies). */
  if (fn->flags & FLAG_COMPILED) {
#ifdef ALCOVE_JIT
    if (fn->bc->jit) {
      ret = fn->bc->jit(newenv);
      if (!ret)
        ret = vm_run(fn, newenv); /* JIT deopt → bytecode */
    } else
#endif
      ret = vm_run(fn, newenv);
    destroy_env(newenv);
    unrefexp(fn);
    unrefexp(e);
    in_tail_position = outer_tail;
    return ret;
  }

  /* Break-on-function: now that args are bound, arm a stop so the per-form hook
     lands on the first BODY form with the callee's params in scope (not on an
     argument expression evaluated back in the caller). */
  if (g_debug && fn->meta && dbg_fn_breakpointed((const char *)fn->meta))
    g_dbg_mode = 1;
  exp_t *cur = body;
  while (cur) {
    if (ret) {
      unrefexp(ret);
      ret = NULL;
    }
    int is_last = (cur->next == NULL);
    in_tail_position = is_last;
    ret = EVAL(cur->content, newenv);

    if (is_last && ret && ispair(ret) && (ret->flags & FLAG_TAILREC) &&
        islambda(ret->content)) {
      exp_t *marker = ret;
      ret = NULL;
      exp_t *resolved_fn = marker->content;

      if (resolved_fn == fn) {
        /* Self-recursion fast path: rebind params in place, skip the
           env teardown/rebuild. Marker args are already evaluated. */
        int i;
        for (i = 0; i < newenv->n_inline; i++)
          unrefexp(newenv->inline_vals[i]);
        newenv->n_inline = 0;
        if (newenv->d) {
          destroy_dict(newenv->d);
          newenv->d = NULL;
        }

        exp_t *curvar = fn->content;
        exp_t *curval = marker->next;
        while (curvar && curval) {
          exp_t *v = curval->content;
          if (issymbol(curvar->content)) {
            if (newenv->n_inline < ENV_INLINE_SLOTS) {
              newenv->inline_keys[newenv->n_inline] = exp_text(curvar->content);
              newenv->inline_vals[newenv->n_inline] = refexp(v);
              newenv->n_inline++;
            } else {
              if (!newenv->d)
                newenv->d = create_dict();
              set_get_keyval_dict(newenv->d, exp_text(curvar->content), v);
            }
          }
          curvar = curvar->next;
          curval = curval->next;
        }

        unrefexp(marker);
        /* self-tail loop back-edge (AST tier): runaway-budget checkpoint.
           Build the error before unref'ing e (error() refs it). */
        int _b = budget_check();
        if (_b) {
          exp_t *er = error(ERROR_ILLEGAL_VALUE, e, env,
                            _b == 2 ? "interrupted: memory limit exceeded"
                                    : "interrupted: time limit exceeded");
          destroy_env(newenv);
          unrefexp(fn);
          unrefexp(e);
          in_tail_position = outer_tail;
          return er;
        }
        cur = body; /* restart the body loop */
        continue;
      }

      /* Different function: full unwind + tailrec jump. */
      exp_t *new_fn = resolved_fn;
      marker->content = NULL;
      if (new_fn->meta) {
        marker->content =
            make_symbol((char *)new_fn->meta, strlen((char *)new_fn->meta));
      } else {
        marker->content = make_symbol("_", 1);
      }
      marker->flags &= ~FLAG_TAILREC;

      destroy_env(newenv);
      unrefexp(fn);
      unrefexp(e);
      fn = new_fn;
      e = marker;
      marker_args =
          1; /* marker's arg nodes are pre-evaluated — don't re-eval */
      /* The tail call may resolve to a MULTI (defn) wrapper — re-dispatch on
         the marker's arg count to the matching clause, exactly as the entry
         path does. The wrapper has no body of its own (fn->next is NULL), so
         jumping to tailrec with it read through NULL: (defn f ((n) (f n 0))
         ((n acc) ...tail (f ...))) segfaulted on the AST tier. */
      if (fn->flags & FLAG_MULTI) {
        int _n = 0;
        for (exp_t *a = e->next; a; a = a->next)
          _n++;
        exp_t *chosen = multi_pick(fn->content, _n);
        if (!chosen) {
          exp_t *er = error(ERROR_MISSING_PARAMETER, e, env,
                            "no matching clause for %d argument(s)", _n);
          unrefexp(fn);
          unrefexp(e);
          in_tail_position = outer_tail;
          return er;
        }
        refexp(chosen); /* own the clause; drop the wrapper's ref */
        unrefexp(fn);
        fn = chosen;
      }
      /* cross-function tail loop back-edge (AST tier): runaway-budget
         checkpoint. newenv is already destroyed above; we own fn (new_fn) and
         e (marker). Build the error before unref'ing e (error() refs it). */
      {
        int _b = budget_check();
        if (_b) {
          exp_t *er = error(ERROR_ILLEGAL_VALUE, e, env,
                            _b == 2 ? "interrupted: memory limit exceeded"
                                    : "interrupted: time limit exceeded");
          unrefexp(fn);
          unrefexp(e);
          in_tail_position = outer_tail;
          return er;
        }
      }
      goto tailrec;
    }

    if (ret && iserror(ret)) {
      destroy_env(newenv);
      unrefexp(fn);
      unrefexp(e);
      in_tail_position = outer_tail;
      return ret;
    }
    cur = cur->next;
  }

  destroy_env(newenv);
  unrefexp(fn);
  unrefexp(e);
  in_tail_position = outer_tail;
  return ret;
}
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
exp_t *expandmacro(exp_t *e, exp_t *fn, env_t *env) {
  env_t *newenv = make_env(NULL); // NULL instead of env
  exp_t *ret;

  if ((ret = var2env(e, fn->content, e->next, newenv, false))) {
    destroy_env(newenv);
    return ret;
  }
  /* Macro body evaluation produces the expansion AST. If a user-fn
     call in the body's tail position inherited in_tail_position from
     the macro caller, it would return a tail marker that becomes the
     "expansion" — invokemacro then evaluates the marker as if it were
     code, which mis-dispatches via FLAG_TAILREC. Body eval is NOT in
     tail position relative to the macro's caller — the caller's tail
     position applies to the EXPANSION's eval (invokemacro line 13337). */
  int outer_tail = in_tail_position;
  in_tail_position = 0;
  ret = EVAL(fn->next->content, newenv);
  in_tail_position = outer_tail;
  destroy_env(newenv);
  return ret;
}
#pragma GCC diagnostic warning "-Wunused-parameter"

exp_t *invokemacro(exp_t *e, exp_t *fn, env_t *env) {
  /* e->content = fn name,
     e->next->content->
     fn->content = var names
     fn-> next-> content =body*/
  exp_t *ret;
  ret = expandmacro(e, fn, env);
  unrefexp(e);
  if (ret && iserror(ret))
    return ret;
  ret = evaluate(ret, env);

  return ret;
}

/* AST side of infix dispatch (see is_infix_op). `e` is a form whose head has
   ALREADY evaluated to the non-callable value `head_val` (borrowed). If `e` is
   exactly (head op rhs) and the operator position evaluates to a binary
   builtin, evaluate it as (op head rhs): sets *matched=1 and returns the owned
   result (or an owned error). Otherwise *matched=0 and returns NULL — the
   caller self-evaluates the form as before. */
static exp_t *ast_try_infix(exp_t *head_val, exp_t *e, env_t *env,
                            int *matched) {
  *matched = 0;
  if (!e || !e->next || !e->next->next || e->next->next->next)
    return NULL; /* not exactly a 3-element (head op rhs) */
  int outer_tail = in_tail_position;
  in_tail_position = 0;
  exp_t *op = EVAL(e->next->content, env); /* operator position */
  int idx = infix_op_index(op);
  if (idx < 0 && !infix_is_fn(op)) {
    in_tail_position = outer_tail;
    unrefexp(op);
    return NULL; /* 2nd element is neither an operator nor a function */
  }
  *matched = 1;
  exp_t *rhs = EVAL(e->next->next->content, env);
  in_tail_position = outer_tail;
  if (iserror(rhs)) {
    unrefexp(op);
    return rhs;
  }
  /* head_val/rhs borrowed. Fixed operators keep the symbol path (so cmpcmd
     decodes < > <= >= and the arith tower stays identical); any other function
     is applied directly as (op head rhs). */
  exp_t *res = idx >= 0 ? infix_apply(idx, head_val, rhs, env)
                        : infix_apply_fn(op, head_val, rhs, env);
  unrefexp(op);
  unrefexp(rhs);
  return res;
}

exp_t *evaluate(exp_t *e, env_t *env) {
  /* TO DO UN REF VARS*/
  exp_t *tmpexp = NULL;
  exp_t *tmpexp2 = NULL;
  exp_t *tmpevexp = NULL; /* Evaluated structure to be freed*/
  exp_t *ret = NULL;

  if (e == NULL)
    return NULL;
  if isatom (e) {
    if issymbol (e) {
      if (((char *)exp_text(e))[0] == ':')
        return e; // e is a keyword
      if ((tmpexp = lookup(e, env))) {
        unrefexp(e);
        return tmpexp;
      } else {
        const char *_nm = (const char *)exp_text(e);
        const char *_sg = alc_suggest_symbol(_nm, env);
        ret = _sg ? error(ERROR_UNBOUND_VARIABLE, e, env,
                          "Unbound variable %s (did you mean '%s'?)", _nm, _sg)
                  : error(ERROR_UNBOUND_VARIABLE, e, env, "Unbound variable %s",
                          _nm);
        unrefexp(e);
        return ret;
      }
    } else
      return e; // Number? String? Char? Boolean? Vector?
  } else if ispair (e) {
    if (g_debug)
      dbg_hook(e, env); /* gated; one never-taken branch when not debugging */
    tmpexp = car(e);
    if (tmpexp && ispair(tmpexp)) {
      tmpevexp = EVAL(tmpexp, env);
      tmpexp = tmpevexp;
      /* If evaluating the head form itself raised (e.g. an unbound operator in
         ((undefined-fn) 1 2)), propagate that error. Without this the form
         falls through every callable-shape arm to the `return e` below, which
         silently swallows the error (returning the form as if self-evaluated)
         and leaks the evaluated head. */
      if (tmpexp && iserror(tmpexp)) {
        ret = tmpevexp;
        tmpevexp = NULL;
        goto finish;
      }
    }
    if (tmpexp) {
      if isinternal (tmpexp) {
        int was_tail = in_tail_position;
        if (!(tmpexp->flags & FLAG_TAIL_AWARE))
          in_tail_position = 0;
        ret = invoke_internal(tmpexp, e, env);
        in_tail_position = was_tail;
        goto finisht;
      }
      if (issymbol(tmpexp) && !tmpevexp) {
        if (((char *)exp_text(tmpexp))[0] == ':') {
          int ifx_matched;
          exp_t *ifx = ast_try_infix(tmpexp, e, env, &ifx_matched);
          if (ifx_matched) {
            ret = ifx;
            goto finisht;
          }
          ret = error(ERROR_ILLEGAL_VALUE, e, env,
                      "Error keyword %s can not be used as function",
                      exp_text(tmpexp));
          goto finish;
        } // e is a keyword
        if ((tmpexp2 = lookup(tmpexp, env))) {
#ifdef ALCOVE_FFI
          if isffi (tmpexp2) {
            /* Eval each arg and dispatch through libffi to the C
               function held by tmpexp2. Clear in_tail_position around
               arg eval — FFI args are real values to marshal, not
               return values, so a user-fn call inside an arg must NOT
               produce a tail marker (which alc_ffi_call would reject
               as a non-number/non-string and fail type validation). */
            int outer_tail = in_tail_position;
            in_tail_position = 0;
            alc_ffi_t *f = (alc_ffi_t *)tmpexp2->ptr;
            int n = 0;
            exp_t *acur = e->next;
            while (acur && n < ALC_FFI_MAX_ARGS) {
              n++;
              acur = acur->next;
            }
            exp_t **argv = (n > 0) ? memalloc(n, sizeof(exp_t *)) : NULL;
            acur = e->next;
            int i = 0;
            for (; i < n; i++, acur = acur->next) {
              argv[i] = EVAL(acur->content, env);
              if (iserror(argv[i])) {
                exp_t *eret = argv[i];
                for (int j = 0; j < i; j++)
                  unrefexp(argv[j]);
                free(argv);
                unrefexp(tmpexp2);
                in_tail_position = outer_tail;
                ret = eret;
                goto finish;
              }
            }
            ret = alc_ffi_call(f, n, argv);
            free(argv);
            unrefexp(tmpexp2);
            in_tail_position = outer_tail;
            goto finisht;
          }
#endif
          if isinternal (tmpexp2) {
            /* Tail flag propagates only into tail-aware cmds (ifcmd).
               Others get it cleared so their sub-evaluations don't
               misfire and build spurious trampoline markers. */
            int was_tail = in_tail_position;
            if (!(tmpexp2->flags & FLAG_TAIL_AWARE))
              in_tail_position = 0;
            ret = invoke_internal(tmpexp2, e, env);
            in_tail_position = was_tail;
            goto finisht;
          } else if islambda (tmpexp2) {
            if (in_tail_position && !g_debug) { /* debug: real frames, no TCO */
              ret = make_tail_marker(tmpexp2, e, env);
              unrefexp(tmpexp2);
              goto finisht;
            }
            ret = invoke(e, tmpexp2, env);
            /* invoke() borrows fn (its own refexp/unrefexp net to zero) and
               consumes e — but tmpexp2 is a separately-owned ref from the
               lookup above, so release it here like every sibling branch
               does. Omitting this leaked one ref per call on env-resolved
               closures, pinning the closure↔frame cycle alive. */
            unrefexp(tmpexp2);
            goto finisht;
          } else if (iscont(tmpexp2)) {
            /* (k v) — invoking an escape continuation: yields an escape token
               that propagates up to the matching call/cc frame. */
            ret = eval_cont_call(tmpexp2, e, env);
            unrefexp(tmpexp2);
            goto finish; /* eval_cont_call did not consume e */
          } else if ismacro (tmpexp2) {
            ret = invokemacro(e, tmpexp2, env);
            /* Same ownership as the lambda branch above: invokemacro borrows
               fn and consumes e, so release the looked-up tmpexp2 ref here.
               (A macro defined inside a fn body leaked its frame otherwise.) */
            unrefexp(tmpexp2);
            goto finisht;
          } else if (iscallable_container(tmpexp2)) {
            /* (c arg) where c is a var bound to a callable container —
               indexable: element by int index; keyed (dict/hamt/set):
               value/member by key. Without this the lookup path returned the
               container whole, ignoring the arg. Clear tail position for the
               arg eval — a leaked tail marker would otherwise be misread. */
            int outer_tail = in_tail_position;
            in_tail_position = 0;
            exp_t *idx = EVAL(cadr(e), env);
            in_tail_position = outer_tail;
            if (iserror(idx)) {
              unrefexp(tmpexp2);
              ret = idx;
              goto finish;
            }
            /* (s op rhs) with op an operator/function -> infix (op s rhs),
               reusing the already-evaluated idx as the operator. */
            int cinfix;
            exp_t *cifx = ast_container_infix(tmpexp2, idx, e, env, &cinfix);
            if (cinfix) {
              unrefexp(idx);
              unrefexp(tmpexp2);
              ret = cifx;
              goto finish;
            }
            ret = container_apply(tmpexp2, idx, env); /* consumes idx */
            unrefexp(tmpexp2);
            goto finish;
          } else {
            /* Infix: (a op b) where a is a variable bound to a non-callable
               value and op is a binary operator -> (op a b). Gated on
               non-callable head, so (apply + xs) etc. are unaffected. */
            int ifx_matched;
            exp_t *ifx = ast_try_infix(tmpexp2, e, env, &ifx_matched);
            if (ifx_matched) {
              unrefexp(tmpexp2);
              ret = ifx;
              goto finisht;
            }
            /* Any other value (a number/float/rational/decimal/pair/nil) in
               head position is NOT callable and NOT an infix form: calling it
               is an error, matching the VM (vm_invoke_values). Identical
               message text so the two tiers compare equal. */
            unrefexp(tmpexp2);
            ret = error(ERROR_ILLEGAL_VALUE, e, env,
                        "call: head is not a function");
            goto finish;
          }
        } else {
          const char *_nm = (const char *)exp_text(tmpexp);
          const char *_sg = alc_suggest_symbol(_nm, env);
          ret =
              _sg ? error(ERROR_UNBOUND_VARIABLE, e, env,
                          "Unbound variable %s (did you mean '%s'?)", _nm, _sg)
                  : error(ERROR_UNBOUND_VARIABLE, e, env, "Unbound variable %s",
                          _nm);
          goto finish;
        }
        /* Unreachable today: every arm of the bound-symbol dispatch above, and
           the unbound-symbol else, transfers control via goto. Kept as a
           defensive fallthrough — if a future dispatch arm omits its goto, this
           returns the form itself instead of falling off into undefined
           behavior. */
        ret = e;
        goto finisht;
      } else if (iscallable_container(tmpexp)) {
        /* (c arg) literal container head — indexable element or keyed lookup.
           Same tail-clear as the symbol-bound path above. tmpexp is borrowed
           from e (not unref'd); container_apply consumes the evaluated arg. */
        int outer_tail = in_tail_position;
        in_tail_position = 0;
        tmpexp2 = EVAL(cadr(e), env);
        in_tail_position = outer_tail;
        if (iserror(tmpexp2)) {
          ret = tmpexp2;
          goto finish;
        }
        /* (s op rhs) infix on a literal container head — see
           ast_container_infix. tmpexp (container) is borrowed from e; tmpexp2
           (op) is owned. */
        int lcinfix;
        exp_t *lcifx = ast_container_infix(tmpexp, tmpexp2, e, env, &lcinfix);
        if (lcinfix) {
          unrefexp(tmpexp2);
          ret = lcifx;
          goto finish;
        }
        ret = container_apply(tmpexp, tmpexp2, env); /* consumes tmpexp2 */
        goto finish;
      } else if (islambda(tmpexp)) {
        if (in_tail_position && !g_debug) { /* debug: real frames, no TCO */
          ret = make_tail_marker(tmpexp, e, env);
          goto finisht;
        }
        ret = invoke(e, tmpexp, env);
        goto finisht;
      } else if (iscont(tmpexp)) {
        ret = eval_cont_call(tmpexp, e, env); /* tmpexp borrowed from e */
        goto finish;
      } else if (ismacro(tmpexp)) {
        ret = invokemacro(e, tmpexp, env);
        goto finisht;
      }
      /* Infix: (head op rhs) with a non-callable head and a binary-operator
         middle evaluates as (op head rhs) — e.g. (a + b) where a is a number.
         Gated on non-callable head, so HOF calls like (apply + xs) are safe. */
      {
        int ifx_matched;
        exp_t *ifx = ast_try_infix(tmpexp, e, env, &ifx_matched);
        if (ifx_matched) {
          ret = ifx;
          goto finisht;
        }
      }
      /* Pair head evaluated to a non-callable, non-infix value (a number/
         float/rational/decimal/pair/nil): calling it is an error, matching the
         VM (vm_invoke_values). Identical message text so the two tiers compare
         equal. goto finish (unref e) since e is no longer the return value. */
      ret = error(ERROR_ILLEGAL_VALUE, e, env, "call: head is not a function");
      goto finish;
    } else
      return e; /* head evaluated to NULL — return the form unchanged */
  } else {
    // is ???
    return e;
  }
  return e;
finish:
  unrefexp(e);
finisht:
  unrefexp(tmpevexp);
  return ret;
}

/* REPL prompt hook. If `varname` (*prompt-in* / *prompt-out*) is bound to a
   function, call it with the cell number `idx` and return its string result as
   a fresh malloc'd buffer (the caller frees) — so the hook supplies its own
   text/coloring/spacing. Returns NULL on unset/nil, a non-function value, a
   hook error, or a non-string result, so the caller falls back to the built-in
   default; a broken hook can therefore never brick the REPL. The hook takes
   one argument (the cell number) and returns a string. Used by every prompt
   site: the interactive readline readers and the piped/non-TTY printf paths. */
static char *repl_prompt_str(env_t *env, const char *varname, int idx) {
  if (!env || !env->d)
    return NULL;
  keyval_t *kv = set_get_keyval_dict(env->d, (char *)varname, NULL);
  exp_t *fn = kv ? (exp_t *)kv->val : NULL;
  if (!fn || !islambda(fn))
    return NULL;
  exp_t *argv[1] = {make_integeri(idx)}; /* alc_apply_n consumes this ref */
  exp_t *res = alc_apply_n(fn, 1, argv, env);
  char *out = NULL;
  if (res && !iserror(res) && isstring(res))
    out = strdup((const char *)exp_text(res));
  if (res)
    unrefexp(res);
  return out;
}

/* The post-transform input text of the line currently being evaluated, set in
   repl_eval_text after *input-hook* runs and read by the *output-hook* call in
   repl_eval_print_form. NULL when no line text is available (e.g. the piped
   non-readline stream loop), where the output hook serializes the form instead.
 */
static const char *g_repl_input = NULL;

/* Input-transform hook. If `varname` (*input-hook*) is bound to a function,
   call it with `input` (the source text) and return its STRING result as a
   fresh malloc'd buffer (caller frees) — the text to evaluate in place of the
   original. Returns NULL on unset/nil, a non-function, a hook error, or a
   non-string result, so the caller keeps the original input; a broken hook
   can't break the REPL. The hook takes one argument (the input string) and
   returns a string. */
static char *repl_hook_transform(env_t *env, const char *varname,
                                 const char *input) {
  if (!env || !env->d || !input)
    return NULL;
  keyval_t *kv = set_get_keyval_dict(env->d, (char *)varname, NULL);
  exp_t *fn = kv ? (exp_t *)kv->val : NULL;
  if (!fn || !islambda(fn))
    return NULL;
  exp_t *argv[1] = {make_string((char *)input, (int)strlen(input))};
  exp_t *res = alc_apply_n(fn, 1, argv, env);
  char *out = NULL;
  if (res && !iserror(res) && isstring(res))
    out = strdup((const char *)exp_text(res));
  if (res)
    unrefexp(res);
  return out;
}

#ifdef ALCOVE_READLINE
#undef ISDIGIT /* char.h defines ISDIGIT as a bitmask; readline redefines it   \
                */
#include <readline/history.h>
#include <readline/readline.h>
#include <sys/stat.h> /* chmod(0600) on the persisted history file */

/* Iterate over a dict and emit names matching `prefix` into the
   readline-allocated match list. Each match must be malloc'd; readline
   takes ownership and free()s it. */
static void rl_collect_dict(dict_t *d, const char *prefix, size_t plen,
                            char ***out, int *nout, int *cap) {
  if (!d)
    return;
  int h;
  size_t i;
  keyval_t *k;
  for (h = 0; h < 2; h++) {
    if (!d->ht[h].size)
      continue;
    for (i = 0; i < d->ht[h].size; i++) {
      for (k = d->ht[h].table[i]; k; k = k->next) {
        if (!k->key)
          continue;
        if (plen && strncmp((const char *)k->key, prefix, plen) != 0)
          continue;
        if (*nout >= *cap) {
          *cap = *cap ? *cap * 2 : 16;
          *out = xrealloc(*out, sizeof(char *) * (*cap));
        }
        (*out)[(*nout)++] = strdup((const char *)k->key);
      }
    }
  }
}

/* Collect every binding visible from `start` (each frame's inline keys + its
   overflow dict, walking root-ward) whose name has the given prefix. Sibling to
   rl_collect_dict; shared by the REPL and debugger completion generators so the
   identical env-walk doesn't live in two places and drift. */
static void rl_collect_env_chain(env_t *start, const char *prefix, size_t plen,
                                 char ***out, int *nout, int *cap) {
  for (env_t *cur = start; cur; cur = cur->root) {
    for (int i = 0; i < cur->n_inline; i++) {
      const char *kk = cur->inline_keys[i];
      if (!kk || (plen && strncmp(kk, prefix, plen) != 0))
        continue;
      if (*nout >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *out = xrealloc(*out, sizeof(char *) * (*cap));
      }
      (*out)[(*nout)++] = strdup(kk);
    }
    rl_collect_dict(cur->d, prefix, plen, out, nout, cap);
  }
}

/* readline completion generator — called repeatedly with state=0 first,
   then state>0 until it returns NULL. Builds the match list lazily on
   the first call.

   readline takes ownership of every string we return and free()s it
   itself. On a fresh state==0 call we therefore must NOT free the
   strings still in our array — only the array storage. The strings
   readline never consumed (if any) leak; in practice readline
   exhausts the generator so this is rare. */
static char *alcove_completion_generator(const char *text, int state) {
  static char **matches = NULL;
  static int n_matches = 0;
  static int cap = 0;
  static int idx = 0;
  if (state == 0) {
    /* Drop the array but NOT its contents — readline owns those now. */
    free(matches);
    matches = NULL;
    n_matches = 0;
    cap = 0;
    idx = 0;

    size_t tlen = strlen(text);
    rl_collect_dict(reserved_symbol, text, tlen, &matches, &n_matches, &cap);
    /* Walk env chain inner→outer (covers global defs). */
    rl_collect_env_chain(g_global_env, text, tlen, &matches, &n_matches, &cap);
  }
  if (idx < n_matches)
    return matches[idx++]; /* readline frees the strdup */
  return NULL;
}
static char **alcove_rl_completer(const char *text, int start, int end) {
  (void)start;
  (void)end;
  rl_attempted_completion_over = 1; /* skip default file completion */
  return rl_completion_matches(text, alcove_completion_generator);
}

/* Quick paren depth — comments and string literals don't count. Returns
   the running depth (0 = balanced, >0 = need more, <0 = extra closer). */
static int rl_paren_depth(const char *s) {
  int depth = 0, in_string = 0;
  while (*s) {
    if (in_string) {
      if (*s == '\\' && s[1])
        s += 2;
      else if (*s == '"') {
        in_string = 0;
        s++;
      } else
        s++;
    } else {
      if (*s == '"') {
        in_string = 1;
        s++;
      } else if (*s == '(') {
        depth++;
        s++;
      } else if (*s == ')') {
        depth--;
        s++;
      } else if (*s == ';') {
        while (*s && *s != '\n')
          s++;
      } else
        s++;
    }
  }
  return depth;
}

/* Keywords that get the bold-magenta treatment in the colored
   redisplay. Kept short on purpose — coloring random user-defined
   names would be misleading. */
static const char *alcove_kw[] = {
    "def",  "fn",  "if",   "do",   "let",    "for",     "while",
    "and",  "or",  "not",  "is",   "isnt",   "no",      "yes",
    "t",    "nil", "cond", "when", "unless", "quote",   "with",
    "each", "mac", "set",  "=",    "setf",   " return", NULL};
static int alc_is_kw(const char *s, int n) {
  int i;
  for (i = 0; alcove_kw[i]; i++) {
    int kn = (int)strlen(alcove_kw[i]);
    if (kn == n && strncmp(alcove_kw[i], s, n) == 0)
      return 1;
  }
  return 0;
}
static int alc_sym_char(unsigned char c) {
  /* alcove symbol chars: anything that isn't whitespace, paren, quote,
     comma, comment-start, or string delimiter. */
  if (c <= ' ')
    return 0;
  if (c == '(' || c == ')' || c == '\'' || c == '`' || c == ',' || c == ';' ||
      c == '"')
    return 0;
  return 1;
}
/* Walk `s` of length n and emit ANSI-colored output to `out`. Strings,
   comments, parens, numbers, and a small set of keywords get colored;
   everything else passes through. */
static void alc_print_colored(const char *s, int n, FILE *out) {
  int i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c == '(' || c == ')') {
      fprintf(out, "\x1B[33m%c\x1B[39m", c);
      i++;
    } else if (c == ';') {
      fputs("\x1B[90m", out);
      while (i < n && s[i] != '\n') {
        fputc(s[i], out);
        i++;
      }
      fputs("\x1B[39m", out);
    } else if (c == '"') {
      fputs("\x1B[32m\"", out);
      i++;
      while (i < n && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < n) {
          fputc(s[i], out);
          i++;
        }
        if (i < n) {
          fputc(s[i], out);
          i++;
        }
      }
      if (i < n) {
        fputc(s[i], out);
        i++;
      } /* closing quote */
      fputs("\x1B[39m", out);
    } else if ((c >= '0' && c <= '9') ||
               (c == '-' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9' &&
                (i == 0 || !alc_sym_char((unsigned char)s[i - 1])))) {
      fputs("\x1B[36m", out);
      if (c == '-') {
        fputc('-', out);
        i++;
      }
      while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) {
        fputc(s[i], out);
        i++;
      }
      fputs("\x1B[39m", out);
    } else if (alc_sym_char(c)) {
      int start = i;
      while (i < n && alc_sym_char((unsigned char)s[i]))
        i++;
      int len = i - start;
      if (alc_is_kw(s + start, len))
        fprintf(out, "\x1B[1;35m%.*s\x1B[22;39m", len, s + start);
      else
        fwrite(s + start, 1, len, out);
    } else {
      fputc(c, out);
      i++;
    }
  }
}
/* Custom readline redisplay: prints prompt + colored line buffer,
   then walks cursor back to rl_point. Limitations: assumes single
   physical terminal line (we don't track wrap). For long lines
   (> 256 chars) we fall back to readline's default redisplay. */
/* Print the prompt while stripping RL_PROMPT_START/END_IGNORE bytes
   (\001 \002). Those markers exist for readline's own width math; if
   we let them through they show up as literal glyphs in the terminal. */
static void alc_print_prompt_stripped(const char *p, FILE *out) {
  if (!p)
    return;
  for (; *p; p++)
    if (*p != '\001' && *p != '\002')
      fputc(*p, out);
}

/* Visible (on-screen) width of a prompt: skip the \001..\002 spans and
   any CSI escape sequences inside them — those render zero-width. */
static int alc_prompt_vwidth(const char *p) {
  int w = 0;
  for (size_t i = 0; p && p[i];) {
    if (p[i] == '\001') {
      while (p[i] && p[i] != '\002')
        i++;
      if (p[i])
        i++;
      continue;
    }
    if (p[i] == '\002') {
      i++;
      continue;
    }
    if (p[i] == '\x1B') {
      i++;
      if (p[i] == '[') {
        i++;
        while (p[i] && !(p[i] >= '@' && p[i] <= '~'))
          i++;
        if (p[i])
          i++;
      }
      continue;
    }
    w++;
    i++;
  }
  return w;
}

/* Cursor's row offset within the current readline render. Reset to 0
   before every readline() call so the first paint starts clean. */
static int g_rd_crow = 0;

/* Resolved *prompt-cont* string for the current form (NULL = built-in default
   "    ... "). Set once per form in rl_read_form/als_rl_read_form so BOTH the
   explicit continuation prompt AND the redisplay (used for pasted/recalled
   multi-line buffers) honor the hook. Owned here; freed at end of the form. */
static char *g_repl_cont = NULL;
/* The continuation prompt the redisplay should paint on wrapped rows, and its
   visible width for cursor tracking. */
static const char *repl_cont_text(void) {
  return g_repl_cont ? g_repl_cont : "    ... ";
}

/* Approximate terminal column width of a codepoint: 0 for combining /
   zero-width marks, 2 for the common East-Asian-wide and emoji ranges,
   1 otherwise. Enough to place the REPL cursor correctly; not a full
   Unicode width table. */
static int alc_cp_width(uint32_t cp) {
  if (cp == 0)
    return 0;
  if ((cp >= 0x0300 && cp <= 0x036F) || /* combining diacritics */
      (cp >= 0x200B && cp <= 0x200F) || /* zero-width spaces / marks */
      (cp >= 0xFE00 && cp <= 0xFE0F) || /* variation selectors */
      cp == 0x00AD)                     /* soft hyphen */
    return 0;
  if ((cp >= 0x1100 && cp <= 0x115F) ||   /* Hangul Jamo */
      (cp >= 0x2E80 && cp <= 0x303E) ||   /* CJK radicals, Kangxi */
      (cp >= 0x3041 && cp <= 0x33FF) ||   /* Kana .. CJK symbols */
      (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK Ext A */
      (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK Unified */
      (cp >= 0xA000 && cp <= 0xA4CF) ||   /* Yi */
      (cp >= 0xAC00 && cp <= 0xD7A3) ||   /* Hangul syllables */
      (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK compatibility */
      (cp >= 0xFE30 && cp <= 0xFE4F) ||   /* CJK compatibility forms */
      (cp >= 0xFF00 && cp <= 0xFF60) ||   /* fullwidth forms */
      (cp >= 0xFFE0 && cp <= 0xFFE6) ||   /* fullwidth signs */
      (cp >= 0x1F300 && cp <= 0x1FAFF) || /* emoji & pictographs */
      (cp >= 0x20000 && cp <= 0x3FFFD))   /* CJK Ext B+ */
    return 2;
  return 1;
}

/* Multi-line-aware colored redisplay. A recalled history entry (or any
   accumulated form) may contain '\n' and span several screen rows;
   instead of the old single-line "\r\x1B[K" (which reprinted row 0 on
   every keystroke — see the history-recall cascade bug) we walk up to
   the render's first row, clear downward, repaint prompt + colored
   buffer, then place the cursor at rl_point's (row,col). Self-contained
   (never mixes with readline's own redisplay), so no screen desync. */
static void alcove_colored_redisplay(void) {
  if (rl_end > 256) { /* pathological line: let readline cope */
    rl_redisplay();
    return;
  }
  FILE *o = rl_outstream;
  const char *pr = rl_display_prompt ? rl_display_prompt : rl_prompt;
  fputc('\r', o);
  if (g_rd_crow > 0)
    fprintf(o, "\x1B[%dA", g_rd_crow); /* up to row 0 of our render */
  fputs("\x1B[J", o);                  /* clear from here to end of screen */
  /* Row 0 gets the real prompt; a multi-line buffer (recalled history
     entry) gets the "    ... " continuation prompt on each later row so
     it looks the same as it did when typed. The prompt is display-only
     — it is never part of rl_line_buffer / the evaluated text. */
  const char *CONT = repl_cont_text();
  const int CONT_W = alc_prompt_vwidth(CONT);
  alc_print_prompt_stripped(pr, o);
  for (int start = 0, i = 0; i <= rl_end; i++) {
    if (i == rl_end || rl_line_buffer[i] == '\n') {
      alc_print_colored(rl_line_buffer + start, i - start, o);
      if (i < rl_end) {
        fputc('\n', o);
        fputs(CONT, o);
        start = i + 1;
      }
    }
  }

  int pw = alc_prompt_vwidth(pr);
  int prow = 0, pcol = pw, erow = 0;
  /* Advance the cursor column by each codepoint's display width, not by
     byte count — a multi-byte char (é, ï, 世) is one (or two) columns,
     not one column per byte. */
  for (int i = 0; i < rl_point;) {
    if (rl_line_buffer[i] == '\n') {
      prow++;
      pcol = CONT_W; /* next row starts past the continuation prompt */
      i++;
    } else {
      size_t off = (size_t)i;
      uint32_t cp = utf8_decode_at(rl_line_buffer, &off);
      pcol += alc_cp_width(cp);
      i = (int)off;
    }
  }
  for (int i = 0; i < rl_end; i++)
    if (rl_line_buffer[i] == '\n')
      erow++;
  /* cursor is at (erow, end). Walk it back to (prow, pcol). */
  if (erow > prow)
    fprintf(o, "\x1B[%dA", erow - prow);
  fputc('\r', o);
  if (pcol > 0)
    fprintf(o, "\x1B[%dC", pcol);
  g_rd_crow = prow;
  fflush(o);
}

/* Every prompt starts a fresh render, so the cursor-row tracker must
   reset before each readline(). Bracketed paste: readline skips its own
   ESC[?2004h handshake whenever an application installs a custom
   rl_redisplay_function (verified empirically against readline 8.2), even
   though its ESC[200~ keybinding still works — so emit/retract the mode
   ourselves around each call. With it, a multi-line paste arrives as ONE
   buffer (newlines as literal chars, rendered by the custom redisplay with
   continuation prompts) instead of every pasted newline acting as Enter
   and stacking the pasted indentation onto the auto-indent. */
static char *alc_readline(const char *prompt) {
  g_rd_crow = 0;
  fputs("\x1B[?2004h", rl_outstream ? rl_outstream : stdout);
  fflush(rl_outstream ? rl_outstream : stdout);
  char *line = readline(prompt);
  fputs("\x1B[?2004l", rl_outstream ? rl_outstream : stdout);
  fflush(rl_outstream ? rl_outstream : stdout);
  return line;
}

/* ---- REPL Ctrl-C: cancel the current input, exit only on an empty line -----
   While a form is being read interactively, SIGINT (Ctrl-C) abandons whatever is
   on the line (and the whole multi-line form) and returns to a fresh prompt; on
   a truly empty prompt it exits. readline's own signal trapping is turned off
   (see repl_install_sigint) so this handler owns SIGINT during readline — the
   form reader's longjmp landing pad restores the terminal via
   rl_cleanup_after_signal. Outside a form read (g_repl_reading == 0, e.g. while
   evaluating) Ctrl-C terminates as before. The accumulator is file-scope so the
   landing pad can free it safely after siglongjmp (a modified local would be
   indeterminate). */
static sigjmp_buf g_repl_sigint_jmp;
static volatile sig_atomic_t g_repl_reading = 0;  /* 1 while blocked in readline */
static volatile sig_atomic_t g_repl_has_text = 0; /* line non-empty / mid-form */
static char *g_repl_acc = NULL;                   /* in-progress form */

static void repl_sigint_handler(int sig) {
  (void)sig;
  if (!g_repl_reading)
    _exit(130); /* not at a prompt (e.g. mid-eval): terminate, as before */
  if (rl_end > 0)
    g_repl_has_text = 1; /* text on the current line -> cancel, don't exit */
  siglongjmp(g_repl_sigint_jmp, 1);
}

/* Landing pad for the SIGINT longjmp, shared by both form readers. Restores the
   terminal readline left in raw mode, ends bracketed paste, drops to a new line,
   and frees the partial form. Returns NULL (empty line -> the REPL loop exits)
   or a fresh "" (had text -> the REPL loop skips it and reprompts). */
static char *repl_sigint_recover(void) {
  g_repl_reading = 0;
  rl_startup_hook = NULL;
  rl_free_line_state();
  rl_cleanup_after_signal();
  FILE *out = rl_outstream ? rl_outstream : stdout;
  fputs("\x1B[?2004l", out); /* disable bracketed paste (alc_readline's pair) */
  fflush(out);
  putchar('\n');
  free(g_repl_acc);
  g_repl_acc = NULL;
  return g_repl_has_text ? strdup("") : NULL;
}

/* Install the Ctrl-C handler and stop readline from trapping SIGINT itself.
   Window-resize (SIGWINCH) handling is independent (rl_catch_sigwinch) and stays
   on. Called from repl_readline_setup, i.e. only when stdin is a tty. */
static void repl_install_sigint(void) {
  rl_catch_signals = 0;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = repl_sigint_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* persistent; no SA_RESTART so the blocking read is broken */
  sigaction(SIGINT, &sa, NULL);
}

/* Read one complete top-level form from the terminal. Continues
   prompting (with a continuation prompt) until paren balance hits 0.
   Returned string is malloc'd. NULL on EOF. */
#ifndef ALCOVE_ALS /* superseded by als_rl_read_form in the adder build */
static char *rl_read_form(int idx) {
  char prompt[64];
  /* Wrap escape codes with \001/\002 (RL_PROMPT_START_IGNORE /
     END_IGNORE) so readline counts visible width correctly. Doesn't
     affect our custom redisplay (which writes the prompt verbatim)
     but makes the cursor land in the right column on fallback. */
  snprintf(prompt, sizeof prompt,
           "\001\x1B[34m\002In "
           "[\001\x1B[94m\002%d\001\x1B[34m\002]:\001\x1B[39m\002 ",
           idx);
  /* Resolve the continuation prompt once per form (freed at the next form's
     start). Cached in g_repl_cont so BOTH the explicit continuation prompt and
     the redisplay — which paints it on the wrapped rows of a pasted or recalled
     multi-line buffer — honor *prompt-cont*. */
  free(g_repl_cont);
  g_repl_cont = repl_prompt_str(g_global_env, "*prompt-cont*", idx);
  g_repl_acc = NULL;
  g_repl_has_text = 0;
  if (sigsetjmp(g_repl_sigint_jmp, 1))
    return repl_sigint_recover(); /* Ctrl-C: cancel form / exit on empty line */
  g_repl_reading = 1;
  char *hook = repl_prompt_str(g_global_env, "*prompt-in*", idx);
  char *line = alc_readline(hook ? hook : prompt);
  free(hook);
  /* The custom redisplay (alcove_colored_redisplay) leaves the cursor
     inside the input line — readline's default redisplay would emit a
     trailing \r\n itself, but our hook doesn't, so the eval result
     would otherwise appear glued to the input. Emit it ourselves. */
  putchar('\n');
  if (!line) {
    g_repl_reading = 0;
    return NULL; /* Ctrl-D on empty line */
  }
  size_t len = strlen(line);
  if (len)
    g_repl_has_text = 1;
  size_t cap = len + 256;
  g_repl_acc = malloc(cap);
  memcpy(g_repl_acc, line, len + 1);
  free(line);
  while (rl_paren_depth(g_repl_acc) > 0) {
    g_repl_has_text = 1; /* mid-form -> Ctrl-C cancels, never exits */
    char *more = alc_readline(repl_cont_text());
    putchar('\n'); /* same fix for continuation lines */
    if (!more)
      break;
    size_t al = strlen(g_repl_acc), ml = strlen(more);
    size_t need = al + ml + 2;
    if (need > cap) {
      cap = need * 2;
      g_repl_acc = xrealloc(g_repl_acc, cap);
    }
    g_repl_acc[al] = '\n'; /* append "\n" + more at the known offset */
    memcpy(g_repl_acc + al + 1, more, ml + 1); /* copies more's NUL too */
    free(more);
  }
  g_repl_reading = 0;
  if (g_repl_acc[0])
    add_history(g_repl_acc);
  char *ret = g_repl_acc;
  g_repl_acc = NULL;
  return ret;
}
#endif /* !ALCOVE_ALS */

#ifdef ALCOVE_ALS
/* True if `line` (after dropping a `#` comment) ends in a block-opening
   colon. Used to decide whether the REPL needs continuation lines. */
static int als_line_opens_block(const char *line) {
  char *nc = als_strip_comment(line);
  size_t n = strlen(nc);
  while (n > 0 && (nc[n - 1] == ' ' || nc[n - 1] == '\t' || nc[n - 1] == '\r'))
    nc[--n] = 0;
  int r = (n > 0 && nc[n - 1] == ':');
  free(nc);
  return r;
}

/* Auto-indent: how many leading spaces the *next* continuation line
   should start with, given everything entered so far. It is the indent
   of the last non-blank line, plus one level (2) if that line opens a
   block. So `def f (x):<enter>` lands you indented under the def. */
static int als_next_indent(const char *acc) {
  const char *line_start = acc, *last = NULL;
  for (const char *p = acc;; p++) {
    if (*p == '\n' || *p == 0) {
      int blank = 1;
      for (const char *q = line_start; q < p; q++)
        if (*q != ' ' && *q != '\t') {
          blank = 0;
          break;
        }
      if (!blank)
        last = line_start;
      if (*p == 0)
        break;
      line_start = p + 1;
    }
  }
  if (!last)
    return 0;
  const char *e = last;
  while (*e && *e != '\n')
    e++;
  int indent = 0;
  while (last[indent] == ' ' || last[indent] == '\t')
    indent++;
  size_t llen = (size_t)(e - last);
  char *buf = (char *)malloc(llen + 1);
  memcpy(buf, last, llen);
  buf[llen] = 0;
  int ob = als_line_opens_block(buf);
  free(buf);
  return indent + (ob ? 2 : 0);
}

/* readline startup hook: seeds the line buffer with the pending
   auto-indent. Using rl_startup_hook (not rl_pre_input_hook) means the
   text is inserted *before* the first prompt paint, so the indent +
   cursor show immediately with no manual rl_redisplay() (which would
   double the prompt under our custom redisplay function). */
static int als_pending_indent = 0;
static int als_preinput(void) {
  if (als_pending_indent > 0) {
    char sp[128];
    int n = als_pending_indent < (int)sizeof sp - 1 ? als_pending_indent
                                                    : (int)sizeof sp - 1;
    memset(sp, ' ', (size_t)n);
    sp[n] = 0;
    rl_insert_text(sp);
  }
  return 0;
}

/* adder-aware form reader for the interactive prompt. A one-line
   form (balanced parens, no trailing `:`) submits on Enter. Anything
   that opens a block or has unbalanced parens enters continuation mode
   ("    ... " prompt, auto-indented) and submits on a whitespace-only
   line once parens balance. Returns the raw text (malloc'd); NULL EOF. */
static char *als_rl_read_form(int idx) {
  char prompt[64];
  snprintf(prompt, sizeof prompt,
           "\001\x1B[34m\002In "
           "[\001\x1B[94m\002%d\001\x1B[34m\002]:\001\x1B[39m\002 ",
           idx);
  /* Resolve the continuation prompt once per form (freed at the next form's
     start). Cached in g_repl_cont so BOTH the explicit continuation prompt and
     the redisplay — which paints it on the wrapped rows of a pasted or recalled
     multi-line buffer — honor *prompt-cont*. */
  free(g_repl_cont);
  g_repl_cont = repl_prompt_str(g_global_env, "*prompt-cont*", idx);
  g_repl_acc = NULL;
  g_repl_has_text = 0;
  if (sigsetjmp(g_repl_sigint_jmp, 1))
    return repl_sigint_recover(); /* Ctrl-C: cancel form / exit on empty line */
  g_repl_reading = 1;
  char *hook = repl_prompt_str(g_global_env, "*prompt-in*", idx);
  char *line = alc_readline(hook ? hook : prompt);
  free(hook);
  putchar('\n');
  if (!line) {
    g_repl_reading = 0;
    return NULL;
  }
  size_t len = strlen(line);
  if (len)
    g_repl_has_text = 1;
  size_t cap = len + 256;
  g_repl_acc = malloc(cap);
  memcpy(g_repl_acc, line, len + 1);
  free(line);
  /* A recalled (or pasted) history entry arrives from the first
     readline() as one buffer that already contains '\n'. Treat that as
     an open block too, so the user lands back in continuation mode and
     can append lines / submit with a blank line — same as fresh input.
     Without this, recalling a multi-line form and hitting Enter would
     submit immediately instead of letting it be extended. */
  int multiline = (rl_paren_depth(g_repl_acc) > 0) ||
                  als_line_opens_block(g_repl_acc) ||
                  memchr(g_repl_acc, '\n', len) != NULL;
  if (multiline) {
    g_repl_has_text = 1; /* mid-form -> Ctrl-C cancels, never exits */
    for (;;) {
      als_pending_indent = als_next_indent(g_repl_acc);
      rl_startup_hook = als_preinput;
      char *more = alc_readline(repl_cont_text());
      rl_startup_hook = NULL;
      putchar('\n');
      if (!more)
        break; /* Ctrl-D ends the block early */
      int blank = 1;
      for (char *q = more; *q; q++)
        if (*q != ' ' && *q != '\t') {
          blank = 0;
          break;
        }
      if (blank && rl_paren_depth(g_repl_acc) <= 0) {
        free(more); /* whitespace-only line + balanced parens -> submit */
        break;
      }
      size_t al = strlen(g_repl_acc), ml = strlen(more);
      size_t need = al + ml + 2;
      if (need > cap) {
        cap = need * 2;
        g_repl_acc = xrealloc(g_repl_acc, cap);
      }
      g_repl_acc[al] = '\n'; /* append "\n" + more at the known offset */
      memcpy(g_repl_acc + al + 1, more, ml + 1); /* copies more's NUL too */
      free(more);
    }
  }
  g_repl_reading = 0;
  if (g_repl_acc[0])
    add_history(g_repl_acc);
  char *ret = g_repl_acc;
  g_repl_acc = NULL;
  return ret;
}

#endif /* ALCOVE_ALS */

/* TAB at the start of a line — column 0, or only whitespace before the cursor
   — inserts one indent level instead of triggering completion. With a real
   token to the left it completes as usual, so `(fi<TAB>` etc. keep working.
   Shared by both binaries: convenient indentation in plain alcove, and
   required by Adder's whitespace-significant syntax. */
#define ALCOVE_INDENT "  " /* one indent level = 2 spaces */
static int alcove_smart_tab(int count, int key) {
  for (int i = 0; i < rl_point; i++)
    if (rl_line_buffer[i] != ' ' && rl_line_buffer[i] != '\t')
      return rl_complete(count, key); /* a token precedes -> complete */
  rl_insert_text(ALCOVE_INDENT);
  return 0;
}

/* Shift-TAB (back-tab, the ESC[Z sequence) dedents: removes up to one indent
   level (2 spaces) immediately before the cursor, when present. A no-op if the
   cursor isn't preceded by spaces. The inverse of alcove_smart_tab's indent. */
static int alcove_back_tab(int count, int key) {
  (void)count;
  (void)key;
  int k = 0;
  while (k < 2 && rl_point - k - 1 >= 0 &&
         rl_line_buffer[rl_point - k - 1] == ' ')
    k++;
  if (k > 0) {
    rl_delete_text(rl_point - k, rl_point);
    rl_point -= k;
  }
  return 0;
}

/* ---- Debugger tab-completion (readline) -----------------------------------
 */
static const char *g_dbg_commands[] = {
    "bt",     "backtrace", "where", "frame", "up",   "down",
    "locals", "p",         "print", "step",  "next", "continue",
    "break",  "return",    "quit",  "help",  NULL};
/* First word: complete a debug command name (empty input lists them all). */
static char *dbg_cmd_generator(const char *text, int state) {
  static int idx;
  static size_t tl;
  if (state == 0) {
    idx = 0;
    tl = strlen(text);
  }
  while (g_dbg_commands[idx]) {
    const char *cmd = g_dbg_commands[idx++];
    if (!tl || strncmp(cmd, text, tl) == 0)
      return strdup(cmd);
  }
  return NULL;
}
/* After p / print / break: complete a symbol from the selected frame's scope
   (locals via its env chain → globals) plus the builtins. */
static char *dbg_sym_generator(const char *text, int state) {
  static char **m = NULL;
  static int n = 0, cap = 0, idx = 0;
  if (state == 0) {
    free(m);
    m = NULL;
    n = 0;
    cap = 0;
    idx = 0;
    size_t tl = strlen(text);
    rl_collect_dict(reserved_symbol, text, tl, &m, &n, &cap); /* builtins */
    rl_collect_env_chain(g_dbg_complete_env, text, tl, &m, &n, &cap);
  }
  if (idx < n)
    return m[idx++];
  return NULL;
}
static char **dbg_rl_completer(const char *text, int start, int end) {
  (void)end;
  rl_attempted_completion_over = 1; /* never fall back to filename completion */
  if (start == 0)
    return rl_completion_matches(text,
                                 dbg_cmd_generator); /* the command word */
  const char *b = rl_line_buffer;
  while (*b == ' ' || *b == '\t')
    b++;
  if (strncmp(b, "p ", 2) == 0 || strncmp(b, "print ", 6) == 0 ||
      strncmp(b, "break ", 6) == 0 || strncmp(b, "b ", 2) == 0 ||
      strncmp(b, "return ", 7) == 0)
    return rl_completion_matches(text, dbg_sym_generator);
  return NULL;
}
/* Read one debug command on a tty with completion (TAB → commands / symbols).
   g_dbg_complete_env is the frame whose locals feed symbol completion. Returns
   a malloc'd, newline-free line (caller frees), or NULL at EOF. */
static char *dbg_readline_tty(env_t *frame_env) {
  g_dbg_complete_env = frame_env;
  rl_completion_func_t *prev = rl_attempted_completion_function;
  rl_attempted_completion_function = dbg_rl_completer;
  rl_bind_key('\t', rl_complete); /* plain complete, not the REPL's smart-tab */
  /* One TAB lists the matches when there's no common prefix — so TAB on an
     empty line shows every command (acts as help). */
  rl_variable_bind("show-all-if-ambiguous", "on");
  char *l = readline(g_dbg_color ? DBGC_HDR "(dbg)" DBGC_RST " " : "(dbg) ");
  rl_attempted_completion_function = prev;
  rl_bind_key('\t', alcove_smart_tab);
  rl_variable_bind("show-all-if-ambiguous", "off");
  g_dbg_complete_env = NULL;
  return l;
}
#endif /* ALCOVE_READLINE */

/* Read one debug command: readline+completion on a tty, plain getline otherwise
   (pipes, the test harness, or a no-readline build). Malloc'd line / NULL@EOF.
 */
static char *dbg_read_command(env_t *frame_env) {
#ifdef ALCOVE_READLINE
  if (isatty(fileno(stdin)))
    return dbg_readline_tty(frame_env);
#else
  (void)frame_env;
#endif
  fprintf(stderr, "%s(dbg)%s ", dc(DBGC_HDR), dc(DBGC_RST));
  fflush(stderr);
  char *buf = NULL;
  size_t cap = 0;
  ssize_t n = getline(&buf, &cap, stdin);
  if (n < 0) {
    free(buf);
    return NULL;
  }
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = 0;
  return buf;
}
