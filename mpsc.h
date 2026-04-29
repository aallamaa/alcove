/* mpsc.h — header-only multi-producer / single-consumer queue and a
   thin wake-up shim for cross-thread eventing.
   ------------------------------------------------------------------
   Used by alcove's sharded reactor (docs/multithreading.md, Step 2):
   each shard owns one mpsc_queue_t as its inbox; any number of
   producer threads can enqueue; only the owning shard's consumer
   thread dequeues. Producers wake the consumer through alc_wake_t,
   which is select()/poll()-able alongside the shard's client fds.

   Design: Dmitry Vyukov's intrusive single-consumer / multi-producer
   queue. Lock-free for both producers and the consumer. Producers
   never block each other (single atomic exchange on the head). The
   consumer never blocks producers (it only reads tail and the linked
   nexts). The queue is intrusive — caller embeds an mpsc_node_t at
   the start of their work struct and recovers the outer pointer with
   container_of in their consumer loop. No allocation in the queue
   itself.

   Wake shim:
     Linux  → eventfd(EFD_NONBLOCK|EFD_CLOEXEC). One fd. Multiple
              signals coalesce into a single counter; one read drains.
     macOS  → pipe(O_NONBLOCK|O_CLOEXEC). Two fds. One byte per signal;
              consumer drains until EAGAIN. We swallow EAGAIN on the
              writer side too — if the pipe buffer is already full,
              the consumer will be woken anyway, so dropping the byte
              is fine.

   The shim wakes "edge-triggered enough" semantics: at least one
   signal between drain calls guarantees one drain returns positive.
   That matches what a select()-driven reactor needs. */

#ifndef ALCOVE_MPSC_H
#define ALCOVE_MPSC_H

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/eventfd.h>
#endif

/* ============================================================ Queue */

/* Embed one of these as the FIRST field of any struct you enqueue.
   The consumer recovers the outer pointer via container_of (or, if
   the node is the first field, plain pointer arithmetic). */
typedef struct mpsc_node {
  _Atomic(struct mpsc_node *) next;
} mpsc_node_t;

typedef struct mpsc_queue {
  /* Producers swap in their new node here via atomic_exchange. */
  _Atomic(mpsc_node_t *) head;
  /* Consumer-only — no atomic ops on tail itself; the consumer reads
     tail->next with an acquire load. */
  mpsc_node_t *tail;
  /* Sentinel so head/tail are never NULL. The queue is "empty" iff
     head == tail == &stub and stub.next == NULL. */
  mpsc_node_t stub;
} mpsc_queue_t;

static inline void mpsc_init(mpsc_queue_t *q) {
  atomic_store_explicit(&q->stub.next, NULL, memory_order_relaxed);
  atomic_store_explicit(&q->head, &q->stub, memory_order_relaxed);
  q->tail = &q->stub;
}

/* Producer side. Safe to call from any thread.
   The release-store on prev->next publishes the new node to the
   consumer; pairs with the acquire-load in mpsc_dequeue. */
static inline void mpsc_enqueue(mpsc_queue_t *q, mpsc_node_t *n) {
  atomic_store_explicit(&n->next, NULL, memory_order_relaxed);
  mpsc_node_t *prev =
      atomic_exchange_explicit(&q->head, n, memory_order_acq_rel);
  atomic_store_explicit(&prev->next, n, memory_order_release);
}

/* Consumer side. Must be called from at most one thread at a time.
   Returns NULL if the queue is empty OR if a producer's enqueue is
   in flight (the brief gap between the head-exchange and the
   prev->next-store). The consumer should treat NULL as "try again
   later" — the wake-fd will fire once the producer completes.

   Returns the dequeued node otherwise. The caller owns the node and
   should NOT touch its `next` field after dequeue. */
