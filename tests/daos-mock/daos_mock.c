/*
 * daos_mock -- a standalone reproduction of DAOS's use of Argobots, run
 * against the Argobots-on-Reconverse shim.
 *
 * DAOS itself is Linux-only, so this is the pilot: the same API sequence and
 * the same object shapes as the DAOS engine, with no DAOS code.  Every part
 * is mapped to the DAOS file:line it mirrors in README.md; the survey it
 * follows is ../../../daos/DAOS-ABT-USAGE.md (sections 3 and 4.1).
 *
 * What it reproduces:
 *   - dss_sched_init(): three ABT_pool_create_basic(FIFO, MPSC, automatic)
 *     pools per "target" xstream, an ABT_sched_def with a user run loop, an
 *     ABT_sched_config carrying two user variables (idx 0 INT event_freq,
 *     idx 1 PTR dx), ABT_sched_create, ABT_xstream_create_with_rank.
 *   - sched_run(): DAOS's exact loop -- net poll first, NVMe poll second,
 *     then one ULT from the generic pool, ABT_xstream_run_unit, cycle
 *     accounting, ABT_sched_has_to_stop / ABT_xstream_check_events every
 *     event_freq iterations, and an idle relax step driven by
 *     ABT_pool_get_size / ABT_pool_get_total_size.
 *   - The work DAOS puts in those pools: a net-poll ULT that yields forever,
 *     NVMe-poll tasklets, RPC-handler ULTs (named and unnamed) that use
 *     mutex / recursive mutex / cond broadcast / rwlock / eventual / future /
 *     key, and one ULT created from an external pthread.
 *   - DAOS's shutdown: stop flag, drain, ABT_xstream_join + ABT_xstream_free,
 *     ABT_sched_free, ABT_finalize.
 */

