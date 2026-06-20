/* blob.h — binary-safe byte blob (EXP_BLOB) ops: make-blob, blob-ref,
 * read-bytes, blob->string, string->blob. FRAGMENT #included into alcove.c
 * (single TU). The blob dump/load serializers stay in the persistence cluster.
 * NOT standalone, NOT separately compiled.
 */
/* ---------- blob ops ---------- */

const char doc_makeblob[] = "(make-blob N) — N-byte zero-filled blob; or "
                            "(make-blob \"...\") to copy a string.";
exp_t *makeblobcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(a);
  exp_t *ret;
  if (isnumber(a)) {
    int64_t n = FIX_VAL(a);
    if (n < 0)
      CLEAN_RETURN_1(a, error(ERROR_ILLEGAL_VALUE, NULL, env,
                              "make-blob: negative length"));
    ret = make_blob(NULL, (size_t)n);
  } else if (isstring(a)) {
    const char *_t = exp_text(a);
    ret = make_blob((const char *)_t, strlen((char *)_t));
  } else {
    CLEAN_RETURN_1(a, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "make-blob: arg must be a number or string"));
  }
  CLEAN_RETURN_1(a, ret);
}

const char doc_bloblen[] = "(blob-len b) — byte count of b.";
UNARY_TYPE_CMD(bloblencmd, "blob-len: arg must be a blob", isblob, alc_blob_t,
               MAKE_FIX((int64_t)val_ptr->len))

const char doc_blobref[] =
    "(blob-ref b i) — byte at index i as fixnum (0..255).";
exp_t *blobrefcmd(exp_t *e, env_t *env) {
  EVAL_ARG_2(b, i);
  REQUIRE_TYPE(b, isblob, CLEAN_RETURN_2(b, i, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "blob-ref: first arg must be a blob");
  REQUIRE_TYPE(i, isnumber, CLEAN_RETURN_2(b, i, _alc_e), ERROR_NUMBER_EXPECTED,
               NULL, env, "blob-ref: index must be a number");
  int64_t idx = FIX_VAL(i);
  alc_blob_t *bb = (alc_blob_t *)b->ptr;
  if (idx < 0 || (size_t)idx >= bb->len)
    CLEAN_RETURN_2(b, i,
                   error(ERROR_INDEX_OUT_OF_RANGE, NULL, env,
                         "blob-ref: index %lld out of range", (long long)idx));
  int64_t v = (int64_t)(unsigned char)bb->bytes[idx];
  CLEAN_RETURN_2(b, i, MAKE_FIX(v));
}

const char doc_readbytes[] =
    "(read-bytes \"path\") — slurp a file into a blob. Returns nil on "
    "missing/unreadable file, or an error on bad arg.";
exp_t *readbytescmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(a);
  REQUIRE_TYPE(a, isstring, CLEAN_RETURN_1(a, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "read-bytes: path must be a string");
  FILE *fp = fopen((const char *)exp_text(a), "rb");
  unrefexp(a);
  unrefexp(e);
  if (!fp)
    return refexp(NIL_EXP);
  /* Two-pass: stat-style seek so we allocate exactly once. fseek/ftell
     can lie on pipes, so cap defensively and fall back if the size
     looks bogus. */
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return refexp(NIL_EXP);
  }
  long sz = ftell(fp);
  if (sz < 0 || sz > (long)(1L << 30)) {
    fclose(fp);
    return refexp(NIL_EXP);
  }
  rewind(fp);
  exp_t *blob = make_blob(NULL, (size_t)sz);
  alc_blob_t *bb = (alc_blob_t *)blob->ptr;
  if (sz > 0 && fread(bb->bytes, 1, (size_t)sz, fp) != (size_t)sz) {
    fclose(fp);
    unrefexp(blob);
    return refexp(NIL_EXP);
  }
  fclose(fp);
  return blob;
}

const char doc_blob2string[] =
    "(blob->string b) — copy blob bytes into a fresh string. Errors unless "
    "the bytes are valid UTF-8 and NUL-free (a string is NUL-terminated and "
    "codepoint-oriented, so neither is representable).";
exp_t *blob2stringcmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(obj);
  REQUIRE_TYPE(obj, isblob, CLEAN_RETURN_1(obj, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "blob->string: arg must be a blob");
  size_t n = blob_len(obj);
  const char *bytes = blob_bytes(obj);
  for (size_t i = 0; i < n; i++)
    if (bytes[i] == '\0')
      CLEAN_RETURN_1(
          obj, error(ERROR_ILLEGAL_VALUE, NULL, env,
                     "blob->string: blob has a NUL byte at offset %zu — not "
                     "representable as a string",
                     i));
  size_t bad = 0;
  if (!utf8_valid(bytes, n, &bad))
    CLEAN_RETURN_1(obj,
                   error(ERROR_ILLEGAL_VALUE, NULL, env,
                         "blob->string: invalid UTF-8 at offset %zu", bad));
  exp_t *ret = make_string((char *)bytes, (int)n);
  CLEAN_RETURN_1(obj, ret);
}
const char doc_string2blob[] =
    "(string->blob s) — wrap string bytes in a fresh blob.";
UNARY_TYPE_CMD(string2blobcmd, "string->blob: arg must be a string", isstring,
               char, make_blob(val_ptr, strlen(val_ptr)))

/* ---------- byte codecs: base64 / hex ---------- */

static const char b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Accept a blob or a string arg; yields bytes+len. Returns 0 on wrong type. */
static int blob_or_string_bytes(exp_t *v, const char **bytes, size_t *len) {
  if (isblob(v)) {
    *bytes = blob_bytes(v);
    *len = blob_len(v);
    return 1;
  }
  if (isstring(v)) {
    *bytes = (const char *)exp_text(v);
    *len = strlen(*bytes);
    return 1;
  }
  return 0;
}

