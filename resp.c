/* resp.c — RESP2 (Redis serialization protocol v2) server.

   Compiled as part of the alcove single TU (#included from alcove.c
   above main()). Lets us call file-static helpers (gettimeusec,
   bernstein_hash, ...) without exporting them.

   Threading model: single-threaded select() loop. `./alcove -r` does
   NOT start the REPL; the process is the server. The interpreter's
   global env is untouched — RESP keys live in a dedicated table.

   Build requirement: atomic refcounts (the default `make` / `make jit`
   path). Refuses to start if alcove was built with
   -DALCOVE_SINGLE_THREADED=1.

   Storage is a private hash table (NOT alcove's dict_t) so values can
   be binary-safe (arbitrary bytes, including embedded NULs). The
   table maps `char* key` → `resp_val_t*` which is a tagged union:
   string, list (doubly-linked), or hash (small chained table). Each
   top-level entry also carries an absolute microsecond expiry — 0
   means "never". Expiry is checked lazily on every key touch.

   Commands implemented:
     server : PING ECHO QUIT COMMAND SELECT DBSIZE FLUSHDB FLUSHALL
              KEYS (only `KEYS *`)
     generic: DEL EXISTS TYPE EXPIRE PEXPIRE TTL PTTL PERSIST
     string : GET SET (with EX/PX/NX/XX) STRLEN APPEND INCR DECR INCRBY DECRBY
     list   : LPUSH RPUSH LPOP RPOP LLEN LRANGE LINDEX
     hash   : HSET HGET HDEL HEXISTS HKEYS HVALS HLEN HGETALL

   Not yet: SAVE/BGSAVE persistence, pub/sub, transactions,
   SCAN cursors, EVAL.
*/

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>

#define RESP_DEFAULT_PORT 6379
#define RESP_RBUF_INIT 4096
#define RESP_RBUF_MAX (16 * 1024 * 1024)
/* Max accept()s per reactor tick. Bounds the worst-case time spent
   on new-connection work so per-client I/O can't be starved by a
   SYN flood. Excess connections get the next select() iteration. */
#define RESP_ACCEPT_BURST 32
#define RESP_LISTEN_BACKLOG 64
#define RESP_MAX_BULK (512 * 1024 * 1024)
#define RESP_MAX_ARGS 1048576
#define RESP_TABLE_INIT 64
#define RESP_HASH_INIT 8
#define RESP_ARGV_POOL_INIT 8 /* covers SET k v, HSET h f v, etc. */

/* ---------- value model ----------
   The top-level keyspace is alcove's own dict_t (chained, bernstein_hash,
   strdup'd C-string keys, value = exp_t* with refcounting). Values are
   exp_t of three types:
     EXP_BLOB — binary-safe bytes (Redis string)
     EXP_LIST — alc_list_t with O(1) head/tail (Redis list)
     EXP_DICT — dict_t with EXP_BLOB values (Redis hash)
   Per-key TTL piggybacks on keyval_t->timestamp via a sign convention:
     0  — no TTL, not persisted
     >0 — persist mark (set by alcove's (persist sym); only used on the
          env dict, not on resp_db, but the convention keeps the field
          reusable across both)
     <0 — absolute-µs expiry deadline; deadline = -timestamp
   Lazy expiry: every key access goes through resp_db_lookup, which
   evicts on the spot if the deadline has passed. No background scanner. */

/* ---------- per-client + server state ---------- */

typedef struct resp_client {
  int fd;
  char *rbuf;
  /* rbuf is a window [rhead, rlen) — reads append at rlen, parser
     advances rhead. Slides down to rhead=0 lazily (when rcap - rlen
     drops below the slide threshold), so a 16-cmd pipeline costs one
     amortised memmove instead of 16 per-command shifts. Invariant:
     rhead <= rlen <= rcap. */
  size_t rhead, rlen, rcap;
  char *wbuf;
  /* wbuf is a window [whead, wlen) — appends advance wlen, drains
     advance whead. Invariant: whead <= wlen <= wcap. */
  size_t whead, wlen, wcap;
  /* argv/argl pool — reused across every parsed command on this
     connection. Grows monotonically up to the largest argc the client
     has ever sent (cap stored in argv_cap). Pre-pool, every command
     paid 2× malloc + 2× free; under pipelining at P=16 that was 64
     heap ops per syscall. */
  char **argv_pool;
  long *argl_pool;
  int argv_cap;
  struct resp_client *next;
} resp_client_t;

/* Per-reactor client list. Each reactor owns its own clients (acquired
   via accept on its own SO_REUSEPORT socket); never touched by peers. */
static ALCOVE_TLS resp_client_t *resp_clients = NULL;
/* Global lock-free keyspace shared by every reactor. Lazy-allocated on
   first SET via resp_kv_ensure(). Each shard_t.kv points at this same
   `g_resp_kv` so the existing `current_shard->kv` access pattern stays
   uniform; the indirection lets us swap per-shard partitioning in later
   without touching call sites. */
static lfkv_t *g_resp_kv = NULL;
/* Default slot count — power of 2. 2^20 = 1M slots ≈ 8MB pointer table.
   Sized to comfortably hold redis-benchmark's `-r 1000000` random-key
   load at <60% fill factor. Tune via RESP_KV_SLOTS env var if needed. */
#define RESP_KV_DEFAULT_SLOTS (1u << 20)
#define resp_kv (current_shard->kv)
#define resp_last_sweep_us (current_shard->db_last_sweep_us)
static volatile sig_atomic_t resp_stop = 0;
/* Set by resp_serve / resp_repl_serve once bind succeeds; read by the
   (redis-port) builtin so REPL code can discover the listening port
   without out-of-band coordination. 0 means "no server running". */
static int resp_active_port = 0;

static void resp_sigint(int sig) {
  (void)sig;
  resp_stop = 1;
}

/* ---------- storage helpers ----------
   Thin wrappers over the lock-free keyspace (lfkv.h). The lfkv table
   stores `exp_t *` slots directly with per-slot atomic expiry; no
   sign-encoding, no per-keyval allocation on overwrite. */

static int64_t resp_now_us(void) { return gettimeusec(); }

/* Inner-hash counter — Redis hashes still use dict_t for field storage
   (the outer key→hash slot is the lock-free part). */
static unsigned long dict_count(dict_t *d) {
  return d ? d->ht[0].used + d->ht[1].used : 0;
}

/* Lazy-create the global lfkv on first write. Concurrent first-callers
   race via CAS on `g_resp_kv`; losers free their stillborn table. */
static void resp_kv_ensure(void) {
  if (atomic_load_explicit((_Atomic(lfkv_t *) *)&g_resp_kv,
                           memory_order_acquire)) {
    /* Mirror the global into the TLS shard pointer once per shard. */
    if (!current_shard->kv) current_shard->kv = g_resp_kv;
    return;
  }
  size_t slots = RESP_KV_DEFAULT_SLOTS;
  const char *env = getenv("RESP_KV_SLOTS");
  if (env) {
    char *e;
    unsigned long v = strtoul(env, &e, 10);
    if (*e == '\0' && v && (v & (v - 1)) == 0) slots = v;
  }
  lfkv_t *fresh = lfkv_new(slots);
  if (!fresh) return;
  lfkv_t *expected = NULL;
  if (!atomic_compare_exchange_strong_explicit(
          (_Atomic(lfkv_t *) *)&g_resp_kv, &expected, fresh,
          memory_order_release, memory_order_acquire)) {
    lfkv_destroy(fresh); /* lost race */
  }
  current_shard->kv = g_resp_kv;
}

/* Lookup: returns refexp-bumped value or NULL. Caller MUST unrefexp.
   Lazy expiry handled inside lfkv_get. */
static inline exp_t *resp_kv_lookup(const char *key, size_t klen) {
  return resp_kv ? lfkv_get(resp_kv, key, klen) : NULL;
}

/* Borrowed-pointer lookup — no refcount bump. Pointer valid only
   for the rest of this reactor turn (until the next epoch_tick).
   Use only on synchronous read paths that copy the value to wbuf
   and return; never on paths that escape to the Lisp evaluator or
   schedule async work. */
static inline exp_t *resp_kv_peek(const char *key, size_t klen) {
  return resp_kv ? lfkv_peek(resp_kv, key, klen) : NULL;
}

/* SET-style write: consumes the caller's `val` ref, clears prior TTL. */
static inline void resp_kv_set(const char *key, size_t klen, exp_t *val) {
  resp_kv_ensure();
  if (lfkv_set(resp_kv, key, klen, val) < 0) {
    /* Table full — best-effort: drop the value. */
    unrefexp(val);
  }
}

static inline int resp_kv_del(const char *key, size_t klen) {
  return resp_kv ? lfkv_del(resp_kv, key, klen) : 0;
}

/* FLUSHDB: tombstone every live slot, retire all values. Slot records
   stay allocated until process exit (keys recycle on re-insert). */
static inline void resp_kv_clear(void) {
  if (resp_kv) lfkv_clear(resp_kv);
}

/* TTL helpers — preserve API names of the old keyval_t-based wrappers
   so handlers stay close to their original shape. */
static inline int64_t resp_kv_get_expiry(const char *key, size_t klen) {
  if (!resp_kv) return -1;
  return lfkv_get_expiry(resp_kv, key, klen);
}
static inline int resp_kv_set_expiry(const char *key, size_t klen,
                                     int64_t expire_at_us) {
  if (!resp_kv) return 0;
  return lfkv_set_expiry(resp_kv, key, klen, expire_at_us);
}

static inline size_t resp_kv_count(void) {
  return resp_kv ? lfkv_count(resp_kv) : 0;
}

/* Evict every expired entry in one pass. KEYS * + count-exact paths
   call this so the array header matches the emitted count. */
static inline void resp_kv_evict_expired(void) {
  if (resp_kv) (void)lfkv_evict_expired(resp_kv);
}

/* Throttled background sweep — called from the reactor's top-of-loop
   so write-then-never-read TTL'd keys can't pile up unboundedly. The
   1s interval matches the reactor's select() timeout, so an idle
   server still ticks the sweep at the same cadence. */
#define RESP_SWEEP_INTERVAL_US 1000000
static void resp_kv_maybe_sweep(void) {
  if (!resp_kv) return;
  int64_t now = resp_now_us();
  if (now - resp_last_sweep_us < RESP_SWEEP_INTERVAL_US) return;
  resp_last_sweep_us = now;
  resp_kv_evict_expired();
}

/* ---------- per-key dict helpers (Redis hash internals) ----------
   A Redis hash is an EXP_DICT whose dict_t holds field→EXP_BLOB. */

static exp_t *resp_dict_field_get(dict_t *d, const char *fk) {
  keyval_t *k = d ? set_get_keyval_dict(d, (char *)fk, NULL) : NULL;
  return k ? k->val : NULL;
}

/* Returns 1 if a new field was created, 0 if existing was overwritten.
   Takes ownership of `val`'s ref (consumes one reference). */
static int resp_dict_field_set(dict_t *d, const char *fk, exp_t *val) {
  unsigned long before = d->ht[0].used;
  set_get_keyval_dict(d, (char *)fk, val);
  unrefexp(val);
  return d->ht[0].used > before;
}

static int resp_dict_field_del(dict_t *d, const char *fk) {
  return d ? del_keyval_dict(d, (char *)fk) : 0;
}

