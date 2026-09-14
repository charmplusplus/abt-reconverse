/* First functional test of the shim: init, pools, xstreams, ULTs across
 * xstreams, join/free, yield stress, migration, self queries, finalize. */
#include <abt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

static int ranks_seen[64];
static volatile int counter;
static ABT_thread g_watch[9]; static int g_nwatch; static volatile int g_watch_done;
static ABT_xstream g_wxs[4]; static int g_nwxs; static const char *g_phase = "";
static void *watchdog(void *a) {
  (void)a;
  for (;;) {
    sleep(2);
    if (g_watch_done) return NULL;
    fprintf(stderr, "watchdog[%s]: counter=%d\n", g_phase, counter);
    for (int i = 0; i < g_nwatch; i++) { ABT_thread_state st = 99; ABT_thread_get_state(g_watch[i], &st); fprintf(stderr, "  thread %d state %d\n", i, (int)st); }
    for (int i = 0; i < g_nwxs; i++) { ABT_xstream_state st = 99; ABT_xstream_get_state(g_wxs[i], &st); fprintf(stderr, "  xstream %d state %d\n", i, (int)st); }
  }
}

#define CHECK(call) do { int _r = (call); if (_r != ABT_SUCCESS) { char s[128]; size_t l = sizeof s; ABT_error_get_str(_r, s, &l); fprintf(stderr, "%s:%d: %s -> %s\n", __FILE__, __LINE__, #call, s); exit(1); } } while (0)
#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)


static void hello(void *arg) {
  int rank = -1;
  CHECK(ABT_self_get_xstream_rank(&rank));
  ASSERT(rank >= 0 && rank < 64);
  __sync_fetch_and_add(&ranks_seen[rank], 1);
  __sync_fetch_and_add(&counter, 1);
  (void)arg;
}

static void yielder(void *arg) {
  int n = *(int *)arg;
  ABT_thread self;
  CHECK(ABT_thread_self(&self));
  for (int i = 0; i < n; i++) {
    ABT_thread_state st;
    CHECK(ABT_thread_get_state(self, &st));
    ASSERT(st == ABT_THREAD_STATE_RUNNING);
    CHECK(ABT_thread_yield());
  }
  __sync_fetch_and_add(&counter, 1);
}

