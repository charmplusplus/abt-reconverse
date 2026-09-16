/* ULTs over reconverse threads: creation into a pool, wake routing through
 * the thread's pool (deferred migration applied at wake), join through the
 * post-switch exit callback, yield, self queries, attributes, keys. */
#include "abti.h"
#include <cstring>
#include <sched.h>

/* Per-OS-thread cache of descriptor memory. A ULT per request (Margo, DAOS)
 * allocates and frees one ABTI_thread per ULT; with the stack and the Cth
 * struct cached in reconverse, new/delete of this object was the next largest
 * per-ULT cost (DAOS pilot step 4). The destructor and constructor still run;
 * only the allocator is bypassed. */
#include <new>
static thread_local std::vector<void *> ABTI_thread_cache;
static ABTI_thread *ABTI_thread_alloc() {
  if (!ABTI_thread_cache.empty()) {
    void *m = ABTI_thread_cache.back();
    ABTI_thread_cache.pop_back();
    return new (m) ABTI_thread();
  }
  return new ABTI_thread();
}
static void ABTI_thread_release(ABTI_thread *t) {
  t->~ABTI_thread();
  if (ABTI_thread_cache.size() < 1024) ABTI_thread_cache.push_back(static_cast<void *>(t));
  else ::operator delete(static_cast<void *>(t));
}

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

/* ---- key destructors (process-wide, indexed by key id) ---- */
static std::mutex g_key_mutex;
static std::vector<void (*)(void *)> g_key_dtors;

static void run_key_destructors(ABTI_thread *t) {
  for (size_t i = 0; i < t->keys.size(); i++) {
    void *v = t->keys[i];
    if (!v) continue;
    void (*d)(void *) = nullptr;
    { std::lock_guard<std::mutex> g(g_key_mutex); if (i < g_key_dtors.size()) d = g_key_dtors[i]; }
    t->keys[i] = nullptr;
    if (d) d(v);
  }
}

/* the reconverse thread body */
static void thread_main(void *arg) {
  ABTI_thread *t = static_cast<ABTI_thread *>(arg);
  /* ABT_thread_exit unwinds to here; reconverse has no terminate-self
   * primitive. C++ objects with destructors live on the ULT's stack between
   * the two points are skipped, as with Argobots' own exit. */
  if (setjmp(t->exit_jmp) == 0) t->fn(t->arg);
  run_key_destructors(t);
}

static void unlock_mutex(void *m) { static_cast<std::mutex *>(m)->unlock(); }

void ABTI_thread_terminated(ABTI_thread *t) {
  std::vector<CthThread> joiners;
  {
    std::lock_guard<std::mutex> g(t->jm);
    t->terminated.store(1, std::memory_order_release);
    joiners.swap(t->joiners);
  }
  for (CthThread j : joiners) CthAwakenIfBlocked(j);
  if (t->type == ABTI_THREAD_DETACHED) {
    ABTI_pool_disassociate(t);
    ABTI_thread_release(t); /* a ULT's CthThread is freed by reconverse; a tasklet has none */
  }
}

/* runs post-switch, on the exiting thread's PE, once it is off its stack */
static void thread_exit_fn(void *arg) { ABTI_thread_terminated(static_cast<ABTI_thread *>(arg)); }

void ABTI_thread_awaken_fn(CthThread cth, void *arg) {
  ABTI_thread *t = static_cast<ABTI_thread *>(arg);
  if (t->type == ABTI_THREAD_PRIMARY) ABTI_DBG("wake primary -> pool %p", (void *)t->pool);
  /* a blocked ULT stops counting as blocked the moment it is made ready, as
   * in Argobots (ABTI_ythread_set_ready); counting it until it resumed made
   * ABT_pool_get_total_size overshoot and Margo's progress loop spin */
  if (t->blocked_counted.exchange(0, std::memory_order_acq_rel))
    t->blocked_pool->num_blocked.fetch_sub(1, std::memory_order_acq_rel);
  if (t->migrate_to) {
    ABTI_pool *p = t->migrate_to;
    t->migrate_to = nullptr;
    ABTI_pool_associate(t, p);
  }
  t->pool->push(t);
}

void ABTI_thread_block(ABTI_thread *self, CthVoidFn after, void *arg) {
  if (self->type == ABTI_THREAD_SCHED) { CthSuspendBlocked(after, arg); return; } /* not a pool member */
  ABTI_pool *p = self->pool;
  p->num_blocked.fetch_add(1, std::memory_order_acq_rel);
  self->blocked_pool = p;
  self->blocked_counted.store(1, std::memory_order_release);
  CthSuspendBlocked(after, arg);
  /* the waker decremented num_blocked when it made us ready */
}

/* the runner of a user-defined scheduler: it may yield, block on barriers
 * and mutexes (Argobots' "main scheduler can yield"); its wake goes to its
 * own PE's queue so the PE's built-in scheduler resumes it (nothing else
 * polls a runner) */
