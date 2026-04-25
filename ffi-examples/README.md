# alcove FFI

alcove can call C functions from any shared library via libffi.
The integration is one builtin: `(ffi-fn LIB FN-NAME RETURN-TYPE ARG-TYPES…)`
returns a callable; you store it like any value and call it like any
alcove function.

> **Security:** FFI runs C code with the full privileges of the alcove
> process. A user script can `(ffi-fn "libc.so.6" "system" "int" "string")`
> and execute arbitrary shell commands. Only run scripts you trust —
> there is no sandbox.

## Quick example

```
(= sqrt (ffi-fn "libm.so.6" "sqrt" "double" "double"))
(prn (sqrt 16.0))   ; → 4.000000

(= pow  (ffi-fn "libm.so.6" "pow"  "double" "double" "double"))
(prn (pow 2.0 10.0))   ; → 1024.000000

(= strlen (ffi-fn "libc.so.6" "strlen" "long" "string"))
(prn (strlen "alcove"))   ; → 6
```

## Type names

The C ABI types alcove understands. Pass each as a string literal.

| name              | C type     | alcove type         |
|-------------------|------------|---------------------|
| `"void"`          | `void`     | nil (return only)   |
| `"int"`           | `int32_t`  | fixnum              |
| `"long"`, `"int64"` | `int64_t` | fixnum              |
| `"double"`        | `double`   | float               |
| `"string"`, `"char*"` | `const char*` | string         |
| `"ptr"`, `"void*"` | `void*`    | fixnum (the address)|

`(ffi-fn "lib" "name" "RETURN-TYPE" "ARG-TYPE-1" "ARG-TYPE-2" …)` —
omit arg types for zero-arg functions.

## Library names

Pass the runtime `.so` (not the dev `.so` symlink). Common ones on Linux:

| library          | typical SONAME       |
|------------------|----------------------|
| C standard       | `libc.so.6`          |
| math             | `libm.so.6`          |
| pthreads         | `libpthread.so.0`    |
| dynamic loader   | `libdl.so.2`         |
| readline         | `libreadline.so.8`   |

For your own libraries, use a path with a slash (`./mylib.so`) or
install on `LD_LIBRARY_PATH` / standard search paths.

## Examples in this directory

- **01-libm-math.alc** — sqrt, sin, cos, pow, log, exp.
- **02-libc-strings.alc** — strlen, strcmp, toupper, tolower, getpid.
- **03-time-syscalls.alc** — time(2), usleep(3).
- **04-custom-mylib.c / .alc** — build your own .so and call into it
  (add, fib, strlen, stateful counter, returned C string).

Run them all (auto-builds `libmylib.so`):

```
make run
```

## Custom libraries

Build with `-shared -fPIC`:

```sh
cc -shared -fPIC -O2 -o libmylib.so mylib.c
```

Then from alcove:

```
(= my-add (ffi-fn "./libmylib.so" "mylib_add" "long" "long" "long"))
(prn (my-add 3 4))   ; → 7
```

State across calls works fine — the library's static variables are
preserved between alcove invocations because alcove keeps the dlopen
handle for the lifetime of the process.

## Limitations and gotchas

- **Max 8 args per FFI fn** (compile-time constant, easy to bump).
- **No struct-by-value, varargs, or callbacks.** Pointer args + a C
  shim are the workaround.
- **Returned `char*` is copied** into a fresh alcove string. If your
  function returns a pointer the caller is supposed to `free()`,
  alcove leaks it (no `free` builtin yet — wrap it via FFI:
  `(= cfree (ffi-fn "libc.so.6" "free" "void" "ptr"))`).
- **Reserved names can't be shadowed at top level.** alcove has a
  builtin `time`, so `(= time (ffi-fn "libc.so.6" "time" …))` does
  bind `time` in the global env but `lookup` checks the reserved
  symbol table first and never finds your binding. Use a different
  name (e.g. `ctime`).
- **NULL pointer arg.** Pass `0` (a fixnum) wherever the C function
  expects `NULL` for a pointer arg.
- **No bignum** — return values bigger than 2^61 silently overflow
  alcove's tagged fixnum encoding.

## Implementation

The FFI is libffi-backed (`-lffi`). Each `(ffi-fn …)` call:

1. dlopens the library (cached per-name in a process-wide table).
2. dlsyms the function.
3. Builds an `ffi_cif` from the type signature.
4. Wraps the fn pointer + cif in an `EXP_FFI` exp_t.

When invoked, alcove's `evaluate` detects `EXP_FFI` ahead of the
`islambda`/`isinternal` paths, evaluates each arg, marshals to the
right C type, calls `ffi_call`, and marshals the return back.

See `alcove.c`'s `alc_ffi_t` struct, `ffifncmd`, and `alc_ffi_call`
for the implementation (~150 lines).
