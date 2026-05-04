/* lfkv — implementation. See lfkv.h for design notes. */

#include "lfkv.h"
#include "epoch.h"

#include <stdlib.h>
#include <string.h>

/* Adapter: epoch_retire takes a generic freer. unrefexp returns int and
 * takes exp_t *; wrap to (void)(void *). */
static void unrefexp_void(void *p) { unrefexp((exp_t *)p); }

lfkv_t *lfkv_new(size_t nslots) {
  if (nslots == 0 || (nslots & (nslots - 1)) != 0) return NULL;
  lfkv_t *kv = calloc(1, sizeof *kv);
  if (!kv) return NULL;
  kv->slots = calloc(nslots, sizeof *kv->slots);
  if (!kv->slots) { free(kv); return NULL; }
  kv->nslots = nslots;
  kv->mask = nslots - 1;
  return kv;
}

void lfkv_destroy(lfkv_t *kv) {
  if (!kv) return;
  for (size_t i = 0; i < kv->nslots; i++) {
    lfslot_t *s = atomic_load_explicit(&kv->slots[i], memory_order_relaxed);
    if (!s) continue;
    exp_t *v = atomic_load_explicit(&s->val, memory_order_relaxed);
    if (v) unrefexp(v);
    free(s);
  }
  free(kv->slots);
  free(kv);
}

static inline int slot_key_eq(lfslot_t *s, uint32_t h,
                              const char *k, size_t klen) {
  if (s->khash != h || s->klen != klen) return 0;
  return memcmp(s->key, k, klen) == 0;
}

static lfslot_t *slot_alloc(uint32_t h, const char *k, size_t klen,
                            exp_t *initial_val, int64_t expiry) {
  lfslot_t *s = malloc(sizeof *s + klen);
  if (!s) return NULL;
  s->khash = h;
  s->klen = (uint32_t)klen;
  atomic_store_explicit(&s->val, initial_val, memory_order_relaxed);
  atomic_store_explicit(&s->expiry_us, expiry, memory_order_relaxed);
  memcpy(s->key, k, klen);
  return s;
}

/* Returns: pointer to slot if found (live or tombstoned), NULL if the
 * key is not in the table (probe hit a NULL slot). On return, *idx is
 * the slot index where the key lives (or where it would be inserted).
 * `tombstone_idx` is the index of the first tombstone seen during the
 * probe (or SIZE_MAX), useful for SET to reuse a tombstoned slot. */
static lfslot_t *probe(lfkv_t *kv, uint32_t h, const char *k, size_t klen,
                       size_t *idx, size_t *tombstone_idx) {
  size_t i = h & kv->mask;
  *tombstone_idx = SIZE_MAX;
  for (size_t p = 0; p < kv->nslots; p++, i = (i + 1) & kv->mask) {
    lfslot_t *s = atomic_load_explicit(&kv->slots[i], memory_order_acquire);
    if (s == NULL) { *idx = i; return NULL; }
    if (slot_key_eq(s, h, k, klen)) { *idx = i; return s; }
  }
  *idx = (h & kv->mask);
  return NULL; /* table full of mismatches — pathological */
}

static int slot_is_expired(lfslot_t *s) {
  int64_t exp = atomic_load_explicit(&s->expiry_us, memory_order_acquire);
  if (exp == 0) return 0;
  return gettimeusec() >= exp;
}

