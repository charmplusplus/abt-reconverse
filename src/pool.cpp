/* Pools: built-in FIFO (one implementation for every kind and access mode,
 * as in Argobots) and user-defined pools (ABT_pool_def, Margo's prio_wait
 * and earliest_first). A pool holds ABTI_thread*; the thread's token (a
 * Converse message) is what the scheduler hands to CmiHandleMessage. */
#include "abti.h"
#include <chrono>
#include <algorithm>
#include <map>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

static ABTI_thread *unit_to_thread(ABT_unit u) {
  if (ABTI_is_null_handle(u)) return nullptr;
  if (ABTI_g->num_user_pools.load(std::memory_order_acquire) == 0) return nullptr; /* no user pool: units are thread pointers */
  std::lock_guard<std::mutex> g(ABTI_g->um);
  auto it = ABTI_g->units.find((void *)u);
  return it == ABTI_g->units.end() ? nullptr : it->second;
}

/* The primary ULT is a member of its pool like any other ULT (FIFO order
 * matters: Margo's monitoring test expects a handler woken earlier to run
 * before the primary), but its token can only be resumed by its home PE. */
static inline bool pinned_elsewhere(ABTI_thread *t) {
  return t->type == ABTI_THREAD_PRIMARY && CthGetHomeRank(t->cth) != CmiMyRank();
}

void ABTI_pool::add_sleeper(int rank) {
  smask[rank >> 6].fetch_or(1ull << (rank & 63), std::memory_order_acq_rel);
  nsleepers.fetch_add(1, std::memory_order_seq_cst);
}
void ABTI_pool::remove_sleeper(int rank) {
  smask[rank >> 6].fetch_and(~(1ull << (rank & 63)), std::memory_order_acq_rel);
  nsleepers.fetch_sub(1, std::memory_order_seq_cst);
}

/* After the unit is visible (count incremented, seq_cst): a parked timed pop
 * and any PE parked in CsdIdleWait for this pool are woken. Both waiters
 * publish themselves (waiters / nsleepers, seq_cst) before re-checking
 * count, so either they see the unit or we see them. Under load neither
 * counter is set and this is two loads. */
void ABTI_pool::wake_after_push() {
  if (waiters.load(std::memory_order_seq_cst) > 0) {
    std::lock_guard<std::mutex> g(wm);
    cv.notify_one();
  }
  if (nsleepers.load(std::memory_order_seq_cst) > 0) {
    for (int w = 0; w < 4; w++) {
      uint64_t m = smask[w].load(std::memory_order_acquire);
      while (m) { CsdIdleNotify(w * 64 + __builtin_ctzll(m)); m &= m - 1; }
    }
  }
}

void ABTI_pool::push(ABTI_thread *t) {
  if (user) {
    def.p_push(ABTI_pool_handle(this), t->unit);
    wake_after_push();
    return;
  }
  if (t->in_pool.exchange(true, std::memory_order_acq_rel))
    CmiAbort("abt: a unit was pushed into a pool it is already queued in "
             "(a ULT made ready twice, or re-pushed after ABT_pool_remove on an MPSC pool)\n");
  if (single_consumer) {
    t->qn.next.store(nullptr, std::memory_order_relaxed);
    ABTI_qnode *prev = tail.exchange(&t->qn, std::memory_order_acq_rel);
    prev->next.store(&t->qn, std::memory_order_release);
  } else {
    lk.lock();
    t->lnext = nullptr;
    t->lprev = ltail;
    if (ltail) ltail->lnext = t; else lhead = t;
    ltail = t;
    lk.unlock();
  }
  count.fetch_add(1, std::memory_order_seq_cst);
  wake_after_push();
}

void ABTI_pool::list_unlink(ABTI_thread *t) {
  if (t->lprev) t->lprev->lnext = t->lnext; else lhead = t->lnext;
  if (t->lnext) t->lnext->lprev = t->lprev; else ltail = t->lprev;
  t->lprev = t->lnext = nullptr;
}

