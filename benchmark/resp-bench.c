/*
 * resp-bench — randomised-key SET/GET benchmark client for any RESP server.
 *
 * Pre-generates a `keyspace` of fixed-width keys, then drives `ops`
 * SETs and `ops` GETs against a random subset of those keys, split
 * across `clients` parallel pipelined connections. Reports wall-clock
 * rps for SET and GET separately.
 *
 *   Build:  cc -O3 -pthread -o resp-bench resp-bench.c
 *           (or: make -C benchmark resp-bench)
 *
 *   Usage:  ./resp-bench [--host H] [--port P] [--clients C]
 *                        [--pipeline N] [--keyspace K] [--ops O]
 *                        [--value-size V] [--seed S] [--quiet]
 *
 * Why this exists: redis-benchmark by default hammers a single hot key
 * (L1-cache-bound, not realistic). `redis-benchmark -r KEYSPACE` does
 * randomise — but we want a portable self-contained tool that doesn't
 * depend on `redis-benchmark` being installed. Python is too slow
 * (GIL + interp overhead bottlenecks the client at ~1.5 M rps no matter
 * what the server can do); this C version saturates the server.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---------------- config (from argv) ---------------- */
static char g_host[256] = "127.0.0.1";
static int g_port = 6379;
static int g_clients = 50;
static int g_pipeline = 64;
static int g_keyspace = 100000;
static int g_value_size = 16;
static int g_ops = 100000;
static unsigned int g_seed = 0;
static int g_quiet = 0;

/* ---------------- fixed key/cmd layout ---------------- *
 * Keys are "key:NNNNNNNNNN" (4 + 10 = 14 bytes), zero-padded so every
 * SET/GET command has a fixed encoded length. That lets each client
 * thread address its slice of the pre-built command stream by simple
 * pointer arithmetic — no per-cmd offset table.
 */
#define KEY_WIDTH 14

static int per_cmd_set(int value_size) {
  /* "*3\r\n$3\r\nSET\r\n$14\r\nKEY...\r\n$<vs>\r\n<val>\r\n" */
  char tmp[16];
  int vd = snprintf(tmp, sizeof tmp, "%d", value_size);
  return 4 + 9 + 1 + 2 + 2 + KEY_WIDTH + 2 + 1 + vd + 2 + value_size + 2;
}

static int per_cmd_get(void) {
  /* "*2\r\n$3\r\nGET\r\n$14\r\nKEY...\r\n" */
  return 4 + 9 + 1 + 2 + 2 + KEY_WIDTH + 2;
}

/* Format key:<10 digit zero padded> into `out` (14 bytes, no NUL). */
static void make_key(char *out, int idx) {
  /* snprintf would add NUL and shift bytes — write directly. */
  memcpy(out, "key:", 4);
  for (int i = 13; i >= 4; i--) {
    out[i] = '0' + (idx % 10);
    idx /= 10;
  }
}

/* ---------------- RESP reply parser ----------------
 * Just enough to consume +/-/:/$ in a pipelined stream. We don't care
 * about the actual values — only that one reply has fully landed. */
typedef struct {
  char *buf;
  size_t pos, len, cap;
} resp_reader_t;

static void resp_init(resp_reader_t *r) {
  r->cap = 64 * 1024;
  r->buf = malloc(r->cap);
  r->pos = r->len = 0;
}

static void resp_free(resp_reader_t *r) { free(r->buf); }

/* Compact unread bytes to the start of the buffer. */
static void resp_compact(resp_reader_t *r) {
  if (r->pos == 0)
    return;
  size_t live = r->len - r->pos;
  if (live > 0)
    memmove(r->buf, r->buf + r->pos, live);
  r->len = live;
  r->pos = 0;
}

/* Try to consume up to `want` replies. Returns how many we actually
 * consumed (0..want). Advances r->pos. */
