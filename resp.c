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
#define RESP_MAX_BULK (512 * 1024 * 1024)
#define RESP_MAX_ARGS 1048576
#define RESP_TABLE_INIT 64
#define RESP_HASH_INIT 8

/* ---------- value model ---------- */

enum {
  RESP_T_STRING = 1,
  RESP_T_LIST,
  RESP_T_HASH,
};

/* String: arbitrary bytes, NOT NUL-terminated. `s` is malloc'd, `n`
   is the byte length. A zero-length string has s != NULL (1-byte
   alloc) so the GET path can pass a non-NULL pointer to write_bulk. */
typedef struct {
  char *s;
  size_t n;
} resp_str_t;

/* Doubly-linked list. Each node owns its byte buffer. */
typedef struct resp_listnode {
  char *s;
  size_t n;
  struct resp_listnode *prev, *next;
} resp_listnode_t;

typedef struct {
  resp_listnode_t *head, *tail;
  long len;
} resp_list_t;

/* Per-hash-key table. Field name is NUL-terminated (rejected on
   embedded NUL); value is binary-safe. Chained buckets, doubled at
   load factor 1.0. */
typedef struct resp_field {
  char *key;
  char *val;
  size_t vlen;
  struct resp_field *next;
} resp_field_t;

typedef struct {
  resp_field_t **buckets;
  size_t nbuckets;
  size_t count;
} resp_hash_t;

typedef struct resp_val {
  int type;
  union {
    resp_str_t str;
    resp_list_t list;
    resp_hash_t hash;
  };
} resp_val_t;

/* Top-level table. Same shape as resp_hash_t but val is resp_val_t*
   and we carry per-key TTL. */
typedef struct resp_entry {
  char *key;
  resp_val_t *val;
  int64_t expire_us; /* 0 = no TTL; else absolute deadline */
  struct resp_entry *next;
} resp_entry_t;

typedef struct {
  resp_entry_t **buckets;
  size_t nbuckets;
  size_t count;
} resp_table_t;

/* ---------- per-client + server state ---------- */

typedef struct resp_client {
  int fd;
  char *rbuf;
  size_t rlen, rcap;
  char *wbuf;
  size_t wlen, wcap;
  struct resp_client *next;
} resp_client_t;

static resp_client_t *resp_clients = NULL;
static resp_table_t resp_db; /* zero-initialised; lazily grown */
static volatile sig_atomic_t resp_stop = 0;
/* Set by resp_serve / resp_repl_serve once bind succeeds; read by the
   (redis-port) builtin so REPL code can discover the listening port
   without out-of-band coordination. 0 means "no server running". */
static int resp_active_port = 0;

static void resp_sigint(int sig) {
  (void)sig;
  resp_stop = 1;
}

/* ---------- value lifetime ---------- */

static void resp_free_str(resp_str_t *s) {
  free(s->s);
  s->s = NULL;
  s->n = 0;
}

static void resp_free_list(resp_list_t *l) {
  resp_listnode_t *n = l->head;
  while (n) {
    resp_listnode_t *next = n->next;
    free(n->s);
    free(n);
    n = next;
  }
  l->head = l->tail = NULL;
  l->len = 0;
}

static void resp_free_hash(resp_hash_t *h) {
  for (size_t b = 0; b < h->nbuckets; b++) {
    resp_field_t *f = h->buckets[b];
    while (f) {
      resp_field_t *next = f->next;
      free(f->key);
      free(f->val);
      free(f);
      f = next;
    }
  }
  free(h->buckets);
  h->buckets = NULL;
  h->nbuckets = 0;
  h->count = 0;
}

static void resp_val_free(resp_val_t *v) {
  if (!v) return;
  switch (v->type) {
    case RESP_T_STRING: resp_free_str(&v->str); break;
    case RESP_T_LIST: resp_free_list(&v->list); break;
    case RESP_T_HASH: resp_free_hash(&v->hash); break;
  }
  free(v);
}

static resp_val_t *resp_val_new_str(const char *p, size_t n) {
  resp_val_t *v = calloc(1, sizeof *v);
  v->type = RESP_T_STRING;
  v->str.s = malloc(n + 1); /* +1 so n=0 still gives non-NULL */
  if (n) memcpy(v->str.s, p, n);
  v->str.s[n] = '\0';
  v->str.n = n;
  return v;
}

static resp_val_t *resp_val_new_list(void) {
  resp_val_t *v = calloc(1, sizeof *v);
  v->type = RESP_T_LIST;
  return v;
}

static resp_val_t *resp_val_new_hash(void) {
  resp_val_t *v = calloc(1, sizeof *v);
  v->type = RESP_T_HASH;
  return v;
}

/* ---------- hash table primitives (top-level + per-key hash) ---------- */

