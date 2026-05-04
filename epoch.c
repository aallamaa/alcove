/* epoch.c — see epoch.h for design. */

#include "epoch.h"

#include <stdint.h>
#include <stdlib.h>

epoch_thread_t epoch_threads[EPOCH_MAX_THREADS];
_Atomic uint64_t epoch_global = 1; /* 0 reserved for "unregistered" */
_Atomic int epoch_nthreads = 0;
ALCOVE_TLS int epoch_my_idx = -1;

int epoch_register(void) {
  if (epoch_my_idx >= 0) return epoch_my_idx;
  int idx = atomic_fetch_add_explicit(&epoch_nthreads, 1, memory_order_relaxed);
  if (idx >= EPOCH_MAX_THREADS) {
    /* Roll back the bump so other registrants can still try. */
    atomic_fetch_sub_explicit(&epoch_nthreads, 1, memory_order_relaxed);
    return -1;
  }
  epoch_my_idx = idx;
  /* Seed quiescent to current global so gc doesn't immediately consider
     this thread's "view" to be from epoch 0. */
  uint64_t g = atomic_load_explicit(&epoch_global, memory_order_acquire);
  atomic_store_explicit(&epoch_threads[idx].quiescent, g,
                        memory_order_release);
  epoch_threads[idx].retire_head = NULL;
  epoch_threads[idx].retire_count = 0;
  return idx;
}

static uint64_t epoch_min_quiescent(void) {
  int n = atomic_load_explicit(&epoch_nthreads, memory_order_acquire);
  if (n <= 0) return UINT64_MAX;
  uint64_t mn = UINT64_MAX;
  for (int i = 0; i < n; i++) {
    uint64_t q = atomic_load_explicit(&epoch_threads[i].quiescent,
                                      memory_order_acquire);
    if (q == 0) continue; /* slot reserved but thread quiescent-unset */
    if (q < mn) mn = q;
  }
  return mn;
}

void epoch_tick(void) {
  if (epoch_my_idx < 0) return;
  uint64_t g = atomic_load_explicit(&epoch_global, memory_order_acquire);
  atomic_store_explicit(&epoch_threads[epoch_my_idx].quiescent, g,
                        memory_order_release);
  if (epoch_threads[epoch_my_idx].retire_count >= EPOCH_GC_THRESHOLD)
    epoch_gc();
}

void epoch_retire(void *ptr, epoch_freer_t freer) {
  if (!ptr) return;
  if (epoch_my_idx < 0) {
    /* Caller forgot to register. Best we can do is free immediately —
       which is unsafe under concurrency, but at least we don't leak. */
    freer(ptr);
    return;
  }
  /* Bump global to mark this retirement. Future ticks of any reactor
     that hasn't yet observed >= retire_epoch + 1 still hold a "view"
     from before the retire, so the strict-less-than gc condition keeps
     them safe. */
  uint64_t r = atomic_fetch_add_explicit(&epoch_global, 1,
                                         memory_order_acq_rel);
  epoch_retire_t *node = malloc(sizeof *node);
  if (!node) {
    /* OOM: best-effort immediate free. Risk: peer reader may still hold
       the pointer. In practice the retire-list malloc is the smallest
       allocation we make, and OOM here is fatal anyway. */
    freer(ptr);
    return;
  }
  node->ptr = ptr;
  node->freer = freer;
  node->retire_epoch = r;
  node->next = epoch_threads[epoch_my_idx].retire_head;
  epoch_threads[epoch_my_idx].retire_head = node;
  epoch_threads[epoch_my_idx].retire_count++;
}

void epoch_gc(void) {
  if (epoch_my_idx < 0) return;
  uint64_t safe = epoch_min_quiescent();
  if (safe == UINT64_MAX) return;
  epoch_retire_t **pp = &epoch_threads[epoch_my_idx].retire_head;
  while (*pp) {
    epoch_retire_t *cur = *pp;
    if (cur->retire_epoch < safe) {
      *pp = cur->next;
      cur->freer(cur->ptr);
      free(cur);
      epoch_threads[epoch_my_idx].retire_count--;
    } else {
      pp = &cur->next;
    }
  }
}

void epoch_drain_all(void) {
  if (epoch_my_idx < 0) return;
  epoch_retire_t *cur = epoch_threads[epoch_my_idx].retire_head;
  while (cur) {
    epoch_retire_t *next = cur->next;
    cur->freer(cur->ptr);
    free(cur);
    cur = next;
  }
  epoch_threads[epoch_my_idx].retire_head = NULL;
  epoch_threads[epoch_my_idx].retire_count = 0;
}