/* ---------- per-client buffer helpers ---------- */

static void resp_client_free(resp_client_t *c) {
  if (!c) return;
  if (c->fd >= 0) close(c->fd);
  free(c->rbuf);
  free(c->wbuf);
  free(c->argv_pool);
  free(c->argl_pool);
  free(c);
}

static void resp_client_unlink(resp_client_t *c) {
  resp_client_t **pp = &resp_clients;
  while (*pp && *pp != c) pp = &(*pp)->next;
  if (*pp) *pp = c->next;
  resp_client_free(c);
}

static void resp_write(resp_client_t *c, const char *p, size_t n) {
  if (c->wlen + n > c->wcap) {
    /* Slide the live window down before paying for a realloc. Only
       firing when steady-state drains have fallen behind. */
    if (c->whead > 0) {
      size_t live = c->wlen - c->whead;
      memmove(c->wbuf, c->wbuf + c->whead, live);
      c->wlen = live;
      c->whead = 0;
    }
    if (c->wlen + n > c->wcap) {
      size_t cap = c->wcap ? c->wcap : 256;
      while (cap < c->wlen + n) cap *= 2;
      c->wbuf = realloc(c->wbuf, cap);
      c->wcap = cap;
    }
  }
  memcpy(c->wbuf + c->wlen, p, n);
  c->wlen += n;
}

/* Drain a window's worth of pending output. Returns 1 to signal the
   caller to drop the client (write returned a non-EAGAIN error), 0
   otherwise. */
static inline int resp_client_drain_write(resp_client_t *c) {
  size_t live = c->wlen - c->whead;
  ssize_t n = write(c->fd, c->wbuf + c->whead, live);
  if (n < 0) return (errno != EAGAIN && errno != EWOULDBLOCK);
  if ((size_t)n == live) {
    c->whead = 0;
    c->wlen = 0;
  } else {
    c->whead += (size_t)n;
  }
  return 0;
}

static void resp_write_str(resp_client_t *c, const char *s) {
  resp_write(c, s, strlen(s));
}

static void resp_write_simple(resp_client_t *c, const char *s) {
  resp_write(c, "+", 1);
  resp_write_str(c, s);
  resp_write(c, "\r\n", 2);
}

static void resp_write_err(resp_client_t *c, const char *s) {
  resp_write(c, "-", 1);
  resp_write_str(c, s);
  resp_write(c, "\r\n", 2);
}

/* Fast unsigned-to-decimal — snprintf was the #1 hotspot under load
   (~30% of CPU samples vs printf-family). 20 digits covers UINT64_MAX. */
static int resp_u64_to_ascii(char *out, uint64_t v) {
  char tmp[20];
  int i = 0;
  do { tmp[i++] = '0' + (int)(v % 10); v /= 10; } while (v);
  for (int j = 0; j < i; j++) out[j] = tmp[i - 1 - j];
  return i;
}

static int resp_i64_to_ascii(char *out, int64_t v) {
  if (v < 0) {
    out[0] = '-';
    return 1 + resp_u64_to_ascii(out + 1, (uint64_t)(-(v + 1)) + 1);
  }
  return resp_u64_to_ascii(out, (uint64_t)v);
}

static void resp_write_int(resp_client_t *c, long long v) {
  char buf[32];
  buf[0] = ':';
  int n = 1 + resp_i64_to_ascii(buf + 1, (int64_t)v);
  buf[n++] = '\r';
  buf[n++] = '\n';
  resp_write(c, buf, (size_t)n);
}

static void resp_write_bulk(resp_client_t *c, const char *p, size_t n) {
  char hdr[32];
  hdr[0] = '$';
  int hn = 1 + resp_u64_to_ascii(hdr + 1, (uint64_t)n);
  hdr[hn++] = '\r';
  hdr[hn++] = '\n';
  resp_write(c, hdr, (size_t)hn);
  resp_write(c, p, n);
  resp_write(c, "\r\n", 2);
}

static void resp_write_nil(resp_client_t *c) { resp_write(c, "$-1\r\n", 5); }

static void resp_write_array_hdr(resp_client_t *c, long long n) {
  char hdr[32];
  hdr[0] = '*';
  int hn = 1 + resp_i64_to_ascii(hdr + 1, (int64_t)n);
  hdr[hn++] = '\r';
  hdr[hn++] = '\n';
  resp_write(c, hdr, (size_t)hn);
}

static void resp_write_wrongtype(resp_client_t *c) {
  resp_write_err(c,
      "WRONGTYPE Operation against a key holding the wrong kind of value");
}

/* Grow the per-client argv/argl pool to fit `n` slots. Pool is sticky
   across commands — only realloc when a command exceeds the previous
   high-water mark. Returns 0 on success, -1 on alloc failure. */
static int resp_argv_pool_reserve(resp_client_t *c, int n) {
  if (n <= c->argv_cap) return 0;
  int cap = c->argv_cap ? c->argv_cap : RESP_ARGV_POOL_INIT;
  while (cap < n) cap *= 2;
  char **nv = realloc(c->argv_pool, sizeof(char *) * (size_t)cap);
  if (!nv) return -1;
  c->argv_pool = nv;
  long *nl = realloc(c->argl_pool, sizeof(long) * (size_t)cap);
  if (!nl) return -1;
  c->argl_pool = nl;
  c->argv_cap = cap;
  return 0;
}

/* ---------- RESP frame parser ----------
   Returns:
     >0 = bytes consumed, argv/argl/argc filled (pointers alias rbuf,
          valid only until next read; dispatch must finish first.
          argv/argl are owned by the client pool — caller must not free)
      0 = need more data
     <0 = protocol error (caller drops the client) */
static int resp_parse_one(resp_client_t *c, char *buf, size_t len,
                          char ***argv_out, long **argl_out,
                          int *argc_out) {
  if (len == 0) return 0;
  /* Inline command form: any non-`*` first byte means a bare-text line
     like `PING\r\n` or `SET foo bar\n`. redis-benchmark's PING_INLINE
     test relies on this. We tokenize on space/tab; argv slots alias
     the input buffer (lengths carried in argl), no copy needed. */
  if (buf[0] != '*') {
    size_t end = 0;
    while (end < len && buf[end] != '\n') end++;
    if (end >= len) return 0;
    size_t line_end = end;
    if (line_end > 0 && buf[line_end - 1] == '\r') line_end--;

    long n = 0;
    size_t p = 0;
    while (p < line_end) {
      while (p < line_end && (buf[p] == ' ' || buf[p] == '\t')) p++;
      if (p >= line_end) break;
      n++;
      if (n > RESP_MAX_ARGS) return -1;
      while (p < line_end && buf[p] != ' ' && buf[p] != '\t') p++;
    }
    if (n == 0) return (int)(end + 1);

    if (resp_argv_pool_reserve(c, (int)n) < 0) return -1;
    char **argv = c->argv_pool;
    long *argl = c->argl_pool;
    long ai = 0;
    p = 0;
    while (p < line_end && ai < n) {
      while (p < line_end && (buf[p] == ' ' || buf[p] == '\t')) p++;
      if (p >= line_end) break;
      size_t s = p;
      while (p < line_end && buf[p] != ' ' && buf[p] != '\t') p++;
      argv[ai] = buf + s;
      argl[ai] = (long)(p - s);
      ai++;
    }
    *argv_out = argv;
    *argl_out = argl;
    *argc_out = (int)n;
    return (int)(end + 1);
  }

  size_t i = 1;
  long n = 0;
  while (i < len && buf[i] != '\r') {
    if (buf[i] < '0' || buf[i] > '9') return -1;
    n = n * 10 + (buf[i] - '0');
    if (n > RESP_MAX_ARGS) return -1;
    i++;
  }
  if (i + 1 >= len) return 0;
  if (buf[i] != '\r' || buf[i + 1] != '\n') return -1;
  i += 2;
  if (n <= 0) return -1;

  if (resp_argv_pool_reserve(c, (int)n) < 0) return -1;
  char **argv = c->argv_pool;
  long *argl = c->argl_pool;

  for (long a = 0; a < n; a++) {
    if (i >= len) return 0;
    if (buf[i] != '$') return -1;
    i++;
    long blen = 0;
    while (i < len && buf[i] != '\r') {
      if (buf[i] < '0' || buf[i] > '9') return -1;
      blen = blen * 10 + (buf[i] - '0');
      if (blen > RESP_MAX_BULK) return -1;
      i++;
    }
    if (i + 1 >= len) return 0;
    if (buf[i] != '\r' || buf[i + 1] != '\n') return -1;
    i += 2;
    if (i + (size_t)blen + 2 > len) return 0;
    if (buf[i + blen] != '\r' || buf[i + blen + 1] != '\n') return -1;
    argv[a] = buf + i;
    argl[a] = blen;
    i += blen + 2;
  }

  *argv_out = argv;
  *argl_out = argl;
  *argc_out = (int)n;
  return (int)i;
}

/* ---------- arg helpers ---------- */

static int resp_cmd_eq(const char *p, long len, const char *want) {
  size_t wlen = strlen(want);
  if ((size_t)len != wlen) return 0;
  for (size_t i = 0; i < wlen; i++) {
    char a = p[i];
    if (a >= 'a' && a <= 'z') a -= 32;
    if (a != want[i]) return 0;
  }
  return 1;
}

/* Copy a bulk arg to NUL-terminated heap. Reject embedded NUL (we
   key our hash via strcmp). Returns NULL on NUL or alloc failure. */
static char *resp_dup_key(const char *p, long len) {
  for (long i = 0; i < len; i++)
    if (p[i] == '\0') return NULL;
  char *k = malloc(len + 1);
  if (!k) return NULL;
  memcpy(k, p, len);
  k[len] = '\0';
  return k;
}

/* Parse a signed integer arg into *out. Returns 1 on success, 0 on
   garbage (caller emits protocol error). */
static int resp_arg_to_ll(const char *p, long len, long long *out) {
  if (len <= 0 || len > 30) return 0;
  char buf[32];
  memcpy(buf, p, len);
  buf[len] = '\0';
  char *end;
  errno = 0;
  long long v = strtoll(buf, &end, 10);
  if (*end != '\0' || errno) return 0;
  *out = v;
  return 1;
}

/* ---------- dispatch helpers ---------- */

#define ARGN(needed)                                                          \
  do {                                                                        \
    if (argc != (needed)) {                                                   \
      resp_write_err(c, "ERR wrong number of arguments");                     \
      return;                                                                 \
    }                                                                         \
  } while (0)
#define ARG_AT_LEAST(min)                                                     \
  do {                                                                        \
    if (argc < (min)) {                                                       \
      resp_write_err(c, "ERR wrong number of arguments");                     \
      return;                                                                 \
    }                                                                         \
  } while (0)

/* Translate a Redis-style negative index (from-end). Returns the
   normalised non-negative index or -1 if out of range. */
static long resp_norm_index(long idx, long len) {
  if (idx < 0) idx += len;
  if (idx < 0 || idx >= len) return -1;
  return idx;
}

/* ---------- command implementations ---------- */

static void cmd_ping(resp_client_t *c, char **argv, long *argl, int argc) {
  if (argc == 1) resp_write_simple(c, "PONG");
  else if (argc == 2) resp_write_bulk(c, argv[1], argl[1]);
  else resp_write_err(c, "ERR wrong number of arguments for 'ping'");
}

