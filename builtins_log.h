/* builtins_log.h — Tier 3 observability: structured error codes, leveled
 * logfmt logging, and a small metrics registry. FRAGMENT #included into
 * alcove.c (after builtins_stdlib.h, so exp_to_string_buf / make_* / EVAL /
 * CLEAN_RETURN_* / iserror / gettimeusec / ALCOVE_TLS are all in scope). resp.c
 * is #included LATER, so it can call the metric helpers here for the RESP
 * auto-instrumentation. Plain-stderr logging is sandbox-safe (no FLAG_UNSAFE).
 */

/* ===== A. structured error codes — (error-code e) ========================= */

/* Map an error's stored errnum (exp_t.flags, the exp_error_t enum in alcove.h)
 * to a STABLE, prose-independent symbol so errors are machine-dispatchable. */
static const char *error_code_name(int errnum) {
  switch (errnum) {
  case ERROR_ILLEGAL_VALUE:        return "illegal-value";
  case ERROR_DIV_BY0:              return "div-by-zero";
  case ERROR_MISSING_PARAMETER:    return "missing-parameter";
  case ERROR_UNBOUND_VARIABLE:     return "unbound-variable";
  case ERROR_NUMBER_EXPECTED:      return "number-expected";
  case ERROR_INDEX_OUT_OF_RANGE:   return "index-out-of-range";
  case EXP_ERROR_MISSING_NAME:     return "missing-name";
  case EXP_ERROR_BODY_NOT_LIST:    return "body-not-list";
  case EXP_ERROR_PARAM_NOT_LIST:   return "param-not-list";
  case EXP_ERROR_INVALID_KEY_UPDATE: return "invalid-key-update";
  case EXP_ERROR_PARSING_MACROCHAR:
  case EXP_ERROR_PARSING_ILLEGAL_CHAR:
  case EXP_ERROR_PARSING_EOF:
  case EXP_ERROR_PARSING_ESCAPE:   return "parse-error";
  default:                         return "error";
  }
}

const char doc_error_code[] =
    "(error-code e) — the error's machine-readable CLASS as a stable symbol "
    "('div-by-zero, 'illegal-value, 'unbound-variable, 'missing-parameter, "
    "'index-out-of-range, 'number-expected, 'parse-error, …), or nil if e is "
    "not an error. Unlike (error-message) (prose), the code is stable across "
    "message wording, so handlers can dispatch: (if (is (error-code e) "
    "'div-by-zero) …). Does NOT re-raise e.";
exp_t *errorcodecmd(exp_t *e, env_t *env) {
  exp_t *a = NULL, *ret = NIL_EXP;
  if (e->next) {
    a = EVAL(e->next->content, env); /* non-propagating: inspect, don't re-raise */
    if (a && iserror(a)) {
      const char *nm = error_code_name((int)a->flags);
      ret = make_symbol((char *)nm, (int)strlen(nm));
    }
  }
  if (a)
    unrefexp(a);
  unrefexp(e);
  return ret;
}

/* ===== shared: render a value to a growable C buffer ====================== */

/* Append raw bytes to a growable buffer (the exp_to_string_buf convention). */
static void lb_add(char **b, size_t *n, size_t *cap, const char *s, size_t sl) {
  if (*n + sl + 1 > *cap) {
    size_t nc = *cap ? *cap * 2 : 64;
    while (nc < *n + sl + 1)
      nc *= 2;
    *b = xrealloc(*b, nc);
    *cap = nc;
  }
  memcpy(*b + *n, s, sl);
  *n += sl;
  (*b)[*n] = '\0';
}

/* ===== B. leveled logfmt logging ========================================== */

enum { LOG_DEBUG = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_ERROR = 3 };
static const char *const g_log_level_names[] = {"debug", "info", "warn", "error"};
/* Threshold: messages below it are dropped. Atomic — set/read across reactors. */
static _Atomic int g_log_level = LOG_INFO;

