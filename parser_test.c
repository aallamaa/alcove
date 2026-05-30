/* parser_test.c — C-level unit tests + fuzzing for the Alcove reader.
 *
 * The reader (reader(), callmacrochar(), tokenize()/tokenadd(),
 * make_atom_from_token(), escapereader()) turns a byte stream into an AST.
 * It is the one component that consumes fully untrusted input — every REPL
 * line, file, pipe, and -e string flows through it — so it must never crash,
 * read out of bounds, or leak on ANY input. These tests pin its behavior on
 * known forms and then hammer it with random/structured garbage.
 *
 * We compile alcove.c straight into this TU (same trick as adder.c): rename
 * alcove's entry point so we own main() and get direct access to the
 * non-static reader internals. Inputs are fed via fmemopen, exactly like the
 * real -e / web-eval paths.
 *
 * Build/run:
 *   make parser-test                     # unit tests + bounded random fuzz
 * (ASan) make fuzz                            # coverage-guided libFuzzer
 * (clang)
 *   ./parser_fuzz corpus/ -max_total_time=60
 */
#define main alcove_real_main
#include "alcove.c"
#undef main

/* The reader returns NIL_EXP (nil_singleton) for `()` and compares against
   the true/nil singletons; main() normally builds these. We own main() here,
   so initialize the same globals before any parsing. */
static void init_singletons(void) {
  if (!nil_singleton)
    nil_singleton = make_nil();
  if (!true_singleton)
    true_singleton = make_symbol("t", 1);
}

/* ----- read every form out of a buffer, freeing each (the fuzz workhorse) --
 */
/* Returns the number of forms read. Never returns on its own without draining
   the stream to EOF or an error; capped so a pathological input can't spin. */
static int drain(const char *src, size_t len) {
  FILE *f = fmemopen((void *)src, len, "r");
  if (!f)
    return 0;
  int forms = 0;
  for (int i = 0; i < 1000000; i++) {
    exp_t *e = reader(f, 0, 0);
    if (e == NULL)
      continue; /* comment / "no form here" — keep going */
    int err = iserror(e);
    unrefexp(e); /* frees heap exps; safe on tagged immediates and errors */
    if (err)
      break; /* EOF and parse errors both surface as EXP_ERROR */
    forms++;
  }
  fclose(f);
  return forms;
}

/* Parse exactly the first form of a NUL-terminated string. Caller unrefs. */
static exp_t *parse1(const char *src) {
  FILE *f = fmemopen((void *)src, strlen(src), "r");
  if (!f)
    return NULL;
  exp_t *e;
  do {
    e = reader(f, 0, 0);
  } while (e == NULL); /* skip leading comments/whitespace-only reads */
  fclose(f);
  return e;
}

/* ---------------------------- unit tests ---------------------------------- */
static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      g_pass++;                                                                \
    } else {                                                                   \
      g_fail++;                                                                \
      printf("  FAIL [%s:%d]: ", __func__, __LINE__);                          \
      printf(__VA_ARGS__);                                                     \
      printf("\n");                                                            \
    }                                                                          \
  } while (0)

/* The head symbol of a parsed (head ...) list form, or NULL. */
static const char *head_sym(exp_t *e) {
  if (!is_ptr(e) || e->type != EXP_PAIR || !e->content)
    return NULL;
  exp_t *h = e->content;
  return (is_ptr(h) && h->type == EXP_SYMBOL) ? exp_text(h) : NULL;
}

/* Count top-level elements of a parsed list form. */
static int list_count(exp_t *e) {
  int n = 0;
  for (exp_t *p = e;
       is_ptr(p) && p->type == EXP_PAIR && (p->content || p->next); p = p->next)
    n++;
  return n;
}

static void test_atoms(void) {
  exp_t *e;

  e = parse1("42");
  CHECK(isnumber(e) && FIX_VAL(e) == 42, "42 -> %lld",
        (long long)(isnumber(e) ? FIX_VAL(e) : -1));
  unrefexp(e);

  e = parse1("-7");
  CHECK(isnumber(e) && FIX_VAL(e) == -7, "-7 parse");
  unrefexp(e);

  e = parse1("0");
  CHECK(isnumber(e) && FIX_VAL(e) == 0, "0 parse");
  unrefexp(e);

  e = parse1("3.5");
  CHECK(isfloat(e) && e->f > 3.49 && e->f < 3.51, "3.5 -> float");
  unrefexp(e);

  e = parse1("foo-bar");
  CHECK(is_ptr(e) && e->type == EXP_SYMBOL &&
            strcmp(exp_text(e), "foo-bar") == 0,
        "symbol foo-bar");
  unrefexp(e);

  e = parse1("\"hello world\"");
  CHECK(is_ptr(e) && e->type == EXP_STRING &&
            strcmp(exp_text(e), "hello world") == 0,
        "string literal");
  unrefexp(e);

  e = parse1("\"with \\\"quote\\\"\"");
  CHECK(is_ptr(e) && e->type == EXP_STRING &&
            strcmp(exp_text(e), "with \"quote\"") == 0,
        "string with escaped quotes -> [%s]",
        (is_ptr(e) && e->type == EXP_STRING) ? exp_text(e) : "?");
  unrefexp(e);

  e = parse1("#\\A");
  CHECK(ischar(e) && CHAR_VAL(e) == 'A', "char #\\A");
  unrefexp(e);
}

