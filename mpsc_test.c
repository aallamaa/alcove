/* mpsc_test.c — torture test for mpsc.h.
   Build: make mpsc-test
   Or:    cc -O2 -Wall -W -pthread -o mpsc_test mpsc_test.c
   Tsan:  cc -O1 -g -Wall -W -pthread -fsanitize=thread -o mpsc_test mpsc_test.c

   Tests:
     1. Single-threaded: enqueue then dequeue N items in order.
     2. Multi-producer / single-consumer: P producers each enqueue N
        items; consumer drains the expected P*N total. We verify both
        the count and that every (producer_id, sequence) pair appears
        exactly once.
     3. Wake roundtrip: signal in one thread, drain in another after
        select() reports readable. Repeat. Confirms the fd is genuinely
        select()-able and that drain returns it to not-readable. */

#include "mpsc.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

#define N_PRODUCERS 8
#define N_PER_PRODUCER 100000
#define N_TOTAL (N_PRODUCERS * N_PER_PRODUCER)

typedef struct work {
  mpsc_node_t node; /* must be first for the cast in dequeue */
  int producer_id;
  int seq;
} work_t;

/* ======================================================= Test 1 */

static int test_single_threaded(void) {
  mpsc_queue_t q;
  mpsc_init(&q);

  if (mpsc_dequeue(&q) != NULL) {
    fprintf(stderr, "test1: empty queue should return NULL\n");
    return 1;
  }

  enum { N = 1024 };
  work_t *items = calloc(N, sizeof *items);
  for (int i = 0; i < N; i++) {
    items[i].seq = i;
    mpsc_enqueue(&q, &items[i].node);
  }

  for (int i = 0; i < N; i++) {
    mpsc_node_t *n = mpsc_dequeue(&q);
    if (!n) {
      fprintf(stderr, "test1: dequeue returned NULL at i=%d\n", i);
      free(items);
      return 1;
    }
    work_t *w = (work_t *)n;
    if (w->seq != i) {
      fprintf(stderr, "test1: out-of-order at i=%d (got seq=%d)\n", i, w->seq);
      free(items);
      return 1;
    }
  }

  if (mpsc_dequeue(&q) != NULL) {
    fprintf(stderr, "test1: expected empty after drain\n");
    free(items);
    return 1;
  }

  free(items);
  printf("test1 single-threaded: ok (%d items)\n", N);
  return 0;
}

/* ======================================================= Test 2 */

typedef struct producer_arg {
  mpsc_queue_t *q;
  alc_wake_t *wake;
  int producer_id;
  work_t *items; /* pre-allocated [N_PER_PRODUCER] */
} producer_arg_t;

static void *producer_main(void *vp) {
  producer_arg_t *a = vp;
  for (int i = 0; i < N_PER_PRODUCER; i++) {
    a->items[i].producer_id = a->producer_id;
    a->items[i].seq = i;
    mpsc_enqueue(a->q, &a->items[i].node);
    alc_wake_signal(a->wake);
  }
  return NULL;
}

