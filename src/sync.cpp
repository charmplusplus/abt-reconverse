/* Synchronization objects. Each fits the 64-byte ABT_*_memory layouts with
 * all-zero as the initialized state (the recursive-mutex static initializer
 * sets the first int to 1, so 'attrs' comes first, as in Argobots). Waiters
 * live on the waiter's stack; a ULT waiter blocks through CthSuspendBlocked,
 * releasing the object's spinlock only once it is BLOCKED, so a waker that
 * finds it on the list can always wake it with CthAwakenIfBlocked. External
 * (non-ULT) waiters spin on their record. */
#include "abti.h"
#include <cstring>
#include <sched.h>
#include <pthread.h>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

struct ABTI_waiter {
  CthThread thread;              /* NULL for an external thread */
  std::atomic<int> signaled;
  ABTI_waiter *next;
};

static inline void spin_acquire(std::atomic<int> &l) {
  while (l.exchange(1, std::memory_order_acquire)) { while (l.load(std::memory_order_relaxed)) sched_yield(); }
}
static inline void spin_release(std::atomic<int> &l) { l.store(0, std::memory_order_release); }
static void spin_release_cb(void *l) { spin_release(*static_cast<std::atomic<int> *>(l)); }

struct ABTI_waitlist { ABTI_waiter *head; ABTI_waiter *tail; };
static void wl_push(ABTI_waitlist &wl, ABTI_waiter *w) {
  w->next = nullptr;
  if (wl.tail) wl.tail->next = w; else wl.head = w;
  wl.tail = w;
}
static ABTI_waiter *wl_pop(ABTI_waitlist &wl) {
  ABTI_waiter *w = wl.head;
  if (!w) return nullptr;
  wl.head = w->next;
  if (!wl.head) wl.tail = nullptr;
  return w;
}
static bool wl_remove(ABTI_waitlist &wl, ABTI_waiter *w) {
  ABTI_waiter *prev = nullptr;
  for (ABTI_waiter *c = wl.head; c; prev = c, c = c->next) {
    if (c != w) continue;
    if (prev) prev->next = c->next; else wl.head = c->next;
    if (wl.tail == c) wl.tail = prev;
    return true;
  }
  return false;
}
/* wake one waiter popped from a list; touches w only before the waiter can run */
static void wake(ABTI_waiter *w) {
  CthThread t = w->thread;
  w->signaled.store(1, std::memory_order_release);
  if (t) CthAwakenIfBlocked(t);
}
/* block the caller as a waiter already on a list guarded by 'lock' (held) */
static void wait_on(ABTI_waiter &w, std::atomic<int> &lock, ABTI_thread *self) {
  if (ABTI_can_block(self)) {
    ABTI_thread_block(self, spin_release_cb, &lock); /* BLOCKED, then unlock */
  } else {
    spin_release(lock);
    while (!w.signaled.load(std::memory_order_acquire)) sched_yield();
  }
}
/* yield-poll until signaled or the deadline; on timeout remove w under lock.
 * Returns true if signaled. (Argobots' ULT timed waits also yield-poll.) */
static bool wait_on_timed(ABTI_waiter &w, std::atomic<int> &lock, ABTI_waitlist &wl, ABTI_thread *self, double deadline) {
  spin_release(lock);
  for (;;) {
    if (w.signaled.load(std::memory_order_acquire)) return true;
    if (CmiWallTimer() >= deadline) {
      spin_acquire(lock);
      if (w.signaled.load(std::memory_order_acquire)) { spin_release(lock); return true; }
      wl_remove(wl, &w);
      spin_release(lock);
      return false;
    }
    if (ABTI_can_block(self)) CthYield(); else sched_yield();
  }
}

/* defined here, outside extern "C": gcc rejects a static function declared
 * with C++ linkage and defined inside a C linkage block */