static void cmd_echo(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  resp_write_bulk(c, argv[1], argl[1]);
}

static void cmd_quit(resp_client_t *c) {
  resp_write_simple(c, "OK");
  shutdown(c->fd, SHUT_RD);
}

static void cmd_select(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  if (argl[1] == 1 && argv[1][0] == '0') resp_write_simple(c, "OK");
  else resp_write_err(c, "ERR DB index out of range (alcove RESP has 1 db)");
}

static void cmd_dbsize(resp_client_t *c) {
  resp_write_int(c, (long long)resp_kv_count());
}

static void cmd_flushdb(resp_client_t *c) {
  resp_kv_clear();
  resp_write_simple(c, "OK");
}

/* Snapshot one (key,klen) per live entry into a heap-allocated array.
   Used by KEYS * and the redis-keys/redis-foreach REPL builtins so the
   emit phase doesn't see partial mutations from concurrent reactors.
   Returns -1 on alloc failure. */
typedef struct {
  char *bytes; /* one alloc; entries point into here */
  size_t blen, bcap;
  size_t *off;  /* per-entry start offset into `bytes` */
  size_t *len;  /* per-entry key length */
  size_t n, ncap;
} resp_keys_snap_t;

static int resp_keys_snap_cb(const char *k, size_t klen, exp_t *v,
                             int64_t exp_us, void *ctx) {
  (void)v; (void)exp_us;
  resp_keys_snap_t *s = ctx;
  if (s->n == s->ncap) {
    size_t nc = s->ncap ? s->ncap * 2 : 64;
    size_t *no = realloc(s->off, nc * sizeof *no);
    size_t *nl = realloc(s->len, nc * sizeof *nl);
    if (!no || !nl) { free(no); free(nl); return -1; }
    s->off = no; s->len = nl; s->ncap = nc;
  }
  if (s->blen + klen > s->bcap) {
    size_t nc = s->bcap ? s->bcap * 2 : 4096;
    while (nc < s->blen + klen) nc *= 2;
    char *nb = realloc(s->bytes, nc);
    if (!nb) return -1;
    s->bytes = nb; s->bcap = nc;
  }
  s->off[s->n] = s->blen;
  s->len[s->n] = klen;
  memcpy(s->bytes + s->blen, k, klen);
  s->blen += klen;
  s->n++;
  return 0;
}

static void resp_keys_snap_free(resp_keys_snap_t *s) {
  free(s->bytes); free(s->off); free(s->len);
}

static void cmd_keys_star(resp_client_t *c, char **argv, long *argl,
                          int argc) {
  ARGN(2);
  if (argl[1] != 1 || argv[1][0] != '*') {
    resp_write_err(c, "ERR alcove RESP server only supports KEYS *");
    return;
  }
  if (!resp_kv) { resp_write_array_hdr(c, 0); return; }
  resp_kv_evict_expired();
  resp_keys_snap_t s = {0};
  lfkv_foreach(resp_kv, resp_keys_snap_cb, &s);
  resp_write_array_hdr(c, (long long)s.n);
  for (size_t i = 0; i < s.n; i++)
    resp_write_bulk(c, s.bytes + s.off[i], s.len[i]);
  resp_keys_snap_free(&s);
}

static const char *resp_type_name(exp_t *v) {
  if (!v) return "none";
  if (isblob(v)) return "string";
  if (islist(v)) return "list";
  if (isdict(v)) return "hash";
  return "none";
}

static void cmd_type(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  exp_t *v = resp_kv_lookup(argv[1], (size_t)argl[1]);
  if (!v) { resp_write_simple(c, "none"); return; }
  resp_write_simple(c, resp_type_name(v));
  unrefexp(v);
}

static void cmd_del(resp_client_t *c, char **argv, long *argl, int argc) {
  ARG_AT_LEAST(2);
  long long deleted = 0;
  for (int a = 1; a < argc; a++) {
    /* lfkv_del returns 1 if a live value was tombstoned. Lazy-expiry
       still kicks in via lfkv_get if needed; for DEL we just attempt. */
    if (resp_kv_del(argv[a], (size_t)argl[a])) deleted++;
  }
  resp_write_int(c, deleted);
}

static void cmd_exists(resp_client_t *c, char **argv, long *argl, int argc) {
  ARG_AT_LEAST(2);
  long long present = 0;
  for (int a = 1; a < argc; a++) {
    exp_t *v = resp_kv_lookup(argv[a], (size_t)argl[a]);
    if (v) { present++; unrefexp(v); }
  }
  resp_write_int(c, present);
}

/* EXPIRE key seconds | PEXPIRE key millis. Returns 1 if applied,
   0 if key missing. */
static void cmd_expire(resp_client_t *c, char **argv, long *argl, int argc,
                       int millis) {
  ARGN(3);
  long long delta;
  if (!resp_arg_to_ll(argv[2], argl[2], &delta)) {
    resp_write_err(c, "ERR value is not an integer or out of range");
    return;
  }
  const char *k = argv[1];
  size_t klen = (size_t)argl[1];
  /* Negative/zero TTL == immediate delete (Redis semantics). */
  if (delta <= 0) {
    int hit = resp_kv_del(k, klen);
    resp_write_int(c, hit);
    return;
  }
  int64_t deadline = resp_now_us() + delta * (millis ? 1000LL : 1000000LL);
  resp_write_int(c, resp_kv_set_expiry(k, klen, deadline));
}

/* TTL key | PTTL key. Returns:
     -2 if key missing
     -1 if key exists but no expire
     remaining time otherwise */
static void cmd_ttl(resp_client_t *c, char **argv, long *argl, int argc,
                    int millis) {
  ARGN(2);
  int64_t exp = resp_kv_get_expiry(argv[1], (size_t)argl[1]);
  if (exp < 0) { resp_write_int(c, -2); return; }
  if (exp == 0) { resp_write_int(c, -1); return; }
  int64_t left = exp - resp_now_us();
  if (left < 0) left = 0;
  resp_write_int(c, millis ? (long long)(left / 1000)
                           : (long long)(left / 1000000));
}

static void cmd_persist(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  const char *k = argv[1];
  size_t klen = (size_t)argl[1];
  int64_t exp = resp_kv_get_expiry(k, klen);
  if (exp <= 0) { resp_write_int(c, 0); return; } /* absent or no TTL */
  resp_kv_set_expiry(k, klen, 0);
  resp_write_int(c, 1);
}

/* SET key value [EX seconds | PX millis] [NX | XX] */
static void cmd_set(resp_client_t *c, char **argv, long *argl, int argc) {
  ARG_AT_LEAST(3);
  long long expire_us = 0; /* delta from now, in us */
  int nx = 0, xx = 0;
  for (int i = 3; i < argc; i++) {
    if (resp_cmd_eq(argv[i], argl[i], "EX") && i + 1 < argc) {
      long long s;
      if (!resp_arg_to_ll(argv[i + 1], argl[i + 1], &s) || s <= 0) {
        resp_write_err(c, "ERR invalid expire time in 'set'");
        return;
      }
      expire_us = s * 1000000LL;
      i++;
    } else if (resp_cmd_eq(argv[i], argl[i], "PX") && i + 1 < argc) {
      long long ms;
      if (!resp_arg_to_ll(argv[i + 1], argl[i + 1], &ms) || ms <= 0) {
        resp_write_err(c, "ERR invalid expire time in 'set'");
        return;
      }
      expire_us = ms * 1000LL;
      i++;
    } else if (resp_cmd_eq(argv[i], argl[i], "NX")) {
      nx = 1;
    } else if (resp_cmd_eq(argv[i], argl[i], "XX")) {
      xx = 1;
    } else {
      resp_write_err(c, "ERR syntax error");
      return;
    }
  }
  if (nx && xx) {
    resp_write_err(c, "ERR syntax error");
    return;
  }
  const char *k = argv[1];
  size_t klen = (size_t)argl[1];
  exp_t *fresh = make_blob(argv[2], (size_t)argl[2]);
  resp_kv_ensure();
  if (nx || xx) {
    int ok = nx ? lfkv_set_nx(resp_kv, k, klen, fresh)
                : lfkv_set_xx(resp_kv, k, klen, fresh);
    if (!ok) { unrefexp(fresh); resp_write_nil(c); return; }
  } else {
    resp_kv_set(k, klen, fresh); /* always consumes the ref */
  }
  if (expire_us)
    resp_kv_set_expiry(k, klen, resp_now_us() + expire_us);
  resp_write_simple(c, "OK");
}

static void cmd_get(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  exp_t *v = resp_kv_peek(argv[1], (size_t)argl[1]);
  if (!v) { resp_write_nil(c); return; }
  if (!isblob(v)) { resp_write_wrongtype(c); return; }
  alc_blob_t *b = (alc_blob_t *)v->ptr;
  resp_write_bulk(c, b->bytes, b->len);
}

static void cmd_strlen(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  exp_t *v = resp_kv_peek(argv[1], (size_t)argl[1]);
  if (!v) { resp_write_int(c, 0); return; }
  if (!isblob(v)) { resp_write_wrongtype(c); return; }
  resp_write_int(c, (long long)blob_len(v));
}

/* Shared core for INCR/DECR/INCRBY/DECRBY. CAS-loop on the value slot
   so concurrent reactors can't lose updates; lfkv_cas preserves TTL
   (does not touch the slot's expiry_us). */
static void resp_apply_incr(resp_client_t *c, const char *key, size_t klen,
                            long long delta) {
  resp_kv_ensure();
  for (;;) {
    exp_t *cur_v = resp_kv_lookup(key, klen);
    long long cur = 0;
    if (cur_v) {
      if (!isblob(cur_v)) {
        unrefexp(cur_v);
        resp_write_wrongtype(c);
        return;
      }
      alc_blob_t *b = (alc_blob_t *)cur_v->ptr;
      if (b->len == 0 || b->len > 30) {
        unrefexp(cur_v);
        resp_write_err(c, "ERR value is not an integer or out of range");
        return;
      }
      char buf[32];
      memcpy(buf, b->bytes, b->len);
      buf[b->len] = '\0';
      char *end;
      errno = 0;
      cur = strtoll(buf, &end, 10);
      if (*end != '\0' || errno) {
        unrefexp(cur_v);
        resp_write_err(c, "ERR value is not an integer or out of range");
        return;
      }
    }
    if ((delta > 0 && cur > LLONG_MAX - delta) ||
        (delta < 0 && cur < LLONG_MIN - delta)) {
      if (cur_v) unrefexp(cur_v);
      resp_write_err(c, "ERR increment or decrement would overflow");
      return;
    }
    long long next = cur + delta;
    char buf[32];
    int n = resp_i64_to_ascii(buf, (int64_t)next);
    exp_t *new_blob = make_blob(buf, (size_t)n);
    int ok;
    if (cur_v) {
      /* CAS preserves TTL. cur_v has 1 caller-bumped ref; on success
         lfkv_cas retires the slot's old (== cur_v) ref. We still need
         to drop our bumped ref afterwards. */
      ok = lfkv_cas(resp_kv, key, klen, cur_v, new_blob);
      unrefexp(cur_v);
      if (!ok) { unrefexp(new_blob); continue; }
    } else {
      ok = lfkv_set_nx(resp_kv, key, klen, new_blob);
      if (!ok) { unrefexp(new_blob); continue; }
    }
    resp_write_int(c, next);
    return;
  }
}

