/* ABT_init / ABT_finalize / ABT_initialized and the unimplemented-call
 * bookkeeping shared by every module. */
#include "abti.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

namespace {
std::atomic<const char *> g_first_unimplemented{nullptr};
std::atomic<unsigned long> g_unimplemented_calls{0};
std::mutex g_init_mutex;
int g_init_refs = 0;
} // namespace

int ABTI_note_unimplemented(const char *name) noexcept {
  g_unimplemented_calls.fetch_add(1, std::memory_order_relaxed);
  const char *expected = nullptr;
  g_first_unimplemented.compare_exchange_strong(expected, name, std::memory_order_release, std::memory_order_relaxed);
  if (getenv("ABT_RECONVERSE_TRACE_NA")) fprintf(stderr, "abt-reconverse: %s not implemented\n", name);
  return ABT_ERR_FEATURE_NA;
}
const char *ABTI_first_unimplemented(void) noexcept { return g_first_unimplemented.load(std::memory_order_acquire); }
unsigned long ABTI_unimplemented_count(void) noexcept { return g_unimplemented_calls.load(std::memory_order_relaxed); }

ABTI_global *ABTI_g = nullptr;

static long env_long(const char *name, long dflt) {
  const char *v = getenv(name);
  if (!v || !*v) return dflt;
  char *end; long x = strtol(v, &end, 10);
  return (*end == 0 && x > 0) ? x : dflt;
}

/* every PE registers the same handlers in the same order; ranks > 0 here,
 * rank 0 after ConverseInit returns */
static void per_pe_setup() {
  ABTI_register_handlers();
  CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE, ABTI_xstream_idle_hook, nullptr);
}

static void startfn(int, char **) {
  /* ranks > 0: unleased PEs sleep until an xstream is created on them */
  per_pe_setup();
  CsdSetSleepOnIdle(1);
  CsdScheduler(-1);
}

/* ABT_MAX_NUM_XSTREAMS counts secondary streams in practice (Argobots' tests
 * create that many plus the primary), so one more PE. Unset: be generous --
 * Argobots only warns past its limit and Mochi configs routinely
 * oversubscribe; unleased PEs sleep, so an extra PE costs a parked pthread. */
int ABTI_num_pes_rule() {
  long cores = sysconf(_SC_NPROCESSORS_ONLN);
  if (cores < 1) cores = 1;
  long maxx = env_long("ABT_MAX_NUM_XSTREAMS", 0);
  long floor_n = cores * 2 < 16 ? 16 : cores * 2;
  long n = maxx > 0 ? (maxx + 1 > floor_n ? maxx + 1 : floor_n) : floor_n; /* the PE count is fixed at the first init */
  if (n < 2) n = 2;
  if (n > 128) n = 128;
  return (int)n;
}

static bool g_runtime_started = false;

/* the reconverse runtime is torn down once, when the process exits; ABT
 * objects are created and destroyed per ABT_init/ABT_finalize cycle */
static void shutdown_runtime() {
  if (g_runtime_started && ABTI_on_pe() && CmiMyRank() == 0) ConverseFinalize();
}

static void start_runtime(int num_pes) {
  std::string pes = std::to_string(num_pes);
  char *av[6];
  av[0] = strdup("abt");
  av[1] = strdup("+pe"); av[2] = strdup(pes.c_str());
  av[3] = strdup("+backend_poll_thread"); av[4] = strdup(pes.c_str()); /* only PE 0 polls the comm backend */
  av[5] = nullptr;
  ConverseSetLibraryMode(1);
  ConverseInit(5, av, startfn, /*usched=*/1, /*initret=*/1);
  /* rank 0, the primary execution stream, continues here */
  per_pe_setup();
  g_runtime_started = true;
  atexit(shutdown_runtime);
}

