/* Schedulers: a predefined scheduler is a list of pools plus a policy that
 * becomes the PE's poll-table weights when an xstream runs it. */
#include "abti.h"

#include <cstdarg>
#include <map>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

/* exported configuration variables (values as in Argobots) */
extern "C" {
ABT_sched_config_var ABT_sched_config_var_end = { .idx = -1, .type = ABT_SCHED_CONFIG_INT };
ABT_sched_config_var ABT_sched_config_access = { .idx = -2, .type = ABT_SCHED_CONFIG_INT };
ABT_sched_config_var ABT_sched_config_automatic = { .idx = -3, .type = ABT_SCHED_CONFIG_INT };
ABT_sched_config_var ABT_sched_basic_freq = { .idx = -4, .type = ABT_SCHED_CONFIG_INT };
}

/* ---- scheduler configuration objects ------------------------------------
 * A small typed key/value map.  Keys are ABT_sched_config_var::idx values:
 * the predefined vars use negative idx (-1 end, -2 access, -3 automatic,
 * -4 basic_freq), user-defined vars number themselves 0, 1, 2, ... and
 * ABT_sched_config_read() reads them back positionally by that number. */
namespace {
struct ABTI_sched_config_val {
  ABT_sched_config_type type;
  int i;
  double d;
  const void *p;
};
} // namespace

struct ABTI_sched_config {
  std::map<int, ABTI_sched_config_val> vals;
};

static ABTI_sched_config *SC(ABT_sched_config h) { return ABTI_obj<ABTI_sched_config>(h); }

/* the predefined int knobs a basic scheduler understands */
static void sched_apply_config(ABTI_sched *s, ABT_sched_config config) {
  ABTI_sched_config *c = SC(config);
  if (!c) return;
  auto it = c->vals.find(ABT_sched_basic_freq.idx);
  if (it != c->vals.end() && it->second.type == ABT_SCHED_CONFIG_INT && it->second.i > 0)
    s->event_freq = it->second.i;
  it = c->vals.find(ABT_sched_config_automatic.idx);
  if (it != c->vals.end() && it->second.type == ABT_SCHED_CONFIG_INT)
    s->automatic = it->second.i ? ABT_TRUE : ABT_FALSE;
  /* ABT_sched_config_access is ignored by Argobots too */
}

ABTI_sched *ABTI_sched_create_predef(ABT_sched_predef predef, int num_pools, ABT_pool *pools, int *err) {
  *err = ABT_SUCCESS;
  if (predef < ABT_SCHED_DEFAULT || predef > ABT_SCHED_BASIC_WAIT) { *err = ABT_ERR_INV_SCHED_PREDEF; return nullptr; }
  if (num_pools < 0) { *err = ABT_ERR_INV_ARG; return nullptr; }
  ABTI_sched *s = new ABTI_sched();
  s->predef = predef;
  s->user_def = false;
  s->runner = nullptr;
  s->config = ABT_SCHED_CONFIG_NULL;
  s->automatic = ABT_TRUE;
  s->event_freq = 50;
  s->used_by = nullptr;
  s->data = nullptr;
  ABT_pool_kind kind = predef == ABT_SCHED_BASIC_WAIT ? ABT_POOL_FIFO_WAIT : ABT_POOL_FIFO;
  if (pools == nullptr) {
    int n = predef == ABT_SCHED_PRIO ? 3 : 1; /* ABTI_SCHED_NUM_PRIO */
    for (int i = 0; i < n; i++)
      s->pools.push_back(ABTI_pool_create_builtin(kind, ABT_POOL_ACCESS_MPMC, ABT_TRUE));
  } else {
    for (int i = 0; i < num_pools; i++) {
      ABTI_pool *p = ABTI_pool_get(pools[i]);
      if (!p) {
        p = ABTI_pool_create_builtin(kind, ABT_POOL_ACCESS_MPMC, ABT_TRUE);
        pools[i] = ABTI_pool_handle(p); /* Argobots hands the created pool back */
      }
      s->pools.push_back(p);
    }
  }
  for (ABTI_pool *p : s->pools) p->num_scheds.fetch_add(1);
  return s;
}

void ABTI_sched_destroy(ABTI_sched *s) {
  if (s->user_def && s->def.free) s->def.free(ABTI_sched_handle(s));
  for (ABTI_pool *p : s->pools) {
    int left = p->num_scheds.fetch_sub(1) - 1;
    /* automatic pools go with their last scheduler, if nothing is left in
     * them; otherwise they are the user's (Argobots: "the user's
     * responsibility to handle work units in pools of the old scheduler") */
    if (left == 0 && p->automatic && p->size() == 0 && p->num_blocked.load() == 0)
      ABTI_pool_destroy(p);
  }
  delete s;
}

