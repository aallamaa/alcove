/* repl_builtins.h — Lisp-facing REPL editing + key-binding builtins. FRAGMENT
 * #included into alcove.c (single TU). NOT standalone, NOT separately compiled.
 * `make tidy` lints it in context via alcove.c.
 */

/* ---- Lisp-facing REPL editing + key-binding builtins ----------------------
   Always compiled (lispProcList references them in every build); the readline
   machinery is real only under ALCOVE_READLINE and inert otherwise. The editing
   builtins are meaningful inside a (bind-key ...) handler, where readline is
   mid-line; elsewhere they read empty / are no-ops. */

/* (bind-key keyspec handler) — bind a terminal key to a no-arg handler thunk.
   keyspec aliases: tab S-tab home end C-<a-z> M-<char>, else raw keyseq bytes.
   A nil handler makes the key an inert no-op. Returns t on success. */
exp_t *bindkeycmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(spec, handler);
  exp_t *ret = refexp(NIL_EXP);
#ifdef ALCOVE_READLINE
  if (spec && isstring(spec)) {
    char seq[64];
    if (repl_resolve_keyseq((const char *)exp_text(spec), seq, sizeof seq)) {
      repl_bind_one(seq, handler ? handler : NIL_EXP);
      unrefexp(ret);
      ret = refexp(TRUE_EXP);
    }
  }
#else
  (void)spec;
  (void)handler;
#endif
  if (spec)
    unrefexp(spec);
  if (handler)
    unrefexp(handler);
  unrefexp(e);
  return ret;
}

/* (repl-line) — the line currently being edited, or "" outside a handler. */
exp_t *repllinecmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
#ifdef ALCOVE_READLINE
  if (rl_line_buffer)
    return make_string(rl_line_buffer, (int)strlen(rl_line_buffer));
#endif
  return make_string("", 0);
}

/* (repl-point) — cursor byte offset; (repl-end) — buffer length. */
exp_t *replpointcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
#ifdef ALCOVE_READLINE
  return make_integeri(rl_point);
#else
  return make_integeri(0);
#endif
}
exp_t *replendcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
#ifdef ALCOVE_READLINE
  return make_integeri(rl_end);
#else
  return make_integeri(0);
#endif
}

/* (repl-goto n) — move the cursor to byte offset n (clamped). */
exp_t *replgotocmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(nv);
#ifdef ALCOVE_READLINE
  if (nv && isnumber(nv)) {
    int n = (int)FIX_VAL(nv);
    if (n < 0)
      n = 0;
    if (n > rl_end)
      n = rl_end;
    rl_point = n;
  }
#endif
  if (nv)
    unrefexp(nv);
  unrefexp(e);
  return refexp(NIL_EXP);
}

/* (repl-insert s) — insert string s at the cursor. */
exp_t *replinsertcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(sv);
#ifdef ALCOVE_READLINE
  if (sv && isstring(sv) && rl_line_buffer)
    rl_insert_text((char *)exp_text(sv));
#endif
  if (sv)
    unrefexp(sv);
  unrefexp(e);
  return refexp(NIL_EXP);
}

/* (repl-delete a b) — delete the byte range [a, b). */
exp_t *repldeletecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(av, bv);
#ifdef ALCOVE_READLINE
  if (av && isnumber(av) && bv && isnumber(bv) && rl_line_buffer) {
    int a = (int)FIX_VAL(av), b = (int)FIX_VAL(bv);
    if (a < 0)
      a = 0;
    if (b > rl_end)
      b = rl_end;
    if (a < b)
      rl_delete_text(a, b);
  }
#endif
  if (av)
    unrefexp(av);
  if (bv)
    unrefexp(bv);
  unrefexp(e);
  return refexp(NIL_EXP);
}

/* (repl-replace-line s) — replace the whole buffer with s; cursor to end. */
exp_t *replreplacelinecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(sv);
#ifdef ALCOVE_READLINE
  if (sv && isstring(sv) && rl_line_buffer) {
    rl_replace_line((char *)exp_text(sv), 0);
    rl_point = rl_end;
  }
#endif
  if (sv)
    unrefexp(sv);
  unrefexp(e);
  return refexp(NIL_EXP);
}

/* (repl-refresh) — force a redisplay of the current line. */
exp_t *replrefreshcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
#ifdef ALCOVE_READLINE
  rl_forced_update_display();
#endif
  return refexp(NIL_EXP);
}

/* (repl-completions prefix) — list of names (builtins + visible vars) that
   start with prefix; lets a handler offer its own completion. */
exp_t *replcompletionscmd(exp_t *e, env_t *env) {
  (void)env;
  EVAL_ARG_1(pv);
  exp_t *lst = refexp(NIL_EXP);
#ifdef ALCOVE_READLINE
  if (pv && isstring(pv)) {
    const char *pfx = (const char *)exp_text(pv);
    size_t plen = strlen(pfx);
    char **m = NULL;
    int nm = 0, cap = 0;
    rl_collect_dict(reserved_symbol, pfx, plen, &m, &nm, &cap);
    rl_collect_env_chain(g_global_env, pfx, plen, &m, &nm, &cap);
    exp_t *head = NULL, *tail = NULL;
    for (int i = 0; i < nm; i++) {
      exp_t *node = make_node(make_string(m[i], (int)strlen(m[i])));
      if (!head)
        head = node;
      else
        tail->next = node;
      tail = node;
      free(m[i]);
    }
    free(m);
    if (head) {
      unrefexp(lst);
      lst = head;
    }
  }
#endif
  if (pv)
    unrefexp(pv);
  unrefexp(e);
  return lst;
}

/* Reset the eval-state cursors abandoned by an OOM longjmp, so the next
   top-level form starts clean. The partially-built allocation and its refs are
   leaked (the process survives — the whole point); we only zero the global
   cursors a fresh evaluation assumes. */