int lfkv_set(lfkv_t *kv, const char *k, size_t klen, exp_t *val) {
  uint32_t h = bernstein_hash((unsigned char *)k, (int)klen);
  size_t i, tomb;
  lfslot_t *s = probe(kv, h, k, klen, &i, &tomb);

  if (s) {
    /* Found existing slot for this key. CAS-swap the value. */
    for (;;) {
      exp_t *old = atomic_load_explicit(&s->val, memory_order_acquire);
      if (atomic_compare_exchange_strong_explicit(
              &s->val, &old, val,
              memory_order_release, memory_order_acquire)) {
        /* Clear any old expiry — SET overwrites without preserving TTL,
           matching Redis SET semantics for the basic form. */
        atomic_store_explicit(&s->expiry_us, (int64_t)0,
                              memory_order_release);
        if (old == NULL)
          atomic_fetch_add_explicit(&kv->count, 1, memory_order_relaxed);
        else
          epoch_retire(old, unrefexp_void);
        return 0;
      }
      /* Lost CAS — retry. */
    }
  }

  /* Key not present. Allocate a new slot record. */
  lfslot_t *fresh = slot_alloc(h, k, klen, val, 0);
  if (!fresh) return -1;

  /* Try to claim slots[i]. */
  for (;;) {
    lfslot_t *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(
            &kv->slots[i], &expected, fresh,
            memory_order_release, memory_order_acquire)) {
      atomic_fetch_add_explicit(&kv->count, 1, memory_order_relaxed);
      return 0;
    }
    /* Lost CAS — someone wrote slots[i]. If it's our key, fall back to
       the existing-slot path on the loaded `expected`. Else continue
       linear probe from i+1. */
    if (expected && slot_key_eq(expected, h, k, klen)) {
      free(fresh);
      /* Swap into the now-existing slot. */
      for (;;) {
        exp_t *old = atomic_load_explicit(&expected->val, memory_order_acquire);
        if (atomic_compare_exchange_strong_explicit(
                &expected->val, &old, val,
                memory_order_release, memory_order_acquire)) {
          atomic_store_explicit(&expected->expiry_us, (int64_t)0,
                                memory_order_release);
          if (old == NULL)
            atomic_fetch_add_explicit(&kv->count, 1, memory_order_relaxed);
          else
            epoch_retire(old, unrefexp_void);
          return 0;
        }
      }
    }
    /* Different occupant — continue probe. */
    i = (i + 1) & kv->mask;
    /* Re-probe rest of table. If we wrap fully, table is full. */
    for (size_t p = 0; p < kv->nslots; p++, i = (i + 1) & kv->mask) {
      lfslot_t *s2 = atomic_load_explicit(&kv->slots[i], memory_order_acquire);
      if (s2 == NULL) goto try_claim;
      if (slot_key_eq(s2, h, k, klen)) {
        free(fresh);
        for (;;) {
          exp_t *old = atomic_load_explicit(&s2->val, memory_order_acquire);
          if (atomic_compare_exchange_strong_explicit(
                  &s2->val, &old, val,
                  memory_order_release, memory_order_acquire)) {
            atomic_store_explicit(&s2->expiry_us, (int64_t)0,
                                  memory_order_release);
            if (old == NULL)
              atomic_fetch_add_explicit(&kv->count, 1, memory_order_relaxed);
            else
              epoch_retire(old, unrefexp_void);
            return 0;
          }
        }
      }
    }
    free(fresh);
    return -1; /* full */
  try_claim:
    continue;
  }
}

exp_t *lfkv_get(lfkv_t *kv, const char *k, size_t klen) {
  uint32_t h = bernstein_hash((unsigned char *)k, (int)klen);
  size_t i, tomb;
  lfslot_t *s = probe(kv, h, k, klen, &i, &tomb);
  if (!s) return NULL;
  exp_t *v = atomic_load_explicit(&s->val, memory_order_acquire);
  if (!v) return NULL;
  if (slot_is_expired(s)) {
    /* Lazy eviction: CAS-swap to NULL. Multiple readers may race here;
       only one wins and retires `v`. */
    if (atomic_compare_exchange_strong_explicit(
            &s->val, &v, (exp_t *)NULL,
            memory_order_release, memory_order_acquire)) {
      atomic_store_explicit(&s->expiry_us, (int64_t)0, memory_order_release);
      atomic_fetch_sub_explicit(&kv->count, 1, memory_order_relaxed);
      epoch_retire(v, unrefexp_void);
    }
    return NULL;
  }
  /* Bump refcount before returning so caller can outlive the next
     epoch boundary. The value is guaranteed not yet freed because we're
     still in our quiescent window — even if a concurrent SET retired
     it after our load, the retire-list ref keeps it alive. refexp's
     atomic inc takes us from 1→2 (or higher), so subsequent retires
     can't drop it to 0 until the caller releases. */
  return refexp(v);
}

