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
  size_t rlen, rcap;
  char *wbuf;
  /* wbuf is a window [whead, wlen) — appends advance wlen, drains
     advance whead. Invariant: whead <= wlen <= wcap. */
  size_t whead, wlen, wcap;
  struct resp_client *next;
} resp_client_t;

static resp_client_t *resp_clients = NULL;
static dict_t *resp_db = NULL; /* lazily created on first SET */
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
   Thin wrappers over alcove's dict_t. TTL is sign-encoded on
   keyval_t->timestamp: 0 = no TTL, < 0 = absolute-µs expire-at. */

static int64_t resp_now_us(void) { return gettimeusec(); }

static unsigned long dict_count(dict_t *d) {
  return d ? d->ht[0].used + d->ht[1].used : 0;
}

/* TTL accessors — keep the sign-encoding hidden. */
static inline int kv_has_ttl(keyval_t *k) { return k->timestamp < 0; }
static inline int kv_is_expired(keyval_t *k, int64_t now) {
  return k->timestamp < 0 && (-k->timestamp) <= now;
}
static inline int64_t kv_ttl_remaining_us(keyval_t *k, int64_t now) {
  return (-k->timestamp) - now;
}
static inline void kv_set_expire_us(keyval_t *k, int64_t expire_at_us) {
  k->timestamp = -expire_at_us;
}
static inline void kv_clear_ttl(keyval_t *k) { k->timestamp = 0; }

static void resp_db_ensure(void) {
  if (!resp_db) resp_db = create_dict();
}

static keyval_t *resp_db_find(const char *key) {
  return resp_db ? set_get_keyval_dict(resp_db, (char *)key, NULL) : NULL;
}

/* Lookup with lazy TTL eviction. Returns NULL if missing or expired. */
static keyval_t *resp_db_lookup(const char *key) {
  keyval_t *k = resp_db_find(key);
  if (!k) return NULL;
  if (kv_is_expired(k, resp_now_us())) {
    del_keyval_dict(resp_db, (char *)key);
    return NULL;
  }
  return k;
}

/* SET-style write: replaces value, clears any prior TTL.
   `val` ownership transfers in — set_get_keyval_dict takes its own ref
   via refexp, then we drop the caller's ref. */
static keyval_t *resp_db_set(const char *key, exp_t *val) {
  resp_db_ensure();
  keyval_t *k = set_get_keyval_dict(resp_db, (char *)key, val);
  if (k) kv_clear_ttl(k);
  unrefexp(val);
  return k;
}

static int resp_db_del(const char *key) {
  return resp_db ? del_keyval_dict(resp_db, (char *)key) : 0;
}

static void resp_db_clear(void) {
  if (!resp_db) return;
  destroy_dict(resp_db);
  resp_db = NULL;
}

/* Evict every expired entry in one pass. KEYS * + count-exact paths
   call this so the array header matches the emitted count. */
static void resp_db_evict_expired(void) {
  if (!resp_db) return;
  int64_t now = resp_now_us();
  for (int hi = 0; hi < 2; hi++) {
    kvht_t *h = &resp_db->ht[hi];
    for (unsigned long b = 0; b < h->size; b++) {
      keyval_t **pp = &h->table[b];
      while (*pp) {
        keyval_t *kv = *pp;
        if (kv_is_expired(kv, now)) {
          char *k = (char *)kv->key;
          *pp = kv->next;
          unrefexp(kv->val);
          free(k);
          free(kv);
          h->used--;
        } else {
          pp = &kv->next;
        }
      }
    }
  }
}

/* Throttled background sweep — called from the reactor's top-of-loop
   so write-then-never-read TTL'd keys can't pile up unboundedly. The
   1s interval matches the reactor's select() timeout, so an idle
   server still ticks the sweep at the same cadence. */
