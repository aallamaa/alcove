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
- **API/ABI version.** `ALCOVE_API_VERSION` (in `alcove.h`) is the embedding
  API/ABI version; it bumps whenever a change could break a separately-compiled
  consumer (the `exp_t`/`env_t` layout, an exported function's signature, the
  calling convention). The embedding C API is **pre-1.0 and not yet ABI-frozen**
  (see [`docs/stability.md`](../../docs/stability.md)) — rebuild embedders and
  native modules against the same source revision.
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
  exp_t *r = alcove_make_int(alcove_arg_int(e,env,0) + alcove_arg_int(e,env,1));
  unrefexp(e);   /* a builtin MUST consume its call form — see below */
  return r;
}
int alcove_module_abi(void) { return ALCOVE_API_VERSION; } /* ABI guard */
int alcove_module_init(void) { return alcove_register_cmd("nm/add", nm_add, 0); }
```

**Export `alcove_module_abi`.** It returns the `ALCOVE_API_VERSION` the module
was compiled against; the host checks it at `(require)` time and **refuses a
mismatch** with a clear error rather than dlopen'ing a binary whose `exp_t`
layout no longer matches (which would corrupt silently). The symbol is optional
for backward compatibility — a module without it loads as before — but every new
module should export it.

**Ownership: a builtin must consume its call form.** Read every argument, then
`unrefexp(e)` exactly once before returning — just like every core builtin
(`conscmd` etc.). The interpreter hands `e` with one ref it expects you to
release. This always held, but it became load-bearing once non-tail-aware
builtins compile to a real fast call (`OP_CALL_GLOBAL`): the bytecode VM then
builds a **fresh** call form per call, so a builtin that forgets `unrefexp(e)`
leaks one form on every call — unbounded inside a hot loop. (Pull args with
`alcove_arg_int`/`alcove_arg_string`, which borrow; if you `EVAL` an arg you own
the result and must `unrefexp` it too.)

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

## Custom object types (with persistence)

A native module can define its **own value type** — a tagged heap object holding
a C struct — with serializers so instances survive `savedb`/`loaddb`:

```c
static unsigned short MY = 0;                 /* runtime type id */
static void my_destroy(exp_t *e){ free(e->ptr); }          /* at refcount 0 */
static void my_print(exp_t *e){ printf("#<my %d>", ...); } /* optional */
static exp_t *my_dump(exp_t *e, FILE *s){     /* tag THEN payload */
  if (dumptype(s, &e->type) <= 0) return NULL;
  /* fwrite the struct fields */ return e; }
static exp_t *my_load(exp_t *e, FILE *s){     /* payload only (tag consumed) */
  my_t *p = malloc(sizeof *p); /* fread fields */ e->ptr = p; return e; }

int alcove_module_init(void){
  exp_tfunc ops; memset(&ops,0,sizeof ops);
  ops.destroy=my_destroy; ops.print=my_print; ops.dump=my_dump; ops.load=my_load;
  MY = alcove_register_type("mymod/my", &ops);   /* qualified, durable name */
  /* register constructors that call alcove_make_foreign(MY, ptr) */
}
```

- `alcove_register_type(name, ops)` reserves a 2-byte runtime type id and is
  **idempotent by name** (re-`require` reuses it). `name` is the durable
  identity stored in `db.dump`; the runtime id is per-process.
- `alcove_make_foreign(id, ptr)` builds an instance; `alcove_foreign_ptr(e)` /
  `alcove_is_foreign(e, id)` read it back. The `destroy` hook frees the C
  payload at refcount zero.
- **Persistence:** `savedb` writes a header type-table (id → name → module spec)
  and the object's payload; `loaddb` remaps the id by name and **auto-`require`s
  the module** if it isn't loaded, so a dump opens in a fresh process. Run with
  `--safe` to disable that auto-load (the object then won't reconstruct).

See [`nativemod.c`](nativemod.c)'s `nm/counter` (`make native-module-example`).

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
