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
               ERROR_ILLEGAL_VALUE, NULL, env, "getenv: name must be a string");
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

const char doc_eprn[] = "(eprn x ...) — like prn, but to stderr (no colour).";
exp_t *eprncmd(exp_t *e, env_t *env) { return epr_common(e, env, 1); }

/* ---------- stdin ---------- */

const char doc_readline[] =
    "(read-line [port]) — read one line (without trailing newline) from stdin, "
    "or from the given readable port. Returns nil at end of input.";
exp_t *readlinecmd(exp_t *e, env_t *env) {
  FILE *fp = stdin;
  exp_t *portexp = NULL;
  if (e->next) {
    portexp = EVAL(e->next->content, env);
    if (iserror(portexp)) { unrefexp(e); return portexp; }
    if (!isport(portexp)) {
      unrefexp(portexp); unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "read-line: argument must be a port");
    }
    alc_port_t *p = (alc_port_t *)portexp->ptr;
    if (!p || p->closed || !p->fp || p->mode != 'r') {
      unrefexp(portexp); unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "read-line: port is not open for reading");
    }
    fp = p->fp;
  }
  char *line = NULL;
  size_t cap = 0;
  ssize_t n = getline(&line, &cap, fp);
  if (portexp) unrefexp(portexp);
  unrefexp(e);
  if (n < 0) { free(line); return refexp(NIL_EXP); }
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
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
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
    CLEAN_RETURN_1(p,
                   error(ERROR_ILLEGAL_VALUE, NULL, env, "delete-file: %s: %s",
                         exp_text(p), strerror(errno)));
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
    list_append_owned(&head, &tail,
                      make_string(names[i], (int)strlen(names[i])));
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
    CLEAN_RETURN_1(
        c, error(ERROR_ILLEGAL_VALUE, NULL, env, "shell: %s", strerror(errno)));
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
  int code = WIFEXITED(status)     ? WEXITSTATUS(status)
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
    "(flush [port]) — flush stdout, or the given writable port.";
exp_t *flushcmd(exp_t *e, env_t *env) {
  if (e->next) {
    exp_t *portexp = EVAL(e->next->content, env);
    if (iserror(portexp)) { unrefexp(e); return portexp; }
    if (!isport(portexp)) {
      unrefexp(portexp); unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "flush: argument must be a port");
    }
    alc_port_t *p = (alc_port_t *)portexp->ptr;
    if (p && !p->closed && p->fp) fflush(p->fp);
    unrefexp(portexp);
  } else {
    fflush(stdout);
  }
  unrefexp(e);
  return refexp(NIL_EXP);
}

const char doc_direxistsp[] =
    "(dir-exists? path) — t if path exists and is a directory, nil otherwise.";
exp_t *direxistspcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(p);
  REQUIRE_TYPE(p, isstring, CLEAN_RETURN_1(p, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "dir-exists?: path must be a string");
  struct stat st;
  int res = (stat((const char *)exp_text(p), &st) == 0 && S_ISDIR(st.st_mode))
                ? 1
                : 0;
  CLEAN_RETURN_1(p, refexp(res ? TRUE_EXP : NIL_EXP));
}

const char doc_pathjoin[] = "(path-join a b ...) — join path components using "
                            "a single slash separator.";
