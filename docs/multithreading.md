# Multithreading via sharded reactors (path B)

Status: design draft — no code lands without explicit approval per item.

## Goal

Take the RESP2 server multi-core without breaking alcove's invariants:
- Refcounted `exp_t*` graph, mutated in place.
- Tree-walker / VM evaluator that recurses on the C stack.
- Single global `dict_t` for the keyspace.
- Lisp callbacks via `(redis-defcmd "NAME" fn)` that reach back into
  the evaluator and may call any builtin, including `(redis-get k)`.
- `NO CLOSURES` invariant for the env arena.

Concurrency we want: N CPU cores serving M independent client connections
with throughput that scales sublinearly but materially with N (target:
3–4× on 4 cores for non-conflicting key access).

## Why not the simpler paths

**Path A — multi-process shared-nothing (`SO_REUSEPORT`).** Trivial to
implement (fork N workers, accept on the same port). Zero atomics. But
no shared keyspace: `SET foo 1` on worker 0 isn't visible to worker 1
without an external store. The whole point of an embedded RESP server
is shared in-process state. Rejected.

**Path C — full lock-free dict (Cliff Click NBHT, hazard pointers).**
Highest payoff on contended single keys. But our values are refcounted
mutable `exp_t*` — every `GET k` would need hazard-pointer protection
on the value pointer to safely read it before another thread mutates,
and every `SET k v` would need to coordinate the unref of the old
value with concurrent readers. RCU/epoch reclamation works but at this
scale it's a 6-month project and the steady state is "alcove is fast
enough already." Deferred indefinitely.

## Path B in one paragraph

Hash each connection's RESP key to one of N **shards**. Each shard owns
its own keyspace dict, its own select() reactor, its own evaluator
state. A connection is **pinned** to a shard for the lifetime of the
command (not the connection — see "Cross-shard ops" below). Single
writer per shard means no atomics on dict mutations and no atomics on
refcounts within a shard. Cross-shard ops use a serialized fallback.

## Architecture

```
                    ┌─────────────────────────────────────┐
  client TCP ──────►│  acceptor thread (1)                │
                    │  - listens on 127.0.0.1:6379        │
                    │  - hands cfd to a "router"          │
                    └─────────────────────────────────────┘
                                    │
                            (per-connection
                             read of first cmd
                             to decide shard)
                                    ▼
        ┌─────────────────┬─────────────────┬─────────────────┐
        │  shard 0        │  shard 1        │  shard N-1      │
        │  ┌──────────┐   │  ┌──────────┐   │  ┌──────────┐   │
        │  │ reactor  │   │  │ reactor  │   │  │ reactor  │   │
        │  │  loop    │   │  │  loop    │   │  │  loop    │   │
        │  ├──────────┤   │  ├──────────┤   │  ├──────────┤   │
        │  │ dict_t * │   │  │ dict_t * │   │  │ dict_t * │   │
        │  │ exp_t*…  │   │  │ exp_t*…  │   │  │ exp_t*…  │   │
        │  │ env_t  * │   │  │ env_t  * │   │  │ env_t  * │   │
        │  └──────────┘   │  └──────────┘   │  └──────────┘   │
        │  pthread_t      │  pthread_t      │  pthread_t      │
        └─────────────────┴─────────────────┴─────────────────┘
```

Each shard is a separate pthread running a copy of `resp_serve`'s
inner loop. Shards do **not** share `resp_clients`, `g_global_env`,
or any `exp_t*` (with one exception, see "Lisp globals").

## Sharding key

**Decision: hash on the RESP key (first arg of most commands), not on
fd.**

Rationale: hashing on fd is dead simple but defeats the point — two
connections both pounding `key:hot` will land on different shards if
fds happen to differ, so the shared dict illusion has to span shards
again. Hashing on the key keeps each key's home shard stable, which
makes intra-shard ops fully lock-free.

Implementation: in the parsed-RESP-command stage, before dispatch,
read `argv[1]` (the key for SET/GET/INCR/LPUSH/HSET/...) and route:

```c
shard_id = bernstein_hash(argv[1], argl[1]) & (N - 1);
```

Commands without a key (PING, COMMAND, FLUSHDB, KEYS *, DBSIZE) are
**broadcast or aggregated** — see below.

## Per-shard data