static void test_lists(void) {
  exp_t *e;

  e = parse1("(1 2 3)");
  CHECK(list_count(e) == 3, "(1 2 3) count=%d", list_count(e));
  unrefexp(e);

  e = parse1("()");
  CHECK(is_ptr(e) && e->type == EXP_PAIR && !e->content && !e->next,
        "() -> nil/empty");
  unrefexp(e);

  e = parse1("((a) (b c) ())");
  CHECK(list_count(e) == 3, "nested count=%d", list_count(e));
  unrefexp(e);

  e = parse1("'x");
  CHECK(head_sym(e) && strcmp(head_sym(e), "quote") == 0, "'x -> (quote x)");
  unrefexp(e);

  e = parse1("`(a ,b ,@c)");
  CHECK(head_sym(e) && strcmp(head_sym(e), "quasiquote") == 0,
        "quasiquote head");
  unrefexp(e);
}

static void test_dispatch(void) {
  exp_t *e;

  /* #[ ... ] -> (vector ...) */
  e = parse1("#[1 2 3]");
  CHECK(head_sym(e) && strcmp(head_sym(e), "vector") == 0, "#[..] -> vector");
  unrefexp(e);

  /* { ... } -> (hash-map ...) */
  e = parse1("{:a 1 :b 2}");
  CHECK(head_sym(e) && strcmp(head_sym(e), "hash-map") == 0,
        "{..} -> hash-map");
  unrefexp(e);

  /* #{ ... } -> (hash-set ...)  [round-trips an EXP_SET print] */
  e = parse1("#{1 2 3}");
  CHECK(head_sym(e) && strcmp(head_sym(e), "hash-set") == 0,
        "#{..} -> hash-set (got %s)", head_sym(e) ? head_sym(e) : "(none)");
  unrefexp(e);

  /* #b"..." -> EXP_BLOB with the string's bytes [round-trips a blob print] */
  e = parse1("#b\"test\"");
  CHECK(isblob(e) && blob_len(e) == 4 && memcmp(blob_bytes(e), "test", 4) == 0,
        "#b\"test\" -> blob len=%zu", isblob(e) ? blob_len(e) : (size_t)-1);
  unrefexp(e);

  /* escaped quote inside a blob literal */
  e = parse1("#b\"a\\\"b\"");
  CHECK(isblob(e) && blob_len(e) == 3 && memcmp(blob_bytes(e), "a\"b", 3) == 0,
        "#b with escaped quote, len=%zu", isblob(e) ? blob_len(e) : (size_t)-1);
  unrefexp(e);
}

static void test_errors(void) {
  exp_t *e;

  e = parse1("(1 2 3");
  CHECK(iserror(e), "unterminated list -> error");
  unrefexp(e);

  e = parse1("\"unterminated");
  CHECK(iserror(e), "unterminated string -> error");
  unrefexp(e);

  e = parse1("#z");
  CHECK(iserror(e), "unknown dispatch #z -> error");
  unrefexp(e);

  e = parse1("#b foo");
  CHECK(iserror(e), "#b without string -> error");
  unrefexp(e);

  e = parse1("{:a 1");
  CHECK(iserror(e), "unterminated hash-map -> error");
  unrefexp(e);
}

#define DRAIN(s) drain((s), strlen(s))
static void test_multiform(void) {
  /* drain() must walk multiple top-level forms and comments without leaking. */
  CHECK(DRAIN("1 2 3") == 3, "three top-level atoms");
  CHECK(DRAIN("; comment\n(a b) ; trailing\n42") == 2, "forms around comments");
  CHECK(DRAIN("") == 0, "empty input");
  CHECK(DRAIN("   \t\n  ") == 0, "whitespace only");
}

