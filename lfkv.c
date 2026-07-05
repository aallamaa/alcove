/* lfkv — implementation. See lfkv.h for design notes. */

#include "lfkv.h"
#include "epoch.h"

#include <stdlib.h>
#include <string.h>

/* Layer-2 keyspace watch hooks live in resp.c, which the web build
   excludes (no pthread/epoll/socket under emscripten) — no-op there. */
#ifndef ALCOVE_WEB
extern _Atomic int g_lfkv_watch_enabled;
extern void resp_watch_emit(int op, const char *k, size_t klen, struct exp_t *val);

#define LFKV_EMIT(op, k, klen, val) do { \
  if (atomic_load_explicit(&g_lfkv_watch_enabled, memory_order_relaxed)) { \
    resp_watch_emit((op), (k), (klen), (val)); \
  } \
} while (0)
#else
#define LFKV_EMIT(op, k, klen, val) ((void)0)
#endif

static lfentry_t *entry_alloc(exp_t *val, int64_t expiry_us) {
  lfentry_t *e = malloc(sizeof *e);
  if (!e)
    return NULL;
  e->val = val;
  e->expiry_us = expiry_us;
  return e;
}

/* Frees an entry that was published in a slot: it owns entry->val. */
static void entry_free_owned(void *p) {
  lfentry_t *e = (lfentry_t *)p;
  if (!e)
    return;
  if (e->val)
    unrefexp(e->val);
  free(e);
}

/* Frees an unpublished wrapper. The caller still owns the value ref. */
static void entry_free_wrapper(lfentry_t *e) { free(e); }

#define LFKV_PROBE(kv, k, klen, h, i, tomb, s)                                 \
  uint32_t h = bernstein_hash((unsigned char *)(k), (int)(klen));              \
  size_t i, tomb;                                                              \
  lfslot_t *s = probe((kv), h, (k), (klen), &i, &tomb)

static inline int entry_expired(lfentry_t *e, int64_t now) {
  return e && e->expiry_us != 0 && now >= e->expiry_us;
}

static inline int slot_retire_entry(lfkv_t *kv, lfslot_t *s,
                                    lfentry_t **expected_e) {
  if (!expected_e || !*expected_e)
    return 0;
  if (atomic_compare_exchange_strong_explicit(
          &s->entry, expected_e, (lfentry_t *)NULL, memory_order_release,
          memory_order_acquire)) {
    atomic_fetch_sub_explicit(&kv->count, 1, memory_order_relaxed);
    epoch_retire(*expected_e, entry_free_owned);
    LFKV_EMIT(2, s->key, s->klen, NULL);
    return 1;
  }
  return 0;
}

static inline void slot_replace_entry(lfkv_t *kv, lfslot_t *s,
                                      lfentry_t *new_entry) {
  for (;;) {
    lfentry_t *old = atomic_load_explicit(&s->entry, memory_order_acquire);
    if (atomic_compare_exchange_strong_explicit(&s->entry, &old, new_entry,
                                                memory_order_release,
                                                memory_order_acquire)) {
      if (old == NULL)
        atomic_fetch_add_explicit(&kv->count, 1, memory_order_relaxed);
      else
        epoch_retire(old, entry_free_owned);
      LFKV_EMIT(1, s->key, s->klen, new_entry->val);
      return;
    }
  }
}

lfkv_t *lfkv_new(size_t nslots) {
  if (nslots == 0 || (nslots & (nslots - 1)) != 0)
    return NULL;
  lfkv_t *kv = calloc(1, sizeof *kv);
  if (!kv)
    return NULL;
  kv->slots = calloc(nslots, sizeof *kv->slots);
  if (!kv->slots) {
    free(kv);
    return NULL;
  }
  kv->nslots = nslots;
  kv->mask = nslots - 1;
  return kv;
}

void lfkv_destroy(lfkv_t *kv) {
  if (!kv)
    return;
  for (size_t i = 0; i < kv->nslots; i++) {
    lfslot_t *s = atomic_load_explicit(&kv->slots[i], memory_order_relaxed);
    if (!s)
      continue;
    lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_relaxed);
    entry_free_owned(ent);
    free(s);
  }
  free(kv->slots);
  free(kv);
}

static inline int slot_key_eq(lfslot_t *s, uint32_t h, const char *k,
                              size_t klen) {
  if (s->khash != h || s->klen != klen)
    return 0;
  return memcmp(s->key, k, klen) == 0;
}

