/* utf8_test.c — standalone unit tests + fuzzing for utf8.h.
 *
 * Because utf8.h is libc-only, this test includes ONLY utf8.h — no alcove.c.
 * It is the most isolated test in the tree: a tiny, fast TU. Covers
 * encode/decode round-trips across the 1..4-byte ranges, codepoint counting
 * and indexing, strict validation, and fuzzes utf8_valid / utf8_decode_at on
 * random bytes (must never read OOB — run under ASan via the make target).
 *
 * Build/run:  make utf8-test
 */
#include "utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void rt(uint32_t cp, int want_n) {
  char buf[8] = {0};
  int n = utf8_encode(cp, buf);
  size_t off = 0;
  uint32_t d = utf8_decode_at(buf, &off);
  CHECK(n == want_n && d == cp && (int)off == n,
        "round-trip U+%06X: n=%d(want %d) decoded=U+%06X off=%zu", cp, n,
        want_n, d, off);
}

static void test_roundtrip(void) {
  rt('A', 1);
  rt(0x7F, 1);     /* last 1-byte */
  rt(0x80, 2);     /* first 2-byte */
  rt(0xE9, 2);     /* é */
  rt(0x7FF, 2);    /* last 2-byte */
  rt(0x800, 3);    /* first 3-byte */
  rt(0x4E16, 3);   /* 世 */
  rt(0xFFFF, 3);   /* last 3-byte */
  rt(0x10000, 4);  /* first 4-byte */
  rt(0x1F600, 4);  /* 😀 */
  rt(0x10FFFF, 4); /* last valid codepoint */
}

static void test_count_index(void) {
  /* "Aé世😀" = 1+1+1+1 = 4 codepoints, 1+2+3+4 = 10 bytes */
  const char *s = "A\xC3\xA9\xE4\xB8\x96\xF0\x9F\x98\x80";
  CHECK(strlen(s) == 10, "byte length 10");
  CHECK(utf8_strlen(s) == 4, "codepoint count 4 (got %lld)",
        (long long)utf8_strlen(s));
  uint32_t cp = 0;
  CHECK(utf8_index(s, 0, &cp) && cp == 'A', "index 0 = A");
  CHECK(utf8_index(s, 1, &cp) && cp == 0xE9, "index 1 = é");
  CHECK(utf8_index(s, 2, &cp) && cp == 0x4E16, "index 2 = 世");
  CHECK(utf8_index(s, 3, &cp) && cp == 0x1F600, "index 3 = 😀");
  CHECK(!utf8_index(s, 4, &cp), "index 4 out of range");
}

static void test_valid(void) {
  size_t bad = 0;
  CHECK(utf8_valid("hello", 5, &bad), "ascii valid");
  CHECK(utf8_valid("A\xC3\xA9", 3, &bad), "é valid");
  /* stray continuation byte */
  CHECK(!utf8_valid("\x80", 1, &bad), "lone continuation invalid");
  /* truncated 2-byte sequence */
  CHECK(!utf8_valid("\xC3", 1, &bad), "truncated 2-byte invalid");
  /* truncated 4-byte */
  CHECK(!utf8_valid("\xF0\x9F\x98", 3, &bad), "truncated 4-byte invalid");
  /* overlong / invalid lead 0xFF */
  CHECK(!utf8_valid("\xFF", 1, &bad), "0xFF invalid lead");
}

/* ------------------------------ fuzzing ----------------------------------- */
static uint64_t g_seed = 0x2545f4914f6cdd1dULL;
static uint64_t xs(void) {
  uint64_t x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return g_seed = x;
}

/* Random bytes through utf8_valid and a bounded utf8_decode_at walk; neither
   may read past the buffer (ASan enforces). If utf8_valid says valid, the
   decode walk must land exactly on the end. */
static void fuzz(int iters) {
  for (int i = 0; i < iters; i++) {
    int len = (int)(xs() % 32);
    char buf[33];
    for (int j = 0; j < len; j++)
      buf[j] = (char)(xs() & 0xff);
    buf[len] = 0; /* NUL-terminate for the strlen-based helpers */
    size_t bad = 0;
    int ok = utf8_valid(buf, (size_t)len, &bad);
    if (ok) {
      /* walk via decode_at; must consume exactly len bytes */
      size_t off = 0;
      int guard = 0;
      while (off < (size_t)len && guard++ < 64)
        utf8_decode_at(buf, &off);
      if (off != (size_t)len)
        g_fail++;
    } else {
      CHECK(bad <= (size_t)len, "valid() reports bad offset in range");
    }
    /* index helpers on the NUL-terminated buffer must not run off the end */
    (void)utf8_strlen(buf);
    uint32_t cp = 0;
    (void)utf8_index(buf, (int64_t)(xs() % 40), &cp);
  }
}

int main(int argc, char *argv[]) {
  int iters = (argc > 1) ? atoi(argv[1]) : 500000;
  printf("=== utf8 unit tests ===\n");
  test_roundtrip();
  test_count_index();
  test_valid();
  printf("=== utf8 fuzz (%d random buffers) ===\n", iters);
  fuzz(iters);
  printf("unit+fuzz: %d passed, %d failed\n", g_pass, g_fail);
  if (g_fail) {
    printf(">>> UTF8 TESTS FAILED (%d)\n", g_fail);
    return 1;
  }
  printf(">>> ALL UTF8 TESTS PASSED\n");
  return 0;
}
