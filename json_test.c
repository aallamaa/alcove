/* json_test.c — C-level unit tests + fuzzing for the JSON codec (json.h).
 * Mirrors msgpack_test.c:
 *   1. round-trip: js_encode(v) then js_decode() yields an iso-equal value
 *      (modulo the documented lossiness: false/null → nil, symbols → strings,
 *      vectors → lists).
 *   2. decoder robustness: js_decode() consumes fully untrusted text, so feed
 *      it random / mutated garbage and require it to never crash, read OOB,
 *      or leak (run under ASan/UBSan via the make target).
 *
 * Compiles alcove.c into this TU (parser_test.c pattern) for the static js_*.
 * Build/run:  make json-test     fuzz:  make json-fuzz
 */
#define main alcove_real_main
#include "alcove.c"
#undef main
#include <stdint.h>
#include <stdio.h>

static void init_singletons(void) {
  if (!nil_singleton)
    nil_singleton = make_nil();
  if (!true_singleton)
    true_singleton = make_symbol("t", 1);
}

#ifdef JSON_LIBFUZZER
/* Coverage-guided entry for the text decoder — it parses fully untrusted
   input (files, network payloads via shell/FFI). libFuzzer supplies main. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (!nil_singleton)
    init_singletons();
  size_t pos = 0;
  exp_t *d = js_decode((const char *)data, size, &pos, 0);
  if (d)
    unrefexp(d);
  return 0;
}
#else

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c)                                                                     \
      g_pass++;                                                                \
    else {                                                                     \
      g_fail++;                                                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                              \
      printf(__VA_ARGS__);                                                     \
      printf("\n");                                                            \
    }                                                                          \
  } while (0)

/* decode(text) and require iso-equality with `expect` (consumes expect). */
static void roundtrip_decode(const char *text, exp_t *expect) {
  size_t pos = 0;
  exp_t *d = js_decode(text, strlen(text), &pos, 0);
  CHECK(d != NULL, "decode failed: %s", text);
  if (d) {
    js_skip_ws(text, strlen(text), &pos);
    CHECK(pos == strlen(text), "trailing input: %s", text);
    CHECK(isoequal(d, expect), "decode != expect: %s", text);
    unrefexp(d);
  }
  unrefexp(expect);
}

/* encode v, decode the result, require iso-equality with v. */
static void roundtrip_value(exp_t *v) {
  js_buf m = {NULL, 0, 0};
  int ok = js_encode(v, &m, 0, 0);
  CHECK(ok, "encode failed");
  if (ok) {
    size_t pos = 0;
    exp_t *d = js_decode(m.b, m.len, &pos, 0);
    CHECK(d && pos == m.len, "re-decode failed: %.*s", (int)m.len, m.b);
    if (d) {
      CHECK(isoequal(d, v), "round-trip mismatch: %.*s", (int)m.len, m.b);
      unrefexp(d);
    }
  }
  free(m.b);
  unrefexp(v);
}

static exp_t *mklist2(exp_t *a, exp_t *b) {
  exp_t *n1 = make_node(a), *n2 = make_node(b);
  n1->next = n2;
  return n1;
}

static void test_roundtrip(void) {
  roundtrip_decode("null", refexp(NIL_EXP));
  roundtrip_decode("false", refexp(NIL_EXP));
  roundtrip_decode("true", refexp(TRUE_EXP));
  roundtrip_decode("42", MAKE_FIX(42));
  roundtrip_decode("-7", MAKE_FIX(-7));
  roundtrip_decode("  [1, 2]  ", mklist2(MAKE_FIX(1), MAKE_FIX(2)));
  roundtrip_decode("2.5", make_floatf(2.5));
  roundtrip_decode("1e3", make_floatf(1000.0));
  roundtrip_decode("\"a\\u00e9b\"", make_string("a\xc3\xa9"
                                                "b",
                                                4));
  /* astral plane via surrogate pair: U+1F600 → 4-byte UTF-8 */
  roundtrip_decode("\"\\ud83d\\ude00\"", make_string("\xf0\x9f\x98\x80", 4));

  roundtrip_value(MAKE_FIX(0));
  roundtrip_value(MAKE_FIX(-123456789));
  roundtrip_value(make_floatf(3.141592653589793));
  roundtrip_value(make_floatf(-0.0));
  roundtrip_value(
      make_string("with \"quotes\" \\ and\nnewline\tetc",
                  (int)strlen("with \"quotes\" \\ and\nnewline\tetc")));
  roundtrip_value(refexp(TRUE_EXP));
  roundtrip_value(mklist2(MAKE_FIX(1), mklist2(MAKE_FIX(2), MAKE_FIX(3))));

  /* malformed inputs must fail cleanly */
  const char *bad[] = {"",      "{",   "[1,",     "tru",       "01",    "-",
                       "1.",    ".5",  "\"\\x\"", "\"\\u12\"", "[1 2]", "{1:2}",
                       "nulll", "[]]", "\"\xc3",  "{\"a\"}",   NULL};
  for (int i = 0; bad[i]; i++) {
    size_t pos = 0;
    exp_t *d = js_decode(bad[i], strlen(bad[i]), &pos, 0);
    int trailing = 0;
    if (d) {
      js_skip_ws(bad[i], strlen(bad[i]), &pos);
      trailing = (pos != strlen(bad[i]));
      unrefexp(d);
    }
    CHECK(!d || trailing, "accepted malformed: %s", bad[i]);
  }
}

/* Random-buffer fuzz: ASCII-heavy garbage at the decoder. */
static void fuzz_decode(int iters, int maxlen) {
  unsigned int seed = 12345;
  char buf[512];
  const char alphabet[] = "{}[]\",:.0123456789-+eEtrufalsn\\u \t\n\xc3\xa9\x01";
  for (int it = 0; it < iters; it++) {
    int n = (int)(rand_r(&seed) % (unsigned)maxlen);
    for (int i = 0; i < n; i++)
      buf[i] = alphabet[rand_r(&seed) % (sizeof alphabet - 1)];
    size_t pos = 0;
    exp_t *d = js_decode(buf, (size_t)n, &pos, 0);
    if (d)
      unrefexp(d);
  }
}

int main(int argc, char *argv[]) {
  int iters = (argc > 1) ? atoi(argv[1]) : 300000;
  init_singletons();
  printf("=== json round-trip unit tests ===\n");
  test_roundtrip();
  printf("=== json decoder fuzz (%d random buffers) ===\n", iters);
  fuzz_decode(iters, 256);
  printf("json_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
#endif /* JSON_LIBFUZZER */
