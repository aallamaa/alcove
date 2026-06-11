# Adder -> alcove

`adr.py` (repo root) is a homoiconic reader for the Python-like
indentation syntax described in `adder-spec.md`. It turns source
into Lisp *forms* (not an AST) and emits alcove.

```sh
python3 adr.py examples/adder/demo.adr            # to stdout
python3 adr.py examples/adder/demo.adr -o out.alc # to a file
python3 adr.py examples/adder/demo.adr | ./alcove --noload
```

This is **not** the same as `alcove-py.py`, which compiles *real* Python
(`return`, `range`, `elif`, ...) via `ast`. Adder has no Python
semantics: bare words are symbols, there is no `return`, a line is just
a list.

## Native binary: ./adder

`make als` builds `./adder` — the full alcove runtime (JIT, readline
highlighting, completion, history, FFI, RESP) with the Adder
front end wired in. `./alcove` is untouched (built without `ALCOVE_ALS`).

Adder scripts are directly executable — `#!` is a comment, so
`#!/usr/bin/env adder` works; arguments arrive in `*args*`.
See `dirstat.adr` (a file-type summary utility) for the v0.2
scripting floor in action: `./dirstat.adr ~/Code`.

```sh
make als                       # -> ./adder
./adder                    # ALS REPL, syntax highlighted
./adder prog.adr            # run an ALS file
cat prog.adr | ./adder      # piped ALS
./adder -e 'prn (+ 2 3)'   # ALS one-liner
```

In the REPL a one-line form submits on Enter. A line ending in `:`
(or with unbalanced parens) opens continuation mode (`    ...`); a
**blank line submits** the whole block:

```
In [1]: def fib (n):
   ...:   if (< n 2):
   ...:     n
   ...:     + (fib (- n 1)) (fib (- n 2))
   ...:                       <- blank line here evaluates the def
Out[1]: #<procedure:fib>
In [2]: prn (fib 10)
55
```

`adder.c` simply `#include`s `alcove.c` (renaming its `main`); the
transpiler lives in `adr.h` and is a C port of `adr.py`.
`alc2adr.py` / `adr.py` remain useful as offline batch tools.

## Reader rules

- A line becomes a list of its top-level forms; many forms -> `(a b c)`.
- A line ending in `:` opens a block; the indented lines below are
  appended as further elements of that line's list.
- `(...)` are ordinary inline Lisp lists and nest.
- `name(a b)` == `name (a b)` (symbol then list) -> ideal for
  `def f (a b):`. `name()` is the no-arg call `(name)`.
- `'x` -> `(quote x)`; explicit `quote:` blocks also work.
- `# ...` is a comment (not inside strings); string escapes are kept
  verbatim for alcove's own reader (`\t`, `\x1B`, ...).

## Deviation from the spec

The spec says a lone bare word `qux` reads as the call `(qux)`. Here a
**lone token reads as its value** (so `n` on its own line returns `n`).
Write a no-arg call as `(qux)` or `qux()`.

## alcove-target mapping

`true -> t`, `false -> nil`, `nil -> nil`; head `macro -> defmacro`,
head `set -> =`. `def fn do if for while` are already native.

Mind alcove's real dialect (see `docs/alcove-language.md`): `let` takes
**one** binding -- use `with (a 1 b 2):` for several; there is no
`cond`/`progn`; `0` is falsey.