exp_t *pathjoincmd(exp_t *e, env_t *env) {
  size_t cap = 64, len = 0;
  char *buf = memalloc(cap, 1);
  int first = 1;
  for (exp_t *cur = cdr(e); cur; cur = cur->next) {
    exp_t *val = EVAL(cur->content, env);
    if (iserror(val)) {
      free(buf);
      unrefexp(e);
      return val;
    }
    if (!isstring(val)) {
      unrefexp(val);
      free(buf);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "path-join: arguments must be strings");
    }
    const char *s = (const char *)exp_text(val);
    size_t sl = strlen(s);
    if (sl > 0) {
      if (!first && len > 0 && buf[len - 1] != '/' && s[0] != '/') {
        if (len + 1 >= cap) {
          cap *= 2;
          buf = xrealloc(buf, cap);
        }
        buf[len++] = '/';
      }
      size_t start_offset = 0;
      if (len > 0 && buf[len - 1] == '/' && s[0] == '/') {
        start_offset = 1;
      }
      if (len + sl - start_offset + 1 >= cap) {
        cap = (len + sl + 1) * 2;
        buf = xrealloc(buf, cap);
      }
      memcpy(buf + len, s + start_offset, sl - start_offset);
      len += sl - start_offset;
      first = 0;
    }
    unrefexp(val);
  }
  buf[len] = 0;
  exp_t *ret = make_string_take(buf, (int)len);
  unrefexp(e);
  return ret;
}

const char doc_pathdirname[] = "(path-dirname path) — directory component of "
                               "path (everything before the last slash).";
exp_t *pathdirnamecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(p);
  REQUIRE_TYPE(p, isstring, CLEAN_RETURN_1(p, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "path-dirname: path must be a string");
  const char *path = (const char *)exp_text(p);
  const char *last_slash = strrchr(path, '/');
  if (!last_slash) {
    CLEAN_RETURN_1(p, make_string((char *)".", 1));
  }
  if (last_slash == path) {
    CLEAN_RETURN_1(p, make_string((char *)"/", 1));
  }
  size_t len = (size_t)(last_slash - path);
  CLEAN_RETURN_1(p, make_string((char *)path, (int)len));
}

const char doc_pathbasename[] = "(path-basename path) — filename component of "
                                "path (everything after the last slash).";
exp_t *pathbasenamecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(p);
  REQUIRE_TYPE(p, isstring, CLEAN_RETURN_1(p, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "path-basename: path must be a string");
  const char *path = (const char *)exp_text(p);
  const char *last_slash = strrchr(path, '/');
  if (!last_slash) {
    CLEAN_RETURN_1(p, refexp(p));
  }
  const char *base = last_slash + 1;
  CLEAN_RETURN_1(p, make_string((char *)base, (int)strlen(base)));
}

const char doc_resolvehost[] = "(resolve-host hostname) — resolve hostname to "
                               "a list of IP address strings (IPv4/IPv6).";
exp_t *resolvehostcmd(exp_t *e, env_t *env) {
#ifdef ALCOVE_WEB
  (void)env;
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "resolve-host: not available in the web build");
#else
  EVAL_ARG_1(hostexp);
  REQUIRE_TYPE(hostexp, isstring, CLEAN_RETURN_1(hostexp, _alc_e),
               ERROR_ILLEGAL_VALUE, NULL, env,
               "resolve-host: hostname must be a string");
  const char *hostname = (const char *)exp_text(hostexp);
  struct addrinfo hints, *res, *p;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  int status = getaddrinfo(hostname, NULL, &hints, &res);
  if (status != 0) {
    CLEAN_RETURN_1(hostexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env, "resolve-host: %s: %s",
                         hostname, gai_strerror(status)));
  }
  exp_t *head = NULL, *tail = NULL;
  for (p = res; p != NULL; p = p->ai_next) {
    if (!p->ai_addr)
      continue;
    char ipstr[INET6_ADDRSTRLEN];
    void *addr = NULL;
    if (p->ai_family == AF_INET) {
      struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
      addr = &(ipv4->sin_addr);
    } else if (p->ai_family == AF_INET6) {
      struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
      addr = &(ipv6->sin6_addr);
    } else {
      continue;
    }
    if (!inet_ntop(p->ai_family, addr, ipstr, sizeof(ipstr))) {
      continue;
    }
    int dup = 0;
    for (exp_t *curr = head; curr && ispair(curr); curr = curr->next) {
      if (strcmp((const char *)exp_text(curr->content), ipstr) == 0) {
        dup = 1;
        break;
      }
    }
    if (!dup) {
      list_append_owned(&head, &tail,
                        make_string((char *)ipstr, (int)strlen(ipstr)));
    }
  }
  freeaddrinfo(res);
  CLEAN_RETURN_1(hostexp, head ? head : refexp(NIL_EXP));