static void runner_awaken_fn(CthThread cth, void *arg) {
  ABTI_thread *t = static_cast<ABTI_thread *>(arg);
  if (t->blocked_counted.exchange(0, std::memory_order_acq_rel))
    t->blocked_pool->num_blocked.fetch_sub(1, std::memory_order_acq_rel);
  CmiPushPE(t->last_xstream ? t->last_xstream->rank : CmiMyRank(), CthGetToken(cth));
}
ABTI_thread *ABTI_thread_wrap_runner(CthThread cth, ABTI_pool *pool) {
  ABTI_thread *t = ABTI_thread_alloc();
  t->cth = cth;
  t->type = ABTI_THREAD_SCHED;
  t->fn = nullptr; t->arg = nullptr;
  t->attr = ABTI_thread_attr{0, nullptr, ABT_FALSE};
  t->id = ABTI_g->next_id.fetch_add(1);
  t->pool = pool; t->unit = ABT_UNIT_NULL; /* not a member of the pool: no association */
  t->last_xstream = ABTI_tls_xstream; t->migrate_to = nullptr;
  t->freed_by_exit = false; t->is_task = false; t->blocked_pool = nullptr;
  CthSetUserData(cth, t);
  CthSetAwakenFn(cth, runner_awaken_fn, t);
  return t;
}

ABTI_thread *ABTI_thread_wrap_primary(CthThread cth, ABTI_pool *pool) {
  ABTI_thread *t = ABTI_thread_alloc();
  t->cth = cth;
  t->type = ABTI_THREAD_PRIMARY;
  t->fn = nullptr; t->arg = nullptr;
  t->attr = ABTI_thread_attr{0, nullptr, ABT_TRUE};
  t->id = ABTI_g->next_id.fetch_add(1);
  t->pool = nullptr; t->unit = ABT_UNIT_NULL;
  t->last_xstream = nullptr; t->migrate_to = nullptr;
  t->freed_by_exit = false;
  t->is_task = false;
  t->blocked_pool = nullptr;
  ABTI_pool_associate(t, pool);
  CthSetUserData(cth, t);
  CthSetAwakenFn(cth, ABTI_thread_awaken_fn, t); /* pinned to PE 0 by reconverse */
  return t;
}

int ABTI_thread_join_impl(ABTI_thread *t) {
  if (t->terminated.load(std::memory_order_acquire)) return ABT_SUCCESS;
  ABTI_thread *self = ABTI_self_thread();
  if (self == t) return ABT_ERR_INV_THREAD;
  if (ABTI_can_block(self)) {
    t->jm.lock();
    if (t->terminated.load(std::memory_order_acquire)) { t->jm.unlock(); return ABT_SUCCESS; }
    t->joiners.push_back(self->cth);
    ABTI_thread_block(self, unlock_mutex, &t->jm); /* unlocks once BLOCKED */
    return ABT_SUCCESS;
  }
  while (!t->terminated.load(std::memory_order_acquire)) sched_yield(); /* external thread */
  return ABT_SUCCESS;
}

void ABTI_thread_destroy(ABTI_thread *t) {
  ABTI_pool_disassociate(t);
  if (t->cth && ABTI_on_pe()) CthFree(t->cth); /* off-PE the stack is leaked rather than freed unsafely */
  ABTI_thread_release(t);
}

static ABT_thread_state map_state(ABTI_thread *t) {
  if (t->terminated.load(std::memory_order_acquire)) return ABT_THREAD_STATE_TERMINATED;
  switch (t->cth ? CthGetState(t->cth) : t->tstate.load()) {
  case CTH_STATE_RUNNING: return ABT_THREAD_STATE_RUNNING;
  case CTH_STATE_BLOCKED: return ABT_THREAD_STATE_BLOCKED;
  case CTH_STATE_TERMINATED: return ABT_THREAD_STATE_TERMINATED;
  default: return ABT_THREAD_STATE_READY;
  }
}

