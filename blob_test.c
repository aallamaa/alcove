/* blob_test.c — C-level unit + fuzz tests for the blob (EXP_BLOB) primitives in
 * blob.h: make_blob / blob_len / blob_bytes.
 *
 * Blobs are the interpreter's binary-safe byte container, so the property that
 * matters most — and that test.alc cannot exercise, since Lisp strings can't
 * carry embedded NULs — is exact byte preservation including zero bytes and the
 * full 0..255 range. Fuzzed under ASan/UBSan to catch any OOB in the
 * single-allocation flex-array layout.
 *
 * Compiles alcove.c into this TU. Build/run:  make blob-test
 */
#define main alcove_real_main
#include "alcove.c"
#undef main
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static void test_basic(void) {
  exp_t *b = make_blob("hello", 5);
  CHECK(isblob(b), "make_blob -> EXP_BLOB");
  CHECK(blob_len(b) == 5, "len 5 (got %zu)", blob_len(b));
  CHECK(memcmp(blob_bytes(b), "hello", 5) == 0, "bytes preserved");
  unrefexp(b);

  exp_t *e = make_blob("", 0);
  CHECK(isblob(e) && blob_len(e) == 0, "empty blob");
  unrefexp(e);

  /* embedded NULs + full byte range — the binary-safety property */
  unsigned char raw[256];
  for (int i = 0; i < 256; i++)
    raw[i] = (unsigned char)i;
  exp_t *all = make_blob((char *)raw, 256);
  CHECK(blob_len(all) == 256, "256-byte blob length");
  CHECK(memcmp(blob_bytes(all), raw, 256) == 0,
        "all 256 byte values (incl. NUL) preserved");
  /* a NUL in the middle must not truncate */
  const char nulmid[] = {'a', 0, 'b', 0, 'c'};
  exp_t *nb = make_blob(nulmid, 5);
  CHECK(blob_len(nb) == 5 && memcmp(blob_bytes(nb), nulmid, 5) == 0,
        "embedded NULs preserved (no truncation)");
  unrefexp(all);
  unrefexp(nb);
}

/* ------------------------------ fuzzing ----------------------------------- */
static uint64_t g_seed = 0x6c62face5eedULL;
static uint64_t xs(void) {
  uint64_t x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return g_seed = x;
}

/* Random-length, random-byte blobs must round-trip their exact bytes; ASan
   verifies the flex-array alloc has no OOB at either end. */
static void fuzz(int iters) {
  int mism = 0;
  for (int i = 0; i < iters; i++) {
    size_t len = (size_t)(xs() % 4096);
    unsigned char *src = (unsigned char *)malloc(len ? len : 1);
    for (size_t j = 0; j < len; j++)
      src[j] = (unsigned char)(xs() & 0xff);
    exp_t *b = make_blob((char *)src, len);
    if (blob_len(b) != len || (len && memcmp(blob_bytes(b), src, len) != 0))
      mism++;
    unrefexp(b);
    free(src);
  }
  CHECK(mism == 0, "fuzz: %d byte-preservation mismatches", mism);
}

int main(int argc, char *argv[]) {
  int iters = (argc > 1) ? atoi(argv[1]) : 200000;
  init_singletons();
  printf("=== blob unit tests ===\n");
  test_basic();
  printf("=== blob fuzz (%d random blobs) ===\n", iters);
  fuzz(iters);
  printf("unit+fuzz: %d passed, %d failed\n", g_pass, g_fail);
  if (g_fail) {
    printf(">>> BLOB TESTS FAILED (%d)\n", g_fail);
    return 1;
  }
  printf(">>> ALL BLOB TESTS PASSED\n");
  return 0;
}
