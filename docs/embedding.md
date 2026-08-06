# Embedding alcove in a C program

Alcove is a **unity build**: one translation unit, `alcove.c`, which
`#include`s ~30 fragments. That is load-bearing rather than incidental — the
value model, refcounting and JIT glue inline across fragment boundaries, and
that inlining is the performance the project exists for.

The consequence for embedders is the thing to understand first:

> **There is no `libalcove.a` or `libalcove.so`, and there will not be one.**
> You embed alcove by compiling its translation unit into *your* program.

So `pkg-config --cflags alcove` gives you an **include path**, not a link line
for a library that does not exist. `--libs` gives only what alcove's own TU
needs (`-lm`, plus libffi/readline when the build detected them).

## Install

```sh
make install       # the alcove + adder binaries    -> $PREFIX/bin
make install-dev   # sources + alcove.pc for embedders
```

`PREFIX` defaults to `~/.local`; `DESTDIR` is honoured for packaging. The dev
install puts the engine sources in `$PREFIX/include/alcove/` and a
pkg-config file in `$PREFIX/lib/pkgconfig/alcove.pc`.

`alcove.pc` is **generated at install time, not committed**, so its `Libs`
match the autodetection of the build that produced it. A `.pc` claiming
`-lffi` on a machine that built without libffi would not link.

## Hello, engine

```c
#define ALCOVE_NO_MAIN     /* omit alcove's own main() */
#include "alcove.c"

#include <stdio.h>

int main(void) {
  alcove_init();
  exp_t *v = alcove_eval_string("(+ 1 2)");
  printf("%lld\n", (long long)FIX_VAL(v));
  unrefexp(v);                       /* you own what you are handed */
  return 0;
}
```

```sh
cc -O2 -fno-strict-aliasing -o host host.c $(pkg-config --cflags --libs alcove)
```

`examples/embed/host.c` is the fuller worked example — registering a C
function as a builtin, reading arguments, returning values, and handling an
error value. `make embed-example` builds and runs it, and is part of
`make test-all`, so the embedding path cannot rot unnoticed.

## The API surface

| | |
|---|---|
| `env_t *alcove_init(void)` | bring the engine up (once) |
| `exp_t *alcove_eval_string(const char *)` | evaluate s-expressions, get a value |
| `int alcove_register_cmd(name, fn, tail_aware)` | expose a C function to alcove |
| `make_integeri` / `make_floatf` / `make_string` … | build values |
| `isnumber` / `isfloat` / `isstring`, `FIX_VAL`, `->f`, `exp_text` | read values |
| `refexp` / `unrefexp` | ownership |

Two rules cover most mistakes:

- **Refcounting, not GC.** Unref what you are handed. `unrefexp(e)` *before*
  `e`'s last read is the classic bug — build your result first, then unref.
- **Errors are values.** `alcove_eval_string` returns an `EXP_ERROR` rather
  than longjmp'ing or exiting; check `iserror(v)` and read `error-message`.
  A syntax error in embedded source is a value too, never a crash — which is
  what makes it safe to evaluate a program you did not write.

## ABI version

`ALCOVE_API_VERSION` (alcove.h) is bumped when an exported signature or its
semantics change. A **native module** — a `.so` loaded by `(require "foo.so")`
rather than a host that embeds the engine — must export

```c
int alcove_module_abi(void) { return ALCOVE_API_VERSION; }
```

and is refused if it disagrees with the running engine. `make
native-module-example` builds one; it is also gated by `make test-all`.

## Build flags that matter

| flag | effect |
|---|---|
| `ALCOVE_NO_MAIN` | omit alcove's `main()` — **required** when embedding |
| `ALCOVE_SINGLE_THREADED` | plain `++`/`--` refcounts instead of atomics; faster, single-threaded only |
| `ALCOVE_JIT=1` | native JIT (amd64/arm64); without it the bytecode VM runs |
| `ALCOVE_ALS` | compile the Adder surface dialect in as well |

The same `-fno-strict-aliasing` alcove builds itself with is recommended for
your host TU, since it is the TU containing alcove.

## Threads

The engine's allocator and environment arena are per-shard and reached through
a TLS `current_shard`. Embedding into a multi-threaded host means one shard per
thread; the RESP reactor pool is the worked example, and its contract —
including what a callback may and may not touch — is
[docs/multithreading.md](multithreading.md). Start there before calling into
alcove from more than one thread.