ABTI_thread *ABTI_pool::pop_list() {
  lk.lock();
  ABTI_thread *t = lhead;
  if (t && pinned_elsewhere(t)) t = t->lnext; /* only one primary exists */
  if (t) {
    list_unlink(t);
    t->in_pool.store(false, std::memory_order_release);
  }
  lk.unlock();
  if (t) count.fetch_sub(1, std::memory_order_seq_cst);
  return t;
}

/* Vyukov's intrusive MPSC queue: producers exchange the tail and link, the
 * consumer walks from head. A pop that finds a producer between its exchange
 * and its link returns NULL; the unit is seen at the next poll. */
ABTI_thread *ABTI_pool::pop_mpsc() {
  lk.lock();
  for (;;) {
    ABTI_qnode *h = head;
    ABTI_qnode *next = h->next.load(std::memory_order_acquire);
    if (h == &stub) {
      if (!next) { lk.unlock(); return nullptr; }
      head = h = next;
      next = next->next.load(std::memory_order_acquire);
    }
    if (!next) {
      if (h != tail.load(std::memory_order_acquire)) { lk.unlock(); return nullptr; }
      /* h is the last node: re-append the stub so h can be detached */
      stub.next.store(nullptr, std::memory_order_relaxed);
      ABTI_qnode *prev = tail.exchange(&stub, std::memory_order_acq_rel);
      prev->next.store(&stub, std::memory_order_release);
      next = h->next.load(std::memory_order_acquire);
      if (!next) { lk.unlock(); return nullptr; }
    }
    head = next;
    ABTI_thread *t = h->owner;
    t->in_pool.store(false, std::memory_order_release);
    if (t->removed.exchange(false, std::memory_order_acq_rel)) continue; /* ABT_pool_remove: count already adjusted */
    lk.unlock();
    count.fetch_sub(1, std::memory_order_seq_cst);
    if (pinned_elsewhere(t)) {
      /* a single-consumer pool polled by a PE that is not the primary's home:
       * outside Argobots' rules; keep the unit for its home PE and report
       * empty for this poll (no livelock: the poller goes idle as usual) */
      push(t);
      return nullptr;
    }
    return t;
  }
}

ABTI_thread *ABTI_pool::pop() {
  if (user) {
    ABTI_thread *t = unit_to_thread(def.p_pop(ABTI_pool_handle(this)));
    if (t && pinned_elsewhere(t)) {
      /* not ours: put it back and take the next one, if any */
      def.p_push(ABTI_pool_handle(this), t->unit);
      ABTI_thread *n = unit_to_thread(def.p_pop(ABTI_pool_handle(this)));
      if (n && pinned_elsewhere(n)) { def.p_push(ABTI_pool_handle(this), n->unit); return nullptr; }
      return n;
    }
    return t;
  }
  return single_consumer ? pop_mpsc() : pop_list();
}

ABTI_thread *ABTI_pool::pop_timedwait(double abs_deadline) {
  if (user && def.p_pop_timedwait)
    return unit_to_thread(def.p_pop_timedwait(ABTI_pool_handle(this), abs_deadline));
  ABTI_thread *t = pop();
  if (t) return t;
  double now = CmiWallTimer();
  if (abs_deadline <= now) return nullptr;
  auto dur = std::chrono::duration<double>(abs_deadline - now);
  {
    std::unique_lock<std::mutex> g(wm);
    waiters.fetch_add(1, std::memory_order_seq_cst);
    if (user) cv.wait_for(g, dur); /* push notifies; re-check through the user's pop */
    else if (count.load(std::memory_order_seq_cst) == 0)
      cv.wait_for(g, dur, [this] { return count.load(std::memory_order_seq_cst) > 0; });
    waiters.fetch_sub(1, std::memory_order_seq_cst);
  }
  return pop();
}

size_t ABTI_pool::size() {
  if (user) return def.p_get_size(ABTI_pool_handle(this));
  long c = count.load(std::memory_order_seq_cst);
  return c > 0 ? (size_t)c : 0;
}

