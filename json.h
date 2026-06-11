/* json.h — JSON encode/decode (json-encode / json-decode).
 * FRAGMENT #included into alcove.c (single TU), structurally cloned from
 * msgpack.h: all helpers static, only the two *cmd entry points external
 * (prototyped in builtins.h). NOT standalone, NOT separately compiled.
 * Fuzzed by json_fuzz (Makefile target) — the decoder parses untrusted bytes.
 *
 * Value mapping (mirrors msgpack-encode/-decode where both exist):
 *   encode: dict→object  list/vector→array  string→string  symbol→string
 *           fixnum→number  finite float→number  char→codepoint number
 *           t→true  nil→null (NB the empty list IS nil → null)
 *           blob/lambda/… → error;  NaN/inf → error (JSON has no spelling)
 *   decode: object→dict  array→list  string→string  true→t
 *           false→nil  null→nil      (lossy, exactly like msgpack's 0xc2)
 *           number→fixnum when integral and in int64 range, else float
 * Decode errors on malformed/truncated/trailing input; nesting is capped
 * (JS_MAX_DEPTH) because input is untrusted. */

#define JS_MAX_DEPTH 512

typedef struct {
  char *b;
  size_t len, cap;
} js_buf;
static int js_reserve(js_buf *m, size_t n) {
  if (m->len + n <= m->cap)
    return 1;
  size_t nc = m->cap ? m->cap : 64;
  while (nc < m->len + n)
    nc *= 2;
  char *nb = (char *)realloc(m->b, nc);
  if (!nb)
    return 0;
  m->b = nb;
  m->cap = nc;
  return 1;
}
static int js_put1(js_buf *m, char c) {
  if (!js_reserve(m, 1))
    return 0;
  m->b[m->len++] = c;
  return 1;
}
static int js_putn(js_buf *m, const char *p, size_t n) {
  if (!js_reserve(m, n))
    return 0;
  memcpy(m->b + m->len, p, n);
  m->len += n;
  return 1;
}
static int js_puts(js_buf *m, const char *s) { return js_putn(m, s, strlen(s)); }

/* String body, escaped per RFC 8259: ", \, and control bytes; everything
   else (incl. multi-byte UTF-8) passes through verbatim. */
static int js_put_string(js_buf *m, const char *s) {
  if (!js_put1(m, '"'))
    return 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned char c = *p;
    int ok;
    switch (c) {
    case '"':  ok = js_putn(m, "\\\"", 2); break;
    case '\\': ok = js_putn(m, "\\\\", 2); break;
    case '\b': ok = js_putn(m, "\\b", 2); break;
    case '\f': ok = js_putn(m, "\\f", 2); break;
    case '\n': ok = js_putn(m, "\\n", 2); break;
    case '\r': ok = js_putn(m, "\\r", 2); break;
    case '\t': ok = js_putn(m, "\\t", 2); break;
    default:
      if (c < 0x20) {
        char esc[8];
        snprintf(esc, sizeof esc, "\\u%04x", c);
        ok = js_putn(m, esc, 6);
      } else
        ok = js_put1(m, (char)c);
    }
    if (!ok)
      return 0;
  }
  return js_put1(m, '"');
}

/* indent==0 → compact (no whitespace); indent>0 → pretty, that many spaces
   per nesting level. */
static int js_newline_indent(js_buf *m, int indent, int depth) {
  if (!indent)
    return 1;
  if (!js_put1(m, '\n'))
    return 0;
  for (int i = 0; i < indent * depth; i++)
    if (!js_put1(m, ' '))
      return 0;
  return 1;
}