static int resp_consume(resp_reader_t *r, int want) {
  int got = 0;
  while (got < want) {
    if (r->pos >= r->len)
      break;
    char t = r->buf[r->pos];
    if (t == '+' || t == '-' || t == ':') {
      /* simple string / error / integer: find next \n */
      char *nl =
          memchr(r->buf + r->pos, '\n', r->len - r->pos);
      if (!nl)
        break;
      r->pos = (size_t)(nl - r->buf) + 1;
      got++;
    } else if (t == '$') {
      /* bulk string: "$N\r\n<N bytes>\r\n" or "$-1\r\n" */
      char *nl =
          memchr(r->buf + r->pos + 1, '\n', r->len - r->pos - 1);
      if (!nl)
        break;
      long n = strtol(r->buf + r->pos + 1, NULL, 10);
      if (n < 0) {
        r->pos = (size_t)(nl - r->buf) + 1;
        got++;
      } else {
        size_t end = (size_t)(nl - r->buf) + 1 + (size_t)n + 2;
        if (end > r->len)
          break;
        r->pos = end;
        got++;
      }
    } else {
      fprintf(stderr, "resp-bench: unsupported RESP type byte %c (0x%02x) "
                      "at pos %zu of %zu\n",
              t, (unsigned char)t, r->pos, r->len);
      exit(1);
    }
  }
  return got;
}

/* ---------------- per-client worker ---------------- */
typedef struct {
  const char *cmds;    /* pointer into shared command stream */
  size_t per_cmd;      /* bytes per command (constant) */
  int n_cmds;          /* total commands this thread owns */
} worker_arg_t;

static int connect_server(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    perror("socket");
    exit(1);
  }
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(g_port);
  if (inet_pton(AF_INET, g_host, &sa.sin_addr) != 1) {
    fprintf(stderr, "resp-bench: bad host %s\n", g_host);
    exit(1);
  }
  if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
    perror("connect");
    exit(1);
  }
  return s;
}

static void *worker(void *arg) {
  worker_arg_t *wa = (worker_arg_t *)arg;
  int sock = connect_server();
  resp_reader_t rd;
  resp_init(&rd);

  int i = 0;
  while (i < wa->n_cmds) {
    int batch = wa->n_cmds - i;
    if (batch > g_pipeline)
      batch = g_pipeline;
    /* send the entire batch */
    const char *p = wa->cmds + (size_t)i * wa->per_cmd;
    size_t want = (size_t)batch * wa->per_cmd;
    while (want > 0) {
      ssize_t s = send(sock, p, want, 0);
      if (s < 0) {
        if (errno == EINTR)
          continue;
        perror("send");
        exit(1);
      }
      p += s;
      want -= (size_t)s;
    }
    /* read back `batch` replies */
    int got = 0;
    while (got < batch) {
      if (rd.len == rd.cap) {
        rd.cap *= 2;
        rd.buf = realloc(rd.buf, rd.cap);
      }
      ssize_t r = recv(sock, rd.buf + rd.len, rd.cap - rd.len, 0);
      if (r < 0) {
        if (errno == EINTR)
          continue;
        perror("recv");
        exit(1);
      }
      if (r == 0) {
        fprintf(stderr, "resp-bench: server closed connection mid-batch\n");
        exit(1);
      }
      rd.len += (size_t)r;
      got += resp_consume(&rd, batch - got);
      resp_compact(&rd);
    }
    i += batch;
  }
  close(sock);
  resp_free(&rd);
  return NULL;
}

/* ---------------- command-stream builders ---------------- */
static char *build_set_stream(int n, const char *value) {
  size_t per = (size_t)per_cmd_set(g_value_size);
  size_t total = per * (size_t)n;
  char *buf = malloc(total);
  if (!buf) {
    fprintf(stderr, "resp-bench: out of memory (%zu bytes)\n", total);
    exit(1);
  }
  char vlen[16];
  int vd = snprintf(vlen, sizeof vlen, "%d", g_value_size);
  unsigned int rs = g_seed ^ 0x9e3779b9u;
  for (int i = 0; i < n; i++) {
    int key_idx = (int)((unsigned int)rand_r(&rs) % (unsigned int)g_keyspace);
    char *p = buf + (size_t)i * per;
    memcpy(p, "*3\r\n$3\r\nSET\r\n$14\r\n", 18);
    p += 18;
    make_key(p, key_idx);
    p += KEY_WIDTH;
    memcpy(p, "\r\n$", 3);
    p += 3;
    memcpy(p, vlen, (size_t)vd);
    p += vd;
    memcpy(p, "\r\n", 2);
    p += 2;
    memcpy(p, value, (size_t)g_value_size);
    p += g_value_size;
    memcpy(p, "\r\n", 2);
  }
  return buf;
}