static void migrant(void *arg) {
  ABT_pool target = *(ABT_pool *)arg;
  ABT_thread self;
  int before, after;
  CHECK(ABT_thread_self(&self));
  CHECK(ABT_self_get_xstream_rank(&before));
  CHECK(ABT_thread_migrate_to_pool(self, target)); /* applied at the next wake */
  CHECK(ABT_thread_yield());
  CHECK(ABT_self_get_xstream_rank(&after));
  ABT_pool last;
  CHECK(ABT_thread_get_last_pool(self, &last));
  ASSERT(last == target);
  __sync_fetch_and_add(&counter, 1);
  (void)before; (void)after;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setenv("ABT_MAX_NUM_XSTREAMS", "3", 0); /* 3 secondary + the primary = 4 PEs */
  CHECK(ABT_init(argc, argv));
  ASSERT(ABT_initialized() == ABT_SUCCESS);

  ABT_xstream self_xs; int self_rank;
  CHECK(ABT_xstream_self(&self_xs));
  CHECK(ABT_xstream_self_rank(&self_rank));
  ASSERT(self_rank == 0);
  ABT_bool b;
  CHECK(ABT_self_is_primary(&b)); ASSERT(b == ABT_TRUE);
  CHECK(ABT_self_on_primary_xstream(&b)); ASSERT(b == ABT_TRUE);

  /* a shared pool polled by three secondary xstreams */
  ABT_pool shared;
  CHECK(ABT_pool_create_basic(ABT_POOL_FIFO, ABT_POOL_ACCESS_MPMC, ABT_FALSE, &shared));
  ABT_xstream xs[3];
  for (int i = 0; i < 3; i++) CHECK(ABT_xstream_create_basic(ABT_SCHED_BASIC, 1, &shared, ABT_SCHED_CONFIG_NULL, &xs[i]));
  int num; CHECK(ABT_xstream_get_num(&num)); ASSERT(num == 4);

  /* 1: detached ULTs run on the secondary xstreams */
  counter = 0;
  const int N = 200;
  for (int i = 0; i < N; i++) CHECK(ABT_thread_create(shared, hello, NULL, ABT_THREAD_ATTR_NULL, NULL));
  while (counter < N) CHECK(ABT_thread_yield());
  int used = 0; for (int r = 1; r < 4; r++) used += ranks_seen[r] > 0;
  ASSERT(ranks_seen[0] == 0);
  printf("1 ok: %d detached ULTs ran on %d secondary xstreams\n", N, used);

  /* 2: named ULTs, join + free */
  counter = 0;
  ABT_thread named[16];
  for (int i = 0; i < 16; i++) CHECK(ABT_thread_create(shared, hello, NULL, ABT_THREAD_ATTR_NULL, &named[i]));
  for (int i = 0; i < 16; i++) CHECK(ABT_thread_join(named[i]));
  ASSERT(counter == 16);
  for (int i = 0; i < 16; i++) { ABT_thread_state st; CHECK(ABT_thread_get_state(named[i], &st)); ASSERT(st == ABT_THREAD_STATE_TERMINATED); CHECK(ABT_thread_free(&named[i])); ASSERT(named[i] == ABT_THREAD_NULL); }
  printf("2 ok: 16 named ULTs joined and freed\n");

  /* 3: yield stress across 3 pollers */
  counter = 0;
  int iters = 2000;
  ABT_thread ys[8];
  for (int i = 0; i < 8; i++) CHECK(ABT_thread_create(shared, yielder, &iters, ABT_THREAD_ATTR_NULL, &ys[i]));
  { ABT_thread prim; CHECK(ABT_thread_self(&prim)); for (int i = 0; i < 8; i++) g_watch[i] = ys[i]; g_watch[8] = prim; g_nwatch = 9; pthread_t w; pthread_create(&w, NULL, watchdog, NULL); pthread_detach(w); g_phase = "3"; }
  for (int i = 0; i < 8; i++) CHECK(ABT_thread_free(&ys[i])); /* free joins first */
  ASSERT(counter == 8);
  g_phase = "4"; g_nwatch = 1; g_nwxs = 3; for (int i = 0; i < 3; i++) g_wxs[i] = xs[i];
  printf("3 ok: 8 ULTs x %d yields\n", iters);

  /* 4: migration to another pool */
  ABT_pool other; ABT_xstream xs4;
  CHECK(ABT_pool_create_basic(ABT_POOL_FIFO, ABT_POOL_ACCESS_MPMC, ABT_FALSE, &other));
  int r4 = ABT_xstream_create_basic(ABT_SCHED_BASIC, 1, &other, ABT_SCHED_CONFIG_NULL, &xs4);
  ASSERT(r4 == ABT_ERR_INV_XSTREAM_RANK); /* all 4 PEs leased: the cap holds */
  ABT_pool p0; CHECK(ABT_xstream_get_main_pools(xs[0], 1, &p0)); ASSERT(p0 == shared);
  /* free one xstream, lease its PE again with the other pool */
  CHECK(ABT_xstream_join(xs[2]));
  CHECK(ABT_xstream_free(&xs[2]));
  CHECK(ABT_xstream_create_basic(ABT_SCHED_BASIC, 1, &other, ABT_SCHED_CONFIG_NULL, &xs4));
  counter = 0;
  ABT_thread m;
  CHECK(ABT_thread_create(shared, migrant, &other, ABT_THREAD_ATTR_NULL, &m));
  CHECK(ABT_thread_free(&m));
  ASSERT(counter == 1);
  g_watch_done = 1;
  printf("4 ok: xstream re-leased; ULT migrated between pools\n");

  /* 5: total size counts blocked ULTs */
  size_t total; CHECK(ABT_pool_get_total_size(shared, &total)); ASSERT(total == 0);

  /* teardown */
  CHECK(ABT_xstream_join(xs[0])); CHECK(ABT_xstream_free(&xs[0]));
  CHECK(ABT_xstream_join(xs[1])); CHECK(ABT_xstream_free(&xs[1]));
  CHECK(ABT_xstream_join(xs4)); CHECK(ABT_xstream_free(&xs4));
  CHECK(ABT_pool_free(&shared));
  CHECK(ABT_pool_free(&other));
  CHECK(ABT_finalize());
  ASSERT(ABT_initialized() == ABT_ERR_UNINITIALIZED);
  printf("core_smoke ok: finalized, main returning\n");
  return 0;
}
