/* builtins_os.h — the OS/scripting floor: environment variables, stderr
 * printing, stdin line reads (and, in later stages, filesystem ops + shell).
 * FRAGMENT #included into alcove.c (single TU) after builtins_stdlib.h so the
 * EVAL/CLEAN_RETURN macros and exp_to_string_buf are in scope.
 * NOT standalone, NOT separately compiled.
 *
 * The `*args*` global (script argv) is NOT here — it's a plain global binding
 * created in alcove_init() and rebound by main() once argv is parsed.
 */

/* ---------- environment ---------- */

const char doc_getenv[] =
    "(getenv name) — the environment variable's value as a string, or nil if "
    "unset. (getenv name default) returns default when unset.";
exp_t *getenvcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(nameexp);
  exp_t *defexp = NIL_EXP;
  if (e->next && e->next->next)
    defexp = EVAL(e->next->next->content, env);
  if (iserror(defexp))
    CLEAN_RETURN_1(nameexp, defexp);
  REQUIRE_TYPE(nameexp, isstring, CLEAN_RETURN_2(nameexp, defexp, _alc_e),
               ERROR_ILLEGAL_VALUE, NULL, env,
               "getenv: name must be a string");
  const char *v = getenv((const char *)exp_text(nameexp));
  exp_t *ret = v ? make_string((char *)v, (int)strlen(v)) : refexp(defexp);
  CLEAN_RETURN_2(nameexp, defexp, ret);
}

const char doc_setenv[] =
    "(setenv name val) — set an environment variable for this process (and "
    "its children, e.g. (shell ...)). Returns t.";
exp_t *setenvcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(nameexp, valexp);
  if (!isstring(nameexp) || !isstring(valexp))
    CLEAN_RETURN_2(nameexp, valexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "(setenv name val): both must be strings"));
  if (setenv((const char *)exp_text(nameexp), (const char *)exp_text(valexp),
             1) != 0)
    CLEAN_RETURN_2(nameexp, valexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env, "setenv failed"));
  CLEAN_RETURN_2(nameexp, valexp, refexp(TRUE_EXP));
}

/* ---------- stderr printing ---------- */

/* epr/eprn mirror pr/prn but write to stderr, rendered via exp_to_string_buf
   (the (str ...) renderer): strings raw, everything else in its printed form,
   no ANSI colour — diagnostics should stay grep-able when redirected. */
static exp_t *epr_common(exp_t *e, env_t *env, int newline) {
  size_t cap = 64, len = 0;
  char *buf = memalloc(cap, 1);
  for (exp_t *a = cdr(e); a; a = a->next) {
    exp_t *v = EVAL(a->content, env);
    if (iserror(v)) {
      free(buf);
      unrefexp(e);
      return v;
    }
    exp_to_string_buf(v, &buf, &len, &cap);
    unrefexp(v);
  }
  fwrite(buf, 1, len, stderr);
  if (newline)
    fputc('\n', stderr);
  fflush(stderr);
  free(buf);
  unrefexp(e);
  return NIL_EXP;
}

const char doc_epr[] = "(epr x ...) — like pr, but to stderr (no colour).";
exp_t *eprcmd(exp_t *e, env_t *env) { return epr_common(e, env, 0); }

const char doc_eprn[] =
    "(eprn x ...) — like prn, but to stderr (no colour).";
exp_t *eprncmd(exp_t *e, env_t *env) { return epr_common(e, env, 1); }

/* ---------- stdin ---------- */

const char doc_readline[] =
    "(read-line) — read one line from stdin (without the trailing newline). "
    "Returns nil at end of input.";
exp_t *readlinecmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  char *line = NULL;
  size_t cap = 0;
  ssize_t n = getline(&line, &cap, stdin);
  if (n < 0) {
    free(line);
    return refexp(NIL_EXP);
  }
  while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
    n--;
  exp_t *ret = make_string(line, (int)n);
  free(line);
  return ret;
}