static int js_encode(exp_t *v, js_buf *m, int indent, int depth) {
  if (depth > JS_MAX_DEPTH)
    return 0;
  if (!v || v == NIL_EXP)
    return js_puts(m, "null"); /* nil — also the empty list */
  if (v == TRUE_EXP)
    return js_puts(m, "true");
  if (isnumber(v)) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", (long long)FIX_VAL(v));
    return js_puts(m, tmp);
  }
  if (ischar(v)) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%u", (unsigned)CHAR_VAL(v));
    return js_puts(m, tmp);
  }
  if (isfloat(v)) {
    double d = v->f;
    if (d != d || d == 1.0 / 0.0 || d == -1.0 / 0.0)
      return 0; /* JSON has no NaN/Infinity spelling */
    char tmp[40];
    snprintf(tmp, sizeof tmp, "%.17g", d);
    /* keep it a JSON *number* that round-trips as float: %.17g of an
       integral double prints no '.'/exponent — add ".0" so 2.0 ≠ 2 */
    if (!strpbrk(tmp, ".eE"))
      strcat(tmp, ".0");
    return js_puts(m, tmp);
  }
  if (isstring(v) || issymbol(v))
    return js_put_string(m, (const char *)exp_text(v));
  if (ispair(v)) {
    if (!js_put1(m, '['))
      return 0;
    int first = 1;
    for (exp_t *p = v; p && ispair(p) && p->content; p = p->next) {
      if (!first && !js_put1(m, ','))
        return 0;
      first = 0;
      if (!js_newline_indent(m, indent, depth + 1))
        return 0;
      if (!js_encode(p->content, m, indent, depth + 1))
        return 0;
    }
    if (!first && !js_newline_indent(m, indent, depth))
      return 0;
    return js_put1(m, ']');
  }
  if (isvector(v)) {
    if (!js_put1(m, '['))
      return 0;
    int64_t n = vec_len(v);
    for (int64_t i = 0; i < n; i++) {
      if (i && !js_put1(m, ','))
        return 0;
      if (!js_newline_indent(m, indent, depth + 1))
        return 0;
      exp_t *el = vec_get_boxed(v, i);
      int ok = js_encode(el, m, indent, depth + 1);
      unrefexp(el);
      if (!ok)
        return 0;
    }
    if (n && !js_newline_indent(m, indent, depth))
      return 0;
    return js_put1(m, ']');
  }
  if (isdict(v)) {
    dict_t *d = (dict_t *)v->ptr;
    if (!js_put1(m, '{'))
      return 0;
    int first = 1;
    if (d)
      for (unsigned int i = 0; i < d->ht[0].size; i++)
        for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
          if (!first && !js_put1(m, ','))
            return 0;
          first = 0;
          if (!js_newline_indent(m, indent, depth + 1))
            return 0;
          if (!js_put_string(m, (const char *)k->key))
            return 0;
          if (!js_put1(m, ':'))
            return 0;
          if (indent && !js_put1(m, ' '))
            return 0;
          if (!js_encode(k->val, m, indent, depth + 1))
            return 0;
        }
    if (!first && !js_newline_indent(m, indent, depth))
      return 0;
    return js_put1(m, '}');
  }
  return 0; /* unsupported type (blob, lambda, …) */
}

/* ---------- decoder ---------- */

static void js_skip_ws(const char *b, size_t len, size_t *pos) {
  while (*pos < len && (b[*pos] == ' ' || b[*pos] == '\t' || b[*pos] == '\n' ||
                        b[*pos] == '\r'))
    (*pos)++;
}

/* Append a Unicode codepoint as UTF-8. */
static int js_put_utf8(js_buf *m, uint32_t cp) {
  if (cp < 0x80)
    return js_put1(m, (char)cp);
  if (cp < 0x800)
    return js_put1(m, (char)(0xc0 | (cp >> 6))) &&
           js_put1(m, (char)(0x80 | (cp & 0x3f)));
  if (cp < 0x10000)
    return js_put1(m, (char)(0xe0 | (cp >> 12))) &&
           js_put1(m, (char)(0x80 | ((cp >> 6) & 0x3f))) &&
           js_put1(m, (char)(0x80 | (cp & 0x3f)));
  return js_put1(m, (char)(0xf0 | (cp >> 18))) &&
         js_put1(m, (char)(0x80 | ((cp >> 12) & 0x3f))) &&
         js_put1(m, (char)(0x80 | ((cp >> 6) & 0x3f))) &&
         js_put1(m, (char)(0x80 | (cp & 0x3f)));
}

static int js_hex4(const char *b, size_t len, size_t pos, uint32_t *out) {
  if (pos + 4 > len)
    return 0;
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    char c = b[pos + i];
    v <<= 4;
    if (c >= '0' && c <= '9')
      v |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f')
      v |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      v |= (uint32_t)(c - 'A' + 10);
    else
      return 0;
  }
  *out = v;
  return 1;
}

/* Parse a JSON string body (opening quote already consumed) into an owned
   EXP_STRING, or NULL on malformed input. */
