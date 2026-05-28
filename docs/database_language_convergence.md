# Database / language convergence in alcove

This document explains how alcove's **Lisp variable environment** and its
**Redis-compatible keyspace** relate to each other: what is shared today,
what is deliberately kept apart and why, and how `(with-db n ...)` lets Lisp
code address numbered databases. It is sourced from `alcove.c`, `resp.c`,
and `lfkv.{c,h}`, and reflects the interpreter as it actually runs.

The short version: alcove keeps **two stores** that already share a value
type and a bridge, and it makes the keyspace ergonomic from Lisp via
`with-db` — but it does **not** merge the variable environment into the
keyspace, because their concurrency and lifetime models are incompatible.

---

## 1. The two stores

| | Variable environment | Keyspace (Redis db) |
|---|---|---|
| Backing | `env_t` chain → root `dict_t *d` (+ inline slots) | `lfkv_t` (lock-free, sharded) |
| Created by | `def`, `(= x v)` at top level, `let`/`with` (locals) | `SET`/`GET` over RESP, or `redis-set`/`redis-val` from Lisp |
| Keys | symbol names | arbitrary byte strings (binary-safe) |
| Values | any `exp_t` (incl. lambdas, closures, vectors) | any `exp_t` (resp.c stores native `exp_t`) |
| Scope | a **chain** — locals shadow globals | **flat**, and now **numbered** (db 0..15) |
| Concurrency | single-threaded, env-arena, refcounted | **lock-free, sharded, epoch-reclaimed** |
| Lifetime | refcount + arena | epoch reclamation; entries carry TTL |

Two things are **already converged**:

1. **Values are the same type.** Both stores hold `exp_t *`. resp.c was
   migrated to native `dict_t`/`exp_t`, so a Redis value and a Lisp value
   are the same object kind — no marshalling boundary between them.
2. **There is a bridge.** `redis-set` / `redis-val` / `redis-del` /
   `redis-count` / `redis-keys` let Lisp read and write the keyspace
   in-process, without opening a socket. `savedb` persists it.

So "convergence" here is not about unifying the *storage* — it is about
making the keyspace a first-class, ergonomic part of the language while
keeping the two stores' very different runtime models intact.

---

## 2. Why not merge the environment into the keyspace

It is tempting to make `(= x 5)` simply be `SET x 5` — one store, one
namespace, `savedb` persists everything. The blocker is **not** plumbing
(the values are already the same type); it is the runtime model.

- The keyspace (`lfkv`) is built for **concurrent** server access: lock-free,
  sharded across reactor threads, epoch-reclaimed. The variable environment
  is **single-threaded**: an env arena, plain refcounting, a chain of
  scopes, and a per-symbol resolution cache (`exp_t.meta`).
- The values that live in the environment are not all safe to share with
  server threads. A **closure** captures a single-shard env arena; a
  **vector** must not be reallocated once it is `FLAG_SHARED`; refcounting
  is not the epoch model. Exposing the whole global namespace — including
  lambdas and growable containers — to concurrent RESP clients would be a
  thread-safety minefield.

Merging would therefore require either making the entire value model
thread-safe (a large undertaking) or restricting what may live in the
shared namespace (at which point it is no longer "the environment").

**Decision:** keep the right separation — *environment = code + locals
(single-threaded, refcounted); keyspace = data (concurrent, persistent,
selectable)* — and converge at the **ergonomics** layer instead.

---

## 3. Numbered databases and `(with-db n ...)`

alcove exposes redis-style numbered databases (db `0`..`15`) over the
in-process keyspace. The database for keyspace operations is chosen for a
**dynamic extent** by `with-db`:

```lisp
(redis-set "k" "zero")                 ; db 0 — the default / shared db
(with-db 1 (redis-set "k" "one"))      ; writes "k" into db 1
(redis-val "k")                        ; => "zero"  (db 0 untouched)
(with-db 1 (redis-val "k"))            ; => "one"   (db 1)
(with-db 5 (redis-val "k"))            ; => nil     (db 5 is empty / isolated)
```

