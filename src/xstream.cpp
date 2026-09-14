/* Execution streams = leased reconverse PEs. Rank r runs on PE r; the
 * scheduler's pools become PE r's poll table; join means "finish once the
 * pools drain", completed by the PE's own idle hook. */
#include "abti.h"
#include <cstring>
#include <sched.h>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

thread_local ABTI_xstream *ABTI_tls_xstream = nullptr;

struct ABTI_pe_msg {
  char hdr[CmiMsgHeaderSizeBytes];
  ABTI_xstream *xs;
  int op;
  int cpuid;
};
enum { ABTI_OP_LEASE = 1, ABTI_OP_AFFINITY = 2 };

static void send_pe_msg(int rank, ABTI_xstream *xs, int op, int cpuid, int handler) {
  ABTI_pe_msg *m = (ABTI_pe_msg *)CmiAlloc(sizeof(ABTI_pe_msg));
  CmiInitMsgHeader(m, (int)sizeof(ABTI_pe_msg));
  CmiSetHandler(m, handler);
  m->xs = xs; m->op = op; m->cpuid = cpuid;
  CmiPushPE(rank, m);
}

/* runs on the target PE */
static void lease_handler(void *vm) {
  ABTI_pe_msg *m = (ABTI_pe_msg *)vm;
  ABTI_tls_xstream = m->xs;
  m->xs->state.store(ABT_XSTREAM_STATE_RUNNING);
  CsdSetSleepOnIdle(0); /* a leased PE spins or blocks on its pool, as Argobots does */
  if (m->xs->cpubind >= 0) CmiSetCPUAffinity(m->xs->cpubind);
  CmiFree(m);
}
static void affinity_handler(void *vm) {
  ABTI_pe_msg *m = (ABTI_pe_msg *)vm;
  if (m->cpuid >= 0) CmiSetCPUAffinity(m->cpuid);
  CmiFree(m);
}

void ABTI_register_handlers() {
  int l = CmiRegisterHandler((CmiHandler)lease_handler);
  int a = CmiRegisterHandler((CmiHandler)affinity_handler);
  int e = -1;
  if (!ABTI_g->lease_handler.compare_exchange_strong(e, l) && e != l) CmiAbort("abt: handler indices differ across PEs\n");
  e = -1;
  if (!ABTI_g->affinity_handler.compare_exchange_strong(e, a) && e != a) CmiAbort("abt: handler indices differ across PEs\n");
}

static void unlock_mutex(void *m) { static_cast<std::mutex *>(m)->unlock(); }

/* Argobots' rule (ABTI_sched_has_unit): blocked ULTs keep an xstream alive
 * only for pools this scheduler owns alone; a pool shared with other
 * schedulers may hold blocked ULTs that belong to them (the joiner itself,
 * typically), so only queued work counts there. */
static bool pools_drained(ABTI_sched *s) {
  for (ABTI_pool *p : s->pools) {
    if (p->size() != 0) return false;
    if (p->num_scheds.load() <= 1 && p->num_blocked.load(std::memory_order_acquire) != 0) return false;
  }
  return true;
}

/* CcdPROCESSOR_STILL_IDLE on every PE */
void ABTI_xstream_idle_hook(void *) {
  ABTI_xstream *xs = ABTI_tls_xstream;
  if (!xs) return;
  if (xs->finishing.load(std::memory_order_acquire) && pools_drained(xs->main_sched)) {
    /* release the lease: default table, park policy, wake joiners */
    CsdSchedTableInstall(CmiMyRank(), CsdSchedTableCreate(nullptr, 0));
    CsdSetSleepOnIdle(1);
    ABTI_tls_xstream = nullptr;
    xs->main_sched->used_by = nullptr;
    xs->state.store(ABT_XSTREAM_STATE_TERMINATED);
    std::vector<CthThread> joiners;
    {
      std::lock_guard<std::mutex> g(xs->jm);
      xs->finished.store(1, std::memory_order_release);
      joiners.swap(xs->joiners);
    }
    for (CthThread j : joiners) CthAwakenIfBlocked(j);
    return;
  }
  if (xs->main_sched->predef == ABT_SCHED_BASIC_WAIT && !xs->main_sched->pools.empty()) {
    /* Argobots' basic_wait blocks on pools[0] when nothing is runnable. A
     * user pool with its own timed pop blocks inside it (short deadline so
     * runtime messages to this PE are not delayed); a built-in pool parks
     * the PE in CsdIdleWait, which a push to the pool (sleeper list) or to
     * any of the PE's own queues ends. */
    ABTI_pool *p0 = xs->main_sched->pools[0];
    ABTI_thread *t;
    if (p0->user && p0->def.p_pop_timedwait) {
      t = p0->pop_timedwait(CmiWallTimer() + 0.002);
    } else {
      int rank = CmiMyRank();
      p0->add_sleeper(rank);
      t = p0->pop();
      if (!t) { CsdIdleWait(0.010); t = p0->pop(); }
      p0->remove_sleeper(rank);
    }
    if (t) { CsdReleaseIdle(); ABTI_pool_run_thread(t); }
  }
}

