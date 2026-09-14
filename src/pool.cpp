/* Pools: built-in FIFO (one implementation for every kind and access mode,
 * as in Argobots) and user-defined pools (ABT_pool_def, Margo's prio_wait
 * and earliest_first). A pool holds ABTI_thread*; the thread's token (a
 * Converse message) is what the scheduler hands to CmiHandleMessage. */
#include "abti.h"
#include <chrono>
#include <algorithm>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

static ABTI_thread *unit_to_thread(ABT_unit u) {
  if (ABTI_is_null_handle(u)) return nullptr;
  std::lock_guard<std::mutex> g(ABTI_g->um);
  auto it = ABTI_g->units.find((void *)u);
  return it == ABTI_g->units.end() ? nullptr : it->second;
}

void ABTI_pool::push(ABTI_thread *t) {
  if (user) {
    def.p_push(ABTI_pool_handle(this), t->unit);
  } else {
    std::lock_guard<std::mutex> g(m);
    q.push_back(t);
  }
  if (waiters.load(std::memory_order_acquire) > 0) {
    std::lock_guard<std::mutex> g(m);
    cv.notify_one();
  }
  {
    std::lock_guard<std::mutex> g(sm);
    for (int r : sleepers) CsdIdleNotify(r);
  }
}

void ABTI_pool::add_sleeper(int rank) { std::lock_guard<std::mutex> g(sm); sleepers.push_back(rank); }
void ABTI_pool::remove_sleeper(int rank) {
  std::lock_guard<std::mutex> g(sm);
  for (size_t i = 0; i < sleepers.size(); i++) if (sleepers[i] == rank) { sleepers.erase(sleepers.begin() + i); break; }
}

ABTI_thread *ABTI_pool::pop() {
  if (user) return unit_to_thread(def.p_pop(ABTI_pool_handle(this)));
  std::lock_guard<std::mutex> g(m);
  if (q.empty()) return nullptr;
  ABTI_thread *t = q.front();
  q.pop_front();
  return t;
}

ABTI_thread *ABTI_pool::pop_timedwait(double abs_deadline) {
  if (user && def.p_pop_timedwait)
    return unit_to_thread(def.p_pop_timedwait(ABTI_pool_handle(this), abs_deadline));
  ABTI_thread *t = pop();
  if (t) return t;
  double now = CmiWallTimer();
  if (abs_deadline <= now) return nullptr;
  auto dur = std::chrono::duration<double>(abs_deadline - now);
  std::unique_lock<std::mutex> g(m);
  waiters.fetch_add(1, std::memory_order_acq_rel);
  if (user) {
    cv.wait_for(g, dur); /* push notifies; re-check through the user's pop */
    waiters.fetch_sub(1, std::memory_order_acq_rel);
    g.unlock();
    return pop();
  }
  cv.wait_for(g, dur, [this] { return !q.empty(); });
  waiters.fetch_sub(1, std::memory_order_acq_rel);
  if (q.empty()) return nullptr;
  t = q.front();
  q.pop_front();
  return t;
}

size_t ABTI_pool::size() {
  if (user) return def.p_get_size(ABTI_pool_handle(this));
  std::lock_guard<std::mutex> g(m);
  return q.size();
}

ABTI_pool *ABTI_pool_create_builtin(ABT_pool_kind kind, ABT_pool_access access, ABT_bool automatic) {
  ABTI_pool *p = new ABTI_pool();
  p->kind = kind;
  p->access = access;
  p->automatic = automatic;
  p->id = ABTI_g->next_id.fetch_add(1);
  p->user = false;
  p->data = nullptr;
  return p;
}

void ABTI_pool_destroy(ABTI_pool *p) {
  if (p->user && p->def.p_free) p->def.p_free(ABTI_pool_handle(p));
  delete p;
}

void ABTI_pool_disassociate(ABTI_thread *t) {
  if (t->pool && t->pool->user && !ABTI_is_null_handle(t->unit)) {
    {
      std::lock_guard<std::mutex> g(ABTI_g->um);
      ABTI_g->units.erase((void *)t->unit);
    }
    t->pool->def.u_free(&t->unit);
  }
  t->unit = ABT_UNIT_NULL;
  t->pool = nullptr;
}

