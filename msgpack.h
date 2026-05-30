/* msgpack.h — MessagePack encode/decode (msgpack-encode / msgpack-decode).
 * FRAGMENT #included into alcove.c (single TU). Self-contained: all helpers
 * are static; only the two *cmd entry points are external (prototyped in
 * builtins.h). NOT standalone, NOT separately compiled. Fuzzed by
 * msgpack_test.c.
 */
/* ---------- MsgPack serialization (msgpack-encode / msgpack-decode) ----------
   Compact binary codec for the JSON-ish subset of alcove values: nil, t,
   fixnums, floats, strings (and symbols), blobs (→ bin), lists (→ array), and
   dicts with string keys (→ map). MessagePack is big-endian. Round-trips with
   itself; useful for interop over RESP / FFI / files. Unsupported types
   (lambda/ffi/…) make encode error; malformed/truncated input makes decode
   error. */
typedef struct {
  uint8_t *b;
  size_t len, cap;
} mp_buf;
static int mp_reserve(mp_buf *m, size_t n) {
  if (m->len + n <= m->cap)
    return 1;
  size_t nc = m->cap ? m->cap : 64;
  while (nc < m->len + n)
    nc *= 2;
  uint8_t *nb = (uint8_t *)realloc(m->b, nc);
  if (!nb)
    return 0;
  m->b = nb;
  m->cap = nc;
  return 1;
}
static int mp_put1(mp_buf *m, uint8_t b) {
  if (!mp_reserve(m, 1))
    return 0;
  m->b[m->len++] = b;
  return 1;
}
static int mp_putn(mp_buf *m, const void *p, size_t n) {
  if (!mp_reserve(m, n))
    return 0;
  memcpy(m->b + m->len, p, n);
  m->len += n;
  return 1;
}
static int mp_put_be(mp_buf *m, uint64_t v, int nbytes) {
  for (int i = nbytes - 1; i >= 0; i--)
    if (!mp_put1(m, (uint8_t)(v >> (i * 8))))
      return 0;
  return 1;
}
static int mp_encode(exp_t *v, mp_buf *m);
static int mp_encode_int(mp_buf *m, int64_t n) {
  if (n >= 0) {
    if (n < 128)
      return mp_put1(m, (uint8_t)n);
    if (n <= 0xffLL)
      return mp_put1(m, 0xcc) && mp_put_be(m, (uint64_t)n, 1);
    if (n <= 0xffffLL)
      return mp_put1(m, 0xcd) && mp_put_be(m, (uint64_t)n, 2);
    if (n <= 0xffffffffLL)
      return mp_put1(m, 0xce) && mp_put_be(m, (uint64_t)n, 4);
    return mp_put1(m, 0xd3) && mp_put_be(m, (uint64_t)n, 8);
  }
  if (n >= -32)
    return mp_put1(m, (uint8_t)n);
  if (n >= -128)
    return mp_put1(m, 0xd0) && mp_put_be(m, (uint64_t)n, 1);
  if (n >= -32768)
    return mp_put1(m, 0xd1) && mp_put_be(m, (uint64_t)n, 2);
  if (n >= -2147483648LL)
    return mp_put1(m, 0xd2) && mp_put_be(m, (uint64_t)n, 4);
  return mp_put1(m, 0xd3) && mp_put_be(m, (uint64_t)n, 8);
}
static int mp_encode_strlen(mp_buf *m, size_t len) {
  if (len < 32)
    return mp_put1(m, (uint8_t)(0xa0 | len));
  if (len <= 0xff)
    return mp_put1(m, 0xd9) && mp_put_be(m, len, 1);
  if (len <= 0xffff)
    return mp_put1(m, 0xda) && mp_put_be(m, len, 2);
  return mp_put1(m, 0xdb) && mp_put_be(m, len, 4);
}
static int mp_encode(exp_t *v, mp_buf *m) {
  if (!v || v == NIL_EXP)
    return mp_put1(m, 0xc0); /* nil (also the empty list) */
  if (v == TRUE_EXP)
    return mp_put1(m, 0xc3);
  if (isnumber(v))
    return mp_encode_int(m, FIX_VAL(v));
  if (ischar(v))
    return mp_encode_int(m, (int64_t)CHAR_VAL(v));
  if (isfloat(v)) {
    uint64_t bits;
    double d = v->f;
    memcpy(&bits, &d, 8);
    return mp_put1(m, 0xcb) && mp_put_be(m, bits, 8);
  }
  if (isstring(v) || issymbol(v)) {
    const char *s = exp_text(v);
    size_t len = strlen(s);
    return mp_encode_strlen(m, len) && mp_putn(m, s, len);
  }
  if (isblob(v)) {
    size_t len = blob_len(v);
    int h = (len <= 0xff)     ? (mp_put1(m, 0xc4) && mp_put_be(m, len, 1))
            : (len <= 0xffff) ? (mp_put1(m, 0xc5) && mp_put_be(m, len, 2))
                              : (mp_put1(m, 0xc6) && mp_put_be(m, len, 4));
    return h && mp_putn(m, blob_bytes(v), len);
  }
  if (ispair(v)) {
    size_t n = 0;
    for (exp_t *p = v; p && ispair(p) && p->content; p = p->next)
      n++;
    int h = (n < 16)        ? mp_put1(m, (uint8_t)(0x90 | n))
            : (n <= 0xffff) ? (mp_put1(m, 0xdc) && mp_put_be(m, n, 2))
                            : (mp_put1(m, 0xdd) && mp_put_be(m, n, 4));
    if (!h)
      return 0;
    for (exp_t *p = v; p && ispair(p) && p->content; p = p->next)
      if (!mp_encode(p->content, m))
        return 0;
    return 1;
  }
  if (isdict(v)) {
    dict_t *d = (dict_t *)v->ptr;
    size_t n = d ? d->ht[0].used : 0;
    int h = (n < 16)        ? mp_put1(m, (uint8_t)(0x80 | n))
            : (n <= 0xffff) ? (mp_put1(m, 0xde) && mp_put_be(m, n, 2))
                            : (mp_put1(m, 0xdf) && mp_put_be(m, n, 4));
    if (!h)
      return 0;
    if (d)
      for (unsigned int i = 0; i < d->ht[0].size; i++)
        for (keyval_t *k = d->ht[0].table[i]; k; k = k->next) {
          const char *key = (char *)k->key;
          size_t kl = strlen(key);
          if (!(mp_encode_strlen(m, kl) && mp_putn(m, key, kl)))
            return 0;
          if (!mp_encode(k->val, m))
            return 0;
        }
    return 1;
  }
  return 0; /* unsupported type */
}