void ABTI_xstream_install(ABTI_xstream *xs) {
  /* lease first, table second: both travel through the PE's queue in order,
   * so the PE knows its xstream before the new table can pop a ULT */
  send_pe_msg(xs->rank, xs, ABTI_OP_LEASE, xs->cpubind, ABTI_g->lease_handler);
  CsdSchedTableInstall(xs->rank, ABTI_sched_build_table(xs->main_sched));
}

int ABTI_xstream_lease(ABTI_sched *sched, int want_rank, ABTI_xstream **out) {
  std::lock_guard<std::mutex> g(ABTI_g->xm);
  int rank = -1;
  if (want_rank < 0) {
    for (int r = 1; r < ABTI_g->num_pes; r++) if (!ABTI_g->xstreams[r]) { rank = r; break; }
    if (rank < 0) return ABT_ERR_INV_XSTREAM_RANK; /* more xstreams than ABT_MAX_NUM_XSTREAMS PEs */
  } else {
    if (want_rank <= 0 || want_rank >= ABTI_g->num_pes || ABTI_g->xstreams[want_rank]) return ABT_ERR_INV_XSTREAM_RANK;
    rank = want_rank;
  }
  ABTI_xstream *xs = new ABTI_xstream();
  xs->rank = rank;
  xs->state.store(ABT_XSTREAM_STATE_RUNNING);
  xs->main_sched = sched;
  xs->primary = false;
  xs->cpubind = -1;
  sched->used_by = xs;
  ABTI_g->xstreams[rank] = xs;
  *out = xs;
  return ABT_SUCCESS;
}

static int join_impl(ABTI_xstream *x) {
  if (x->primary) return ABT_ERR_INV_XSTREAM;
  if (ABTI_tls_xstream == x && ABTI_on_pe()) return ABT_ERR_INV_XSTREAM; /* cannot join self */
  if (x->finished.load(std::memory_order_acquire)) return ABT_SUCCESS;
  x->finishing.store(1, std::memory_order_release);
  ABTI_thread *self = ABTI_self_thread();
  if (self) {
    x->jm.lock();
    if (x->finished.load(std::memory_order_acquire)) { x->jm.unlock(); return ABT_SUCCESS; }
    x->joiners.push_back(self->cth);
    ABTI_thread_block(self, unlock_mutex, &x->jm);
    return ABT_SUCCESS;
  }
  while (!x->finished.load(std::memory_order_acquire)) sched_yield();
  return ABT_SUCCESS;
}