void ABTI_pool_associate(ABTI_thread *t, ABTI_pool *p) {
  if (t->pool == p) return;
  ABTI_pool_disassociate(t);
  t->pool = p;
  if (p->user) {
    ABT_unit u = p->def.u_create_from_thread(ABTI_thread_handle(t));
    if (ABTI_is_null_handle(u)) CmiAbort("abt: u_create_from_thread returned ABT_UNIT_NULL\n");
    t->unit = u;
    std::lock_guard<std::mutex> g(ABTI_g->um);
    ABTI_g->units[(void *)u] = t;
  }
}

void ABTI_pool_run_thread(ABTI_thread *t) {
  t->last_xstream = ABTI_tls_xstream;
  CmiHandleMessage(CthGetToken(t->cth));
}

int ABTI_poll_pool(void *ctx) {
  ABTI_pool *p = static_cast<ABTI_pool *>(ctx);
  ABTI_thread *t = p->pop();
  if (!t) return 0;
  CsdReleaseIdle();
  ABTI_pool_run_thread(t);
  return 1;
}

extern "C" {

const ABT_pool_config_var ABT_pool_config_automatic = { -2, ABT_POOL_CONFIG_INT };

int ABT_pool_create_basic(ABT_pool_kind kind, ABT_pool_access access, ABT_bool automatic, ABT_pool *newpool) {
  ABTI_CHECK_INITIALIZED();
  ABTI_CHECK_NULL(newpool, ABT_ERR_INV_ARG);
  if (kind != ABT_POOL_FIFO && kind != ABT_POOL_FIFO_WAIT && kind != ABT_POOL_RANDWS) return ABT_ERR_INV_POOL_KIND;
  if (access < ABT_POOL_ACCESS_PRIV || access > ABT_POOL_ACCESS_MPMC) return ABT_ERR_INV_POOL_ACCESS;
  *newpool = ABTI_pool_handle(ABTI_pool_create_builtin(kind, access, automatic));
  return ABT_SUCCESS;
}

int ABT_pool_create(ABT_pool_user_def def, ABT_pool_config config, ABT_pool *newpool) {
  ABTI_CHECK_INITIALIZED();
  ABTI_CHECK_NULL(def, ABT_ERR_INV_POOL);
  ABTI_CHECK_NULL(newpool, ABT_ERR_INV_ARG);
  /* legacy ABT_pool_def (the only kind this build accepts; the new-style
   * definition object would have a NULL first word where 'access' sits) */
  const ABT_pool_def *d = def;
  if (!d->u_create_from_thread || !d->u_free || !d->p_get_size || !d->p_push || !d->p_pop) return ABT_ERR_INV_POOL;
  ABTI_pool *p = new ABTI_pool();
  p->user = true;
  p->def = *d;
  p->access = d->access;
  p->kind = ABT_POOL_FIFO;
  p->automatic = ABT_FALSE;
  p->id = ABTI_g->next_id.fetch_add(1);
  p->data = nullptr;
  if (d->p_init) {
    int ret = d->p_init(ABTI_pool_handle(p), config);
    if (ret != ABT_SUCCESS) { delete p; return ret; }
  }
  *newpool = ABTI_pool_handle(p);
  return ABT_SUCCESS;
}

int ABT_pool_free(ABT_pool *pool) {
  ABTI_CHECK_INITIALIZED();
  ABTI_CHECK_NULL(pool, ABT_ERR_INV_POOL);
  ABTI_pool *p = ABTI_pool_get(*pool);
  ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  ABTI_pool_destroy(p);
  *pool = ABT_POOL_NULL;
  return ABT_SUCCESS;
}

int ABT_pool_get_access(ABT_pool pool, ABT_pool_access *access) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  *access = p->access; return ABT_SUCCESS;
}
int ABT_pool_is_empty(ABT_pool pool, ABT_bool *is_empty) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  *is_empty = p->size() == 0 ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
}
int ABT_pool_get_size(ABT_pool pool, size_t *size) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  *size = p->size(); return ABT_SUCCESS;
}
int ABT_pool_get_total_size(ABT_pool pool, size_t *size) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  long b = p->num_blocked.load(std::memory_order_relaxed);
  *size = p->size() + (b > 0 ? (size_t)b : 0); return ABT_SUCCESS;
}
int ABT_pool_set_data(ABT_pool pool, void *data) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  p->data = data; return ABT_SUCCESS;
}
int ABT_pool_get_data(ABT_pool pool, void **data) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  *data = p->data; return ABT_SUCCESS;
}
int ABT_pool_add_sched(ABT_pool pool, ABT_sched sched) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  p->num_scheds.fetch_add(1); return ABT_SUCCESS;
}
int ABT_pool_get_id(ABT_pool pool, int *id) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  *id = (int)p->id; return ABT_SUCCESS;
}

