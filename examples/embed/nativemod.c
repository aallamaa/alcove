/* nativemod.c — a separately-compiled Alcove module (a native extension).
 *
 * A native module is a shared library that #includes alcove.h, defines some
 * builtins, and exports a single hook:
 *
 *     int alcove_module_init(void);   // returns 0 on success
 *
 * `(require "nativemod.so")` dlopens the library and calls that hook, which
 * registers the builtins with alcove_register_cmd. The module resolves the
 * host's alcove_register_cmd / make_* / refexp symbols at load time, so the
 * alcove binary must be built with FFI enabled (it links -rdynamic then).
 *
 * Build (Linux):  cc -shared -fPIC -I/path/to/alcove -o nativemod.so nativemod.c
 *        (macOS):  cc -shared -fPIC -I/path/to/alcove -o nativemod.dylib nativemod.c
 * Or from the repo root:  make native-module-example
 *
 * Use:  (require "nativemod.so")
 *       (nm/add 20 22)        => 42
 *       (nm/scale 4)          => 10.0
 *       (nm/greet "world")    => "native hello, world"
 *
 * Conventions: a builtin receives the UNEVALUATED call form `e`; pull evaluated
 * arguments with alcove_arg_int / alcove_arg_string (which don't transfer
 * ownership), or EVAL them yourself (then you own the result — unrefexp it).
 * Return a fresh value built with make_integeri / make_floatf / make_string /
 * alcove_make_int. Register names qualified (here "nm/...") so they don't
 * collide with the host's globals — references are just `nm/add` etc.
 */
#include "alcove.h"
#include <string.h>

static exp_t *nm_add(exp_t *e, env_t *env) {
  return alcove_make_int(alcove_arg_int(e, env, 0) + alcove_arg_int(e, env, 1));
}

static exp_t *nm_scale(exp_t *e, env_t *env) { /* (nm/scale x) -> 2.5 * x */
  exp_t *xv = EVAL(cadr(e), env);              /* we eval, so we own xv */
  double x = isfloat(xv) ? xv->f : (double)FIX_VAL(xv);
  unrefexp(xv);
  return make_floatf(2.5 * x);
}

static exp_t *nm_greet(exp_t *e, env_t *env) { /* (nm/greet name) -> string */
  const char *who = alcove_arg_string(e, env, 0); /* borrowed, valid this call */
  char buf[256];
  int n = snprintf(buf, sizeof buf, "native hello, %s", who ? who : "?");
  return make_string(buf, n);
}

/* ---- a CUSTOM OBJECT TYPE: nm/counter (a heap C struct) ----
   Shows defining your own value type with dump/load so instances persist
   through savedb/loaddb (the dump records the durable type name "nm/counter";
   loaddb auto-(require)s this module to reconstruct it). */
#include <stdlib.h>
#include <string.h>
typedef struct {
  long long n;
} counter_t;
static unsigned short NM_COUNTER = 0; /* runtime type id, set in init */

static void counter_destroy(exp_t *e) { free(e->ptr); } /* free the C struct */
static void counter_print(exp_t *e) {
  printf("#<counter %lld>", ((counter_t *)e->ptr)->n);
}
/* dump writes the 2-byte type tag (via dumptype) then the payload; load reads
   only the payload — load_exp_t already consumed + remapped the tag. */
static exp_t *counter_dump(exp_t *e, FILE *s) {
  if (dumptype(s, &e->type) <= 0)
    return NULL;
  if (fwrite(&((counter_t *)e->ptr)->n, sizeof(long long), 1, s) != 1)
    return NULL;
  return e;
}
static exp_t *counter_load(exp_t *e, FILE *s) {
  counter_t *c = malloc(sizeof *c);
  if (!c || fread(&c->n, sizeof(long long), 1, s) != 1) {
    free(c);
    return NULL;
  }
  e->ptr = c;
  return e;
}
static exp_t *counter_make(exp_t *e, env_t *env) { /* (nm/counter n) */
  counter_t *c = malloc(sizeof *c);
  c->n = alcove_arg_int(e, env, 0);
  return alcove_make_foreign(NM_COUNTER, c);
}
static exp_t *counter_inc(exp_t *e, env_t *env) { /* (nm/inc! c) -> c */
  exp_t *o = EVAL(cadr(e), env);
  counter_t *c = (counter_t *)alcove_foreign_ptr(o);
  if (c)
    c->n++;
  return o; /* owned ref returned */
}
static exp_t *counter_get(exp_t *e, env_t *env) { /* (nm/get c) -> int */
  exp_t *o = EVAL(cadr(e), env);
  counter_t *c = (counter_t *)alcove_foreign_ptr(o);
  exp_t *r = alcove_make_int(c ? (int)c->n : -1);
  unrefexp(o);
  return r;
}

int alcove_module_init(void) {
  if (alcove_register_cmd("nm/add", nm_add, 0) != 0)
    return -1;
  if (alcove_register_cmd("nm/scale", nm_scale, 0) != 0)
    return -1;
  if (alcove_register_cmd("nm/greet", nm_greet, 0) != 0)
    return -1;
  /* Register the custom type (idempotent by name) + its constructors. */
  exp_tfunc ops;
  memset(&ops, 0, sizeof ops);
  ops.destroy = counter_destroy;
  ops.print = counter_print;
  ops.dump = counter_dump;
  ops.load = counter_load;
  NM_COUNTER = alcove_register_type("nm/counter", &ops);
  if (!NM_COUNTER)
    return -1;
  if (alcove_register_cmd("nm/counter", counter_make, 0) != 0 ||
      alcove_register_cmd("nm/inc!", counter_inc, 0) != 0 ||
      alcove_register_cmd("nm/get", counter_get, 0) != 0)
    return -1;
  return 0;
}