/* INCR / DECR — one-step ±1. */
static void cmd_incr_decr(resp_client_t *c, char **argv, long *argl, int argc,
                          int sign) {
  ARGN(2);
  resp_apply_incr(c, argv[1], (size_t)argl[1], sign);
}

/* INCRBY / DECRBY — caller-supplied integer delta in argv[2]. */
static void cmd_incrby_decrby(resp_client_t *c, char **argv, long *argl,
                              int argc, int sign) {
  ARGN(3);
  if (argl[2] == 0 || argl[2] > 20) {
    resp_write_err(c, "ERR value is not an integer or out of range");
    return;
  }
  char dbuf[32];
  memcpy(dbuf, argv[2], argl[2]);
  dbuf[argl[2]] = '\0';
  char *end;
  errno = 0;
  long long delta = strtoll(dbuf, &end, 10);
  if (*end != '\0' || errno) {
    resp_write_err(c, "ERR value is not an integer or out of range");
    return;
  }
  if (sign < 0) {
    if (delta == LLONG_MIN) {
      resp_write_err(c, "ERR value is not an integer or out of range");
      return;
    }
    delta = -delta;
  }
  resp_apply_incr(c, argv[1], (size_t)argl[1], delta);
}

/* APPEND key value — append to an existing string, or create the key
   holding `value` if missing. Returns the new length. CAS-loops to
   handle concurrent writers; lfkv_cas preserves TTL. */
static void cmd_append(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(3);
  const char *k = argv[1];
  size_t klen = (size_t)argl[1];
  size_t add = (size_t)argl[2];
  resp_kv_ensure();
  for (;;) {
    exp_t *cur_v = resp_kv_lookup(k, klen);
    if (!cur_v) {
      exp_t *fresh = make_blob(argv[2], add);
      if (lfkv_set_nx(resp_kv, k, klen, fresh)) {
        resp_write_int(c, (long long)add);
        return;
      }
      unrefexp(fresh);
      continue; /* someone else inserted — retry as append-existing */
    }
    if (!isblob(cur_v)) {
      unrefexp(cur_v);
      resp_write_wrongtype(c);
      return;
    }
    alc_blob_t *old = (alc_blob_t *)cur_v->ptr;
    size_t old_n = old->len;
    size_t new_n = old_n + add;
    exp_t *fresh = make_blob(NULL, new_n);
    alc_blob_t *nb = (alc_blob_t *)fresh->ptr;
    if (old_n) memcpy(nb->bytes, old->bytes, old_n);
    if (add) memcpy(nb->bytes + old_n, argv[2], add);
    int ok = lfkv_cas(resp_kv, k, klen, cur_v, fresh);
    unrefexp(cur_v);
    if (!ok) { unrefexp(fresh); continue; }
    resp_write_int(c, (long long)new_n);
    return;
  }
}

/* ---------- list commands ----------
   Mutations are copy-on-write: clone the list, modify the copy, then
   lfkv_cas-swap. O(N) per op but simple and lock-free. Reads grab a
   refexp-bumped exp_t and read directly. */

/* Deep-copy a list into a brand-new EXP_LIST. Bumps each value's ref. */
static exp_t *resp_list_clone(alc_list_t *src) {
  exp_t *dst_exp = make_list_exp();
  alc_list_t *dst = (alc_list_t *)dst_exp->ptr;
  for (alc_listnode_t *nd = src->head; nd; nd = nd->next)
    alc_list_push_right(dst, refexp(nd->val));
  return dst_exp;
}

static void cmd_lpush_rpush(resp_client_t *c, char **argv, long *argl,
                            int argc, int left) {
  ARG_AT_LEAST(3);
  const char *k = argv[1];
  size_t klen = (size_t)argl[1];
  resp_kv_ensure();
  for (;;) {
    exp_t *cur = resp_kv_lookup(k, klen);
    if (cur && !islist(cur)) {
      unrefexp(cur);
      resp_write_wrongtype(c);
      return;
    }
    exp_t *fresh = cur ? resp_list_clone((alc_list_t *)cur->ptr)
                       : make_list_exp();
    alc_list_t *l = (alc_list_t *)fresh->ptr;
    for (int i = 2; i < argc; i++) {
      exp_t *nv = make_blob(argv[i], (size_t)argl[i]);
      if (left) alc_list_push_left(l, nv);
      else      alc_list_push_right(l, nv);
    }
    int ok = cur ? lfkv_cas(resp_kv, k, klen, cur, fresh)
                 : lfkv_set_nx(resp_kv, k, klen, fresh);
    if (cur) unrefexp(cur);
    if (!ok) { unrefexp(fresh); continue; }
    resp_write_int(c, (long long)l->len);
    return;
  }
}

static void cmd_lpop_rpop(resp_client_t *c, char **argv, long *argl, int argc,
                          int left) {
  ARGN(2);
  const char *k = argv[1];
  size_t klen = (size_t)argl[1];
  for (;;) {
    exp_t *cur = resp_kv_lookup(k, klen);
    if (!cur) { resp_write_nil(c); return; }
    if (!islist(cur)) { unrefexp(cur); resp_write_wrongtype(c); return; }
    alc_list_t *src = (alc_list_t *)cur->ptr;
    if (src->len == 0) { unrefexp(cur); resp_write_nil(c); return; }
    /* Capture the popped value's bytes BEFORE the swap so an evicted
       blob can't get freed under us. */
    alc_listnode_t *target = left ? src->head : src->tail;
    alc_blob_t *tb = (alc_blob_t *)target->val->ptr;
    size_t blen = tb->len;
    char *bcopy = malloc(blen);
    if (blen) memcpy(bcopy, tb->bytes, blen);
    int ok;
    if (src->len == 1) {
      /* Last element — just delete the key (Redis container rule). */
      ok = lfkv_cas(resp_kv, k, klen, cur, NULL);
    } else {
      exp_t *fresh = make_list_exp();
      alc_list_t *dst = (alc_list_t *)fresh->ptr;
      alc_listnode_t *start = left ? src->head->next : src->head;
      alc_listnode_t *stop  = left ? NULL : src->tail; /* exclusive of tail when right */
      for (alc_listnode_t *nd = start; nd; nd = nd->next) {
        if (!left && nd == src->tail) break;
        alc_list_push_right(dst, refexp(nd->val));
      }
      (void)stop;
      ok = lfkv_cas(resp_kv, k, klen, cur, fresh);
      if (!ok) { unrefexp(fresh); }
    }
    unrefexp(cur);
    if (!ok) { free(bcopy); continue; }
    resp_write_bulk(c, bcopy, blen);
    free(bcopy);
    return;
  }
}

static void cmd_llen(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  exp_t *v = resp_kv_lookup(argv[1], (size_t)argl[1]);
  if (!v) { resp_write_int(c, 0); return; }
  if (!islist(v)) { unrefexp(v); resp_write_wrongtype(c); return; }
  resp_write_int(c, (long long)((alc_list_t *)v->ptr)->len);
  unrefexp(v);
}

static void cmd_lindex(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(3);
  long long idx;
  if (!resp_arg_to_ll(argv[2], argl[2], &idx)) {
    resp_write_err(c, "ERR value is not an integer or out of range");
    return;
  }
  exp_t *v = resp_kv_lookup(argv[1], (size_t)argl[1]);
  if (!v) { resp_write_nil(c); return; }
  if (!islist(v)) { unrefexp(v); resp_write_wrongtype(c); return; }
  alc_list_t *l = (alc_list_t *)v->ptr;
  long ni = resp_norm_index((long)idx, l->len);
  if (ni < 0) { unrefexp(v); resp_write_nil(c); return; }
  alc_listnode_t *nd;
  if (ni < l->len / 2) {
    nd = l->head;
    for (long i = 0; i < ni; i++) nd = nd->next;
  } else {
    nd = l->tail;
    for (long i = l->len - 1; i > ni; i--) nd = nd->prev;
  }
  alc_blob_t *b = (alc_blob_t *)nd->val->ptr;
  resp_write_bulk(c, b->bytes, b->len);
  unrefexp(v);
}

static void cmd_lrange(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(4);
  long long start, stop;
  if (!resp_arg_to_ll(argv[2], argl[2], &start) ||
      !resp_arg_to_ll(argv[3], argl[3], &stop)) {
    resp_write_err(c, "ERR value is not an integer or out of range");
    return;
  }
  exp_t *v = resp_kv_lookup(argv[1], (size_t)argl[1]);
  if (!v) { resp_write_array_hdr(c, 0); return; }
  if (!islist(v)) { unrefexp(v); resp_write_wrongtype(c); return; }
  alc_list_t *l = (alc_list_t *)v->ptr;
  long len = l->len;
  if (start < 0) start += len;
  if (stop < 0) stop += len;
  if (start < 0) start = 0;
  if (stop >= len) stop = len - 1;
  if (start > stop || start >= len) {
    unrefexp(v);
    resp_write_array_hdr(c, 0);
    return;
  }
  resp_write_array_hdr(c, stop - start + 1);
  alc_listnode_t *nd = l->head;
  for (long i = 0; i < start; i++) nd = nd->next;
  for (long i = start; i <= stop; i++, nd = nd->next) {
    alc_blob_t *b = (alc_blob_t *)nd->val->ptr;
    resp_write_bulk(c, b->bytes, b->len);
  }
  unrefexp(v);
}

/* ---------- hash commands ----------
   A Redis hash is an EXP_DICT whose dict_t holds field→EXP_BLOB.
   Mutations (HSET/HDEL) clone the dict_t, modify the clone, then
   lfkv_cas-swap the EXP_DICT slot. Reads grab a refexp-bumped exp_t
   and walk it directly. */

/* Deep-copy a dict_t into a fresh EXP_DICT. Each value's ref bumps
   (set_get_keyval_dict refexps internally), so src and dst both own
   independent refs. */
static exp_t *resp_dict_clone(dict_t *src) {
  exp_t *dst_exp = make_dict_exp();
  dict_t *dst = (dict_t *)dst_exp->ptr;
  for (int hi = 0; hi < 2; hi++) {
    kvht_t *h = &src->ht[hi];
    for (unsigned long b = 0; b < h->size; b++)
      for (keyval_t *kv = h->table[b]; kv; kv = kv->next)
        set_get_keyval_dict(dst, (char *)kv->key, kv->val);
  }
  return dst_exp;
}

/* HSET key field value [field value ...] — returns count of NEW
   fields created (not updated). COW: clone, write, CAS-swap. */
