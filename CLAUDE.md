# CLAUDE.md — working in the alcove / adder codebase

Alcove is an embeddable, JIT-compiled numeric Lisp; **Adder** is its
Python-flavored surface dialect (same engine, a string→string transpiler in
front). Both are co-equal headline languages.

This file is the map. Read it before editing so you can change **one file** with
**minimal surrounding context** — which is exactly what the architecture below is
organized for.

---

## Build & test

```sh
make              # default: `jit` — release build with the native JIT -> ./alcove
make adder        # the Adder front end -> ./adder   (adder.c #includes alcove.c)
make mono         # single-threaded refcounts (plain ++/--, no atomics)
make jit-mono     # fastest build (JIT + mono)
make install      # -> ~/.local/bin (PREFIX). RE-RUN after rebuilding, or a stale
                  # installed `adder`/`alcove` on $PATH shadows your fresh build.

make test-all     # THE gate: every build variant + the AST=VM / recur / stderr-spew
                  # differentials. Run this after any evaluator/VM/JIT change.
make test-asan    # whole suite under ASan+UBSan (the only hot-path-sanitized run)
make jit-fuzz     # VM == JIT byte-identical fuzz
make eval-fuzz    # AST == VM whole-program fuzz
make adfmt-test   # the `adder fmt` formatter gate
make fmt-check    # clang-format check (no changes); `make fmt` applies it
make tidy         # clang-tidy in-context via alcove.c
```

The test corpus is `test.alc` (Alcove) and the generated `test.adr` (Adder, via
`make gen-test-adr`). Keep them in lockstep. A run prints `TEST RESULT: N passed,
M failed`; M must be 0.

---

## Architecture — READ THIS FIRST

**This is a deliberate single translation unit (a "unity build"), and that is
load-bearing, not an accident.** `alcove.c` `#include`s ~30 *fragment* files —
both `.h` and `.c` — in a **strict order**; the fragment `.h` files hold
**implementations**, not just declarations. `adder.c` in turn `#include`s
`alcove.c`. The whole engine compiles as one TU so the value model, refcounting,
and JIT glue all **inline across fragment boundaries** — that inlining is the
performance the project exists for. Do not try to make a fragment compile
standalone, and do not introduce real separate compilation without an explicit
decision: it would regress the JIT and break the differential gates.

Consequences for editing:
- A fragment sees everything `#include`d **before** it in `alcove.c`. Ordering
  encodes forward-reference dependencies — moving an `#include` can break the
  build. The order is listed in the module map below.
- A new **special form** must be taught to BOTH the AST evaluator *and* the
  bytecode compiler (`compile_expr`) — otherwise deep tail recursion through it
  falls back to AST and segfaults the C stack. See "Invariants".
- Headers still carry the doc comments: the top-of-file banner in each fragment
  is the API summary. Read it instead of skimming the whole `.c`.

---

## Module map (in `alcove.c` include order)

`alcove.c` itself (~14.6k lines — the one real monolith, slated for further
splitting) holds: the `exp_t` value model + refcounting, the environment arena,
the AST evaluator, the argument-eval macros (`EVAL_ARG_n` / `CLEAN_RETURN_n`),
the `lispProcList[]` builtin dispatch table (line ~439), the bytecode
compiler + VM (`compile_expr` / the run loop, ~7211–10913), the source
pretty-printer, the debugger, the REPL editing builtins, and `main`.