#define RESP_SWEEP_INTERVAL_US 1000000
static int64_t resp_last_sweep_us = 0;
static void resp_db_maybe_sweep(void) {
  if (!resp_db) return;
  int64_t now = resp_now_us();
  if (now - resp_last_sweep_us < RESP_SWEEP_INTERVAL_US) return;
  resp_last_sweep_us = now;
  resp_db_evict_expired();
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

/* ---------- RESP frame parser ----------
   Returns:
     >0 = bytes consumed, argv/argl/argc filled (pointers alias rbuf,
          valid only until next read; dispatch must finish first)
      0 = need more data
     <0 = protocol error (caller drops the client) */
static int resp_parse_one(char *buf, size_t len, char ***argv_out,
                          long **argl_out, int *argc_out) {
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

    char **argv = malloc(sizeof(char *) * n);
    long *argl = malloc(sizeof(long) * n);
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

  char **argv = malloc(sizeof(char *) * n);
  long *argl = malloc(sizeof(long) * n);

  for (long a = 0; a < n; a++) {
    if (i >= len) goto need_more;
    if (buf[i] != '$') goto bad;
    i++;
    long blen = 0;
    while (i < len && buf[i] != '\r') {
      if (buf[i] < '0' || buf[i] > '9') goto bad;
      blen = blen * 10 + (buf[i] - '0');
      if (blen > RESP_MAX_BULK) goto bad;
      i++;
    }
    if (i + 1 >= len) goto need_more;
    if (buf[i] != '\r' || buf[i + 1] != '\n') goto bad;
    i += 2;
    if (i + (size_t)blen + 2 > len) goto need_more;
    if (buf[i + blen] != '\r' || buf[i + blen + 1] != '\n') goto bad;
    argv[a] = buf + i;
    argl[a] = blen;
    i += blen + 2;
  }

  *argv_out = argv;
  *argl_out = argl;
  *argc_out = (int)n;
  return (int)i;

need_more:
  free(argv);
  free(argl);
  return 0;
bad:
  free(argv);
  free(argl);
  return -1;
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
  resp_write_int(c, (long long)dict_count(resp_db));
}

static void cmd_flushdb(resp_client_t *c) {
  resp_db_clear();
  resp_write_simple(c, "OK");
}

static void cmd_keys_star(resp_client_t *c, char **argv, long *argl,
                          int argc) {
  ARGN(2);
  if (argl[1] != 1 || argv[1][0] != '*') {
    resp_write_err(c, "ERR alcove RESP server only supports KEYS *");
    return;
  }
  /* Lazy-expire pass first, then count + emit. The header count must
     exactly match the number of bulks we emit. */
  resp_db_evict_expired();
  resp_write_array_hdr(c, (long long)dict_count(resp_db));
  if (!resp_db) return;
  for (int hi = 0; hi < 2; hi++) {
    kvht_t *h = &resp_db->ht[hi];
    for (unsigned long b = 0; b < h->size; b++)
      for (keyval_t *kv = h->table[b]; kv; kv = kv->next)
        resp_write_bulk(c, (char *)kv->key, strlen((char *)kv->key));
  }
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
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_simple(c, "none");
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  free(k);
  resp_write_simple(c, kv ? resp_type_name(kv->val) : "none");
}

static void cmd_del(resp_client_t *c, char **argv, long *argl, int argc) {
  ARG_AT_LEAST(2);
  long long deleted = 0;
  for (int a = 1; a < argc; a++) {
    char *k = resp_dup_key(argv[a], argl[a]);
    if (!k) continue;
    /* lookup first to honour lazy expiry (an expired key shouldn't
       count as a successful delete) */
    if (resp_db_lookup(k)) {
      resp_db_del(k);
      deleted++;
    }
    free(k);
  }
  resp_write_int(c, deleted);
}

static void cmd_exists(resp_client_t *c, char **argv, long *argl, int argc) {
  ARG_AT_LEAST(2);
  long long present = 0;
  for (int a = 1; a < argc; a++) {
    char *k = resp_dup_key(argv[a], argl[a]);
    if (!k) continue;
    if (resp_db_lookup(k)) present++;
    free(k);
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
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, 0);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  if (!kv) {
    free(k);
    resp_write_int(c, 0);
    return;
  }
  /* Negative TTL == immediate delete (Redis semantics). */
  if (delta <= 0) {
    resp_db_del(k);
    free(k);
    resp_write_int(c, 1);
    return;
  }
  int64_t deadline = resp_now_us() + delta * (millis ? 1000LL : 1000000LL);
  kv_set_expire_us(kv, deadline);
  free(k);
  resp_write_int(c, 1);
}

/* TTL key | PTTL key. Returns:
     -2 if key missing
     -1 if key exists but no expire
     remaining time otherwise */
static void cmd_ttl(resp_client_t *c, char **argv, long *argl, int argc,
                    int millis) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, -2);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  if (!kv) {
    free(k);
    resp_write_int(c, -2);
    return;
  }
  if (!kv_has_ttl(kv)) {
    free(k);
    resp_write_int(c, -1);
    return;
  }
  int64_t left = kv_ttl_remaining_us(kv, resp_now_us());
  if (left < 0) left = 0;
  free(k);
  resp_write_int(c, millis ? (long long)(left / 1000)
                           : (long long)(left / 1000000));
}

