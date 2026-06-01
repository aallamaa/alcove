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