```c
typedef struct shard {
  int id;
  pthread_t tid;

  /* reactor state — private */
  resp_client_t *clients;     /* clients currently steered to this shard */
  fd_set rfds, wfds;

  /* keyspace — single-writer (this thread), readers within thread */
  dict_t *db;

  /* evaluator state — separate per-shard env/arena */
  env_t *global;
  exp_arena_t *exp_arena;     /* exp_t freelist, bump alloc */
  env_arena_t *env_arena;

  /* cross-shard inbox: lock-free MPSC queue of work to execute on
     this shard's thread. Producers are other shards; consumer is this
     shard's reactor. */
  mpsc_queue_t inbox;
  int inbox_eventfd;          /* select()-able wake-up */
} shard_t;

static shard_t shards[N_SHARDS];
```

Refcount macros revert to plain `++/--` within a shard
(`ALCOVE_SINGLE_THREADED=1` semantics) because no two threads ever
touch the same `exp_t*` simultaneously (modulo the cross-shard
exception, see below).

## Connection lifecycle

1. Acceptor thread accepts, reads the first complete RESP command,
   parses it, looks at `argv[1]` if any.
2. Acceptor enqueues `(client_fd, parsed_cmd)` into `shards[shard_of(key)].inbox`.
3. Target shard's reactor picks it up, takes ownership of the fd, adds
   to its `clients` list.
4. Subsequent commands from that fd are read by the target shard's
   reactor — but a command with a **different key** may need to hop
   shards (see "Per-command vs per-connection pinning").

### Per-command vs per-connection pinning

**Per-connection pinning (cheap, broken):** once a connection lands on
shard 0, all its commands stay on shard 0 even when they target keys
owned by shard 1. shard 0 then needs to either lock shard 1's dict or
synchronously RPC into shard 1. Both defeat the locality model.

**Per-command pinning (correct, more plumbing):** each parsed command
ships to the shard owning its key. The current connection's reactor
goes back to reading; the response from the target shard is enqueued
back via a per-connection response queue, and the home reactor flushes
it. This requires:

- An MPSC inbox per shard for inbound commands.
- An MPSC outbox per **connection** for ordered responses (RESP is
  pipeline-ordered; out-of-order replies break clients).
- A "home reactor" per connection that owns the wbuf and the socket
  write. Other shards never touch the wbuf.

**Decision: per-command pinning, with the connection's home reactor
chosen at accept time (e.g. round-robin) and stable for the connection
lifetime.** Home reactor handles read, parses, dispatches to owner
shard, drains responses from outbox, writes.

Pipeline ordering invariant: each command parsed on the home reactor
is tagged with a monotonic seq number. The home reactor's outbox is a
priority queue keyed by seq; it only writes responses in seq order, so
slow shards block the wbuf flush behind them. Backpressure naturally
propagates.

## Cross-shard operations

