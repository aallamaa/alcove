/* builtins_regex.h — POSIX Extended Regular Expression builtins:
 * re-match / re-find / re-find-all / re-replace / re-split.
 * FRAGMENT #included into alcove.c (single TU). Named builtins_regex.h, NOT
 * regex.h — alcove builds with -I., so a fragment called regex.h would
 * shadow the SYSTEM <regex.h> this file depends on.
 *
 * Zero-dependency by design: libc regcomp/regexec (REG_EXTENDED). musl /
 * emscripten ship it, so the browser playground gets regex for free — the
 * reason this is in core rather than an FFI binding (which would be
 * native-only). The costs, documented loudly in the docstrings + guide:
 * ERE syntax ([[:digit:]] not \d, no lazy quantifiers, no lookaround) and
 * BYTE offsets (not codepoints) from re-find.
 *
 * A small static cache of compiled patterns (RE_CACHE_N, round-robin
 * eviction) keeps hot loops from recompiling; it is entirely local to this
 * fragment and thread-confined state is acceptable here because builtins
 * already run under the interpreter's single-threaded contract (the RESP
 * reactors guard global mutation separately — worst case a cache slot is
 * recompiled).
 */
#include <regex.h>

#define RE_CACHE_N 8
#define RE_MAX_GROUPS 32

typedef struct {
  char *pat; /* strdup'd pattern, NULL = empty slot */
  regex_t re;
} re_cache_slot;
static ALCOVE_TLS re_cache_slot re_cache[RE_CACHE_N];
static ALCOVE_TLS int re_cache_next = 0;

/* Compile (or fetch the cached) pattern. Returns NULL + fills errbuf on a
   bad pattern. The returned regex_t* stays valid until RE_CACHE_N newer
   patterns evict it — callers use it immediately and don't hold it. */
static regex_t *re_compiled(const char *pat, char *errbuf, size_t errn) {
  for (int i = 0; i < RE_CACHE_N; i++)
    if (re_cache[i].pat && strcmp(re_cache[i].pat, pat) == 0)
      return &re_cache[i].re;
  regex_t re;
  int rc = regcomp(&re, pat, REG_EXTENDED);
  if (rc != 0) {
    regerror(rc, &re, errbuf, errn);
    return NULL;
  }
  re_cache_slot *slot = &re_cache[re_cache_next];
  re_cache_next = (re_cache_next + 1) % RE_CACHE_N;
  if (slot->pat) {
    free(slot->pat);
    regfree(&slot->re);
  }
  slot->pat = strdup(pat);
  slot->re = re;
  return &slot->re;
}

/* Common arg handling: evaluate (pat s) and compile. On failure, returns 0
   with *err set to an owned error exp. */
static int re_args(exp_t *e, env_t *env, const char *name, exp_t **pat,
                   exp_t **s, regex_t **re, exp_t **err) {
  *pat = EVAL(cadr(e), env);
  if (*pat && iserror(*pat)) {
    *err = *pat;
    *pat = NULL;
    return 0;
  }
  *s = e->next && e->next->next ? EVAL(e->next->next->content, env) : NULL;
  if (*s && iserror(*s)) {
    *err = *s;
    *s = NULL;
    return 0;
  }
  if (!*pat || !*s || !isstring(*pat) || !isstring(*s)) {
    *err = error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "(%s pattern string): both must be strings", name);
    return 0;
  }
  char errbuf[128];
  *re = re_compiled((const char *)exp_text(*pat), errbuf, sizeof errbuf);
  if (!*re) {
    *err = error(ERROR_ILLEGAL_VALUE, NULL, env, "%s: bad pattern: %s", name,
                 errbuf);
    return 0;
  }
  return 1;
}

#define RE_CLEANUP(pat, s)                                                     \
  do {                                                                         \
    if (pat)                                                                   \
      unrefexp(pat);                                                           \
    if (s)                                                                     \
      unrefexp(s);                                                             \
    unrefexp(e);                                                               \
  } while (0)

const char doc_rematch[] =
    "(re-match pat s) — POSIX EXTENDED regex (ERE: [[:digit:]] etc., no \\d, "
    "no lazy quantifiers). First match as (full group1 group2 ...), an "
    "unmatched optional group as nil; nil when there is no match.";