/* unit-level access (custom schedulers, Thallium). In a built-in pool a
 * unit is the thread handle itself; in a user pool it is the user's unit. */
static ABT_unit thread_to_unit(ABTI_thread *t) {
  if (!t) return ABT_UNIT_NULL;
  return t->pool && t->pool->user ? t->unit : (ABT_unit)t;
}
static ABTI_thread *any_unit_to_thread(ABT_unit u) {
  ABTI_thread *t = unit_to_thread(u);
  return t ? t : reinterpret_cast<ABTI_thread *>(u);
}

int ABT_pool_pop(ABT_pool pool, ABT_unit *p_unit) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  *p_unit = thread_to_unit(p->pop()); return ABT_SUCCESS;
}
int ABT_pool_pop_wait(ABT_pool pool, ABT_unit *p_unit, double time_secs) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  *p_unit = thread_to_unit(p->pop_timedwait(CmiWallTimer() + time_secs)); return ABT_SUCCESS;
}
int ABT_pool_pop_timedwait(ABT_pool pool, ABT_unit *p_unit, double abstime_secs) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  *p_unit = thread_to_unit(p->pop_timedwait(abstime_secs)); return ABT_SUCCESS;
}
int ABT_pool_push(ABT_pool pool, ABT_unit unit) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  ABTI_thread *t = any_unit_to_thread(unit); ABTI_CHECK_NULL(t, ABT_ERR_INV_UNIT);
  if (t->pool != p) ABTI_pool_associate(t, p);
  p->push(t); return ABT_SUCCESS;
}
int ABT_pool_remove(ABT_pool pool, ABT_unit unit) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  if (p->user) { if (p->def.p_remove) p->def.p_remove(pool, unit); return ABT_SUCCESS; }
  ABTI_thread *t = reinterpret_cast<ABTI_thread *>(unit);
  std::lock_guard<std::mutex> g(p->m);
  auto it = std::find(p->q.begin(), p->q.end(), t);
  if (it != p->q.end()) p->q.erase(it);
  return ABT_SUCCESS;
}
int ABT_pool_print_all(ABT_pool pool, void *arg, void (*print_fn)(void *, ABT_unit)) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  if (p->user) { if (p->def.p_print_all) p->def.p_print_all(pool, arg, print_fn); return ABT_SUCCESS; }
  std::lock_guard<std::mutex> g(p->m);
  for (ABTI_thread *t : p->q) print_fn(arg, (ABT_unit)t);
  return ABT_SUCCESS;
}
int ABT_pool_pop_thread(ABT_pool pool, ABT_thread *thread) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  *thread = ABTI_thread_handle(p->pop()); return ABT_SUCCESS;
}
int ABT_pool_pop_thread_ex(ABT_pool pool, ABT_thread *thread, ABT_pool_context ctx) {
  return ABT_pool_pop_thread(pool, thread);
}
int ABT_pool_pop_threads(ABT_pool pool, ABT_thread *threads, size_t max, size_t *num) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  size_t n = 0;
  while (n < max) { ABTI_thread *t = p->pop(); if (!t) break; threads[n++] = ABTI_thread_handle(t); }
  *num = n; return ABT_SUCCESS;
}
int ABT_pool_pop_threads_ex(ABT_pool pool, ABT_thread *threads, size_t max, size_t *num, ABT_pool_context ctx) {
  return ABT_pool_pop_threads(pool, threads, max, num);
}
int ABT_pool_push_thread(ABT_pool pool, ABT_thread thread) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
  if (t->pool != p) ABTI_pool_associate(t, p);
  p->push(t); return ABT_SUCCESS;
}
int ABT_pool_push_thread_ex(ABT_pool pool, ABT_thread thread, ABT_pool_context ctx) {
  return ABT_pool_push_thread(pool, thread);
}
int ABT_pool_push_threads(ABT_pool pool, const ABT_thread *threads, size_t num) {
  for (size_t i = 0; i < num; i++) { int r = ABT_pool_push_thread(pool, threads[i]); if (r != ABT_SUCCESS) return r; }
  return ABT_SUCCESS;
}
int ABT_pool_push_threads_ex(ABT_pool pool, const ABT_thread *threads, size_t num, ABT_pool_context ctx) {
  return ABT_pool_push_threads(pool, threads, num);
}
int ABT_pool_pop_wait_thread(ABT_pool pool, ABT_thread *thread, double time_secs) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  *thread = ABTI_thread_handle(p->pop_timedwait(CmiWallTimer() + time_secs)); return ABT_SUCCESS;
}
int ABT_pool_pop_wait_thread_ex(ABT_pool pool, ABT_thread *thread, double time_secs, ABT_pool_context ctx) {
  return ABT_pool_pop_wait_thread(pool, thread, time_secs);
}
int ABT_pool_print_all_threads(ABT_pool pool, void *arg, void (*print_f)(void *, ABT_thread)) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  if (p->user) return ABT_ERR_FEATURE_NA;
  std::lock_guard<std::mutex> g(p->m);
  for (ABTI_thread *t : p->q) print_f(arg, ABTI_thread_handle(t));
  return ABT_SUCCESS;
}
int ABT_unit_get_thread(ABT_unit unit, ABT_thread *thread) {
  if (ABTI_is_null_handle(unit)) { *thread = ABT_THREAD_NULL; return ABT_SUCCESS; }
  *thread = ABTI_thread_handle(any_unit_to_thread(unit)); return ABT_SUCCESS;
}
int ABT_unit_set_associated_pool(ABT_unit unit, ABT_pool pool) { ABTI_UNIMPLEMENTED("ABT_unit_set_associated_pool"); }