static void cmd_hset(resp_client_t *c, char **argv, long *argl, int argc) {
  ARG_AT_LEAST(4);
  if ((argc - 2) % 2 != 0) {
    resp_write_err(c, "ERR wrong number of arguments for 'hset'");
    return;
  }
  const char *k = argv[1];
  size_t klen = (size_t)argl[1];
  resp_kv_ensure();
  for (;;) {
    exp_t *cur = resp_kv_lookup(k, klen);
    if (cur && !isdict(cur)) {
      unrefexp(cur);
      resp_write_wrongtype(c);
      return;
    }
    exp_t *fresh = cur ? resp_dict_clone((dict_t *)cur->ptr)
                       : make_dict_exp();
    dict_t *h = (dict_t *)fresh->ptr;
    long long created = 0;
    for (int i = 2; i + 1 < argc; i += 2) {
      char *fk = resp_dup_key(argv[i], argl[i]);
      if (!fk) continue;
      exp_t *fv = make_blob(argv[i + 1], (size_t)argl[i + 1]);
      created += resp_dict_field_set(h, fk, fv);
      free(fk);
    }
    int ok = cur ? lfkv_cas(resp_kv, k, klen, cur, fresh)
                 : lfkv_set_nx(resp_kv, k, klen, fresh);
    if (cur) unrefexp(cur);
    if (!ok) { unrefexp(fresh); continue; }
    resp_write_int(c, created);
    return;
  }
}

static void cmd_hget(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(3);
  exp_t *v = resp_kv_lookup(argv[1], (size_t)argl[1]);
  if (!v) { resp_write_nil(c); return; }
  if (!isdict(v)) { unrefexp(v); resp_write_wrongtype(c); return; }
  char *fk = resp_dup_key(argv[2], argl[2]);
  if (!fk) { unrefexp(v); resp_write_nil(c); return; }
  exp_t *fv = resp_dict_field_get((dict_t *)v->ptr, fk);
  free(fk);
  if (!fv) { unrefexp(v); resp_write_nil(c); return; }
  alc_blob_t *b = (alc_blob_t *)fv->ptr;
  resp_write_bulk(c, b->bytes, b->len);
  unrefexp(v);
}

static void cmd_hdel(resp_client_t *c, char **argv, long *argl, int argc) {
  ARG_AT_LEAST(3);
  const char *k = argv[1];
  size_t klen = (size_t)argl[1];
  for (;;) {
    exp_t *cur = resp_kv_lookup(k, klen);
    if (!cur) { resp_write_int(c, 0); return; }
    if (!isdict(cur)) { unrefexp(cur); resp_write_wrongtype(c); return; }
    exp_t *fresh = resp_dict_clone((dict_t *)cur->ptr);
    dict_t *h = (dict_t *)fresh->ptr;
    long long deleted = 0;
    for (int i = 2; i < argc; i++) {
      char *fk = resp_dup_key(argv[i], argl[i]);
      if (!fk) continue;
      deleted += resp_dict_field_del(h, fk);
      free(fk);
    }
    int became_empty = (dict_count(h) == 0);
    int ok;
    if (became_empty) {
      unrefexp(fresh); /* not needed — drop the empty container */
      ok = lfkv_cas(resp_kv, k, klen, cur, NULL);
    } else {
      ok = lfkv_cas(resp_kv, k, klen, cur, fresh);
      if (!ok) unrefexp(fresh);
    }
    unrefexp(cur);
    if (!ok) continue;
    resp_write_int(c, deleted);
    return;
  }
}

static void cmd_hexists(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(3);
  exp_t *v = resp_kv_lookup(argv[1], (size_t)argl[1]);
  if (!v) { resp_write_int(c, 0); return; }
  if (!isdict(v)) { unrefexp(v); resp_write_wrongtype(c); return; }
  char *fk = resp_dup_key(argv[2], argl[2]);
  if (!fk) { unrefexp(v); resp_write_int(c, 0); return; }
  exp_t *fv = resp_dict_field_get((dict_t *)v->ptr, fk);
  free(fk);
  resp_write_int(c, fv ? 1 : 0);
  unrefexp(v);
}

static void cmd_hlen(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  exp_t *v = resp_kv_lookup(argv[1], (size_t)argl[1]);
  if (!v) { resp_write_int(c, 0); return; }
  if (!isdict(v)) { unrefexp(v); resp_write_wrongtype(c); return; }
  resp_write_int(c, (long long)dict_count((dict_t *)v->ptr));
  unrefexp(v);
}

/* Shared walker for HKEYS / HVALS / HGETALL — emit keys, vals, or
   alternating pairs. */
static void hash_emit(resp_client_t *c, dict_t *h, int keys, int vals) {
  unsigned long count = dict_count(h);
  long long n = (long long)count * ((keys && vals) ? 2 : 1);
  resp_write_array_hdr(c, n);
  for (int hi = 0; hi < 2; hi++) {
    kvht_t *ht = &h->ht[hi];
    for (unsigned long b = 0; b < ht->size; b++) {
      for (keyval_t *kv = ht->table[b]; kv; kv = kv->next) {
        if (keys)
          resp_write_bulk(c, (char *)kv->key, strlen((char *)kv->key));
        if (vals) {
          alc_blob_t *bb = (alc_blob_t *)kv->val->ptr;
          resp_write_bulk(c, bb->bytes, bb->len);
        }
      }
    }
  }
}

static void cmd_hkeys_hvals_hgetall(resp_client_t *c, char **argv, long *argl,
                                    int argc, int keys, int vals) {
  ARGN(2);
  exp_t *v = resp_kv_lookup(argv[1], (size_t)argl[1]);
  if (!v) { resp_write_array_hdr(c, 0); return; }
  if (!isdict(v)) { unrefexp(v); resp_write_wrongtype(c); return; }
  hash_emit(c, (dict_t *)v->ptr, keys, vals);
  unrefexp(v);
}

/* ---------- top-level dispatch ---------- */

/* Forward decl — resp_user_dispatch lives near the bottom of the file
   alongside the (redis-defcmd ...) builtin, but resp_dispatch (above)
   needs to call it on the unknown-command path. */
static int resp_user_dispatch(resp_client_t *c, const char *cmd, long clen,
                              char **argv, long *argl, int argc);

/* FNV-1a + linear probe over a 64-bucket table. Commands that share a
   handler (e.g. FLUSHDB/FLUSHALL) differ only by `kind`, decoded by a
   switch in resp_dispatch. */
typedef enum {
  HK_PING = 1, HK_ECHO, HK_QUIT, HK_COMMAND, HK_SELECT, HK_DBSIZE,
  HK_FLUSHDB, HK_KEYS, HK_TYPE, HK_DEL, HK_EXISTS,
  HK_EXPIRE_S, HK_EXPIRE_MS, HK_TTL_S, HK_TTL_MS, HK_PERSIST,
  HK_GET, HK_SET, HK_STRLEN,
  HK_INCR, HK_DECR, HK_INCRBY, HK_DECRBY, HK_APPEND,
  HK_LPUSH, HK_RPUSH, HK_LPOP, HK_RPOP, HK_LLEN, HK_LINDEX, HK_LRANGE,
  HK_HSET, HK_HGET, HK_HDEL, HK_HEXISTS, HK_HLEN,
  HK_HKEYS, HK_HVALS, HK_HGETALL,
} resp_kind_t;

#define RESP_CMD_TABLE_BITS 6           /* 64 buckets, load = 41/64 = 0.64 */
#define RESP_CMD_TABLE_SIZE (1u << RESP_CMD_TABLE_BITS)
#define RESP_CMD_TABLE_MASK (RESP_CMD_TABLE_SIZE - 1)
#define RESP_CMD_NAMEMAX 12

typedef struct {
  unsigned char namelen;
  unsigned char kind;
  char name[RESP_CMD_NAMEMAX];        /* uppercase, NOT NUL-terminated */
} resp_cmd_t;

static resp_cmd_t resp_cmd_table[RESP_CMD_TABLE_SIZE];

static const struct { const char *name; resp_kind_t kind; } resp_cmd_seed[] = {
  {"PING",     HK_PING},      {"ECHO",     HK_ECHO},
  {"QUIT",     HK_QUIT},      {"COMMAND",  HK_COMMAND},
  {"SELECT",   HK_SELECT},    {"DBSIZE",   HK_DBSIZE},
  {"FLUSHDB",  HK_FLUSHDB},   {"FLUSHALL", HK_FLUSHDB},
  {"KEYS",     HK_KEYS},      {"TYPE",     HK_TYPE},
  {"DEL",      HK_DEL},       {"UNLINK",   HK_DEL},
  {"EXISTS",   HK_EXISTS},    {"EXPIRE",   HK_EXPIRE_S},
  {"PEXPIRE",  HK_EXPIRE_MS}, {"TTL",      HK_TTL_S},
  {"PTTL",     HK_TTL_MS},    {"PERSIST",  HK_PERSIST},
  {"GET",      HK_GET},       {"SET",      HK_SET},
  {"STRLEN",   HK_STRLEN},    {"INCR",     HK_INCR},
  {"DECR",     HK_DECR},      {"INCRBY",   HK_INCRBY},
  {"DECRBY",   HK_DECRBY},    {"APPEND",   HK_APPEND},
  {"LPUSH",    HK_LPUSH},     {"RPUSH",    HK_RPUSH},
  {"LPOP",     HK_LPOP},      {"RPOP",     HK_RPOP},
  {"LLEN",     HK_LLEN},      {"LINDEX",   HK_LINDEX},
  {"LRANGE",   HK_LRANGE},    {"HSET",     HK_HSET},
  {"HGET",     HK_HGET},      {"HDEL",     HK_HDEL},
  {"HEXISTS",  HK_HEXISTS},   {"HLEN",     HK_HLEN},
  {"HKEYS",    HK_HKEYS},     {"HVALS",    HK_HVALS},
  {"HGETALL",  HK_HGETALL},
};

/* The seed→table build is one-shot for the whole process; calling it
   per-reactor would re-probe an already-populated bucket array and
   overflow on the second pass. Gate with pthread_once so multi-reactor
   startup is safe. */
static pthread_once_t resp_cmd_table_once = PTHREAD_ONCE_INIT;
static void resp_cmd_table_init_inner(void) {
  for (size_t s = 0; s < sizeof resp_cmd_seed / sizeof *resp_cmd_seed; s++) {
    const char *name = resp_cmd_seed[s].name;
    long len = (long)strlen(name);
    uint32_t h = 2166136261u;
    for (long j = 0; j < len; j++) {
      h ^= (unsigned char)name[j];
      h *= 16777619u;
    }
    uint32_t i = h & RESP_CMD_TABLE_MASK;
    for (uint32_t probes = 0; probes < RESP_CMD_TABLE_SIZE; probes++) {
      if (resp_cmd_table[i].kind == 0) {
        resp_cmd_table[i].namelen = (unsigned char)len;
        resp_cmd_table[i].kind = (unsigned char)resp_cmd_seed[s].kind;
        memcpy(resp_cmd_table[i].name, name, len);
        break;
      }
      i = (i + 1) & RESP_CMD_TABLE_MASK;
      if (probes + 1 == RESP_CMD_TABLE_SIZE) {
        fprintf(stderr, "resp_cmd_table: overflow inserting %s\n", name);
        abort();
      }
    }
  }
}

static void resp_cmd_table_init(void) {
  pthread_once(&resp_cmd_table_once, resp_cmd_table_init_inner);
}