| Command | Strategy |
|---|---|
| `SET k v`, `GET k`, `INCR k`, single-key ops | route to `shard_of(k)`. Default path. |
| `MGET k1 k2 ...` | split by shard, scatter, gather, reorder by request index. |
| `MSET k1 v1 k2 v2 ...` | split by shard, scatter, **no atomicity guarantee** (already true in alcove single-threaded since we don't have transactions). |
| `KEYS *`, `DBSIZE`, `SCAN` | broadcast: enqueue on every shard inbox, gather. |
| `FLUSHDB`, `FLUSHALL` | broadcast. Each shard flushes its own dict. |
| `DEL k1 k2 ...` | split by shard. |
| `EXPIRE k`, `PEXPIRE k`, `TTL k`, `PERSIST k` | route to `shard_of(k)`. |
| `LPUSH q v`, `LRANGE q ...`, all list ops | route to `shard_of(q)`. The list value lives entirely on one shard. |
| `HSET h f v`, all hash ops | route to `shard_of(h)`. |
| `(redis-defcmd ...)` user commands | see below. |

## Lisp globals — the only shared state

`reserved_symbol`, the builtin function table, and `(redis-defcmd)`-
registered user commands need to be visible to every shard, but they
are **read-mostly**.

**Strategy:** initialize the global symbol dict and `lispProcList`
before any shard thread starts. After that, treat them as immutable
from shard threads. `(redis-defcmd)` and `(def)` from a connection-
issued REPL form mutate them — that path is rare and can take a
coarse `pthread_rwlock_t` write lock; reads are wait-free under
`pthread_rwlock_rdlock` (or, on platforms with seqlock support, a
seqlock for true wait-free reads).

`g_global_env`: one shared global env, but writes to it (def, defmacro,
updatebang on a global key) take the same rwlock. Reads are protected
by holding the read lock for the duration of the lookup chain — this
is correct because a writer can't free env entries that a reader is
walking.

**`(redis-defcmd "FOO" fn)` evaluation:**
- Lookup of `FOO` in the user-cmd table: rwlock read.
- Evaluation of `fn`: runs on the shard that owns the requested key,
  using **that shard's** env arena. The closure body and its constants
  are shared via the global env, but every per-call `env_t` allocation
  comes from the shard-local arena. Refcount writes on the closure's
  param/let slots stay shard-local because the env is shard-local.
- Shared exp_t* (the closure body itself, its symbol/literal nodes)
  must use atomic refcount. Compromise: any `exp_t*` reachable from
  `g_global_env` flips to atomic refcounting; per-shard intermediate
  values stay non-atomic. Distinguishing them at runtime needs either
  a per-exp_t flag bit or two separate allocators (see "Refcount
  duality").

## Refcount duality

The hardest part. Today every `exp_t*` uses the same `REFCOUNT_INC/DEC`
macros. Path B needs:

- **Shard-local exp_t*** (the vast majority — closure bodies parsed
  per call, intermediate values during eval): plain `++/--`.
- **Shared exp_t*** (anything reachable from the global env or the
  user-cmd table): atomic.

**Option 1 — flag bit.** Add `EXP_FLAG_SHARED` to `e->flags`. The
refcount macros become:
```c
#define REFCOUNT_INC(e) ((e)->flags & EXP_FLAG_SHARED \
    ? __sync_add_and_fetch(&(e)->nref, 1) : ++(e)->nref)
```
Cost: a load+test+branch on every refcount op. Branch is well-predicted
(99% shard-local in steady state) but still 1-2 cycles in the hot path.

**Option 2 — two arenas.** Shard-local and shared arenas have different
addresses; we can derive `is_shared(e)` from `e`'s page or from a bit
of the address (e.g. high bit of arena base). Cleaner runtime check
but requires an arena-design pass.

**Option 3 — write barrier on assignment to global env.** When
`set_get_keyval_dict(g_global_env, ...)` is called, walk the value's
exp_t graph and flip every node to shared/atomic refcounting. Keeps
the hot path branch-free. Cost: graph walk on every global mutation
(rare). Implementation: a "promote to shared" function that recursively
flips the flag and re-counts via atomics from then on.

**Decision: Option 3 with the flag bit (Option 1) as the runtime
check.** Promotion is rare. Steady-state hot path: one branch, well-
predicted. This gets the benefit without the arena rewrite.

## The MPSC inbox/outbox

Multiple producer single consumer queue, lock-free. The standard
implementation: a singly-linked list with atomic head pointer for
producers, a separate stack-of-popped-items for the consumer to drain
in batches.

```c
typedef struct mpsc_node {
  struct mpsc_node *next;
  void *payload;
} mpsc_node_t;

typedef struct {
  _Atomic(mpsc_node_t *) head;   /* producers CAS here */
  mpsc_node_t *consumer_local;   /* drained by consumer, no atomics */
  int eventfd;                   /* writes wake up the consumer */
} mpsc_queue_t;

void mpsc_push(mpsc_queue_t *q, void *p) {
  mpsc_node_t *n = malloc(sizeof *n);
  n->payload = p;
  mpsc_node_t *old;
  do { old = atomic_load(&q->head); n->next = old; }
  while (!atomic_compare_exchange_weak(&q->head, &old, n));
  uint64_t one = 1; write(q->eventfd, &one, 8);  /* wake reactor */
}
```

Consumer (the shard's reactor) calls `mpsc_drain` after select() wakes
on `eventfd`, which atomically swaps the head with NULL and walks the
list in producer-LIFO order. We reverse it to get FIFO.

**eventfd is Linux-only.** macOS path: `pipe()` with the read end in
the reactor's `rfds`. Same shape.

## Shard count

Default `N = ceil(num_cores * 0.75)`, min 1, max 16. Configurable via
`-r-shards N` flag.

Single-shard mode (`N=1`) is identical to today's behavior. Useful for
debugging and as the migration's first step (the shared infra works
with N=1, just doesn't help).

## Rollout plan

1. **Step 0 — refcount audit.** Today's `__sync_*` macros are still
   live in the multi-thread build. Verify every `e->nref` access goes
   through them; verify the freelist (`exp_freelist`) is per-shard,
   not global. (It's currently global. Easy fix: TLS variable.)
2. **Step 1 — single-shard scaffold.** Land `shard_t`, `mpsc_queue_t`,
   per-shard freelist, accept-and-route. N=1 shard. Must match
   today's perf within ±5%.
3. **Step 2 — promote-to-shared write barrier** on global-env writes.
   Ship Option 1+3 from "Refcount duality." Validate via a 2-shard
   test that mutating an exp_t reachable from the global env from one
   shard while another reads it doesn't crash.
4. **Step 3 — N-shard with key-based routing.** Cross-shard MGET/DEL,
   broadcast for KEYS/FLUSHDB/DBSIZE.
5. **Step 4 — Lisp callback path.** `(redis-defcmd)` runs on owning
   shard, with reads of the global env protected by the rwlock.
6. **Step 5 — connection-home reactor + per-connection ordered
   outbox.** Pipeline ordering correctness.
7. **Step 6 — benchmark.** Target: 4-shard ≥ 3× single-shard on a
   key-distributed workload (`redis-benchmark -t SET,GET -r 100000`).

Each step is its own commit and its own benchmark gate. If step 6
doesn't show the win, we revisit before merging.

## Risks

- **Promote-to-shared races.** A symbol cached via `e->meta` may be
  promoted on one shard while another shard is mid-read. Need a
  rcu-style one-time fence after promotion before the new shared
  pointer becomes visible. Or: do the promotion eagerly at startup
  (parse all initial Lisp before any shard starts) and forbid runtime
  promotion (no `(def)` from running connections).
- **GC pauses.** No GC today (refcount only) so this is a non-issue —
  but if cycle collection ever lands, it has to be per-shard or stop-
  the-world.
- **`(redis-defcmd)` callback that reads OTHER keys.** A user command
  pinned to shard A that calls `(redis-get "key-on-shard-B")` from its
  body. Today this is one C call. Under path B it becomes a cross-
  shard RPC with a blocking wait — easy to implement (sync on a
  per-call eventfd), but the latency floor jumps from ns to µs. May
  motivate a "small" cache of shared read-mostly keys.
- **TTL sweep.** Currently a single 1s tick that walks the global
  dict. Under path B each shard sweeps its own dict, so the sweep
  scales linearly with N — strict win.

## What we are NOT doing

- Lock-free dict (path C). Per-shard dict is single-writer, no atomics
  needed.
- Async/await reactor rewrite. Reactors stay select()-based; only the
  inbox/outbox channels are concurrency primitives.
- Distributed clustering. This is in-process N-thread parallelism, not
  Redis Cluster.

## Open questions

1. What's the right default for `N` when this is embedded in a Lisp
   process that's also doing arbitrary user computation? 1 might be
   the safer default and N opt-in.
2. Should `(redis-defcmd)` callbacks be allowed to mutate the global
   env? If yes, every callback hits the rwlock write path — slow.
   If no, that's a runtime restriction we need to document and enforce.
3. `EXPIRE` precision under N-shard: each shard's sweep is independent,
   so a key may live 2 ticks longer than expected on some shards. The
   1s sweep already has 1s slop, so this is fine — call it out in
   docs.

---

## Step 0 — audit results (2026-04-29)

Inventory of refcount accesses and global state that has to move before
multi-shard scaffolding can land. Findings split into "safe as-is",
"needs fix", and "needs sharding."

### Refcount: direct `nref` writes (bypass `REFCOUNT_INC/DEC`)

| Site | Kind | Verdict |
|---|---|---|
| `alcove.c:519` `newenv->nref = 1` in `make_env` | initial store, fresh env | **Safe** — env is not visible to any other thread until `ref_env`/install. |
| `alcove.c:843` `nil_exp->nref = 1` in `make_nil` | initial store, fresh exp | **Safe** — same reasoning. |
| `alcove.c:4894` `v->nref` printf in `inspect_value` | diagnostic read | **Tolerable** — torn read at worst, debug-only. |

These three are *publication writes* and stay non-atomic.

### Refcount: JIT-emitted load/inc/store (NON-ATOMIC)

The JIT inlines `refexp` and `unrefexp` as plain word-size load/add/store.
This is **incorrect under multi-thread** for any `exp_t` that has been
promoted-to-shared (Option 3 from "Refcount duality"). Sites:

| Backend | Function | Lines |
|---|---|---|
| arm64 | `try_jit_is_prime_given` | 6828–6840 |
| arm64 | `try_jit_safe_p` | 7009–7022 |
| x86_64 | `try_jit_safe_p` | 8957, 8979 |
| x86_64 | `try_jit_is_prime_given` | 9184, 9207 |

Resolution under path B: each JIT'd lambda runs on its lambda-home shard,
so its environment slots and locals are single-threaded — non-atomic
ops stay correct. The unsafe case is when a JIT'd function dereferences
an exp reachable through the global env (a shared exp). The
promote-to-shared write barrier marks shared exps with a flag bit
(`EXP_FLAG_SHARED`). The JIT must:

1. Before each `refexp`/`unrefexp` inline, test the flag bit on the target.
2. If unset → keep the fast non-atomic path.
3. If set → branch to a deopt stub (fall back to bytecode, which uses the
   `__sync_*` macros).

Cost: one TBZ + one branch per inline refop. Same shape as the existing
`is_ptr` / nil / true tag-check skips, so it folds into the predictor
the same way. `try_jit_for_loop_inc` and the others that don't touch
nref are unaffected.

### Globals to convert to per-shard / TLS

| Global | Line | Purpose | Plan |
|---|---|---|---|
| `exp_freelist` | 383 | exp_t recycling free-list | `__thread` (TLS, one per shard worker). |
| `exp_bump_next`, `exp_bump_left` | 391, 392 | exp_t bump-alloc fallback | `__thread` together with freelist. |
| `env_arena[]`, `env_arena_sp` | 485, 486 | env_t LIFO arena | per-shard (move into `shard_t`; `make_env` takes shard ptr). |
| `in_tail_position` | 55 | TCO marker for `evaluate` | `__thread` — per evaluator stack. |
| `alcove_load_depth` | 1182 | recursive `load` guard | `__thread` — depth is caller-local. |

Note on env_arena: TLS is fine for the freelist (a 16-byte pointer pair),
but the arena is 8192 × `sizeof(env_t)` ≈ 1 MB per shard. Putting it in
TLS bloats the executable's TLS section. Better: store it in `shard_t`
and thread the shard pointer through `make_env`/`destroy_env`.

### Globals that stay global

| Global | Line | Why it's safe |
|---|---|---|
| `nil_singleton`, `true_singleton` | 45, 46 | Allocated once in `main`; refexp/unrefexp short-circuit so nref is never touched. |
| `reserved_symbol` | 41 | Init-once read-only dict. |
| `exp_tfuncList` | 42 | Init-once dispatch table. |
| `lispProcList` | 158 | const initializer. |
| `bernstein_seed` | 606 | const after init. |
| `g_global_env` | 50 | Read-mostly; protected by rwlock per design above. |

### Globals needing atomicization (not sharding)

| Global | Line | Why | Fix |
|---|---|---|---|
| `alcove_global_gen` | 494 | Bumped from 6 sites on global mutation; read by every gcache check. Currently `uint64_t++` — torn under SMP. | `__sync_add_and_fetch(&alcove_global_gen, 1)` on every bump; reads stay plain (a stale read just forces a re-resolve, which is correct). |
| `g_ffi_libs` | 3276 | Linked-list mutated on `(ffi-fn)` calls. | Mutex (rare path; FFI calls are not hot). Or per-shard cache (libs are idempotent; double-load is fine). |

### Items already correct

- `__sync_add_and_fetch` / `__sync_sub_and_fetch` are the right primitives;
  they imply full barriers on both arm64 and x86_64. No need to switch to
  C11 `atomic_*` for the macros.
- `jit_alloc()` mmap is per-bytecode and read-only after `jit_write_end`.
  No global JIT cache to protect.
- The ENV_INLINE_SLOTS hot path (`make_env` → `inline_vals[i]`) is single-
  writer per env, and an env is single-shard, so inline-slot stores stay
  plain.
- `bytecode_t.gcache[].gen` is per-bytecode and only written by the
  owning shard's evaluator, so the gcache check
  `gcache[i].gen == alcove_global_gen` works as long as the read of
  `alcove_global_gen` is monotonic — `__sync_add_and_fetch` guarantees that.

### Punch list for Step 1

In commit-sized chunks, ordered for bisectability:

1. Convert `exp_freelist`, `exp_bump_next`, `exp_bump_left` to `__thread`.
   Run benchmarks — must match within ±1%. (Single-thread today, so TLS
   adds an addressing mode but no contention.)
2. Convert `in_tail_position` and `alcove_load_depth` to `__thread`.
   Trivial.
3. Wrap every `alcove_global_gen++` in `__sync_add_and_fetch`. 6 sites.
4. Introduce `shard_t` with `env_arena[ENV_ARENA_SLOTS]` and
   `env_arena_sp` as members. Thread `shard_t *self` through
   `make_env`/`destroy_env`. (Or use TLS-pointer-to-shard if signature
   churn is too painful. Decide before this step.)
5. `g_ffi_libs`: add a mutex around the linked-list mutation. Minimal
   footprint.

After (1–5), the codebase compiles to a binary that's still single-shard
but has zero non-shard global mutable state outside the `g_global_env`
rwlock + `alcove_global_gen` atomic. That's the precondition for Step 1's
shard scaffold.