static void oom_recover_reset(void) {
  g_handler_sp = 0; /* abandon any open (try ...) handlers */
  g_try_depth = 0;
  in_tail_position = 0;
  g_calldepth = 0;
  bt_clear();
  g_deadline_ms =
      0; /* abandon any active with-time-limit / with-memory-limit */
  g_chunk_ceiling = 0;
  g_budget_tick = 0;
  g_resp_cb_guard = 0;
  g_in_client_cmd = 0;
  g_dbg_evaluating = 0;
  g_dbg_active = 0;
}

/* Eval one already-read top-level form with REPL semantics. `quiet` (file
   load) suppresses the Out[] print. Handles the quit/exit and toeval REPL
   words. Consumes `form`. Returns 1 iff quit/exit was seen. */
static int repl_eval_print_form(exp_t *form, env_t *env, int idx, int quiet) {
  const char *sv = issymbol(form) ? (const char *)exp_text(form) : NULL;
  if (sv && (strcmp(sv, "quit") == 0 || strcmp(sv, "exit") == 0)) {
    unrefexp(form);
    return 1;
  }
  if (sv && strcmp(sv, "toeval") == 0) {
    toeval = 1 - toeval;
    printf("%d\n", toeval);
    unrefexp(form);
    return 0;
  }
  exp_t *res = NULL;
  /* Each top-level form starts with clean backtrace-capture state. Without
     this, an error CREATED but swallowed earlier (the reader's EOF sentinel
     while loading .init.alc, an (error? ...) probe in a previous form) leaves
     a stale snapshot that blocks capture for this form's real error — the
     uncaught-error backtrace then renders empty (or worse, someone else's
     frames). The capture's meaningful scope is exactly one top-level form:
     anything live is rendered right below, before the next form runs. */
  bt_clear();
  g_err_line = 0; /* precise error position, filled by error()/RUNTIME_ERR */
  g_err_col = 0;
  /* *output-hook* input fallback: the piped/stream path has no line text
     (g_repl_input is NULL), so serialize the form NOW — before eval consumes it
     — but only when an *output-hook* is actually bound. Interactive lines
     already carry their text in g_repl_input, so this stays NULL there. */
  char *oh_in = NULL;
  if (!quiet && !g_repl_input && env && env->d) {
    keyval_t *ohkv = set_get_keyval_dict(env->d, "*output-hook*", NULL);
    if (ohkv && ohkv->val && islambda((exp_t *)ohkv->val)) {
      size_t cap = 64, len = 0;
      char *b = memalloc(cap, 1);
      exp_to_string_buf(form, &b, &len, &cap);
      exp_t *s = make_string(b, (int)len);
      free(b);
      oh_in = strdup((const char *)exp_text(s));
      unrefexp(s);
    }
  }
  if (toeval) {
    g_dbg_evaluating = 1; /* arm break-on-raise only for this top-level form */
    /* OOM recovery point: a failed allocation anywhere under this form longjmps
       back here instead of killing the process. Save/restore the prior jmp_buf
       so a nested top-level eval (e.g. embedding alcove_eval_string) is safe.
     */
    jmp_buf oom_prev;
    int oom_prev_armed = g_oom_armed;
    memcpy(&oom_prev, &g_oom_jmp, sizeof(jmp_buf));
    if (setjmp(g_oom_jmp) == 0) {
      g_oom_armed = 1;
      res = evaluate(form, env);
    } else {
      /* g_oom_armed was cleared by oom_raise() before it unwound. Reset the
         abandoned eval state, then surface a catchable out-of-memory error
         (built now that allocation can succeed again). `form` and any partial
         work leak — the process lives. */
      oom_recover_reset();
      res = error(ERROR_ILLEGAL_VALUE, NULL, env,
                  "out of memory (computation aborted)");
    }
    memcpy(&g_oom_jmp, &oom_prev, sizeof(jmp_buf));
    g_oom_armed = oom_prev_armed;
    g_dbg_evaluating = 0;
  } else
    unrefexp(form);
  /* Where to point the caret/location: the precise failing form when we have it
     (g_err_line, from the AST form or the VM pc→loc table), else the enclosing
     top-level form. */
  int eln = g_err_line ? g_err_line : g_form_line;
  int ecol = g_err_line ? g_err_col : g_form_col;
  if (!quiet) {
    if (res) {
      char *oph = repl_prompt_str(env, "*prompt-out*", idx);
      if (oph) {
        fputs(oph, stdout);
        free(oph);
      } else
        printf("\x1B[31mOut[\x1B[91m%d\x1B[31m]:\x1B[39m", idx);
      print_node(res);
      if (iserror(
              res)) { /* show the offending line + caret under the message */
        render_form_caret(stdout, eln, ecol);
        render_backtrace(stdout);
      }
    } else
      printf("nil");
    printf("\n\n");
    fflush(stdout); /* keep interactive (-R reactor / piped) output prompt */
  } else if (res && iserror(res)) {
    /* Quiet (running a FILE or -e): a top-level form errored. Surface it on
       stderr — previously this was swallowed entirely unless a file source
       label was set, so `alcove -e '(oops)'` failed SILENTLY with exit 0.
       With a source label (file mode) prefix "<src>:<line>:" — the PRECISE
       failing form's line when known, else the top-level form's. */
    if (g_reader_src)
      annotate_error_loc(res, g_reader_src, display_line(eln));
    fprintf(stderr, "%s\n", (const char *)res->ptr);
    render_form_caret(stderr, eln, ecol);
    render_backtrace(stderr);
    g_script_error =
        1; /* script (file/-e) form errored → main exits non-zero */
  }
  /* Output hook: hand (cell-number, input-text, result-value) to *output-hook*
     for capture/logging. Interactive only (fires once per evaluated form, after
     the result is shown); a broken hook can't break the REPL. */
  if (!quiet && res && env && env->d) {
    keyval_t *ohkv = set_get_keyval_dict(env->d, "*output-hook*", NULL);
    exp_t *ohf = ohkv ? (exp_t *)ohkv->val : NULL;
    if (ohf && islambda(ohf)) {
      const char *instr = g_repl_input ? g_repl_input : (oh_in ? oh_in : "");
      exp_t *argv[3] = {make_integeri(idx),
                        make_string((char *)instr, (int)strlen(instr)),
                        refexp(res)}; /* alc_apply_n consumes all three */
      exp_t *hr = alc_apply_n(ohf, 3, argv, env);
      if (hr)
        unrefexp(hr);
    }
  }
  free(oh_in);
  if (res)
    unrefexp(res);
  return 0;
}