extern "C" {

static int thread_create_impl(ABT_pool pool, void (*thread_func)(void *), void *arg, ABT_thread_attr attr, ABT_thread *newthread, bool is_task);
/* a detached ULT with an explicit stack size, for the runtime's own units */
extern "C++" int ABTI_thread_create_internal(ABT_pool pool, void (*fn)(void *), void *arg, size_t stacksize) {
  ABT_thread_attr a; int r = ABT_thread_attr_create(&a); if (r != ABT_SUCCESS) return r;
  ABT_thread_attr_set_stacksize(a, stacksize);
  r = thread_create_impl(pool, fn, arg, a, nullptr, false);
  ABT_thread_attr_free(&a);
  return r;
}
struct create_req2 { ABT_pool pool; void (*fn)(void *); void *arg; ABT_thread_attr attr; ABT_thread *out; bool is_task; int ret; };
static void create_on_pe2(void *a) { create_req2 *r = (create_req2 *)a; r->ret = thread_create_impl(r->pool, r->fn, r->arg, r->attr, r->out, r->is_task); }

int ABT_thread_create(ABT_pool pool, void (*thread_func)(void *), void *arg, ABT_thread_attr attr, ABT_thread *newthread) {
  return thread_create_impl(pool, thread_func, arg, attr, newthread, false);
}

static int thread_create_impl(ABT_pool pool, void (*thread_func)(void *), void *arg, ABT_thread_attr attr, ABT_thread *newthread, bool is_task) {
  ABTI_CHECK_INITIALIZED();
  ABTI_pool *p = ABTI_pool_get(pool);
  ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  if (!ABTI_on_pe()) {
    /* stacks and tokens are PE-owned: create on a PE and wait (Mercury
     * callbacks and Mochi clients do create ULTs from plain pthreads) */
    create_req2 r{pool, thread_func, arg, attr, newthread, is_task, ABT_SUCCESS};
    ABTI_run_on_pe(create_on_pe2, &r);
    return r.ret;
  }
  ABTI_thread *t = ABTI_thread_alloc();
  t->type = newthread ? ABTI_THREAD_NAMED : ABTI_THREAD_DETACHED;
  t->fn = thread_func; t->arg = arg;
  t->attr = ABTI_attr_get(attr) ? *ABTI_attr_get(attr) : ABTI_thread_attr{0, nullptr, ABT_TRUE};
  size_t stacksize = t->attr.stacksize ? t->attr.stacksize : ABTI_g->default_stacksize;
  t->id = ABTI_g->next_id.fetch_add(1);
  t->pool = nullptr; t->unit = ABT_UNIT_NULL;
  t->last_xstream = nullptr; t->migrate_to = nullptr;
  t->freed_by_exit = t->type == ABTI_THREAD_DETACHED;
  t->is_task = is_task;
  t->blocked_pool = nullptr;
  t->cth = CthCreate(thread_main, t, (int)stacksize);
  CthSetUserData(t->cth, t);
  CthSetAwakenFn(t->cth, ABTI_thread_awaken_fn, t);
  CthSetChooseFn(t->cth, ABTI_choose_fn); /* back to a user scheduler's runner if one owns this PE */
  if (t->type == ABTI_THREAD_NAMED) CthSetKeepOnExit(t->cth, 1);
  CthSetExitFn(t->cth, thread_exit_fn, t);
  ABTI_pool_associate(t, p);
  if (newthread) *newthread = ABTI_thread_handle(t);
  CthAwakenIfBlocked(t->cth); /* BLOCKED -> READY, pushed to the pool */
  return ABT_SUCCESS;
}

int ABT_thread_create_on_xstream(ABT_xstream xstream, void (*thread_func)(void *), void *arg, ABT_thread_attr attr, ABT_thread *newthread) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  if (!x->main_sched || x->main_sched->pools.empty()) return ABT_ERR_INV_XSTREAM;
  return ABT_thread_create(ABTI_pool_handle(x->main_sched->pools[0]), thread_func, arg, attr, newthread);
}

int ABT_thread_join(ABT_thread thread) {
  ABTI_CHECK_INITIALIZED();
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  if (t->type != ABTI_THREAD_NAMED) return ABT_ERR_INV_THREAD;
  return ABTI_thread_join_impl(t);
}

int ABT_thread_join_many(int num_threads, ABT_thread *thread_list) {
  for (int i = 0; i < num_threads; i++) { int r = ABT_thread_join(thread_list[i]); if (r != ABT_SUCCESS) return r; }
  return ABT_SUCCESS;
}

int ABT_thread_free(ABT_thread *thread) {
  ABTI_CHECK_INITIALIZED();
  ABTI_CHECK_NULL(thread, ABT_ERR_INV_THREAD);
  ABTI_thread *t = ABTI_thread_get(*thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  if (t->type != ABTI_THREAD_NAMED) return ABT_ERR_INV_THREAD;
  if (ABTI_self_thread() == t) return ABT_ERR_INV_THREAD;
  int r = ABTI_thread_join_impl(t);
  if (r != ABT_SUCCESS) return r;
  ABTI_thread_destroy(t);
  *thread = ABT_THREAD_NULL;
  return ABT_SUCCESS;
}

int ABT_thread_free_many(int num, ABT_thread *thread_list) {
  for (int i = 0; i < num; i++) { int r = ABT_thread_free(&thread_list[i]); if (r != ABT_SUCCESS) return r; }
  return ABT_SUCCESS;
}

int ABT_thread_yield(void) {
  ABTI_CHECK_INITIALIZED();
  ABTI_thread *t = ABTI_self_thread();
  if (!t || t->is_task) return ABT_SUCCESS; /* external thread or tasklet: no-op (Argobots 1.x) */
  CthYield();
  return ABT_SUCCESS;
}
int ABT_thread_yield_to(ABT_thread thread) { return ABT_thread_yield(); }

int ABT_thread_resume(ABT_thread thread) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  return CthAwakenIfBlocked(t->cth) ? ABT_SUCCESS : ABT_ERR_THREAD;
}

