/* hamt_test.c — C-level unit tests + fuzzing for the HAMT (hamt.h).
 *
 * Drives the persistent-map node API (hamt_node_assoc / _dissoc / _get,
 * hamt_wrap, hamt_free-via-unrefexp) directly, with integer keys/values —
 * which are tagged immediates, so there is no key/value refcount bookkeeping
 * to get wrong; only the EXP_HAMT wrappers are freed. Checks the map against a
 * dense oracle and asserts the persistence (structural-sharing) invariant.
 * Run under ASan/UBSan (the make target) to catch leaks/use-after-free.
 *
 * Compiles alcove.c into this TU (same trick as parser_test.c) for access to
 * the hamt_* internals.
 *
 * Build/run:  make hamt-test
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

/* ---- value-level helpers (integer keys/values; old map NOT freed here) ----
 */
static exp_t *hassoc(exp_t *m, int k, int v) {
  hamt_t *h = (hamt_t *)m->ptr;
  int added = 0;
  hamt_node *nr = hamt_node_assoc(h->root, MAKE_FIX(k), MAKE_FIX(v),
                                  hamt_hashkey(MAKE_FIX(k)), 0, &added);
  return hamt_wrap(nr, h->count + added);
}
static exp_t *hdissoc(exp_t *m, int k) {
  hamt_t *h = (hamt_t *)m->ptr;
  int removed = 0;
  hamt_node *nr = hamt_node_dissoc(h->root, MAKE_FIX(k),
                                   hamt_hashkey(MAKE_FIX(k)), 0, &removed);
  return hamt_wrap(nr, h->count - removed);
}
static int hget(exp_t *m, int k, int *out) {
  hamt_t *h = (hamt_t *)m->ptr;
  exp_t *v = hamt_node_get(h->root, MAKE_FIX(k), hamt_hashkey(MAKE_FIX(k)), 0);
  if (!v)
    return 0;
  *out = (int)FIX_VAL(v);
  return 1;
}
static int hcount(exp_t *m) { return (int)((hamt_t *)m->ptr)->count; }

/* ---------------------------- unit tests ---------------------------------- */
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
  const int N = 500; /* enough to force multi-level nodes */
  exp_t *m = hamt_wrap(NULL, 0);
  for (int i = 0; i < N; i++) {
    exp_t *m2 = hassoc(m, i, i * 10);
    unrefexp(m);
    m = m2;
  }
  CHECK(hcount(m) == N, "count=%d want %d", hcount(m), N);
  int ok = 1, out = 0;
  for (int i = 0; i < N; i++)
    if (!hget(m, i, &out) || out != i * 10)
      ok = 0;
  CHECK(ok, "all keys present with correct values");
  CHECK(!hget(m, N + 7, &out), "absent key returns 0");
  /* overwrite an existing key: count unchanged, value updated */
  exp_t *m2 = hassoc(m, 3, 999);
  unrefexp(m);
  m = m2;
  CHECK(hcount(m) == N, "overwrite keeps count");
  CHECK(hget(m, 3, &out) && out == 999, "overwrite updates value");
  unrefexp(m);
}

static void test_persistence(void) {
  exp_t *m = hamt_wrap(NULL, 0);
  for (int i = 0; i < 5; i++) {
    exp_t *n = hassoc(m, i, i);
    unrefexp(m);
    m = n;
  }
  exp_t *snap = refexp(m); /* snapshot the 5-entry map */
  for (int i = 5; i < 20; i++) {
    exp_t *n = hassoc(m, i, i);
    unrefexp(m);
    m = n;
  }
  int out = 0;
  CHECK(hcount(snap) == 5, "snapshot count frozen at 5 (got %d)", hcount(snap));
  CHECK(!hget(snap, 12, &out), "snapshot does not see later insert");
  CHECK(hget(m, 12, &out) && out == 12, "current map sees the insert");
  unrefexp(snap);
  unrefexp(m);
}

static void test_dissoc(void) {
  const int N = 300;
  exp_t *m = hamt_wrap(NULL, 0);
  for (int i = 0; i < N; i++) {
    exp_t *n = hassoc(m, i, i);
    unrefexp(m);
    m = n;
  }
  for (int i = 0; i < N; i += 2) { /* drop evens */
    exp_t *n = hdissoc(m, i);
    unrefexp(m);
    m = n;
  }
  CHECK(hcount(m) == N / 2, "count after dropping evens = %d", hcount(m));
  int ok = 1, out = 0;
  for (int i = 0; i < N; i++) {
    int present = hget(m, i, &out);
    if ((i % 2 == 0) && present)
      ok = 0;
    if ((i % 2 == 1) && (!present || out != i))
      ok = 0;
  }
  CHECK(ok, "evens gone, odds intact");
  /* dissoc of an absent key is a no-op */
  exp_t *n = hdissoc(m, 99999);
  CHECK(hcount(n) == hcount(m), "dissoc absent key keeps count");
  unrefexp(n);
  unrefexp(m);
}

/* ------------------------------ fuzzing ----------------------------------- */
static uint64_t g_seed = 0xcbf29ce484222325ULL;
static uint64_t xs(void) {
  uint64_t x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return g_seed = x;
}

/* Random assoc/dissoc against a dense oracle; verify get() always agrees and
   the final map matches the oracle exactly. ASan checks the node refcounting.
 */
static void fuzz_against_oracle(int iters) {
  enum { K = 256 };
  int oracle[K];
  for (int i = 0; i < K; i++)
    oracle[i] = -1; /* -1 == absent */
  exp_t *m = hamt_wrap(NULL, 0);
  int live = 0, mism = 0;
  for (int it = 0; it < iters; it++) {
    int k = (int)(xs() % K);
    if (oracle[k] == -1 || (xs() & 3)) { /* mostly assoc */
      int v = (int)(xs() % 1000000);
      exp_t *n = hassoc(m, k, v);
      unrefexp(m);
      m = n;
      if (oracle[k] == -1)
        live++;
      oracle[k] = v;
    } else { /* sometimes dissoc */
      exp_t *n = hdissoc(m, k);
      unrefexp(m);
      m = n;
      oracle[k] = -1;
      live--;
    }
    /* spot-check a random key against the oracle */
    int q = (int)(xs() % K), out = 0, present = hget(m, q, &out);
    if (present != (oracle[q] != -1) || (present && out != oracle[q]))
      mism++;
  }
  if (hcount(m) != live)
    mism++;
  for (int i = 0; i < K; i++) { /* full final reconciliation */
    int out = 0, present = hget(m, i, &out);
    if (present != (oracle[i] != -1) || (present && out != oracle[i]))
      mism++;
  }
  unrefexp(m);
  CHECK(mism == 0, "fuzz: %d mismatches vs oracle", mism);
}

int main(int argc, char *argv[]) {
  int iters = (argc > 1) ? atoi(argv[1]) : 200000;
  init_singletons();

  printf("=== HAMT unit tests ===\n");
  test_basic();
  test_persistence();
  test_dissoc();
  printf("=== HAMT fuzz (%d ops vs oracle) ===\n", iters);
  fuzz_against_oracle(iters);
  printf("unit+fuzz: %d passed, %d failed\n", g_pass, g_fail);

  if (g_fail) {
    printf(">>> HAMT TESTS FAILED (%d)\n", g_fail);
    return 1;
  }
  printf(">>> ALL HAMT TESTS PASSED\n");
  return 0;
}