/* DJB2-ish hash that walks `len` bytes (key may be binary in theory,
   but resp_dup_key strdups via strcmp paths, so we end up always
   feeding NUL-terminated bytes; len passed explicitly anyway). */
static unsigned long resp_hash_bytes(const char *p, size_t n) {
  unsigned long h = 5381;
  for (size_t i = 0; i < n; i++) h = ((h << 5) + h) ^ (unsigned char)p[i];
  return h;
}

static void resp_table_grow(resp_table_t *t, size_t newcap) {
  resp_entry_t **nb = calloc(newcap, sizeof *nb);
  for (size_t b = 0; b < t->nbuckets; b++) {
    resp_entry_t *e = t->buckets[b];
    while (e) {
      resp_entry_t *next = e->next;
      unsigned long h = resp_hash_bytes(e->key, strlen(e->key));
      size_t idx = h & (newcap - 1);
      e->next = nb[idx];
      nb[idx] = e;
      e = next;
    }
  }
  free(t->buckets);
  t->buckets = nb;
  t->nbuckets = newcap;
}

static resp_entry_t *resp_table_find(resp_table_t *t, const char *key) {
  if (!t->nbuckets) return NULL;
  unsigned long h = resp_hash_bytes(key, strlen(key));
  for (resp_entry_t *e = t->buckets[h & (t->nbuckets - 1)]; e; e = e->next)
    if (strcmp(e->key, key) == 0) return e;
  return NULL;
}

/* Set or replace. `val` ownership transfers in (we free old). Returns
   the entry. */
static resp_entry_t *resp_table_put(resp_table_t *t, const char *key,
                                    resp_val_t *val) {
  if (!t->nbuckets) resp_table_grow(t, RESP_TABLE_INIT);
  unsigned long h = resp_hash_bytes(key, strlen(key));
  size_t idx = h & (t->nbuckets - 1);
  for (resp_entry_t *e = t->buckets[idx]; e; e = e->next) {
    if (strcmp(e->key, key) == 0) {
      resp_val_free(e->val);
      e->val = val;
      e->expire_us = 0; /* SET clears any prior TTL — Redis semantics */
      return e;
    }
  }
  resp_entry_t *e = calloc(1, sizeof *e);
  e->key = strdup(key);
  e->val = val;
  e->next = t->buckets[idx];
  t->buckets[idx] = e;
  t->count++;
  if (t->count >= t->nbuckets) resp_table_grow(t, t->nbuckets * 2);
  return e;
}

/* Returns 1 if removed, 0 if not present. */
static int resp_table_del(resp_table_t *t, const char *key) {
  if (!t->nbuckets) return 0;
  unsigned long h = resp_hash_bytes(key, strlen(key));
  size_t idx = h & (t->nbuckets - 1);
  resp_entry_t **pp = &t->buckets[idx];
  while (*pp) {
    resp_entry_t *e = *pp;
    if (strcmp(e->key, key) == 0) {
      *pp = e->next;
      resp_val_free(e->val);
      free(e->key);
      free(e);
      t->count--;
      return 1;
    }
    pp = &e->next;
  }
  return 0;
}

static void resp_table_clear(resp_table_t *t) {
  for (size_t b = 0; b < t->nbuckets; b++) {
    resp_entry_t *e = t->buckets[b];
    while (e) {
      resp_entry_t *next = e->next;
      resp_val_free(e->val);
      free(e->key);
      free(e);
      e = next;
    }
  }
  free(t->buckets);
  t->buckets = NULL;
  t->nbuckets = 0;
  t->count = 0;
}

/* Per-key hash field operations. */

static void resp_hash_grow(resp_hash_t *h, size_t newcap) {
  resp_field_t **nb = calloc(newcap, sizeof *nb);
  for (size_t b = 0; b < h->nbuckets; b++) {
    resp_field_t *f = h->buckets[b];
    while (f) {
      resp_field_t *next = f->next;
      unsigned long hash = resp_hash_bytes(f->key, strlen(f->key));
      size_t idx = hash & (newcap - 1);
      f->next = nb[idx];
      nb[idx] = f;
      f = next;
    }
  }
  free(h->buckets);
  h->buckets = nb;
  h->nbuckets = newcap;
}

static resp_field_t *resp_hash_find(resp_hash_t *h, const char *key) {
  if (!h->nbuckets) return NULL;
  unsigned long hash = resp_hash_bytes(key, strlen(key));
  for (resp_field_t *f = h->buckets[hash & (h->nbuckets - 1)]; f; f = f->next)
    if (strcmp(f->key, key) == 0) return f;
  return NULL;
}