bool ABTI_pool::remove_unit(ABTI_thread *t) {
  bool hit = false;
  lk.lock();
  if (t->in_pool.load(std::memory_order_acquire) && t->pool == this) {
    if (single_consumer) {
      /* cannot unlink under producers: mark it, pop skips it */
      if (!t->removed.exchange(true, std::memory_order_acq_rel)) hit = true;
    } else {
      list_unlink(t);
      t->in_pool.store(false, std::memory_order_release);
      hit = true;
    }
  }
  lk.unlock();
  if (hit) count.fetch_sub(1, std::memory_order_seq_cst);
  return hit;
}

ABTI_pool *ABTI_pool_create_builtin(ABT_pool_kind kind, ABT_pool_access access, ABT_bool automatic) {
  ABTI_pool *p = new ABTI_pool();
  p->kind = kind;
  p->access = access;
  p->automatic = automatic;
  p->id = ABTI_g->next_id.fetch_add(1);
  p->user = false;
  p->data = nullptr;
  p->single_consumer = access <= ABT_POOL_ACCESS_MPSC;
  return p;
}

void ABTI_pool_destroy(ABTI_pool *p) {
  if (p->user && p->def.p_free) p->def.p_free(ABTI_pool_handle(p));
  /* the count stays raised while any unit of this pool may still be looked
   * up; a freed pool's units are freed with it (ABTI_pool_disassociate), so
   * decrementing here is safe only once no thread references them -- keep
   * it conservative: never decrement (a process with user pools keeps the
   * map path; DAOS never creates one) */
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

thread_local ABTI_thread *ABTI_tls_task = nullptr;

void ABTI_pool_run_thread(ABTI_thread *t) {
  if (t->type == ABTI_THREAD_PRIMARY) ABTI_DBG("run primary");
  t->last_xstream = ABTI_tls_xstream;
  /* resumed by a ULT (a scheduler runner, a stacked scheduler, an
   * ABT_self_schedule caller): come back to it; resumed by the PE's own
   * loop: come back to the PE's scheduling thread */
  ABTI_thread *caller = ABTI_self_thread();
  t->parent = (caller && caller->cth && !caller->is_task) ? caller->cth : nullptr;
  if (!t->cth) {
    /* a tasklet: run inline on this stack, then retire it */
    ABTI_thread *prev = ABTI_tls_task;
    ABTI_tls_task = t;
    t->tstate.store(CTH_STATE_RUNNING);
    t->fn(t->arg);
    t->tstate.store(CTH_STATE_TERMINATED);
    ABTI_tls_task = prev;
    ABTI_thread_terminated(t);
    return;
  }
  CmiHandleMessage(CthGetToken(t->cth));
}

/* strict priority: a pool yields nothing while any higher-priority pool of
 * its scheduler still holds queued work (Argobots' prio scheduler) */
int ABTI_poll_pool_prio(void *ctx) {
  ABTI_sched::PrioCtx *pc = static_cast<ABTI_sched::PrioCtx *>(ctx);
  for (int i = 0; i < pc->idx; i++)
    if (pc->sched->pools[i]->size() != 0) return 0;
  return ABTI_poll_pool(pc->sched->pools[pc->idx]);
}

int ABTI_poll_pool(void *ctx) {
  ABTI_pool *p = static_cast<ABTI_pool *>(ctx);
  ABTI_thread *t = p->pop();
  if (!t) return 0;
  CsdReleaseIdle();
  ABTI_pool_run_thread(t);
  return 1;
}

/* ---- pool configuration objects ----------------------------------------
 * Keys are ABT_pool_config_var::key values; the predefined
 * ABT_pool_config_automatic uses -2, user-defined keys are the caller's. */
namespace {
struct ABTI_pool_config_val {
  ABT_pool_config_type type;
  int i;
  double d;
  const void *p;
};
} // namespace

struct ABTI_pool_config {
  std::map<int, ABTI_pool_config_val> vals;
};

static ABTI_pool_config *PC(ABT_pool_config h) { return ABTI_obj<ABTI_pool_config>(h); }

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
  ABTI_g->num_user_pools.fetch_add(1, std::memory_order_acq_rel);
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
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  return ABTI_sched_add_to_pool(s, p); /* the scheduler becomes a ULT unit of pool */
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
  p->remove_unit(t);
  return ABT_SUCCESS;
}
int ABT_pool_print_all(ABT_pool pool, void *arg, void (*print_fn)(void *, ABT_unit)) {
  ABTI_pool *p = ABTI_pool_get(pool); ABTI_CHECK_NULL(p, ABT_ERR_INV_POOL);
  if (p->user) { if (p->def.p_print_all) p->def.p_print_all(pool, arg, print_fn); return ABT_SUCCESS; }
  p->for_each_unit([&](ABTI_thread *t) { print_fn(arg, (ABT_unit)t); });
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
  p->for_each_unit([&](ABTI_thread *t) { print_f(arg, ABTI_thread_handle(t)); });
  return ABT_SUCCESS;
}
int ABT_unit_get_thread(ABT_unit unit, ABT_thread *thread) {
  if (ABTI_is_null_handle(unit)) { *thread = ABT_THREAD_NULL; return ABT_SUCCESS; }
  *thread = ABTI_thread_handle(any_unit_to_thread(unit)); return ABT_SUCCESS;
}
int ABT_unit_set_associated_pool(ABT_unit unit, ABT_pool pool) { ABTI_UNIMPLEMENTED("ABT_unit_set_associated_pool"); }