static exp_t *mp_decode(const uint8_t *b, size_t len, size_t *pos);
static uint64_t mp_get_be(const uint8_t *b, size_t pos, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++)
    v = (v << 8) | b[pos + i];
  return v;
}
/* Decode `n` elements into a fresh list. */
static exp_t *mp_decode_array(const uint8_t *b, size_t len, size_t *pos,
                              size_t n) {
  exp_t *head = NULL, *tail = NULL;
  for (size_t i = 0; i < n; i++) {
    exp_t *el = mp_decode(b, len, pos);
    if (!el) {
      if (head)
        unrefexp(head);
      return NULL;
    }
    exp_t *node = make_node(el);
    if (!head) {
      head = node;
      tail = node;
    } else {
      tail->next = node;
      tail = node;
    }
  }
  return head ? head : refexp(NIL_EXP);
}
/* Decode `n` key/value pairs into a fresh dict (keys must be msgpack strings).
 */
static exp_t *mp_decode_map(const uint8_t *b, size_t len, size_t *pos,
                            size_t n) {
  exp_t *dexp = make_dict_exp();
  dict_t *d = (dict_t *)dexp->ptr;
  for (size_t i = 0; i < n; i++) {
    exp_t *key = mp_decode(b, len, pos);
    if (!key) {
      unrefexp(dexp);
      return NULL;
    }
    if (!isstring(key)) {
      unrefexp(key);
      unrefexp(dexp);
      return NULL;
    }
    exp_t *val = mp_decode(b, len, pos);
    if (!val) {
      unrefexp(key);
      unrefexp(dexp);
      return NULL;
    }
    set_get_keyval_dict(d, exp_text(key), val); /* takes its own ref on val */
    unrefexp(key);
    unrefexp(val);
  }
  return dexp;
}
static exp_t *mp_decode(const uint8_t *b, size_t len, size_t *pos) {
  if (*pos >= len)
    return NULL;
  uint8_t c = b[(*pos)++];
  if (c <= 0x7f)
    return MAKE_FIX(c); /* positive fixint */
  if (c >= 0xe0)
    return MAKE_FIX((int8_t)c); /* negative fixint */
  if ((c & 0xe0) == 0xa0) {     /* fixstr */
    size_t n = c & 0x1f;
    if (*pos + n > len)
      return NULL;
    exp_t *s = make_string((char *)(b + *pos), (int)n);
    *pos += n;
    return s;
  }
  if ((c & 0xf0) == 0x90)
    return mp_decode_array(b, len, pos, c & 0x0f);
  if ((c & 0xf0) == 0x80)
    return mp_decode_map(b, len, pos, c & 0x0f);
#define MP_NEED(n)                                                             \
  do {                                                                         \
    if (*pos + (n) > len)                                                      \
      return NULL;                                                             \
  } while (0)
  switch (c) {
  case 0xc0:
    return refexp(NIL_EXP);
  case 0xc2:
    return refexp(NIL_EXP); /* false → nil */
  case 0xc3:
    return refexp(TRUE_EXP); /* true → t */
  case 0xcc: {
    MP_NEED(1);
    int64_t v = (int64_t)mp_get_be(b, *pos, 1);
    *pos += 1;
    return MAKE_FIX(v);
  }
  case 0xcd: {
    MP_NEED(2);
    int64_t v = (int64_t)mp_get_be(b, *pos, 2);
    *pos += 2;
    return MAKE_FIX(v);
  }
  case 0xce: {
    MP_NEED(4);
    int64_t v = (int64_t)mp_get_be(b, *pos, 4);
    *pos += 4;
    return MAKE_FIX(v);
  }
  case 0xcf: {
    MP_NEED(8);
    int64_t v = (int64_t)mp_get_be(b, *pos, 8);
    *pos += 8;
    return MAKE_FIX(v);
  }
  case 0xd0: {
    MP_NEED(1);
    int64_t v = (int8_t)mp_get_be(b, *pos, 1);
    *pos += 1;
    return MAKE_FIX(v);
  }
  case 0xd1: {
    MP_NEED(2);
    int64_t v = (int16_t)mp_get_be(b, *pos, 2);
    *pos += 2;
    return MAKE_FIX(v);
  }
  case 0xd2: {
    MP_NEED(4);
    int64_t v = (int32_t)mp_get_be(b, *pos, 4);
    *pos += 4;
    return MAKE_FIX(v);
  }
  case 0xd3: {
    MP_NEED(8);
    int64_t v = (int64_t)mp_get_be(b, *pos, 8);
    *pos += 8;
    return MAKE_FIX(v);
  }
  case 0xca: {
    MP_NEED(4);
    uint32_t bits = (uint32_t)mp_get_be(b, *pos, 4);
    *pos += 4;
    float f;
    memcpy(&f, &bits, 4);
    return make_floatf((double)f);
  }
  case 0xcb: {
    MP_NEED(8);
    uint64_t bits = mp_get_be(b, *pos, 8);
    *pos += 8;
    double d;
    memcpy(&d, &bits, 8);
    return make_floatf(d);
  }
  case 0xd9:
  case 0xda:
  case 0xdb: {
    int nb = (c == 0xd9) ? 1 : (c == 0xda) ? 2 : 4;
    MP_NEED((size_t)nb);
    size_t n = (size_t)mp_get_be(b, *pos, nb);
    *pos += nb;
    if (*pos + n > len)
      return NULL;
    exp_t *s = make_string((char *)(b + *pos), (int)n);
    *pos += n;
    return s;
  }
  case 0xc4:
  case 0xc5:
  case 0xc6: {
    int nb = (c == 0xc4) ? 1 : (c == 0xc5) ? 2 : 4;
    MP_NEED((size_t)nb);
    size_t n = (size_t)mp_get_be(b, *pos, nb);
    *pos += nb;
    if (*pos + n > len)
      return NULL;
    exp_t *bl = make_blob((char *)(b + *pos), n);
    *pos += n;
    return bl;
  }
  case 0xdc:
  case 0xdd: {
    int nb = (c == 0xdc) ? 2 : 4;
    MP_NEED((size_t)nb);
    size_t n = (size_t)mp_get_be(b, *pos, nb);
    *pos += nb;
    return mp_decode_array(b, len, pos, n);
  }
  case 0xde:
  case 0xdf: {
    int nb = (c == 0xde) ? 2 : 4;
    MP_NEED((size_t)nb);
    size_t n = (size_t)mp_get_be(b, *pos, nb);
    *pos += nb;
    return mp_decode_map(b, len, pos, n);
  }
  default:
    return NULL;
  }
#undef MP_NEED
}