/* Returns 1 if a brand-new field was created, 0 if updated existing. */
static int resp_hash_set(resp_hash_t *h, const char *key, const char *val,
                         size_t vlen) {
  if (!h->nbuckets) resp_hash_grow(h, RESP_HASH_INIT);
  unsigned long hash = resp_hash_bytes(key, strlen(key));
  size_t idx = hash & (h->nbuckets - 1);
  for (resp_field_t *f = h->buckets[idx]; f; f = f->next) {
    if (strcmp(f->key, key) == 0) {
      free(f->val);
      f->val = malloc(vlen + 1);
      if (vlen) memcpy(f->val, val, vlen);
      f->val[vlen] = '\0';
      f->vlen = vlen;
      return 0;
    }
  }
  resp_field_t *f = calloc(1, sizeof *f);
  f->key = strdup(key);
  f->val = malloc(vlen + 1);
  if (vlen) memcpy(f->val, val, vlen);
  f->val[vlen] = '\0';
  f->vlen = vlen;
  f->next = h->buckets[idx];
  h->buckets[idx] = f;
  h->count++;
  if (h->count >= h->nbuckets) resp_hash_grow(h, h->nbuckets * 2);
  return 1;
}

static int resp_hash_del(resp_hash_t *h, const char *key) {
  if (!h->nbuckets) return 0;
  unsigned long hash = resp_hash_bytes(key, strlen(key));
  size_t idx = hash & (h->nbuckets - 1);
  resp_field_t **pp = &h->buckets[idx];
  while (*pp) {
    resp_field_t *f = *pp;
    if (strcmp(f->key, key) == 0) {
      *pp = f->next;
      free(f->key);
      free(f->val);
      free(f);
      h->count--;
      return 1;
    }
    pp = &f->next;
  }
  return 0;
}

/* ---------- TTL ----------

   Lazy expiry: every key access goes through resp_lookup, which
   evicts on the spot if the deadline has passed. There's no
   background scanner — fine for an MVP, matches Redis's behaviour
   for inactive keys. */

static int64_t resp_now_us(void) { return gettimeusec(); }

static resp_entry_t *resp_lookup(const char *key) {
  resp_entry_t *e = resp_table_find(&resp_db, key);
  if (!e) return NULL;
  if (e->expire_us && e->expire_us <= resp_now_us()) {
    resp_table_del(&resp_db, key);
    return NULL;
  }
  return e;
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
    size_t cap = c->wcap ? c->wcap : 256;
    while (cap < c->wlen + n) cap *= 2;
    c->wbuf = realloc(c->wbuf, cap);
    c->wcap = cap;
  }
  memcpy(c->wbuf + c->wlen, p, n);
  c->wlen += n;
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

static void resp_write_int(resp_client_t *c, long long v) {
  char buf[32];
  int n = snprintf(buf, sizeof buf, ":%lld\r\n", v);
  resp_write(c, buf, n);
}

static void resp_write_bulk(resp_client_t *c, const char *p, size_t n) {
  char hdr[32];
  int hn = snprintf(hdr, sizeof hdr, "$%zu\r\n", n);
  resp_write(c, hdr, hn);
  resp_write(c, p, n);
  resp_write(c, "\r\n", 2);
}

static void resp_write_nil(resp_client_t *c) { resp_write(c, "$-1\r\n", 5); }

static void resp_write_array_hdr(resp_client_t *c, long long n) {
  char hdr[32];
  int hn = snprintf(hdr, sizeof hdr, "*%lld\r\n", n);
  resp_write(c, hdr, hn);
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
  resp_write_int(c, (long long)resp_db.count);
}

static void cmd_flushdb(resp_client_t *c) {
  resp_table_clear(&resp_db);
  resp_write_simple(c, "OK");
}

static void cmd_keys_star(resp_client_t *c, char **argv, long *argl,
                          int argc) {
  ARGN(2);
  if (argl[1] != 1 || argv[1][0] != '*') {
    resp_write_err(c, "ERR alcove RESP server only supports KEYS *");
    return;
  }
  /* Lazy-expire pass first, then count + emit. We could fold into
     one walk but two is simpler and the count needs to be exact for
     the array header. */
  int64_t now = resp_now_us();
  for (size_t b = 0; b < resp_db.nbuckets; b++) {
    resp_entry_t **pp = &resp_db.buckets[b];
    while (*pp) {
      resp_entry_t *e = *pp;
      if (e->expire_us && e->expire_us <= now) {
        *pp = e->next;
        resp_val_free(e->val);
        free(e->key);
        free(e);
        resp_db.count--;
      } else {
        pp = &e->next;
      }
    }
  }
  resp_write_array_hdr(c, (long long)resp_db.count);
  for (size_t b = 0; b < resp_db.nbuckets; b++)
    for (resp_entry_t *e = resp_db.buckets[b]; e; e = e->next)
      resp_write_bulk(c, e->key, strlen(e->key));
}