int lfkv_del(lfkv_t *kv, const char *k, size_t klen) {
  uint32_t h = bernstein_hash((unsigned char *)k, (int)klen);
  size_t i, tomb;
  lfslot_t *s = probe(kv, h, k, klen, &i, &tomb);
  if (!s) return 0;
  for (;;) {
    exp_t *old = atomic_load_explicit(&s->val, memory_order_acquire);
    if (old == NULL) return 0;
    if (atomic_compare_exchange_strong_explicit(
            &s->val, &old, (exp_t *)NULL,
            memory_order_release, memory_order_acquire)) {
      atomic_store_explicit(&s->expiry_us, (int64_t)0, memory_order_release);
      atomic_fetch_sub_explicit(&kv->count, 1, memory_order_relaxed);
      epoch_retire(old, unrefexp_void);
      return 1;
    }
  }
}

size_t lfkv_count(lfkv_t *kv) {
  return (size_t)atomic_load_explicit(&kv->count, memory_order_relaxed);
}

int lfkv_set_expiry(lfkv_t *kv, const char *k, size_t klen, int64_t ts_us) {
  uint32_t h = bernstein_hash((unsigned char *)k, (int)klen);
  size_t i, tomb;
  lfslot_t *s = probe(kv, h, k, klen, &i, &tomb);
  if (!s) return 0;
  exp_t *v = atomic_load_explicit(&s->val, memory_order_acquire);
  if (!v) return 0;
  atomic_store_explicit(&s->expiry_us, ts_us, memory_order_release);
  return 1;
}

int64_t lfkv_get_expiry(lfkv_t *kv, const char *k, size_t klen) {
  uint32_t h = bernstein_hash((unsigned char *)k, (int)klen);
  size_t i, tomb;
  lfslot_t *s = probe(kv, h, k, klen, &i, &tomb);
  if (!s) return -1;
  exp_t *v = atomic_load_explicit(&s->val, memory_order_acquire);
  if (!v) return -1;
  return atomic_load_explicit(&s->expiry_us, memory_order_acquire);
}

/* Try to install `val` into an existing tombstoned slot via CAS. Returns
 * 1 on install, 0 if a concurrent writer beat us with a non-NULL value
 * (caller keeps ref). Helper for set_nx — only stores when current is NULL. */
static int slot_install_if_null(lfkv_t *kv, lfslot_t *s, exp_t *val) {
  exp_t *old = NULL;
  if (atomic_compare_exchange_strong_explicit(
          &s->val, &old, val,
          memory_order_release, memory_order_acquire)) {
    atomic_store_explicit(&s->expiry_us, (int64_t)0, memory_order_release);
    atomic_fetch_add_explicit(&kv->count, 1, memory_order_relaxed);
    return 1;
  }
  /* old is now whatever the slot actually holds. If non-NULL, lost race. */
  return 0;
}

int lfkv_set_nx(lfkv_t *kv, const char *k, size_t klen, exp_t *val) {
  uint32_t h = bernstein_hash((unsigned char *)k, (int)klen);
  size_t i, tomb;
  lfslot_t *s = probe(kv, h, k, klen, &i, &tomb);

  if (s) {
    /* Slot exists. If val == NULL (tombstone), try to install. Else 0. */
    exp_t *cur = atomic_load_explicit(&s->val, memory_order_acquire);
    if (cur != NULL) return 0;
    return slot_install_if_null(kv, s, val);
  }

  /* No slot — allocate and claim. Same probe-and-claim dance as lfkv_set,
     but on a key-match collision we must check the live value. */
  lfslot_t *fresh = slot_alloc(h, k, klen, val, 0);
  if (!fresh) return -1;

  for (;;) {
    lfslot_t *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(
            &kv->slots[i], &expected, fresh,
            memory_order_release, memory_order_acquire)) {
      atomic_fetch_add_explicit(&kv->count, 1, memory_order_relaxed);
      return 1;
    }
    if (expected && slot_key_eq(expected, h, k, klen)) {
      free(fresh);
      exp_t *cur = atomic_load_explicit(&expected->val, memory_order_acquire);
      if (cur != NULL) return 0;
      return slot_install_if_null(kv, expected, val);
    }
    i = (i + 1) & kv->mask;
    for (size_t p = 0; p < kv->nslots; p++, i = (i + 1) & kv->mask) {
      lfslot_t *s2 = atomic_load_explicit(&kv->slots[i], memory_order_acquire);
      if (s2 == NULL) goto try_claim;
      if (slot_key_eq(s2, h, k, klen)) {
        free(fresh);
        exp_t *cur = atomic_load_explicit(&s2->val, memory_order_acquire);
        if (cur != NULL) return 0;
        return slot_install_if_null(kv, s2, val);
      }
    }
    free(fresh);
    return -1; /* full */
  try_claim:
    continue;
  }
}