CsdSchedTable ABTI_sched_build_table(ABTI_sched *s) {
  std::vector<CsdPollEntry> entries;
  int n = (int)s->pools.size();
  if (s->predef == ABT_SCHED_PRIO) {
    /* strict priority by pool index, equal slots; each entry declines while a
     * higher-priority pool has queued work */
    s->prio_ctx.clear();
    s->prio_ctx.reserve(n);
    for (int i = 0; i < n; i++) s->prio_ctx.push_back(ABTI_sched::PrioCtx{s, i});
    for (int i = 0; i < n; i++)
      entries.push_back(CsdPollEntry{ABTI_poll_pool_prio, &s->prio_ctx[i], 16, "abt prio pool"});
    return CsdSchedTableCreate(entries.data(), n);
  }
  for (int i = 0; i < n; i++) {
    unsigned freq = 16;
    if (s->predef == ABT_SCHED_RANDWS) freq = i == 0 ? 32 : 4;
    entries.push_back(CsdPollEntry{ABTI_poll_pool, s->pools[i], freq, "abt pool"});
  }
  return CsdSchedTableCreate(entries.data(), n);
}

/* ---- stackable schedulers ---------------------------------------------
 * ABT_pool_add_sched makes a scheduler a work unit of a pool.  As in Argobots
 * (ABTI_ythread_create_sched) the unit is a ULT whose body runs the
 * scheduler's policy over the scheduler's own pools; ULTs it resumes come
 * back to it through ABTI_choose_fn (ABTI_pool_run_thread records the
 * resuming ULT as their parent at every resume).  It returns to the
 * parent scheduler when its pools hold no unit, blocked ULTs included for
 * pools it consumes alone (ABTI_sched_has_unit); while only blocked ULTs
 * remain it keeps the PE, as Argobots does (its basic loop sleeps 100 ns per
 * empty pass) -- here the PE parks in CsdIdleWait until a push to one of the
 * pools.  Handing the PE back to the parent while waiting would be a
 * one-line change but changes the order units run in, so it is not done. */
static bool sched_has_unit(ABTI_sched *s) {
  for (ABTI_pool *p : s->pools) {
    if (p->size() > 0) return true;
    if (p->access == ABT_POOL_ACCESS_PRIV || p->num_scheds.load() == 1)
      if (p->num_blocked.load() > 0) return true;
  }
  return false;
}

static ABTI_thread *sched_pop_by_policy(ABTI_sched *s) {
  int n = (int)s->pools.size();
  if (n == 0) return nullptr;
  if (s->predef == ABT_SCHED_RANDWS && n > 1) {
    if (ABTI_thread *t = s->pools[0]->pop()) return t;
    return s->pools[1 + (int)(random() % (n - 1))]->pop();
  }
  /* BASIC, DEFAULT, BASIC_WAIT, PRIO: first non-empty pool in index order
   * (Argobots' basic loop restarts from pools[0] after every unit) */
  for (int i = 0; i < n; i++) if (ABTI_thread *t = s->pools[i]->pop()) return t;
  return nullptr;
}

static void stacked_sched_main(void *arg) {
  ABTI_sched *s = static_cast<ABTI_sched *>(arg);
  ABTI_DBG("stacked sched %p start (%d pools)", (void *)s, (int)s->pools.size());
  if (s->user_def) {
    s->def.run(ABTI_sched_handle(s)); /* the user's loop decides when to stop (ABT_sched_has_to_stop) */
  } else {
    int rank = CmiMyRank();
    for (;;) {
      if (s->request.load() & ABTI_SCHED_REQ_EXIT) break;
      if (ABTI_thread *t = sched_pop_by_policy(s)) { ABTI_pool_run_thread(t); continue; }
      if (!sched_has_unit(s)) break; /* drained: finish (Argobots: "used in pool -> finish it anyway") */
      /* only blocked ULTs left: hold the PE until one of them is pushed back */
      for (ABTI_pool *p : s->pools) p->add_sleeper(rank);
      bool ready = false;
      for (ABTI_pool *p : s->pools) if (p->size() > 0) { ready = true; break; }
      if (!ready) CsdIdleWait(0.010);
      for (ABTI_pool *p : s->pools) p->remove_sleeper(rank);
    }
  }
  ABTI_DBG("stacked sched %p end", (void *)s);
  s->in_pool = false;
}

