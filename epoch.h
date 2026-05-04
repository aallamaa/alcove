/* epoch.h — epoch-based memory reclamation for the lock-free keyspace.
 *
 * Mental model: every reactor periodically advertises its "quiescent
 * epoch" — a monotonic counter bumped at points where it is *not*
 * currently dereferencing any pointer it loaded from a lock-free slot.
 * The reactor loop top is the natural quiescent point: between select()
 * iterations, no command is in flight.
 *
 * Retiring: when a writer CAS-swaps a slot and needs to free the old
 * value, it cannot free immediately — a peer reactor may have just
 * loaded that pointer and be mid-use. Instead, the writer captures the
 * current global epoch as `retire_epoch` and queues the pointer on its
 * own retire list (TLS — no contention).
 *
 * Reclaiming: an object retired at epoch R is safe to free once
 * min(quiescent[i] for all registered reactors) > R. Each reactor scans
 * its own retire list at gc time and frees eligible entries.
 *
 * Correctness: a reader at tick time T loads quiescent = G(T). If a
 * writer retires P at G_retire >= G(T), but the reader had already
 * loaded P before the writer's CAS, the reader's quiescent is still
 * G(T) <= G_retire, so the free condition retire_epoch < min(quiescent)
 * fails. The reader's next tick advances quiescent past G_retire, and
 * the next gc scan can reclaim.
 *
 * Bounding retire-list growth: epoch_tick triggers epoch_gc when the
 * caller's retire_count exceeds EPOCH_GC_THRESHOLD.
 *
 * Threads that don't read the lock-free dict should not register.
 * Registered threads must continue to tick (or set quiescent to
 * UINT64_MAX before exiting) or gc stalls forever.
 */

#ifndef ALCOVE_EPOCH_H
#define ALCOVE_EPOCH_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "alcove.h"

#define EPOCH_MAX_THREADS 64
#define EPOCH_GC_THRESHOLD 1024

typedef void (*epoch_freer_t)(void *);

typedef struct epoch_retire {
  struct epoch_retire *next;
  void *ptr;
  epoch_freer_t freer;
  uint64_t retire_epoch;
} epoch_retire_t;

/* Cache-line aligned to avoid false sharing between reactor slots. */
typedef struct epoch_thread {
  _Atomic uint64_t quiescent; /* 0 = unregistered */
  epoch_retire_t *retire_head; /* TLS-owned: only this slot's owner reads/writes */
  size_t retire_count;
  char _pad[64 - sizeof(_Atomic uint64_t) - sizeof(void *) - sizeof(size_t)];
} __attribute__((aligned(64))) epoch_thread_t;

extern epoch_thread_t epoch_threads[EPOCH_MAX_THREADS];
extern _Atomic uint64_t epoch_global;
extern _Atomic int epoch_nthreads;
extern ALCOVE_TLS int epoch_my_idx; /* -1 until epoch_register is called */

/* Register caller as a reactor. Returns slot index (>= 0) or -1 on
 * overflow. Idempotent: calling twice from the same thread returns the
 * existing index. */
int epoch_register(void);

/* Mark caller as quiescent at the current global epoch and (lazily) gc
 * its retire list. Call at the top of each reactor select() iteration. */
void epoch_tick(void);

/* Queue `ptr` for deferred free via `freer`. Cheap — no syscalls, no
 * locks. Caller must NOT free `ptr` directly afterwards. */
void epoch_retire(void *ptr, epoch_freer_t freer);

/* Walk caller's retire list and free entries whose retire_epoch is
 * strictly less than min(quiescent[*]). Called automatically by
 * epoch_tick when the retire list grows past EPOCH_GC_THRESHOLD. */
void epoch_gc(void);

/* For teardown: drain all retired entries unconditionally. Safe only
 * when the caller can prove no other thread holds any pointer it ever
 * retired (e.g., all peers have joined). */
void epoch_drain_all(void);

#endif