int ABT_thread_self(ABT_thread *thread) {
  if (thread) *thread = ABT_THREAD_NULL; /* written even on failure (Argobots does) */
  ABTI_CHECK_INITIALIZED();
  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM;
  ABTI_thread *t = ABTI_self_thread();
  if (!t || t->is_task) return ABT_ERR_INV_THREAD; /* a tasklet is not a ULT */
  *thread = ABTI_thread_handle(t); return ABT_SUCCESS;
}
int ABT_thread_self_id(ABT_unit_id *id) {
  ABTI_thread *t = ABTI_self_thread(); if (!t || t->is_task) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  *id = t->id; return ABT_SUCCESS;
}
int ABT_thread_get_id(ABT_thread thread, ABT_unit_id *thread_id) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *thread_id = t->id; return ABT_SUCCESS;
}
int ABT_thread_get_state(ABT_thread thread, ABT_thread_state *state) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *state = map_state(t); return ABT_SUCCESS;
}
int ABT_thread_get_last_xstream(ABT_thread thread, ABT_xstream *xstream) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *xstream = ABTI_xstream_handle(t->last_xstream); return ABT_SUCCESS;
}
int ABT_thread_get_last_pool(ABT_thread thread, ABT_pool *pool) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *pool = ABTI_pool_handle(t->pool); return ABT_SUCCESS;
}
int ABT_thread_get_last_pool_id(ABT_thread thread, int *id) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *id = t->pool ? (int)t->pool->id : -1; return ABT_SUCCESS;
}
int ABT_thread_get_unit(ABT_thread thread, ABT_unit *unit) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *unit = (t->pool && t->pool->user) ? t->unit : (ABT_unit)t; return ABT_SUCCESS;
}
int ABT_thread_set_associated_pool(ABT_thread thread, ABT_pool pool) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  ABTI_pool_associate(t, p); return ABT_SUCCESS;
}
int ABT_thread_migrate_to_pool(ABT_thread thread, ABT_pool pool) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  t->migrate_to = p; /* applied the next time the thread is made ready */
  return ABT_SUCCESS;
}
int ABT_thread_migrate_to_xstream(ABT_thread thread, ABT_xstream xstream) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  if (!x->main_sched || x->main_sched->pools.empty()) return ABT_ERR_INV_XSTREAM;
  return ABT_thread_migrate_to_pool(thread, ABTI_pool_handle(x->main_sched->pools[0]));
}
int ABT_thread_migrate_to_sched(ABT_thread thread, ABT_sched sched) {
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  if (s->pools.empty()) return ABT_ERR_INV_SCHED;
  return ABT_thread_migrate_to_pool(thread, ABTI_pool_handle(s->pools[0]));
}
int ABT_thread_migrate(ABT_thread thread) {
  /* to some other xstream's main pool, as Argobots does */
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  int avoid = t->last_xstream ? t->last_xstream->rank : (ABTI_tls_xstream ? ABTI_tls_xstream->rank : -1);
  ABTI_pool *target = nullptr;
  {
    std::lock_guard<std::mutex> g(ABTI_g->xm);
    for (ABTI_xstream *x : ABTI_g->xstreams)
      if (x && x->rank != avoid && x->main_sched && !x->main_sched->pools.empty()) { target = x->main_sched->pools[0]; break; }
  }
  if (!target) return ABT_ERR_MIGRATION_TARGET;
  t->migrate_to = target;
  return ABT_SUCCESS;
}
int ABT_thread_set_callback(ABT_thread thread, void (*cb_func)(ABT_thread, void *), void *cb_arg) { return ABT_SUCCESS; /* migration callbacks: never invoked */ }
int ABT_thread_set_migratable(ABT_thread thread, ABT_bool migratable) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  t->attr.migratable = migratable; return ABT_SUCCESS;
}
int ABT_thread_is_migratable(ABT_thread thread, ABT_bool *is_migratable) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *is_migratable = t->attr.migratable; return ABT_SUCCESS;
}
int ABT_thread_is_primary(ABT_thread thread, ABT_bool *is_primary) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *is_primary = t->type == ABTI_THREAD_PRIMARY ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
}
int ABT_thread_is_unnamed(ABT_thread thread, ABT_bool *is_unnamed) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *is_unnamed = t->type == ABTI_THREAD_DETACHED ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
}
int ABT_thread_equal(ABT_thread thread1, ABT_thread thread2, ABT_bool *result) {
  *result = thread1 == thread2 ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
}
int ABT_thread_get_stacksize(ABT_thread thread, size_t *stacksize) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *stacksize = t->attr.stacksize ? t->attr.stacksize : ABTI_g->default_stacksize; return ABT_SUCCESS;
}
int ABT_thread_get_stack(ABT_thread thread, void **stackaddr, size_t *stacksize) { ABTI_UNIMPLEMENTED("ABT_thread_get_stack"); }
int ABT_thread_set_arg(ABT_thread thread, void *arg) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  t->arg = arg; return ABT_SUCCESS;
}
int ABT_thread_get_arg(ABT_thread thread, void **arg) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *arg = t->arg; return ABT_SUCCESS;
}
int ABT_thread_get_thread_func(ABT_thread thread, void (**thread_func)(void *)) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  *thread_func = t->fn; return ABT_SUCCESS;
}
int ABT_thread_get_attr(ABT_thread thread, ABT_thread_attr *attr) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  ABTI_thread_attr *a = new ABTI_thread_attr(t->attr);
  if (a->stacksize == 0) a->stacksize = ABTI_g->default_stacksize;
  *attr = reinterpret_cast<ABT_thread_attr>(a); return ABT_SUCCESS;
}