/* Transpile a complete input chunk (adder in the adder build,
   s-expressions otherwise) and eval+print every top-level form it yields.
   Returns 1 iff quit/exit was seen. The interactive readline REPL and the
   -R combined REPL both feed it one complete unit at a time, so neither
   has to know about transpilation or form iteration. */
static int repl_eval_text_raw(const char *src, size_t n, env_t *env, int idx) {
#ifdef ALCOVE_ALS
  char *tmp = (char *)malloc(n + 1);
  if (!tmp)
    return 0;
  memcpy(tmp, src, n);
  tmp[n] = 0;
  als_map amap = {0};
  char *body = als_to_sexpr_mapped(tmp, &amap); /* + generated→Adder line map */
  if (!body) {
    free(tmp);
    als_map_free(&amap);
    return 0;
  }
  /* tmp (the Adder source) is kept until the end so the caret can render it. */
#else
  char *body = (char *)malloc(n + 1); /* already s-expressions */
  if (!body)
    return 0;
  memcpy(body, src, n);
  body[n] = 0;
#endif
  /* Run the reader over the s-expr source with a trailing newline so a
     bare final token (quit / a number) terminates instead of EOF-ing
     mid-token. */
  size_t blen = strlen(body);
  char *buf = (char *)malloc(blen + 2);
  if (!buf) {
    free(body);
    return 0;
  }
  memcpy(buf, body, blen);
  buf[blen] = '\n';
  buf[blen + 1] = 0;
  free(body);
  FILE *fs = fmemopen(buf, blen + 1, "r");
  int quit = 0;
  /* Save reader location state; the interactive caret renders against this
     input buffer. (Adder maps back to the user's surface source in Step 4; the
     plain build's buffer already IS the user's s-expr input.) */
  const char *prev_srctext = g_reader_srctext;
  size_t prev_srctext_len = g_reader_srctext_len;
  als_map *prev_adder_map = g_adder_map;
  int prev_line = g_reader_line, prev_col = g_reader_col;
  long prev_off = g_reader_off;
  g_reader_line = 1;
  g_reader_col = 1;
  g_reader_off = 0;
#ifdef ALCOVE_ALS
  g_reader_srctext = tmp; /* the user's Adder source */
  g_reader_srctext_len = n;
  g_adder_map = &amap;
#else
  g_reader_srctext = buf;
  g_reader_srctext_len = blen;
#endif
  if (fs) {
    for (;;) {
      g_form_line = g_reader_line; /* fallback if no significant byte is read */
      g_form_line_arm = 1;         /* reader() stamps the true form position */
      exp_t *form = reader(fs, 0, 0);
      if (!form)
        break;
      if (iserror(form) && form->flags == EXP_ERROR_PARSING_EOF) {
        unrefexp(form);
        break;
      }
      if (repl_eval_print_form(form, env, idx, 0)) {
        quit = 1;
        break;
      }
    }
    fclose(fs);
  }
  g_reader_srctext = prev_srctext;
  g_reader_srctext_len = prev_srctext_len;
  g_adder_map = prev_adder_map;
  g_reader_line = prev_line;
  g_reader_col = prev_col;
  g_reader_off = prev_off;
  free(buf);
#ifdef ALCOVE_ALS
  free(tmp);
  als_map_free(&amap);
#endif
  return quit;
}

/* Evaluate one REPL line, first passing it through *input-hook* (if bound): the
   hook may rewrite the text, and the rewritten text is both what gets evaluated
   AND what the *output-hook* receives as `input`. g_repl_input points at it for
   the duration so repl_eval_print_form can hand it to the output hook. */
static int repl_eval_text(const char *src, size_t n, env_t *env, int idx) {
  char *in0 = (char *)malloc(n + 1);
  if (!in0)
    return repl_eval_text_raw(src, n, env, idx); /* OOM: skip the hook */
  memcpy(in0, src, n);
  in0[n] = 0;
  char *xf = repl_hook_transform(env, "*input-hook*", in0);
  if (xf) {
    free(in0);
    in0 = xf;
  }
  const char *prev = g_repl_input;
  g_repl_input = in0;
  int rc = repl_eval_text_raw(in0, strlen(in0), env, idx);
  g_repl_input = prev;
  free(in0);
  return rc;
}

/* RESP2 server prototype lives in its own file but is included as
   part of this single TU so it can use file-static helpers
   (make_string, set_get_keyval_dict, ...) without exporting them.
   Excluded from the web build since it needs pthread/epoll/sockets. */
#ifndef ALCOVE_WEB
#include "resp.c"
#endif

#ifndef ALCOVE_WEB
/* shard_main — the future pthread entrypoint for a worker shard. For
   N=1 (today) the main thread invokes it with &main_shard, behavior
   identical to a direct resp_serve(port) call: same select() loop,
   same client list. The skeleton wires the per-shard inbox and wake
   fd through shard_runtime_init/destroy so Step 2.3 (acceptor split)
   only has to start producing — no further reactor surgery. */
int shard_main(shard_t *sh, int port) {
  current_shard = sh;
  if (shard_runtime_init(sh) < 0) {
    fprintf(stderr, "alcove: shard_runtime_init failed: %s\n", strerror(errno));
    /* Fall through anyway — resp_serve detects runtime_ready != 1
       and runs in single-thread degraded mode (no inbox). */
  }
  /* Mirror the global lfkv into the shard's TLS pointer if it has
     already been created (auto-load at startup, or a peer shard
     already wrote). resp_kv_ensure handles the lazy first-write
     creation; this just ensures reads on a fresh shard see the
     pre-loaded keyspace without waiting for a write. */
  if (!sh->kv)
    sh->kv = resp_kv_get();
  int rc = resp_serve(port);
  shard_runtime_destroy(sh);
  return rc;
}

