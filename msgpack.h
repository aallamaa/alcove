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
static int mp_reserve(mp_buf *m, size_t n) { /* see buf_reserve (alcove.c) */
  void *nb = buf_reserve(m->b, m->len, n, &m->cap);
  if (!nb)
    return 0;
  m->b = (uint8_t *)nb;
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
/* Emit a MessagePack length prefix for a container/string. The 4 length-
   prefixed kinds (str, bin, array, map) share one big-endian cascade that
   differs only in which tiers exist and the tag bytes:
     - fix tier:   if n < fixlim, emit a single byte (fixmask | n).
                   fixlim==0 disables it (bin has no fix tier).
     - 1-byte tier: if n <= 0xff, emit tag8 then a 1-byte length.
                   tag8==0 disables it (array/map have no 8-bit tier).
     - 2-byte tier: if n <= 0xffff, emit tag16 then a 2-byte length.
     - 4-byte tier: otherwise emit tag32 then a 4-byte length.
   Emits exactly the same bytes the open-coded cascades did. */
static int mp_put_sized(mp_buf *m, uint8_t fixmask, size_t fixlim, uint8_t tag8,
                        uint8_t tag16, uint8_t tag32, size_t n) {
  if (fixlim && n < fixlim)
    return mp_put1(m, (uint8_t)(fixmask | n));
  if (tag8 && n <= 0xff)
    return mp_put1(m, tag8) && mp_put_be(m, n, 1);
  if (n <= 0xffff)
    return mp_put1(m, tag16) && mp_put_be(m, n, 2);
  return mp_put1(m, tag32) && mp_put_be(m, n, 4);
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
  return mp_put_sized(m, 0xa0, 32, 0xd9, 0xda, 0xdb, len);
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
    int h = mp_put_sized(m, 0, 0, 0xc4, 0xc5, 0xc6, len);
    return h && mp_putn(m, blob_bytes(v), len);
  }
  if (ispair(v)) {
    size_t n = 0;
    for (exp_t *p = v; p && ispair(p) && p->content; p = p->next)
      n++;
    int h = mp_put_sized(m, 0x90, 16, 0, 0xdc, 0xdd, n);
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
    int h = mp_put_sized(m, 0x80, 16, 0, 0xde, 0xdf, n);
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

static exp_t *mp_decode(const uint8_t *b, size_t len, size_t *pos, int depth);
static uint64_t mp_get_be(const uint8_t *b, size_t pos, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++)
    v = (v << 8) | b[pos + i];
  return v;
}
/* Sign-extend the low nb*8 bits of v to a full int64. nb==8 is a no-op
   (full width). Reproduces exactly the per-width `(int8_t/int16_t/int32_t/
   int64_t)` casts the signed integer arms used: shift the sign bit of the
   nb-byte field up to bit 63, then arithmetic-shift back down. */
static inline int64_t mp_sext(uint64_t v, int nb) {
  int sh = 64 - nb * 8;
  return (int64_t)(v << sh) >> sh;
}
/* Recursion-depth cap for untrusted input — a deeply-nested blob (e.g. a long
   run of 0x91 fixarray bytes) would otherwise blow the C stack. Mirrors
   json.h's JS_MAX_DEPTH and the persistence loader's depth guard. */
#define MP_MAX_DEPTH 512
/* Decode `n` elements into a fresh list. */
static exp_t *mp_decode_array(const uint8_t *b, size_t len, size_t *pos,
                              size_t n, int depth) {
  exp_t *head = NULL, *tail = NULL;
  for (size_t i = 0; i < n; i++) {
    exp_t *el = mp_decode(b, len, pos, depth + 1);
    if (!el) {
      if (head)
        unrefexp(head);
      return NULL;
    }
    list_append_owned(&head, &tail, el);
  }
  return head ? head : refexp(NIL_EXP);
}
/* Decode `n` key/value pairs into a fresh dict (keys must be msgpack strings).
 */
static exp_t *mp_decode_map(const uint8_t *b, size_t len, size_t *pos,
                            size_t n, int depth) {
  exp_t *dexp = make_dict_exp();
  dict_t *d = (dict_t *)dexp->ptr;
  for (size_t i = 0; i < n; i++) {
    exp_t *key = mp_decode(b, len, pos, depth + 1);
    if (!key) {
      unrefexp(dexp);
      return NULL;
    }
    if (!isstring(key)) {
      unrefexp(key);
      unrefexp(dexp);
      return NULL;
    }
    exp_t *val = mp_decode(b, len, pos, depth + 1);
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
static exp_t *mp_decode(const uint8_t *b, size_t len, size_t *pos, int depth) {
  if (depth > MP_MAX_DEPTH)
    return NULL;
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
    return mp_decode_array(b, len, pos, c & 0x0f, depth);
  if ((c & 0xf0) == 0x80)
    return mp_decode_map(b, len, pos, c & 0x0f, depth);
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
  case 0xcc: /* uint8 */
  case 0xcd: /* uint16 */
  case 0xce: /* uint32 */
  case 0xcf: /* uint64 (reinterpreted as int64) */ {
    int nb = (c == 0xcc) ? 1 : (c == 0xcd) ? 2 : (c == 0xce) ? 4 : 8;
    MP_NEED((size_t)nb);
    int64_t v = (int64_t)mp_get_be(b, *pos, nb);
    *pos += nb;
    return MAKE_FIX(v);
  }
  case 0xd0: /* int8 */
  case 0xd1: /* int16 */
  case 0xd2: /* int32 */
  case 0xd3: /* int64 */ {
    int nb = (c == 0xd0) ? 1 : (c == 0xd1) ? 2 : (c == 0xd2) ? 4 : 8;
    MP_NEED((size_t)nb);
    int64_t v = mp_sext(mp_get_be(b, *pos, nb), nb);
    *pos += nb;
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
    if (n > len - *pos) /* underflow-safe (*pos<=len after MP_NEED); n is 32-bit attacker data */
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
    if (n > len - *pos) /* underflow-safe (*pos<=len after MP_NEED); n is 32-bit attacker data */
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
    return mp_decode_array(b, len, pos, n, depth);
  }
  case 0xde:
  case 0xdf: {
    int nb = (c == 0xde) ? 2 : 4;
    MP_NEED((size_t)nb);
    size_t n = (size_t)mp_get_be(b, *pos, nb);
    *pos += nb;
    return mp_decode_map(b, len, pos, n, depth);
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
  exp_t *ret = mp_decode((const uint8_t *)blob_bytes(b), len, &pos, 0);
  if (!ret || pos != len) {
    if (ret)
      unrefexp(ret);
    CLEAN_RETURN_1(b, error(ERROR_ILLEGAL_VALUE, e, env,
                            "msgpack-decode: malformed or trailing data"));
  }
  CLEAN_RETURN_1(b, ret);
}