| Fragment | Responsibility |
|---|---|
| `char.h` | character-class tables (`chrmap`, `chr2hex`) used by the reader |
| `adr.h` | Adder → Alcove s-expr transpiler (`als_to_sexpr`); string→string |
| `alcove.h` | the master header: `exp_t`, tags, the `exp_error_t` / `EXP_*` / opcode X-macro enums, refcount macros, prototypes |
| `lfkv.h` | lock-free keyspace for the RESP server (design + decls) |
| `builtins.h` | introspection predicate/accessor declarations + `doc_*` |
| `env.h` | environment (`env_t`) lifecycle: arena-bump make/destroy, ref/unref |
| `dict.h` | open-chaining hash table (`dict_t`): Bernstein hash, rehash, the env binding store |
| `utf8.h` | UTF-8 encode/decode/strlen/index/validate |
| `print.h` | `print_node`: canonical value printer (REPL echo + `pr`) |
| `numeric.h` | exact non-integer numerics: rationals + bounded decimals |
| `persist.h` | per-type binary (de)serializers for `savedb`/`loaddb` |
| `reader.c` | the s-expression reader / tokenizer |
| `vector.h` | dense numeric vector (`EXP_VECTOR`): storage + the NN/tensor ops |
| `ffi.h` | libffi binding: `ffi-fn` / `ffi-callback` / `ffi-struct` / `ffi-pack` |
| `builtins_stdlib.h` | the standard library: arithmetic, string, list, sort, HOFs |
| `builtins_log.h` | observability: error codes (`error-code`), logfmt logging, opt-in metrics |
| `builtins_os.h` | OS/scripting floor: env vars, shell-out, filesystem |
| `builtins_regex.h` | POSIX ERE builtins |
| `pp.h` | Source pretty-printer (both Alcove and Adder dialects) |
| `builtins_control.h` | control-flow builtins: `cond`, `match`, generators |
| `compiler.h` | bytecode VM disassembly and location helpers |
| `jit_common.h` | JIT runtime glue: W^X mmap, gcache trampolines, shape-matcher helpers |
| `jit_arm64.h` | arm64 JIT backend: encoders + shape matchers (selected by `__aarch64__`) |
| `jit_amd64.h` | amd64 JIT backend: encoders + shape matchers (selected by `__x86_64__`) |
| `compiler_impl.h` | Core compiler emit helpers, AST-to-bytecode compiler, VM execution dispatch loop |
| `debugger.h` | Debugger implementation and tab-completion (readline) |
| `builtins_dict.h` | Lisp-value hash-map (`EXP_DICT`) builtins |
| `set.h` | hash-set (`EXP_SET`): canonical key encoder + ops |
| `hamt.h` | persistent map (HAMT / `EXP_HAMT`): nodes, ops, builtins |
| `msgpack.h` | MessagePack encode/decode |
| `json.h` | JSON encode/decode |
| `deque.h` | doubly-linked deque (`EXP_LIST`) ops |
| `blob.h` | binary-safe byte blob (`EXP_BLOB`) ops |
| `gc.h` | `(gc-cycles)`: on-demand trial-deletion cycle collector (zero hot-path cost; walks pairs/dicts/sets/deques/gen-vectors, unwalked types are conservative roots) |
| `weak.h` | weak references (`EXP_WEAK`): `(weak v)`/`(weak-get w)`/`(weak? x)`, TLS target→chain registry, free-path hooks (cells null on target free) |
| `comprehensions.h` | the `lfor`/`sfor`/`dfor`/`gfor` comprehension family (shared eager driver + gfor's synthesized generator pipeline) |
| `watch.h` | `(watch! obj fn)` post-modification hooks on the mutable containers (FLAG_WATCHED + TLS registry; mutator builtins call `watch_notify`) |
| `epoch.c` | epoch-based reclamation for the lock-free keyspace |
| `lfkv.c` | lock-free keyspace implementation |
| `repl_builtins.h` | Lisp-facing REPL editing, diagnostics (check-syntax), and key-binding builtins |
| `resp.c` | RESP2 (Redis protocol) server; `--threads` multi-reactor |

Other TUs (NOT included into alcove.c): `mpsc.h` (MPSC queue, used by resp under
threads), `adfmt.c` (`adder fmt`, a **separate** TU linked into `adder`), and the
`*_test.c` unit harnesses.

---

## Naming & conventions

- **Module prefix on public symbols:** `als_*` (transpiler), `resp_*`, `lfkv_*`,
  `epoch_*`, `hamt_*`, `vec_*`, `dict_*`, `alcove_*` (embed API). Follow the
  prefix of the fragment you are editing.
- **Function definitions start at column 0** so `grep '^name'` finds the
  definition (not its call sites). Prefer `static` for fragment-local helpers.
- **Banner comments** `/* ---- section ---- */` group related functions and are
  the anchors for search and `str_replace` — keep them unique and descriptive.
- **Builtins** register via the `LISPCMD(name, fn, doc)` family (alcove.c:417)
  in `lispProcList[]`; the `fn` is `name##cmd`, the `doc_*` string and the
  prototype live in `builtins.h` (or the owning fragment's header). New-builtin
  recipe: write `fooCmd` + `doc_foo` in the fragment, declare both in the header,
  add one `LISPCMD("foo", foocmd, doc_foo)` row, add a `test.alc` assert,
  `make gen-test-adr`.
- **Formatting:** `.clang-format` is committed; run `make fmt` (or rely on the
  `fmt-check` gate) so diffs stay minimal and predictable. C code only — the
  `*.adr`/`*.alc` corpus is formatted by `adder fmt` (`make adfmt-test`).

---

## Key invariants (do not break these silently)

- **Three execution tiers agree byte-for-byte:** AST tree-walker ≡ bytecode VM ≡
  native JIT. Enforced by `make test-all` (equiv_sweep / recur), `eval-fuzz`
  (AST=VM), `jit-fuzz` (VM=JIT). Run them after any evaluator/compiler/JIT edit.
- **A new special form needs the compiler too.** Add it to the AST evaluator AND
  to `compile_expr`'s dispatch (alcove.c) — else TCO breaks and deep recursion
  segfaults. Builtins reached only via the AST fallback (`OP_EVAL_AST`) are fine.
- **Refcounting, not GC:** `refexp`/`unrefexp`; cycles leak (documented). The
  classic bug is `unrefexp(e)` *before* `e`'s last read — build the error/result
  first, then unref. `ALCOVE_SINGLE_THREADED` swaps atomic refcounts for `++/--`.
- **Integer overflow is an error**, never a silent wrap or float promotion.
- **Errors are first-class values** (`EXP_ERROR`, code in `flags`); `error-code`
  / `error-message` / `try` inspect without re-raising. The `exp_error_t` /
  `EXP_*` / opcode enums are single-sourced via X-macros in `alcove.h` — add an
  enumerator there and its name comes along.
- **stderr-spew gate:** a fixed baseline of intentional error-recovery tests
  print to stderr by design (test-all pins the count). A *new* uncounted
  top-level error trips the gate even when "0 failed".
- **`--threads` (RESP) contract:** keyspace is concurrency-safe (lock-free +
  epoch reclamation); the command table is immutable after the reactor pool
  spawns; callbacks must be read-only w.r.t. Lisp globals. TSan-gated
  (`make resp-tsan`).

---

## ARM / cross checks

`aarch64-linux-gnu-gcc` + `qemu-aarch64` are available for validating the arm64
JIT backend and weak-memory atomics without ARM hardware:
`aarch64-linux-gnu-gcc -O2 -DALCOVE_JIT=1 -static -o /tmp/a alcove.c -lm && qemu-aarch64 /tmp/a test.alc`.