#endif
}

const char doc_tcpconnect[] =
    "(tcp-connect host port) — open a TCP connection to the host on the given "
    "port. Returns a file descriptor (fixnum).";
exp_t *tcpconnectcmd(exp_t *e, env_t *env) {
#ifdef ALCOVE_WEB
  (void)env;
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "tcp-connect: not available in the web build");
#else
  EVAL_ARG_2(hostexp, portexp);
  if (!isstring(hostexp) || !isnumber(portexp)) {
    CLEAN_RETURN_2(
        hostexp, portexp,
        error(ERROR_ILLEGAL_VALUE, NULL, env,
              "(tcp-connect host port): string + port number expected"));
  }
  const char *host = (const char *)exp_text(hostexp);
  int64_t port = FIX_VAL(portexp);
  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", (int)port);
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  int status = getaddrinfo(host, port_str, &hints, &res);
  if (status != 0) {
    CLEAN_RETURN_2(hostexp, portexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "tcp-connect: resolve failed: %s",
                         gai_strerror(status)));
  }
  int fd = -1;
  struct addrinfo *p;
  for (p = res; p != NULL; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0)
      continue;
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    CLEAN_RETURN_2(hostexp, portexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "tcp-connect: connect failed: %s", strerror(errno)));
  }
  CLEAN_RETURN_2(hostexp, portexp, MAKE_FIX((int64_t)fd));
#endif
}

const char doc_tcpsend[] = "(tcp-send fd string) — send string data over the "
                           "TCP socket. Returns number of bytes sent.";
exp_t *tcpsendcmd(exp_t *e, env_t *env) {
#ifdef ALCOVE_WEB
  (void)env;
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "tcp-send: not available in the web build");
#else
  EVAL_ARG_2(fdexp, strexp);
  if (!isnumber(fdexp) || !isstring(strexp)) {
    CLEAN_RETURN_2(fdexp, strexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "(tcp-send fd string): integer fd + string expected"));
  }
  int fd = (int)FIX_VAL(fdexp);
  const char *data = (const char *)exp_text(strexp);
  size_t len = strlen(data);
  ssize_t sent = send(fd, data, len, 0);
  if (sent < 0) {
    CLEAN_RETURN_2(
        fdexp, strexp,
        error(ERROR_ILLEGAL_VALUE, NULL, env, "tcp-send: %s", strerror(errno)));
  }
  CLEAN_RETURN_2(fdexp, strexp, MAKE_FIX((int64_t)sent));
#endif
}

const char doc_tcprecv[] = "(tcp-recv fd max-bytes) — receive up to max-bytes "
                           "from the TCP socket as a string.";
exp_t *tcprecvcmd(exp_t *e, env_t *env) {
#ifdef ALCOVE_WEB
  (void)env;
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "tcp-recv: not available in the web build");
#else
  EVAL_ARG_2(fdexp, maxexp);
  if (!isnumber(fdexp) || !isnumber(maxexp) || FIX_VAL(maxexp) <= 0 ||
      FIX_VAL(maxexp) > 16 * 1024 * 1024) {
    CLEAN_RETURN_2(fdexp, maxexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "(tcp-recv fd max-bytes): integer fd + positive "
                         "max-bytes (<= 16MB) expected"));
  }
  int fd = (int)FIX_VAL(fdexp);
  size_t max_bytes = (size_t)FIX_VAL(maxexp);
  char *buf = memalloc(max_bytes + 1, 1);
  ssize_t received = recv(fd, buf, max_bytes, 0);
  if (received < 0) {
    free(buf);
    CLEAN_RETURN_2(
        fdexp, maxexp,
        error(ERROR_ILLEGAL_VALUE, NULL, env, "tcp-recv: %s", strerror(errno)));
  }
  exp_t *ret = make_string_take(buf, (int)received);
  CLEAN_RETURN_2(fdexp, maxexp, ret);