const char doc_base64encode[] =
    "(base64-encode x) — RFC 4648 base64 of a blob or string, as a string "
    "(padded, no line wrapping).";
exp_t *base64encodecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  const char *src;
  size_t n;
  if (!blob_or_string_bytes(v, &src, &n))
    CLEAN_RETURN_1(v, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "base64-encode: arg must be a blob or string"));
  if (n > SIZE_MAX /
              2) /* ((n+2)/3)*4+1 must not wrap (defensive; n is in-memory) */
    CLEAN_RETURN_1(v, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "base64-encode: input too large"));
  size_t outn = ((n + 2) / 3) * 4;
  char *out = memalloc(outn + 1, 1);
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t w = (uint32_t)(unsigned char)src[i] << 16;
    if (i + 1 < n)
      w |= (uint32_t)(unsigned char)src[i + 1] << 8;
    if (i + 2 < n)
      w |= (uint32_t)(unsigned char)src[i + 2];
    out[o++] = b64_tab[(w >> 18) & 63];
    out[o++] = b64_tab[(w >> 12) & 63];
    out[o++] = i + 1 < n ? b64_tab[(w >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? b64_tab[w & 63] : '=';
  }
  exp_t *ret = make_string(out, (int)o);
  free(out);
  CLEAN_RETURN_1(v, ret);
}

const char doc_base64decode[] =
    "(base64-decode s) — decode a base64 string to a blob ((blob->string b) "
    "for text). Errors on bad characters or length.";
exp_t *base64decodecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(s);
  REQUIRE_TYPE(s, isstring, CLEAN_RETURN_1(s, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "base64-decode: arg must be a string");
  const char *src = (const char *)exp_text(s);
  size_t n = strlen(src);
  while (n > 0 && src[n - 1] == '=')
    n--;
  if ((strlen(src) % 4 != 0 && strlen(src) != 0) || strlen(src) - n > 2)
    CLEAN_RETURN_1(s, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "base64-decode: bad input length"));
  size_t outn = (n / 4) * 3 + (n % 4 == 3 ? 2 : n % 4 == 2 ? 1 : 0);
  if (n % 4 == 1)
    CLEAN_RETURN_1(s, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "base64-decode: bad input length"));
  char *out = memalloc(outn ? outn : 1, 1);
  uint32_t w = 0;
  int nb = 0;
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    const char *p = strchr(b64_tab, src[i]);
    if (!p || !src[i]) {
      free(out);
      CLEAN_RETURN_1(s, error(ERROR_ILLEGAL_VALUE, NULL, env,
                              "base64-decode: bad character '%c'", src[i]));
    }
    w = (w << 6) | (uint32_t)(p - b64_tab);
    if (++nb == 4) {
      out[o++] = (char)(w >> 16);
      out[o++] = (char)(w >> 8);
      out[o++] = (char)w;
      nb = 0;
      w = 0;
    }
  }
  if (nb == 3) {
    out[o++] = (char)(w >> 10);
    out[o++] = (char)(w >> 2);
  } else if (nb == 2)
    out[o++] = (char)(w >> 4);
  exp_t *ret = make_blob(out, o);
  free(out);
  CLEAN_RETURN_1(s, ret);
}

const char doc_hexencode[] =
    "(hex-encode x) — lowercase hex of a blob or string, as a string.";
exp_t *hexencodecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(v);
  const char *src;
  size_t n;
  if (!blob_or_string_bytes(v, &src, &n))
    CLEAN_RETURN_1(v, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "hex-encode: arg must be a blob or string"));
  if (n >
      (SIZE_MAX - 1) / 2) /* n*2+1 must not wrap (defensive; n is in-memory) */
    CLEAN_RETURN_1(v, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "hex-encode: input too large"));
  char *out = memalloc(n * 2 + 1, 1);
  static const char hx[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hx[((unsigned char)src[i]) >> 4];
    out[i * 2 + 1] = hx[((unsigned char)src[i]) & 15];
  }
  exp_t *ret = make_string(out, (int)(n * 2));
  free(out);
  CLEAN_RETURN_1(v, ret);
}

const char doc_hexdecode[] =
    "(hex-decode s) — decode a hex string (either case) to a blob. Errors on "
    "odd length or a non-hex digit.";
exp_t *hexdecodecmd(exp_t *e, env_t *env) {
  EVAL_ARG_1(s);
  REQUIRE_TYPE(s, isstring, CLEAN_RETURN_1(s, _alc_e), ERROR_ILLEGAL_VALUE,
               NULL, env, "hex-decode: arg must be a string");
  const char *src = (const char *)exp_text(s);
  size_t n = strlen(src);
  if (n % 2)
    CLEAN_RETURN_1(s, error(ERROR_ILLEGAL_VALUE, NULL, env,
                            "hex-decode: odd-length input"));
  char *out = memalloc(n / 2 + 1, 1);
  for (size_t i = 0; i < n; i++) {
    char c = src[i];
    int d = chr2hex[(unsigned char)c]; /* shared byte->nibble table (char.h) */
    if (d < 0) {
      free(out);
      CLEAN_RETURN_1(s, error(ERROR_ILLEGAL_VALUE, NULL, env,
                              "hex-decode: bad hex digit '%c'", c));
    }
    if (i % 2)
      out[i / 2] |= (char)d;
    else
      out[i / 2] = (char)(d << 4);
  }
  exp_t *ret = make_blob(out, n / 2);
  free(out);
  CLEAN_RETURN_1(s, ret);
}
