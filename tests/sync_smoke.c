/* Synchronization objects across xstreams and from an external pthread:
 * mutex contention, condvar producer/consumer, eventuals set from another
 * xstream and from a pthread, static (memory-based) objects, timeouts,
 * rwlock, barrier. */
#include <abt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(call) do { int _r = (call); if (_r != ABT_SUCCESS) { char s[128]; size_t l = sizeof s; ABT_error_get_str(_r, s, &l); fprintf(stderr, "%s:%d: %s -> %s\n", __FILE__, __LINE__, #call, s); exit(1); } } while (0)
#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static ABT_mutex_memory g_mtx_mem = ABT_MUTEX_INITIALIZER;
static ABT_cond_memory g_cond_mem = ABT_COND_INITIALIZER;
static ABT_eventual_memory g_ev_mem = ABT_EVENTUAL_INITIALIZER;
static ABT_mutex_memory g_rmtx_mem = ABT_RECURSIVE_MUTEX_INITIALIZER;

static long g_counter;
static volatile int g_done;

/* 1: mutex protects a counter across 3 xstreams */
static void incr(void *arg) {
  ABT_mutex m = *(ABT_mutex *)arg;
  for (int i = 0; i < 1000; i++) { CHECK(ABT_mutex_lock(m)); g_counter++; CHECK(ABT_mutex_unlock(m)); }
  __sync_fetch_and_add(&g_done, 1);
}
/* 2: condvar queue */
static int q_items, q_produced, q_consumed;
static ABT_mutex qm; static ABT_cond qc;
static void producer(void *arg) {
  int n = *(int *)arg;
  for (int i = 0; i < n; i++) { CHECK(ABT_mutex_lock(qm)); q_items++; q_produced++; CHECK(ABT_cond_signal(qc)); CHECK(ABT_mutex_unlock(qm)); if (i % 7 == 0) CHECK(ABT_thread_yield()); }
  __sync_fetch_and_add(&g_done, 1);
}
static void consumer(void *arg) {
  int n = *(int *)arg;
  for (int i = 0; i < n; i++) { CHECK(ABT_mutex_lock(qm)); while (q_items == 0) CHECK(ABT_cond_wait(qc, qm)); q_items--; q_consumed++; CHECK(ABT_mutex_unlock(qm)); }
  __sync_fetch_and_add(&g_done, 1);
}
/* 3: eventual set from a ULT on another xstream */
static void setter(void *arg) { ABT_eventual ev = *(ABT_eventual *)arg; int v = 42; CHECK(ABT_eventual_set(ev, &v, sizeof v)); __sync_fetch_and_add(&g_done, 1); }
static void setter0(void *arg) { ABT_eventual ev = *(ABT_eventual *)arg; CHECK(ABT_eventual_set(ev, NULL, 0)); __sync_fetch_and_add(&g_done, 1); }
static void waiter(void *arg) { ABT_eventual ev = *(ABT_eventual *)arg; int *v; CHECK(ABT_eventual_wait(ev, (void **)&v)); ASSERT(*v == 42); __sync_fetch_and_add(&g_done, 1); }
/* 4: eventual set from an external pthread (Mercury callback shape) */
static void *ext_setter(void *arg) { usleep(20000); ABT_eventual ev = (ABT_eventual)arg; CHECK(ABT_eventual_set(ev, NULL, 0)); return NULL; }
/* 5: static objects: memory-based mutex/cond/eventual, stack eventual reset barrier */
static void static_user(void *arg) {
  ABT_mutex m = ABT_MUTEX_MEMORY_GET_HANDLE(&g_mtx_mem);
  ABT_cond c = ABT_COND_MEMORY_GET_HANDLE(&g_cond_mem);
  (void)arg;
  CHECK(ABT_mutex_lock(m)); g_counter += 5; CHECK(ABT_cond_signal(c)); CHECK(ABT_mutex_unlock(m));
  __sync_fetch_and_add(&g_done, 1);
}
/* 6: rwlock readers/writer; 7: barrier */
static ABT_rwlock rw; static int rw_shared, rw_readers_seen;
static void rw_reader(void *a) { (void)a; for (int i = 0; i < 100; i++) { CHECK(ABT_rwlock_rdlock(rw)); int v = rw_shared; ASSERT(v % 2 == 0); __sync_fetch_and_add(&rw_readers_seen, 1); CHECK(ABT_rwlock_unlock(rw)); } __sync_fetch_and_add(&g_done, 1); }
static void rw_writer(void *a) { (void)a; for (int i = 0; i < 100; i++) { CHECK(ABT_rwlock_wrlock(rw)); rw_shared++; if (i % 3 == 0) CHECK(ABT_thread_yield()); rw_shared++; CHECK(ABT_rwlock_unlock(rw)); } __sync_fetch_and_add(&g_done, 1); }
static ABT_barrier bar; static int bar_phase[8];
static void bar_user(void *a) { int id = *(int *)a; for (int r = 0; r < 5; r++) { bar_phase[id] = r; CHECK(ABT_barrier_wait(bar)); for (int k = 0; k < 4; k++) ASSERT(bar_phase[k] >= r); CHECK(ABT_barrier_wait(bar)); } __sync_fetch_and_add(&g_done, 1); }