static double realtime_to_wall(const struct timespec *abstime) {
  struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
  double rel = (double)(abstime->tv_sec - now.tv_sec) + (double)(abstime->tv_nsec - now.tv_nsec) * 1e-9;
  return CmiWallTimer() + rel;
}
/* ---- mutex ---- */
struct ABTI_mutex {
  int attrs;                    /* bit 0: recursive (ABT_RECURSIVE_MUTEX_INITIALIZER) */
  std::atomic<int> slock;       /* guards everything below */
  std::atomic<int> locked;
  int nesting;
  uint64_t owner;               /* ABTI_thread id or (uint64_t)-1 for an external thread */
  ABTI_waitlist wl;
};
static_assert(sizeof(ABTI_mutex) <= sizeof(ABT_mutex_memory), "mutex layout");
struct ABTI_mutex_attr { int recursive; };
static inline ABTI_mutex *M(ABT_mutex h) { return ABTI_obj<ABTI_mutex>(h); }
/* ULTs use their ABT id; an external pthread its pthread_self with the top
 * bit set, so recursive locking works for it too */
static inline uint64_t self_id(ABTI_thread *self) {
  return self ? self->id : (((uint64_t)(uintptr_t)pthread_self()) | (1ULL << 63));
}

static int mutex_lock(ABTI_mutex *m, bool spin_only) {
  ABTI_thread *self = ABTI_self_thread();
  uint64_t me = self_id(self);
  for (;;) {
    spin_acquire(m->slock);
    if (!m->locked.load(std::memory_order_relaxed)) {
      m->locked.store(1, std::memory_order_relaxed); m->owner = me; m->nesting = 0;
      spin_release(m->slock); return ABT_SUCCESS;
    }
    if ((m->attrs & 1) && m->owner == me) { m->nesting++; spin_release(m->slock); return ABT_SUCCESS; }
    if (spin_only) { spin_release(m->slock); sched_yield(); continue; }
    ABTI_waiter w{ABTI_can_block(self) ? self->cth : nullptr, {0}, nullptr};
    wl_push(m->wl, &w);
    wait_on(w, m->slock, self);
    /* woken: retry (another thread may have taken the lock meanwhile) */
  }
}