/*
 * Pool configuration objects: a typed key/value map, the same shape as
 * ABT_sched_config in src/sched.cpp but keyed by ABT_pool_config_var::key and
 * with no variadic constructor.  ABT_pool_create does not consume one yet.
 */
int ABT_pool_config_create(ABT_pool_config *config) {
  if (!config) return ABT_ERR_INV_POOL_CONFIG;
  *config = reinterpret_cast<ABT_pool_config>(new ABTI_pool_config());
  return ABT_SUCCESS;
}
int ABT_pool_config_free(ABT_pool_config *config) {
  if (!config) return ABT_ERR_INV_POOL_CONFIG;
  ABTI_pool_config *c = PC(*config);
  if (!c) return ABT_ERR_INV_POOL_CONFIG;
  delete c;
  *config = ABT_POOL_CONFIG_NULL;
  return ABT_SUCCESS;
}
/* val == NULL deletes the entry */
int ABT_pool_config_set(ABT_pool_config config, int key, ABT_pool_config_type type, const void *val) {
  ABTI_pool_config *c = PC(config);
  if (!c) return ABT_ERR_INV_POOL_CONFIG;
  if (!val) { c->vals.erase(key); return ABT_SUCCESS; }
  ABTI_pool_config_val v{};
  v.type = type;
  if (type == ABT_POOL_CONFIG_INT) v.i = *(const int *)val;
  else if (type == ABT_POOL_CONFIG_DOUBLE) v.d = *(const double *)val;
  else v.p = *(void *const *)val;
  c->vals[key] = v;
  return ABT_SUCCESS;
}
/* an unset key is an error and must leave both outputs untouched */
int ABT_pool_config_get(ABT_pool_config config, int key, ABT_pool_config_type *type, void *val) {
  ABTI_pool_config *c = PC(config);
  if (!c) return ABT_ERR_INV_POOL_CONFIG;
  auto it = c->vals.find(key);
  if (it == c->vals.end()) return ABT_ERR_INV_POOL_CONFIG;
  if (type) *type = it->second.type;
  if (val) {
    if (it->second.type == ABT_POOL_CONFIG_INT) *(int *)val = it->second.i;
    else if (it->second.type == ABT_POOL_CONFIG_DOUBLE) *(double *)val = it->second.d;
    else *(const void **)val = it->second.p;
  }
  return ABT_SUCCESS;
}

/* the new-style user definition object: still not supported */
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