static lfslot_t *slot_alloc(uint32_t h, const char *k, size_t klen,
                            lfentry_t *initial_entry) {
  lfslot_t *s = malloc(sizeof *s + klen);
  if (!s)
    return NULL;
  s->khash = h;
  s->klen = (uint32_t)klen;
  atomic_store_explicit(&s->entry, initial_entry, memory_order_relaxed);
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
    if (s == NULL) {
      *idx = i;
      return NULL;
    }
    if (slot_key_eq(s, h, k, klen)) {
      *idx = i;
      return s;
    }
  }
  *idx = (h & kv->mask);
  return NULL; /* table full of mismatches — pathological */
}

int lfkv_set(lfkv_t *kv, const char *k, size_t klen, exp_t *val) {
  lfentry_t *new_entry = entry_alloc(val, 0);
  if (!new_entry)
    return -1;

  LFKV_PROBE(kv, k, klen, h, i, tomb, s);

  if (s) {
    slot_replace_entry(kv, s, new_entry);
    return 0;
  }

  lfslot_t *fresh = slot_alloc(h, k, klen, new_entry);
  if (!fresh) {
    entry_free_wrapper(new_entry);
    return -1;
  }

  for (;;) {
    lfslot_t *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&kv->slots[i], &expected, fresh,
                                                memory_order_release,
                                                memory_order_acquire)) {
      atomic_fetch_add_explicit(&kv->count, 1, memory_order_relaxed);
      LFKV_EMIT(1, k, klen, val);
      return 0;
    }
    if (expected && slot_key_eq(expected, h, k, klen)) {
      free(fresh);
      slot_replace_entry(kv, expected, new_entry);
      return 0;
    }
    i = (i + 1) & kv->mask;
    for (size_t p = 0; p < kv->nslots; p++, i = (i + 1) & kv->mask) {
      lfslot_t *s2 = atomic_load_explicit(&kv->slots[i], memory_order_acquire);
      if (s2 == NULL)
        goto try_claim;
      if (slot_key_eq(s2, h, k, klen)) {
        free(fresh);
        slot_replace_entry(kv, s2, new_entry);
        return 0;
      }
    }
    free(fresh);
    entry_free_wrapper(new_entry);
    return -1; /* full */
  try_claim:
    continue;
  }
}

exp_t *lfkv_get(lfkv_t *kv, const char *k, size_t klen) {
  int64_t now = gettimeusec();
  for (;;) {
    LFKV_PROBE(kv, k, klen, h, i, tomb, s);
    if (!s)
      return NULL;
    lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
    if (!ent || !ent->val)
      return NULL;
    if (entry_expired(ent, now)) {
      lfentry_t *expected = ent;
      if (slot_retire_entry(kv, s, &expected))
        return NULL;
      continue;
    }
    return refexp(ent->val);
  }
}

/* Borrowed-pointer variant. Skips the refexp/unrefexp pair on the read
   path. See lfkv.h for the safety contract. */
exp_t *lfkv_peek(lfkv_t *kv, const char *k, size_t klen) {
  int64_t now = gettimeusec();
  for (;;) {
    LFKV_PROBE(kv, k, klen, h, i, tomb, s);
    if (!s)
      return NULL;
    lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
    if (!ent || !ent->val)
      return NULL;
    if (entry_expired(ent, now)) {
      lfentry_t *expected = ent;
      if (slot_retire_entry(kv, s, &expected))
        return NULL;
      continue;
    }
    return ent->val;
  }
}

int lfkv_del(lfkv_t *kv, const char *k, size_t klen) {
  int64_t now = gettimeusec();
  for (;;) {
    LFKV_PROBE(kv, k, klen, h, i, tomb, s);
    if (!s)
      return 0;
    lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
    if (!ent)
      return 0;
    lfentry_t *expected = ent;
    if (entry_expired(ent, now)) {
      if (slot_retire_entry(kv, s, &expected))
        return 0;
      continue;
    }
    if (slot_retire_entry(kv, s, &expected))
      return 1;
  }
}

size_t lfkv_count(lfkv_t *kv) {
  return (size_t)atomic_load_explicit(&kv->count, memory_order_relaxed);
}

