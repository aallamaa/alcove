/* adr_test.c — C-level unit tests + fuzzing for the Adder transpiler (adr.h).
 *
 * adr.h is a self-contained string->string transpiler: it turns Adder's
 * whitespace/`:`-block surface syntax into ordinary alcove s-expression text
 * via the single entry point `als_to_sexpr(const char *src)`. It is the first
 * thing every Adder REPL line / file / -e string flows through, so it must
 * never crash, read out of bounds, or leak on ANY input.
 *
 * Because adr.h needs no alcove headers, we just #include it directly — no
 * `#define main` trick. These tests pin the transpilation of known forms
 * (atoms, lists, call-sugar, the #[ #{ #b" { } dispatch/brace literals, the
 * quasiquote/unquote meta-syntax, `# ` comments, and `:`-blocks) and then
 * hammer als_to_sexpr with random / mutation garbage.
 *
 * Build/run:
 *   make adr-test                 # unit tests + bounded random fuzz (ASan)
 *   make adr-fuzz                 # coverage-guided libFuzzer (clang)
 */
#include "adr.h"
#include <stdint.h>
#include <stdio.h>

/* ---- run als_to_sexpr on a NUL-terminated copy, freeing the result. The
   fuzz workhorse: any non-crash, non-leak outcome is a pass. */
static void transpile_bytes(const char *src, size_t len) {
  char *z = (char *)malloc(len + 1);
  if (!z)
    return;
  memcpy(z, src, len);
  z[len] = 0;
  char *out = als_to_sexpr(z);
  free(out);
  free(z);
}

/* ---------------------------- unit tests ---------------------------------- */
static int g_pass = 0, g_fail = 0;

/* Transpile `src`, strip a single trailing newline, compare to `expected`. */
static int eq_trans(const char *src, const char *expected) {
  char *got = als_to_sexpr(src);
  if (!got)
    return 0;
  size_t n = strlen(got);
  if (n && got[n - 1] == '\n')
    got[n - 1] = 0;
  int ok = strcmp(got, expected) == 0;
  if (!ok)
    printf("  FAIL: [%s] => [%s], expected [%s]\n", src, got, expected);
  free(got);
  return ok;
}

#define CHECK_EQ(src, exp)                                                     \
  do {                                                                         \
    if (eq_trans((src), (exp)))                                                \
      g_pass++;                                                                \
    else                                                                       \
      g_fail++;                                                                \
  } while (0)

static void test_atoms(void) {
  CHECK_EQ("foo", "foo");
  CHECK_EQ("42", "42");
  CHECK_EQ("-3.14", "-3.14");
  CHECK_EQ("\"hi there\"", "\"hi there\"");
  CHECK_EQ("true", "t");    /* literal mapping */
  CHECK_EQ("false", "nil"); /* literal mapping */
  CHECK_EQ("#\\A", "#\\A"); /* char literal survives */
}

static void test_lists_and_callsugar(void) {
  CHECK_EQ("(a b c)", "(a b c)");
  CHECK_EQ("a b c", "(a b c)");    /* many forms on a line -> a call */
  CHECK_EQ("f(a b)", "(f (a b))"); /* call sugar */
  CHECK_EQ("g()", "(g)");          /* empty-arg call sugar */
  CHECK_EQ("(f (g x) y)", "(f (g x) y)");
}

static void test_brace_literals(void) {
  CHECK_EQ("#[1 2 3]", "(vector 1 2 3)");
  CHECK_EQ("#{1 2 3}", "(hash-set 1 2 3)");
  CHECK_EQ("#b\"x\"", "(string->blob \"x\")");
  /* hash-map literal — commas are entry separators (whitespace) here */
  CHECK_EQ("{:a 1 :b 2}", "(hash-map :a 1 :b 2)");
  CHECK_EQ("{:a 1, :b 2}", "(hash-map :a 1 :b 2)");
  CHECK_EQ("{}", "(hash-map)");
  CHECK_EQ("{:k (* 2 3)}", "(hash-map :k (* 2 3))");
  /* set with commas is lenient too (printer uses spaces, but accept commas) */
  CHECK_EQ("#{1, 2, 3}", "(hash-set 1 2 3)");
}

/* The meta-syntax must survive untouched: a comma OUTSIDE a brace container is
   still the unquote reader macro; only inside {…}/#{…} is it a separator. */
static void test_meta_syntax(void) {
  CHECK_EQ("'x", "(quote x)");
  CHECK_EQ("`(a b)", "(quasiquote (a b))");
  CHECK_EQ("`(a ,b ,@c)", "(quasiquote (a (unquote b) (unquote-splicing c)))");
  CHECK_EQ("(a , b)", "(a (unquote b))"); /* comma outside braces = unquote */
  CHECK_EQ(",x", "(unquote x)");
  CHECK_EQ(",@x", "(unquote-splicing x)");
}