exp_t *rematchcmd(exp_t *e, env_t *env) {
  exp_t *pat, *s, *err;
  regex_t *re;
  if (!re_args(e, env, "re-match", &pat, &s, &re, &err)) {
    RE_CLEANUP(pat, s);
    return err;
  }
  regmatch_t m[RE_MAX_GROUPS];
  const char *str = (const char *)exp_text(s);
  if (regexec(re, str, RE_MAX_GROUPS, m, 0) != 0) {
    RE_CLEANUP(pat, s);
    return refexp(NIL_EXP);
  }
  size_t ng = re->re_nsub + 1;
  if (ng > RE_MAX_GROUPS)
    ng = RE_MAX_GROUPS;
  exp_t *head = NULL, *tail = NULL;
  for (size_t g = 0; g < ng; g++) {
    exp_t *el;
    if (m[g].rm_so < 0)
      el = refexp(NIL_EXP); /* optional group that didn't participate */
    else
      el =
          make_string((char *)str + m[g].rm_so, (int)(m[g].rm_eo - m[g].rm_so));
    list_append_owned(&head, &tail, el);
  }
  RE_CLEANUP(pat, s);
  return head;
}

const char doc_refind[] =
    "(re-find pat s [start]) — (start end) BYTE offsets of the first match "
    "at/after `start` (default 0), or nil. Compose with substr; offsets are "
    "bytes, not codepoints.";
exp_t *refindcmd(exp_t *e, env_t *env) {
  exp_t *pat, *s, *err;
  regex_t *re;
  if (!re_args(e, env, "re-find", &pat, &s, &re, &err)) {
    RE_CLEANUP(pat, s);
    return err;
  }
  int64_t start = 0;
  if (e->next->next->next) {
    exp_t *st = EVAL(e->next->next->next->content, env);
    if (st && iserror(st)) {
      RE_CLEANUP(pat, s);
      return st;
    }
    if (!st || !isnumber(st) || FIX_VAL(st) < 0) {
      if (st)
        unrefexp(st);
      RE_CLEANUP(pat, s);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "re-find: start must be a non-negative integer");
    }
    start = FIX_VAL(st);
  }
  const char *str = (const char *)exp_text(s);
  size_t slen = strlen(str);
  if ((size_t)start > slen) {
    RE_CLEANUP(pat, s);
    return refexp(NIL_EXP);
  }
  regmatch_t m[1];
  /* REG_NOTBOL when start>0: ^ shouldn't match mid-string */
  if (regexec(re, str + start, 1, m, start ? REG_NOTBOL : 0) != 0) {
    RE_CLEANUP(pat, s);
    return refexp(NIL_EXP);
  }
  exp_t *n1 = make_node(MAKE_FIX(start + m[0].rm_so));
  exp_t *n2 = make_node(MAKE_FIX(start + m[0].rm_eo));
  n1->next = n2;
  RE_CLEANUP(pat, s);
  return n1;
}

const char doc_refindall[] =
    "(re-find-all pat s) — every (non-overlapping) full-match substring, in "
    "order, as a list of strings. An empty match advances one byte.";
exp_t *refindallcmd(exp_t *e, env_t *env) {
  exp_t *pat, *s, *err;
  regex_t *re;
  if (!re_args(e, env, "re-find-all", &pat, &s, &re, &err)) {
    RE_CLEANUP(pat, s);
    return err;
  }
  const char *str = (const char *)exp_text(s);
  size_t slen = strlen(str), off = 0;
  exp_t *head = NULL, *tail = NULL;
  while (off <= slen) {
    regmatch_t m[1];
    if (regexec(re, str + off, 1, m, off ? REG_NOTBOL : 0) != 0)
      break;
    list_append_owned(&head, &tail,
                      make_string((char *)str + off + m[0].rm_so,
                                  (int)(m[0].rm_eo - m[0].rm_so)));
    off += (size_t)m[0].rm_eo;
    if (m[0].rm_eo == m[0].rm_so)
      off++; /* empty match — advance to avoid looping forever */
  }
  RE_CLEANUP(pat, s);
  return head ? head : refexp(NIL_EXP);
}