static void cmd_persist(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, 0);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  if (!kv || !kv_has_ttl(kv)) {
    free(k);
    resp_write_int(c, 0);
    return;
  }
  kv_clear_ttl(kv);
  free(k);
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
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_err(c, "ERR keys with embedded NUL not supported");
    return;
  }
  keyval_t *exist = resp_db_lookup(k);
  if (nx && exist) {
    free(k);
    resp_write_nil(c);
    return;
  }
  if (xx && !exist) {
    free(k);
    resp_write_nil(c);
    return;
  }
  keyval_t *kv = resp_db_set(k, make_blob(argv[2], (size_t)argl[2]));
  if (expire_us && kv) kv_set_expire_us(kv, resp_now_us() + expire_us);
  free(k);
  resp_write_simple(c, "OK");
}

static void cmd_get(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_nil(c);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  free(k);
  if (!kv) {
    resp_write_nil(c);
    return;
  }
  if (!isblob(kv->val)) {
    resp_write_wrongtype(c);
    return;
  }
  alc_blob_t *b = (alc_blob_t *)kv->val->ptr;
  resp_write_bulk(c, b->bytes, b->len);
}

static void cmd_strlen(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, 0);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  free(k);
  if (!kv) {
    resp_write_int(c, 0);
    return;
  }
  if (!isblob(kv->val)) {
    resp_write_wrongtype(c);
    return;
  }
  resp_write_int(c, (long long)blob_len(kv->val));
}

/* Shared core for INCR/DECR/INCRBY/DECRBY. Caller owns `key`. Mutates
   kv->val in place to preserve any existing TTL across the operation. */
static void resp_apply_incr(resp_client_t *c, const char *key,
                            long long delta) {
  keyval_t *kv = resp_db_lookup(key);
  long long cur = 0;
  if (kv) {
    if (!isblob(kv->val)) {
      resp_write_wrongtype(c);
      return;
    }
    alc_blob_t *b = (alc_blob_t *)kv->val->ptr;
    if (b->len == 0 || b->len > 30) {
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
      resp_write_err(c, "ERR value is not an integer or out of range");
      return;
    }
  }
  /* Signed-overflow guard before the addition itself wraps. */
  if ((delta > 0 && cur > LLONG_MAX - delta) ||
      (delta < 0 && cur < LLONG_MIN - delta)) {
    resp_write_err(c, "ERR increment or decrement would overflow");
    return;
  }
  cur += delta;
  char buf[32];
  int n = resp_i64_to_ascii(buf, (int64_t)cur);
  exp_t *new_blob = make_blob(buf, (size_t)n);
  if (kv) {
    /* In-place mutation preserves kv->timestamp (TTL survives INCR). */
    unrefexp(kv->val);
    kv->val = new_blob;
  } else {
    resp_db_set(key, new_blob);
  }
  resp_write_int(c, cur);
}

/* INCR / DECR — one-step ±1. */
static void cmd_incr_decr(resp_client_t *c, char **argv, long *argl, int argc,
                          int sign) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_err(c, "ERR keys with embedded NUL not supported");
    return;
  }
  resp_apply_incr(c, k, sign);
  free(k);
}

/* INCRBY / DECRBY — caller-supplied integer delta in argv[2]. */
static void cmd_incrby_decrby(resp_client_t *c, char **argv, long *argl,
                              int argc, int sign) {
  ARGN(3);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_err(c, "ERR keys with embedded NUL not supported");
    return;
  }
  if (argl[2] == 0 || argl[2] > 20) {
    free(k);
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
    free(k);
    resp_write_err(c, "ERR value is not an integer or out of range");
    return;
  }
  if (sign < 0) {
    /* DECRBY: negate. LLONG_MIN has no positive counterpart. */
    if (delta == LLONG_MIN) {
      free(k);
      resp_write_err(c, "ERR value is not an integer or out of range");
      return;
    }
    delta = -delta;
  }
  resp_apply_incr(c, k, delta);
  free(k);
}