static const char *resp_type_name(resp_val_t *v) {
  switch (v->type) {
    case RESP_T_STRING: return "string";
    case RESP_T_LIST: return "list";
    case RESP_T_HASH: return "hash";
  }
  return "none";
}

static void cmd_type(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_simple(c, "none");
    return;
  }
  resp_entry_t *e = resp_lookup(k);
  free(k);
  resp_write_simple(c, e ? resp_type_name(e->val) : "none");
}

static void cmd_del(resp_client_t *c, char **argv, long *argl, int argc) {
  ARG_AT_LEAST(2);
  long long deleted = 0;
  for (int a = 1; a < argc; a++) {
    char *k = resp_dup_key(argv[a], argl[a]);
    if (!k) continue;
    /* lookup first to honour lazy expiry (an expired key shouldn't
       count as a successful delete) */
    if (resp_lookup(k)) {
      resp_table_del(&resp_db, k);
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
    if (resp_lookup(k)) present++;
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
  resp_entry_t *e = resp_lookup(k);
  if (!e) {
    free(k);
    resp_write_int(c, 0);
    return;
  }
  /* Negative TTL == immediate delete (Redis semantics). */
  if (delta <= 0) {
    resp_table_del(&resp_db, k);
    free(k);
    resp_write_int(c, 1);
    return;
  }
  e->expire_us = resp_now_us() + delta * (millis ? 1000LL : 1000000LL);
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
  resp_entry_t *e = resp_lookup(k);
  if (!e) {
    free(k);
    resp_write_int(c, -2);
    return;
  }
  if (!e->expire_us) {
    free(k);
    resp_write_int(c, -1);
    return;
  }
  int64_t left = e->expire_us - resp_now_us();
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
  resp_entry_t *e = resp_lookup(k);
  if (!e || !e->expire_us) {
    free(k);
    resp_write_int(c, 0);
    return;
  }
  e->expire_us = 0;
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
  resp_entry_t *exist = resp_lookup(k);
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
  resp_val_t *v = resp_val_new_str(argv[2], (size_t)argl[2]);
  resp_entry_t *e = resp_table_put(&resp_db, k, v);
  if (expire_us) e->expire_us = resp_now_us() + expire_us;
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
  resp_entry_t *e = resp_lookup(k);
  free(k);
  if (!e) {
    resp_write_nil(c);
    return;
  }
  if (e->val->type != RESP_T_STRING) {
    resp_write_wrongtype(c);
    return;
  }
  resp_write_bulk(c, e->val->str.s, e->val->str.n);
}

static void cmd_strlen(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, 0);
    return;
  }
  resp_entry_t *e = resp_lookup(k);
  free(k);
  if (!e) {
    resp_write_int(c, 0);
    return;
  }
  if (e->val->type != RESP_T_STRING) {
    resp_write_wrongtype(c);
    return;
  }
  resp_write_int(c, (long long)e->val->str.n);
}

/* Shared core for INCR/DECR/INCRBY/DECRBY. Caller owns `key` (still
   valid on return). Creates the entry as "0" if missing, then mutates
   in place. Preserves TTL across the resp_table_put. */
static void resp_apply_incr(resp_client_t *c, const char *key,
                            long long delta) {
  resp_entry_t *e = resp_lookup(key);
  long long cur = 0;
  int64_t saved_expire = 0;
  if (e) {
    if (e->val->type != RESP_T_STRING) {
      resp_write_wrongtype(c);
      return;
    }
    if (e->val->str.n == 0 || e->val->str.n > 30) {
      resp_write_err(c, "ERR value is not an integer or out of range");
      return;
    }
    char buf[32];
    memcpy(buf, e->val->str.s, e->val->str.n);
    buf[e->val->str.n] = '\0';
    char *end;
    errno = 0;
    cur = strtoll(buf, &end, 10);
    if (*end != '\0' || errno) {
      resp_write_err(c, "ERR value is not an integer or out of range");
      return;
    }
    /* Snapshot TTL before resp_table_put — when the entry already exists,
       it returns the same node and zeroes its expire_us in the process,
       so reading it after the put would always give 0. */
    saved_expire = e->expire_us;
  }
  /* Signed-overflow guard before the addition itself wraps. */
  if ((delta > 0 && cur > LLONG_MAX - delta) ||
      (delta < 0 && cur < LLONG_MIN - delta)) {
    resp_write_err(c, "ERR increment or decrement would overflow");
    return;
  }
  cur += delta;
  char buf[32];
  int n = snprintf(buf, sizeof buf, "%lld", cur);
  resp_val_t *v = resp_val_new_str(buf, (size_t)n);
  resp_entry_t *ne = resp_table_put(&resp_db, key, v);
  ne->expire_us = saved_expire;
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

/* APPEND key value — appends bytes to an existing string, or creates
   the key holding `value` if missing. Returns the new length.
   In-place realloc preserves TTL (no resp_table_put on the hot path). */
static void cmd_append(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(3);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_err(c, "ERR keys with embedded NUL not supported");
    return;
  }
  resp_entry_t *e = resp_lookup(k);
  if (!e) {
    resp_val_t *v = resp_val_new_str(argv[2], (size_t)argl[2]);
    resp_table_put(&resp_db, k, v);
    free(k);
    resp_write_int(c, (long long)argl[2]);
    return;
  }
  if (e->val->type != RESP_T_STRING) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  size_t old_n = e->val->str.n;
  size_t add = (size_t)argl[2];
  size_t new_n = old_n + add;
  char *ns = realloc(e->val->str.s, new_n + 1);
  if (!ns) {
    free(k);
    resp_write_err(c, "ERR out of memory");
    return;
  }
  if (add) memcpy(ns + old_n, argv[2], add);
  ns[new_n] = '\0';
  e->val->str.s = ns;
  e->val->str.n = new_n;
  free(k);
  resp_write_int(c, (long long)new_n);
}