int lfkv_set_expiry(lfkv_t *kv, const char *k, size_t klen, int64_t ts_us) {
  int64_t now = gettimeusec();
  for (;;) {
    LFKV_PROBE(kv, k, klen, h, i, tomb, s);
    if (!s)
      return 0;
    lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
    if (!ent || !ent->val)
      return 0;
    if (entry_expired(ent, now)) {
      lfentry_t *expected = ent;
      if (slot_retire_entry(kv, s, &expected))
        return 0;
      continue;
    }
    lfentry_t *new_entry = entry_alloc(ent->val, ts_us);
    if (!new_entry)
      return 0;
    refexp(ent->val);
    lfentry_t *expected = ent;
    if (atomic_compare_exchange_strong_explicit(
            &s->entry, &expected, new_entry, memory_order_release,
            memory_order_acquire)) {
      epoch_retire(ent, entry_free_owned);
      return 1;
    }
    entry_free_owned(new_entry);
  }
}

int lfkv_touch_if_value(lfkv_t *kv, const char *k, size_t klen, exp_t *expected,
                        int64_t expiry_us) {
  if (!expected)
    return 0;
  int64_t now = gettimeusec();
  LFKV_PROBE(kv, k, klen, h, i, tomb, s);
  if (!s)
    return 0;
  for (;;) {
    lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
    if (!ent || ent->val != expected)
      return 0;
    if (entry_expired(ent, now)) {
      lfentry_t *old = ent;
      if (slot_retire_entry(kv, s, &old))
        return 0;
      continue;
    }
    if (expiry_us == 0 && ent->expiry_us == 0)
      return 1;
    lfentry_t *new_entry = entry_alloc(expected, expiry_us);
    if (!new_entry)
      return 0;
    refexp(expected);
    lfentry_t *old = ent;
    if (atomic_compare_exchange_strong_explicit(
            &s->entry, &old, new_entry, memory_order_release,
            memory_order_acquire)) {
      epoch_retire(ent, entry_free_owned);
      return 1;
    }
    entry_free_owned(new_entry);
    continue;
  }
}

int64_t lfkv_get_expiry(lfkv_t *kv, const char *k, size_t klen) {
  int64_t now = gettimeusec();
  for (;;) {
    LFKV_PROBE(kv, k, klen, h, i, tomb, s);
    if (!s)
      return -1;
    lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
    if (!ent || !ent->val)
      return -1;
    if (entry_expired(ent, now)) {
      lfentry_t *expected = ent;
      if (slot_retire_entry(kv, s, &expected))
        return -1;
      continue;
    }
    return ent->expiry_us;
  }
}

/* Try to install `val` into an existing tombstoned slot via CAS. Returns
 * 1 on install, 0 if a concurrent writer beat us with a non-NULL value
 * (caller keeps ref). Helper for set_nx — only stores when current is NULL. */
static int slot_install_if_null(lfkv_t *kv, lfslot_t *s, lfentry_t *new_entry) {
  lfentry_t *old = NULL;
  if (atomic_compare_exchange_strong_explicit(
          &s->entry, &old, new_entry, memory_order_release,
          memory_order_acquire)) {
    atomic_fetch_add_explicit(&kv->count, 1, memory_order_relaxed);
    LFKV_EMIT(1, s->key, s->klen, new_entry->val);
    return 1;
  }
  return 0;
}

int lfkv_set_nx(lfkv_t *kv, const char *k, size_t klen, exp_t *val) {
  int64_t now = gettimeusec();
  for (;;) {
    LFKV_PROBE(kv, k, klen, h, i, tomb, s);

    if (s) {
      lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
      if (ent) {
        if (entry_expired(ent, now)) {
          lfentry_t *expected = ent;
          if (slot_retire_entry(kv, s, &expected))
            continue;
          continue;
        }
        return 0;
      }
      lfentry_t *new_entry = entry_alloc(val, 0);
      if (!new_entry)
        return -1;
      if (slot_install_if_null(kv, s, new_entry))
        return 1;
      entry_free_wrapper(new_entry);
      continue;
    }

    lfentry_t *new_entry = entry_alloc(val, 0);
    if (!new_entry)
      return -1;
    lfslot_t *fresh = slot_alloc(h, k, klen, new_entry);
    if (!fresh) {
      entry_free_wrapper(new_entry);
      return -1;
    }

    for (;;) {
      lfslot_t *expected = NULL;
      if (atomic_compare_exchange_strong_explicit(&kv->slots[i], &expected,
                                                  fresh, memory_order_release,
                                                  memory_order_acquire)) {
        atomic_fetch_add_explicit(&kv->count, 1, memory_order_relaxed);
        LFKV_EMIT(1, k, klen, val);
        return 1;
      }
      if (expected && slot_key_eq(expected, h, k, klen)) {
        free(fresh);
        entry_free_wrapper(new_entry);
        break; /* retry through the existing-slot path */
      }
      i = (i + 1) & kv->mask;
      for (size_t p = 0; p < kv->nslots; p++, i = (i + 1) & kv->mask) {
        lfslot_t *s2 = atomic_load_explicit(&kv->slots[i],
                                            memory_order_acquire);
        if (s2 == NULL)
          goto try_claim;
        if (slot_key_eq(s2, h, k, klen)) {
          free(fresh);
          entry_free_wrapper(new_entry);
          goto retry_existing;
        }
      }
      free(fresh);
      entry_free_wrapper(new_entry);
      return -1; /* full */
    try_claim:
      continue;
    }
  retry_existing:
    continue;
  }
}

