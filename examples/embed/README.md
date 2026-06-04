# Embedding Alcove in a C program

Alcove is a single translation unit, so embedding it is just a `#include` —
no library to link, no build system to integrate. Define `ALCOVE_NO_MAIN`
(so Alcove's own `main()` is omitted), include `alcove.c`, and drive the
engine through a small C API.

```c
#define ALCOVE_NO_MAIN
#include "alcove.c"

static exp_t *host_mul(exp_t *e, env_t *env) {        /* a C builtin */
  return alcove_make_int(alcove_arg_int(e, env, 0) * alcove_arg_int(e, env, 1));
}

int main(void) {
  alcove_init();                                 /* bring the engine up */
  alcove_register_cmd("host-mul", host_mul, 0);  /* expose C to Alcove  */
  exp_t *r = alcove_eval_string("(host-mul 6 7)");
  printf("%lld\n", (long long)FIX_VAL(r));        /* 42 */
  unrefexp(r);                                    /* own it → unref it   */
}
```

Build & run (from the repo root):

```sh
make embed-example
# or by hand:
cc -I. -O2 -fno-strict-aliasing -o host examples/embed/host.c -lm
```

## The API

| Function | Purpose |
|----------|---------|
| `env_t *alcove_init(void)` | Initialize the engine once; returns the global env (also `g_global_env`). |
| `exp_t *alcove_eval_string(const char *src)` | Evaluate s-expressions; returns the last value as an **owned** ref, or an error value (test `iserror`). |
| `int alcove_register_cmd(const char *name, lispCmd *fn, int tail_aware)` | Expose a C function `exp_t *(exp_t *e, env_t *env)` as an Alcove builtin. |
| `alcove_arg_int / alcove_arg_string` | Inside a C builtin, pull the Nth (evaluated) argument as a C value. |
| `make_integeri / make_floatf / make_string` | Build Alcove values from C. |
| `isnumber/isfloat/isstring`, `FIX_VAL(e)`, `e->f`, `exp_text(e)` | Read Alcove values back into C. |
| `refexp / unrefexp` | Refcount. **You own every `exp_t *` returned to you — `unrefexp` it once.** Tagged immediates (fixnums, chars, `nil`, `t`) need no unref. |

## Notes

- **One engine per process.** The runtime uses global singletons, so there is a
  single interpreter instance (call `alcove_init` once). This matches the
  embedding model; multiple independent states are not supported.
- **Dialect.** `alcove_eval_string` reads Alcove s-expressions. To run Adder
  (`.adr`) surface syntax, transpile first with `als_to_sexpr` (always
  available) or use `require` on a `.adr` file.
- **Errors never crash the host** — a failing form returns an error `exp_t`;
  check `iserror` and read the message with `error-message` / `exp_text`.

See [`host.c`](host.c) for a worked example covering callbacks, float/string
results, error handling, and passing a C-built value into the engine.

## Native modules (separately-compiled extensions)

The flip side of embedding: instead of *your* program hosting Alcove, a
**shared library** extends a *running* Alcove. A native module `#include`s
`alcove.h`, defines builtins, and exports one hook:

```c
#include "alcove.h"
static exp_t *nm_add(exp_t *e, env_t *env) {
  return alcove_make_int(alcove_arg_int(e,env,0) + alcove_arg_int(e,env,1));
}
int alcove_module_init(void) { return alcove_register_cmd("nm/add", nm_add, 0); }
```

```sh
cc -shared -fPIC -I. -o nm.so nm.c        # Linux  (.dylib on macOS)
```

```lisp
(require "nm.so")     ; dlopens it and calls alcove_module_init
(nm/add 20 22)        ; => 42
```

`require` recognizes a `.so`/`.dylib` path and loads it natively (load-once,
like source modules). The module resolves the host's `alcove_register_cmd` /
`make_*` symbols at `dlopen`, so **the alcove binary must be built with FFI
enabled** (it links `-rdynamic` then — the default when libffi is present).
Name your builtins qualified (`nm/...`) so they don't collide with host
globals. See [`nativemod.c`](nativemod.c) (`make native-module-example`).

## Security / trust model

`require` resolves a module by searching, in order: the requiring file's
directory, then each `$ALCOVE_PATH` entry, then the **current working
directory**. Loading any module — source *or* native — runs code, and a native
`.so`/`.dylib` runs arbitrary machine code. So treat the **cwd and
`$ALCOVE_PATH` as trusted input**, exactly as you would Python's `sys.path`:
don't run a script from an untrusted directory if it does bare `(require …)`,
and prefer an explicit `$ALCOVE_PATH` of dirs you control. (A bare name only
ever resolves to `.alc`/`.adr`; a native module requires an explicit
`.so`/`.dylib` in the spec.) Alcove is an embeddable scripting language, not a
sandbox — there's no privilege boundary between a loaded module and the host.