extern "C" {

int ABT_xstream_create(ABT_sched sched, ABT_xstream *newxstream) {
  ABTI_CHECK_INITIALIZED();
  ABTI_CHECK_NULL(newxstream, ABT_ERR_INV_ARG);
  ABTI_sched *s = ABTI_sched_get(sched);
  if (!s) {
    int err; s = ABTI_sched_create_predef(ABT_SCHED_DEFAULT, 0, nullptr, &err);
    if (!s) return err;
  } else if (s->used_by) {
    return ABT_ERR_INV_SCHED;
  }
  ABTI_xstream *xs;
  int r = ABTI_xstream_lease(s, -1, &xs);
  if (r != ABT_SUCCESS) return r;
  ABTI_xstream_install(xs);
  *newxstream = ABTI_xstream_handle(xs);
  return ABT_SUCCESS;
}
int ABT_xstream_create_basic(ABT_sched_predef predef, int num_pools, ABT_pool *pools, ABT_sched_config config, ABT_xstream *newxstream) {
  ABTI_CHECK_INITIALIZED();
  int err; ABTI_sched *s = ABTI_sched_create_predef(predef, num_pools, pools, &err);
  if (!s) return err;
  return ABT_xstream_create(ABTI_sched_handle(s), newxstream);
}
int ABT_xstream_create_with_rank(ABT_sched sched, int rank, ABT_xstream *newxstream) {
  ABTI_CHECK_INITIALIZED();
  ABTI_sched *s = ABTI_sched_get(sched);
  if (!s) { int err; s = ABTI_sched_create_predef(ABT_SCHED_DEFAULT, 0, nullptr, &err); if (!s) return err; }
  else if (s->used_by) return ABT_ERR_INV_SCHED;
  ABTI_xstream *xs;
  int r = ABTI_xstream_lease(s, rank, &xs);
  if (r != ABT_SUCCESS) return r;
  ABTI_xstream_install(xs);
  *newxstream = ABTI_xstream_handle(xs);
  return ABT_SUCCESS;
}
int ABT_xstream_join(ABT_xstream xstream) {
  ABTI_CHECK_INITIALIZED();
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  return join_impl(x);
}
int ABT_xstream_free(ABT_xstream *xstream) {
  ABTI_CHECK_INITIALIZED();
  ABTI_CHECK_NULL(xstream, ABT_ERR_INV_XSTREAM);
  ABTI_xstream *x = ABTI_xstream_get(*xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  int r = join_impl(x);
  if (r != ABT_SUCCESS) return r;
  if (x->main_sched && x->main_sched->automatic) ABTI_sched_destroy(x->main_sched);
  { std::lock_guard<std::mutex> g(ABTI_g->xm); ABTI_g->xstreams[x->rank] = nullptr; }
  delete x;
  *xstream = ABT_XSTREAM_NULL;
  return ABT_SUCCESS;
}
int ABT_xstream_self(ABT_xstream *xstream) {
  if (xstream) *xstream = ABT_XSTREAM_NULL; /* written even on failure (Argobots does) */
  ABTI_CHECK_INITIALIZED();
  return ABT_self_get_xstream(xstream);
}
int ABT_xstream_self_rank(int *rank) { return ABT_self_get_xstream_rank(rank); }
int ABT_xstream_get_rank(ABT_xstream xstream, int *rank) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  *rank = x->rank; return ABT_SUCCESS;
}
int ABT_xstream_set_rank(ABT_xstream xstream, int rank) { ABTI_UNIMPLEMENTED("ABT_xstream_set_rank"); }
int ABT_xstream_set_main_sched(ABT_xstream xstream, ABT_sched sched) {
  ABTI_CHECK_INITIALIZED();
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  if (s->used_by && s->used_by != x) return ABT_ERR_INV_SCHED;
  ABTI_sched *old = x->main_sched;
  if (old == s) return ABT_SUCCESS;
  x->main_sched = s;
  s->used_by = x;
  if (old) old->used_by = nullptr;
  CsdSchedTableInstall(x->rank, ABTI_sched_build_table(s));
  /* the caller, if it lives in one of the old pools, moves to pools[0] */
  ABTI_thread *self = ABTI_self_thread();
  if (self && old && !s->pools.empty()) {
    bool in_old = false;
    for (ABTI_pool *p : old->pools) if (p == self->pool) in_old = true;
    if (in_old) ABTI_pool_associate(self, s->pools[0]);
  }
  if (old && old->automatic) ABTI_sched_destroy(old);
  return ABT_SUCCESS;
}
int ABT_xstream_set_main_sched_basic(ABT_xstream xstream, ABT_sched_predef predef, int num_pools, ABT_pool *pools) {
  int err; ABTI_sched *s = ABTI_sched_create_predef(predef, num_pools, pools, &err);
  if (!s) return err;
  return ABT_xstream_set_main_sched(xstream, ABTI_sched_handle(s));
}
int ABT_xstream_get_main_sched(ABT_xstream xstream, ABT_sched *sched) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  *sched = ABTI_sched_handle(x->main_sched); return ABT_SUCCESS;
}
int ABT_xstream_get_main_pools(ABT_xstream xstream, int max_pools, ABT_pool *pools) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  int n = (int)x->main_sched->pools.size();
  for (int i = 0; i < max_pools && i < n; i++) pools[i] = ABTI_pool_handle(x->main_sched->pools[i]);
  return ABT_SUCCESS;
}
int ABT_xstream_get_state(ABT_xstream xstream, ABT_xstream_state *state) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  *state = (ABT_xstream_state)x->state.load(); return ABT_SUCCESS;
}
int ABT_xstream_equal(ABT_xstream xstream1, ABT_xstream xstream2, ABT_bool *result) {
  *result = xstream1 == xstream2 ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
}
int ABT_xstream_get_num(int *num_xstreams) {
  ABTI_CHECK_INITIALIZED();
  std::lock_guard<std::mutex> g(ABTI_g->xm);
  int n = 0; for (ABTI_xstream *x : ABTI_g->xstreams) if (x) n++;
  *num_xstreams = n; return ABT_SUCCESS;
}
int ABT_xstream_is_primary(ABT_xstream xstream, ABT_bool *is_primary) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  *is_primary = x->primary ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
}
int ABT_xstream_check_events(ABT_sched sched) { return ABT_SUCCESS; }
#ifdef __APPLE__
#define ABTI_AFFINITY_NA 1 /* no thread binding on macOS: native Argobots answers FEATURE_NA */
#else
#define ABTI_AFFINITY_NA 0
#endif
int ABT_xstream_set_cpubind(ABT_xstream xstream, int cpuid) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  if (ABTI_AFFINITY_NA) return ABT_ERR_FEATURE_NA;
  x->cpubind = cpuid;
  send_pe_msg(x->rank, x, ABTI_OP_AFFINITY, cpuid, ABTI_g->affinity_handler);
  return ABT_SUCCESS;
}
int ABT_xstream_get_cpubind(ABT_xstream xstream, int *cpuid) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  if (ABTI_AFFINITY_NA || x->cpubind < 0) return ABT_ERR_FEATURE_NA;
  *cpuid = x->cpubind; return ABT_SUCCESS;
}
int ABT_xstream_set_affinity(ABT_xstream xstream, int num_cpuids, int *cpuids) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  if (ABTI_AFFINITY_NA) return ABT_ERR_FEATURE_NA;
  x->affinity.assign(cpuids, cpuids + (num_cpuids > 0 ? num_cpuids : 0));
  if (num_cpuids > 0) return ABT_xstream_set_cpubind(xstream, cpuids[0]); /* one PE binds to one core */
  return ABT_SUCCESS;
}
int ABT_xstream_get_affinity(ABT_xstream xstream, int max_cpuids, int *cpuids, int *num_cpuids) {
  ABTI_xstream *x = ABTI_xstream_get(xstream); ABTI_CHECK_NULL(x, ABT_ERR_INV_XSTREAM);
  if (ABTI_AFFINITY_NA || x->affinity.empty()) return ABT_ERR_FEATURE_NA;
  int n = (int)x->affinity.size();
  for (int i = 0; i < max_cpuids && i < n; i++) cpuids[i] = x->affinity[i];
  *num_cpuids = n; return ABT_SUCCESS;
}
int ABT_xstream_revive(ABT_xstream xstream) { ABTI_UNIMPLEMENTED("ABT_xstream_revive"); }
int ABT_xstream_exit(void) { ABTI_UNIMPLEMENTED("ABT_xstream_exit"); }
int ABT_xstream_cancel(ABT_xstream xstream) { ABTI_UNIMPLEMENTED("ABT_xstream_cancel"); }
int ABT_xstream_run_unit(ABT_unit unit, ABT_pool pool) { ABTI_UNIMPLEMENTED("ABT_xstream_run_unit"); }
int ABT_xstream_barrier_create(uint32_t num_waiters, ABT_xstream_barrier *newbarrier) { ABTI_UNIMPLEMENTED("ABT_xstream_barrier_create"); }
int ABT_xstream_barrier_free(ABT_xstream_barrier *barrier) { ABTI_UNIMPLEMENTED("ABT_xstream_barrier_free"); }
int ABT_xstream_barrier_wait(ABT_xstream_barrier barrier) { ABTI_UNIMPLEMENTED("ABT_xstream_barrier_wait"); }

} /* extern "C" */