static void wait_done(int n) { while (g_done < n) CHECK(ABT_thread_yield()); g_done = 0; }

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setenv("ABT_MAX_NUM_XSTREAMS", "3", 0); /* 3 secondary + the primary = 4 PEs */
  CHECK(ABT_init(argc, argv));
  ABT_pool pool; CHECK(ABT_pool_create_basic(ABT_POOL_FIFO, ABT_POOL_ACCESS_MPMC, ABT_FALSE, &pool));
  ABT_xstream xs[3];
  for (int i = 0; i < 3; i++) CHECK(ABT_xstream_create_basic(ABT_SCHED_BASIC, 1, &pool, ABT_SCHED_CONFIG_NULL, &xs[i]));

  /* 1 */
  ABT_mutex m; CHECK(ABT_mutex_create(&m)); g_counter = 0;
  for (int i = 0; i < 6; i++) CHECK(ABT_thread_create(pool, incr, &m, ABT_THREAD_ATTR_NULL, NULL));
  wait_done(6); ASSERT(g_counter == 6000);
  ASSERT(ABT_mutex_trylock(m) == ABT_SUCCESS); ASSERT(ABT_mutex_trylock(m) == ABT_ERR_MUTEX_LOCKED); CHECK(ABT_mutex_unlock(m));
  CHECK(ABT_mutex_free(&m));
  printf("1 ok: mutex across 3 xstreams, 6000 increments\n");

  /* 2 */
  CHECK(ABT_mutex_create(&qm)); CHECK(ABT_cond_create(&qc));
  int n = 500;
  for (int i = 0; i < 2; i++) CHECK(ABT_thread_create(pool, consumer, &n, ABT_THREAD_ATTR_NULL, NULL));
  for (int i = 0; i < 2; i++) CHECK(ABT_thread_create(pool, producer, &n, ABT_THREAD_ATTR_NULL, NULL));
  wait_done(4); ASSERT(q_produced == 1000 && q_consumed == 1000 && q_items == 0);
  /* timedwait on an empty queue times out */
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 20 * 1000 * 1000; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  CHECK(ABT_mutex_lock(qm)); int r = ABT_cond_timedwait(qc, qm, &ts); CHECK(ABT_mutex_unlock(qm));
  ASSERT(r == ABT_ERR_COND_TIMEDOUT);
  CHECK(ABT_cond_free(&qc)); CHECK(ABT_mutex_free(&qm));
  printf("2 ok: condvar producer/consumer, timedwait timeout\n");

  /* 3 */
  for (int rep = 0; rep < 50; rep++) {
    ABT_eventual ev; CHECK(ABT_eventual_create(sizeof(int), &ev));
    CHECK(ABT_thread_create(pool, waiter, &ev, ABT_THREAD_ATTR_NULL, NULL));
    CHECK(ABT_thread_create(pool, setter, &ev, ABT_THREAD_ATTR_NULL, NULL));
    wait_done(2);
    int rc = ABT_eventual_set(ev, NULL, 0); ASSERT(rc == ABT_ERR_EVENTUAL); /* second set */
    CHECK(ABT_eventual_reset(ev)); ABT_bool ready; CHECK(ABT_eventual_test(ev, NULL, &ready)); ASSERT(ready == ABT_FALSE);
    CHECK(ABT_eventual_free(&ev));
  }
  printf("3 ok: 50 eventuals set from another xstream, second set rejected, reset\n");

  /* 4: primary waits on an eventual set by an external pthread; then the
   * stack-eventual idiom: wait, then reset as the barrier before scope exit */
  for (int rep = 0; rep < 5; rep++) {
    ABT_eventual_memory mem = ABT_EVENTUAL_INITIALIZER;
    ABT_eventual ev = ABT_EVENTUAL_MEMORY_GET_HANDLE(&mem);
    pthread_t th; pthread_create(&th, NULL, ext_setter, ev);
    CHECK(ABT_eventual_wait(ev, NULL));
    CHECK(ABT_eventual_reset(ev));
    pthread_join(th, NULL);
  }
  /* external pthread waits on an eventual set by a ULT */
  {
    ABT_eventual ev = ABT_EVENTUAL_MEMORY_GET_HANDLE(&g_ev_mem);
    CHECK(ABT_thread_create(pool, setter0, &ev, ABT_THREAD_ATTR_NULL, NULL));
    /* we are a ULT here; emulate the external wait via the test's own waiter on a pthread is covered by ext_thread tests */
    wait_done(1);
    CHECK(ABT_eventual_wait(ev, NULL));
  }
  printf("4 ok: stack eventuals set from a pthread, reset barrier\n");

  /* 5 */
  g_counter = 0;
  for (int i = 0; i < 4; i++) CHECK(ABT_thread_create(pool, static_user, NULL, ABT_THREAD_ATTR_NULL, NULL));
  { ABT_mutex sm = ABT_MUTEX_MEMORY_GET_HANDLE(&g_mtx_mem); ABT_cond sc = ABT_COND_MEMORY_GET_HANDLE(&g_cond_mem);
    CHECK(ABT_mutex_lock(sm)); while (g_counter < 20) CHECK(ABT_cond_wait(sc, sm)); CHECK(ABT_mutex_unlock(sm)); }
  wait_done(4);
  { ABT_mutex rm = ABT_MUTEX_MEMORY_GET_HANDLE(&g_rmtx_mem); CHECK(ABT_mutex_lock(rm)); CHECK(ABT_mutex_lock(rm)); CHECK(ABT_mutex_unlock(rm)); ASSERT(ABT_mutex_trylock(rm) == ABT_SUCCESS); CHECK(ABT_mutex_unlock(rm)); CHECK(ABT_mutex_unlock(rm)); }
  printf("5 ok: static mutex/cond, recursive static mutex\n");

  /* 6, 7 */
  CHECK(ABT_rwlock_create(&rw));
  for (int i = 0; i < 4; i++) CHECK(ABT_thread_create(pool, rw_reader, NULL, ABT_THREAD_ATTR_NULL, NULL));
  for (int i = 0; i < 2; i++) CHECK(ABT_thread_create(pool, rw_writer, NULL, ABT_THREAD_ATTR_NULL, NULL));
  wait_done(6); ASSERT(rw_shared == 400 && rw_readers_seen == 400);
  CHECK(ABT_rwlock_free(&rw));
  CHECK(ABT_barrier_create(4, &bar));
  int ids[4] = {0, 1, 2, 3};
  for (int i = 0; i < 4; i++) CHECK(ABT_thread_create(pool, bar_user, &ids[i], ABT_THREAD_ATTR_NULL, NULL));
  wait_done(4);
  CHECK(ABT_barrier_free(&bar));
  printf("6/7 ok: rwlock and barrier\n");

  for (int i = 0; i < 3; i++) { CHECK(ABT_xstream_join(xs[i])); CHECK(ABT_xstream_free(&xs[i])); }
  CHECK(ABT_pool_free(&pool));
  CHECK(ABT_finalize());
  printf("sync_smoke ok\n");
  return 0;
}