/* keys */
int ABT_thread_set_specific(ABT_thread thread, ABT_key key, void *value) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  ABTI_key *k = ABTI_key_get(key); ABTI_CHECK_NULL(k, ABT_ERR_INV_KEY);
  std::lock_guard<std::mutex> g(t->jm); /* another ULT may set a running thread's key */
  if ((size_t)k->id >= t->keys.size()) t->keys.resize(k->id + 1, nullptr);
  t->keys[k->id] = value; return ABT_SUCCESS;
}
int ABT_thread_get_specific(ABT_thread thread, ABT_key key, void **value) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  ABTI_key *k = ABTI_key_get(key); ABTI_CHECK_NULL(k, ABT_ERR_INV_KEY);
  std::lock_guard<std::mutex> g(t->jm);
  *value = (size_t)k->id < t->keys.size() ? t->keys[k->id] : nullptr; return ABT_SUCCESS;
}
int ABT_key_create(void (*destructor)(void *value), ABT_key *newkey) {
  ABTI_CHECK_INITIALIZED();
  ABTI_key *k = new ABTI_key();
  k->destructor = destructor;
  k->id = ABTI_g->next_key.fetch_add(1);
  { std::lock_guard<std::mutex> g(g_key_mutex); if ((size_t)k->id >= g_key_dtors.size()) g_key_dtors.resize(k->id + 1, nullptr); g_key_dtors[k->id] = destructor; }
  *newkey = reinterpret_cast<ABT_key>(k); return ABT_SUCCESS;
}
int ABT_key_free(ABT_key *key) {
  ABTI_key *k = ABTI_key_get(*key); ABTI_CHECK_NULL(k, ABT_ERR_INV_KEY);
  { std::lock_guard<std::mutex> g(g_key_mutex); if ((size_t)k->id < g_key_dtors.size()) g_key_dtors[k->id] = nullptr; }
  delete k; *key = ABT_KEY_NULL; return ABT_SUCCESS;
}
int ABT_key_set(ABT_key key, void *value) {
  ABTI_thread *t = ABTI_self_thread(); if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  return ABT_thread_set_specific(ABTI_thread_handle(t), key, value);
}
int ABT_key_get(ABT_key key, void **value) {
  ABTI_thread *t = ABTI_self_thread(); if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  return ABT_thread_get_specific(ABTI_thread_handle(t), key, value);
}

/* attributes */
int ABT_thread_attr_create(ABT_thread_attr *newattr) {
  *newattr = reinterpret_cast<ABT_thread_attr>(new ABTI_thread_attr{0, nullptr, ABT_TRUE}); return ABT_SUCCESS;
}
int ABT_thread_attr_free(ABT_thread_attr *attr) {
  ABTI_thread_attr *a = ABTI_attr_get(*attr); ABTI_CHECK_NULL(a, ABT_ERR_INV_THREAD_ATTR);
  delete a; *attr = ABT_THREAD_ATTR_NULL; return ABT_SUCCESS;
}
int ABT_thread_attr_set_stack(ABT_thread_attr attr, void *stackaddr, size_t stacksize) {
  ABTI_thread_attr *a = ABTI_attr_get(attr); ABTI_CHECK_NULL(a, ABT_ERR_INV_THREAD_ATTR);
  /* the size is honored; the address is recorded but reconverse allocates
   * the stack itself (Margo and Thallium only ever set sizes) */
  a->stackaddr = stackaddr;
  a->stacksize = stacksize; return ABT_SUCCESS;
}
int ABT_thread_attr_get_stack(ABT_thread_attr attr, void **stackaddr, size_t *stacksize) {
  ABTI_thread_attr *a = ABTI_attr_get(attr); ABTI_CHECK_NULL(a, ABT_ERR_INV_THREAD_ATTR);
  *stackaddr = a->stackaddr; *stacksize = a->stacksize; return ABT_SUCCESS;
}
int ABT_thread_attr_set_stacksize(ABT_thread_attr attr, size_t stacksize) {
  ABTI_thread_attr *a = ABTI_attr_get(attr); ABTI_CHECK_NULL(a, ABT_ERR_INV_THREAD_ATTR);
  a->stacksize = stacksize; return ABT_SUCCESS;
}
int ABT_thread_attr_get_stacksize(ABT_thread_attr attr, size_t *stacksize) {
  ABTI_thread_attr *a = ABTI_attr_get(attr); ABTI_CHECK_NULL(a, ABT_ERR_INV_THREAD_ATTR);
  *stacksize = a->stacksize ? a->stacksize : ABTI_g->default_stacksize; return ABT_SUCCESS;
}
int ABT_thread_attr_set_callback(ABT_thread_attr attr, void (*cb_func)(ABT_thread, void *), void *cb_arg) { return ABT_SUCCESS; }
int ABT_thread_attr_set_migratable(ABT_thread_attr attr, ABT_bool is_migratable) {
  ABTI_thread_attr *a = ABTI_attr_get(attr); ABTI_CHECK_NULL(a, ABT_ERR_INV_THREAD_ATTR);
  a->migratable = is_migratable; return ABT_SUCCESS;
}