/* ---------- list commands ---------- */

/* Returns the entry's list, creating one if missing. NULL means
   wrongtype (existing key is not a list). */
static resp_list_t *resp_get_or_create_list(const char *key, int create) {
  resp_entry_t *e = resp_lookup(key);
  if (e) {
    if (e->val->type != RESP_T_LIST) return NULL;
    return &e->val->list;
  }
  if (!create) return NULL;
  resp_val_t *v = resp_val_new_list();
  e = resp_table_put(&resp_db, key, v);
  return &e->val->list;
}

static resp_listnode_t *resp_listnode_new(const char *p, size_t n) {
  resp_listnode_t *nd = calloc(1, sizeof *nd);
  nd->s = malloc(n + 1);
  if (n) memcpy(nd->s, p, n);
  nd->s[n] = '\0';
  nd->n = n;
  return nd;
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
  resp_entry_t *e = resp_lookup(k);
  if (e && e->val->type != RESP_T_LIST) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  resp_list_t *l = resp_get_or_create_list(k, 1);
  for (int i = 2; i < argc; i++) {
    resp_listnode_t *nd = resp_listnode_new(argv[i], (size_t)argl[i]);
    if (left) {
      nd->next = l->head;
      if (l->head) l->head->prev = nd;
      else l->tail = nd;
      l->head = nd;
    } else {
      nd->prev = l->tail;
      if (l->tail) l->tail->next = nd;
      else l->head = nd;
      l->tail = nd;
    }
    l->len++;
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
  resp_entry_t *e = resp_lookup(k);
  if (!e) {
    free(k);
    resp_write_nil(c);
    return;
  }
  if (e->val->type != RESP_T_LIST) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  resp_list_t *l = &e->val->list;
  if (l->len == 0) {
    free(k);
    resp_write_nil(c);
    return;
  }
  resp_listnode_t *nd = left ? l->head : l->tail;
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
  resp_write_bulk(c, nd->s, nd->n);
  free(nd->s);
  free(nd);
  /* Empty list is deleted (Redis semantics — keys never hold
     empty containers). */
  if (l->len == 0) resp_table_del(&resp_db, k);
  free(k);
}

static void cmd_llen(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, 0);
    return;
  }
  resp_entry_t *e = resp_lookup(k);
  free(k);
  if (!e) {
    resp_write_int(c, 0);
    return;
  }
  if (e->val->type != RESP_T_LIST) {
    resp_write_wrongtype(c);
    return;
  }
  resp_write_int(c, (long long)e->val->list.len);
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
  resp_entry_t *e = resp_lookup(k);
  free(k);
  if (!e) {
    resp_write_nil(c);
    return;
  }
  if (e->val->type != RESP_T_LIST) {
    resp_write_wrongtype(c);
    return;
  }
  resp_list_t *l = &e->val->list;
  long ni = resp_norm_index((long)idx, l->len);
  if (ni < 0) {
    resp_write_nil(c);
    return;
  }
  /* Walk from whichever end is closer. */
  resp_listnode_t *nd;
  if (ni < l->len / 2) {
    nd = l->head;
    for (long i = 0; i < ni; i++) nd = nd->next;
  } else {
    nd = l->tail;
    for (long i = l->len - 1; i > ni; i--) nd = nd->prev;
  }
  resp_write_bulk(c, nd->s, nd->n);
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
  resp_entry_t *e = resp_lookup(k);
  free(k);
  if (!e) {
    resp_write_array_hdr(c, 0);
    return;
  }
  if (e->val->type != RESP_T_LIST) {
    resp_write_wrongtype(c);
    return;
  }
  long len = e->val->list.len;
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
  resp_listnode_t *nd = e->val->list.head;
  for (long i = 0; i < start; i++) nd = nd->next;
  for (long i = start; i <= stop; i++, nd = nd->next)
    resp_write_bulk(c, nd->s, nd->n);
}