#include <abt.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(call)                                                                                \
	do {                                                                                       \
		int _r = (call);                                                                   \
		if (_r != ABT_SUCCESS) {                                                           \
			char   s[128];                                                             \
			size_t l = sizeof s;                                                       \
			ABT_error_get_str(_r, s, &l);                                              \
			fprintf(stderr, "%s:%d: %s -> %s (%d)\n", __FILE__, __LINE__, #call, s,    \
				_r);                                                               \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define ASSERT(c)                                                                                  \
	do {                                                                                       \
		if (!(c)) {                                                                        \
			fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #c);   \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define ADD(v, n) __sync_fetch_and_add(&(v), (n))

/* ---------------------------------------------------------------------- */
/* DAOS constants and shapes                                               */
/* ---------------------------------------------------------------------- */

/* srv_internal.h:23-26 */
enum {
	DSS_POOL_NET_POLL = 0,
	DSS_POOL_NVME_POLL,
	DSS_POOL_GENERIC,
	DSS_POOL_CNT,
};

#define SCHED_AGE_NET_MAX  32  /* sched.c:1823 */
#define SCHED_AGE_NVME_MAX 64  /* sched.c:1824 */
#define SCHED_EVENT_FREQ   512 /* sched.c:2506, the event_freq config value */

#define NR_TGT		   3 /* "targets": one xstream each, plus the primary */
#define NR_NAMED	   4 /* named RPC-handler ULTs per target */
#define NR_UNNAMED	   3 /* unnamed RPC-handler ULTs per target */
#define NR_NVME_CYCLES	   5 /* NVMe-poll tasklets pushed per target */

/* sched.c:1792-1799, struct sched_cycle */
struct sched_cycle {
	uint32_t sc_ults_cnt[DSS_POOL_CNT];
	uint32_t sc_ults_tot;
	uint32_t sc_age_net;
	uint32_t sc_age_nvme;
	unsigned sc_new_cycle : 1;
	unsigned sc_cycle_started : 1;
};

/* srv_internal.h:89-119, struct dss_xstream (only the ABT-relevant fields) */
struct dss_xstream {
	int	     dx_xs_id;
	ABT_pool     dx_pools[DSS_POOL_CNT];
	ABT_sched    dx_sched;
	ABT_xstream  dx_xstream;
	volatile int dx_stopping; /* dx_stopping/dx_shutdown, srv.c:652,658 */
	volatile int dx_timeout;  /* usec; written by sched_try_relax, sched.c:2020 */
	/* counters, written only by this xstream's scheduler */
	uint64_t     dx_units_run;
	uint64_t     dx_cycles;
	uint64_t     dx_relax;
	uint64_t     dx_watchdog_prep;
	void	   (*dx_ult_func)(void *); /* si_ult_func, sched.c:2297 */
	int	     dx_sched_freed;
};

/* sched.c:1801-1806, struct sched_data */
struct sched_data {
	uint32_t	    sd_event_freq;
	struct dss_xstream *sd_dx;
	struct sched_cycle  sd_cycle;
};

static struct dss_xstream g_dx[NR_TGT];

/* ---------------------------------------------------------------------- */
/* Global counters, all asserted at the end                                */
/* ---------------------------------------------------------------------- */

static volatile int g_named_run;
static volatile int g_unnamed_run;
static volatile int g_net_polls;
static volatile int g_net_exits;
static volatile int g_tasklets_run;
static volatile int g_cond_waiting;
static volatile int g_cond_woken;
static volatile int g_gate;
static volatile int g_rw_reads;
static volatile int g_rw_writes;
static volatile int g_future_cb;
static volatile int g_ext_ult_run;
static volatile int g_ev_waiters;
static volatile int g_key_checks;
static volatile int g_relax_sleeps;
static volatile int g_run_unit_calls;

static long	    g_mutex_counter;   /* under g_mutex */
static long	    g_rmutex_counter;  /* under g_rmutex, recursive */
static volatile int g_rw_shared;       /* under g_rwlock, kept even outside */

/* ---------------------------------------------------------------------- */
/* Synchronization objects DAOS uses most (survey section 3)               */
/* ---------------------------------------------------------------------- */

static ABT_mutex    g_mutex;	 /* mutex_create, 41 sites */
static ABT_mutex    g_rmutex;	 /* recursive, vos/sys_db.c:389 */
static ABT_mutex    g_cond_mutex;
static ABT_cond	    g_cond;	 /* cond_broadcast, 52 sites */
static ABT_rwlock   g_rwlock;	 /* rwlock_create, 8 sites */
static ABT_eventual g_ev[NR_TGT];    /* sizeof(int), engine/rpc.c:36-56 */
static ABT_eventual g_ext_ev;	 /* set from an external pthread */
static ABT_eventual g_ext_ult_ev;    /* set by the pthread-created ULT */
static ABT_future   g_future;	 /* 2 compartments + reduce cb, ult.c:135 */
static ABT_key	    g_key;	 /* NULL destructor (DAOS uses no keys; see README) */

static int	    g_future_vals[2] = {7, 11};
static int	    g_future_seen[2];

/* ---------------------------------------------------------------------- */
/* The user-defined scheduler: DAOS's sched_init / sched_run / sched_free  */
/* ---------------------------------------------------------------------- */

/* sched.c:1827-1846 */
static int
sched_init(ABT_sched sched, ABT_sched_config config)
{
	struct sched_data *data;
	int		   ret;

	data = calloc(1, sizeof(*data));
	if (data == NULL)
		return ABT_ERR_MEM;

	/* the two user vars are read back positionally, idx 0 then idx 1 */
	ret = ABT_sched_config_read(config, 2, &data->sd_event_freq, &data->sd_dx);
	if (ret != ABT_SUCCESS) {
		free(data);
		return ret;
	}
	if (data->sd_event_freq != SCHED_EVENT_FREQ || data->sd_dx == NULL) {
		fprintf(stderr, "sched_init: config read back wrong: freq=%u dx=%p\n",
			data->sd_event_freq, (void *)data->sd_dx);
		free(data);
		return ABT_ERR_INV_SCHED_CONFIG;
	}

	return ABT_sched_set_data(sched, (void *)data);
}

/* sched.c:2421-2430 */
static int
sched_free(ABT_sched sched)
{
	struct sched_data *data = NULL;

	ABT_sched_get_data(sched, (void **)&data);
	if (data == NULL) {
		fprintf(stderr, "sched_free: no data\n");
		return ABT_ERR_INV_SCHED;
	}
	ASSERT(data->sd_event_freq == SCHED_EVENT_FREQ);
	ASSERT(data->sd_dx != NULL);
	data->sd_dx->dx_sched_freed = 1;
	free(data);
	return ABT_SUCCESS;
}

/* sched.c:1849-1871 */
static int
need_net_poll(struct sched_cycle *cycle)
{
	if (!cycle->sc_cycle_started)
		return 1; /* net poll starts a new cycle */
	if (cycle->sc_ults_tot == 0)
		return 0; /* an nvme poll ends it */
	if (cycle->sc_age_net > SCHED_AGE_NET_MAX)
		return 1;
	return 0;
}

/* sched.c:1872-1900 */
static ABT_unit
sched_pop_net_poll(struct sched_data *data, ABT_pool pool)
{
	struct sched_cycle *cycle = &data->sd_cycle;
	ABT_unit	    unit;
	int		    ret;

	if (!need_net_poll(cycle))
		return ABT_UNIT_NULL;

	cycle->sc_age_net = 0;
	cycle->sc_age_nvme++;
	if (cycle->sc_ults_tot == 0) {
		ASSERT(!cycle->sc_cycle_started);
		cycle->sc_new_cycle = 1;
	}

	ret = ABT_pool_pop(pool, &unit);
	if (ret != ABT_SUCCESS) {
		fprintf(stderr, "Failed to pop network poll ULT: %d\n", ret);
		return ABT_UNIT_NULL;
	}
	return unit;
}

/* sched.c:1903-1939 (the bio_need_nvme_poll() backlog test is mocked away) */
static int
need_nvme_poll(struct dss_xstream *dx, struct sched_cycle *cycle)
{
	(void)dx;
	if (!cycle->sc_cycle_started)
		return 0;
	if (cycle->sc_ults_tot == 0)
		return 1; /* nvme poll ends the cycle */
	if (cycle->sc_age_nvme > SCHED_AGE_NVME_MAX)
		return 1;
	return 0;
}

/* sched.c:1941-1969 */
static ABT_unit
sched_pop_nvme_poll(struct sched_data *data, ABT_pool pool)
{
	struct dss_xstream *dx	  = data->sd_dx;
	struct sched_cycle *cycle = &data->sd_cycle;
	ABT_unit	    unit;
	int		    ret;

	if (!need_nvme_poll(dx, cycle))
		return ABT_UNIT_NULL;

	ASSERT(cycle->sc_cycle_started);
	cycle->sc_age_nvme = 0;
	cycle->sc_age_net++;
	if (cycle->sc_ults_tot == 0)
		cycle->sc_cycle_started = 0;

	ret = ABT_pool_pop(pool, &unit);
	if (ret != ABT_SUCCESS) {
		fprintf(stderr, "Failed to pop NVMe poll ULT: %d\n", ret);
		return ABT_UNIT_NULL;
	}
	return unit;
}

/* sched.c:1971-2006 */
static ABT_unit
sched_pop_one(struct sched_data *data, ABT_pool pool, int pool_idx)
{
	struct sched_cycle *cycle = &data->sd_cycle;
	ABT_unit	    unit;
	int		    ret;

	ASSERT(cycle->sc_ults_tot >= cycle->sc_ults_cnt[pool_idx]);
	if (cycle->sc_ults_cnt[pool_idx] == 0)
		return ABT_UNIT_NULL;

	ret = ABT_pool_pop(pool, &unit);
	if (ret != ABT_SUCCESS) {
		fprintf(stderr, "Failed to pop ULT for ABT pool(%d): %d\n", pool_idx, ret);
		return ABT_UNIT_NULL;
	}
	/* a NULL unit is legal here: ABT_thread_join may have removed the ULT */

	cycle->sc_age_net++;
	cycle->sc_age_nvme++;
	cycle->sc_ults_cnt[pool_idx] -= 1;
	cycle->sc_ults_tot -= 1;

	return unit;
}

/*
 * sched.c:2020-2098, sched_try_relax().  DAOS counts blocked ULTs with
 * ABT_pool_get_total_size and then either lets the net-poll ULT block in
 * Mercury for dx_timeout, or usleep()s when there is nothing at all to run.
 */
static void
sched_try_relax(struct dss_xstream *dx, ABT_pool *pools, uint32_t running)
{
	size_t sz[DSS_POOL_CNT];
	size_t blocked = 0;
	int    i;

	if (running != 0) {
		dx->dx_timeout = 0;
		return;
	}

	for (i = 0; i < DSS_POOL_CNT; i++)
		CHECK(ABT_pool_get_size(pools[i], &sz[i]));
	/* total size = queued + blocked; sched.c:2046 */
	CHECK(ABT_pool_get_total_size(pools[DSS_POOL_GENERIC], &blocked));
	ASSERT(blocked >= sz[DSS_POOL_GENERIC]);

	dx->dx_relax++;
	if (sz[DSS_POOL_GENERIC] != 0 || sz[DSS_POOL_NVME_POLL] != 0) {
		dx->dx_timeout = 0;
		return;
	}
	/* idle: let the poll ULT wait instead of spinning (dx_timeout) */
	dx->dx_timeout = 100;
	if (sz[DSS_POOL_NET_POLL] == 0) {
		/* not even a poll ULT to run: sleep here, as DAOS does */
		ADD(g_relax_sleeps, 1);
		usleep(100);
	}
}

/* sched.c:2180-2233, sched_start_cycle() */
static void
sched_start_cycle(struct sched_data *data, ABT_pool *pools)
{
	struct dss_xstream *dx	  = data->sd_dx;
	struct sched_cycle *cycle = &data->sd_cycle;
	size_t		    cnt;
	int		    ret;

	ASSERT(cycle->sc_new_cycle == 1);
	ASSERT(cycle->sc_cycle_started == 0);
	ASSERT(cycle->sc_ults_tot == 0);

	cycle->sc_new_cycle	= 0;
	cycle->sc_cycle_started = 1;
	dx->dx_cycles++;

	ASSERT(cycle->sc_ults_cnt[DSS_POOL_GENERIC] == 0);
	ret = ABT_pool_get_size(pools[DSS_POOL_GENERIC], &cnt);
	if (ret != ABT_SUCCESS) {
		fprintf(stderr, "Get ABT pool(%d) size error: %d\n", DSS_POOL_GENERIC, ret);
		cnt = 0;
	}
	cycle->sc_ults_cnt[DSS_POOL_GENERIC] = (uint32_t)cnt;
	cycle->sc_ults_tot += (uint32_t)cnt;

	sched_try_relax(dx, pools, cycle->sc_ults_tot);
}

/* sched.c:2280-2298, sched_watchdog_prep() */
static void
sched_watchdog_prep(struct dss_xstream *dx, ABT_unit unit)
{
	ABT_thread thread	= ABT_THREAD_NULL;
	void	 (*thread_func)(void *) = NULL;
	int	   rc;

	rc = ABT_unit_get_thread(unit, &thread);
	ASSERT(rc == ABT_SUCCESS);
	rc = ABT_thread_get_thread_func(thread, &thread_func);
	ASSERT(rc == ABT_SUCCESS);
	dx->dx_ult_func = thread_func;
	dx->dx_watchdog_prep++;
}

/* sched.c:2345-2418, sched_run(): the loop body of every engine xstream */
static void
sched_run(ABT_sched sched)
{
	struct sched_data  *data;
	struct sched_cycle *cycle;
	struct dss_xstream *dx;
	ABT_pool	    pools[DSS_POOL_CNT];
	ABT_pool	    pool;
	ABT_unit	    unit;
	uint32_t	    work_count = 0;
	int		    ret;

	ABT_sched_get_data(sched, (void **)&data);
	ASSERT(data != NULL);
	cycle = &data->sd_cycle;
	dx    = data->sd_dx;

	ret = ABT_sched_get_pools(sched, DSS_POOL_CNT, 0, pools);
	if (ret != ABT_SUCCESS) {
		fprintf(stderr, "Get ABT pools error: %d\n", ret);
		return;
	}
	ASSERT(pools[DSS_POOL_NET_POLL] == dx->dx_pools[DSS_POOL_NET_POLL]);
	ASSERT(pools[DSS_POOL_NVME_POLL] == dx->dx_pools[DSS_POOL_NVME_POLL]);
	ASSERT(pools[DSS_POOL_GENERIC] == dx->dx_pools[DSS_POOL_GENERIC]);

	while (1) {
		/* Try to pick network poll ULT */
		pool = pools[DSS_POOL_NET_POLL];
		unit = sched_pop_net_poll(data, pool);
		if (unit != ABT_UNIT_NULL)
			goto execute;

		/* Try to pick NVMe poll ULT */
		pool = pools[DSS_POOL_NVME_POLL];
		unit = sched_pop_nvme_poll(data, pool);
		if (unit != ABT_UNIT_NULL)
			goto execute;

		if (cycle->sc_ults_tot == 0)
			goto start_cycle;

		/* Try to pick a ULT from the generic ABT pool */
		pool = pools[DSS_POOL_GENERIC];
		unit = sched_pop_one(data, pool, DSS_POOL_GENERIC);
		if (unit != ABT_UNIT_NULL)
			goto execute;

		goto check_event;
execute:
		ASSERT(pool != ABT_POOL_NULL);
		sched_watchdog_prep(dx, unit);

		ADD(g_run_unit_calls, 1);
		CHECK(ABT_xstream_run_unit(unit, pool));

		dx->dx_units_run++;
start_cycle:
		if (cycle->sc_new_cycle)
			sched_start_cycle(data, pools);
check_event:
		if (++work_count >= data->sd_event_freq) {
			ABT_bool stop;

			CHECK(ABT_sched_has_to_stop(sched, &stop));
			if (stop == ABT_TRUE)
				break;
			work_count = 0;
			CHECK(ABT_xstream_check_events(sched));
		}
	}
}

/* ---------------------------------------------------------------------- */
/* The work DAOS puts in those pools                                       */
/* ---------------------------------------------------------------------- */

/* srv.c:406-600, dss_srv_handler(): the net-poll ULT, yields forever */
static void
net_poll_ult(void *arg)
{
	struct dss_xstream *dx = arg;
	ABT_thread	    self;
	ABT_pool	    last;

	CHECK(ABT_thread_self(&self));
	for (;;) {
		/* crt_progress(ctx, dx_timeout) -- the engine's idle mechanism */
		if (dx->dx_timeout)
			usleep((useconds_t)dx->dx_timeout);
		ADD(g_net_polls, 1);

		if (dx->dx_stopping) /* dss_xstream_exiting(dx) */
			break;

		CHECK(ABT_thread_yield());
		/*
		 * Exact yield-to-pool semantics are required: the yield must
		 * return this ULT to DSS_POOL_NET_POLL and nowhere else.
		 */
		CHECK(ABT_thread_get_last_pool(self, &last));
		ASSERT(last == dx->dx_pools[DSS_POOL_NET_POLL]);
	}
	ADD(g_net_exits, 1);
}

/* srv.c:338-345 + bio_nvme_poll(): the NVMe poll work, a tasklet here */
static void
nvme_poll_task(void *arg)
{
	struct dss_xstream *dx = arg;
	int		    rank = -1;

	CHECK(ABT_self_get_xstream_rank(&rank));
	ASSERT(rank == dx->dx_xs_id + 1);
	ADD(g_tasklets_run, 1);
}

struct handler_arg {
	struct dss_xstream *dx;
	int		    id;
};

static struct handler_arg g_named_args[NR_TGT][NR_NAMED];
static struct handler_arg g_unnamed_args[NR_TGT][NR_UNNAMED];
static struct handler_arg g_cond_args[NR_TGT];
static struct handler_arg g_ev_wait_args[NR_TGT];
static struct handler_arg g_ev_set_args[NR_TGT];
static struct handler_arg g_future_args[2];

/*
 * A named RPC handler: the shape of dss_rpc_hdlr / a collective task.  Uses
 * the objects DAOS uses most, and checks that per-ULT key storage and the
 * xstream rank survive a yield.
 */
static void
rpc_handler(void *arg)
{
	struct handler_arg *ha	 = arg;
	int		    rank = -1;
	void		   *kv	 = NULL;
	int		    i;

	CHECK(ABT_self_get_xstream_rank(&rank));
	ASSERT(rank == ha->dx->dx_xs_id + 1);

	/* ABT_key with a NULL destructor, set and read back across a yield */
	CHECK(ABT_key_set(g_key, ha));
	CHECK(ABT_thread_yield());
	CHECK(ABT_key_get(g_key, &kv));
	ASSERT(kv == (void *)ha);
	ADD(g_key_checks, 1);

	/* M3: a ULT must never migrate between xstreams */
	CHECK(ABT_self_get_xstream_rank(&rank));
	ASSERT(rank == ha->dx->dx_xs_id + 1);

	for (i = 0; i < 64; i++) {
		CHECK(ABT_mutex_lock(g_mutex));
		g_mutex_counter++;
		CHECK(ABT_mutex_unlock(g_mutex));
		if ((i & 15) == 0)
			CHECK(ABT_thread_yield());
	}

	/* the one recursive mutex: vos/sys_db.c re-enters through callbacks */
	CHECK(ABT_mutex_lock(g_rmutex));
	CHECK(ABT_mutex_lock(g_rmutex));
	g_rmutex_counter += 2;
	CHECK(ABT_mutex_unlock(g_rmutex));
	CHECK(ABT_mutex_unlock(g_rmutex));

	/* rwlock: readers, plus a writer on every fourth handler */
	if ((ha->id & 3) == 3) {
		for (i = 0; i < 16; i++) {
			CHECK(ABT_rwlock_wrlock(g_rwlock));
			g_rw_shared++;
			if ((i & 3) == 0)
				CHECK(ABT_thread_yield());
			g_rw_shared++;
			CHECK(ABT_rwlock_unlock(g_rwlock));
			ADD(g_rw_writes, 1);
		}
	} else {
		for (i = 0; i < 32; i++) {
			int v;

			CHECK(ABT_rwlock_rdlock(g_rwlock));
			v = g_rw_shared;
			ASSERT((v & 1) == 0); /* writers leave it even */
			CHECK(ABT_rwlock_unlock(g_rwlock));
			ADD(g_rw_reads, 1);
		}
	}

	ADD(g_named_run, 1);
}

/* ult.c:195 / :655, dss_ult_create_all(): handlers created with newthread == NULL */
static void
rpc_handler_unnamed(void *arg)
{
	struct handler_arg *ha	 = arg;
	int		    rank = -1;
	int		    i;

	CHECK(ABT_self_get_xstream_rank(&rank));
	ASSERT(rank == ha->dx->dx_xs_id + 1);
	for (i = 0; i < 8; i++) {
		CHECK(ABT_mutex_lock(g_mutex));
		g_mutex_counter++;
		CHECK(ABT_mutex_unlock(g_mutex));
		CHECK(ABT_thread_yield());
	}
	ADD(g_unnamed_run, 1);
}

/* ult.c:806-920, dss_chore_queue_ult(): blocks on a cond, woken by broadcast */
static void
cond_waiter(void *arg)
{
	struct handler_arg *ha = arg;

	(void)ha;
	CHECK(ABT_mutex_lock(g_cond_mutex));
	ADD(g_cond_waiting, 1);
	while (!g_gate)
		CHECK(ABT_cond_wait(g_cond, g_cond_mutex));
	ADD(g_cond_woken, 1);
	CHECK(ABT_mutex_unlock(g_cond_mutex));
}

/* engine/rpc.c:36-56: the offload-completion idiom, eventual of sizeof(int) */
static void
eventual_waiter(void *arg)
{
	struct handler_arg *ha = arg;
	int		   *v;

	CHECK(ABT_eventual_wait(g_ev[ha->id], (void **)&v));
	ASSERT(*v == 100 + ha->id);
	ADD(g_ev_waiters, 1);
}

static void
eventual_setter(void *arg)
{
	struct handler_arg *ha = arg;
	int		    v  = 100 + ha->id;

	CHECK(ABT_eventual_set(g_ev[ha->id], &v, sizeof(v)));
}

/* ult.c:135, collective_reduce(): a future with a real reduce callback */
static void
future_cb(void **args)
{
	g_future_seen[0] = *(int *)args[0];
	g_future_seen[1] = *(int *)args[1];
	ADD(g_future_cb, 1);
}

static void
future_setter(void *arg)
{
	struct handler_arg *ha = arg;

	CHECK(ABT_future_set(g_future, &g_future_vals[ha->id]));
}

/* The ULT an external pthread creates (SPDK/NVMe pthread shape) */
static void
ext_created_ult(void *arg)
{
	int rank = -1;

	(void)arg;
	CHECK(ABT_self_get_xstream_rank(&rank));
	ASSERT(rank == g_dx[NR_TGT - 1].dx_xs_id + 1);
	CHECK(ABT_thread_yield());
	ADD(g_ext_ult_run, 1);
	CHECK(ABT_eventual_set(g_ext_ult_ev, NULL, 0));
}

static void *
ext_pthread(void *arg)
{
	struct dss_xstream *dx = arg;
	int		    v  = 4242;

	usleep(5000);
	/* an external thread pushing a ULT into an xstream's generic pool */
	CHECK(ABT_thread_create(dx->dx_pools[DSS_POOL_GENERIC], ext_created_ult, NULL,
				ABT_THREAD_ATTR_NULL, NULL));
	/* and completing an offload, as bio_monitor.c:158,86 does */
	CHECK(ABT_eventual_set(g_ext_ev, &v, sizeof(v)));
	return NULL;
}

/* ---------------------------------------------------------------------- */
/* dss_sched_init / dss_sched_fini                                         */
/* ---------------------------------------------------------------------- */

/* sched.c:2445-2465, sched_create_pools() */
static int
sched_create_pools(struct dss_xstream *dx)
{
	int i, rc;

	for (i = 0; i < DSS_POOL_CNT; i++) {
		ASSERT(dx->dx_pools[i] == ABT_POOL_NULL);
		rc = ABT_pool_create_basic(ABT_POOL_FIFO, ABT_POOL_ACCESS_MPSC, ABT_TRUE,
					   &dx->dx_pools[i]);
		if (rc != ABT_SUCCESS)
			return rc;
	}
	return ABT_SUCCESS;
}

/* sched.c:2432-2443, sched_free_pools(): DAOS's dss_sched_init error path */
static void
sched_free_pools(struct dss_xstream *dx)
{
	int i;

	for (i = 0; i < DSS_POOL_CNT; i++) {
		if (dx->dx_pools[i] != ABT_POOL_NULL) {
			CHECK(ABT_pool_free(&dx->dx_pools[i]));
			ASSERT(dx->dx_pools[i] == ABT_POOL_NULL);
		}
	}
}

/* sched.c:2476-2521, dss_sched_init() */
static int
dss_sched_init(struct dss_xstream *dx)
{
	ABT_sched_config     config;
	ABT_sched_config_var event_freq = {.idx = 0, .type = ABT_SCHED_CONFIG_INT};
	ABT_sched_config_var dx_ptr	= {.idx = 1, .type = ABT_SCHED_CONFIG_PTR};
	ABT_sched_def	     sched_def	= {.type	  = ABT_SCHED_TYPE_ULT,
					   .init	  = sched_init,
					   .run		  = sched_run,
					   .free	  = sched_free,
					   .get_migr_pool = NULL};
	int		     rc;

	rc = sched_create_pools(dx);
	if (rc != ABT_SUCCESS)
		return rc;

	rc = ABT_sched_config_create(&config, event_freq, SCHED_EVENT_FREQ, dx_ptr, dx,
				     ABT_sched_config_var_end);
	if (rc != ABT_SUCCESS)
		return rc;

	rc = ABT_sched_create(&sched_def, DSS_POOL_CNT, dx->dx_pools, config, &dx->dx_sched);
	CHECK(ABT_sched_config_free(&config));
	ASSERT(config == ABT_SCHED_CONFIG_NULL);

	return rc;
}

/* ---------------------------------------------------------------------- */
/* helpers for the primary                                                 */
/* ---------------------------------------------------------------------- */

/* The primary ULT waits for a counter, yielding like DAOS's own poll loops. */
static void
wait_for(volatile int *counter, int target, const char *what)
{
	int spins = 0;

	while (__sync_fetch_and_add(counter, 0) < target) {
		CHECK(ABT_thread_yield());
		usleep(200);
		if (++spins > 150000) {
			fprintf(stderr, "timeout waiting for %s: %d < %d\n", what, *counter,
				target);
			exit(1);
		}
	}
}

int
main(int argc, char **argv)
{
	pthread_t  ext;
	ABT_thread net_ult[NR_TGT];
	ABT_thread named[NR_TGT][NR_NAMED];
	ABT_thread cond_ult[NR_TGT];
	ABT_thread ev_wait[NR_TGT];
	ABT_thread ev_set[NR_TGT];
	ABT_thread fut[2];
	ABT_bool   unnamed_flag;
	int	   t, i, c, rank;
	int	  *evp;

	setvbuf(stdout, NULL, _IONBF, 0);

	/*
	 * init.c:525-551: DAOS sets ABT_MAX_NUM_XSTREAMS to 1 + the number of
	 * xstreams it will create, before ABT_init, then initializes ABT on the
	 * process main thread, which becomes the primary xstream (rank 0).
	 */
	{
		char buf[32];

		snprintf(buf, sizeof buf, "%d", 1 + NR_TGT);
		setenv("ABT_MAX_NUM_XSTREAMS", buf, 1);
	}
	CHECK(ABT_init(argc, argv));

	CHECK(ABT_xstream_self_rank(&rank));
	ASSERT(rank == 0); /* "ABT rank 0 is reserved for the primary xstream" */

	/* --- the sync objects, all created dynamically (no *_INITIALIZER) --- */
	CHECK(ABT_mutex_create(&g_mutex));
	{
		ABT_mutex_attr attr;

		CHECK(ABT_mutex_attr_create(&attr));
		CHECK(ABT_mutex_attr_set_recursive(attr, ABT_TRUE));
		CHECK(ABT_mutex_create_with_attr(attr, &g_rmutex));
		CHECK(ABT_mutex_attr_free(&attr));
	}
	CHECK(ABT_mutex_create(&g_cond_mutex));
	CHECK(ABT_cond_create(&g_cond));
	CHECK(ABT_rwlock_create(&g_rwlock));
	for (t = 0; t < NR_TGT; t++)
		CHECK(ABT_eventual_create((int)sizeof(int), &g_ev[t]));
	CHECK(ABT_eventual_create((int)sizeof(int), &g_ext_ev));
	CHECK(ABT_eventual_create(0, &g_ext_ult_ev)); /* pure signalling */
	CHECK(ABT_future_create(2, future_cb, &g_future));
	CHECK(ABT_key_create(NULL, &g_key));

	/* --- dss_sched_init + ABT_xstream_create_with_rank per target ------ */
	for (t = 0; t < NR_TGT; t++) {
		struct dss_xstream *dx = &g_dx[t];

		dx->dx_xs_id = t;
		for (i = 0; i < DSS_POOL_CNT; i++)
			dx->dx_pools[i] = ABT_POOL_NULL;
		dx->dx_sched = ABT_SCHED_NULL;

		CHECK(dss_sched_init(dx));
		ASSERT(dx->dx_sched != ABT_SCHED_NULL);
		/* srv.c:839: rank xs_id + 1, rank 0 is the primary's */
		CHECK(ABT_xstream_create_with_rank(dx->dx_sched, t + 1, &dx->dx_xstream));
	}

	/* --- the net-poll ULT per target: srv.c:852-860 --------------------- */
	for (t = 0; t < NR_TGT; t++)
		CHECK(ABT_thread_create(g_dx[t].dx_pools[DSS_POOL_NET_POLL], net_poll_ult, &g_dx[t],
					ABT_THREAD_ATTR_NULL, &net_ult[t]));

	/* --- RPC handlers into each target's generic pool ------------------- */
	for (t = 0; t < NR_TGT; t++) {
		for (i = 0; i < NR_NAMED; i++) {
			g_named_args[t][i].dx = &g_dx[t];
			g_named_args[t][i].id = i;
			/* sched_create_thread(), srv_internal.h:311-330 */
			CHECK(ABT_thread_create(g_dx[t].dx_pools[DSS_POOL_GENERIC], rpc_handler,
						&g_named_args[t][i], ABT_THREAD_ATTR_NULL,
						&named[t][i]));
			CHECK(ABT_thread_is_unnamed(named[t][i], &unnamed_flag));
			ASSERT(unnamed_flag == ABT_FALSE); /* sched.c:1657 */
		}
		for (i = 0; i < NR_UNNAMED; i++) {
			g_unnamed_args[t][i].dx = &g_dx[t];
			g_unnamed_args[t][i].id = i;
			CHECK(ABT_thread_create(g_dx[t].dx_pools[DSS_POOL_GENERIC],
						rpc_handler_unnamed, &g_unnamed_args[t][i],
						ABT_THREAD_ATTR_NULL, NULL));
		}

		g_cond_args[t].dx = &g_dx[t];
		g_cond_args[t].id = t;
		CHECK(ABT_thread_create(g_dx[t].dx_pools[DSS_POOL_GENERIC], cond_waiter,
					&g_cond_args[t], ABT_THREAD_ATTR_NULL, &cond_ult[t]));
	}

	/* --- eventual: waiter on target t, setter on target (t+1) % N ------- */
	for (t = 0; t < NR_TGT; t++) {
		g_ev_wait_args[t].dx = &g_dx[t];
		g_ev_wait_args[t].id = t;
		CHECK(ABT_thread_create(g_dx[t].dx_pools[DSS_POOL_GENERIC], eventual_waiter,
					&g_ev_wait_args[t], ABT_THREAD_ATTR_NULL, &ev_wait[t]));
	}
	for (t = 0; t < NR_TGT; t++) {
		int setter_xs	    = (t + 1) % NR_TGT;

		g_ev_set_args[t].dx = &g_dx[setter_xs];
		g_ev_set_args[t].id = t;
		CHECK(ABT_thread_create(g_dx[setter_xs].dx_pools[DSS_POOL_GENERIC], eventual_setter,
					&g_ev_set_args[t], ABT_THREAD_ATTR_NULL, &ev_set[t]));
	}

	/* --- future: two compartments set from two different targets -------- */
	for (i = 0; i < 2; i++) {
		g_future_args[i].dx = &g_dx[i];
		g_future_args[i].id = i;
		CHECK(ABT_thread_create(g_dx[i].dx_pools[DSS_POOL_GENERIC], future_setter,
					&g_future_args[i], ABT_THREAD_ATTR_NULL, &fut[i]));
	}

	/* --- one ULT created from an external pthread ----------------------- */
	ASSERT(pthread_create(&ext, NULL, ext_pthread, &g_dx[NR_TGT - 1]) == 0);

	/* --- NVMe-poll tasklets, a few per cycle ---------------------------- */
	for (c = 0; c < NR_NVME_CYCLES; c++) {
		for (t = 0; t < NR_TGT; t++) {
			ABT_task task;

			/* sched_create_task(), srv_internal.h:307 */
			CHECK(ABT_task_create(g_dx[t].dx_pools[DSS_POOL_NVME_POLL], nvme_poll_task,
					      &g_dx[t], &task));
			/* srv.c:1520: create then free, i.e. join the tasklet */
			CHECK(ABT_task_free(&task));
		}
	}
	ASSERT(g_tasklets_run == NR_NVME_CYCLES * NR_TGT);

	/* --- cond broadcast once every waiter is actually blocked ----------- */
	wait_for(&g_cond_waiting, NR_TGT, "cond waiters");
	CHECK(ABT_mutex_lock(g_cond_mutex));
	g_gate = 1;
	CHECK(ABT_cond_broadcast(g_cond));
	CHECK(ABT_mutex_unlock(g_cond_mutex));

	/* --- the primary waits on the future and the external eventuals ----- */
	CHECK(ABT_future_wait(g_future));
	{
		ABT_bool ready;

		CHECK(ABT_future_test(g_future, &ready));
		ASSERT(ready == ABT_TRUE); /* srv_internal.h:286 polls this way */
	}
	CHECK(ABT_eventual_wait(g_ext_ev, (void **)&evp));
	ASSERT(*evp == 4242);
	CHECK(ABT_eventual_wait(g_ext_ult_ev, NULL));
	ASSERT(pthread_join(ext, NULL) == 0);

	/* --- drain: join the named ULTs, wait out the unnamed ones ---------- */
	for (t = 0; t < NR_TGT; t++) {
		for (i = 0; i < NR_NAMED; i++) {
			CHECK(ABT_thread_join(named[t][i]));
			CHECK(ABT_thread_free(&named[t][i]));
		}
		CHECK(ABT_thread_join(cond_ult[t]));
		CHECK(ABT_thread_free(&cond_ult[t]));
		CHECK(ABT_thread_join(ev_wait[t]));
		CHECK(ABT_thread_free(&ev_wait[t]));
		CHECK(ABT_thread_join(ev_set[t]));
		CHECK(ABT_thread_free(&ev_set[t]));
	}
	for (i = 0; i < 2; i++) {
		CHECK(ABT_thread_join(fut[i]));
		CHECK(ABT_thread_free(&fut[i]));
	}
	wait_for(&g_unnamed_run, NR_TGT * NR_UNNAMED, "unnamed handlers");
	wait_for(&g_ext_ult_run, 1, "pthread-created ULT");

	/* --- shutdown, as DAOS does ---------------------------------------- */
	for (t = 0; t < NR_TGT; t++)
		g_dx[t].dx_stopping = 1; /* dss_srv_set_shutting_down() */
	for (t = 0; t < NR_TGT; t++) {
		CHECK(ABT_thread_join(net_ult[t]));
		CHECK(ABT_thread_free(&net_ult[t]));
	}
	for (t = 0; t < NR_TGT; t++) {
		struct dss_xstream *dx = &g_dx[t];

		/* srv.c:889-890 / :943-944 */
		CHECK(ABT_xstream_join(dx->dx_xstream));
		CHECK(ABT_xstream_free(&dx->dx_xstream));
		/* dss_sched_fini(), sched.c:2467-2473; frees the pools too */
		CHECK(ABT_sched_free(&dx->dx_sched));
		ASSERT(dx->dx_sched == ABT_SCHED_NULL);
		ASSERT(dx->dx_sched_freed == 1);
	}

	/*
	 * DAOS's dss_sched_init error path: pools created, no scheduler, freed
	 * explicitly with ABT_pool_free (sched_free_pools, sched.c:2432).
	 */
	{
		struct dss_xstream tmp;

		memset(&tmp, 0, sizeof tmp);
		for (i = 0; i < DSS_POOL_CNT; i++)
			tmp.dx_pools[i] = ABT_POOL_NULL;
		CHECK(sched_create_pools(&tmp));
		sched_free_pools(&tmp);
	}

	/* --- free the sync objects ----------------------------------------- */
	for (t = 0; t < NR_TGT; t++)
		CHECK(ABT_eventual_free(&g_ev[t]));
	CHECK(ABT_eventual_free(&g_ext_ev));
	CHECK(ABT_eventual_free(&g_ext_ult_ev));
	CHECK(ABT_future_free(&g_future));
	CHECK(ABT_key_free(&g_key));
	CHECK(ABT_rwlock_free(&g_rwlock));
	CHECK(ABT_cond_free(&g_cond));
	CHECK(ABT_mutex_free(&g_cond_mutex));
	CHECK(ABT_mutex_free(&g_rmutex));
	CHECK(ABT_mutex_free(&g_mutex));

	CHECK(ABT_finalize());

	/* --- every counter ------------------------------------------------- */
	ASSERT(g_named_run == NR_TGT * NR_NAMED);
	ASSERT(g_unnamed_run == NR_TGT * NR_UNNAMED);
	ASSERT(g_key_checks == NR_TGT * NR_NAMED);
	ASSERT(g_cond_waiting == NR_TGT);
	ASSERT(g_cond_woken == NR_TGT);
	ASSERT(g_ev_waiters == NR_TGT);
	ASSERT(g_future_cb == 1);
	/* compartments fill in set order, so either order is correct */
	ASSERT(g_future_seen[0] + g_future_seen[1] == g_future_vals[0] + g_future_vals[1]);
	ASSERT(g_future_seen[0] != g_future_seen[1]);
	ASSERT(g_ext_ult_run == 1);
	ASSERT(g_tasklets_run == NR_NVME_CYCLES * NR_TGT);
	ASSERT(g_net_polls > NR_TGT); /* every target polled at least once */
	ASSERT(g_net_exits == NR_TGT);
	ASSERT(g_mutex_counter == (long)(NR_TGT * (NR_NAMED * 64 + NR_UNNAMED * 8)));
	ASSERT(g_rmutex_counter == (long)(NR_TGT * NR_NAMED * 2));
	ASSERT(g_rw_writes == NR_TGT * (NR_NAMED / 4) * 16);
	ASSERT(g_rw_reads == NR_TGT * (NR_NAMED - NR_NAMED / 4) * 32);
	ASSERT(g_rw_shared == 2 * g_rw_writes);
	ASSERT(g_run_unit_calls >= g_named_run + g_unnamed_run + g_tasklets_run);
	for (t = 0; t < NR_TGT; t++) {
		ASSERT(g_dx[t].dx_units_run > 0);
		ASSERT(g_dx[t].dx_cycles > 0);
		ASSERT(g_dx[t].dx_relax > 0);
		ASSERT(g_dx[t].dx_watchdog_prep == g_dx[t].dx_units_run);
		ASSERT(g_dx[t].dx_sched_freed == 1);
	}

	printf("targets=%d units_run=%llu/%llu/%llu cycles=%llu/%llu/%llu "
	       "net_polls=%d tasklets=%d relax_sleeps=%d\n",
	       NR_TGT, (unsigned long long)g_dx[0].dx_units_run,
	       (unsigned long long)g_dx[1].dx_units_run, (unsigned long long)g_dx[2].dx_units_run,
	       (unsigned long long)g_dx[0].dx_cycles, (unsigned long long)g_dx[1].dx_cycles,
	       (unsigned long long)g_dx[2].dx_cycles, g_net_polls, g_tasklets_run, g_relax_sleeps);
	printf("daos_mock ok\n");
	return 0;
}