/* APPEND key value — append to an existing string, or create the key
   holding `value` if missing. Returns the new length. In-place mutation
   on the existing path preserves TTL. */
static void cmd_append(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(3);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_err(c, "ERR keys with embedded NUL not supported");
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  if (!kv) {
    resp_db_set(k, make_blob(argv[2], (size_t)argl[2]));
    free(k);
    resp_write_int(c, (long long)argl[2]);
    return;
  }
  if (!isblob(kv->val)) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  alc_blob_t *old = (alc_blob_t *)kv->val->ptr;
  size_t old_n = old->len;
  size_t add = (size_t)argl[2];
  size_t new_n = old_n + add;
  /* Allocate a fresh blob (flex-array, single alloc) and copy.
     Cheaper than realloc'ing the existing one because alc_blob_t is
     header+payload in one allocation — there's no detachable payload. */
  exp_t *fresh = make_blob(NULL, new_n);
  alc_blob_t *nb = (alc_blob_t *)fresh->ptr;
  if (old_n) memcpy(nb->bytes, old->bytes, old_n);
  if (add) memcpy(nb->bytes + old_n, argv[2], add);
  unrefexp(kv->val);
  kv->val = fresh; /* make_blob gave it 1 ref */
  free(k);
  resp_write_int(c, (long long)new_n);
}

/* ---------- list commands ---------- */

/* Returns the entry's alc_list_t, creating one if missing. NULL means
   wrongtype (existing key is not a list). */
static alc_list_t *resp_get_or_create_list(const char *key, int create) {
  keyval_t *kv = resp_db_lookup(key);
  if (kv) {
    if (!islist(kv->val)) return NULL;
    return (alc_list_t *)kv->val->ptr;
  }
  if (!create) return NULL;
  kv = resp_db_set(key, make_list_exp());
  return (alc_list_t *)kv->val->ptr;
}

static void cmd_lpush_rpush(resp_client_t *c, char **argv, long *argl,
                            int argc, int left) {
  ARG_AT_LEAST(3);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_err(c, "ERR keys with embedded NUL not supported");
    return;
  }
  /* Type-check existing key BEFORE creating, so wrongtype doesn't
     leave a stray empty list behind. */
  keyval_t *kv = resp_db_lookup(k);
  if (kv && !islist(kv->val)) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  alc_list_t *l = resp_get_or_create_list(k, 1);
  for (int i = 2; i < argc; i++) {
    exp_t *node_val = make_blob(argv[i], (size_t)argl[i]);
    if (left) alc_list_push_left(l, node_val);
    else      alc_list_push_right(l, node_val);
  }
  long long len = l->len;
  free(k);
  resp_write_int(c, len);
}

static void cmd_lpop_rpop(resp_client_t *c, char **argv, long *argl, int argc,
                          int left) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_nil(c);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  if (!kv) {
    free(k);
    resp_write_nil(c);
    return;
  }
  if (!islist(kv->val)) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  alc_list_t *l = (alc_list_t *)kv->val->ptr;
  if (l->len == 0) {
    free(k);
    resp_write_nil(c);
    return;
  }
  alc_listnode_t *nd = left ? l->head : l->tail;
  if (left) {
    l->head = nd->next;
    if (l->head) l->head->prev = NULL;
    else l->tail = NULL;
  } else {
    l->tail = nd->prev;
    if (l->tail) l->tail->next = NULL;
    else l->head = NULL;
  }
  l->len--;
  /* nd->val is an EXP_BLOB owned by the node; emit before dropping the
     ref so the bytes don't get reaped under us. */
  alc_blob_t *b = (alc_blob_t *)nd->val->ptr;
  resp_write_bulk(c, b->bytes, b->len);
  unrefexp(nd->val);
  free(nd);
  /* Empty list is deleted (Redis semantics — keys never hold
     empty containers). */
  if (l->len == 0) resp_db_del(k);
  free(k);
}

static void cmd_llen(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, 0);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  free(k);
  if (!kv) {
    resp_write_int(c, 0);
    return;
  }
  if (!islist(kv->val)) {
    resp_write_wrongtype(c);
    return;
  }
  resp_write_int(c, (long long)((alc_list_t *)kv->val->ptr)->len);
}