### Semantics

- **Dynamic scope.** Inside the body — and inside any function the body
  calls — `redis-set` / `redis-val` / `redis-del` / `redis-count` /
  `redis-flush` and the rest of the bridge target db `n`.
- **Restored on exit, including on error.** The previous database is put
  back when the body returns *or* raises:

  ```lisp
  (error? (with-db 1 (car 5)))   ; => t
  (redis-val "k")                ; => "zero"  (db 0 active again)
  ```
- **Nests.** The inner form's database is unwound on the way out:

  ```lisp
  (with-db 1
    (with-db 2 (redis-set "k" "two"))
    (blob->string (redis-val "k")))   ; => "one"  (back in db 1)
  ```
- **Returns the last body value**, like `do`.
- **Out-of-range indices error** (`0..15`).
- **db 0 is the shared keyspace** the RESP server uses; **1..15** are
  separate, lazily-created tables.

### A note on "closures"

`with-db` is **dynamic** scope, which matches "a scope where db *n* is
active." It is *not* lexical capture: a function defined inside
`(with-db 1 ...)` and called *later, outside* it sees the ambient
database, not db 1. If you want a function that *permanently* binds a
database wherever it is called, that is a small follow-up — a wrapper that
re-enters `with-db n` on each invocation (see §6).

---

## 4. How it works: one db-aware chokepoint

The implementation is deliberately small because every keyspace wrapper
already funnels through a single function, `resp_kv_current()` (resp.c).
Making *that* database-aware made the whole bridge database-aware.

- A **thread-local** selector, `alcove_kv_db` (alcove.c), holds the
  current database index (default `0`).
- `resp_kv_current()` consults it: db `0` returns the shared `g_resp_kv`;
  higher indices return `g_resp_db_extra[n]`, created on first use with the
  same compare-and-swap pattern as `g_resp_kv`.
- `(with-db n ...)` (`withdbcmd`) saves `alcove_kv_db`, sets it to `n`,
  evaluates the body, and restores it — even on error.

Two properties fall out of this design:

- **The selector is thread-local**, so a REPL thread's `with-db` cannot
  perturb the server's reactor threads. They always observe db 0. There is
  no change to the concurrent hot path.
- **Higher databases are lazily allocated**, so programs that never call
  `with-db` pay nothing.

---

## 5. What is *not* converged (yet)

- **The server's `SELECT` is db-0 only.** Over the wire, `SELECT 0`
  succeeds and any other index is refused. Wiring `SELECT` to the same
  `g_resp_db_extra[]` array (with a per-connection current-db on
  `resp_client_t`) is a separate, server-side change.
- **`savedb` persists db 0.** Databases `1..15` are scratch space for the
  dynamic extent of `with-db`; they are not written to `db.dump`.
- **The variable environment is untouched.** `with-db` selects the
  *keyspace* database; it does not affect Lisp variable bindings. `(= x 5)`
  inside `(with-db 1 ...)` still binds the ordinary global `x`.

---

## 6. Possible next steps

- **`(db-bind n fn)` — lexical database capture.** A wrapper that makes
  `fn` always run under `with-db n`, so a closure can carry its database
  with it regardless of the ambient selection.
- **Per-connection `SELECT` for the server.** Give `resp_client_t` a `db`
  field, set it in `cmd_select`, and resolve the per-command keyspace from
  it — reusing the same `g_resp_db_extra[]` array `with-db` already drives.
- **A first-class `(db k)` accessor.** Read/write the current database with
  container syntax — `(db "k")` to get, `(= (db "k") v)` to set — layering
  dict-style access over the bridge so the keyspace *reads* like a native
  container.
- **Multi-db persistence.** Extend `savedb`/`loaddb` to round-trip all
  populated databases, not just db 0.

None of these require revisiting the §2 decision: the environment and the
keyspace stay distinct runtime models, sharing a value type and an
increasingly ergonomic bridge.