/* Returns the kind for a built-in command, or 0 if not found. */
static int resp_cmd_lookup(const char *cmd, long clen) {
  if (clen <= 0 || clen > RESP_CMD_NAMEMAX) return 0;
  /* Single pass: case-fold into up[] and hash simultaneously. */
  char up[RESP_CMD_NAMEMAX];
  uint32_t h = 2166136261u;
  for (long i = 0; i < clen; i++) {
    char a = cmd[i];
    if (a >= 'a' && a <= 'z') a -= 32;
    up[i] = a;
    h ^= (unsigned char)a;
    h *= 16777619u;
  }
  uint32_t i = h & RESP_CMD_TABLE_MASK;
  for (uint32_t probes = 0; probes < RESP_CMD_TABLE_SIZE; probes++) {
    const resp_cmd_t *e = &resp_cmd_table[i];
    if (e->kind == 0) return 0;
    if (e->namelen == clen && memcmp(e->name, up, clen) == 0)
      return e->kind;
    i = (i + 1) & RESP_CMD_TABLE_MASK;
  }
  return 0;
}

static void resp_dispatch(resp_client_t *c, char **argv, long *argl, int argc) {
  if (argc < 1) return;
  const char *cmd = argv[0];
  long clen = argl[0];
  int kind = resp_cmd_lookup(cmd, clen);

  switch (kind) {
  case HK_PING:    return cmd_ping(c, argv, argl, argc);
  case HK_ECHO:    return cmd_echo(c, argv, argl, argc);
  case HK_QUIT:    return cmd_quit(c);
  case HK_COMMAND:
    /* redis-cli sends `COMMAND DOCS` on connect to build help. An
       empty array satisfies it. */
    resp_write_array_hdr(c, 0);
    return;
  case HK_SELECT:    return cmd_select(c, argv, argl, argc);
  case HK_DBSIZE:    return cmd_dbsize(c);
  case HK_FLUSHDB:   return cmd_flushdb(c);
  case HK_KEYS:      return cmd_keys_star(c, argv, argl, argc);
  case HK_TYPE:      return cmd_type(c, argv, argl, argc);
  case HK_DEL:       return cmd_del(c, argv, argl, argc);
  case HK_EXISTS:    return cmd_exists(c, argv, argl, argc);
  case HK_EXPIRE_S:  return cmd_expire(c, argv, argl, argc, 0);
  case HK_EXPIRE_MS: return cmd_expire(c, argv, argl, argc, 1);
  case HK_TTL_S:     return cmd_ttl(c, argv, argl, argc, 0);
  case HK_TTL_MS:    return cmd_ttl(c, argv, argl, argc, 1);
  case HK_PERSIST:   return cmd_persist(c, argv, argl, argc);
  case HK_GET:       return cmd_get(c, argv, argl, argc);
  case HK_SET:       return cmd_set(c, argv, argl, argc);
  case HK_STRLEN:    return cmd_strlen(c, argv, argl, argc);
  case HK_INCR:      return cmd_incr_decr(c, argv, argl, argc, +1);
  case HK_DECR:      return cmd_incr_decr(c, argv, argl, argc, -1);
  case HK_INCRBY:    return cmd_incrby_decrby(c, argv, argl, argc, +1);
  case HK_DECRBY:    return cmd_incrby_decrby(c, argv, argl, argc, -1);
  case HK_APPEND:    return cmd_append(c, argv, argl, argc);
  case HK_LPUSH:     return cmd_lpush_rpush(c, argv, argl, argc, 1);
  case HK_RPUSH:     return cmd_lpush_rpush(c, argv, argl, argc, 0);
  case HK_LPOP:      return cmd_lpop_rpop(c, argv, argl, argc, 1);
  case HK_RPOP:      return cmd_lpop_rpop(c, argv, argl, argc, 0);
  case HK_LLEN:      return cmd_llen(c, argv, argl, argc);
  case HK_LINDEX:    return cmd_lindex(c, argv, argl, argc);
  case HK_LRANGE:    return cmd_lrange(c, argv, argl, argc);
  case HK_HSET:      return cmd_hset(c, argv, argl, argc);
  case HK_HGET:      return cmd_hget(c, argv, argl, argc);
  case HK_HDEL:      return cmd_hdel(c, argv, argl, argc);
  case HK_HEXISTS:   return cmd_hexists(c, argv, argl, argc);
  case HK_HLEN:      return cmd_hlen(c, argv, argl, argc);
  case HK_HKEYS:     return cmd_hkeys_hvals_hgetall(c, argv, argl, argc, 1, 0);
  case HK_HVALS:     return cmd_hkeys_hvals_hgetall(c, argv, argl, argc, 0, 1);
  case HK_HGETALL:   return cmd_hkeys_hvals_hgetall(c, argv, argl, argc, 1, 1);
  }

  if (resp_user_dispatch(c, cmd, clen, argv, argl, argc)) return;

  char buf[256];
  int n = snprintf(buf, sizeof buf, "ERR unknown command '%.*s'", (int)clen,
                   cmd);
  if (n < 0) n = 0;
  if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
  resp_write(c, "-", 1);
  resp_write(c, buf, n);
  resp_write(c, "\r\n", 2);
}

static void resp_process_input(resp_client_t *c) {
  for (;;) {
    char **argv = NULL;
    long *argl = NULL;
    int argc = 0;
    int consumed = resp_parse_one(c, c->rbuf + c->rhead,
                                  c->rlen - c->rhead,
                                  &argv, &argl, &argc);
    if (consumed == 0) break;
    if (consumed < 0) {
      resp_write_err(c, "ERR Protocol error");
      shutdown(c->fd, SHUT_RD);
      return;
    }
    resp_dispatch(c, argv, argl, argc);
    /* argv/argl belong to c->argv_pool — reused on the next iteration,
       freed only at client teardown. */
    c->rhead += (size_t)consumed;
  }
  /* Cheap reset when the window is fully consumed — the common case
     after a pipelined burst. Skips the next slide-down memmove. */
  if (c->rhead == c->rlen) c->rhead = c->rlen = 0;
}

static int resp_set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -1;
  return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Takes ownership of cfd: on allocation failure it is closed and NULL
   returned. Splices into the reactor-owned resp_clients list, so this
   must only run on the reactor thread that owns that list. */
static resp_client_t *resp_client_new(int cfd) {
  if (resp_set_nonblock(cfd) < 0) { close(cfd); return NULL; }
  resp_client_t *cl = calloc(1, sizeof *cl);
  if (!cl) { close(cfd); return NULL; }
  cl->fd = cfd;
  cl->rcap = RESP_RBUF_INIT;
  cl->rbuf = malloc(cl->rcap);
  /* Pre-size the argv pool so the first command on a fresh connection
     skips the lazy realloc(NULL,…) hop; growth still kicks in for
     wider commands via resp_argv_pool_reserve. */
  cl->argv_cap = RESP_ARGV_POOL_INIT;
  cl->argv_pool = malloc(sizeof(char *) * cl->argv_cap);
  cl->argl_pool = malloc(sizeof(long) * cl->argv_cap);
  if (!cl->rbuf || !cl->argv_pool || !cl->argl_pool) {
    resp_client_free(cl);
    return NULL;
  }
  cl->next = resp_clients;
  resp_clients = cl;
  return cl;
}

/* Drain pending accepts on a non-blocking listen socket, capped at
   RESP_ACCEPT_BURST. EAGAIN/EWOULDBLOCK is the normal exit (queue empty,
   or under SO_REUSEPORT a peer reactor grabbed the connection). EMFILE/
   ENFILE are real fd-exhaustion problems we want surfaced. */
static void resp_accept_drain(int srv) {
  for (int i = 0; i < RESP_ACCEPT_BURST; i++) {
    int cfd = accept(srv, NULL, NULL);
    if (cfd < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept");
      break;
    }
    (void)resp_client_new(cfd);
  }
}

/* Per-client IO step: slide-and-grow rbuf, read, dispatch, opportunistic
   flush, then drain wbuf if select said writeable. Returns 1 if the
   client should be dropped (EOF, hard error, or buffer ceiling hit).
   Both reactor loops (resp_serve and the combined REPL+RESP loop) call
   this — keeps the read/flush/write logic in one place. Inline so the
   per-iteration call cost stays at zero in the hot loop. */
static inline int resp_client_handle_io(resp_client_t *cur,
                                        fd_set *rfds, fd_set *wfds) {
  int drop = 0;
  if (FD_ISSET(cur->fd, rfds)) {
    /* Slide before grow: reclaiming a parsed prefix is far cheaper
       than doubling the buffer. */
    if (cur->rlen + 4096 > cur->rcap && cur->rhead > 0) {
      size_t live = cur->rlen - cur->rhead;
      memmove(cur->rbuf, cur->rbuf + cur->rhead, live);
      cur->rlen = live;
      cur->rhead = 0;
    }
    if (cur->rlen + 4096 > cur->rcap) {
      if (cur->rcap >= RESP_RBUF_MAX) drop = 1;
      else {
        size_t cap = cur->rcap * 2;
        if (cap > RESP_RBUF_MAX) cap = RESP_RBUF_MAX;
        cur->rbuf = realloc(cur->rbuf, cap);
        cur->rcap = cap;
      }
    }
    if (!drop) {
      ssize_t n = read(cur->fd, cur->rbuf + cur->rlen,
                       cur->rcap - cur->rlen);
      if (n == 0) drop = 1;
      else if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) drop = 1;
      } else {
        cur->rlen += (size_t)n;
        resp_process_input(cur);
        /* beforeSleep-style opportunistic flush: dispatched replies
           sit in wbuf, and the socket we read from is almost
           certainly writeable. Draining now skips the next select()
           round-trip — compounds under pipelining (P=16 → 16 replies
           flushed in one syscall instead of two event-loop turns). */
        if (cur->wlen > cur->whead)
          drop = resp_client_drain_write(cur);
      }
    }
  }
  if (!drop && FD_ISSET(cur->fd, wfds) && cur->wlen > cur->whead)
    drop = resp_client_drain_write(cur);
  return drop;
}

/* Bind/listen/nonblock on 127.0.0.1:port, returning the listen fd or -1.
   Always sets SO_REUSEPORT — Linux hashes flows across N reactors that
   bind the same port; macOS picks whichever wakes first (weaker
   fairness). At N=1 it's a no-op, so harmless on the single-binder
   REPL path. */
static int resp_listen(int port) {
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) { perror("socket"); return -1; }
  int yes = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#ifdef SO_REUSEPORT
  (void)setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof yes);
#endif
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
    fprintf(stderr, "alcove: bind 127.0.0.1:%d failed: %s\n", port,
            strerror(errno));
    close(srv); return -1;
  }
  if (listen(srv, RESP_LISTEN_BACKLOG) < 0) {
    perror("listen"); close(srv); return -1;
  }
  /* Non-blocking so a select() false-positive can't stall the loop. */
  resp_set_nonblock(srv);
  return srv;
}

/* Standard reactor signal disposition: ignore SIGPIPE (write() returns
   EPIPE instead of killing us), trip resp_stop on SIGINT/SIGTERM. */
static void resp_install_signals(void) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, resp_sigint);
  signal(SIGTERM, resp_sigint);
}

/* Common fdset build: rfds gets srv + extra_rfd (if >= 0) + every
   client; wfds gets clients with pending output. Returns maxfd for
   select(). extra_rfd is an optional second readable fd, or -1 to skip. */