#endif
}

const char doc_tcpclose[] =
    "(tcp-close fd) — close the TCP connection file descriptor. Returns t.";
exp_t *tcpclosecmd(exp_t *e, env_t *env) {
#ifdef ALCOVE_WEB
  (void)env;
  unrefexp(e);
  return error(ERROR_ILLEGAL_VALUE, NULL, env,
               "tcp-close: not available in the web build");
#else
  EVAL_ARG_1(fdexp);
  if (!isnumber(fdexp)) {
    CLEAN_RETURN_1(fdexp, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                "tcp-close: fd must be an integer"));
  }
  int fd = (int)FIX_VAL(fdexp);
  close(fd);
  CLEAN_RETURN_1(fdexp, refexp(TRUE_EXP));
#endif
}

const char doc_formattime[] =
    "(format-time [unix-seconds [format-string]]) — format unix seconds "
    "(default now) as UTC into a string using strftime "
    "(default %Y-%m-%d %H:%M:%S).";
exp_t *formattimecmd(exp_t *e, env_t *env) {
  exp_t *sec_exp = NIL_EXP;
  exp_t *fmt_exp = NIL_EXP;
  if (e->next) {
    sec_exp = EVAL(e->next->content, env);
    if (iserror(sec_exp)) {
      unrefexp(e);
      return sec_exp;
    }
    if (e->next->next) {
      fmt_exp = EVAL(e->next->next->content, env);
      if (iserror(fmt_exp)) {
        unrefexp(sec_exp);
        unrefexp(e);
        return fmt_exp;
      }
    }
  }
  time_t t;
  if (sec_exp == NIL_EXP) {
    t = time(NULL);
  } else {
    if (!isnumber(sec_exp)) {
      unrefexp(sec_exp);
      unrefexp(fmt_exp);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "format-time: unix-seconds must be a number");
    }
    t = (time_t)FIX_VAL(sec_exp);
  }
  const char *fmt = "%Y-%m-%d %H:%M:%S";
  if (fmt_exp != NIL_EXP) {
    if (!isstring(fmt_exp)) {
      unrefexp(sec_exp);
      unrefexp(fmt_exp);
      unrefexp(e);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "format-time: format-string must be a string");
    }
    fmt = (const char *)exp_text(fmt_exp);
  }
  struct tm tmbuf;
  struct tm *tmp = gmtime_r(&t, &tmbuf);
  if (!tmp) {
    unrefexp(sec_exp);
    unrefexp(fmt_exp);
    unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env, "format-time: gmtime failed");
  }
  char out[256];
  size_t n = strftime(out, sizeof(out), fmt, tmp);
  exp_t *ret = make_string(out, (int)n);
  unrefexp(sec_exp);
  unrefexp(fmt_exp);
  unrefexp(e);
  return ret;
}

const char doc_parsetime[] =
    "(parse-time format-string string) — parse a UTC time string into unix "
    "seconds (fixnum) using strptime. Returns nil on failure.";
exp_t *parsetimecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(fmtexp, strexp);
  REQUIRE_2_STRINGS(fmtexp, strexp, CLEAN_RETURN_2(fmtexp, strexp, _alc_e),
                    ERROR_ILLEGAL_VALUE, NULL, env,
                    "(parse-time format-string string): both must be strings");
  const char *fmt = (const char *)exp_text(fmtexp);
  const char *str = (const char *)exp_text(strexp);
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = 70;
  tm.tm_mday = 1;
  char *res = strptime(str, fmt, &tm);
  if (!res) {
    CLEAN_RETURN_2(fmtexp, strexp, refexp(NIL_EXP));
  }
  time_t t = timegm(&tm);
  if (t == (time_t)-1) {
    CLEAN_RETURN_2(fmtexp, strexp, refexp(NIL_EXP));
  }
  CLEAN_RETURN_2(fmtexp, strexp, MAKE_FIX((int64_t)t));
}