/* ---------- multi-reactor entry (Step LF-7) ----------
   Spawns N reactor threads that all share the same global lock-free
   keyspace (g_resp_kv) and bind the same port via SO_REUSEPORT. Each
   thread gets its own shard_t (its own arena, inbox, wake fd, client
   list), registers with the epoch system, and runs an independent
   select() loop. The kernel hashes incoming connections across the
   N listening sockets; each connection thereafter is owned by a
   single reactor for its lifetime — no cross-thread fd sharing. */

typedef struct shard_thread_arg {
  shard_t *sh;
  int port;
  int rc;
} shard_thread_arg_t;

#if !ALCOVE_SINGLE_THREADED
static void *shard_thread_entry(void *p) {
  shard_thread_arg_t *a = p;
  a->rc = shard_main(a->sh, a->port);
  return NULL;
}
#endif

int respN_serve(int port, int nthreads) {
  if (nthreads <= 1)
    return shard_main(&main_shard, port);
#if ALCOVE_SINGLE_THREADED
  fprintf(stderr,
          "alcove: --threads %d ignored — this build is single-threaded "
          "(rebuild without ALCOVE_SINGLE_THREADED).\n",
          nthreads);
  return shard_main(&main_shard, port);
#else
  if (nthreads > EPOCH_MAX_THREADS) {
    fprintf(stderr, "alcove: clamping --threads %d to EPOCH_MAX_THREADS=%d\n",
            nthreads, EPOCH_MAX_THREADS);
    nthreads = EPOCH_MAX_THREADS;
  }
  /* Allocate N-1 extra shards (shard 0 is main_shard). Each gets its
     own heap arena so make_env doesn't race on the bump pointer. */
  shard_t **shards = calloc((size_t)nthreads, sizeof *shards);
  shard_thread_arg_t *args = calloc((size_t)nthreads, sizeof *args);
  pthread_t *tids = calloc((size_t)nthreads, sizeof *tids);
  if (!shards || !args || !tids) {
    fprintf(stderr, "alcove: OOM allocating reactor scaffolding\n");
    free(shards);
    free(args);
    free(tids);
    return 1;
  }
  shards[0] = &main_shard;
  for (int i = 1; i < nthreads; i++) {
    shard_t *sh = calloc(1, sizeof *sh);
    env_t *arena = calloc(ENV_ARENA_SLOTS, sizeof *arena);
    if (!sh || !arena) {
      fprintf(stderr, "alcove: OOM allocating shard %d\n", i);
      free(sh);
      free(arena);
      /* Spawning happens later in the function — no threads to join
         here, just release the shards we already allocated. (The
         older pthread_cancel call was dead code AND not portable
         to Bionic libc, which doesn't ship pthread_cancel.) */
      for (int j = 1; j < i; j++) {
        free(shards[j]->arena);
        free(shards[j]);
      }
      free(shards);
      free(args);
      free(tids);
      return 1;
    }
    sh->arena = arena;
    sh->arena_sp = arena;
    sh->arena_end = arena + ENV_ARENA_SLOTS;
    shards[i] = sh;
  }
  /* More than one reactor → arm the RESP callback read-only guard so that
     global-mutating special forms refuse rather than race on the shared
     global env. (Single-reactor / mono paths return early above, leaving
     g_resp_multi = 0 and callbacks free to mutate globals.) */
  g_resp_multi = (nthreads > 1);
  /* Set up the process-global server state (port, command table, signals,
     keyspace) ONCE here, before any reactor spawns — pthread_create then
     publishes it with a happens-before to every worker. Otherwise each
     reactor's resp_serve would race on these (TSan-confirmed). */
  resp_serve_shared_init(port);
  /* Spawn workers 1..N-1; main thread runs worker 0. */
  printf(
      ALCOVE_PROGNAME
      ": spawning %d reactor threads on port %d (--threads is EXPERIMENTAL: "
      "the lock-free keyspace is concurrency-safe, but RESP callbacks must be "
      "read-only w.r.t. Lisp globals — see docs/multithreading.md)\n",
      nthreads, port);
  fflush(stdout);
  for (int i = 1; i < nthreads; i++) {
    args[i].sh = shards[i];
    args[i].port = port;
    if (pthread_create(&tids[i], NULL, shard_thread_entry, &args[i]) != 0) {
      fprintf(stderr, "alcove: pthread_create failed for shard %d: %s\n", i,
              strerror(errno));
      /* Continue with fewer threads rather than abort — but mark
         this slot as not-launched so we don't try to join it. */
      tids[i] = 0;
    }
  }
  args[0].sh = &main_shard;
  args[0].port = port;
  args[0].rc = shard_main(&main_shard, port);
  /* Once main returns (SIGINT was observed), wait for peers to drain. */
  for (int i = 1; i < nthreads; i++) {
    if (tids[i])
      pthread_join(tids[i], NULL);
  }
  /* All reactors have exited → run the process-global teardown ONCE, here,
     where it's single-threaded (each reactor skipped it under g_resp_multi to
     avoid racing the shared port / keyspace / epoch state). */
  resp_serve_shared_teardown();
  for (int i = 1; i < nthreads; i++) {
    free(shards[i]->arena);
    free(shards[i]);
  }
  int rc = args[0].rc;
  free(shards);
  free(args);
  free(tids);
  return rc;
#endif /* !ALCOVE_SINGLE_THREADED */
}
#endif /* !ALCOVE_WEB */

/* Read every top-level form from `path` and evaluate it in `global`.
   Returns 1 if the file existed (and we processed it), 0 if missing.
   Errors mid-file are printed but do not abort the load — same loose
   policy as the -e flag and the interactive REPL. */
