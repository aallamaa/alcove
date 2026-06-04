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

int alcove_module_init(void) {
  if (alcove_register_cmd("nm/add", nm_add, 0) != 0)
    return -1;
  if (alcove_register_cmd("nm/scale", nm_scale, 0) != 0)
    return -1;
  if (alcove_register_cmd("nm/greet", nm_greet, 0) != 0)
    return -1;
  return 0;
}