/* Parse a :keyword / symbol level name to 0-3, or -1 if unknown. */
static int log_level_of(exp_t *v) {
  if (!v || !is_ptr(v) || !issymbol(v))
    return -1;
  const char *s = exp_text(v);
  if (!s)
    return -1;
  if (*s == ':')
    s++; /* keyword form :warn */
  for (int i = 0; i < 4; i++)
    if (strcmp(s, g_log_level_names[i]) == 0)
      return i;
  return -1;
}

/* logfmt: append `value`, quoting it iff it contains space/'='/'"' or is empty
 * (escaping embedded quotes). The value is first rendered via the normal
 * value→string path so numbers/symbols/strings/lists all stringify sensibly. */
static void logfmt_value(char **b, size_t *n, size_t *cap, exp_t *v) {
  char *tmp = NULL;
  size_t tn = 0, tc = 0;
  if (isstring(v)) { /* a string's text directly (no surrounding quotes) */
    const char *t = exp_text(v);
    lb_add(&tmp, &tn, &tc, t ? t : "", t ? strlen(t) : 0);
  } else {
    tc = 64; /* exp_to_string_buf/str_buf_put need a non-zero starting cap */
    tmp = memalloc(tc, 1);
    exp_to_string_buf(v, &tmp, &tn, &tc);
  }
  int needq = (tn == 0);
  for (size_t i = 0; i < tn && !needq; i++)
    if (tmp[i] == ' ' || tmp[i] == '=' || tmp[i] == '"' || tmp[i] == '\n')
      needq = 1;
  if (!needq) {
    lb_add(b, n, cap, tmp ? tmp : "", tn);
  } else {
    lb_add(b, n, cap, "\"", 1);
    for (size_t i = 0; i < tn; i++) {
      if (tmp[i] == '"' || tmp[i] == '\\')
        lb_add(b, n, cap, "\\", 1);
      lb_add(b, n, cap, &tmp[i], 1);
    }
    lb_add(b, n, cap, "\"", 1);
  }
  free(tmp);
}

/* Append a key (a :keyword/symbol/string rendered bare, ':' stripped). */
static void logfmt_key(char **b, size_t *n, size_t *cap, exp_t *k) {
  if (is_ptr(k) && issymbol(k)) {
    const char *s = exp_text(k);
    if (s && *s == ':')
      s++;
    lb_add(b, n, cap, s ? s : "k", s ? strlen(s) : 1);
  } else if (isstring(k)) {
    const char *s = exp_text(k);
    lb_add(b, n, cap, s ? s : "k", s ? strlen(s) : 1);
  } else {
    size_t tn = 0, tc = 64;
    char *tmp = memalloc(tc, 1);
    exp_to_string_buf(k, &tmp, &tn, &tc);
    lb_add(b, n, cap, tmp ? tmp : "k", tn);
    free(tmp);
  }
}

/* Core: emit one logfmt line for `level` from MSG + alternating kv args
 * (msgnode = the MSG node; msgnode->next... = kvs). Evaluates each arg. On a
 * below-threshold level: no I/O, returns nil. Otherwise writes one line to
 * stderr (single fwrite) and returns the line as a string. Consumes nothing;
 * caller owns the returned value. Returns an error exp_t if an arg errors. */
