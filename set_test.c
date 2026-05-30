/* set_test.c — C-level unit + fuzz tests for the hash-set (set.h).
 *
 * The subtle, safety-critical property of a set is its canonical key encoding
 * (set_key_for_value): values that PRINT the same but have different TYPES —
 * the integer 2, the string "2", the keyword :2, a blob, a float, a char — must
 * map to DISTINCT keys, or they would alias in the set (the exact bug fixed
 * earlier when set lookup used the untyped alc_key_to_cstr). This pins that,
 * plus membership and an integer fuzz against an oracle. Under ASan/UBSan.
 *
 * Compiles alcove.c into this TU. Build/run:  make set-test
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

static void test_key_distinctness(void) {
  /* six values that all render as "2" but differ in type */
  exp_t *vals[6];
  vals[0] = MAKE_FIX(2);
  vals[1] = make_string("2", 1);
  vals[2] = make_symbol(":2", 2);
  vals[3] = make_blob("2", 1);
  vals[4] = make_floatf(2.0);
  vals[5] = make_char('2');
  char *keys[6];
  for (int i = 0; i < 6; i++)
    keys[i] = set_key_for_value(vals[i]);
  int distinct = 1;
  for (int i = 0; i < 6; i++)
    for (int j = i + 1; j < 6; j++)
      if (strcmp(keys[i], keys[j]) == 0)
        distinct = 0;
  CHECK(distinct, "int/str/keyword/blob/float/char '2' -> distinct set keys");

  char *again = set_key_for_value(MAKE_FIX(2));
  CHECK(strcmp(keys[0], again) == 0, "same value -> same key (deterministic)");
  free(again);

  for (int i = 0; i < 6; i++) {
    free(keys[i]);
    unrefexp(vals[i]); /* immediates no-op; string/sym/blob/float freed */
  }
}

static void test_membership(void) {
  exp_t *s = make_set_exp();
  dict_t *d = (dict_t *)s->ptr;
  exp_t *str = make_string("hi", 2);
  set_insert_value(d, MAKE_FIX(10)); /* clones the value into the set */
  set_insert_value(d, str);

  char *k10 = set_key_for_value(MAKE_FIX(10));
  char *khi = set_key_for_value(str);
  char *k99 = set_key_for_value(MAKE_FIX(99));
  exp_t *str10 = make_string("10", 2);
  char *ks10 = set_key_for_value(str10);

  CHECK(set_contains_key(s, k10), "inserted int 10 present");
  CHECK(set_contains_key(s, khi), "inserted string present");
  CHECK(!set_contains_key(s, k99), "absent int 99 not present");
  CHECK(!set_contains_key(s, ks10),
        "string \"10\" absent even though int 10 is present (no type alias)");

  /* idempotent insert: re-adding 10 doesn't grow the set */
  unsigned used = d->ht[0].used;
  set_insert_value(d, MAKE_FIX(10));
  CHECK(d->ht[0].used == used, "re-insert of existing member is idempotent");

  free(k10);
  free(khi);
  free(k99);
  free(ks10);
  unrefexp(str);
  unrefexp(str10);
  unrefexp(s);
}

/* ------------------------------ fuzzing ----------------------------------- */
static uint64_t g_seed = 0xa5a5f00ddeadbeefULL;
static uint64_t xs(void) {
  uint64_t x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return g_seed = x;
}

/* Insert random integer members; membership must match a dense oracle. */
static void fuzz_int_membership(int iters) {
  enum { K = 512 };
  unsigned char present[K] = {0};
  exp_t *s = make_set_exp();
  dict_t *d = (dict_t *)s->ptr;
  int mism = 0;
  for (int i = 0; i < iters; i++) {
    int k = (int)(xs() % K);
    set_insert_value(d, MAKE_FIX(k));
    present[k] = 1;
    int q = (int)(xs() % K);
    char *kq = set_key_for_value(MAKE_FIX(q));
    int has = set_contains_key(s, kq);
    free(kq);
    if (has != (present[q] != 0))
      mism++;
  }
  unrefexp(s);
  CHECK(mism == 0, "fuzz: %d membership mismatches vs oracle", mism);
}

int main(int argc, char *argv[]) {
  int iters = (argc > 1) ? atoi(argv[1]) : 200000;
  init_singletons();
  printf("=== set unit tests ===\n");
  test_key_distinctness();
  test_membership();
  printf("=== set fuzz (%d int inserts) ===\n", iters);
  fuzz_int_membership(iters);
  printf("unit+fuzz: %d passed, %d failed\n", g_pass, g_fail);
  if (g_fail) {
    printf(">>> SET TESTS FAILED (%d)\n", g_fail);
    return 1;
  }
  printf(">>> ALL SET TESTS PASSED\n");
  return 0;
}
