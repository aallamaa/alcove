/* lfkv — lock-free keyspace for the RESP server.
 *
 * Open-addressed, linear probing, fixed-size (no resize). Each slot is a
 * pointer to an `lfslot_t` record holding immutable key bytes plus an
 * atomic `_Atomic(exp_t *)` value pointer and atomic expiry timestamp.
 *
 * Semantics:
 *   slots[i] == NULL                      → never reached, end of probe
 *   slots[i] == S, S->val.load() == NULL  → soft-tombstone (deleted), keep probing
 *   slots[i] == S, S->val.load() != NULL  → live entry
 *
 * Slot records are allocated once on first insert for a key and never
 * freed except at lfkv_destroy. This avoids the cost of reallocating
 * per-overwrite while keeping reads simple (key bytes are immutable —
 * no race to read them).
 *
 * Memory ordering:
 *   - acquire on slot pointer loads (slots[i])
 *   - acquire on value pointer loads (slot->val)
 *   - release on slot pointer stores (CAS)
 *   - release on value pointer stores (CAS)
 *   - relaxed on count (approximate is fine)
 *
 * Reclamation: every value swap retires the old exp_t * via epoch.h.
 * The retire freer is `unrefexp_void` which performs `unrefexp(old)` —
 * the slot's implicit ref is transferred to the retire list and released
 * once min_quiescent surpasses retire_epoch.
 *
 * Integration: callers MUST register the calling thread via
 * epoch_register() and call epoch_tick() at quiescent points (top of
 * the reactor select loop). Without ticks, retire lists grow unbounded.
 */

#ifndef ALCOVE_LFKV_H
#define ALCOVE_LFKV_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "alcove.h"

typedef struct lfslot {
  uint32_t khash;
  uint32_t klen;
  _Atomic(struct exp_t *) val;
  _Atomic int64_t expiry_us; /* 0 = no expiry */
  char key[];                /* klen bytes, no NUL */
} lfslot_t;

typedef struct lfkv {
  _Atomic(lfslot_t *) *slots;
  size_t nslots; /* power of 2 */
  size_t mask;
  _Atomic uint64_t count; /* live keys (slot present AND val != NULL) */
} lfkv_t;

/* nslots must be a power of 2. NULL on OOM. */
lfkv_t *lfkv_new(size_t nslots);

/* Tear down: frees slot records and unref's any live values. The caller
 * must guarantee no concurrent reader/writer holds any pointer into
 * this map (i.e., all peer reactors have joined). */
void lfkv_destroy(lfkv_t *kv);

/* Set: store `val` (caller transfers 1 ref). Returns 0 on success, -1
 * on table-full. On overwrite, retires the previous value. */
int lfkv_set(lfkv_t *kv, const char *k, size_t klen, struct exp_t *val);

/* Get: returns the current value pointer with refcount BUMPED for the
 * caller (so it survives epoch boundaries). Returns NULL if absent or
 * expired. Caller must unrefexp when done. */
struct exp_t *lfkv_get(lfkv_t *kv, const char *k, size_t klen);

/* Peek: borrowed pointer (no refcount bump). Valid only until the
 * caller's next epoch_tick — i.e., within a single reactor turn. Use
 * for synchronous read-only paths (cmd_get, cmd_strlen, the lookup
 * leg of CAS-loops) where the value never escapes the current event-
 * loop iteration. Saves two atomic ops per call vs lfkv_get.
 *
 * Safety: epoch_retire queues frees but won't run them until ALL
 * registered reactors have ticked past the retire epoch, so the
 * borrowed pointer stays alive for the rest of this iteration even
 * if a peer SETs over the slot mid-call. */
struct exp_t *lfkv_peek(lfkv_t *kv, const char *k, size_t klen);

/* Del: returns 1 if a value was removed, 0 if absent. Retires old. */
int lfkv_del(lfkv_t *kv, const char *k, size_t klen);

/* Approximate count of live keys. */
size_t lfkv_count(lfkv_t *kv);

/* Set/clear expiry. ts_us=0 clears. Returns 1 if the key exists, 0 not. */
int lfkv_set_expiry(lfkv_t *kv, const char *k, size_t klen, int64_t ts_us);

/* Read expiry. Returns -1 if key absent, 0 if no expiry, else the ts. */
int64_t lfkv_get_expiry(lfkv_t *kv, const char *k, size_t klen);

/* SET-IF-ABSENT: insert only if no live value exists for key. Returns 1
 * on insert (caller's `val` ref transferred to slot), 0 if already
 * present (caller still owns `val`). On insert, clears any prior expiry. */
int lfkv_set_nx(lfkv_t *kv, const char *k, size_t klen, struct exp_t *val);

/* SET-IF-PRESENT: replace only if a live value exists. Returns 1 on
 * replace (old retired, new ref consumed), 0 if absent (caller keeps
 * `val`). Clears prior expiry on replace. */
int lfkv_set_xx(lfkv_t *kv, const char *k, size_t klen, struct exp_t *val);

/* Compare-and-swap: replace value only if current matches `expected`.
 * Used for INCR/APPEND-style read-modify-write loops where TTL must be
 * preserved (does NOT touch slot expiry). Returns 1 on success (old
 * retired, new ref consumed), 0 on miss (neither ref touched).
 * `expected` and `new` may both be non-NULL; caller is responsible for
 * holding the right number of refs in the failure case. */
int lfkv_cas(lfkv_t *kv, const char *k, size_t klen,
             struct exp_t *expected, struct exp_t *new_val);

/* Iteration: caller-supplied callback invoked for each live entry
 * (val != NULL, not expired). Acquire-loads each slot; values handed
 * to the callback are NOT refexp-bumped — the callback runs in the
 * caller's epoch window. If the callback wants to keep the value it
 * must refexp itself. Returning non-zero from the callback aborts
 * iteration. Iteration is NOT a consistent snapshot under writers. */
typedef int (*lfkv_iter_fn)(const char *k, size_t klen,
                            struct exp_t *val, int64_t expiry_us, void *ctx);
void lfkv_foreach(lfkv_t *kv, lfkv_iter_fn cb, void *ctx);

/* Sweep: walk slots, CAS-tombstone any with expiry_us > 0 && expiry <= now.
 * Each evicted value is retired via epoch. Returns # evicted. */
size_t lfkv_evict_expired(lfkv_t *kv);

/* Clear: tombstone every live slot, retire all values. Slot records are
 * NOT freed (key bytes leak until lfkv_destroy). For FLUSHDB. */
void lfkv_clear(lfkv_t *kv);

#endif