static void cmd_lindex(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(3);
  long long idx;
  if (!resp_arg_to_ll(argv[2], argl[2], &idx)) {
    resp_write_err(c, "ERR value is not an integer or out of range");
    return;
  }
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_nil(c);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  free(k);
  if (!kv) {
    resp_write_nil(c);
    return;
  }
  if (!islist(kv->val)) {
    resp_write_wrongtype(c);
    return;
  }
  alc_list_t *l = (alc_list_t *)kv->val->ptr;
  long ni = resp_norm_index((long)idx, l->len);
  if (ni < 0) {
    resp_write_nil(c);
    return;
  }
  /* Walk from whichever end is closer. */
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
}

static void cmd_lrange(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(4);
  long long start, stop;
  if (!resp_arg_to_ll(argv[2], argl[2], &start) ||
      !resp_arg_to_ll(argv[3], argl[3], &stop)) {
    resp_write_err(c, "ERR value is not an integer or out of range");
    return;
  }
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_array_hdr(c, 0);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  free(k);
  if (!kv) {
    resp_write_array_hdr(c, 0);
    return;
  }
  if (!islist(kv->val)) {
    resp_write_wrongtype(c);
    return;
  }
  alc_list_t *l = (alc_list_t *)kv->val->ptr;
  long len = l->len;
  /* Redis LRANGE: negative indices count from end; out-of-range
     start/stop get clamped. Empty range is empty array, not error. */
  if (start < 0) start += len;
  if (stop < 0) stop += len;
  if (start < 0) start = 0;
  if (stop >= len) stop = len - 1;
  if (start > stop || start >= len) {
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
}

/* ---------- hash commands ----------
   A Redis hash is an EXP_DICT whose dict_t holds field→EXP_BLOB. */

static dict_t *resp_get_or_create_hash(const char *key, int create) {
  keyval_t *kv = resp_db_lookup(key);
  if (kv) {
    if (!isdict(kv->val)) return NULL;
    return (dict_t *)kv->val->ptr;
  }
  if (!create) return NULL;
  kv = resp_db_set(key, make_dict_exp());
  return (dict_t *)kv->val->ptr;
}

/* HSET key field value [field value ...] — returns count of NEW
   fields created (not updated). */
static void cmd_hset(resp_client_t *c, char **argv, long *argl, int argc) {
  ARG_AT_LEAST(4);
  if ((argc - 2) % 2 != 0) {
    resp_write_err(c, "ERR wrong number of arguments for 'hset'");
    return;
  }
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_err(c, "ERR keys with embedded NUL not supported");
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  if (kv && !isdict(kv->val)) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  dict_t *h = resp_get_or_create_hash(k, 1);
  long long created = 0;
  for (int i = 2; i + 1 < argc; i += 2) {
    char *fk = resp_dup_key(argv[i], argl[i]);
    if (!fk) continue; /* silently skip NUL field names */
    exp_t *fv = make_blob(argv[i + 1], (size_t)argl[i + 1]);
    created += resp_dict_field_set(h, fk, fv);
    free(fk);
  }
  free(k);
  resp_write_int(c, created);
}

static void cmd_hget(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(3);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_nil(c);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  if (!kv) {
    free(k);
    resp_write_nil(c);
    return;
  }
  if (!isdict(kv->val)) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  char *fk = resp_dup_key(argv[2], argl[2]);
  if (!fk) {
    free(k);
    resp_write_nil(c);
    return;
  }
  exp_t *fv = resp_dict_field_get((dict_t *)kv->val->ptr, fk);
  free(k);
  free(fk);
  if (!fv) {
    resp_write_nil(c);
    return;
  }
  alc_blob_t *b = (alc_blob_t *)fv->ptr;
  resp_write_bulk(c, b->bytes, b->len);
}

static void cmd_hdel(resp_client_t *c, char **argv, long *argl, int argc) {
  ARG_AT_LEAST(3);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, 0);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  if (!kv) {
    free(k);
    resp_write_int(c, 0);
    return;
  }
  if (!isdict(kv->val)) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  dict_t *h = (dict_t *)kv->val->ptr;
  long long deleted = 0;
  for (int i = 2; i < argc; i++) {
    char *fk = resp_dup_key(argv[i], argl[i]);
    if (!fk) continue;
    deleted += resp_dict_field_del(h, fk);
    free(fk);
  }
  /* Drop the key if the hash became empty (Redis container rule). */
  if (dict_count(h) == 0) resp_db_del(k);
  free(k);
  resp_write_int(c, deleted);
}

