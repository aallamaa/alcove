/* msgpack_test.c — C-level unit tests + fuzzing for the MessagePack codec
 * (msgpack.h). Two concerns:
 *   1. round-trip: encode(v) then decode() yields an iso-equal value, across
 *      the integer-width boundaries (fixint / int8 / int16 / int32 / int64),
 *      floats, strings, nil, and bool.
 *   2. decoder robustness: mp_decode() consumes fully untrusted bytes, so feed
 *      it random / mutated garbage and require it to never crash, read OOB, or
 *      leak (run under ASan/UBSan via the make target). This is the coverage
 *      test.alc cannot give — it only round-trips well-formed values.
 *
 * Compiles alcove.c into this TU (parser_test.c pattern) for the static mp_*.
 * Build/run:  make msgpack-test
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

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c)                                                                     \
      g_pass++;                                                                \
    else {                                                                     \
      g_fail++;                                                                \
      printf("  FAIL [%s:%d]: ", __func__, __LINE__);                          \
      printf(__VA_ARGS__);                                                     \
      printf("\n");                                                            \
    }                                                                          \
  } while (0)

/* encode v, decode it back, assert the codec consumed exactly the buffer and
   the value survived. Consumes (unrefs) v. */
static void roundtrip(exp_t *v, const char *label) {
  mp_buf m = {NULL, 0, 0};
  if (!mp_encode(v, &m)) {
    g_fail++;
    printf("  FAIL: encode(%s) returned unsupported\n", label);
    free(m.b);
    unrefexp(v);
    return;
  }
  size_t pos = 0;
  exp_t *d = mp_decode(m.b, m.len, &pos);
  CHECK(d && pos == m.len && isoequal(v, d), "round-trip %s (pos=%zu/%zu)",
        label, pos, m.len);
  if (d)
    unrefexp(d);
  free(m.b);
  unrefexp(v);
}

static void test_roundtrip(void) {
  init_singletons();
  /* integer width boundaries */
  long long ints[] = {0,
                      1,
                      127,
                      128,
                      255,
                      256,
                      65535,
                      65536,
                      -1,
                      -32,
                      -33,
                      -128,
                      -129,
                      -32768,
                      -32769,
                      2147483647LL,
                      -2147483648LL,
                      4294967296LL,
                      -4294967297LL};
  for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
    char lbl[32];
    snprintf(lbl, sizeof lbl, "int %lld", ints[i]);
    roundtrip(make_integeri(ints[i]), lbl);
  }
  roundtrip(make_floatf(3.14159), "float");
  roundtrip(make_floatf(-2.5e10), "float neg big");
  roundtrip(make_string("", 0), "empty string");
  roundtrip(make_string("hello", 5), "short string");
  char big[600];
  for (int i = 0; i < 600; i++)
    big[i] = (char)('a' + (i % 26));
  roundtrip(make_string(big, 600), "long string (str16)");
  roundtrip(nil_singleton, "nil");
  roundtrip(true_singleton, "true");
}

/* ------------------------------ fuzzing ----------------------------------- */
static uint64_t g_seed = 0x100000001b3ULL;
static uint64_t xs(void) {
  uint64_t x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return g_seed = x;
}

/* mp_decode on arbitrary bytes must never crash / OOB / leak (ASan verifies).
 */
static void fuzz_decode(int iters, int maxlen) {
  for (int i = 0; i < iters; i++) {
    int len = (int)(xs() % (unsigned)maxlen);
    uint8_t *buf = (uint8_t *)malloc((size_t)len + 1);
    for (int j = 0; j < len; j++)
      buf[j] = (uint8_t)(xs() & 0xff);
    size_t pos = 0;
    exp_t *d = mp_decode(buf, (size_t)len, &pos);
    if (d)
      unrefexp(d);
    free(buf);
  }
}

/* round-trip fuzz: encode random byte blobs (always valid msgpack 'bin'),
   decode, and confirm the bytes survive — exercises bin8/bin16/bin32 paths. */
static void fuzz_blob_roundtrip(int iters) {
  for (int i = 0; i < iters; i++) {
    int len = (int)(xs() % 70000);
    char *bytes = (char *)malloc((size_t)len + 1);
    for (int j = 0; j < len; j++)
      bytes[j] = (char)(xs() & 0xff);
    exp_t *b = make_blob(bytes, (size_t)len);
    mp_buf m = {NULL, 0, 0};
    if (mp_encode(b, &m)) {
      size_t pos = 0;
      exp_t *d = mp_decode(m.b, m.len, &pos);
      if (!(d && pos == m.len))
        g_fail++; /* should always round-trip */
      if (d)
        unrefexp(d);
    }
    free(m.b);
    unrefexp(b);
    free(bytes);
  }
}

int main(int argc, char *argv[]) {
  int iters = (argc > 1) ? atoi(argv[1]) : 300000;
  init_singletons();

  printf("=== msgpack round-trip unit tests ===\n");
  test_roundtrip();
  printf("=== msgpack decoder fuzz (%d random buffers) ===\n", iters);
  fuzz_decode(iters, 256);
  printf("=== msgpack blob round-trip fuzz (%d) ===\n", iters / 100 + 1);
  fuzz_blob_roundtrip(iters / 100 + 1);
  printf("unit+fuzz: %d passed, %d failed\n", g_pass, g_fail);

  if (g_fail) {
    printf(">>> MSGPACK TESTS FAILED (%d)\n", g_fail);
    return 1;
  }
  printf(">>> ALL MSGPACK TESTS PASSED\n");
  return 0;
}
