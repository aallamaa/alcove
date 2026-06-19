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
  REQUIRE_2_STRINGS(nameexp, valexp, CLEAN_RETURN_2(nameexp, valexp, _alc_e),
                    ERROR_ILLEGAL_VALUE, NULL, env,
                    "(setenv name val): both must be strings");
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

/* ---------- filesystem ---------- */
/* (Headers: the single TU already pulls stdio/stdlib/string; dirent/stat/wait
   are needed only by this fragment.) */
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#ifndef ALCOVE_WEB
#include <sys/wait.h>
#endif

const char doc_deletefile[] =
    "(delete-file path) — delete a file (or empty directory). Returns t, or "
    "an error if it can't.";
exp_t *deletefilecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(p);
  REQUIRE_TYPE(p, isstring, CLEAN_RETURN_1(p, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "delete-file: path must be a string");
  if (remove((const char *)exp_text(p)) != 0)
    CLEAN_RETURN_1(p, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "delete-file: %s: %s", exp_text(p),
                            strerror(errno)));
  CLEAN_RETURN_1(p, refexp(TRUE_EXP));
}

const char doc_renamefile[] =
    "(rename-file old new) — rename/move a file. Returns t, or an error.";
exp_t *renamefilecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(a, b);
  REQUIRE_2_STRINGS(a, b, CLEAN_RETURN_2(a, b, _alc_e), ERROR_ILLEGAL_VALUE,
                    NULL, env, "(rename-file old new): both must be strings");
  if (rename((const char *)exp_text(a), (const char *)exp_text(b)) != 0)
    CLEAN_RETURN_2(a, b,
                   error(ERROR_ILLEGAL_VALUE, NULL, env, "rename-file: %s",
                         strerror(errno)));
  CLEAN_RETURN_2(a, b, refexp(TRUE_EXP));
}

const char doc_makedir[] =
    "(make-dir path) — create a directory, parents included (mkdir -p). "
    "Idempotent: an existing directory returns t.";
exp_t *makedircmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(p);
  REQUIRE_TYPE(p, isstring, CLEAN_RETURN_1(p, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "make-dir: path must be a string");
  const char *path = (const char *)exp_text(p);
  char *tmp = strdup(path);
  int err = 0;
  for (char *c = tmp + 1; *c; c++) { /* +1: skip a leading '/' */
    if (*c == '/') {
      *c = 0;
      if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        err = errno;
        break;
      }
      *c = '/';
    }
  }
  if (!err && mkdir(tmp, 0777) != 0 && errno != EEXIST)
    err = errno;
  free(tmp);
  if (err)
    CLEAN_RETURN_1(p, error(ERROR_ILLEGAL_VALUE, NULL, env, "make-dir: %s: %s",
                            path, strerror(err)));
  CLEAN_RETURN_1(p, refexp(TRUE_EXP));
}

static int os_namecmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

const char doc_listdir[] =
    "(list-dir path) — the directory's entry names as a sorted list of "
    "strings ('.'/'..' excluded). Error if the path can't be opened.";
exp_t *listdircmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(p);
  REQUIRE_TYPE(p, isstring, CLEAN_RETURN_1(p, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "list-dir: path must be a string");
  DIR *d = opendir((const char *)exp_text(p));
  if (!d)
    CLEAN_RETURN_1(p, error(ERROR_ILLEGAL_VALUE, NULL, env, "list-dir: %s: %s",
                            exp_text(p), strerror(errno)));
  size_t n = 0, cap = 16;
  char **names = memalloc(cap, sizeof(char *));
  struct dirent *de;
  while ((de = readdir(d))) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    if (n == cap) {
      cap *= 2;
      char **grown = realloc(names, cap * sizeof(char *));
      if (!grown)
        graceful_shutdown("Fatal error: Out of memory");
      names = grown;
    }
    names[n++] = strdup(de->d_name);
  }
  closedir(d);
  qsort(names, n, sizeof(char *), os_namecmp);
  exp_t *head = NULL, *tail = NULL;
  for (size_t i = 0; i < n; i++) {
    list_append_owned(&head, &tail, make_string(names[i], (int)strlen(names[i])));
    free(names[i]);
  }
  free(names);
  CLEAN_RETURN_1(p, head ? head : refexp(NIL_EXP));
}