static exp_t *log_emit(int level, exp_t *msgnode, env_t *env) {
  if (level < atomic_load_explicit(&g_log_level, memory_order_relaxed))
    return NIL_EXP; /* suppressed — no allocation, no I/O */

  char *b = NULL; size_t n = 0, cap = 0;
  /* ts=<ISO8601 UTC, ms> */
  int64_t us = gettimeusec();
  time_t sec = (time_t)(us / 1000000);
  int ms = (int)((us / 1000) % 1000);
  struct tm tmv;
  char ts[40];
  gmtime_r(&sec, &tmv);
  size_t k = strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tmv);
  k += (size_t)snprintf(ts + k, sizeof ts - k, ".%03dZ", ms);
  lb_add(&b, &n, &cap, "ts=", 3);
  lb_add(&b, &n, &cap, ts, k);
  lb_add(&b, &n, &cap, " level=", 7);
  lb_add(&b, &n, &cap, g_log_level_names[level], strlen(g_log_level_names[level]));

  exp_t *cur = msgnode;
  if (cur) { /* MSG */
    exp_t *m = EVAL(cur->content, env);
    if (m && iserror(m)) { free(b); return m; }
    lb_add(&b, &n, &cap, " msg=", 5);
    logfmt_value(&b, &n, &cap, m ? m : NIL_EXP);
    if (m) unrefexp(m);
    cur = cur->next;
  }
  /* alternating key value … */
  while (cur) {
    exp_t *kx = EVAL(cur->content, env);
    if (kx && iserror(kx)) { free(b); return kx; }
    exp_t *vx = NIL_EXP;
    int have_v = 0;
    if (cur->next) {
      vx = EVAL(cur->next->content, env);
      if (vx && iserror(vx)) { if (kx) unrefexp(kx); free(b); return vx; }
      have_v = 1;
    }
    lb_add(&b, &n, &cap, " ", 1);
    logfmt_key(&b, &n, &cap, kx ? kx : NIL_EXP);
    lb_add(&b, &n, &cap, "=", 1);
    logfmt_value(&b, &n, &cap, have_v ? vx : NIL_EXP);
    if (kx) unrefexp(kx);
    if (have_v && vx) unrefexp(vx);
    cur = cur->next ? cur->next->next : NULL;
  }
  lb_add(&b, &n, &cap, "\n", 1);
  fwrite(b, 1, n, stderr); /* one write per line: lines don't interleave */
  fflush(stderr);
  exp_t *ret = make_string(b, (int)(n ? n - 1 : 0)); /* return without the \n */
  free(b);
  return ret;
}

const char doc_log_emit[] =
    "(log! LEVEL MSG key val …) — emit a structured logfmt line to stderr when "
    "LEVEL (:debug/:info/:warn/:error) is at or above (log-level): "
    "`ts=<ISO8601> level=<lvl> msg=<MSG> key=val …`. Below the threshold it does "
    "nothing and returns nil; otherwise returns the emitted line. Values with "
    "spaces/=/\" are quoted. See (log-info), (set-log-level). (Named log! — log "
    "is the natural logarithm.)";
exp_t *logemitcmd(exp_t *e, env_t *env) {
  exp_t *lv = e->next ? EVAL(e->next->content, env) : NULL;
  if (lv && iserror(lv)) { unrefexp(e); return lv; }
  int level = log_level_of(lv);
  if (lv) unrefexp(lv);
  if (level < 0)
    CLEAN_RETURN_1(NIL_EXP,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "log: LEVEL must be :debug/:info/:warn/:error"));
  exp_t *ret = log_emit(level, e->next ? e->next->next : NULL, env);
  unrefexp(e);
  return ret;
}

/* (log-LEVEL MSG kvs…) convenience wrappers. */
#define LOG_WRAP(cmd, lvl)                                                     \
  exp_t *cmd(exp_t *e, env_t *env) {                                           \
    exp_t *ret = log_emit((lvl), e->next, env);                               \
    unrefexp(e);                                                               \
    return ret;                                                                \
  }
const char doc_log_debug[] = "(log-debug MSG kv…) — (log :debug …).";
LOG_WRAP(logdebugcmd, LOG_DEBUG)
const char doc_log_info[] = "(log-info MSG kv…) — (log :info …).";
LOG_WRAP(loginfocmd, LOG_INFO)
const char doc_log_warn[] = "(log-warn MSG kv…) — (log :warn …).";
LOG_WRAP(logwarncmd, LOG_WARN)
const char doc_log_error[] = "(log-error MSG kv…) — (log :error …).";
LOG_WRAP(logerrorcmd, LOG_ERROR)