/* ---------- hash commands ---------- */

static resp_hash_t *resp_get_or_create_hash(const char *key, int create) {
  resp_entry_t *e = resp_lookup(key);
  if (e) {
    if (e->val->type != RESP_T_HASH) return NULL;
    return &e->val->hash;
  }
  if (!create) return NULL;
  resp_val_t *v = resp_val_new_hash();
  e = resp_table_put(&resp_db, key, v);
  return &e->val->hash;
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
  resp_entry_t *e = resp_lookup(k);
  if (e && e->val->type != RESP_T_HASH) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  resp_hash_t *h = resp_get_or_create_hash(k, 1);
  long long created = 0;
  for (int i = 2; i + 1 < argc; i += 2) {
    char *fk = resp_dup_key(argv[i], argl[i]);
    if (!fk) continue; /* silently skip NUL field names */
    created += resp_hash_set(h, fk, argv[i + 1], (size_t)argl[i + 1]);
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
  resp_entry_t *e = resp_lookup(k);
  if (!e) {
    free(k);
    resp_write_nil(c);
    return;
  }
  if (e->val->type != RESP_T_HASH) {
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
  resp_field_t *f = resp_hash_find(&e->val->hash, fk);
  free(k);
  free(fk);
  if (!f) {
    resp_write_nil(c);
    return;
  }
  resp_write_bulk(c, f->val, f->vlen);
}

static void cmd_hdel(resp_client_t *c, char **argv, long *argl, int argc) {
  ARG_AT_LEAST(3);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, 0);
    return;
  }
  resp_entry_t *e = resp_lookup(k);
  if (!e) {
    free(k);
    resp_write_int(c, 0);
    return;
  }
  if (e->val->type != RESP_T_HASH) {
    free(k);
    resp_write_wrongtype(c);
    return;
  }
  long long deleted = 0;
  for (int i = 2; i < argc; i++) {
    char *fk = resp_dup_key(argv[i], argl[i]);
    if (!fk) continue;
    deleted += resp_hash_del(&e->val->hash, fk);
    free(fk);
  }
  /* Drop the key if the hash became empty (Redis container rule). */
  if (e->val->hash.count == 0) resp_table_del(&resp_db, k);
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
  resp_entry_t *e = resp_lookup(k);
  if (!e) {
    free(k);
    resp_write_int(c, 0);
    return;
  }
  if (e->val->type != RESP_T_HASH) {
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
  resp_field_t *f = resp_hash_find(&e->val->hash, fk);
  free(k);
  free(fk);
  resp_write_int(c, f ? 1 : 0);
}

static void cmd_hlen(resp_client_t *c, char **argv, long *argl, int argc) {
  ARGN(2);
  char *k = resp_dup_key(argv[1], argl[1]);
  if (!k) {
    resp_write_int(c, 0);
    return;
  }
  resp_entry_t *e = resp_lookup(k);
  free(k);
  if (!e) {
    resp_write_int(c, 0);
    return;
  }
  if (e->val->type != RESP_T_HASH) {
    resp_write_wrongtype(c);
    return;
  }
  resp_write_int(c, (long long)e->val->hash.count);
}

/* Shared walker for HKEYS / HVALS / HGETALL — emit keys, vals, or
   alternating pairs. */
static void hash_emit(resp_client_t *c, resp_hash_t *h, int keys, int vals) {
  long long n = (long long)h->count * ((keys && vals) ? 2 : 1);
  resp_write_array_hdr(c, n);
  for (size_t b = 0; b < h->nbuckets; b++) {
    for (resp_field_t *f = h->buckets[b]; f; f = f->next) {
      if (keys) resp_write_bulk(c, f->key, strlen(f->key));
      if (vals) resp_write_bulk(c, f->val, f->vlen);
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
  resp_entry_t *e = resp_lookup(k);
  free(k);
  if (!e) {
    resp_write_array_hdr(c, 0);
    return;
  }
  if (e->val->type != RESP_T_HASH) {
    resp_write_wrongtype(c);
    return;
  }
  hash_emit(c, &e->val->hash, keys, vals);
}

/* ---------- top-level dispatch ---------- */

static void resp_dispatch(resp_client_t *c, char **argv, long *argl, int argc) {
  if (argc < 1) return;
  const char *cmd = argv[0];
  long clen = argl[0];

  if (resp_cmd_eq(cmd, clen, "PING")) return cmd_ping(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "ECHO")) return cmd_echo(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "QUIT")) return cmd_quit(c);
  if (resp_cmd_eq(cmd, clen, "COMMAND")) {
    /* redis-cli sends `COMMAND DOCS` on connect to build help. An
       empty array satisfies it. */
    resp_write_array_hdr(c, 0);
    return;
  }
  if (resp_cmd_eq(cmd, clen, "SELECT")) return cmd_select(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "DBSIZE")) return cmd_dbsize(c);
  if (resp_cmd_eq(cmd, clen, "FLUSHDB") ||
      resp_cmd_eq(cmd, clen, "FLUSHALL"))
    return cmd_flushdb(c);
  if (resp_cmd_eq(cmd, clen, "KEYS")) return cmd_keys_star(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "TYPE")) return cmd_type(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "DEL") || resp_cmd_eq(cmd, clen, "UNLINK"))
    return cmd_del(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "EXISTS")) return cmd_exists(c, argv, argl, argc);

  if (resp_cmd_eq(cmd, clen, "EXPIRE"))
    return cmd_expire(c, argv, argl, argc, 0);
  if (resp_cmd_eq(cmd, clen, "PEXPIRE"))
    return cmd_expire(c, argv, argl, argc, 1);
  if (resp_cmd_eq(cmd, clen, "TTL")) return cmd_ttl(c, argv, argl, argc, 0);
  if (resp_cmd_eq(cmd, clen, "PTTL")) return cmd_ttl(c, argv, argl, argc, 1);
  if (resp_cmd_eq(cmd, clen, "PERSIST"))
    return cmd_persist(c, argv, argl, argc);

  if (resp_cmd_eq(cmd, clen, "GET")) return cmd_get(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "SET")) return cmd_set(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "STRLEN")) return cmd_strlen(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "INCR"))
    return cmd_incr_decr(c, argv, argl, argc, +1);
  if (resp_cmd_eq(cmd, clen, "DECR"))
    return cmd_incr_decr(c, argv, argl, argc, -1);
  if (resp_cmd_eq(cmd, clen, "INCRBY"))
    return cmd_incrby_decrby(c, argv, argl, argc, +1);
  if (resp_cmd_eq(cmd, clen, "DECRBY"))
    return cmd_incrby_decrby(c, argv, argl, argc, -1);
  if (resp_cmd_eq(cmd, clen, "APPEND"))
    return cmd_append(c, argv, argl, argc);

  if (resp_cmd_eq(cmd, clen, "LPUSH"))
    return cmd_lpush_rpush(c, argv, argl, argc, 1);
  if (resp_cmd_eq(cmd, clen, "RPUSH"))
    return cmd_lpush_rpush(c, argv, argl, argc, 0);
  if (resp_cmd_eq(cmd, clen, "LPOP"))
    return cmd_lpop_rpop(c, argv, argl, argc, 1);
  if (resp_cmd_eq(cmd, clen, "RPOP"))
    return cmd_lpop_rpop(c, argv, argl, argc, 0);
  if (resp_cmd_eq(cmd, clen, "LLEN")) return cmd_llen(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "LINDEX")) return cmd_lindex(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "LRANGE")) return cmd_lrange(c, argv, argl, argc);

  if (resp_cmd_eq(cmd, clen, "HSET")) return cmd_hset(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "HGET")) return cmd_hget(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "HDEL")) return cmd_hdel(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "HEXISTS"))
    return cmd_hexists(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "HLEN")) return cmd_hlen(c, argv, argl, argc);
  if (resp_cmd_eq(cmd, clen, "HKEYS"))
    return cmd_hkeys_hvals_hgetall(c, argv, argl, argc, 1, 0);
  if (resp_cmd_eq(cmd, clen, "HVALS"))
    return cmd_hkeys_hvals_hgetall(c, argv, argl, argc, 0, 1);
  if (resp_cmd_eq(cmd, clen, "HGETALL"))
    return cmd_hkeys_hvals_hgetall(c, argv, argl, argc, 1, 1);

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
  if (listen(srv, 64) < 0) {
    perror("listen");
    close(srv);
    return 1;
  }
  resp_set_nonblock(srv);
  resp_active_port = port;

  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, resp_sigint);
  signal(SIGTERM, resp_sigint);

  printf("alcove RESP2 server listening on 127.0.0.1:%d\n", port);
  fflush(stdout);

  while (!resp_stop) {
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int maxfd = srv;
    FD_SET(srv, &rfds);
    for (resp_client_t *cl = resp_clients; cl; cl = cl->next) {
      FD_SET(cl->fd, &rfds);
      if (cl->wlen > 0) FD_SET(cl->fd, &wfds);
      if (cl->fd > maxfd) maxfd = cl->fd;
    }
    struct timeval tv = {1, 0};
    int r = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
    if (r < 0) {
      if (errno == EINTR) continue;
      perror("select");
      break;
    }

    if (FD_ISSET(srv, &rfds)) {
      struct sockaddr_in peer;
      socklen_t plen = sizeof peer;
      int cfd = accept(srv, (struct sockaddr *)&peer, &plen);
      if (cfd >= 0) {
        resp_set_nonblock(cfd);
        resp_client_t *cl = calloc(1, sizeof *cl);
        cl->fd = cfd;
        cl->rcap = RESP_RBUF_INIT;
        cl->rbuf = malloc(cl->rcap);
        cl->next = resp_clients;
        resp_clients = cl;
      }
    }

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

      if (!drop && FD_ISSET(cur->fd, &wfds) && cur->wlen > 0) {
        ssize_t n = write(cur->fd, cur->wbuf, cur->wlen);
        if (n < 0) {
          if (errno != EAGAIN && errno != EWOULDBLOCK) drop = 1;
        } else if ((size_t)n == cur->wlen) {
          cur->wlen = 0;
        } else {
          memmove(cur->wbuf, cur->wbuf + n, cur->wlen - (size_t)n);
          cur->wlen -= (size_t)n;
        }
      }

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
  resp_table_clear(&resp_db);
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
extern int verbose;

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
  if (listen(srv, 64) < 0) {
    perror("listen");
    close(srv);
    return 1;
  }
  resp_set_nonblock(srv);
  resp_set_nonblock(0); /* stdin */
  resp_active_port = port;

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
    if (0 > maxfd) maxfd = 0;
    for (resp_client_t *cl = resp_clients; cl; cl = cl->next) {
      FD_SET(cl->fd, &rfds);
      if (cl->wlen > 0) FD_SET(cl->fd, &wfds);
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
    if (FD_ISSET(srv, &rfds)) {
      struct sockaddr_in peer;
      socklen_t plen = sizeof peer;
      int cfd = accept(srv, (struct sockaddr *)&peer, &plen);
      if (cfd >= 0) {
        resp_set_nonblock(cfd);
        resp_client_t *cl = calloc(1, sizeof *cl);
        cl->fd = cfd;
        cl->rcap = RESP_RBUF_INIT;
        cl->rbuf = malloc(cl->rcap);
        cl->next = resp_clients;
        resp_clients = cl;
      }
    }

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

      if (!drop && FD_ISSET(cur->fd, &wfds) && cur->wlen > 0) {
        ssize_t n = write(cur->fd, cur->wbuf, cur->wlen);
        if (n < 0) {
          if (errno != EAGAIN && errno != EWOULDBLOCK) drop = 1;
        } else if ((size_t)n == cur->wlen) {
          cur->wlen = 0;
        } else {
          memmove(cur->wbuf, cur->wbuf + n, cur->wlen - (size_t)n);
          cur->wlen -= (size_t)n;
        }
      }

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
  resp_table_clear(&resp_db);
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
  return MAKE_FIX((int64_t)resp_db.count);
}

const char doc_redis_keys[] = "(redis-keys) — list of all keys (as strings) in the RESP db. Skips expired entries.";
exp_t *rediskeyscmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  exp_t *ret = NIL_EXP, *cur = NULL;
  int64_t now = resp_now_us();
  for (size_t b = 0; b < resp_db.nbuckets; b++) {
    for (resp_entry_t *ent = resp_db.buckets[b]; ent; ent = ent->next) {
      if (ent->expire_us && ent->expire_us <= now) continue;
      exp_t *node = make_node(make_string(ent->key, (int)strlen(ent->key)));
      if (cur) cur = cur->next = node;
      else { ret = cur = node; }
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
  resp_entry_t *ent = resp_lookup(ks);
  const char *tn = ent ? resp_type_name(ent->val) : "none";
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
  resp_entry_t *ent = resp_lookup(ks);
  exp_t *ret = NIL_EXP;
  if (ent && ent->val->type == RESP_T_STRING) {
    ret = make_blob(ent->val->str.s, ent->val->str.n);
  }
  unrefexp(kx); unrefexp(e);
  return ret;
}

const char doc_redis_flush[] = "(redis-flush) — remove every key from the RESP db (FLUSHDB). Returns t.";
exp_t *redisflushcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  resp_table_clear(&resp_db);
  return TRUE_EXP;
}

const char doc_redis_port[] = "(redis-port) — port the RESP server is bound to, or 0 if not running.";
exp_t *redisportcmd(exp_t *e, env_t *env) {
  (void)env;
  unrefexp(e);
  return MAKE_FIX((int64_t)resp_active_port);
}