static exp_t *js_decode_string(const char *b, size_t len, size_t *pos) {
  js_buf m = {NULL, 0, 0};
  while (1) {
    if (*pos >= len)
      goto bad;
    char c = b[(*pos)++];
    if (c == '"')
      break;
    if ((unsigned char)c < 0x20)
      goto bad; /* raw control byte — RFC 8259 forbids */
    if (c != '\\') {
      if (!js_put1(&m, c))
        goto bad;
      continue;
    }
    if (*pos >= len)
      goto bad;
    char esc = b[(*pos)++];
    int ok = 1;
    switch (esc) {
    case '"':  ok = js_put1(&m, '"'); break;
    case '\\': ok = js_put1(&m, '\\'); break;
    case '/':  ok = js_put1(&m, '/'); break;
    case 'b':  ok = js_put1(&m, '\b'); break;
    case 'f':  ok = js_put1(&m, '\f'); break;
    case 'n':  ok = js_put1(&m, '\n'); break;
    case 'r':  ok = js_put1(&m, '\r'); break;
    case 't':  ok = js_put1(&m, '\t'); break;
    case 'u': {
      uint32_t cp;
      if (!js_hex4(b, len, *pos, &cp))
        goto bad;
      *pos += 4;
      if (cp >= 0xd800 && cp <= 0xdbff) { /* high surrogate: need a low one */
        uint32_t lo;
        if (*pos + 6 > len || b[*pos] != '\\' || b[*pos + 1] != 'u' ||
            !js_hex4(b, len, *pos + 2, &lo) || lo < 0xdc00 || lo > 0xdfff)
          goto bad;
        *pos += 6;
        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
      } else if (cp >= 0xdc00 && cp <= 0xdfff)
        goto bad; /* lone low surrogate */
      ok = js_put_utf8(&m, cp);
      break;
    }
    default:
      goto bad;
    }
    if (!ok)
      goto bad;
  }
  {
    exp_t *s = make_string(m.b ? m.b : "", (int)m.len);
    free(m.b);
    return s;
  }
bad:
  free(m.b);
  return NULL;
}

static exp_t *js_decode(const char *b, size_t len, size_t *pos, int depth);

static exp_t *js_decode_array(const char *b, size_t len, size_t *pos,
                              int depth) {
  exp_t *head = NULL, *tail = NULL;
  js_skip_ws(b, len, pos);
  if (*pos < len && b[*pos] == ']') {
    (*pos)++;
    return refexp(NIL_EXP); /* [] → nil (the empty list) */
  }
  while (1) {
    exp_t *el = js_decode(b, len, pos, depth);
    if (!el) {
      if (head)
        unrefexp(head);
      return NULL;
    }
    exp_t *node = make_node(el);
    if (!head)
      head = tail = node;
    else {
      tail->next = node;
      tail = node;
    }
    js_skip_ws(b, len, pos);
    if (*pos >= len)
      goto bad;
    if (b[*pos] == ',') {
      (*pos)++;
      continue;
    }
    if (b[*pos] == ']') {
      (*pos)++;
      return head;
    }
    goto bad;
  }
bad:
  unrefexp(head);
  return NULL;
}

static exp_t *js_decode_object(const char *b, size_t len, size_t *pos,
                               int depth) {
  exp_t *dexp = make_dict_exp();
  dict_t *d = (dict_t *)dexp->ptr;
  js_skip_ws(b, len, pos);
  if (*pos < len && b[*pos] == '}') {
    (*pos)++;
    return dexp;
  }
  while (1) {
    js_skip_ws(b, len, pos);
    if (*pos >= len || b[*pos] != '"')
      goto bad;
    (*pos)++;
    exp_t *key = js_decode_string(b, len, pos);
    if (!key)
      goto bad;
    js_skip_ws(b, len, pos);
    if (*pos >= len || b[*pos] != ':') {
      unrefexp(key);
      goto bad;
    }
    (*pos)++;
    exp_t *val = js_decode(b, len, pos, depth);
    if (!val) {
      unrefexp(key);
      goto bad;
    }
    set_get_keyval_dict(d, exp_text(key), val); /* takes its own ref */
    unrefexp(key);
    unrefexp(val);
    js_skip_ws(b, len, pos);
    if (*pos >= len)
      goto bad;
    if (b[*pos] == ',') {
      (*pos)++;
      continue;
    }
    if (b[*pos] == '}') {
      (*pos)++;
      return dexp;
    }
    goto bad;
  }
bad:
  unrefexp(dexp);
  return NULL;
}

static exp_t *js_decode_number(const char *b, size_t len, size_t *pos) {
  size_t start = *pos;
  int isfloat_tok = 0;
  if (*pos < len && b[*pos] == '-')
    (*pos)++;
  if (*pos >= len || b[*pos] < '0' || b[*pos] > '9')
    return NULL; /* JSON requires a digit here (no ".5", no "+1", no "-") */
  if (b[*pos] == '0' && *pos + 1 < len && b[*pos + 1] >= '0' &&
      b[*pos + 1] <= '9')
    return NULL; /* RFC 8259: no leading zeros ("01") */
  while (*pos < len && b[*pos] >= '0' && b[*pos] <= '9')
    (*pos)++;
  if (*pos < len && b[*pos] == '.') {
    isfloat_tok = 1;
    (*pos)++;
    if (*pos >= len || b[*pos] < '0' || b[*pos] > '9')
      return NULL;
    while (*pos < len && b[*pos] >= '0' && b[*pos] <= '9')
      (*pos)++;
  }
  if (*pos < len && (b[*pos] == 'e' || b[*pos] == 'E')) {
    isfloat_tok = 1;
    (*pos)++;
    if (*pos < len && (b[*pos] == '+' || b[*pos] == '-'))
      (*pos)++;
    if (*pos >= len || b[*pos] < '0' || b[*pos] > '9')
      return NULL;
    while (*pos < len && b[*pos] >= '0' && b[*pos] <= '9')
      (*pos)++;
  }
  char tmp[64];
  size_t n = *pos - start;
  if (n >= sizeof tmp) { /* absurdly long literal — parse as double */
    char *big = memalloc(n + 1, 1);
    memcpy(big, b + start, n);
    big[n] = 0;
    double d = strtod(big, NULL);
    free(big);
    return make_floatf(d);
  }
  memcpy(tmp, b + start, n);
  tmp[n] = 0;
  if (!isfloat_tok) {
    errno = 0;
    long long v = strtoll(tmp, NULL, 10);
    if (errno != ERANGE)
      return MAKE_FIX((int64_t)v);
    /* doesn't fit int64 → fall through to double */
  }
  return make_floatf(strtod(tmp, NULL));
}