static int alcove_run_init_file(env_t *global, const char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    return 0;
  /* Dialect by extension: a .adr init file is Adder surface syntax — slurp and
     transpile to s-expressions (als_to_sexpr is compiled into both binaries),
     then read that. .alc (and anything else) is read directly as s-expressions,
     so an existing .init.alc still loads under adder via the fallback. */
  char *transpiled = NULL;
  size_t plen = strlen(path);
  if (plen >= 4 && strcmp(path + plen - 4, ".adr") == 0) {
    size_t len = 0;
    char *text = slurp_stream(fp, &len);
    fclose(fp);
    if (!text)
      return 0;
    transpiled = als_to_sexpr(text);
    free(text);
    if (!transpiled)
      return 0;
    fp = fmemopen(transpiled, strlen(transpiled), "r");
    if (!fp) {
      free(transpiled);
      return 0;
    }
  }
  for (;;) {
    exp_t *e = reader(fp, 0, 0);
    if (!e)
      break;
    if (iserror(e) && e->flags == EXP_ERROR_PARSING_EOF) {
      unrefexp(e);
      break;
    }
    if (iserror(e)) {
      print_node(e);
      printf("\n");
      unrefexp(e);
      continue;
    }
    exp_t *r = evaluate(e, global);
    if (r) {
      if (iserror(r)) {
        print_node(r);
        printf("\n");
      }
      unrefexp(r);
    }
  }
  fclose(fp);
  free(transpiled);
  return 1;
}

/* Load the startup init file: dialect-native first (adder -> .init.adr,
   alcove -> .init.alc), then the OTHER dialect's file as a fallback (the loader
   transpiles .adr in either binary, so an .alc program can still pick up an
   existing .init.alc and vice versa — no forced migration). Project-local
   (./) takes priority over the user-global $HOME/.local/alcove/. Stops at the
   first match — never runs more than one. Silent on miss; one-line announce. */
static void alcove_try_init_files(env_t *global) {
#ifdef ALCOVE_ALS
  static const char *const locals[] = {"./.init.adr", "./.init.alc"};
  static const char *const bases[] = {"init.adr", "init.alc"};
#else
  static const char *const locals[] = {"./.init.alc", "./.init.adr"};
  static const char *const bases[] = {"init.alc", "init.adr"};
#endif
  for (int i = 0; i < 2; i++)
    if (alcove_run_init_file(global, locals[i])) {
      printf("%s: loaded %s\n", ALCOVE_PROGNAME, locals[i]);
      return;
    }
  const char *home = getenv("HOME");
  if (!home)
    return;
  char path[1024];
  for (int i = 0; i < 2; i++) {
    int n = snprintf(path, sizeof path, "%s/.local/alcove/%s", home, bases[i]);
    if (n < 0 || (size_t)n >= (int)sizeof path)
      continue;
    if (alcove_run_init_file(global, path)) {
      printf("%s: loaded %s\n", ALCOVE_PROGNAME, path);
      return;
    }
  }
}

/* alcove_init — bring the engine up: the per-type (de)serializers, the immortal
   singletons (nil / t / *done*), and every builtin from lispProcList registered
   into reserved_symbol. Returns the fresh global environment (also published as
   g_global_env). Call exactly once before
   reader()/evaluate()/alcove_eval_string(). Shared by main() and by C embedders
   — a host does: #define ALCOVE_NO_MAIN #include "alcove.c" env_t *g =
   alcove_init(); exp_t *r = alcove_eval_string("(+ 1 2)");  // owned; unrefexp
   when done See examples/embed/ for a worked example. */
env_t *alcove_init(void) {
  /* Idempotent: a second call would leak the first env + reserved_symbol and
     silently drop any builtins registered (via alcove_register_cmd) in between.
     Return the existing engine instead. */
  if (g_global_env)
    return g_global_env;
  env_t *global = make_env(NULL);
  /* Publish the global env early so introspection builtins (source,
     completion, etc.) can compare against it across all entry paths. */
  g_global_env = global;
  exp_t *t, *nil, *val;
  exp_tfuncList[EXP_CHAR] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_CHAR]->load = load_char;
  exp_tfuncList[EXP_CHAR]->dump = dump_char;
  exp_tfuncList[EXP_STRING] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_STRING]->load = load_string;
  exp_tfuncList[EXP_STRING]->dump = dump_string;
  /* Phase 1 persistence: scalars + symbols. */
  exp_tfuncList[EXP_NUMBER] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_NUMBER]->load = load_number;
  exp_tfuncList[EXP_NUMBER]->dump = dump_number;
  exp_tfuncList[EXP_FLOAT] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_FLOAT]->load = load_float;
  exp_tfuncList[EXP_FLOAT]->dump = dump_float;
  exp_tfuncList[EXP_RATIONAL] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_RATIONAL]->load = load_rational;
  exp_tfuncList[EXP_RATIONAL]->dump = dump_rational;
  exp_tfuncList[EXP_DECIMAL] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_DECIMAL]->load = load_decimal;
  exp_tfuncList[EXP_DECIMAL]->dump = dump_decimal;
  exp_tfuncList[EXP_SYMBOL] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_SYMBOL]->load = load_symbol;
  exp_tfuncList[EXP_SYMBOL]->dump = dump_symbol;
  /* Phase 2 persistence: lists + lambdas (source-form). */
  exp_tfuncList[EXP_PAIR] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_PAIR]->load = load_pair;
  exp_tfuncList[EXP_PAIR]->dump = dump_pair;
  exp_tfuncList[EXP_LAMBDA] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_LAMBDA]->load = load_lambda;
  exp_tfuncList[EXP_LAMBDA]->dump = dump_lambda;
  exp_tfuncList[EXP_MACRO] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_MACRO]->load = load_macro;
  exp_tfuncList[EXP_MACRO]->dump = dump_macro;
  /* EXP_BLOB — RESP string values are binary-safe blobs. Registering
     dump/load here lets the unified savedb format persist the RESP
     keyspace (alongside the existing Lisp env section). */
  exp_tfuncList[EXP_BLOB] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_BLOB]->load = load_blob;
  exp_tfuncList[EXP_BLOB]->dump = dump_blob;

  /* EXP_VECTOR — length-prefixed recursive dump; heterogeneous element
     types round-trip through __DUMP__/__LOAD__. Needed for ML / data
     workloads where the natural representation is a numeric vec. */
  exp_tfuncList[EXP_VECTOR] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_VECTOR]->load = load_vec;
  exp_tfuncList[EXP_VECTOR]->dump = dump_vec;
  exp_tfuncList[EXP_SET] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_SET]->load = load_set;
  exp_tfuncList[EXP_SET]->dump = dump_set;
  /* EXP_DICT (hash-map) and EXP_LIST (deque) — round-trip through savedb
     so persisted dicts/deques (and vecs/sets containing them) survive. */
  exp_tfuncList[EXP_DICT] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_DICT]->load = load_dict_value;
  exp_tfuncList[EXP_DICT]->dump = dump_dict_value;
  exp_tfuncList[EXP_LIST] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_LIST]->load = load_deque_value;
  exp_tfuncList[EXP_LIST]->dump = dump_deque_value;
  exp_tfuncList[EXP_HAMT] = (exp_tfunc *)memalloc(1, sizeof(exp_tfunc));
  exp_tfuncList[EXP_HAMT]->load = load_hamt_value;
  exp_tfuncList[EXP_HAMT]->dump = dump_hamt_value;

  reserved_symbol = create_dict();
  /* Allocate immortal singletons before any other code references them. */
  nil_singleton = make_nil();
  true_singleton = make_symbol("t", 1);
  gen_done_singleton = make_symbol("*done*", 6);
  set_get_keyval_dict(reserved_symbol, "nil", nil = NIL_EXP);
  set_get_keyval_dict(reserved_symbol, "t", t = TRUE_EXP);

  int N = sizeof(lispProcList) / sizeof(lispProc);
  int i;
  for (i = 0; i < N; ++i) {
    set_get_keyval_dict(
        reserved_symbol, lispProcList[i].name,
        val = make_internal(
            lispProcList[i].cmd,
            lispProcList[i].flags &
                (FLAG_TAIL_AWARE | FLAG_APPLICATIVE | FLAG_UNSAFE)));
    unrefexp(val);
  }
  (void)t;
  (void)nil;
  /* *args*: the script's command-line arguments (a list of strings). Bound
     nil here so it is ALWAYS defined — REPL, -e without args, embedded, web;
     main() rebinds it after argv parsing when a script receives arguments. */
  if (!global->d)
    global->d = create_dict();
  set_get_keyval_dict(global->d, "*args*", NIL_EXP);
  /* *readline* — t when this build links the readline line-editor (bind-key and
     the repl-* line-editing builtins are live), nil otherwise (e.g. the static
     release binary). Lets programs and the test suite skip interactive-only
     features rather than misbehave. */