static char *build_get_stream(int n) {
  size_t per = (size_t)per_cmd_get();
  size_t total = per * (size_t)n;
  char *buf = malloc(total);
  if (!buf) {
    fprintf(stderr, "resp-bench: out of memory (%zu bytes)\n", total);
    exit(1);
  }
  unsigned int rs = g_seed ^ 0xdeadbeefu;
  for (int i = 0; i < n; i++) {
    int key_idx = (int)((unsigned int)rand_r(&rs) % (unsigned int)g_keyspace);
    char *p = buf + (size_t)i * per;
    memcpy(p, "*2\r\n$3\r\nGET\r\n$14\r\n", 18);
    p += 18;
    make_key(p, key_idx);
    p += KEY_WIDTH;
    memcpy(p, "\r\n", 2);
  }
  return buf;
}

/* ---------------- driver ---------------- */
static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double time_phase(const char *stream, size_t per_cmd, int n_cmds) {
  pthread_t *th = malloc((size_t)g_clients * sizeof *th);
  worker_arg_t *wa = malloc((size_t)g_clients * sizeof *wa);
  int per_worker = n_cmds / g_clients;
  int extra = n_cmds - per_worker * g_clients;
  size_t off = 0;
  for (int i = 0; i < g_clients; i++) {
    wa[i].cmds = stream + off * per_cmd;
    wa[i].per_cmd = per_cmd;
    wa[i].n_cmds = per_worker + (i < extra ? 1 : 0);
    off += (size_t)wa[i].n_cmds;
  }
  double t0 = now_sec();
  for (int i = 0; i < g_clients; i++)
    pthread_create(&th[i], NULL, worker, &wa[i]);
  for (int i = 0; i < g_clients; i++)
    pthread_join(th[i], NULL);
  double dt = now_sec() - t0;
  free(th);
  free(wa);
  return dt;
}

static void usage(const char *p) {
  fprintf(stderr,
          "usage: %s [--host H] [--port P] [--clients C] [--pipeline N]\n"
          "          [--keyspace K] [--ops O] [--value-size V]\n"
          "          [--seed S] [--quiet]\n",
          p);
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--host") && i + 1 < argc) {
      snprintf(g_host, sizeof g_host, "%s", argv[++i]);
    } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
      g_port = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--clients") && i + 1 < argc) {
      g_clients = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--pipeline") && i + 1 < argc) {
      g_pipeline = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--keyspace") && i + 1 < argc) {
      g_keyspace = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--ops") && i + 1 < argc) {
      g_ops = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--value-size") && i + 1 < argc) {
      g_value_size = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
      g_seed = (unsigned int)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--quiet")) {
      g_quiet = 1;
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "resp-bench: unknown arg: %s\n", argv[i]);
      usage(argv[0]);
      return 1;
    }
  }
  if (g_keyspace < 1 || g_clients < 1 || g_pipeline < 1 || g_ops < 1 ||
      g_value_size < 0) {
    fprintf(stderr,
            "resp-bench: --keyspace/--clients/--pipeline/--ops must be >= 1\n");
    return 1;
  }

  if (!g_quiet) {
    printf("resp-bench  host=%s  port=%d  clients=%d  pipeline=%d  "
           "keyspace=%d  value=%dB  ops=%d\n",
           g_host, g_port, g_clients, g_pipeline, g_keyspace, g_value_size,
           g_ops);
  }

  /* Pre-build the command streams. */
  char *value = malloc((size_t)g_value_size);
  memset(value, 'x', (size_t)g_value_size);
  size_t per_set = (size_t)per_cmd_set(g_value_size);
  size_t per_get = (size_t)per_cmd_get();
  char *set_stream = build_set_stream(g_ops, value);
  char *get_stream = build_get_stream(g_ops);

  double set_dt = time_phase(set_stream, per_set, g_ops);
  double get_dt = time_phase(get_stream, per_get, g_ops);

  double set_rps = (double)g_ops / set_dt;
  double get_rps = (double)g_ops / get_dt;
  /* p50 proxy: batch round-trip / pipeline. Crude but consistent. */
  double set_p50_ms =
      (set_dt / ((double)g_ops / (double)g_pipeline)) * 1000.0;
  double get_p50_ms =
      (get_dt / ((double)g_ops / (double)g_pipeline)) * 1000.0;

  if (g_quiet) {
    printf("%.0f %.0f %.3f %.3f\n", set_rps, get_rps, set_p50_ms, get_p50_ms);
  } else {
    printf("  SET   %14.0f rps   p50≈%7.3f ms/batch\n", set_rps, set_p50_ms);
    printf("  GET   %14.0f rps   p50≈%7.3f ms/batch\n", get_rps, get_p50_ms);
  }

  free(value);
  free(set_stream);
  free(get_stream);
  return 0;
}