const char doc_fileinfo[] =
    "(file-info path) — a dict {\"size\" bytes, \"mtime\" unix-seconds, "
    "\"dir?\" t|nil}, or nil if the path doesn't exist.";
exp_t *fileinfocmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(p);
  REQUIRE_TYPE(p, isstring, CLEAN_RETURN_1(p, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "file-info: path must be a string");
  struct stat st;
  if (stat((const char *)exp_text(p), &st) != 0)
    CLEAN_RETURN_1(p, refexp(NIL_EXP));
  exp_t *ret = make_dict_exp();
  dict_t *d = (dict_t *)ret->ptr;
  set_get_keyval_dict(d, "size", MAKE_FIX((int64_t)st.st_size));
  set_get_keyval_dict(d, "mtime", MAKE_FIX((int64_t)st.st_mtime));
  set_get_keyval_dict(d, "dir?", S_ISDIR(st.st_mode) ? TRUE_EXP : NIL_EXP);
  CLEAN_RETURN_1(p, ret);
}

/* ---------- subprocess ---------- */

const char doc_shell[] =
    "(shell cmd) — run cmd through /bin/sh, capture stdout. Returns a dict "
    "{\"out\" string, \"exit\" code}. stderr passes through to the terminal "
    "(append 2>&1 to capture it). Not available in the web build.";
exp_t *shellcmd(exp_t *e, env_t *env) {
#ifdef ALCOVE_WEB
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "shell: not available in the web build");
#else
  EVAL_ARG_1(c);
  REQUIRE_TYPE(c, isstring, CLEAN_RETURN_1(c, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "shell: cmd must be a string");
  FILE *fp = popen((const char *)exp_text(c), "r");
  if (!fp)
    CLEAN_RETURN_1(c, error(ERROR_ILLEGAL_VALUE, NULL, env, "shell: %s",
                            strerror(errno)));
  size_t cap = 256, len = 0;
  char *buf = memalloc(cap, 1);
  size_t got;
  while ((got = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
    len += got;
    if (cap - len < 256) {
      cap *= 2;
      char *grown = realloc(buf, cap);
      if (!grown)
        graceful_shutdown("Fatal error: Out of memory");
      buf = grown;
    }
  }
  int status = pclose(fp);
  int code = WIFEXITED(status)    ? WEXITSTATUS(status)
             : WIFSIGNALED(status) ? 128 + WTERMSIG(status) /* sh convention */
                                   : -1;
  exp_t *ret = make_dict_exp();
  dict_t *d = (dict_t *)ret->ptr;
  exp_t *out = make_string_take(buf, (int)len);
  set_get_keyval_dict(d, "out", out);
  unrefexp(out);
  set_get_keyval_dict(d, "exit", MAKE_FIX((int64_t)code));
  CLEAN_RETURN_1(c, ret);
#endif
}

/* ---------- exact-byte stdin read (LSP/JSON-RPC framing) ---------- */

const char doc_readstdin[] =
    "(read-stdin n) — read EXACTLY n bytes from stdin (blocking until they "
    "arrive), as a string. Returns fewer bytes at end of input, nil when "
    "input is already exhausted. The framing primitive for protocols like "
    "LSP whose bodies are byte-counted rather than line-delimited "
    "(read-line covers the header lines).";
exp_t *readstdincmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(nexp);
  if (!isnumber(nexp) || FIX_VAL(nexp) < 0 || FIX_VAL(nexp) > 64 * 1024 * 1024)
    CLEAN_RETURN_1(nexp, error(ERROR_ILLEGAL_VALUE, NULL, env,
                               "(read-stdin n): n must be 0..64MiB"));
  size_t want = (size_t)FIX_VAL(nexp);
  char *buf = memalloc(want + 1, 1);
  size_t got = 0;
  while (got < want) {
    size_t r = fread(buf + got, 1, want - got, stdin);
    if (r == 0)
      break; /* EOF mid-body — return the short read */
    got += r;
  }
  if (got == 0 && want > 0) {
    free(buf);
    CLEAN_RETURN_1(nexp, refexp(NIL_EXP));
  }
  exp_t *ret = make_string_take(buf, (int)got);
  CLEAN_RETURN_1(nexp, ret);
}

const char doc_flush[] =
    "(flush) — flush stdout. Needed when stdout is a pipe (block-buffered): "
    "protocol servers (LSP) must flush after each message.";
exp_t *flushcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  fflush(stdout);
  return NIL_EXP;
}