#ifdef ALCOVE_READLINE
  set_get_keyval_dict(global->d, "*readline*", TRUE_EXP);
#else
  set_get_keyval_dict(global->d, "*readline*", NIL_EXP);
#endif
  /* REPL prompt hooks: when bound to a function (fn (n) -> string) the REPL
     calls it to render a prompt for cell n; nil = built-in default. *-in* is
     the input prompt, *-out* precedes a result, *-cont* is the multi-line
     continuation prompt (the "    ... " before continued lines). Set them in
     .init.alc to customize. See repl_prompt_str(). */
  set_get_keyval_dict(global->d, "*prompt-in*", NIL_EXP);
  set_get_keyval_dict(global->d, "*prompt-out*", NIL_EXP);
  set_get_keyval_dict(global->d, "*prompt-cont*", NIL_EXP);
  /* REPL eval hooks: *input-hook* (fn (input) -> string) rewrites a typed line
     before it is evaluated; *output-hook* (fn (n input output) ...) observes
     the (post-transform) input and the result value for capture/logging. nil =
     off. See repl_hook_transform() and the *output-hook* call in
     repl_eval_print_form. */
  set_get_keyval_dict(global->d, "*input-hook*", NIL_EXP);
  set_get_keyval_dict(global->d, "*output-hook*", NIL_EXP);
  /* Make the prompt hooks discoverable via (doc *prompt-in*) and friends. They
     are plain globals (not builtins), so their help lives in user_doc rather
     than lispProcList — registering them as builtins would shadow the variable
     in normal lookup. */
  {
    static const struct {
      const char *name, *doc;
    } pdocs[] = {
        {"*prompt-in*",
         "*prompt-in* — REPL input-prompt hook. Bind to a function (fn (n) -> "
         "string); the REPL calls it with the cell number n to render the In "
         "prompt. Return \"\" to suppress it; nil/unset, a non-function, an "
         "error, or a non-string falls back to the built-in In [n]: default."},
        {"*prompt-out*",
         "*prompt-out* — REPL result-prompt hook. Like *prompt-in* but renders "
         "the prefix printed before a result (default Out[n]:)."},
        {"*prompt-cont*",
         "*prompt-cont* — REPL continuation-prompt hook. Like *prompt-in* but "
         "renders the multi-line continuation prompt (default \"    ... \"), "
         "including the wrapped rows of a pasted or recalled multi-line form."},
        {"*input-hook*",
         "*input-hook* — REPL input transform. Bind to (fn (input) -> string); "
         "called with the typed line text and its result replaces what gets "
         "evaluated (and what *output-hook* sees as input). nil/non-string "
         "falls "
         "back to the original. Line-scoped (not the piped stream path)."},
        {"*output-hook*",
         "*output-hook* — REPL capture hook. Bind to (fn (n input output) "
         "...); "
         "called after each interactive eval with the cell number, the "
         "(post-transform) input string, and the result value (errors passed "
         "as "
         "the error value). Return ignored — for transcripts/logging."},
    };
    if (!user_doc)
      user_doc = create_dict();
    for (int i = 0; i < (int)(sizeof pdocs / sizeof pdocs[0]); i++)
      set_get_keyval_dict(
          user_doc, (char *)pdocs[i].name,
          make_string((char *)pdocs[i].doc, (int)strlen(pdocs[i].doc)));
  }
  return global;
}

/* alcove_eval_string — read and evaluate every form in `src` (alcove
   s-expressions), returning the LAST form's value as an OWNED reference (the
   caller must unrefexp it when done), an error exp_t if a form fails to parse
   or evaluate (test with iserror), or NIL_EXP for empty input. The engine must
   already be up (alcove_init). Tagged immediates (fixnums, chars, nil, t) need
   no unref; heap values do — unrefexp the result once, always. */