const char doc_log_level[] =
    "(log-level) — the current minimum log level as a keyword (:info default).";
exp_t *loglevelcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  int lv = atomic_load_explicit(&g_log_level, memory_order_relaxed);
  char kw[16];
  int kn = snprintf(kw, sizeof kw, ":%s", g_log_level_names[lv]);
  return make_symbol(kw, kn);
}

const char doc_set_log_level[] =
    "(set-log-level LEVEL) — set the minimum log level (:debug/:info/:warn/"
    ":error). Messages below it are dropped. Returns the new level keyword.";
exp_t *setloglevelcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(lv);
  int level = log_level_of(lv);
  if (level < 0)
    CLEAN_RETURN_1(lv, error(ERROR_ILLEGAL_VALUE, e, env,
                             "set-log-level: LEVEL must be :debug/:info/:warn/"
                             ":error"));
  atomic_store_explicit(&g_log_level, level, memory_order_relaxed);
  char kw[16];
  int kn = snprintf(kw, sizeof kw, ":%s", g_log_level_names[level]);
  CLEAN_RETURN_1(lv, make_symbol(kw, kn));
}

/* ===== C. metrics registry (+ RESP auto-instrumentation hooks) ============ */
/* OPT-IN: metrics are compiled only with -DALCOVE_METRICS (`make
 * alcove-with-metrics`). The default build ships NONE of this — no registry, no
 * builtins, and (crucially) no per-command atomic bump in the RESP hot path
 * (a shared counter every reactor touches is a cache-line contention point).
 * error-code + logging above are always shipped (zero passive cost). */
#ifdef ALCOVE_METRICS

#define METRIC_MAX 128
#define METRIC_NAME_MAX 48
typedef struct {
  char name[METRIC_NAME_MAX];
  _Atomic int64_t val;
} metric_t;
static metric_t g_metrics[METRIC_MAX];
static _Atomic int g_metrics_n = 0;
#if !ALCOVE_SINGLE_THREADED
static pthread_mutex_t g_metrics_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

/* Find or create a metric slot by name. The slot pointer is stable (the array
 * never moves), so callers may cache it and atomic-inc directly. Creation is
 * guarded; lookups of committed slots are lock-free (names are immutable once
 * published, and g_metrics_n is bumped with release after the slot is filled).
 * Returns NULL only if the table is full. */
static metric_t *metric_slot(const char *name) {
  int n = atomic_load_explicit(&g_metrics_n, memory_order_acquire);
  for (int i = 0; i < n; i++)
    if (strcmp(g_metrics[i].name, name) == 0)
      return &g_metrics[i];
  metric_t *slot = NULL;
#if !ALCOVE_SINGLE_THREADED
  pthread_mutex_lock(&g_metrics_lock);
#endif
  n = atomic_load_explicit(&g_metrics_n, memory_order_acquire); /* re-scan */
  for (int i = 0; i < n; i++)
    if (strcmp(g_metrics[i].name, name) == 0) { slot = &g_metrics[i]; break; }
  if (!slot && n < METRIC_MAX) {
    snprintf(g_metrics[n].name, METRIC_NAME_MAX, "%s", name);
    atomic_store_explicit(&g_metrics[n].val, 0, memory_order_relaxed);
    slot = &g_metrics[n];
    atomic_store_explicit(&g_metrics_n, n + 1, memory_order_release);
  }
#if !ALCOVE_SINGLE_THREADED
  pthread_mutex_unlock(&g_metrics_lock);
#endif
  return slot;
}
/* Hot-path increment of a cached slot (used by the RESP auto-metrics). */
static inline void metric_bump(metric_t *s, int64_t d) {
  if (s)
    atomic_fetch_add_explicit(&s->val, d, memory_order_relaxed);
}

