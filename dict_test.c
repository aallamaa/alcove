/* dict_test.c — C-level unit tests + fuzzing for the dict_t hash table
 * (dict.h).
 *
 * The open-chaining hash table backs both Lisp hash-maps AND every
 * environment's binding store, so its insert/lookup/update/delete/rehash paths
 * are some of the hottest, most safety-critical code in the interpreter. This
 * drives the C API directly with string keys and integer-immediate values (no
 * value refcount to juggle) and checks it against a dense oracle, including the
 * load-factor rehash and hash-collision chains. Run under ASan/UBSan (the make
 * target).
 *
 * Compiles alcove.c into this TU (parser_test.c pattern) for the dict_t
 * internals. Build/run:  make dict-test
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

/* ---- value-level helpers (string keys, integer-immediate values) ----------
 */
static void dput(dict_t *d, const char *key, int v) {
  set_get_keyval_dict(d, (char *)key, MAKE_FIX(v));
}
/* set_get_keyval_dict with val==NULL returns the last node walked in the
   bucket, which is NOT necessarily a match — so confirm the key before trusting
   it. */
static int dget(dict_t *d, const char *key, int *out) {
  keyval_t *k = set_get_keyval_dict(d, (char *)key, NULL);
  if (k && strcmp(k->key, key) == 0) {
    *out = (int)FIX_VAL(k->val);
    return 1;
  }
  return 0;
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
  dict_t *d = create_dict();
  const int N = 2000; /* forces several load-factor rehashes */
  char key[32];
  for (int i = 0; i < N; i++) {
    snprintf(key, sizeof key, "key%d", i);
    dput(d, key, i * 7);
  }
  CHECK((int)d->ht[0].used == N, "used=%lu want %d",
        (unsigned long)d->ht[0].used, N);
  int ok = 1, out = 0;
  for (int i = 0; i < N; i++) {
    snprintf(key, sizeof key, "key%d", i);
    if (!dget(d, key, &out) || out != i * 7)
      ok = 0;
  }
  CHECK(ok, "all keys survive insertion + rehash with correct values");
  CHECK(!dget(d, "absent", &out), "absent key not found");
  destroy_dict(d);
}

static void test_update(void) {
  dict_t *d = create_dict();
  dput(d, "x", 1);
  unsigned used1 = d->ht[0].used;
  dput(d, "x", 2); /* update, not insert */
  int out = 0;
  CHECK(d->ht[0].used == used1, "update keeps used count");
  CHECK(dget(d, "x", &out) && out == 2, "update replaces value");
  destroy_dict(d);
}

static void test_delete(void) {
  dict_t *d = create_dict();
  const int N = 500;
  char key[32];
  for (int i = 0; i < N; i++) {
    snprintf(key, sizeof key, "k%d", i);
    dput(d, key, i);
  }
  for (int i = 0; i < N; i += 2) { /* delete evens */
    snprintf(key, sizeof key, "k%d", i);
    del_keyval_dict(d, key);
  }
  int ok = 1, out = 0;
  for (int i = 0; i < N; i++) {
    snprintf(key, sizeof key, "k%d", i);
    int present = dget(d, key, &out);
    if ((i % 2 == 0) && present)
      ok = 0;
    if ((i % 2 == 1) && (!present || out != i))
      ok = 0;
  }
  CHECK(ok, "evens deleted, odds intact");
  destroy_dict(d);
}

/* ------------------------------ fuzzing ----------------------------------- */
static uint64_t g_seed = 0x9e3779b97f4a7c15ULL;
static uint64_t xs(void) {
  uint64_t x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return g_seed = x;
}

/* Random put/delete over a fixed key space, checked against a dense oracle:
   get() must always agree, and the final table must match exactly. ASan checks
   the chain/rehash allocations and destroy_dict teardown. */
static void fuzz_against_oracle(int iters) {
  enum { K = 512 };
  int oracle[K];
  for (int i = 0; i < K; i++)
    oracle[i] = -1;
  dict_t *d = create_dict();
  char key[32];
  int mism = 0;
  for (int it = 0; it < iters; it++) {
    int k = (int)(xs() % K);
    snprintf(key, sizeof key, "k%d", k);
    if (oracle[k] == -1 || (xs() & 3)) {
      int v = (int)(xs() % 1000000);
      dput(d, key, v);
      oracle[k] = v;
    } else {
      del_keyval_dict(d, key);
      oracle[k] = -1;
    }
    int q = (int)(xs() % K), out = 0;
    snprintf(key, sizeof key, "k%d", q);
    int present = dget(d, key, &out);
    if (present != (oracle[q] != -1) || (present && out != oracle[q]))
      mism++;
  }
  for (int i = 0; i < K; i++) { /* final full reconciliation */
    snprintf(key, sizeof key, "k%d", i);
    int out = 0, present = dget(d, key, &out);
    if (present != (oracle[i] != -1) || (present && out != oracle[i]))
      mism++;
  }
  destroy_dict(d);
  CHECK(mism == 0, "fuzz: %d mismatches vs oracle", mism);
}

int main(int argc, char *argv[]) {
  int iters = (argc > 1) ? atoi(argv[1]) : 300000;
  init_singletons();

  printf("=== dict_t unit tests ===\n");
  test_basic();
  test_update();
  test_delete();
  printf("=== dict_t fuzz (%d ops vs oracle) ===\n", iters);
  fuzz_against_oracle(iters);
  printf("unit+fuzz: %d passed, %d failed\n", g_pass, g_fail);

  if (g_fail) {
    printf(">>> DICT TESTS FAILED (%d)\n", g_fail);
    return 1;
  }
  printf(">>> ALL DICT TESTS PASSED\n");
  return 0;
}