exp_t *alcove_eval_string(const char *src) {
  if (!g_global_env)
    return error(ERROR_ILLEGAL_VALUE, NULL, NULL,
                 "alcove_eval_string: engine not initialized (call alcove_init "
                 "first)");
  if (!src)
    return NIL_EXP; /* nothing to evaluate */
  FILE *stream = fmemopen((void *)src, strlen(src), "r");
  if (!stream)
    return error(ERROR_ILLEGAL_VALUE, NULL, g_global_env,
                 "alcove_eval_string: fmemopen failed");
  exp_t *last = NIL_EXP;
  for (;;) {
    exp_t *form = reader(stream, 0, 0);
    if (iserror(form)) {
      if (form->flags == EXP_ERROR_PARSING_EOF) {
        unrefexp(form);
        break;
      }
      unrefexp(last);
      fclose(stream);
      return form; /* parse error — caller checks iserror */
    }
    bt_clear(); /* per-top-level-form capture scope (see repl_eval_print_form)
                 */
    /* OOM recovery point (see repl_eval_print_form): a failed allocation under
       this form longjmps back here, yielding a catchable error to the embedder
       instead of killing the host. Save/restore the prior jmp_buf for nesting.
     */
    exp_t *volatile r; /* volatile: assigned across setjmp/longjmp (-Wclobbered)
                        */
    jmp_buf oom_prev;
    int oom_prev_armed = g_oom_armed;
    memcpy(&oom_prev, &g_oom_jmp, sizeof(jmp_buf));
    if (setjmp(g_oom_jmp) == 0) {
      g_oom_armed = 1;
      r = evaluate(form, g_global_env);
    } else {
      oom_recover_reset();
      r = error(ERROR_ILLEGAL_VALUE, NULL, g_global_env,
                "out of memory (computation aborted)");
    }
    memcpy(&g_oom_jmp, &oom_prev, sizeof(jmp_buf));
    g_oom_armed = oom_prev_armed;
    if (r && iserror(r)) {
      unrefexp(last);
      fclose(stream);
      return r; /* evaluation error */
    }
    unrefexp(last);
    last = r ? r : NIL_EXP;
  }
  fclose(stream);
  return last;
}

const char doc_checksyntax[] =
    "(check-syntax src) — parse (NOT evaluate) every form in the string. "
    "Returns nil when the whole text parses, else (line message) for the "
    "first syntax error (1-based line; in the adder binary the line refers "
    "to the Adder source). The LSP's diagnostics primitive.";
exp_t *checksyntaxcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(srcexp);
  REQUIRE_TYPE(srcexp, isstring, CLEAN_RETURN_1(srcexp, _alc_e),
               ERROR_ILLEGAL_VALUE, NULL, env,
               "check-syntax: src must be a string");
  const char *src = (const char *)exp_text(srcexp);
#ifdef ALCOVE_ALS
  /* the adder binary checks Adder text: transpile with a line map so the
     reported line points into the user's source, not the generated sexprs */
  als_map lmap;
  memset(&lmap, 0, sizeof lmap);
  char *gen = als_to_sexpr_mapped(src, &lmap);
  FILE *stream = gen ? fmemopen(gen, strlen(gen), "r") : NULL;
#else
  FILE *stream = fmemopen((void *)src, strlen(src), "r");
#endif
  if (!stream) {
#ifdef ALCOVE_ALS
    free(gen);
    als_map_free(&lmap);
#endif
    CLEAN_RETURN_1(srcexp, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                 "check-syntax: empty or unreadable input"));
  }
  /* the reader's position state belongs to the OUTER stream being evaluated
     (a script, the REPL) — save, run on fresh state, restore. */
  int prev_line = g_reader_line, prev_col = g_reader_col,
      prev_off = g_reader_off, prev_arm = g_form_line_arm;
  const char *prev_src = g_reader_src;
  g_reader_line = 1;
  g_reader_col = 1;
  g_reader_off = 0;
  g_reader_src = NULL;
  exp_t *ret = refexp(NIL_EXP);
  while (1) {
    exp_t *form = reader(stream, 0, 0);
    if (!form)
      break;
    if (iserror(form)) {
      if (form->flags == EXP_ERROR_PARSING_EOF) {
        unrefexp(form);
        break;
      }
      int line = g_reader_line;
#ifdef ALCOVE_ALS
      {
        int a = als_map_lookup(&lmap, line);
        if (a > 0)
          line = a;
      }
#endif
      const char *msg = form->ptr ? (const char *)form->ptr : "syntax error";
      exp_t *n1 = make_node(MAKE_FIX(line));
      exp_t *n2 = make_node(make_string((char *)msg, (int)strlen(msg)));
      n1->next = n2;
      unrefexp(form);
      unrefexp(ret);
      ret = n1;
      break;
    }
    unrefexp(form);
  }
  fclose(stream);
#ifdef ALCOVE_ALS
  free(gen);
  als_map_free(&lmap);
#endif
  g_reader_line = prev_line;
  g_reader_col = prev_col;
  g_reader_off = prev_off;
  g_form_line_arm = prev_arm;
  g_reader_src = prev_src;
  CLEAN_RETURN_1(srcexp, ret);
}

/* ---- runtime s-expression reading: read-string-sexpr / read-all-string ----
   The enabling primitives for the homoiconic loop: parse TEXT into s-expr
   VALUES at runtime, so code generated at runtime (e.g. via adder->sexpr, or by
   an LLM) can be handed to eval. Both run the shared reader() over an
   fmemopen'd copy of the string. A syntax error (including an incomplete form)
   is RETURNED as a first-class EXP_ERROR value — caught with try / inspected
   with error? — never raised to top level, so the stderr-spew gate is
   unaffected. They are plain applicative builtins (LISPCMD_APP): pure, no host
   escape, and NOT special forms, so the AST/VM/JIT tiers need no teaching. */