const char doc_counter_bang[] =
    "(counter! NAME n?) — add n (default 1) to the named counter, creating it "
    "at 0 if new; returns the new value. Thread-safe. See (metrics).";
exp_t *counterbangcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(nm, dv);
  if (!nm || !isstring(nm))
    CLEAN_RETURN_2(nm, dv,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "counter!: NAME must be a string"));
  int64_t d = 1;
  if (dv) {
    if (!isnumber(dv))
      CLEAN_RETURN_2(nm, dv, error(ERROR_NUMBER_EXPECTED, e, env,
                                   "counter!: n must be an integer"));
    d = FIX_VAL(dv);
  }
  metric_t *s = metric_slot(exp_text(nm));
  if (!s)
    CLEAN_RETURN_2(nm, dv, error(ERROR_ILLEGAL_VALUE, e, env,
                                 "counter!: metric table full"));
  int64_t v = atomic_fetch_add_explicit(&s->val, d, memory_order_relaxed) + d;
  CLEAN_RETURN_2(nm, dv, make_integeri(v));
}

const char doc_gauge_bang[] =
    "(gauge! NAME v) — set the named gauge to integer v (creating it if new); "
    "returns v. Thread-safe. See (metrics).";
exp_t *gaugebangcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(nm, vv);
  if (!nm || !isstring(nm))
    CLEAN_RETURN_2(nm, vv, error(ERROR_ILLEGAL_VALUE, e, env,
                                 "gauge!: NAME must be a string"));
  if (!vv || !isnumber(vv))
    CLEAN_RETURN_2(nm, vv, error(ERROR_NUMBER_EXPECTED, e, env,
                                 "gauge!: v must be an integer"));
  metric_t *s = metric_slot(exp_text(nm));
  if (!s)
    CLEAN_RETURN_2(nm, vv, error(ERROR_ILLEGAL_VALUE, e, env,
                                 "gauge!: metric table full"));
  atomic_store_explicit(&s->val, FIX_VAL(vv), memory_order_relaxed);
  CLEAN_RETURN_2(nm, vv, make_integeri(FIX_VAL(vv)));
}

const char doc_metric[] =
    "(metric NAME) — current integer value of the named metric, or nil if it "
    "doesn't exist.";
exp_t *metriccmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(nm);
  if (!nm || !isstring(nm))
    CLEAN_RETURN_1(nm, error(ERROR_ILLEGAL_VALUE, e, env,
                             "metric: NAME must be a string"));
  const char *name = exp_text(nm);
  int n = atomic_load_explicit(&g_metrics_n, memory_order_acquire);
  exp_t *ret = NIL_EXP;
  for (int i = 0; i < n; i++)
    if (strcmp(g_metrics[i].name, name) == 0) {
      ret = make_integeri(atomic_load_explicit(&g_metrics[i].val,
                                               memory_order_relaxed));
      break;
    }
  CLEAN_RETURN_1(nm, ret);
}

const char doc_metrics[] =
    "(metrics) — snapshot of all metrics as a plist of alternating "
    "name(string) value(int): (\"resp.commands\" 12 \"my.count\" 3 …). Includes "
    "the RESP server's auto-counters (resp.connections/commands/errors) when a "
    "server is running.";
exp_t *metricscmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  int n = atomic_load_explicit(&g_metrics_n, memory_order_acquire);
  exp_t *head = NULL, *tail = NULL;
  for (int i = 0; i < n; i++) {
    exp_t *kn = make_string(g_metrics[i].name, (int)strlen(g_metrics[i].name));
    exp_t *vn = make_integeri(
        atomic_load_explicit(&g_metrics[i].val, memory_order_relaxed));
    exp_t *kc = make_node(kn), *vc = make_node(vn);
    if (tail) { tail->next = kc; } else { head = kc; }
    kc->next = vc;
    tail = vc;
  }
  return head ? head : NIL_EXP;
}

#endif /* ALCOVE_METRICS */