extern "C" {

int ABT_mutex_create(ABT_mutex *newmutex) {
  ABTI_mutex *m = new ABTI_mutex(); memset((void *)m, 0, sizeof *m);
  *newmutex = reinterpret_cast<ABT_mutex>(m); return ABT_SUCCESS;
}
int ABT_mutex_create_with_attr(ABT_mutex_attr attr, ABT_mutex *newmutex) {
  int r = ABT_mutex_create(newmutex); if (r != ABT_SUCCESS) return r;
  ABTI_mutex_attr *a = ABTI_obj<ABTI_mutex_attr>(attr);
  if (a && a->recursive) M(*newmutex)->attrs |= 1;
  return ABT_SUCCESS;
}
int ABT_mutex_free(ABT_mutex *mutex) {
  ABTI_mutex *m = M(*mutex); ABTI_CHECK_NULL(m, ABT_ERR_INV_MUTEX);
  delete m; *mutex = ABT_MUTEX_NULL; return ABT_SUCCESS;
}
int ABT_mutex_lock(ABT_mutex mutex) { ABTI_mutex *m = M(mutex); ABTI_CHECK_NULL(m, ABT_ERR_INV_MUTEX); return mutex_lock(m, false); }
int ABT_mutex_lock_high(ABT_mutex mutex) { return ABT_mutex_lock(mutex); }
int ABT_mutex_lock_low(ABT_mutex mutex) { return ABT_mutex_lock(mutex); }
int ABT_mutex_spinlock(ABT_mutex mutex) { ABTI_mutex *m = M(mutex); ABTI_CHECK_NULL(m, ABT_ERR_INV_MUTEX); return mutex_lock(m, true); }
int ABT_mutex_trylock(ABT_mutex mutex) {
  ABTI_mutex *m = M(mutex); ABTI_CHECK_NULL(m, ABT_ERR_INV_MUTEX);
  uint64_t me = self_id(ABTI_self_thread());
  spin_acquire(m->slock);
  if (!m->locked.load(std::memory_order_relaxed)) { m->locked.store(1); m->owner = me; m->nesting = 0; spin_release(m->slock); return ABT_SUCCESS; }
  if ((m->attrs & 1) && m->owner == me) { m->nesting++; spin_release(m->slock); return ABT_SUCCESS; }
  spin_release(m->slock); return ABT_ERR_MUTEX_LOCKED;
}
int ABT_mutex_unlock(ABT_mutex mutex) {
  ABTI_mutex *m = M(mutex); ABTI_CHECK_NULL(m, ABT_ERR_INV_MUTEX);
  spin_acquire(m->slock);
  if (m->nesting > 0) { m->nesting--; spin_release(m->slock); return ABT_SUCCESS; }
  m->locked.store(0, std::memory_order_relaxed); m->owner = 0;
  ABTI_waiter *w = wl_pop(m->wl);
  spin_release(m->slock);
  if (w) wake(w);
  return ABT_SUCCESS;
}
int ABT_mutex_unlock_se(ABT_mutex mutex) { return ABT_mutex_unlock(mutex); }
int ABT_mutex_unlock_de(ABT_mutex mutex) { return ABT_mutex_unlock(mutex); }
int ABT_mutex_equal(ABT_mutex mutex1, ABT_mutex mutex2, ABT_bool *result) { *result = mutex1 == mutex2 ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS; }
int ABT_mutex_get_attr(ABT_mutex mutex, ABT_mutex_attr *attr) {
  ABTI_mutex *m = M(mutex); ABTI_CHECK_NULL(m, ABT_ERR_INV_MUTEX);
  ABTI_mutex_attr *a = new ABTI_mutex_attr{m->attrs & 1};
  *attr = reinterpret_cast<ABT_mutex_attr>(a); return ABT_SUCCESS;
}
int ABT_mutex_attr_create(ABT_mutex_attr *newattr) { *newattr = reinterpret_cast<ABT_mutex_attr>(new ABTI_mutex_attr{0}); return ABT_SUCCESS; }
int ABT_mutex_attr_free(ABT_mutex_attr *attr) {
  ABTI_mutex_attr *a = ABTI_obj<ABTI_mutex_attr>(*attr); ABTI_CHECK_NULL(a, ABT_ERR_INV_MUTEX_ATTR);
  delete a; *attr = ABT_MUTEX_ATTR_NULL; return ABT_SUCCESS;
}
int ABT_mutex_attr_set_recursive(ABT_mutex_attr attr, ABT_bool recursive) {
  ABTI_mutex_attr *a = ABTI_obj<ABTI_mutex_attr>(attr); ABTI_CHECK_NULL(a, ABT_ERR_INV_MUTEX_ATTR);
  a->recursive = recursive ? 1 : 0; return ABT_SUCCESS;
}
int ABT_mutex_attr_get_recursive(ABT_mutex_attr attr, ABT_bool *recursive) {
  ABTI_mutex_attr *a = ABTI_obj<ABTI_mutex_attr>(attr); ABTI_CHECK_NULL(a, ABT_ERR_INV_MUTEX_ATTR);
  *recursive = a->recursive ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
}

/* ---- condition variable ---- */
struct ABTI_cond {
  std::atomic<int> slock;
  int pad;
  ABTI_mutex *mutex;            /* the first mutex used; another one is an error */
  ABTI_waitlist wl;
};
static_assert(sizeof(ABTI_cond) <= sizeof(ABT_cond_memory), "cond layout");
static inline ABTI_cond *C(ABT_cond h) { return ABTI_obj<ABTI_cond>(h); }

static int cond_wait_impl(ABTI_cond *c, ABTI_mutex *m, double deadline, bool timed) {
  ABTI_thread *self = ABTI_self_thread();
  spin_acquire(c->slock);
  if (c->mutex == nullptr) c->mutex = m;
  else if (c->mutex != m) { spin_release(c->slock); return ABT_ERR_INV_MUTEX; }
  ABTI_waiter w{ABTI_can_block(self) ? self->cth : nullptr, {0}, nullptr};
  wl_push(c->wl, &w);
  ABT_mutex_unlock(reinterpret_cast<ABT_mutex>(m));
  bool signaled = true;
  if (timed) signaled = wait_on_timed(w, c->slock, c->wl, self, deadline);
  else wait_on(w, c->slock, self);
  ABT_mutex_lock(reinterpret_cast<ABT_mutex>(m));
  return signaled ? ABT_SUCCESS : ABT_ERR_COND_TIMEDOUT;
}

int ABT_cond_create(ABT_cond *newcond) {
  ABTI_cond *c = new ABTI_cond(); memset((void *)c, 0, sizeof *c);
  *newcond = reinterpret_cast<ABT_cond>(c); return ABT_SUCCESS;
}
int ABT_cond_free(ABT_cond *cond) {
  ABTI_cond *c = C(*cond); ABTI_CHECK_NULL(c, ABT_ERR_INV_COND);
  if (c->wl.head) return ABT_ERR_COND;
  delete c; *cond = ABT_COND_NULL; return ABT_SUCCESS;
}
int ABT_cond_wait(ABT_cond cond, ABT_mutex mutex) {
  ABTI_cond *c = C(cond); ABTI_CHECK_NULL(c, ABT_ERR_INV_COND);
  ABTI_mutex *m = M(mutex); ABTI_CHECK_NULL(m, ABT_ERR_INV_MUTEX);
  return cond_wait_impl(c, m, 0.0, false);
}
int ABT_cond_timedwait(ABT_cond cond, ABT_mutex mutex, const struct timespec *abstime) {
  ABTI_cond *c = C(cond); ABTI_CHECK_NULL(c, ABT_ERR_INV_COND);
  ABTI_mutex *m = M(mutex); ABTI_CHECK_NULL(m, ABT_ERR_INV_MUTEX);
  return cond_wait_impl(c, m, realtime_to_wall(abstime), true);
}
int ABT_cond_signal(ABT_cond cond) {
  ABTI_cond *c = C(cond); ABTI_CHECK_NULL(c, ABT_ERR_INV_COND);
  spin_acquire(c->slock);
  ABTI_waiter *w = wl_pop(c->wl);
  spin_release(c->slock);
  if (w) wake(w);
  return ABT_SUCCESS;
}
int ABT_cond_broadcast(ABT_cond cond) {
  ABTI_cond *c = C(cond); ABTI_CHECK_NULL(c, ABT_ERR_INV_COND);
  spin_acquire(c->slock);
  ABTI_waiter *list = c->wl.head; c->wl.head = c->wl.tail = nullptr;
  spin_release(c->slock);
  while (list) { ABTI_waiter *n = list->next; wake(list); list = n; }
  return ABT_SUCCESS;
}

/* ---- eventual ---- */
struct ABTI_eventual {
  std::atomic<int> slock;
  int ready;
  int nbytes;
  int pad;
  void *value;                  /* heap buffer when nbytes > 0 */
  ABTI_waitlist wl;
};
static_assert(sizeof(ABTI_eventual) <= sizeof(ABT_eventual_memory), "eventual layout");
static inline ABTI_eventual *E(ABT_eventual h) { return ABTI_obj<ABTI_eventual>(h); }

int ABT_eventual_create(int nbytes, ABT_eventual *neweventual) {
  if (nbytes < 0) return ABT_ERR_INV_ARG;
  ABTI_eventual *e = new ABTI_eventual(); memset((void *)e, 0, sizeof *e);
  e->nbytes = nbytes;
  e->value = nbytes > 0 ? calloc(1, (size_t)nbytes) : nullptr;
  *neweventual = reinterpret_cast<ABT_eventual>(e); return ABT_SUCCESS;
}
int ABT_eventual_free(ABT_eventual *eventual) {
  ABTI_eventual *e = E(*eventual); ABTI_CHECK_NULL(e, ABT_ERR_INV_EVENTUAL);
  free(e->value); delete e; *eventual = ABT_EVENTUAL_NULL; return ABT_SUCCESS;
}
int ABT_eventual_wait(ABT_eventual eventual, void **value) {
  ABTI_eventual *e = E(eventual); ABTI_CHECK_NULL(e, ABT_ERR_INV_EVENTUAL);
  ABTI_thread *self = ABTI_self_thread();
  spin_acquire(e->slock);
  if (!e->ready) {
    ABTI_waiter w{ABTI_can_block(self) ? self->cth : nullptr, {0}, nullptr};
    wl_push(e->wl, &w);
    wait_on(w, e->slock, self);
    /* the setter published ready and value before releasing the lock */
  } else {
    spin_release(e->slock);
  }
  if (value) *value = e->value;
  return ABT_SUCCESS;
}
int ABT_eventual_timedwait(ABT_eventual eventual, void **value, const struct timespec *abstime) {
  ABTI_eventual *e = E(eventual); ABTI_CHECK_NULL(e, ABT_ERR_INV_EVENTUAL);
  ABTI_thread *self = ABTI_self_thread();
  double deadline = realtime_to_wall(abstime);
  spin_acquire(e->slock);
  if (!e->ready) {
    ABTI_waiter w{ABTI_can_block(self) ? self->cth : nullptr, {0}, nullptr};
    wl_push(e->wl, &w);
    if (!wait_on_timed(w, e->slock, e->wl, self, deadline)) return ABT_ERR_COND_TIMEDOUT; /* Argobots' code for a timed-out eventual */
  } else {
    spin_release(e->slock);
  }
  if (value) *value = e->value;
  return ABT_SUCCESS;
}
int ABT_eventual_test(ABT_eventual eventual, void **value, ABT_bool *is_ready) {
  ABTI_eventual *e = E(eventual); ABTI_CHECK_NULL(e, ABT_ERR_INV_EVENTUAL);
  spin_acquire(e->slock);
  int r = e->ready;
  spin_release(e->slock);
  *is_ready = r ? ABT_TRUE : ABT_FALSE;
  if (value) *value = r ? e->value : nullptr;
  return ABT_SUCCESS;
}
int ABT_eventual_set(ABT_eventual eventual, void *value, int nbytes) {
  ABTI_eventual *e = E(eventual); ABTI_CHECK_NULL(e, ABT_ERR_INV_EVENTUAL);
  spin_acquire(e->slock);
  if (e->ready) { spin_release(e->slock); return ABT_ERR_EVENTUAL; }
  if (nbytes > e->nbytes) { spin_release(e->slock); return ABT_ERR_INV_EVENTUAL; }
  if (nbytes > 0 && value) memcpy(e->value, value, (size_t)nbytes);
  e->ready = 1;
  ABTI_waiter *list = e->wl.head; e->wl.head = e->wl.tail = nullptr;
  spin_release(e->slock); /* nothing touches the object after this: safe for stack eventuals */
  while (list) { ABTI_waiter *n = list->next; wake(list); list = n; }
  return ABT_SUCCESS;
}
int ABT_eventual_reset(ABT_eventual eventual) {
  ABTI_eventual *e = E(eventual); ABTI_CHECK_NULL(e, ABT_ERR_INV_EVENTUAL);
  spin_acquire(e->slock); /* also the barrier Margo relies on: a setter still inside set() holds it */
  e->ready = 0;
  spin_release(e->slock);
  return ABT_SUCCESS;
}

/* ---- rwlock (mutex + two conds) ---- */
struct ABTI_rwlock { ABT_mutex m; ABT_cond rd, wr; int readers; int writer; };
static inline ABTI_rwlock *RW(ABT_rwlock h) { return ABTI_obj<ABTI_rwlock>(h); }
int ABT_rwlock_create(ABT_rwlock *newrwlock) {
  ABTI_rwlock *r = new ABTI_rwlock(); r->readers = 0; r->writer = 0;
  ABT_mutex_create(&r->m); ABT_cond_create(&r->rd); ABT_cond_create(&r->wr);
  *newrwlock = reinterpret_cast<ABT_rwlock>(r); return ABT_SUCCESS;
}
int ABT_rwlock_free(ABT_rwlock *rwlock) {
  ABTI_rwlock *r = RW(*rwlock); ABTI_CHECK_NULL(r, ABT_ERR_INV_RWLOCK);
  ABT_mutex_free(&r->m); ABT_cond_free(&r->rd); ABT_cond_free(&r->wr);
  delete r; *rwlock = ABT_RWLOCK_NULL; return ABT_SUCCESS;
}
int ABT_rwlock_rdlock(ABT_rwlock rwlock) {
  ABTI_rwlock *r = RW(rwlock); ABTI_CHECK_NULL(r, ABT_ERR_INV_RWLOCK);
  ABT_mutex_lock(r->m); while (r->writer) ABT_cond_wait(r->rd, r->m); r->readers++; ABT_mutex_unlock(r->m); return ABT_SUCCESS;
}
int ABT_rwlock_wrlock(ABT_rwlock rwlock) {
  ABTI_rwlock *r = RW(rwlock); ABTI_CHECK_NULL(r, ABT_ERR_INV_RWLOCK);
  ABT_mutex_lock(r->m); while (r->writer || r->readers) ABT_cond_wait(r->wr, r->m); r->writer = 1; ABT_mutex_unlock(r->m); return ABT_SUCCESS;
}
int ABT_rwlock_unlock(ABT_rwlock rwlock) {
  ABTI_rwlock *r = RW(rwlock); ABTI_CHECK_NULL(r, ABT_ERR_INV_RWLOCK);
  ABT_mutex_lock(r->m);
  if (r->writer) { r->writer = 0; ABT_cond_broadcast(r->rd); ABT_cond_signal(r->wr); }
  else { if (--r->readers == 0) ABT_cond_signal(r->wr); }
  ABT_mutex_unlock(r->m); return ABT_SUCCESS;
}

/* ---- barrier ---- */
struct ABTI_barrier { ABT_mutex m; ABT_cond c; uint32_t num; uint32_t waiting; uint64_t generation; };
static inline ABTI_barrier *BR(ABT_barrier h) { return ABTI_obj<ABTI_barrier>(h); }
int ABT_barrier_create(uint32_t num_waiters, ABT_barrier *newbarrier) {
  ABTI_barrier *b = new ABTI_barrier(); b->num = num_waiters; b->waiting = 0; b->generation = 0;
  ABT_mutex_create(&b->m); ABT_cond_create(&b->c);
  *newbarrier = reinterpret_cast<ABT_barrier>(b); return ABT_SUCCESS;
}
int ABT_barrier_reinit(ABT_barrier barrier, uint32_t num_waiters) {
  ABTI_barrier *b = BR(barrier); ABTI_CHECK_NULL(b, ABT_ERR_INV_BARRIER);
  ABT_mutex_lock(b->m); b->num = num_waiters; b->waiting = 0; ABT_mutex_unlock(b->m); return ABT_SUCCESS;
}
int ABT_barrier_free(ABT_barrier *barrier) {
  ABTI_barrier *b = BR(*barrier); ABTI_CHECK_NULL(b, ABT_ERR_INV_BARRIER);
  ABT_mutex_free(&b->m); ABT_cond_free(&b->c); delete b; *barrier = ABT_BARRIER_NULL; return ABT_SUCCESS;
}
int ABT_barrier_wait(ABT_barrier barrier) {
  ABTI_barrier *b = BR(barrier); ABTI_CHECK_NULL(b, ABT_ERR_INV_BARRIER);
  ABT_mutex_lock(b->m);
  uint64_t gen = b->generation;
  if (++b->waiting >= b->num) { b->waiting = 0; b->generation++; ABT_cond_broadcast(b->c); }
  else { while (gen == b->generation) ABT_cond_wait(b->c, b->m); }
  ABT_mutex_unlock(b->m); return ABT_SUCCESS;
}
int ABT_barrier_get_num_waiters(ABT_barrier barrier, uint32_t *num_waiters) {
  ABTI_barrier *b = BR(barrier); ABTI_CHECK_NULL(b, ABT_ERR_INV_BARRIER);
  *num_waiters = b->num; return ABT_SUCCESS;
}

/* ---- future (Thallium) ---- */
struct ABTI_future { ABT_mutex m; ABT_cond c; uint32_t n; uint32_t count; void (*cb)(void **); std::vector<void *> values; int ready; };
static inline ABTI_future *F(ABT_future h) { return ABTI_obj<ABTI_future>(h); }
int ABT_future_create(uint32_t num_compartments, void (*cb_func)(void **arg), ABT_future *newfuture) {
  ABTI_future *f = new ABTI_future(); f->n = num_compartments; f->count = 0; f->cb = cb_func; f->values.assign(num_compartments, nullptr); f->ready = 0;
  ABT_mutex_create(&f->m); ABT_cond_create(&f->c);
  *newfuture = reinterpret_cast<ABT_future>(f); return ABT_SUCCESS;
}
int ABT_future_free(ABT_future *future) {
  ABTI_future *f = F(*future); ABTI_CHECK_NULL(f, ABT_ERR_INV_FUTURE);
  ABT_mutex_free(&f->m); ABT_cond_free(&f->c); delete f; *future = ABT_FUTURE_NULL; return ABT_SUCCESS;
}
int ABT_future_wait(ABT_future future) {
  ABTI_future *f = F(future); ABTI_CHECK_NULL(f, ABT_ERR_INV_FUTURE);
  ABT_mutex_lock(f->m); while (!f->ready) ABT_cond_wait(f->c, f->m); ABT_mutex_unlock(f->m); return ABT_SUCCESS;
}
int ABT_future_test(ABT_future future, ABT_bool *is_ready) {
  ABTI_future *f = F(future); ABTI_CHECK_NULL(f, ABT_ERR_INV_FUTURE);
  ABT_mutex_lock(f->m); *is_ready = f->ready ? ABT_TRUE : ABT_FALSE; ABT_mutex_unlock(f->m); return ABT_SUCCESS;
}
int ABT_future_set(ABT_future future, void *value) {
  ABTI_future *f = F(future); ABTI_CHECK_NULL(f, ABT_ERR_INV_FUTURE);
  ABT_mutex_lock(f->m);
  if (f->count >= f->n) { ABT_mutex_unlock(f->m); return ABT_ERR_FUTURE; }
  f->values[f->count++] = value;
  if (f->count == f->n) { f->ready = 1; if (f->cb) f->cb(f->values.data()); ABT_cond_broadcast(f->c); }
  ABT_mutex_unlock(f->m); return ABT_SUCCESS;
}
int ABT_future_reset(ABT_future future) {
  ABTI_future *f = F(future); ABTI_CHECK_NULL(f, ABT_ERR_INV_FUTURE);
  ABT_mutex_lock(f->m); f->count = 0; f->ready = 0; ABT_mutex_unlock(f->m); return ABT_SUCCESS;
}

} /* extern "C" */