/* Saved snapshot of the reader's global position state, so reading a string
   here never corrupts the outer stream (script / REPL / load) the evaluator
   drives. `buf` is the fmemopen backing buffer (a copy of src + trailing '\n'),
   freed on close. */
typedef struct {
  int line, col, arm;
  long off;
  const char *src;
  char *buf;
} reader_pos_t;

/* Open an fmemopen stream over a copy of `src` (with a trailing newline so a
   bare final token terminates instead of EOF-ing mid-token — same trick as the
   REPL) and reset the reader position to a clean line-1 start, saving the
   previous state into *saved. NULL on alloc/fmemopen failure (reader state
   untouched). */
static FILE *read_sexpr_open(const char *src, reader_pos_t *saved) {
  size_t n = strlen(src);
  char *buf = (char *)malloc(n + 2);
  if (!buf)
    return NULL;
  memcpy(buf, src, n);
  buf[n] = '\n';
  buf[n + 1] = '\0';
  FILE *stream = fmemopen(buf, n + 1, "r");
  if (!stream) {
    free(buf);
    return NULL;
  }
  saved->buf = buf;
  saved->line = g_reader_line;
  saved->col = g_reader_col;
  saved->off = g_reader_off;
  saved->arm = g_form_line_arm;
  saved->src = g_reader_src;
  g_reader_line = 1;
  g_reader_col = 1;
  g_reader_off = 0;
  g_reader_src = NULL;
  return stream;
}

/* Close the stream, free the backing buffer, and restore the reader position
   saved by read_sexpr_open. */
static void read_sexpr_close(FILE *stream, const reader_pos_t *saved) {
  fclose(stream);
  free(saved->buf);
  g_reader_line = saved->line;
  g_reader_col = saved->col;
  g_reader_off = saved->off;
  g_form_line_arm = saved->arm;
  g_reader_src = saved->src;
}

const char doc_read_string_sexpr[] =
    "(read-string-sexpr s) — parse the FIRST s-expression in string s and "
    "return "
    "it as a value (unevaluated). A syntax error (incl. an incomplete form) is "
    "returned as an error value; empty input is an error. Compose with eval to "
    "run generated code: (eval (read-string-sexpr \"(+ 1 2)\")).";
exp_t *readstringsexprcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(srcexp);
  REQUIRE_TYPE(srcexp, isstring, CLEAN_RETURN_1(srcexp, _alc_e),
               ERROR_ILLEGAL_VALUE, NULL, env,
               "read-string-sexpr: argument must be a string");
  reader_pos_t saved;
  FILE *stream = read_sexpr_open((const char *)exp_text(srcexp), &saved);
  if (!stream)
    CLEAN_RETURN_1(srcexp, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                 "read-string-sexpr: unreadable input"));
  exp_t *form = reader(stream, 0, 0);
  read_sexpr_close(stream, &saved);
  /* NULL = no form at all (empty/whitespace input); a parse error (incl.
     EXP_ERROR_PARSING_EOF for an unterminated form) is already an EXP_ERROR
     value — forward it unchanged for try / error?. */
  if (!form)
    form =
        error(ERROR_ILLEGAL_VALUE, NULL, env, "read-string-sexpr: empty input");
  CLEAN_RETURN_1(srcexp, form);
}

const char doc_read_all_string[] =
    "(read-all-string s) — parse EVERY s-expression in string s and return "
    "them "
    "as a list (each element unevaluated). Empty input yields nil. A syntax "
    "error (incl. an incomplete trailing form) is returned as an error value. "
    "Compose with eval/each to run a whole program generated at runtime.";
exp_t *readallstringcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(srcexp);
  REQUIRE_TYPE(srcexp, isstring, CLEAN_RETURN_1(srcexp, _alc_e),
               ERROR_ILLEGAL_VALUE, NULL, env,
               "read-all-string: argument must be a string");
  reader_pos_t saved;
  FILE *stream = read_sexpr_open((const char *)exp_text(srcexp), &saved);
  if (!stream)
    CLEAN_RETURN_1(srcexp, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                 "read-all-string: unreadable input"));
  exp_t *head = NULL, *tail = NULL, *errval = NULL;
  for (;;) {
    exp_t *form = reader(stream, 0, 0);
    if (!form)
      break; /* clean EOF — all forms consumed */
    if (iserror(form)) {
      /* EXP_ERROR_PARSING_EOF is the reader's end-of-stream terminator (and an
         incomplete trailing form lands here too) — treat as clean end, matching
         the canonical file-eval loop. Any other error is a real syntax error.
       */
      if (form->flags == EXP_ERROR_PARSING_EOF) {
        unrefexp(form);
        break;
      }
      errval = form; /* genuine syntax error (already refcounted) */
      break;
    }
    exp_t *node = make_node(form); /* adopts form's ref (no extra unref) */
    if (tail) {
      tail->next = node;
      tail = node;
    } else
      head = tail = node;
  }
  read_sexpr_close(stream, &saved);
  if (errval) {
    if (head)
      unrefexp(head);
    CLEAN_RETURN_1(srcexp, errval);
  }
  CLEAN_RETURN_1(srcexp, head ? head : refexp(NIL_EXP));
}

const char doc_adder_to_sexpr[] =
    "(adder->sexpr s) — transpile Adder surface source string s to Alcove "
    "s-expression TEXT (a string). Compose with read-string-sexpr + eval to "
    "run "
    "Adder generated at runtime: (eval (read-string-sexpr (adder->sexpr s))). "
    "Malformed Adder yields s-expr text that read-string-sexpr then reports as "
    "a "
    "syntax error.";
exp_t *addertosexprcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(srcexp);
  REQUIRE_TYPE(srcexp, isstring, CLEAN_RETURN_1(srcexp, _alc_e),
               ERROR_ILLEGAL_VALUE, NULL, env,
               "adder->sexpr: argument must be a string");
  char *sx = als_to_sexpr((const char *)exp_text(srcexp));
  if (!sx)
    CLEAN_RETURN_1(srcexp, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                 "adder->sexpr: transpile failed"));
  CLEAN_RETURN_1(srcexp, make_string_take(sx, (int)strlen(sx)));
}