static void cmd_hexists(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(3);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, 0);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  if (!kv) {
    free(k);
    resp_write_int(c, 0);
    return;
  }
  if (!isdict(kv->val)) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  char *fk = resp_dup_key(argv[2], argl[2]);
  if (!fk) {
    free(k);
    resp_write_int(c, 0);
    return;
  }
  exp_t *fv = resp_dict_field_get((dict_t *)kv->val->ptr, fk);
  free(k);
  free(fk);
  resp_write_int(c, fv ? 1 : 0);
}

static void cmd_hlen(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, 0);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  free(k);
  if (!kv) {
    resp_write_int(c, 0);
    return;
  }
  if (!isdict(kv->val)) {
    resp_write_wrongtype(c);
    return;
  }
  resp_write_int(c, (long long)dict_count((dict_t *)kv->val->ptr));
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
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_array_hdr(c, 0);
    return;
  }
  keyval_t *kv = resp_db_lookup(k);
  free(k);
  if (!kv) {
    resp_write_array_hdr(c, 0);
    return;
  }
  if (!isdict(kv->val)) {
    resp_write_wrongtype(c);
    return;
  }
  hash_emit(c, (dict_t *)kv->val->ptr, keys, vals);
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

static void resp_cmd_table_init(void) {
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
    int consumed = resp_parse_one(c->rbuf, c->rlen, &argv, &argl, &argc);
    if (consumed == 0) return;
    if (consumed < 0) {
      resp_write_err(c, "ERR Protocol error");
      shutdown(c->fd, SHUT_RD);
      return;
    }
    resp_dispatch(c, argv, argl, argc);
    free(argv);
    free(argl);
    memmove(c->rbuf, c->rbuf + consumed, c->rlen - consumed);
    c->rlen -= consumed;
  }
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
  if (!cl->rbuf) { close(cfd); free(cl); return NULL; }
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
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) {
    perror("socket");
    return 1;
  }
  int yes = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  /* SO_REUSEPORT lets N reactors bind the same port; the kernel
     hashes flows across them (Linux) or accepts on whichever wakes
     first (macOS, weaker fairness). At N=1 it's a no-op. */
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
    close(srv);
    return 1;
  }
  if (listen(srv, RESP_LISTEN_BACKLOG) < 0) {
    perror("listen");
    close(srv);
    return 1;
  }
  /* Non-blocking so a select() false-positive can't stall the loop. */
  resp_set_nonblock(srv);
  resp_active_port = port;
  resp_cmd_table_init();

  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, resp_sigint);
  signal(SIGTERM, resp_sigint);

  printf("alcove RESP2 server listening on 127.0.0.1:%d\n", port);
  fflush(stdout);

  /* Hoist TLS read out of the hot loop. wakefd < 0 when shard runtime
     init failed; reactor still serves clients, just can't be signalled
     from another thread. */
  shard_t *sh = current_shard;
  int wakefd = sh->runtime_ready == 1 ? alc_wake_fd(&sh->wake) : -1;

  while (!resp_stop) {
    resp_db_maybe_sweep();
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int maxfd = srv;
    FD_SET(srv, &rfds);
    if (wakefd >= 0) {
      FD_SET(wakefd, &rfds);
      if (wakefd > maxfd) maxfd = wakefd;
    }
    for (resp_client_t *cl = resp_clients; cl; cl = cl->next) {
      FD_SET(cl->fd, &rfds);
      if (cl->wlen > cl->whead) FD_SET(cl->fd, &wfds);
      if (cl->fd > maxfd) maxfd = cl->fd;
    }
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

    resp_client_t *cur = resp_clients;
    while (cur) {
      resp_client_t *next = cur->next;
      int drop = 0;

      if (FD_ISSET(cur->fd, &rfds)) {
        if (cur->rlen + 4096 > cur->rcap) {
          if (cur->rcap >= RESP_RBUF_MAX) {
            drop = 1;
          } else {
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
          }
        }
      }

      if (!drop && FD_ISSET(cur->fd, &wfds) && cur->wlen > cur->whead)
        drop = resp_client_drain_write(cur);

      if (drop) resp_client_unlink(cur);
      cur = next;
    }
  }

  printf("\nalcove: shutting down RESP server\n");
  close(srv);
  resp_active_port = 0;
  while (resp_clients) {
    resp_client_t *next = resp_clients->next;
    resp_client_free(resp_clients);
    resp_clients = next;
  }
  resp_db_clear();
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
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) { perror("socket"); return 1; }
  int yes = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
    fprintf(stderr, "alcove: bind 127.0.0.1:%d failed: %s\n", port,
            strerror(errno));
    close(srv);
    return 1;
  }
  if (listen(srv, RESP_LISTEN_BACKLOG) < 0) {
    perror("listen");
    close(srv);
    return 1;
  }
  resp_set_nonblock(srv);
  resp_set_nonblock(0); /* stdin */
  resp_active_port = port;
  resp_cmd_table_init();

  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, resp_sigint);
  signal(SIGTERM, resp_sigint);

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

  while (!resp_stop) {
    resp_db_maybe_sweep();
    if (!prompted) {
      printf("\x1B[34mIn [\x1B[94m%d\x1B[34m]:\x1B[39m ", idx + 1);
      fflush(stdout);
      prompted = 1;
    }

    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int maxfd = srv;
    FD_SET(srv, &rfds);
    FD_SET(0, &rfds); /* stdin */
    for (resp_client_t *cl = resp_clients; cl; cl = cl->next) {
      FD_SET(cl->fd, &rfds);
      if (cl->wlen > cl->whead) FD_SET(cl->fd, &wfds);
      if (cl->fd > maxfd) maxfd = cl->fd;
    }
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
    resp_client_t *cur = resp_clients;
    while (cur) {
      resp_client_t *next = cur->next;
      int drop = 0;

      if (FD_ISSET(cur->fd, &rfds)) {
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
          }
        }
      }

      if (!drop && FD_ISSET(cur->fd, &wfds) && cur->wlen > cur->whead)
        drop = resp_client_drain_write(cur);

      if (drop) resp_client_unlink(cur);
      cur = next;
    }
  }

  printf("\nalcove: shutting down combined REPL + RESP\n");
  close(srv);
  resp_active_port = 0;
  while (resp_clients) {
    resp_client_t *next = resp_clients->next;
    resp_client_free(resp_clients);
    resp_clients = next;
  }
  resp_db_clear();
  free(acc);
  return 0;
}