/* self */
int ABT_self_get_xstream(ABT_xstream *xstream) {
  if (xstream) *xstream = ABT_XSTREAM_NULL;
  ABTI_CHECK_INITIALIZED();
  if (!ABTI_on_pe() || !ABTI_tls_xstream) return ABT_ERR_INV_XSTREAM;
  *xstream = ABTI_xstream_handle(ABTI_tls_xstream); return ABT_SUCCESS;
}
int ABT_self_get_xstream_rank(int *rank) {
  ABTI_CHECK_INITIALIZED();
  if (!ABTI_on_pe() || !ABTI_tls_xstream) return ABT_ERR_INV_XSTREAM;
  *rank = ABTI_tls_xstream->rank; return ABT_SUCCESS;
}
int ABT_self_get_thread(ABT_thread *thread) {
  /* 1.x: the calling work unit, ULT or tasklet (ABT_self_get_task is this) */
  if (thread) *thread = ABT_THREAD_NULL;
  ABTI_CHECK_INITIALIZED();
  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM;
  ABTI_thread *t = ABTI_self_thread();
  if (!t) return ABT_ERR_INV_THREAD;
  *thread = ABTI_thread_handle(t); return ABT_SUCCESS;
}
int ABT_self_get_thread_id(ABT_unit_id *id) { return ABT_thread_self_id(id); }
int ABT_self_set_specific(ABT_key key, void *value) { return ABT_key_set(key, value); }
int ABT_self_get_specific(ABT_key key, void **value) { return ABT_key_get(key, value); }
int ABT_self_get_type(ABT_unit_type *type) {
  if (type) *type = ABT_UNIT_TYPE_EXT;
  ABTI_CHECK_INITIALIZED();
  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM; /* 1.x: external thread reports EXT with this error */
  ABTI_thread *st = ABTI_self_thread();
  *type = st ? (st->is_task ? ABT_UNIT_TYPE_TASK : ABT_UNIT_TYPE_THREAD) : ABT_UNIT_TYPE_EXT; return ABT_SUCCESS;
}
int ABT_self_is_primary(ABT_bool *is_primary) {
  if (is_primary) *is_primary = ABT_FALSE;
  ABTI_CHECK_INITIALIZED();
  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM; /* 1.x: external thread */
  ABTI_thread *t = ABTI_self_thread();
  if (t && t->is_task) return ABT_ERR_INV_THREAD; /* 1.x: a tasklet is not a ULT */
  *is_primary = (t && t->type == ABTI_THREAD_PRIMARY) ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
}
int ABT_self_on_primary_xstream(ABT_bool *on_primary) {
  if (on_primary) *on_primary = ABT_FALSE;
  ABTI_CHECK_INITIALIZED();
  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM; /* 1.x: external thread */
  *on_primary = (ABTI_on_pe() && ABTI_tls_xstream && ABTI_tls_xstream->primary) ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
}
int ABT_self_is_unnamed(ABT_bool *is_unnamed) {
  ABTI_thread *t = ABTI_self_thread(); if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  *is_unnamed = t->type == ABTI_THREAD_DETACHED ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
}
int ABT_self_get_last_pool(ABT_pool *pool) {
  ABTI_thread *t = ABTI_self_thread(); if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  *pool = ABTI_pool_handle(t->pool); return ABT_SUCCESS;
}
int ABT_self_get_last_pool_id(int *pool_id) {
  ABTI_thread *t = ABTI_self_thread(); if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  *pool_id = t->pool ? (int)t->pool->id : -1; return ABT_SUCCESS;
}
int ABT_self_set_associated_pool(ABT_pool pool) {
  ABTI_thread *t = ABTI_self_thread(); if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  return ABT_thread_set_associated_pool(ABTI_thread_handle(t), pool);
}
int ABT_self_get_unit(ABT_unit *unit) {
  ABTI_thread *t = ABTI_self_thread(); if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  return ABT_thread_get_unit(ABTI_thread_handle(t), unit);
}
int ABT_self_yield(void) { return ABT_thread_yield(); }
int ABT_self_yield_to(ABT_thread thread) { return ABT_thread_yield(); }
int ABT_self_suspend(void) {
  ABTI_CHECK_INITIALIZED();
  ABTI_thread *t = ABTI_self_thread(); if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  if (!ABTI_can_block(t)) return ABT_ERR_INV_THREAD; /* a tasklet cannot suspend */
  ABTI_thread_block(t, nullptr, nullptr); return ABT_SUCCESS;
}
int ABT_self_set_arg(void *arg) {
  ABTI_thread *t = ABTI_self_thread(); if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  t->arg = arg; return ABT_SUCCESS;
}
int ABT_self_get_arg(void **arg) {
  ABTI_thread *t = ABTI_self_thread(); if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  *arg = t->arg; return ABT_SUCCESS;
}
int ABT_self_get_thread_func(void (**thread_func)(void *)) {
  ABTI_thread *t = ABTI_self_thread(); if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  *thread_func = t->fn; return ABT_SUCCESS;
}

