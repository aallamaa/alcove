# heal — code that fixes its own code, live

```sh
./adder --noload examples/heal/run.adr
```

A "production" function crashes; the supervisor catches the error and
builds a **diagnosis** from the error-introspection surface —
`(error-code e)`, `(error-message e)`, `(error-location e)`,
`(error-backtrace e)`, `(error-form e)` — hands it to a patch generator
(a stub here; `lib/llm.adr` in production), and tries each candidate
patch: parse + evaluate under `with-time-limit`, install it under the
broken function's name, run the test suite, keep the first patch that
passes. The process never restarts and never crashes: syntax errors are
values, runaway candidates hit the time limit, and nothing survives that
the tests reject.

The reusable loop lives in `lib/heal.adr` (`heal/diagnose`,
`heal/attempt`); this example is the whole story in ~25 lines of Adder.
It is `examples/evolve`'s safety envelope pointed at *healing* instead of
*search* — combine the two and you have an entity that both improves and
repairs itself, with the operator owning the tests that define "correct".