static inline mpsc_node_t *mpsc_dequeue(mpsc_queue_t *q) {
  mpsc_node_t *tail = q->tail;
  mpsc_node_t *next = atomic_load_explicit(&tail->next, memory_order_acquire);
  if (tail == &q->stub) {
    if (!next) return NULL;
    q->tail = next;
    tail = next;
    next = atomic_load_explicit(&tail->next, memory_order_acquire);
  }
  if (next) {
    q->tail = next;
    return tail;
  }
  /* tail == head (so far we've seen). If head has moved we missed
     a producer mid-flight; bail and let the caller retry. */
  mpsc_node_t *head = atomic_load_explicit(&q->head, memory_order_acquire);
  if (tail != head) return NULL;
  /* Re-enqueue the stub so the queue stays well-formed. */
  atomic_store_explicit(&q->stub.next, NULL, memory_order_relaxed);
  mpsc_node_t *prev =
      atomic_exchange_explicit(&q->head, &q->stub, memory_order_acq_rel);
  atomic_store_explicit(&prev->next, &q->stub, memory_order_release);
  next = atomic_load_explicit(&tail->next, memory_order_acquire);
  if (next) {
    q->tail = next;
    return tail;
  }
  return NULL;
}

/* ============================================================= Wake */

/* Cross-thread wake-up. Linux: one eventfd. macOS: a pipe pair. */
typedef struct alc_wake {
#ifdef __linux__
  int fd;
#else
  int rfd;
  int wfd;
#endif
} alc_wake_t;

/* Returns 0 on success, -1 on error (errno set). */
static inline int alc_wake_init(alc_wake_t *w) {
#ifdef __linux__
  w->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  return w->fd < 0 ? -1 : 0;
#else
  int fds[2];
  if (pipe(fds) < 0) return -1;
  for (int i = 0; i < 2; i++) {
    int fl = fcntl(fds[i], F_GETFL, 0);
    if (fl < 0 || fcntl(fds[i], F_SETFL, fl | O_NONBLOCK) < 0) {
      close(fds[0]); close(fds[1]);
      return -1;
    }
    fl = fcntl(fds[i], F_GETFD, 0);
    if (fl >= 0) fcntl(fds[i], F_SETFD, fl | FD_CLOEXEC);
  }
  w->rfd = fds[0];
  w->wfd = fds[1];
  return 0;
#endif
}

/* Returns the fd a select()/poll() loop should watch for "readable"
   to detect wake-ups. On Linux this is the eventfd; on macOS it's
   the pipe's read end. */
static inline int alc_wake_fd(const alc_wake_t *w) {
#ifdef __linux__
  return w->fd;
#else
  return w->rfd;
#endif
}

/* Producer: signal that there is work to consume. Idempotent — a
   second signal before drain is harmless. */
static inline void alc_wake_signal(alc_wake_t *w) {
#ifdef __linux__
  uint64_t one = 1;
  ssize_t n;
  do { n = write(w->fd, &one, sizeof one); } while (n < 0 && errno == EINTR);
  /* EAGAIN can't happen on eventfd in counter mode unless the counter
     would overflow (2^64-2) — practically never. */
#else
  char b = 1;
  ssize_t n;
  do { n = write(w->wfd, &b, 1); } while (n < 0 && errno == EINTR);
  /* EAGAIN: pipe buffer full → consumer will wake anyway, so drop. */
#endif
}

/* Consumer: drain all pending signals so the fd goes back to
   not-readable. Must be called after observing the wake fd as
   readable, and before re-entering select(). Multiple signals
   coalesce — call this once per wake regardless of producer count. */
static inline void alc_wake_drain(alc_wake_t *w) {
#ifdef __linux__
  uint64_t v;
  ssize_t n;
  do { n = read(w->fd, &v, sizeof v); } while (n < 0 && errno == EINTR);
#else
  char buf[64];
  ssize_t n;
  do {
    n = read(w->rfd, buf, sizeof buf);
  } while (n > 0 || (n < 0 && errno == EINTR));
#endif
}

static inline void alc_wake_destroy(alc_wake_t *w) {
#ifdef __linux__
  if (w->fd >= 0) close(w->fd);
  w->fd = -1;
#else
  if (w->rfd >= 0) close(w->rfd);
  if (w->wfd >= 0) close(w->wfd);
  w->rfd = w->wfd = -1;
#endif
}

#endif /* ALCOVE_MPSC_H */
