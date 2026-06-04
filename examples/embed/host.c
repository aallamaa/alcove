/* host.c — embedding the Alcove engine in a plain C program.
 *
 * Alcove is a single translation unit, so you embed it by #including alcove.c
 * with ALCOVE_NO_MAIN defined (which omits alcove's own main()), then driving
 * it through the small C API:
 *
 *   env_t *alcove_init(void);                  bring the engine up (once)
 *   exp_t *alcove_eval_string(const char*);    eval s-expressions → value
 *   int    alcove_register_cmd(name, fn, ta);  expose a C function to alcove
 *   exp_t *make_integeri / make_floatf / make_string ...   build values
 *   isnumber/isfloat/isstring + FIX_VAL / ->f / exp_text    read values back
 *   refexp / unrefexp                          ownership (unref what you hold)
 *
 * Build + run from the repo root:   make embed-example
 * Or by hand:                       cc -I. -O2 -fno-strict-aliasing \
 *                                      -o examples/embed/host \
 *                                      examples/embed/host.c -lm
 */
#define ALCOVE_NO_MAIN
#include "alcove.c"

#include <stdio.h>

/* A C function exposed to Alcove as (host-mul a b). A builtin receives the
   UNEVALUATED call form `e`; pull evaluated arguments with alcove_arg_int and
   return a value with alcove_make_int (or any make_* constructor). */
static exp_t *host_mul(exp_t *e, env_t *env) {
  int a = alcove_arg_int(e, env, 0);
  int b = alcove_arg_int(e, env, 1);
  return alcove_make_int(a * b);
}

int main(void) {
  alcove_init();                                /* 1. bring the engine up */
  alcove_register_cmd("host-mul", host_mul, 0); /* 2. expose a C function   */

  /* 3. Evaluate Alcove that calls back into C; read the integer result. */
  exp_t *r = alcove_eval_string("(host-mul 6 7)");
  printf("(host-mul 6 7)            => %lld\n", (long long)FIX_VAL(r));
  unrefexp(r);

  /* 4. Define an Alcove function from C, then call it and read a float. */
  unrefexp(alcove_eval_string("(def mix (a b) (+ (* a b) 1.0))"));
  exp_t *f = alcove_eval_string("(mix 3.0 4.0)");
  if (isfloat(f))
    printf("(mix 3.0 4.0)            => %g\n", f->f);
  unrefexp(f);

  /* 5. A string result, read out as a C string. */
  exp_t *s = alcove_eval_string("(str \"the answer is \" (+ 20 22))");
  if (isstring(s))
    printf("(str ...)                => \"%s\"\n", exp_text(s));
  unrefexp(s);

  /* 6. Errors come back as an error value — test with iserror, never crash. */
  exp_t *bad = alcove_eval_string("(no-such-function 1 2)");
  printf("(no-such-function 1 2)   => %s\n",
         iserror(bad) ? "error (handled)" : "ok");
  unrefexp(bad);

  /* 7. Pass a C-built value into the engine by binding a global, then use it. */
  exp_t *forty_two = make_integeri(42);
  set_get_keyval_dict(g_global_env->d ? g_global_env->d
                                      : (g_global_env->d = create_dict()),
                      "from-c", forty_two);
  GEN_BUMP();
  exp_t *g = alcove_eval_string("(* from-c 10)");
  printf("(* from-c 10) [from-c=42] => %lld\n", (long long)FIX_VAL(g));
  unrefexp(g);

  return 0;
}