/* not supported */
/* create a ULT and yield to it: the new ULT is pushed to pool and the caller
 * yields (goes to the back of its own pool), so the scheduler runs the new
 * one next when the two share a pool */
int ABT_thread_create_to(ABT_pool pool, void (*thread_func)(void *), void *arg, ABT_thread_attr attr, ABT_thread *newthread) {
  int r = thread_create_impl(pool, thread_func, arg, attr, newthread, false);
  if (r != ABT_SUCCESS) return r;
  if (ABTI_can_block(ABTI_self_thread())) return ABT_thread_yield();
  return ABT_SUCCESS;
}
int ABT_thread_create_many(int num_threads, ABT_pool *pool_list, void (**thread_func_list)(void *), void **arg_list, ABT_thread_attr attr, ABT_thread *newthread_list) { ABTI_UNIMPLEMENTED("ABT_thread_create_many"); }
int ABT_thread_revive(ABT_pool pool, void (*thread_func)(void *), void *arg, ABT_thread *thread) { ABTI_UNIMPLEMENTED("ABT_thread_revive"); }
int ABT_thread_revive_to(ABT_pool pool, void (*thread_func)(void *), void *arg, ABT_thread *thread) { ABTI_UNIMPLEMENTED("ABT_thread_revive_to"); }
int ABT_thread_exit(void) {
  ABTI_CHECK_INITIALIZED();
  ABTI_thread *t = ABTI_self_thread();
  if (!t) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
  if (t->type == ABTI_THREAD_PRIMARY) return ABT_ERR_INV_THREAD;
  longjmp(t->exit_jmp, 1); /* does not return: thread_main finishes the thread */
}
int ABT_thread_cancel(ABT_thread thread) { ABTI_UNIMPLEMENTED("ABT_thread_cancel"); }
int ABT_self_resume_yield_to(ABT_thread thread) { ABTI_UNIMPLEMENTED("ABT_self_resume_yield_to"); }
int ABT_self_suspend_to(ABT_thread thread) { ABTI_UNIMPLEMENTED("ABT_self_suspend_to"); }
int ABT_self_resume_suspend_to(ABT_thread thread) { ABTI_UNIMPLEMENTED("ABT_self_resume_suspend_to"); }
int ABT_self_exit(void) { return ABT_thread_exit(); }
int ABT_self_exit_to(ABT_thread thread) { ABTI_UNIMPLEMENTED("ABT_self_exit_to"); }
int ABT_self_resume_exit_to(ABT_thread thread) { ABTI_UNIMPLEMENTED("ABT_self_resume_exit_to"); }
/* run `thread` as a child unit of the calling ULT; the caller resumes when
 * the child yields, blocks or finishes (Argobots: ABTI_ythread_schedule) */
int ABT_self_schedule(ABT_thread thread, ABT_pool pool) {
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  ABTI_thread *self = ABTI_self_thread();
  if (!ABTI_on_pe() || !self || !self->cth) return ABT_ERR_INV_THREAD; /* not on a ULT */
  if (t == self) return ABT_ERR_INV_THREAD;
  if (!ABTI_is_null_handle(pool)) {
    ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
    ABTI_pool_associate(t, p);
  }
  ABTI_pool_run_thread(t); /* records the caller as the child's parent: it returns here */
  return ABT_SUCCESS;
}
/* ---- tasklets: run as ULTs with a small stack. Argobots' tasklets cannot
 * yield or block; ours technically can, which only makes them more
 * permissive. Handles are ABT_thread handles (same opaque type). ---- */
/* A tasklet is a stackless work unit (Kale: a plain Converse handler): the
 * poller runs fn(arg) inline on its own stack when it pops the entry. It
 * cannot yield or block; the unit itself is the pool entry, no CthThread. */
static int task_create_impl(ABT_pool pool, void (*fn)(void *), void *arg, ABT_task *newtask) {
  ABTI_CHECK_INITIALIZED();
  ABTI_pool *p = ABTI_pool_get(pool);
  ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  ABTI_thread *t = ABTI_thread_alloc();
  t->cth = nullptr;
  t->type = newtask ? ABTI_THREAD_NAMED : ABTI_THREAD_DETACHED;
  t->fn = fn; t->arg = arg;
  t->attr = ABTI_thread_attr{0, nullptr, ABT_TRUE};
  t->id = ABTI_g->next_id.fetch_add(1);
  t->pool = nullptr; t->unit = ABT_UNIT_NULL;
  t->last_xstream = nullptr; t->migrate_to = nullptr;
  t->freed_by_exit = t->type == ABTI_THREAD_DETACHED;
  t->is_task = true; t->blocked_pool = nullptr;
  t->tstate.store(CTH_STATE_READY);
  ABTI_pool_associate(t, p);
  if (newtask) *newtask = ABTI_thread_handle(t);
  p->push(t); /* legal from any thread: no PE state involved */
  return ABT_SUCCESS;
}
static inline ABTI_thread *task_get(ABT_task h) { ABTI_thread *t = ABTI_thread_get(h); return (t && t->is_task) ? t : nullptr; }

