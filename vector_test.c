/* vector_test.c — C-level unit + fuzz tests for the dense vector (vector.h).
 *
 * Vectors carry a kind tag (I64 / F64 / GEN) with unboxed int/double fast paths
 * and a boxed fallback; the tricky, safety-critical bits are (a) kind inference
 * at construction, (b) get/set round-trips per kind, (c) vec_read_double across
 * kinds, and (d) auto-promotion to GEN on a type-mismatched write (which must
 * preserve the cells already stored). This exercises all four directly under
 * ASan/UBSan — paths the value-level test.alc reaches only indirectly.
 *
 * Compiles alcove.c into this TU. Build/run:  make vector-test
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

static void test_i64(void) {
  exp_t *v = make_vector(8, MAKE_FIX(7));
  CHECK(vec_kind(v) == VEC_KIND_I64, "int fill -> I64 kind");
  CHECK(vec_len(v) == 8, "len 8 (got %lld)", (long long)vec_len(v));
  int ok = 1;
  for (int i = 0; i < 8; i++) {
    exp_t *g = vec_get_boxed(v, i);
    if (!isnumber(g) || FIX_VAL(g) != 7)
      ok = 0;
    unrefexp(g);
  }
  CHECK(ok, "I64 fill readable as 7 across all cells");
  vec_set_boxed(v, 3, MAKE_FIX(99));
  exp_t *g = vec_get_boxed(v, 3);
  CHECK(isnumber(g) && FIX_VAL(g) == 99, "I64 set/get round-trip");
  unrefexp(g);
  int err = 0;
  CHECK(vec_read_double(v, 3, &err) == 99.0 && !err,
        "read_double of an int cell casts to double");
  unrefexp(v);
}

static void test_f64(void) {
  exp_t *v = make_vector(4, make_floatf(1.5));
  CHECK(vec_kind(v) == VEC_KIND_F64, "float fill -> F64 kind");
  int err = 0;
  CHECK(vec_read_double(v, 2, &err) == 1.5 && !err, "F64 read_double");
  exp_t *g = vec_get_boxed(v, 0);
  CHECK(isfloat(g) && g->f == 1.5, "F64 cell boxes back to a float");
  unrefexp(g);
  CHECK(vec_write_double(v, 1, 2.5), "F64 write_double ok");
  CHECK(vec_read_double(v, 1, &err) == 2.5 && !err,
        "F64 write/read round-trip");
  unrefexp(v);
}

static void test_gen_and_promote(void) {
  exp_t *v = make_vector(4, MAKE_FIX(7)); /* starts I64 */
  exp_t *s = make_string("z", 1);
  CHECK(vec_set_boxed(v, 0, s), "type-mismatched set returns ok");
  CHECK(vec_kind(v) == VEC_KIND_GEN, "mismatched write promotes I64 -> GEN");
  exp_t *g0 = vec_get_boxed(v, 0);
  CHECK(is_ptr(g0) && g0->type == EXP_STRING && strcmp(exp_text(g0), "z") == 0,
        "promoted cell holds the string");
  unrefexp(g0);
  exp_t *g1 = vec_get_boxed(v, 1);
  CHECK(isnumber(g1) && FIX_VAL(g1) == 7,
        "int cells preserved across I64->GEN promotion");
  unrefexp(g1);
  /* NOTE: vec_set_boxed took ownership of `s` (the cell now holds our ref), so
     we must NOT unref it here — unrefexp(v) frees the vector and its cells. */
  unrefexp(v);

  exp_t *gv = make_vector(3, make_string("x", 1));
  CHECK(vec_kind(gv) == VEC_KIND_GEN, "string fill -> GEN kind");
  exp_t *g = vec_get_boxed(gv, 2);
  CHECK(is_ptr(g) && g->type == EXP_STRING, "GEN cell holds the fill value");
  unrefexp(g);
  unrefexp(gv);
}

/* ------------------------------ fuzzing ----------------------------------- */
static uint64_t g_seed = 0x1234567089abcdefULL;
static uint64_t xs(void) {
  uint64_t x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return g_seed = x;
}

/* Random sets into an I64 vector, checked against an oracle array. Some writes
   are floats (forcing GEN promotion); the oracle tracks the expected value and
   we compare via vec_read_double, which reads uniformly across kinds. */
static void fuzz(int iters) {
  enum { N = 64 };
  double oracle[N];
  exp_t *v = make_vector(N, MAKE_FIX(0));
  for (int i = 0; i < N; i++)
    oracle[i] = 0.0;
  int mism = 0;
  for (int it = 0; it < iters; it++) {
    int i = (int)(xs() % N);
    if (xs() & 1) {
      int iv = (int)(xs() % 100000);
      vec_set_boxed(v, i, MAKE_FIX(iv));
      oracle[i] = (double)iv;
    } else {
      double dv = (double)(int)(xs() % 100000) + 0.5;
      vec_set_boxed(v, i, make_floatf((expfloat)dv));
      oracle[i] = dv;
    }
    int q = (int)(xs() % N), err = 0;
    double got = vec_read_double(v, q, &err);
    if (err || got != oracle[q])
      mism++;
  }
  unrefexp(v);
  CHECK(mism == 0, "fuzz: %d read_double mismatches vs oracle", mism);
}

int main(int argc, char *argv[]) {
  int iters = (argc > 1) ? atoi(argv[1]) : 200000;
  init_singletons();
  printf("=== vector unit tests ===\n");
  test_i64();
  test_f64();
  test_gen_and_promote();
  printf("=== vector fuzz (%d random sets) ===\n", iters);
  fuzz(iters);
  printf("unit+fuzz: %d passed, %d failed\n", g_pass, g_fail);
  if (g_fail) {
    printf(">>> VECTOR TESTS FAILED (%d)\n", g_fail);
    return 1;
  }
  printf(">>> ALL VECTOR TESTS PASSED\n");
  return 0;
}