/* configuration objects and the new-style user definition: not supported */
int ABT_pool_config_create(ABT_pool_config *config) { ABTI_UNIMPLEMENTED("ABT_pool_config_create"); }
int ABT_pool_config_free(ABT_pool_config *config) { ABTI_UNIMPLEMENTED("ABT_pool_config_free"); }
int ABT_pool_config_set(ABT_pool_config config, int key, ABT_pool_config_type type, const void *val) { ABTI_UNIMPLEMENTED("ABT_pool_config_set"); }
int ABT_pool_config_get(ABT_pool_config config, int key, ABT_pool_config_type *type, void *val) { ABTI_UNIMPLEMENTED("ABT_pool_config_get"); }
int ABT_pool_user_def_create(ABT_pool_user_create_unit_fn p_create_unit, ABT_pool_user_free_unit_fn p_free_unit, ABT_pool_user_is_empty_fn p_is_empty, ABT_pool_user_pop_fn p_pop, ABT_pool_user_push_fn p_push, ABT_pool_user_def *newdef) { ABTI_UNIMPLEMENTED("ABT_pool_user_def_create"); }
int ABT_pool_user_def_free(ABT_pool_user_def *def) { ABTI_UNIMPLEMENTED("ABT_pool_user_def_free"); }
int ABT_pool_user_def_set_init(ABT_pool_user_def def, ABT_pool_user_init_fn p_init) { ABTI_UNIMPLEMENTED("ABT_pool_user_def_set_init"); }
int ABT_pool_user_def_set_free(ABT_pool_user_def def, ABT_pool_user_free_fn p_free) { ABTI_UNIMPLEMENTED("ABT_pool_user_def_set_free"); }
int ABT_pool_user_def_set_get_size(ABT_pool_user_def def, ABT_pool_user_get_size_fn p_get_size) { ABTI_UNIMPLEMENTED("ABT_pool_user_def_set_get_size"); }
int ABT_pool_user_def_set_pop_wait(ABT_pool_user_def def, ABT_pool_user_pop_wait_fn p_pop_wait) { ABTI_UNIMPLEMENTED("ABT_pool_user_def_set_pop_wait"); }
int ABT_pool_user_def_set_pop_many(ABT_pool_user_def def, ABT_pool_user_pop_many_fn p_pop_many) { ABTI_UNIMPLEMENTED("ABT_pool_user_def_set_pop_many"); }
int ABT_pool_user_def_set_push_many(ABT_pool_user_def def, ABT_pool_user_push_many_fn p_push_many) { ABTI_UNIMPLEMENTED("ABT_pool_user_def_set_push_many"); }
int ABT_pool_user_def_set_print_all(ABT_pool_user_def def, ABT_pool_user_print_all_fn p_print_all) { ABTI_UNIMPLEMENTED("ABT_pool_user_def_set_print_all"); }

} /* extern "C" */
