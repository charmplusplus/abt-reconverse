/* Schedulers: a predefined scheduler is a list of pools plus a policy that
 * becomes the PE's poll-table weights when an xstream runs it. */
#include "abti.h"

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

ABTI_sched *ABTI_sched_create_predef(ABT_sched_predef predef, int num_pools, ABT_pool *pools, int *err) {
  *err = ABT_SUCCESS;
  if (predef < ABT_SCHED_DEFAULT || predef > ABT_SCHED_BASIC_WAIT) { *err = ABT_ERR_INV_SCHED_PREDEF; return nullptr; }
  if (num_pools < 0) { *err = ABT_ERR_INV_ARG; return nullptr; }
  ABTI_sched *s = new ABTI_sched();
  s->predef = predef;
  s->user_def = false;
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
  for (int i = 0; i < n; i++) {
    unsigned freq = 16;
    switch (s->predef) {
    case ABT_SCHED_PRIO: freq = (unsigned)(32 >> (i < 5 ? i : 5)); if (freq == 0) freq = 1; break;
    case ABT_SCHED_RANDWS: freq = i == 0 ? 32 : 4; break;
    default: break;
    }
    entries.push_back(CsdPollEntry{ABTI_poll_pool, s->pools[i], freq, "abt pool"});
  }
  return CsdSchedTableCreate(entries.data(), n);
}

extern "C" {

int ABT_sched_create_basic(ABT_sched_predef predef, int num_pools, ABT_pool *pools, ABT_sched_config config, ABT_sched *newsched) {
  ABTI_CHECK_INITIALIZED();
  ABTI_CHECK_NULL(newsched, ABT_ERR_INV_ARG);
  int err;
  ABTI_sched *s = ABTI_sched_create_predef(predef, num_pools, pools, &err);
  if (!s) return err;
  /* config objects are not supported (Margo passes ABT_SCHED_CONFIG_NULL) */
  *newsched = ABTI_sched_handle(s);
  return ABT_SUCCESS;
}

int ABT_sched_create(ABT_sched_def *def, int num_pools, ABT_pool *pools, ABT_sched_config config, ABT_sched *newsched) {
  ABTI_UNIMPLEMENTED("ABT_sched_create"); /* user-defined schedulers: Thallium only */
}

int ABT_sched_free(ABT_sched *sched) {
  ABTI_CHECK_INITIALIZED();
  ABTI_CHECK_NULL(sched, ABT_ERR_INV_SCHED);
  ABTI_sched *s = ABTI_sched_get(*sched);
  ABTI_CHECK_NULL(s, ABT_ERR_INV_SCHED);
  if (s->used_by) return ABT_ERR_INV_SCHED; /* still running on an xstream */
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
  if (r & ABTI_SCHED_REQ_FINISH) { size_t n; ABT_sched_get_total_size(sched, &n); *stop = n == 0 ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS; }
  *stop = ABT_FALSE; return ABT_SUCCESS;
}
int ABT_sched_config_create(ABT_sched_config *config, ...) { ABTI_UNIMPLEMENTED("ABT_sched_config_create"); }
int ABT_sched_config_read(ABT_sched_config config, int num_vars, ...) { ABTI_UNIMPLEMENTED("ABT_sched_config_read"); }
int ABT_sched_config_free(ABT_sched_config *config) { ABTI_UNIMPLEMENTED("ABT_sched_config_free"); }
int ABT_sched_config_set(ABT_sched_config config, int idx, ABT_sched_config_type type, const void *val) { ABTI_UNIMPLEMENTED("ABT_sched_config_set"); }
int ABT_sched_config_get(ABT_sched_config config, int idx, ABT_sched_config_type *p_type, void *val) { ABTI_UNIMPLEMENTED("ABT_sched_config_get"); }

} /* extern "C" */