static exp_t *js_decode(const char *b, size_t len, size_t *pos, int depth) {
  if (depth > JS_MAX_DEPTH)
    return NULL;
  js_skip_ws(b, len, pos);
  if (*pos >= len)
    return NULL;
  char c = b[*pos];
  if (c == '{') {
    (*pos)++;
    return js_decode_object(b, len, pos, depth + 1);
  }
  if (c == '[') {
    (*pos)++;
    return js_decode_array(b, len, pos, depth + 1);
  }
  if (c == '"') {
    (*pos)++;
    return js_decode_string(b, len, pos);
  }
  if (c == 't' && *pos + 4 <= len && memcmp(b + *pos, "true", 4) == 0) {
    *pos += 4;
    return refexp(TRUE_EXP);
  }
  if (c == 'f' && *pos + 5 <= len && memcmp(b + *pos, "false", 5) == 0) {
    *pos += 5;
    return refexp(NIL_EXP); /* false → nil (lossy; matches msgpack 0xc2) */
  }
  if (c == 'n' && *pos + 4 <= len && memcmp(b + *pos, "null", 4) == 0) {
    *pos += 4;
    return refexp(NIL_EXP);
  }
  if (c == '-' || (c >= '0' && c <= '9'))
    return js_decode_number(b, len, pos);
  return NULL;
}

/* ---------- entry points ---------- */

const char doc_jsonencode[] =
    "(json-encode v) — serialize v to a JSON string: dicts → objects, "
    "lists/vectors → arrays, t → true, nil → null (NB the empty list IS nil). "
    "Symbols encode as strings; NaN/inf and non-JSON types (blob, fn, …) "
    "error. (json-encode v 2) pretty-prints with 2-space indentation.";
exp_t *jsonencodecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  exp_t *indexp = NIL_EXP;
  if (e->next && e->next->next)
    indexp = EVAL(e->next->next->content, env);
  if (iserror(indexp))
    CLEAN_RETURN_1(v, indexp);
  int indent = 0;
  if (indexp != NIL_EXP) {
    if (!isnumber(indexp) || FIX_VAL(indexp) < 0 || FIX_VAL(indexp) > 16)
      CLEAN_RETURN_2(v, indexp,
                     error(ERROR_ILLEGAL_VALUE, e, env,
                           "json-encode: indent must be an integer 0..16"));
    indent = (int)FIX_VAL(indexp);
  }
  js_buf m = {NULL, 0, 0};
  if (!js_encode(v, &m, indent, 0)) {
    free(m.b);
    CLEAN_RETURN_2(v, indexp,
                   error(ERROR_ILLEGAL_VALUE, e, env,
                         "json-encode: unsupported value (blob/fn/NaN/inf) "
                         "or nesting too deep"));
  }
  exp_t *ret = make_string(m.b ? m.b : "", (int)m.len);
  free(m.b);
  CLEAN_RETURN_2(v, indexp, ret);
}

const char doc_jsondecode[] =
    "(json-decode s) — parse a JSON string: objects → dicts, arrays → lists, "
    "true → t, false/null → nil (lossy — same convention as msgpack-decode), "
    "integral numbers → fixnums, others → floats. Errors on malformed or "
    "trailing input.";
exp_t *jsondecodecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(s);
  REQUIRE_TYPE(s, isstring, CLEAN_RETURN_1(s, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "json-decode: argument must be a string");
  const char *b = (const char *)exp_text(s);
  size_t len = strlen(b), pos = 0;
  exp_t *ret = js_decode(b, len, &pos, 0);
  if (ret) {
    js_skip_ws(b, len, &pos);
    if (pos != len) {
      unrefexp(ret);
      ret = NULL;
    }
  }
  if (!ret)
    CLEAN_RETURN_1(s, error(ERROR_ILLEGAL_VALUE, e, env,
                            "json-decode: malformed or trailing input"));
  CLEAN_RETURN_1(s, ret);
}