static inline int resp_build_fdset(int srv, int extra_rfd,
                                   fd_set *rfds, fd_set *wfds) {
  FD_ZERO(rfds);
  FD_ZERO(wfds);
  int maxfd = srv;
  FD_SET(srv, rfds);
  if (extra_rfd >= 0) {
    FD_SET(extra_rfd, rfds);
    if (extra_rfd > maxfd) maxfd = extra_rfd;
  }
  for (resp_client_t *cl = resp_clients; cl; cl = cl->next) {
    FD_SET(cl->fd, rfds);
    if (cl->wlen > cl->whead) FD_SET(cl->fd, wfds);
    if (cl->fd > maxfd) maxfd = cl->fd;
  }
  return maxfd;
}

/* Walk the per-reactor client list, dispatching IO and unlinking
   anyone the handler asks to drop. Tolerates unlink mid-walk via
   the saved next pointer. */
static inline void resp_drive_clients(fd_set *rfds, fd_set *wfds) {
  resp_client_t *cur = resp_clients;
  while (cur) {
    resp_client_t *next = cur->next;
    if (resp_client_handle_io(cur, rfds, wfds))
      resp_client_unlink(cur);
    cur = next;
  }
}

/* Free every client on this reactor — shutdown path only. */
static inline void resp_clients_free_all(void) {
  while (resp_clients) {
    resp_client_t *next = resp_clients->next;
    resp_client_free(resp_clients);
    resp_clients = next;
  }
}

/* Reactor teardown: close listen socket, free per-reactor client
   list, then clear the keyspace and drain the epoch retire list so
   valgrind/leaks see no live allocations. Don't lfkv_destroy here —
   peer reactors may still hold pointers into the same kv. */
static inline void resp_reactor_teardown(int srv) {
  close(srv);
  resp_active_port = 0;
  resp_clients_free_all();
  resp_kv_clear();
  epoch_drain_all();
}

/* Public entry — blocks until SIGINT/SIGTERM, then returns a process
   exit code. Called from main() when -r is on the command line. */
int resp_serve(int port) {
#if ALCOVE_SINGLE_THREADED
  (void)port;
  fprintf(stderr,
          "alcove: -r requires the multi-threaded build "
          "(rebuild without ALCOVE_SINGLE_THREADED).\n");
  return 1;
#else
  int srv = resp_listen(port);
  if (srv < 0) return 1;
  resp_active_port = port;
  resp_cmd_table_init();
  resp_install_signals();

  printf("alcove RESP2 server listening on 127.0.0.1:%d\n", port);
  fflush(stdout);

  /* Hoist TLS read out of the hot loop. wakefd < 0 when shard runtime
     init failed; reactor still serves clients, just can't be signalled
     from another thread. */
  shard_t *sh = current_shard;
  int wakefd = sh->runtime_ready == 1 ? alc_wake_fd(&sh->wake) : -1;

  /* Reactor must register with the epoch system so lock-free retire
     lists know about us. epoch_tick at the top of each select() iter
     publishes our quiescent point — between iterations no command is
     mid-flight, so any pointer we loaded is stale. */
  epoch_register();

  while (!resp_stop) {
    epoch_tick();
    resp_kv_maybe_sweep();
    fd_set rfds, wfds;
    int maxfd = resp_build_fdset(srv, wakefd, &rfds, &wfds);
    struct timeval tv = {1, 0};
    int r = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
    if (r < 0) {
      if (errno == EINTR) continue;
      perror("select");
      break;
    }

    /* Drain the wake fd if signalled. TODO(step-2.5): when cross-shard
       ops add a real inbox producer, also drain sh->inbox here and
       dispatch each message before resuming client I/O. */
    if (wakefd >= 0 && FD_ISSET(wakefd, &rfds)) alc_wake_drain(&sh->wake);

    if (FD_ISSET(srv, &rfds)) resp_accept_drain(srv);

    resp_drive_clients(&rfds, &wfds);
  }

  printf("\nalcove: shutting down RESP server\n");
  resp_reactor_teardown(srv);
  return 0;
#endif
}

/* ============================================================
   Combined REPL + RESP single-reactor loop (-R flag).
   ============================================================
   One thread, one select(). Watches stdin (REPL), the listen socket,
   and all client sockets. Whichever fires first runs to completion
   before the next iteration — so REPL eval and RESP command dispatch
   are mutually exclusive without a mutex.

   Tradeoff: a long REPL eval (e.g. (fib 40)) freezes RESP clients for
   the duration. For inspection-heavy use this is invisible; for
   sustained Redis throughput, prefer plain `-r` (RESP-only).

   Stdin: plain line-buffered accumulation with paren-balance
   detection. No readline integration in -R mode (yet) — line editing
   and history are disabled. Pipe input works fine. */

/* Forward decls — defined in alcove.c above the resp.c include.
   Single-reactor mode works in both threaded and single-threaded
   builds since there's literally one thread; no atomics needed. */
extern int toeval;

/* Drain a single complete top-level form from `acc`/`*plen`. Returns
   the byte count consumed (incl. trailing whitespace) or 0 if no
   complete form is buffered yet. The caller slides the remainder. */
static size_t resp_repl_consume_form(const char *acc, size_t len) {
  /* Walk char-by-char tracking paren depth + string state, identical
     to rl_paren_depth's scanner. Form-end is: depth back to 0 after
     having entered (depth>=1) any time, OR a complete bare token at
     top level (no parens) terminated by whitespace. */
  int depth = 0;
  int in_string = 0;
  int saw_token = 0; /* nonzero once we've seen any non-ws char */
  size_t i = 0;
  for (; i < len; i++) {
    char c = acc[i];
    if (in_string) {
      if (c == '\\' && i + 1 < len) i++;
      else if (c == '"') in_string = 0;
      continue;
    }
    if (c == '"') { in_string = 1; saw_token = 1; continue; }
    if (c == ';') {
      while (i < len && acc[i] != '\n') i++;
      continue;
    }
    if (c == '(') { depth++; saw_token = 1; continue; }
    if (c == ')') {
      depth--;
      if (depth == 0) return i + 1; /* complete parenthesised form */
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (saw_token && depth == 0)
        return i + 1; /* bare top-level token terminated by ws */
      continue;
    }
    saw_token = 1;
  }
  return 0; /* incomplete */
}

static void resp_repl_eval_print(env_t *global, const char *src, size_t n,
                                 int idx) {
  /* fmemopen so the existing reader walks the form unchanged. */
  FILE *fs = fmemopen((void *)src, n, "r");
  if (!fs) return;
  exp_t *form = reader(fs, 0, 0);
  fclose(fs);
  if (!form) return;
  if (iserror(form) && form->flags == EXP_ERROR_PARSING_EOF) {
    unrefexp(form);
    return;
  }
  /* Mirror the file-mode loop: handle quit/exit/toeval, then evaluate. */
  if (issymbol(form) &&
      (strcmp((char *)form->ptr, "quit") == 0 ||
       strcmp((char *)form->ptr, "exit") == 0)) {
    unrefexp(form);
    resp_stop = 1;
    return;
  }
  if (issymbol(form) && strcmp((char *)form->ptr, "toeval") == 0) {
    toeval = 1 - toeval;
    printf("%d\n", toeval);
    unrefexp(form);
    return;
  }
  exp_t *res = NULL;
  if (toeval)
    res = evaluate(form, global);
  else
    unrefexp(form);
  if (res) {
    printf("\x1B[31mOut[\x1B[91m%d\x1B[31m]:\x1B[39m", idx);
    print_node(res);
    unrefexp(res);
  } else
    printf("nil");
  printf("\n\n");
  fflush(stdout);
}

int resp_repl_serve(int port, env_t *global) {
  int srv = resp_listen(port);
  if (srv < 0) return 1;
  resp_set_nonblock(0); /* stdin */
  resp_active_port = port;
  resp_cmd_table_init();
  resp_install_signals();

  printf("alcove combined REPL + RESP2 listening on 127.0.0.1:%d\n", port);
  printf("(use redis-cli to talk RESP, or call (redis-keys), (redis-get k), "
         "(redis-type k), (redis-count), (redis-flush) from the REPL.)\n");
  fflush(stdout);

  /* Stdin accumulation buffer for line-buffered form reading. */
  size_t acc_cap = 4096;
  size_t acc_len = 0;
  char *acc = malloc(acc_cap);
  int idx = 0;
  int prompted = 0;

  epoch_register();
  while (!resp_stop) {
    epoch_tick();
    resp_kv_maybe_sweep();
    if (!prompted) {
      printf("\x1B[34mIn [\x1B[94m%d\x1B[34m]:\x1B[39m ", idx + 1);
      fflush(stdout);
      prompted = 1;
    }

    fd_set rfds, wfds;
    int maxfd = resp_build_fdset(srv, /*stdin*/ 0, &rfds, &wfds);
    struct timeval tv = {1, 0};
    int r = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
    if (r < 0) {
      if (errno == EINTR) continue;
      perror("select");
      break;
    }

    /* --- stdin: REPL --- */
    if (FD_ISSET(0, &rfds)) {
      if (acc_len + 4096 > acc_cap) {
        acc_cap *= 2;
        acc = realloc(acc, acc_cap);
      }
      ssize_t got = read(0, acc + acc_len, acc_cap - acc_len - 1);
      if (got == 0) { resp_stop = 1; break; } /* EOF on stdin */
      else if (got < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) { resp_stop = 1; break; }
      } else {
        acc_len += (size_t)got;
        acc[acc_len] = 0;
        /* Drain every complete form currently buffered. */
        for (;;) {
          size_t consumed = resp_repl_consume_form(acc, acc_len);
          if (!consumed) break;
          idx++;
          resp_repl_eval_print(global, acc, consumed, idx);
          memmove(acc, acc + consumed, acc_len - consumed);
          acc_len -= consumed;
          acc[acc_len] = 0;
          prompted = 0;
        }
        if (!prompted) {
          /* Reprompt for the next form. */
          printf("\x1B[34mIn [\x1B[94m%d\x1B[34m]:\x1B[39m ", idx + 1);
          fflush(stdout);
          prompted = 1;
        }
      }
    }

    /* --- listen socket --- */
    if (FD_ISSET(srv, &rfds)) resp_accept_drain(srv);

    /* --- per-client read/write --- */
    resp_drive_clients(&rfds, &wfds);
  }

  printf("\nalcove: shutting down combined REPL + RESP\n");
  resp_reactor_teardown(srv);
  free(acc);
  return 0;
}

/* ============================================================
   Redis inspector builtins — read resp_kv from REPL (-R only).
   ============================================================
   These run on whichever reactor evaluates Lisp, but resp_kv is the
   global lock-free table so it's safe regardless. Outside -R mode,
   resp_kv is NULL → keys=nil, count=0, get/type=nil/"none". */

const char doc_redis_count[] = "(redis-count) — number of keys in the running RESP server's db.";
exp_t *rediscountcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  return MAKE_FIX((int64_t)resp_kv_count());
}

/* Iteration ctx for rediskeyscmd — accumulates a Lisp list. */
typedef struct {
  exp_t *head, *tail;
} resp_keys_lisp_ctx_t;

static int resp_keys_lisp_cb(const char *k, size_t klen, exp_t *v,
                             int64_t exp_us, void *ctx) {
  (void)v; (void)exp_us;
  resp_keys_lisp_ctx_t *s = ctx;
  exp_t *node = make_node(make_string((char *)k, (int)klen));
  if (s->tail) s->tail = s->tail->next = node;
  else         s->head = s->tail = node;
  return 0;
}