/* ── Stream IO / ports ── */

const char doc_open[] =
    "(open path mode) — open a file for buffered streaming. mode is \"r\", "
    "\"w\", or \"a\". Returns a port, or a catchable error.";
exp_t *opencmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(pathexp, modeexp);
  if (!isstring(pathexp) || !isstring(modeexp))
    CLEAN_RETURN_2(pathexp, modeexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "(open path mode): two strings expected"));
  const char *m = (const char *)exp_text(modeexp);
  if (!m[0] || m[1] || (m[0] != 'r' && m[0] != 'w' && m[0] != 'a'))
    CLEAN_RETURN_2(pathexp, modeexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "open: mode must be \"r\", \"w\", or \"a\""));
  const char *path = (const char *)exp_text(pathexp);
  FILE *fp = fopen(path, m);
  if (!fp)
    CLEAN_RETURN_2(pathexp, modeexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "open: cannot open '%s': %s", path, strerror(errno)));
  alc_port_t *p = (alc_port_t *)memalloc(1, sizeof(alc_port_t));
  p->fp = fp;
  p->path = strdup(path);
  p->mode = m[0];
  p->closed = 0;
  MAKE_TYPED(ret, EXP_PORT, p);
  CLEAN_RETURN_2(pathexp, modeexp, ret);
}

const char doc_close[] =
    "(close port) — close an open port. Idempotent (closing again is a no-op).";
exp_t *closecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(portexp);
  if (!isport(portexp))
    CLEAN_RETURN_1(portexp, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                  "(close port): a port expected"));
  alc_port_t *p = (alc_port_t *)portexp->ptr;
  if (p && !p->closed && p->fp) {
    fclose(p->fp);
    p->fp = NULL;
    p->closed = 1;
  }
  CLEAN_RETURN_1(portexp, refexp(NIL_EXP));
}

const char doc_write[] =
    "(write port string) — write string to a writable port. Returns nil.";
exp_t *writecmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(portexp, strexp);
  if (!isport(portexp) || !isstring(strexp))
    CLEAN_RETURN_2(portexp, strexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "(write port string): a port and a string expected"));
  alc_port_t *p = (alc_port_t *)portexp->ptr;
  if (!p || p->closed || !p->fp || p->mode == 'r')
    CLEAN_RETURN_2(portexp, strexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "write: port is not open for writing"));
  const char *s = (const char *)exp_text(strexp);
  size_t n = strlen(s);
  if (n && fwrite(s, 1, n, p->fp) != n)
    CLEAN_RETURN_2(portexp, strexp,
                   error(ERROR_ILLEGAL_VALUE, NULL, env, "write: write failed"));
  CLEAN_RETURN_2(portexp, strexp, refexp(NIL_EXP));
}

const char doc_eofp[] =
    "(eof? port) — t if the readable port is at end-of-file, nil otherwise.";
exp_t *eofpcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(portexp);
  if (!isport(portexp))
    CLEAN_RETURN_1(portexp, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                  "(eof? port): a port expected"));
  alc_port_t *p = (alc_port_t *)portexp->ptr;
  if (!p || p->closed || !p->fp || p->mode != 'r')
    CLEAN_RETURN_1(portexp, error(ERROR_ILLEGAL_VALUE, NULL, env,
                                  "eof?: port is not open for reading"));
  int c = fgetc(p->fp);
  if (c == EOF)
    CLEAN_RETURN_1(portexp, refexp(TRUE_EXP));
  ungetc(c, p->fp);
  CLEAN_RETURN_1(portexp, refexp(NIL_EXP));
}

const char doc_portp[] = "(port? x) — t if x is a port, nil otherwise.";
exp_t *portpcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(a);
  CLEAN_RETURN_1(a, isport(a) ? refexp(TRUE_EXP) : refexp(NIL_EXP));
}
