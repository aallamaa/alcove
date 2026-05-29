# Alcove Feature Gap Proposal

This proposal uses the Lisp areas covered by
[Hyperpolyglot's Lisp comparison](https://hyperpolyglot.org/lisp) as a feature
checklist, then maps the missing pieces to practical Alcove and Adder
work. The goal is not to clone Common Lisp, Racket, Clojure, or Emacs Lisp; it
is to make Alcove feel complete for scripts, libraries, persistence, and RESP
work without losing the small single-binary model.

## Priorities

| Priority | Feature | Why it matters |
| --- | --- | --- |
| P0 | `load`, file IO, string utilities, exceptions | Basic scripting and library reuse need these before a larger ecosystem can grow. |
| P1 | Modules/namespaces, macro polish, regex/JSON, sets, sequence protocol, complete persistence | These close the largest day-to-day gaps against mature Lisps. |
| P2 | richer numeric tower, records/objects, user concurrency | Valuable, but larger design cost and not required for current core use cases. |

## P0: Load And Library Reuse

Current state: first-step `(load "file.alc")` exists for Alcove source files and
evaluates into the current environment. Adder can call it as `load
"file.alc"`. Remaining gaps are scoped/module loading, loaded-file tracking,
cycle detection, and direct `.adr` loading from plain Alcove.

Proposed Alcove API:

```lisp
(load "lib.alc")                 ; read and evaluate file in current env
(load "lib.alc" env)             ; optional future scoped load
(loaded-files)                   ; list absolute paths loaded this process
```

Proposed Adder:

```text
load "lib.alc"
```

Implementation notes:

- Implemented: reuse the existing reader/evaluator path used for script
  execution.
- Resolve relative paths from the current working directory first; later add a
  library path list.
- Detect recursive load cycles and return a readable error.
- Add tests for relative load, nested load, missing file, and idempotent
  repeated loads.

## P0: General File IO

Current state: whole-file helpers exist for text and binary data:
`read-string`, `write-string`, `append-string`, `read-lines`, `file-exists?`,
`read-bytes`, and `write-bytes`. There is still no stream/port abstraction.

Proposed Alcove API:

```lisp
(read-string "path")
(write-string "path" "text")
(append-string "path" "text")
(read-lines "path")
(file-exists? "path")
```

Later stream API:

```lisp
(with-open-file f "path" "r" body...)
(read-line f)
(write f "text")
(close f)
```

Implementation notes:

- Implemented: whole-file helpers; they are simpler and cover scripts.
- Implemented: blobs for binary and strings/lists for text.
- Keep errors explicit: missing files should produce an error object, not
  silent `nil`, unless a default argument is supplied.
- Add Adder examples using `:` blocks once `with-open-file` exists.

## P0: Exceptions And Error Handling

Current state: internal errors exist, but user code has no `try`/`catch`,
`throw`, or `finally` equivalent.

Proposed Alcove API:

```lisp
(throw "message")
(try expr
  (catch err handler...)
  (finally cleanup...))
```

Smaller first step:

```lisp
(catch expr fallback-fn)
```

Implementation notes:

- Start with non-local jump handling around evaluator errors.
- Represent caught errors as dicts: `(hash-map "type" ... "message" ...)`.
- Ensure cleanup works with refcounts and does not leak partially evaluated
  expressions.
- Tests must cover nested `try`, cleanup on success and failure, and error
  values crossing function calls.

## P0: String Utilities

Current state: strings can be indexed and mutated, and first-step utilities now
cover `str`, `substr`, `string-append`, `string-split`, `string-join`,
`string-trim`, `string-upcase`, and `string-downcase`. `format` remains open.

Proposed Alcove API:

```lisp
(str x ...)
(substr s start end)
(string-append s ...)
(string-split s sep)
(string-join xs sep)
(string-trim s)
(string-upcase s)
(string-downcase s)
(format fmt arg...)
```

Implementation notes:

- Implemented: `str`, because it enables better messages, file paths, and
  scripts.
- Keep `format` deliberately small at first: `%s`, `%d`, `%f`, `%%`.
- Implemented: `string-split` uses literal separators; regex splitting can come
  after regex support.
- Adder should use the same calls; no special syntax needed.

## P1: Modules And Namespaces

Current state: top-level `def` and assignment land in the current top-level
session environment. Function parameters and locals are lexical and do not
become top-level bindings. The missing piece is a named module/namespace layer
above the top-level environment.

Proposed Alcove API:

```lisp
(module name body...)
(require name)
(export symbol...)
(in-module name)
```

Alternative smaller API:

```lisp
(namespace "math")
(def square (x) (* x x))
(use "math")
```

Implementation notes:

- Do not add modules before `load`; module loading depends on file loading.
- Keep top-level interop explicit: imported symbols should be copied or aliased
  intentionally, not silently shadow existing session bindings.
- Persistence should record module-qualified names once modules exist.
- Adder can map `module name:` and `export x y` to the same forms.

## P1: Macro Completeness

Current state: Alcove has `defmacro`, quasiquote, unquote, `macroexpand-1`, and
`eval`. It does not yet provide unquote-splicing or `gensym`, which are common
tools for writing non-trivial Lisp macros.

Proposed Alcove API:

```lisp
`(list ,x ,@xs)
(gensym)
(gensym "prefix")
```

Implementation notes:

- Teach the reader to produce a distinct form for `,@expr`.
- Extend the quasiquote expander to splice list elements into the containing
  list.
- Make `gensym` produce unreadable or globally unique symbols that cannot clash
  with normal source names.
- Add tests using variadic macro expansion, nested quasiquote, and hygiene.

## P1: Generic Sequence Protocol

Current state: lists, vectors, strings, dicts, deques, and blobs have separate
operations. Some builtins are generic, but the shape is incomplete.

Proposed Alcove API:

```lisp
(empty? x)
(first x)
(rest x)
(take n xs)
(drop n xs)
(sort xs)
(range start end)
(seq x)
```

Implementation notes:

- Preserve existing `car`/`cdr` behavior for pairs.
- Implement `first`, `count`, and `empty?` over all collection types.
- `sort` should accept an optional comparator function.
- Add tests for every collection type and empty inputs.

## P1: Complete Persistence For First-Class Containers

Current state: persistence is a core Alcove feature, and vectors/blobs already
round-trip through the unified dump format. Hash maps and deques are first-class
runtime values but are not currently registered as dump/loadable expression
types.

Proposed API: no new user-facing API. Existing forms should work:

```lisp
(= d (hash-map "k" 1))
(= q (deque 1 2 3))
(persist 'd)
(persist 'q)
(savedb)
```

Implementation notes:

- Add `EXP_DICT` dump/load support with recursive dumping of keys and values.
- Add `EXP_LIST` deque dump/load support that preserves order.
- Decide whether dict keys remain restricted to the current key encoding or
  round-trip arbitrary expression keys.
- Tests must cover nested dict/deque/blob/vector values, local-scope `persist`,
  and RESP bridge values stored via `redis-set`.

## P1: Sets

Current state: `EXP_SET` exists as a hash-backed set using typed identity keys
plus stored element values. Alcove has `(set ...)`; Adder should use
`hash-set` because `set` is assignment syntax there.

Proposed Alcove API:

```lisp
(set 1 2 3)
(set? x)
(set-add! s x)
(set-del! s x)
(set-has? s x)
(set-union a b)
(set-intersection a b)
(set-difference a b)
(set->list s)
```

Implementation notes:

- Implemented: an `EXP_SET` backed by the same dictionary core, but with a
  set-only typed key encoding and the original scalar/blob value stored in the
  value slot. This keeps dict key semantics unchanged while making set
  membership distinguish `1`, `"1"`, and symbols with the same printed name.
- Implemented: mutable updates plus non-mutating union/intersection/difference.
- Implemented: set persistence through `savedb`/`loaddb`.
- RESP set compatibility can come later.

## P1: Regex And JSON

Current state: no regular expression or JSON support; docs recommend FFI for
missing systems features.

Proposed Alcove API:

```lisp
(regex-match pattern s)
(regex-find pattern s)
(regex-replace pattern repl s)
(regex-split pattern s)
(json-parse s)
(json-stringify x)
```

Implementation notes:

- Prefer a small embeddable engine if portability matters.
- If using POSIX regex, document platform differences.
- Add literal-string fallback paths for `string-split` and `string-replace`
  so users do not pay regex cost for simple cases.
- Map JSON objects to `hash-map`, arrays to vectors, booleans to `t`/`nil`, and
  JSON null to `nil`.
- Reject blobs and functions in `json-stringify` with clear errors.

## P1: Adder Polish

Current state: ALS supports indentation, `:`, `name(args)` call sugar, `#`
comments, `macro` to `defmacro`, and `set` to `=`.

Proposed work:

```text
import "lib.adr"
module math:
  export square

def square(x):
  * x x
```

Implementation notes:

- Add source locations to ALS reader errors.
- Add a formatter or canonical emitter for ALS.
- Add shebang tests for `.adr` scripts.
- Keep the homoiconic guarantee: every ALS feature should lower to ordinary
  Alcove forms before macro expansion.

## P2: Numeric Tower

Current state: Alcove has fixnums and floats; integers wider than the fixnum
range become floats.

Options:

- Add arbitrary precision integers using a small bigint library.
- Add ratios only after bigint support.
- Leave complex numbers to libraries or FFI unless numeric computing becomes a
  primary goal.

Suggested API:

```lisp
(integer? x)
(float? x)
(bigint? x)
(ratio? x)
```

Implementation notes:

- This touches parser, printer, arithmetic, equality, persistence, and RESP
  serialization. Treat it as a separate milestone.

## P2: Records And Objects

Current state: no structs, records, CLOS-style objects, or protocols. Users use
hash maps, vectors, and deques.

Proposed small record API:

```lisp
(defrecord Point (x y))
(Point 1 2)
(.x p)
(= (.x p) 3)
```

Alternative Lispier API:

```lisp
(record Point (x y))
(point-x p)
```

Implementation notes:

- Start with records, not a full object system.
- Records should print readably and persist safely.
- Consider hash-map compatibility so existing dict functions can inspect them.

## P2: User-Level Concurrency

Current state: the RESP server is multithreaded internally, but user Lisp does
not expose threads, futures, atoms, agents, or channels.

Proposed API:

```lisp
(future expr)
(deref f)
(atom x)
(swap! a fn arg...)
(reset! a x)
```

Implementation notes:

- Only expose this after the evaluator/refcount story is audited for concurrent
  user code.
- A safer first step is a worker pool for isolated tasks that exchange blobs or
  strings, not arbitrary shared `exp_t` graphs.

## Acceptance Plan

1. Implement P0 in this order: `str`, file whole-read/write helpers, `load`,
   minimal `try`/`catch`.
2. Add comparison-doc examples for every new API.
3. Add unit tests in `test.alc` and ALS fixtures where syntax changes.
4. Run `make test`, `alcove --noload test.alc`, `make benchmark-jit`, and
   RESP benchmarks when a feature touches persistence or RESP values.
5. Update `docs/alcove-language.md` so the "Things alcove does not have"
   section stays honest after each feature lands.