static void test_comments(void) {
  /* `;` line comment (classic) and `# ` line comment (Adder-compatible). */
  CHECK(DRAIN("; just a comment") == 0, "; comment only -> no forms");
  CHECK(DRAIN("# just a comment") == 0, "# comment only -> no forms");
  CHECK(DRAIN("; c\n42") == 1, "; comment then atom");
  CHECK(DRAIN("# c\n42") == 1, "# comment then atom");
  CHECK(DRAIN("42 # trailing\n43") == 2, "trailing # comment between atoms");
  CHECK(DRAIN("#\t tabbed comment\n7") == 1, "# tab starts a comment too");

  /* A `#` glued to a dispatch char is NOT a comment — it must still read. */
  exp_t *e = parse1("#\\#");
  CHECK(ischar(e) && CHAR_VAL(e) == '#', "#\\# is the char '#', not a comment");
  unrefexp(e);
  e = parse1("#{1 2}");
  CHECK(head_sym(e) && strcmp(head_sym(e), "hash-set") == 0,
        "#{ still a set under the comment rule");
  unrefexp(e);
  e = parse1("#b\"q\"");
  CHECK(isblob(e) && blob_len(e) == 1,
        "#b\" still a blob under the comment rule");
  unrefexp(e);
}

/* ------------------------------ fuzzing ----------------------------------- */
/* A tiny deterministic PRNG so the built-in fuzz run is reproducible across
   machines (no time/rand() dependency). xorshift64. */
static uint64_t g_seed = 0x9e3779b97f4a7c15ULL;
static uint64_t xs(void) {
  uint64_t x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return g_seed = x;
}

/* Bytes weighted toward the reader's structural characters, so random inputs
   actually exercise nesting/macros/strings rather than mostly-illegal-char
   rejections. */
static const char FUZZ_ALPHABET[] =
    "()[]{}#\\\"'`,;:b 0123456789abcXYZ.-+ \t\n";

static void fuzz_random(int iterations, int maxlen) {
  for (int i = 0; i < iterations; i++) {
    int len = (int)(xs() % (unsigned)maxlen) + 1;
    char *buf = (char *)malloc((size_t)len);
    for (int j = 0; j < len; j++)
      buf[j] = FUZZ_ALPHABET[xs() % (sizeof(FUZZ_ALPHABET) - 1)];
    drain(buf,
          (size_t)len); /* must not crash / read OOB / leak (ASan checks) */
    free(buf);
  }
}

/* Mutation fuzz: start from valid seeds and flip/insert/delete bytes — finds
   bugs near "almost valid" inputs that pure-random rarely reaches. */
static void fuzz_mutate(int iterations) {
  static const char *seeds[] = {
      "(a (b c) d)",    "#[1 2 3]",     "{:k \"v\"}", "#{1 2 3}", "#b\"xyz\"",
      "'(quoted list)", "`(qq ,x ,@y)", "#\\z",       "3.14159",  "((((()))))",
  };
  const int nseeds = (int)(sizeof(seeds) / sizeof(seeds[0]));
  for (int i = 0; i < iterations; i++) {
    const char *seed = seeds[xs() % (unsigned)nseeds];
    size_t slen = strlen(seed);
    size_t cap = slen + 16;
    char *buf = (char *)malloc(cap);
    memcpy(buf, seed, slen);
    size_t len = slen;
    int muts = (int)(xs() % 4) + 1;
    for (int m = 0; m < muts && len > 0; m++) {
      switch (xs() % 3) {
      case 0: /* flip a byte */
        buf[xs() % len] = FUZZ_ALPHABET[xs() % (sizeof(FUZZ_ALPHABET) - 1)];
        break;
      case 1: /* truncate */
        len = xs() % len;
        break;
      case 2: /* duplicate a byte (grow) */
        if (len + 1 < cap) {
          size_t at = xs() % len;
          memmove(buf + at + 1, buf + at, len - at);
          len++;
        }
        break;
      }
    }
    drain(buf, len);
    free(buf);
  }
}

#ifdef PARSER_LIBFUZZER
/* Coverage-guided entry point. Build with:
     clang -DPARSER_LIBFUZZER -fsanitize=fuzzer,address -O1 ... parser_test.c
   libFuzzer supplies main(); we just feed each input through the reader. */
int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  init_singletons();
  return 0;
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  drain((const char *)data, size);
  return 0;
}
#else
int main(int argc, char *argv[]) {
  int fuzz_iters = (argc > 1) ? atoi(argv[1]) : 200000;
  init_singletons();

  printf("=== parser unit tests ===\n");
  test_atoms();
  test_lists();
  test_dispatch();
  test_errors();
  test_multiform();
  test_comments();
  printf("unit tests: %d passed, %d failed\n", g_pass, g_fail);

  printf("=== fuzzing (%d random + %d mutation iterations) ===\n", fuzz_iters,
         fuzz_iters);
  fuzz_random(fuzz_iters, 64);
  fuzz_mutate(fuzz_iters);
  printf(
      "fuzz: survived without crash/leak (run under ASan to verify memory)\n");

  if (g_fail) {
    printf(">>> PARSER TESTS FAILED (%d)\n", g_fail);
    return 1;
  }
  printf(">>> ALL PARSER TESTS PASSED\n");
  return 0;
}
#endif