const char doc_redis_keys[] = "(redis-keys) — list of all keys (as strings) in the RESP db. Skips expired entries.";
exp_t *rediskeyscmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  if (!resp_kv) return NIL_EXP;
  resp_keys_lisp_ctx_t ctx = {NULL, NULL};
  lfkv_foreach(resp_kv, resp_keys_lisp_cb, &ctx);
  return ctx.head ? ctx.head : NIL_EXP;
}

const char doc_redis_type[] = "(redis-type k) — RESP type of key k as a string: \"string\", \"list\", \"hash\", or \"none\".";
exp_t *redistypecmd(exp_t *e, env_t *env) {
  exp_t *kx = EVAL(cadr(e), env);
  if (iserror(kx)) { unrefexp(e); return kx; }
  if (!isstring(kx) && !isblob(kx)) {
    unrefexp(kx); unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "redis-type: key must be a string or blob");
  }
  const char *ks; size_t klen;
  if (isstring(kx)) { ks = (char *)kx->ptr; klen = strlen(ks); }
  else { alc_blob_t *bl = (alc_blob_t *)kx->ptr; ks = bl->bytes; klen = bl->len; }
  exp_t *v = resp_kv_lookup(ks, klen);
  const char *tn = v ? resp_type_name(v) : "none";
  exp_t *ret = make_string((char *)tn, (int)strlen(tn));
  if (v) unrefexp(v);
  unrefexp(kx); unrefexp(e);
  return ret;
}

const char doc_redis_get[] = "(redis-get k) — value of key k. Strings → blob (binary-safe); lists/hashes → nil (use redis-cli for those types).";
exp_t *redisgetcmd(exp_t *e, env_t *env) {
  exp_t *kx = EVAL(cadr(e), env);
  if (iserror(kx)) { unrefexp(e); return kx; }
  if (!isstring(kx) && !isblob(kx)) {
    unrefexp(kx); unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "redis-get: key must be a string or blob");
  }
  const char *ks; size_t klen;
  if (isstring(kx)) { ks = (char *)kx->ptr; klen = strlen(ks); }
  else { alc_blob_t *bl = (alc_blob_t *)kx->ptr; ks = bl->bytes; klen = bl->len; }
  exp_t *v = resp_kv_lookup(ks, klen);
  exp_t *ret = NIL_EXP;
  if (v && isblob(v)) {
    alc_blob_t *bl = (alc_blob_t *)v->ptr;
    ret = make_blob(bl->bytes, bl->len);
  }
  if (v) unrefexp(v);
  unrefexp(kx); unrefexp(e);
  return ret;
}

const char doc_redis_flush[] = "(redis-flush) — remove every key from the RESP db (FLUSHDB). Returns t.";
exp_t *redisflushcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  resp_kv_clear();
  return TRUE_EXP;
}

const char doc_redis_port[] = "(redis-port) — port the RESP server is bound to, or 0 if not running.";
exp_t *redisportcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  return MAKE_FIX((int64_t)resp_active_port);
}

/* ---------- user-defined Redis commands -------------------------------
   (redis-defcmd "NAME" fn) registers `fn` under uppercase NAME so that
   `redis-cli NAME ...` invokes it. fn is a 1-arity lambda; its single
   parameter receives the RESP bulk arguments after the command name as
   a Lisp pair-list of strings (nil if none). The lambda's return value
   is encoded back to RESP:
     nil       → $-1            (null bulk)
     t         → +OK
     fixnum    → :N             (integer)
     string    → $...           (bulk)
     blob      → $...           (bulk; binary-safe)
     pair list → *N $... ...    (array, recursive)
     error     → -ERR <message>
   Single-reactor invariant: dispatch runs on the same thread that
   evaluates Lisp, so the dict + env are touched without locking. */

static dict_t *resp_user_commands = NULL;
static env_t *resp_user_env = NULL;

static void resp_user_upper(const char *src, long len, char *dst, size_t cap) {
  size_t i;
  size_t n = (size_t)len < cap - 1 ? (size_t)len : cap - 1;
  for (i = 0; i < n; i++) {
    char ch = src[i];
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    dst[i] = ch;
  }
  dst[n] = '\0';
}

static void resp_user_encode(resp_client_t *c, exp_t *v) {
  if (v == NIL_EXP || v == NULL) {
    resp_write_nil(c);
    return;
  }
  if (v == TRUE_EXP) {
    resp_write_simple(c, "OK");
    return;
  }
  if (isnumber(v)) {
    resp_write_int(c, (long long)FIX_VAL(v));
    return;
  }
  if (iserror(v)) {
    char buf[512];
    const char *msg = (v->ptr) ? (const char *)v->ptr : "user command error";
    int n = snprintf(buf, sizeof buf, "ERR %s", msg);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    resp_write(c, "-", 1);
    resp_write(c, buf, n);
    resp_write(c, "\r\n", 2);
    return;
  }
  if (isstring(v)) {
    const char *s = (const char *)v->ptr;
    resp_write_bulk(c, s, strlen(s));
    return;
  }
  if (isblob(v)) {
    alc_blob_t *b = (alc_blob_t *)v->ptr;
    resp_write_bulk(c, b->bytes, b->len);
    return;
  }
  if (ischar(v)) {
    char ch = (char)FIX_VAL(v);
    resp_write_bulk(c, &ch, 1);
    return;
  }
  if (ispair(v)) {
    long n = 0;
    exp_t *p;
    for (p = v; ispair(p); p = cdr(p)) n++;
    resp_write_array_hdr(c, n);
    for (p = v; ispair(p); p = cdr(p)) resp_user_encode(c, car(p));
    return;
  }
  if (islist(v)) {
    alc_list_t *l = (alc_list_t *)v->ptr;
    resp_write_array_hdr(c, l->len);
    for (alc_listnode_t *n = l->head; n; n = n->next)
      resp_user_encode(c, n->val);
    return;
  }
  /* Fallback — fold to a printable bulk via the same path print_node uses
     would require buffering; for now emit the type as a debug hint. */
  char tag[64];
  int n = snprintf(tag, sizeof tag, "<unencodable type %d>", v->type);
  if (n < 0) n = 0;
  if ((size_t)n >= sizeof tag) n = (int)sizeof tag - 1;
  resp_write_bulk(c, tag, (size_t)n);
}

static int resp_user_dispatch(resp_client_t *c, const char *cmd, long clen,
                              char **argv, long *argl, int argc) {
  if (!resp_user_commands || resp_user_commands->ht[0].size == 0) return 0;
  char name[256];
  resp_user_upper(cmd, clen, name, sizeof name);
  keyval_t *k = set_get_keyval_dict(resp_user_commands, name, NULL);
  if (!k || !k->val) return 0;
  exp_t *fn = k->val;

  /* Marshal RESP bulks as a single Lisp pair-list bound to the lambda's
     one parameter. So `(def myfn (args) ...)` sees `args` = the list of
     strings. Empty arg lists pass NIL_EXP. */
  exp_t *arglist = NIL_EXP, *cur = NULL;
  int rargc = argc - 1;
  for (int i = 0; i < rargc; i++) {
    exp_t *s = make_string(argv[i + 1], (int)argl[i + 1]);
    exp_t *node = make_node(s);
    if (cur) cur = cur->next = node;
    else { arglist = cur = node; }
  }
  exp_t *fn_ref = refexp(fn);
  exp_t *vargv[1] = {arglist};
  exp_t *ret = vm_invoke_values(fn_ref, 1, vargv, resp_user_env);
  unrefexp(fn_ref);
  resp_user_encode(c, ret);
  if (ret) unrefexp(ret);
  return 1;
}

const char doc_redis_defcmd[] = "(redis-defcmd \"NAME\" fn) — register fn as a Redis command callable from redis-cli. fn must take one parameter; it receives the RESP bulk args after the cmd name as a list of strings (nil if none). Returns t.";
exp_t *rediscmddefcmd(exp_t *e, env_t *env) {
  exp_t *nx = EVAL(cadr(e), env);
  if (iserror(nx)) { unrefexp(e); return nx; }
  if (!isstring(nx) && !isblob(nx)) {
    unrefexp(nx); unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "redis-defcmd: command name must be a string or blob");
  }
  exp_t *fx = EVAL(caddr(e), env);
  if (iserror(fx)) { unrefexp(nx); unrefexp(e); return fx; }
  if (!islambda(fx)) {
    unrefexp(nx); unrefexp(fx); unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "redis-defcmd: second arg must be a lambda");
  }
  const char *raw = isstring(nx) ? (char *)nx->ptr
                                 : ((alc_blob_t *)nx->ptr)->bytes;
  long rawlen = (long)(isstring(nx) ? strlen(raw)
                                    : ((alc_blob_t *)nx->ptr)->len);
  char name[256];
  resp_user_upper(raw, rawlen, name, sizeof name);
  if (!resp_user_commands)
    resp_user_commands = memalloc(1, sizeof(dict_t));
  resp_user_env = env;
  set_get_keyval_dict(resp_user_commands, name, fx);
  unrefexp(nx); unrefexp(fx); unrefexp(e);
  return TRUE_EXP;
}

const char doc_redis_undefcmd[] = "(redis-undefcmd \"NAME\") — remove a previously registered Redis command. Returns t if removed, nil if not found.";
exp_t *rediscmdundefcmd(exp_t *e, env_t *env) {
  exp_t *nx = EVAL(cadr(e), env);
  if (iserror(nx)) { unrefexp(e); return nx; }
  if (!isstring(nx) && !isblob(nx)) {
    unrefexp(nx); unrefexp(e);
    return error(ERROR_ILLEGAL_VALUE, NULL, env,
                 "redis-undefcmd: command name must be a string or blob");
  }
  const char *raw = isstring(nx) ? (char *)nx->ptr
                                 : ((alc_blob_t *)nx->ptr)->bytes;
  long rawlen = (long)(isstring(nx) ? strlen(raw)
                                    : ((alc_blob_t *)nx->ptr)->len);
  char name[256];
  resp_user_upper(raw, rawlen, name, sizeof name);
  exp_t *ret = NIL_EXP;
  if (resp_user_commands && resp_user_commands->ht[0].size) {
    keyval_t *k = set_get_keyval_dict(resp_user_commands, name, NULL);
    if (k && strcmp(k->key, name) == 0) {
      del_keyval_dict(resp_user_commands, name);
      ret = TRUE_EXP;
    }
  }
  unrefexp(nx); unrefexp(e);
  return ret;
}

const char doc_redis_cmds[] = "(redis-cmds) — list of currently registered user Redis command names (uppercase).";
exp_t *rediscmdscmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  exp_t *ret = NIL_EXP, *cur = NULL;
  if (!resp_user_commands || resp_user_commands->ht[0].size == 0)
    return NIL_EXP;
  kvht_t *h = &resp_user_commands->ht[0];
  for (unsigned long b = 0; b < h->size; b++) {
    for (keyval_t *k = h->table[b]; k; k = k->next) {
      exp_t *node = make_node(make_string((char *)k->key,
                                          (int)strlen((char *)k->key)));
      if (cur) cur = cur->next = node;
      else { ret = cur = node; }
    }
  }
  return ret;
}
