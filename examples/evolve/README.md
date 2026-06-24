# examples/evolve — a self-improving program loop (M0)

A minimal demonstration of using Alcove/Adder as a harness for AI-driven,
self-modifying code. It exercises the whole idea with a **stub model** and **no
network**, so it runs anywhere the engine builds.

```
text candidate ──read-string-sexpr──▶ form ──eval (sandboxed)──▶ function
                                                                     │
              best ◀── keep-if-better ◀── score ◀── fitness ────────┘
               │
               └── savedb ──▶ (restart) ──▶ loaddb ──▶ call the evolved closure
```

## The enabling primitives

Three builtins close the loop (see `lib/evolve.adr` and the `repl_builtins.h`
implementations):

- `(read-string-sexpr s)` — parse the first s-expression in a string into a
  **value** (a syntax error is returned as an error value, never a crash).
- `(read-all-string s)` — parse every form into a list.
- `(adder->sexpr s)` — transpile Adder surface text to s-expression text, so a
  model can emit the friendlier Adder dialect; compose with the above + `eval`.

## Run it

```sh
make adder
./adder examples/evolve/run.adr            # evolve a squaring function, save champion
./adder --noload examples/evolve/reload.adr  # fresh process: reload + call it
```

Everything is dialect-symmetric — `./alcove` runs the same logic on s-expression
sources, and the loop is covered in `test.alc` (the `evolve ...` asserts).

## Safety

Each candidate is untrusted code. The loop never lets it escape:

- **parse errors are values** — `read-string-sexpr` returns an error, it isn't raised;
- **crashes are contained** — every `eval`/fitness call runs inside `(try ...)`;
- **runaways are bounded** — `(with-time-limit ...)` aborts a looping candidate;
- a failed candidate simply scores worst and is skipped.

## What changes for a real model

Replace `evolve/stub-llm` with a real completion call (an `(llm prompt)` over the
Anthropic API) and **nothing else in the loop changes** — it only ever sees
candidate code as strings. That is the next milestone (`lib/llm.adr`).