const char doc_msgpackencode[] =
    "(msgpack-encode v) — serialize v to a MessagePack blob. Supports nil, t, "
    "fixnums, floats, strings/symbols, blobs, lists, and string-keyed dicts.";
exp_t *msgpackencodecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  mp_buf m = {NULL, 0, 0};
  if (!mp_encode(v, &m)) {
    free(m.b);
    CLEAN_RETURN_1(v, error(ERROR_ILLEGAL_VALUE, e, env,
                            "msgpack-encode: unsupported value type"));
  }
  exp_t *ret = make_blob((char *)m.b, m.len);
  free(m.b);
  CLEAN_RETURN_1(v, ret);
}
const char doc_msgpackdecode[] =
    "(msgpack-decode blob) — parse a MessagePack blob back into an alcove "
    "value (the inverse of msgpack-encode). Errors on malformed/truncated "
    "input or a non-string map key.";
exp_t *msgpackdecodecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(b);
  if (!isblob(b))
    CLEAN_RETURN_1(b, error(ERROR_ILLEGAL_VALUE, e, env,
                            "msgpack-decode: argument must be a blob"));
  size_t pos = 0, len = blob_len(b);
  exp_t *ret = mp_decode((const uint8_t *)blob_bytes(b), len, &pos);
  if (!ret || pos != len) {
    if (ret)
      unrefexp(ret);
    CLEAN_RETURN_1(b, error(ERROR_ILLEGAL_VALUE, e, env,
                            "msgpack-decode: malformed or trailing data"));
  }
  CLEAN_RETURN_1(b, ret);
}