/* ============================================================
   Redis inspector builtins — read resp_db from REPL (-R only).
   ============================================================
   Single-reactor invariant: these run on the same thread that owns
   resp_db, so no locking is needed. Outside -R mode, resp_db is
   zero-initialised → keys=nil, count=0, get/type=nil/"none". */

const char doc_redis_count[] = "(redis-count) — number of keys in the running RESP server's db.";
exp_t *rediscountcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  return MAKE_FIX((int64_t)dict_count(resp_db));
}

const char doc_redis_keys[] = "(redis-keys) — list of all keys (as strings) in the RESP db. Skips expired entries.";
exp_t *rediskeyscmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  if (!resp_db) return NIL_EXP;
  exp_t *ret = NIL_EXP, *cur = NULL;
  int64_t now = resp_now_us();
  for (int hi = 0; hi < 2; hi++) {
    kvht_t *h = &resp_db->ht[hi];
    for (unsigned long b = 0; b < h->size; b++) {
      for (keyval_t *kv = h->table[b]; kv; kv = kv->next) {
        if (kv_is_expired(kv, now)) continue;
        const char *ks = (const char *)kv->key;
        exp_t *node = make_node(make_string((char *)ks, (int)strlen(ks)));
        if (cur) cur = cur->next = node;
        else { ret = cur = node; }
      }
    }
  }
  return ret ? ret : NIL_EXP;
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
  const char *ks = isstring(kx) ? (char *)kx->ptr : ((alc_blob_t *)kx->ptr)->bytes;
  keyval_t *kv = resp_db_lookup(ks);
  const char *tn = kv ? resp_type_name(kv->val) : "none";
  exp_t *ret = make_string((char *)tn, (int)strlen(tn));
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
  const char *ks = isstring(kx) ? (char *)kx->ptr : ((alc_blob_t *)kx->ptr)->bytes;
  keyval_t *kv = resp_db_lookup(ks);
  exp_t *ret = NIL_EXP;
  if (kv && isblob(kv->val)) {
    alc_blob_t *bl = (alc_blob_t *)kv->val->ptr;
    ret = make_blob(bl->bytes, bl->len);
  }
  unrefexp(kx); unrefexp(e);
  return ret;
}

const char doc_redis_flush[] = "(redis-flush) — remove every key from the RESP db (FLUSHDB). Returns t.";
exp_t *redisflushcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  resp_db_clear();
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