static int test_mpsc(void) {
  mpsc_queue_t q;
  alc_wake_t wake;
  mpsc_init(&q);
  if (alc_wake_init(&wake) < 0) {
    perror("alc_wake_init");
    return 1;
  }

  producer_arg_t args[N_PRODUCERS];
  pthread_t threads[N_PRODUCERS];
  for (int p = 0; p < N_PRODUCERS; p++) {
    args[p].q = &q;
    args[p].wake = &wake;
    args[p].producer_id = p;
    args[p].items = calloc(N_PER_PRODUCER, sizeof(work_t));
    if (!args[p].items) {
      fprintf(stderr, "alloc failed\n");
      return 1;
    }
    pthread_create(&threads[p], NULL, producer_main, &args[p]);
  }

  /* Per-(producer, seq) tally. We expect each pair seen exactly once. */
  unsigned char *seen =
      calloc((size_t)N_PRODUCERS * N_PER_PRODUCER, sizeof *seen);
  if (!seen) {
    fprintf(stderr, "alloc failed\n");
    return 1;
  }

  /* Per-producer monotonicity: within one producer, FIFO must hold
     since the queue is FIFO and the producer enqueues sequentially. */
  int last_seq[N_PRODUCERS];
  for (int p = 0; p < N_PRODUCERS; p++) last_seq[p] = -1;

  int drained = 0;
  while (drained < N_TOTAL) {
    /* Block on the wake fd. Producers signal after every enqueue, so
       at least one of these returns. */
    fd_set rfds;
    FD_ZERO(&rfds);
    int wfd = alc_wake_fd(&wake);
    FD_SET(wfd, &rfds);
    struct timeval tv = {1, 0}; /* 1s safety timeout */
    int sr = select(wfd + 1, &rfds, NULL, NULL, &tv);
    if (sr < 0) {
      perror("select");
      free(seen);
      return 1;
    }
    if (sr == 0) {
      fprintf(stderr,
              "test2: select() timeout with drained=%d/%d (lost wake?)\n",
              drained, N_TOTAL);
      free(seen);
      return 1;
    }
    alc_wake_drain(&wake);

    /* Drain everything currently visible. */
    for (;;) {
      mpsc_node_t *n = mpsc_dequeue(&q);
      if (!n) break;
      work_t *w = (work_t *)n;
      if (w->producer_id < 0 || w->producer_id >= N_PRODUCERS) {
        fprintf(stderr, "test2: bad producer_id=%d\n", w->producer_id);
        free(seen);
        return 1;
      }
      if (w->seq < 0 || w->seq >= N_PER_PRODUCER) {
        fprintf(stderr, "test2: bad seq=%d\n", w->seq);
        free(seen);
        return 1;
      }
      size_t idx = (size_t)w->producer_id * N_PER_PRODUCER + w->seq;
      if (seen[idx]) {
        fprintf(stderr, "test2: duplicate (p=%d, seq=%d)\n",
                w->producer_id, w->seq);
        free(seen);
        return 1;
      }
      seen[idx] = 1;
      if (w->seq <= last_seq[w->producer_id]) {
        fprintf(stderr,
                "test2: out-of-order within producer p=%d (seq=%d <= last=%d)\n",
                w->producer_id, w->seq, last_seq[w->producer_id]);
        free(seen);
        return 1;
      }
      last_seq[w->producer_id] = w->seq;
      drained++;
    }
  }

  for (int p = 0; p < N_PRODUCERS; p++) pthread_join(threads[p], NULL);

  /* Final sanity: all pairs seen. */
  for (size_t i = 0; i < (size_t)N_TOTAL; i++) {
    if (!seen[i]) {
      fprintf(stderr, "test2: missing pair index %zu\n", i);
      free(seen);
      return 1;
    }
  }

  for (int p = 0; p < N_PRODUCERS; p++) free(args[p].items);
  free(seen);
  alc_wake_destroy(&wake);
  printf("test2 MPSC %dp x %d items = %d drained: ok\n", N_PRODUCERS,
         N_PER_PRODUCER, N_TOTAL);
  return 0;
}

/* ======================================================= Test 3 */

static int test_wake_roundtrip(void) {
  alc_wake_t wake;
  if (alc_wake_init(&wake) < 0) {
    perror("alc_wake_init");
    return 1;
  }

  /* Before any signal, fd must NOT be readable. */
  fd_set rfds;
  int wfd = alc_wake_fd(&wake);
  FD_ZERO(&rfds);
  FD_SET(wfd, &rfds);
  struct timeval zero = {0, 0};
  int sr = select(wfd + 1, &rfds, NULL, NULL, &zero);
  if (sr != 0) {
    fprintf(stderr, "test3: fd unexpectedly readable before signal (sr=%d)\n",
            sr);
    alc_wake_destroy(&wake);
    return 1;
  }

  /* Signal three times, expect readable. Drain once, expect not readable. */
  alc_wake_signal(&wake);
  alc_wake_signal(&wake);
  alc_wake_signal(&wake);
  FD_ZERO(&rfds);
  FD_SET(wfd, &rfds);
  zero.tv_sec = zero.tv_usec = 0;
  sr = select(wfd + 1, &rfds, NULL, NULL, &zero);
  if (sr != 1 || !FD_ISSET(wfd, &rfds)) {
    fprintf(stderr, "test3: fd not readable after signal (sr=%d)\n", sr);
    alc_wake_destroy(&wake);
    return 1;
  }

  alc_wake_drain(&wake);
  FD_ZERO(&rfds);
  FD_SET(wfd, &rfds);
  zero.tv_sec = zero.tv_usec = 0;
  sr = select(wfd + 1, &rfds, NULL, NULL, &zero);
  if (sr != 0) {
    fprintf(stderr, "test3: fd still readable after drain (sr=%d)\n", sr);
    alc_wake_destroy(&wake);
    return 1;
  }

  alc_wake_destroy(&wake);
  printf("test3 wake roundtrip: ok\n");
  return 0;
}

/* ============================================================ Main */

int main(void) {
  if (test_single_threaded()) return 1;
  if (test_mpsc()) return 1;
  if (test_wake_roundtrip()) return 1;
  printf("all mpsc tests passed\n");
  return 0;
}