static void test_comments(void) {
  CHECK_EQ("x  # trailing comment", "x");
  CHECK_EQ("# whole-line comment", "");
  CHECK_EQ("(a b)  # tail", "(a b)");
  /* a `#` glued to a token is a literal, not a comment */
  CHECK_EQ("#\\#", "#\\#");
  CHECK_EQ("#{1 2}", "(hash-set 1 2)");
}

static void test_blocks(void) {
  CHECK_EQ("def sq (x):\n  (* x x)", "(def sq (x) (* x x))");
  CHECK_EQ("if (> x 0):\n  pr \"pos\"\n  pr \"done\"",
           "(if (> x 0) (pr \"pos\") (pr \"done\"))");
}

static void test_edges(void) {
  CHECK_EQ("", "");
  CHECK_EQ("   ", "");
  CHECK_EQ("\t\n  \n", "");
}

/* ------------------------------ fuzzing ----------------------------------- */
/* Deterministic xorshift64 PRNG — reproducible across machines (no time/rand).
 */
static uint64_t g_seed = 0x243f6a8885a308d3ULL;
static uint64_t xs(void) {
  uint64_t x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return g_seed = x;
}

/* Weighted toward the transpiler's structural bytes: brackets, braces, the
   `#` dispatch, quote/quasiquote/unquote, the `:` block opener, comma, and a
   few word/number bytes. */
static const char FUZZ_ALPHABET[] = "(){}[]#\\\"'`,:;b xyz012.- \t\n";

static void fuzz_random(int iters, int maxlen) {
  for (int i = 0; i < iters; i++) {
    int len = (int)(xs() % (unsigned)maxlen) + 1;
    char *buf = (char *)malloc((size_t)len);
    for (int j = 0; j < len; j++)
      buf[j] = FUZZ_ALPHABET[xs() % (sizeof(FUZZ_ALPHABET) - 1)];
    transpile_bytes(buf, (size_t)len); /* must not crash / OOB / leak (ASan) */
    free(buf);
  }
}

static void fuzz_mutate(int iters) {
  static const char *seeds[] = {
      "def f (x):\n  (* x x)",
      "{:a 1, :b 2}",
      "#{1 2 3}",
      "#b\"xyz\"",
      "`(a ,b ,@c)",
      "f(a b c)",
      "if (> x 0):\n  pr x",
      "#[1 2 3]",
      "a # comment",
      "{nested {:k #[1 2]}}",
  };
  const int nseeds = (int)(sizeof(seeds) / sizeof(seeds[0]));
  for (int i = 0; i < iters; i++) {
    const char *seed = seeds[xs() % (unsigned)nseeds];
    size_t slen = strlen(seed);
    size_t cap = slen + 16;
    char *buf = (char *)malloc(cap);
    memcpy(buf, seed, slen);
    size_t len = slen;
    int muts = (int)(xs() % 4) + 1;
    for (int m = 0; m < muts && len > 0; m++) {
      switch (xs() % 3) {
      case 0:
        buf[xs() % len] = FUZZ_ALPHABET[xs() % (sizeof(FUZZ_ALPHABET) - 1)];
        break;
      case 1:
        len = xs() % len;
        break;
      case 2:
        if (len + 1 < cap) {
          size_t at = xs() % len;
          memmove(buf + at + 1, buf + at, len - at);
          len++;
        }
        break;
      }
    }
    transpile_bytes(buf, len);
    free(buf);
  }
}

#ifdef ADR_LIBFUZZER
/* Coverage-guided entry point. Build with:
     clang -DADR_LIBFUZZER -fsanitize=fuzzer,address,undefined adr_test.c */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  transpile_bytes((const char *)data, size);
  return 0;
}
#else
int main(int argc, char *argv[]) {
  int fuzz_iters = (argc > 1) ? atoi(argv[1]) : 200000;

  printf("=== adder transpiler unit tests ===\n");
  test_atoms();
  test_lists_and_callsugar();
  test_brace_literals();
  test_meta_syntax();
  test_comments();
  test_blocks();
  test_edges();
  printf("unit tests: %d passed, %d failed\n", g_pass, g_fail);

  printf("=== fuzzing (%d random + %d mutation iterations) ===\n", fuzz_iters,
         fuzz_iters);
  fuzz_random(fuzz_iters, 64);
  fuzz_mutate(fuzz_iters);
  printf("fuzz: survived without crash/leak (run under ASan to verify)\n");

  if (g_fail) {
    printf(">>> ADDER TRANSPILER TESTS FAILED (%d)\n", g_fail);
    return 1;
  }
  printf(">>> ALL ADDER TRANSPILER TESTS PASSED\n");
  return 0;
}
#endif