int lfkv_set_xx(lfkv_t *kv, const char *k, size_t klen, exp_t *val) {
  uint32_t h = bernstein_hash((unsigned char *)k, (int)klen);
  size_t i, tomb;
  lfslot_t *s = probe(kv, h, k, klen, &i, &tomb);
  if (!s) return 0;
  for (;;) {
    exp_t *old = atomic_load_explicit(&s->val, memory_order_acquire);
    if (old == NULL) return 0;
    if (atomic_compare_exchange_strong_explicit(
            &s->val, &old, val,
            memory_order_release, memory_order_acquire)) {
      atomic_store_explicit(&s->expiry_us, (int64_t)0, memory_order_release);
      epoch_retire(old, unrefexp_void);
      return 1;
    }
  }
}

int lfkv_cas(lfkv_t *kv, const char *k, size_t klen,
             exp_t *expected, exp_t *new_val) {
  uint32_t h = bernstein_hash((unsigned char *)k, (int)klen);
  size_t i, tomb;
  lfslot_t *s = probe(kv, h, k, klen, &i, &tomb);
  if (!s) return 0;
  exp_t *cur = expected;
  if (atomic_compare_exchange_strong_explicit(
          &s->val, &cur, new_val,
          memory_order_release, memory_order_acquire)) {
    /* Bookkeeping: count moves only if NULL/non-NULL boundary crossed. */
    if (expected == NULL && new_val != NULL)
      atomic_fetch_add_explicit(&kv->count, 1, memory_order_relaxed);
    else if (expected != NULL && new_val == NULL)
      atomic_fetch_sub_explicit(&kv->count, 1, memory_order_relaxed);
    if (expected != NULL) epoch_retire(expected, unrefexp_void);
    return 1;
  }
  return 0;
}

void lfkv_foreach(lfkv_t *kv, lfkv_iter_fn cb, void *ctx) {
  int64_t now = gettimeusec();
  for (size_t i = 0; i < kv->nslots; i++) {
    lfslot_t *s = atomic_load_explicit(&kv->slots[i], memory_order_acquire);
    if (!s) continue;
    exp_t *v = atomic_load_explicit(&s->val, memory_order_acquire);
    if (!v) continue;
    int64_t exp = atomic_load_explicit(&s->expiry_us, memory_order_acquire);
    if (exp != 0 && now >= exp) continue;
    if (cb(s->key, s->klen, v, exp, ctx) != 0) return;
  }
}

size_t lfkv_evict_expired(lfkv_t *kv) {
  int64_t now = gettimeusec();
  size_t evicted = 0;
  for (size_t i = 0; i < kv->nslots; i++) {
    lfslot_t *s = atomic_load_explicit(&kv->slots[i], memory_order_acquire);
    if (!s) continue;
    int64_t exp = atomic_load_explicit(&s->expiry_us, memory_order_acquire);
    if (exp == 0 || now < exp) continue;
    exp_t *v = atomic_load_explicit(&s->val, memory_order_acquire);
    if (!v) continue;
    if (atomic_compare_exchange_strong_explicit(
            &s->val, &v, (exp_t *)NULL,
            memory_order_release, memory_order_acquire)) {
      atomic_store_explicit(&s->expiry_us, (int64_t)0, memory_order_release);
      atomic_fetch_sub_explicit(&kv->count, 1, memory_order_relaxed);
      epoch_retire(v, unrefexp_void);
      evicted++;
    }
  }
  return evicted;
}

void lfkv_clear(lfkv_t *kv) {
  for (size_t i = 0; i < kv->nslots; i++) {
    lfslot_t *s = atomic_load_explicit(&kv->slots[i], memory_order_acquire);
    if (!s) continue;
    for (;;) {
      exp_t *v = atomic_load_explicit(&s->val, memory_order_acquire);
      if (!v) break;
      if (atomic_compare_exchange_strong_explicit(
              &s->val, &v, (exp_t *)NULL,
              memory_order_release, memory_order_acquire)) {
        atomic_store_explicit(&s->expiry_us, (int64_t)0, memory_order_release);
        atomic_fetch_sub_explicit(&kv->count, 1, memory_order_relaxed);
        epoch_retire(v, unrefexp_void);
        break;
      }
    }
  }
}