int ABT_task_create(ABT_pool pool, void (*task_func)(void *), void *arg, ABT_task *newtask) {
  return task_create_impl(pool, task_func, arg, newtask);
}
int ABT_task_create_on_xstream(ABT_xstream xstream, void (*task_func)(void *), void *arg, ABT_task *newtask) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  if (!x->main_sched || x->main_sched->pools.empty()) return ABT_ERR_INV_XSTREAM;
  return task_create_impl(ABTI_pool_handle(x->main_sched->pools[0]), task_func, arg, newtask);
}
int ABT_task_revive(ABT_pool pool, void (*task_func)(void *), void *arg, ABT_task *task) { ABTI_UNIMPLEMENTED("ABT_task_revive"); }
int ABT_task_free(ABT_task *task) {
  ABTI_CHECK_NULL(task, ABT_ERR_INV_TASK);
  ABTI_thread *t = task_get(*task); ABTI_CHECK_NULL(t, ABT_ERR_INV_TASK);
  int r = ABT_thread_free(task); return r;
}
int ABT_task_join(ABT_task task) {
  ABTI_thread *t = task_get(task); ABTI_CHECK_NULL(t, ABT_ERR_INV_TASK);
  return ABT_thread_join(task);
}
int ABT_task_cancel(ABT_task task) { ABTI_UNIMPLEMENTED("ABT_task_cancel"); }
int ABT_task_self(ABT_task *task) {
  if (task) *task = ABT_TASK_NULL;
  ABTI_CHECK_INITIALIZED();
  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM;
  ABTI_thread *t = ABTI_self_thread();
  if (!t || !t->is_task) return ABT_ERR_INV_TASK; /* a ULT or external caller is not a task */
  *task = ABTI_thread_handle(t); return ABT_SUCCESS;
}
int ABT_task_self_id(ABT_unit_id *id) {
  ABTI_CHECK_INITIALIZED();
  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM;
  ABTI_thread *t = ABTI_self_thread();
  if (!t || !t->is_task) return ABT_ERR_INV_TASK;
  *id = t->id; return ABT_SUCCESS;
}
int ABT_task_get_xstream(ABT_task task, ABT_xstream *xstream) {
  ABTI_thread *t = task_get(task); ABTI_CHECK_NULL(t, ABT_ERR_INV_TASK);
  *xstream = ABTI_xstream_handle(t->last_xstream); return ABT_SUCCESS;
}
int ABT_task_get_state(ABT_task task, ABT_task_state *state) {
  ABTI_thread *t = task_get(task); ABTI_CHECK_NULL(t, ABT_ERR_INV_TASK);
  ABT_thread_state s; ABT_thread_get_state(task, &s);
  switch (s) {
  case ABT_THREAD_STATE_READY: *state = ABT_TASK_STATE_READY; break;
  case ABT_THREAD_STATE_RUNNING: *state = ABT_TASK_STATE_RUNNING; break;
  case ABT_THREAD_STATE_TERMINATED: *state = ABT_TASK_STATE_TERMINATED; break;
  default: *state = ABT_TASK_STATE_RUNNING; break; /* tasklets have no BLOCKED */
  }
  return ABT_SUCCESS;
}
int ABT_task_get_last_pool(ABT_task task, ABT_pool *pool) {
  ABTI_thread *t = task_get(task); ABTI_CHECK_NULL(t, ABT_ERR_INV_TASK);
  *pool = ABTI_pool_handle(t->pool); return ABT_SUCCESS;
}
int ABT_task_get_last_pool_id(ABT_task task, int *id) {
  ABTI_thread *t = task_get(task); ABTI_CHECK_NULL(t, ABT_ERR_INV_TASK);
  *id = t->pool ? (int)t->pool->id : -1; return ABT_SUCCESS;
}
int ABT_task_set_migratable(ABT_task task, ABT_bool flag) {
  ABTI_thread *t = task_get(task); ABTI_CHECK_NULL(t, ABT_ERR_INV_TASK);
  t->attr.migratable = flag; return ABT_SUCCESS;
}
int ABT_task_is_migratable(ABT_task task, ABT_bool *flag) {
  ABTI_thread *t = task_get(task); ABTI_CHECK_NULL(t, ABT_ERR_INV_TASK);
  *flag = t->attr.migratable; return ABT_SUCCESS;
}
int ABT_task_equal(ABT_task task1, ABT_task task2, ABT_bool *result) { *result = task1 == task2 ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS; }
int ABT_task_get_id(ABT_task task, ABT_unit_id *task_id) {
  ABTI_thread *t = task_get(task); ABTI_CHECK_NULL(t, ABT_ERR_INV_TASK);
  *task_id = t->id; return ABT_SUCCESS;
}
int ABT_task_get_arg(ABT_task task, void **arg) {
  ABTI_thread *t = task_get(task); ABTI_CHECK_NULL(t, ABT_ERR_INV_TASK);
  *arg = t->arg; return ABT_SUCCESS;
}

} /* extern "C" */