int lfkv_set_xx(lfkv_t *kv, const char *k, size_t klen, exp_t *val) {
  int64_t now = gettimeusec();
  for (;;) {
    LFKV_PROBE(kv, k, klen, h, i, tomb, s);
    if (!s)
      return 0;
    lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
    if (!ent)
      return 0;
    if (entry_expired(ent, now)) {
      lfentry_t *expected = ent;
      if (slot_retire_entry(kv, s, &expected))
        return 0;
      continue;
    }
    lfentry_t *new_entry = entry_alloc(val, 0);
    if (!new_entry)
      return -1;
    lfentry_t *expected = ent;
    if (atomic_compare_exchange_strong_explicit(
            &s->entry, &expected, new_entry, memory_order_release,
            memory_order_acquire)) {
      epoch_retire(ent, entry_free_owned);
      LFKV_EMIT(1, s->key, s->klen, new_entry->val);
      return 1;
    }
    entry_free_wrapper(new_entry);
  }
}

int lfkv_cas(lfkv_t *kv, const char *k, size_t klen, exp_t *expected,
             exp_t *new_val) {
  int64_t now = gettimeusec();
  LFKV_PROBE(kv, k, klen, h, i, tomb, s);
  if (!s)
    return 0;
  for (;;) {
    lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
    if (!ent || ent->val != expected)
      return 0;
    if (entry_expired(ent, now)) {
      lfentry_t *old = ent;
      if (slot_retire_entry(kv, s, &old))
        return 0;
      continue;
    }
    lfentry_t *new_entry = NULL;
    if (new_val) {
      new_entry = entry_alloc(new_val, ent->expiry_us);
      if (!new_entry)
        return -1;
    }
    lfentry_t *old = ent;
    if (atomic_compare_exchange_strong_explicit(
            &s->entry, &old, new_entry, memory_order_release,
            memory_order_acquire)) {
      if (!new_val)
        atomic_fetch_sub_explicit(&kv->count, 1, memory_order_relaxed);
      epoch_retire(ent, entry_free_owned);
      LFKV_EMIT(new_val ? 1 : 2, s->key, s->klen, new_val);
      return 1;
    }
    if (new_entry)
      entry_free_wrapper(new_entry);
    continue;
  }
}

void lfkv_foreach(lfkv_t *kv, lfkv_iter_fn cb, void *ctx) {
  int64_t now = gettimeusec();
  for (size_t i = 0; i < kv->nslots; i++) {
    lfslot_t *s = atomic_load_explicit(&kv->slots[i], memory_order_acquire);
    if (!s)
      continue;
    lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
    if (!ent || !ent->val)
      continue;
    if (entry_expired(ent, now))
      continue;
    if (cb(s->key, s->klen, ent->val, ent->expiry_us, ctx) != 0)
      return;
  }
}

size_t lfkv_evict_expired(lfkv_t *kv) {
  int64_t now = gettimeusec();
  size_t evicted = 0;
  for (size_t i = 0; i < kv->nslots; i++) {
    lfslot_t *s = atomic_load_explicit(&kv->slots[i], memory_order_acquire);
    if (!s)
      continue;
    lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
    if (!entry_expired(ent, now))
      continue;
    lfentry_t *expected = ent;
    if (slot_retire_entry(kv, s, &expected))
      evicted++;
  }
  return evicted;
}

void lfkv_clear(lfkv_t *kv) {
  for (size_t i = 0; i < kv->nslots; i++) {
    lfslot_t *s = atomic_load_explicit(&kv->slots[i], memory_order_acquire);
    if (!s)
      continue;
    for (;;) {
      lfentry_t *ent = atomic_load_explicit(&s->entry, memory_order_acquire);
      if (!ent)
        break;
      lfentry_t *expected = ent;
      if (slot_retire_entry(kv, s, &expected))
        break;
    }
  }
}