extern "C" {

int ABT_initialized(void) { return ABTI_initialized() ? ABT_SUCCESS : ABT_ERR_UNINITIALIZED; }

int ABT_init(int argc, char **argv) {
  std::lock_guard<std::mutex> g(g_init_mutex);
  if (ABTI_initialized()) { g_init_refs++; return ABT_SUCCESS; }
  if (!ABTI_g) {
    ABTI_global *G = new ABTI_global();
    G->num_pes = ABTI_num_pes_rule();
    G->default_stacksize = (size_t)env_long("ABT_THREAD_STACKSIZE", 2 * 1024 * 1024);
    if (G->default_stacksize < 16384) G->default_stacksize = 16384;
    G->xstreams.assign(G->num_pes, nullptr);
    ABTI_g = G;
    start_runtime(G->num_pes);
  }
  ABTI_global *G = ABTI_g;
  int err;
  ABTI_sched *sched = ABTI_sched_create_predef(ABT_SCHED_DEFAULT, 0, nullptr, &err);
  ABTI_xstream *xs = new ABTI_xstream();
  xs->rank = 0;
  xs->state.store(ABT_XSTREAM_STATE_RUNNING);
  xs->main_sched = sched;
  xs->primary = true;
  xs->cpubind = -1;
  sched->used_by = xs;
  G->xstreams[0] = xs;
  G->primary_xstream = xs;
  ABTI_tls_xstream = xs;
  G->primary_thread = ABTI_thread_wrap_primary(CthSelf(), sched->pools[0]);
  CsdSchedTableInstall(0, ABTI_sched_build_table(sched)); /* consumed when the primary first blocks */
  CsdSetSleepOnIdle(0);
  G->initialized.store(1, std::memory_order_release);
  g_init_refs = 1;
  return ABT_SUCCESS;
}

int ABT_finalize(void) {
  std::lock_guard<std::mutex> g(g_init_mutex);
  if (!ABTI_initialized()) return ABT_ERR_UNINITIALIZED;
  if (g_init_refs > 1) { g_init_refs--; return ABT_SUCCESS; }
  /* a refused finalize leaves everything as it was */
  if (!ABTI_on_pe() || ABTI_tls_xstream != ABTI_g->primary_xstream) return ABT_ERR_INV_XSTREAM;
  if (ABTI_self_thread() != ABTI_g->primary_thread) return ABT_ERR_INV_THREAD;
  {
    std::lock_guard<std::mutex> gx(ABTI_g->xm);
    for (int r = 1; r < ABTI_g->num_pes; r++) if (ABTI_g->xstreams[r]) return ABT_ERR_INV_XSTREAM; /* secondary xstreams must be freed */
  }
  g_init_refs = 0;
  ABTI_global *G = ABTI_g;
  ABTI_xstream *xs = G->primary_xstream;
  ABTI_thread *pt = G->primary_thread;
  /* work still queued in the primary's pools runs first (Argobots' finalize
   * lets the primary scheduler drain); each yield puts us behind it */
  for (;;) {
    size_t queued = 0;
    for (ABTI_pool *p : xs->main_sched->pools) queued += p->size();
    if (queued == 0) break;
    CthYield();
  }
  G->initialized.store(0, std::memory_order_release);
  /* the primary ULT goes back to being a plain reconverse main thread */
  CthSetAwakenFn(pt->cth, nullptr, nullptr);
  CthSetUserData(pt->cth, nullptr);
  ABTI_pool_disassociate(pt);
  delete pt;
  G->primary_thread = nullptr;
  xs->main_sched->used_by = nullptr;
  ABTI_sched_destroy(xs->main_sched);
  G->xstreams[0] = nullptr;
  G->primary_xstream = nullptr;
  ABTI_tls_xstream = nullptr;
  delete xs;
  CsdSchedTableInstall(0, CsdSchedTableCreate(nullptr, 0));
  /* the reconverse PEs stay up (sleeping) for a later ABT_init; the runtime
   * itself is finalized at process exit */
  return ABT_SUCCESS;
}

} /* extern "C" */