int ABTI_sched_add_to_pool(ABTI_sched *s, ABTI_pool *parent) {
  if (s->in_pool || s->used_by) return ABT_ERR_INV_SCHED; /* Argobots: used != NOT_USED */
  s->in_pool = true;
  int r = ABTI_thread_create_internal(ABTI_pool_handle(parent), stacked_sched_main, s, 1024 * 1024);
  if (r != ABT_SUCCESS) s->in_pool = false;
  return r;
}

extern "C" {

int ABT_sched_create_basic(ABT_sched_predef predef, int num_pools, ABT_pool *pools, ABT_sched_config config, ABT_sched *newsched) {
  ABTI_CHECK_INITIALIZED();
  ABTI_CHECK_NULL(newsched, ABT_ERR_INV_ARG);
  int err;
  ABTI_sched *s = ABTI_sched_create_predef(predef, num_pools, pools, &err);
  if (!s) return err;
  sched_apply_config(s, config); /* ABT_SCHED_CONFIG_NULL is a no-op */
  *newsched = ABTI_sched_handle(s);
  return ABT_SUCCESS;
}

int ABT_sched_create(ABT_sched_def *def, int num_pools, ABT_pool *pools, ABT_sched_config config, ABT_sched *newsched) {
  ABTI_CHECK_INITIALIZED();
  ABTI_CHECK_NULL(def, ABT_ERR_INV_SCHED);
  ABTI_CHECK_NULL(newsched, ABT_ERR_INV_ARG);
  if (!def->run) return ABT_ERR_INV_SCHED;
  int err;
  ABTI_sched *s = ABTI_sched_create_predef(ABT_SCHED_BASIC, num_pools, pools, &err);
  if (!s) return err;
  s->user_def = true;
  s->def = *def;
  s->config = config;
  s->runner = nullptr;
  s->automatic = ABT_FALSE; /* ABT_sched_create's default (ABT_sched_config_automatic may raise it) */
  if (def->init) { /* synchronously, as Argobots does: callers free the config right after */
    int r = def->init(ABTI_sched_handle(s), config);
    if (r != ABT_SUCCESS) { ABTI_sched_destroy(s); return r; }
  }
  *newsched = ABTI_sched_handle(s);
  return ABT_SUCCESS;
}

int ABT_sched_free(ABT_sched *sched) {
  ABTI_CHECK_INITIALIZED();
  ABTI_CHECK_NULL(sched, ABT_ERR_INV_SCHED);
  ABTI_sched *s = ABTI_sched_get(*sched);
  ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  if (s->used_by || s->in_pool) return ABT_ERR_INV_SCHED; /* still running on an xstream or as a pool unit */
  ABTI_sched_destroy(s);
  *sched = ABT_SCHED_NULL;
  return ABT_SUCCESS;
}
int ABT_sched_set_data(ABT_sched sched, void *data) {
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  s->data = data; return ABT_SUCCESS;
}
int ABT_sched_get_data(ABT_sched sched, void **data) {
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  *data = s->data; return ABT_SUCCESS;
}
int ABT_sched_get_num_pools(ABT_sched sched, int *num_pools) {
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  *num_pools = (int)s->pools.size(); return ABT_SUCCESS;
}
int ABT_sched_get_pools(ABT_sched sched, int max_pools, int idx, ABT_pool *pools) {
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  if (idx < 0 || idx + max_pools > (int)s->pools.size()) return ABT_ERR_SCHED;
  for (int i = 0; i < max_pools; i++) pools[i] = ABTI_pool_handle(s->pools[idx + i]);
  return ABT_SUCCESS;
}
int ABT_sched_get_size(ABT_sched sched, size_t *size) {
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  size_t n = 0; for (ABTI_pool *p : s->pools) n += p->size();
  *size = n; return ABT_SUCCESS;
}
int ABT_sched_get_total_size(ABT_sched sched, size_t *size) {
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  size_t n = 0;
  for (ABTI_pool *p : s->pools) { long b = p->num_blocked.load(); n += p->size() + (b > 0 ? (size_t)b : 0); }
  *size = n; return ABT_SUCCESS;
}
int ABT_sched_finish(ABT_sched sched) {
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  s->request.fetch_or(ABTI_SCHED_REQ_FINISH); return ABT_SUCCESS;
}
int ABT_sched_exit(ABT_sched sched) {
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  s->request.fetch_or(ABTI_SCHED_REQ_EXIT); return ABT_SUCCESS;
}
int ABT_sched_has_to_stop(ABT_sched sched, ABT_bool *stop) {
  ABTI_sched *s = ABTI_sched_get(sched); ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  int r = s->request.load();
  if (r & ABTI_SCHED_REQ_EXIT) { *stop = ABT_TRUE; return ABT_SUCCESS; }
  if (r & ABTI_SCHED_REQ_FINISH) {
    /* Argobots' ABTI_sched_has_unit: blocked ULTs count only for pools this
     * scheduler owns alone -- a pool shared with other schedulers (work
     * stealing) may hold ULTs blocked on their behalf, e.g. the joiner */
    size_t n = 0;
    for (ABTI_pool *p : s->pools) {
      n += p->size();
      if (p->num_scheds.load() <= 1) { long b = p->num_blocked.load(); if (b > 0) n += (size_t)b; }
    }
    *stop = n == 0 ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
  }
  *stop = ABT_FALSE; return ABT_SUCCESS;
}
/*
 * The variadic list is (ABT_sched_config_var, value) pairs terminated by
 * ABT_sched_config_var_end; the value's type comes from the var.  Structs are
 * passed by value through varargs here, exactly as Argobots does it.
 */
int ABT_sched_config_create(ABT_sched_config *config, ...) {
  if (!config) return ABT_ERR_INV_SCHED_CONFIG;
  ABTI_sched_config *c = new ABTI_sched_config();
  va_list ap;
  va_start(ap, config);
  for (;;) {
    ABT_sched_config_var var = va_arg(ap, ABT_sched_config_var);
    if (var.idx == ABT_sched_config_var_end.idx) break;
    ABTI_sched_config_val v{};
    v.type = var.type;
    if (var.type == ABT_SCHED_CONFIG_INT) v.i = va_arg(ap, int);
    else if (var.type == ABT_SCHED_CONFIG_DOUBLE) v.d = va_arg(ap, double);
    else v.p = va_arg(ap, void *);
    c->vals[var.idx] = v;
  }
  va_end(ap);
  *config = reinterpret_cast<ABT_sched_config>(c);
  return ABT_SUCCESS;
}

/* num_vars pointers, one per index 0..num_vars-1; a NULL pointer skips that
 * index and an index with no value set leaves the caller's variable alone. */
int ABT_sched_config_read(ABT_sched_config config, int num_vars, ...) {
  ABTI_sched_config *c = SC(config);
  if (!c) return ABT_ERR_INV_SCHED_CONFIG;
  va_list ap;
  va_start(ap, num_vars);
  for (int i = 0; i < num_vars; i++) {
    void *dst = va_arg(ap, void *);
    auto it = c->vals.find(i);
    if (!dst || it == c->vals.end()) continue;
    if (it->second.type == ABT_SCHED_CONFIG_INT) *(int *)dst = it->second.i;
    else if (it->second.type == ABT_SCHED_CONFIG_DOUBLE) *(double *)dst = it->second.d;
    else *(const void **)dst = it->second.p;
  }
  va_end(ap);
  return ABT_SUCCESS;
}

int ABT_sched_config_free(ABT_sched_config *config) {
  if (!config) return ABT_ERR_INV_SCHED_CONFIG;
  ABTI_sched_config *c = SC(*config);
  if (!c) return ABT_ERR_INV_SCHED_CONFIG;
  delete c;
  *config = ABT_SCHED_CONFIG_NULL;
  return ABT_SUCCESS;
}

/* val == NULL deletes the entry (Argobots' documented behavior) */
int ABT_sched_config_set(ABT_sched_config config, int idx, ABT_sched_config_type type, const void *val) {
  ABTI_sched_config *c = SC(config);
  if (!c) return ABT_ERR_INV_SCHED_CONFIG;
  if (!val) { c->vals.erase(idx); return ABT_SUCCESS; }
  ABTI_sched_config_val v{};
  v.type = type;
  if (type == ABT_SCHED_CONFIG_INT) v.i = *(const int *)val;
  else if (type == ABT_SCHED_CONFIG_DOUBLE) v.d = *(const double *)val;
  else v.p = *(void *const *)val;
  c->vals[idx] = v;
  return ABT_SUCCESS;
}

/* an unset index is an error and must leave both outputs untouched */
int ABT_sched_config_get(ABT_sched_config config, int idx, ABT_sched_config_type *p_type, void *val) {
  ABTI_sched_config *c = SC(config);
  if (!c) return ABT_ERR_INV_SCHED_CONFIG;
  auto it = c->vals.find(idx);
  if (it == c->vals.end()) return ABT_ERR_INV_SCHED_CONFIG;
  if (p_type) *p_type = it->second.type;
  if (val) {
    if (it->second.type == ABT_SCHED_CONFIG_INT) *(int *)val = it->second.i;
    else if (it->second.type == ABT_SCHED_CONFIG_DOUBLE) *(double *)val = it->second.d;
    else *(const void **)val = it->second.p;
  }
  return ABT_SUCCESS;
}

} /* extern "C" */