const char doc_rereplace[] =
    "(re-replace pat s replacement) — replace every match with the LITERAL "
    "replacement string (no $1 backrefs). An empty match advances one byte.";
exp_t *rereplacecmd(exp_t *e, env_t *env) {
  exp_t *pat, *s, *err;
  regex_t *re;
  if (!re_args(e, env, "re-replace", &pat, &s, &re, &err)) {
    RE_CLEANUP(pat, s);
    return err;
  }
  exp_t *rep =
      e->next->next->next ? EVAL(e->next->next->next->content, env) : NULL;
  if (rep && iserror(rep)) {
    RE_CLEANUP(pat, s);
    return rep;
  }
  if (!rep || !isstring(rep)) {
    if (rep)
      unrefexp(rep);
    RE_CLEANUP(pat, s);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "(re-replace pat s replacement): replacement must be a "
                 "string");
  }
  const char *str = (const char *)exp_text(s);
  const char *rp = (const char *)exp_text(rep);
  size_t rl = strlen(rp), slen = strlen(str), off = 0;
  size_t cap = slen + 32, len = 0;
  char *out = memalloc(cap, 1);
#define RE_OUT(p, n)                                                           \
  do {                                                                         \
    while (len + (n) + 1 > cap) {                                              \
      cap *= 2;                                                                \
      char *_g = realloc(out, cap);                                            \
      if (!_g)                                                                 \
        graceful_shutdown("Fatal error: Out of memory");                       \
      out = _g;                                                                \
    }                                                                          \
    memcpy(out + len, (p), (n));                                               \
    len += (n);                                                                \
  } while (0)
  while (off <= slen) {
    regmatch_t m[1];
    if (regexec(re, str + off, 1, m, off ? REG_NOTBOL : 0) != 0)
      break;
    RE_OUT(str + off, (size_t)m[0].rm_so);
    RE_OUT(rp, rl);
    off += (size_t)m[0].rm_eo;
    if (m[0].rm_eo == m[0].rm_so) { /* empty match: copy 1 byte, advance */
      if (off < slen)
        RE_OUT(str + off, 1);
      off++;
    }
  }
  if (off < slen)
    RE_OUT(str + off, slen - off);
#undef RE_OUT
  exp_t *ret = make_string(out, (int)len);
  free(out);
  unrefexp(rep);
  RE_CLEANUP(pat, s);
  return ret;
}

const char doc_resplit[] =
    "(re-split pat s) — split s on every match of pat (the regex sibling of "
    "string-split): (re-split \"[ ,]+\" \"a, b c\") → (\"a\" \"b\" \"c\"). "
    "Empty patterns error; leading/trailing matches yield empty strings.";
exp_t *resplitcmd(exp_t *e, env_t *env) {
  exp_t *pat, *s, *err;
  regex_t *re;
  if (!re_args(e, env, "re-split", &pat, &s, &re, &err)) {
    RE_CLEANUP(pat, s);
    return err;
  }
  const char *str = (const char *)exp_text(s);
  size_t slen = strlen(str), off = 0;
  exp_t *head = NULL, *tail = NULL;
  while (1) {
    regmatch_t m[1];
    int hit =
        off <= slen && regexec(re, str + off, 1, m, off ? REG_NOTBOL : 0) == 0;
    if (hit && m[0].rm_eo == m[0].rm_so) {
      /* an empty match would split between every byte forever — reject the
         degenerate pattern outright rather than guess */
      if (head)
        unrefexp(head);
      RE_CLEANUP(pat, s);
      return error(ERROR_ILLEGAL_VALUE, NULL, env,
                   "re-split: pattern matches the empty string");
    }
    size_t segend = hit ? off + (size_t)m[0].rm_so : slen;
    list_append_owned(&head, &tail,
                      make_string((char *)str + off, (int)(segend - off)));
    if (!hit)
      break;
    off += (size_t)m[0].rm_eo;
  }
  RE_CLEANUP(pat, s);
  return head;
}
