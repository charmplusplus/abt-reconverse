/*
 * abt-reconverse: an Argobots (ABT) C ABI implemented on top of Reconverse.
 *
 * Error strings, timers, info queries/printing, and the tool interface.
 *
 * Only the tool interface is still unimplemented here: Margo gates on
 * ABT_INFO_QUERY_KIND_ENABLED_TOOL == ABT_FALSE (which this file reports) and
 * then never calls ABT_tool_*.
 */

#include "abti.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <unistd.h>

namespace {
/* Generated from the ABT_SUCCESS / ABT_ERR_* defines in abt.h. */
const char *abti_error_name(int err) noexcept
{
    switch (err) {
    case ABT_SUCCESS: return "ABT_SUCCESS";
    case ABT_ERR_UNINITIALIZED: return "ABT_ERR_UNINITIALIZED";
    case ABT_ERR_MEM: return "ABT_ERR_MEM";
    case ABT_ERR_OTHER: return "ABT_ERR_OTHER";
    case ABT_ERR_INV_XSTREAM: return "ABT_ERR_INV_XSTREAM";
    case ABT_ERR_INV_XSTREAM_RANK: return "ABT_ERR_INV_XSTREAM_RANK";
    case ABT_ERR_INV_XSTREAM_BARRIER: return "ABT_ERR_INV_XSTREAM_BARRIER";
    case ABT_ERR_INV_SCHED: return "ABT_ERR_INV_SCHED";
    case ABT_ERR_INV_SCHED_KIND: return "ABT_ERR_INV_SCHED_KIND";
    case ABT_ERR_INV_SCHED_PREDEF: return "ABT_ERR_INV_SCHED_PREDEF";
    case ABT_ERR_INV_SCHED_TYPE: return "ABT_ERR_INV_SCHED_TYPE";
    case ABT_ERR_INV_SCHED_CONFIG: return "ABT_ERR_INV_SCHED_CONFIG";
    case ABT_ERR_INV_POOL: return "ABT_ERR_INV_POOL";
    case ABT_ERR_INV_POOL_KIND: return "ABT_ERR_INV_POOL_KIND";
    case ABT_ERR_INV_POOL_ACCESS: return "ABT_ERR_INV_POOL_ACCESS";
    case ABT_ERR_INV_UNIT: return "ABT_ERR_INV_UNIT";
    case ABT_ERR_INV_THREAD: return "ABT_ERR_INV_THREAD";
    case ABT_ERR_INV_THREAD_ATTR: return "ABT_ERR_INV_THREAD_ATTR";
    case ABT_ERR_INV_TASK: return "ABT_ERR_INV_TASK";
    case ABT_ERR_INV_KEY: return "ABT_ERR_INV_KEY";
    case ABT_ERR_INV_MUTEX: return "ABT_ERR_INV_MUTEX";
    case ABT_ERR_INV_MUTEX_ATTR: return "ABT_ERR_INV_MUTEX_ATTR";
    case ABT_ERR_INV_COND: return "ABT_ERR_INV_COND";
    case ABT_ERR_INV_RWLOCK: return "ABT_ERR_INV_RWLOCK";
    case ABT_ERR_INV_EVENTUAL: return "ABT_ERR_INV_EVENTUAL";
    case ABT_ERR_INV_FUTURE: return "ABT_ERR_INV_FUTURE";
    case ABT_ERR_INV_BARRIER: return "ABT_ERR_INV_BARRIER";
    case ABT_ERR_INV_TIMER: return "ABT_ERR_INV_TIMER";
    case ABT_ERR_INV_QUERY_KIND: return "ABT_ERR_INV_QUERY_KIND";
    case ABT_ERR_XSTREAM: return "ABT_ERR_XSTREAM";
    case ABT_ERR_XSTREAM_STATE: return "ABT_ERR_XSTREAM_STATE";
    case ABT_ERR_XSTREAM_BARRIER: return "ABT_ERR_XSTREAM_BARRIER";
    case ABT_ERR_SCHED: return "ABT_ERR_SCHED";
    case ABT_ERR_SCHED_CONFIG: return "ABT_ERR_SCHED_CONFIG";
    case ABT_ERR_POOL: return "ABT_ERR_POOL";
    case ABT_ERR_UNIT: return "ABT_ERR_UNIT";
    case ABT_ERR_THREAD: return "ABT_ERR_THREAD";
    case ABT_ERR_TASK: return "ABT_ERR_TASK";
    case ABT_ERR_KEY: return "ABT_ERR_KEY";
    case ABT_ERR_MUTEX: return "ABT_ERR_MUTEX";
    case ABT_ERR_MUTEX_LOCKED: return "ABT_ERR_MUTEX_LOCKED";
    case ABT_ERR_COND: return "ABT_ERR_COND";
    case ABT_ERR_COND_TIMEDOUT: return "ABT_ERR_COND_TIMEDOUT";
    case ABT_ERR_RWLOCK: return "ABT_ERR_RWLOCK";
    case ABT_ERR_EVENTUAL: return "ABT_ERR_EVENTUAL";
    case ABT_ERR_FUTURE: return "ABT_ERR_FUTURE";
    case ABT_ERR_BARRIER: return "ABT_ERR_BARRIER";
    case ABT_ERR_TIMER: return "ABT_ERR_TIMER";
    case ABT_ERR_MIGRATION_TARGET: return "ABT_ERR_MIGRATION_TARGET";
    case ABT_ERR_MIGRATION_NA: return "ABT_ERR_MIGRATION_NA";
    case ABT_ERR_MISSING_JOIN: return "ABT_ERR_MISSING_JOIN";
    case ABT_ERR_FEATURE_NA: return "ABT_ERR_FEATURE_NA";
    case ABT_ERR_INV_TOOL_CONTEXT: return "ABT_ERR_INV_TOOL_CONTEXT";
    case ABT_ERR_INV_ARG: return "ABT_ERR_INV_ARG";
    case ABT_ERR_SYS: return "ABT_ERR_SYS";
    case ABT_ERR_CPUID: return "ABT_ERR_CPUID";
    case ABT_ERR_INV_POOL_CONFIG: return "ABT_ERR_INV_POOL_CONFIG";
    case ABT_ERR_INV_POOL_USER_DEF: return "ABT_ERR_INV_POOL_USER_DEF";
    default: return nullptr;
    }
}
} /* namespace */

extern "C" {

/*
 * Not a stub: Margo prints these names when a call fails, so the shim must
 * answer correctly from day one.  ABT_ENABLE_VER_20_API is 0 in this header, so
 * an unknown code is ABT_ERR_OTHER (the 2.0 API would say ABT_ERR_INV_ARG).
 */
int ABT_error_get_str(int err, char *str, size_t *len)
{
    const char *name = abti_error_name(err);
    if (!name)
        return ABT_ERR_OTHER;
    if (str)
        strcpy(str, name);
    if (len)
        *len = strlen(name);
    return ABT_SUCCESS;
}

} /* extern "C" */

/* ---- timers -------------------------------------------------------------
 * ABT_timer must work irrespective of ABT_init (test/basic/timer.c creates one
 * before ABT_init), so this uses new/delete and CmiWallTimer only, no global
 * state.  CmiWallTimer before ConverseInit returns an absolute clock reading;
 * differences are still correct, which is all a timer exposes. */

struct ABTI_timer {
    double start;
    double end;
};

namespace {
inline ABTI_timer *abti_timer_get(ABT_timer h)
{
    return ABTI_is_null_handle((const void *)h)
               ? nullptr
               : reinterpret_cast<ABTI_timer *>(h);
}
inline ABT_timer abti_timer_handle(ABTI_timer *t)
{
    return reinterpret_cast<ABT_timer>(t);
}

/* Values that must be identical before and after ABT_init (info_query.c checks
 * that consecutive queries agree), so they are read from the same environment
 * variables ABT_init uses, and from ABTI_g once it exists. */
long abti_env_long(const char *name, long dflt)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return dflt;
    char *end;
    long x = strtol(v, &end, 10);
    return (*end == 0 && x > 0) ? x : dflt;
}
unsigned int abti_max_xstreams(void)
{
    if (ABTI_g)
        return (unsigned int)ABTI_g->num_pes;
    return (unsigned int)ABTI_num_pes_rule();
}
size_t abti_default_stacksize(void)
{
    if (ABTI_g)
        return ABTI_g->default_stacksize;
    long s = abti_env_long("ABT_THREAD_STACKSIZE", 2 * 1024 * 1024);
    return (size_t)(s < 16384 ? 16384 : s);
}

const char *abti_xstream_state_name(int s)
{
    switch (s) {
    case ABT_XSTREAM_STATE_RUNNING: return "RUNNING";
    case ABT_XSTREAM_STATE_TERMINATED: return "TERMINATED";
    default: return "UNKNOWN";
    }
}
const char *abti_thread_state_name(ABT_thread_state s)
{
    switch (s) {
    case ABT_THREAD_STATE_READY: return "READY";
    case ABT_THREAD_STATE_RUNNING: return "RUNNING";
    case ABT_THREAD_STATE_BLOCKED: return "BLOCKED";
    case ABT_THREAD_STATE_TERMINATED: return "TERMINATED";
    default: return "UNKNOWN";
    }
}
const char *abti_sched_predef_name(ABT_sched_predef p)
{
    switch (p) {
    case ABT_SCHED_DEFAULT: return "DEFAULT";
    case ABT_SCHED_BASIC: return "BASIC";
    case ABT_SCHED_PRIO: return "PRIO";
    case ABT_SCHED_RANDWS: return "RANDWS";
    case ABT_SCHED_BASIC_WAIT: return "BASIC_WAIT";
    default: return "UNKNOWN";
    }
}
const char *abti_pool_kind_name(ABT_pool_kind k)
{
    switch (k) {
    case ABT_POOL_FIFO: return "FIFO";
    case ABT_POOL_FIFO_WAIT: return "FIFO_WAIT";
    case ABT_POOL_RANDWS: return "RANDWS";
    default: return "USER";
    }
}
const char *abti_pool_access_name(ABT_pool_access a)
{
    switch (a) {
    case ABT_POOL_ACCESS_PRIV: return "PRIV";
    case ABT_POOL_ACCESS_SPSC: return "SPSC";
    case ABT_POOL_ACCESS_MPSC: return "MPSC";
    case ABT_POOL_ACCESS_SPMC: return "SPMC";
    case ABT_POOL_ACCESS_MPMC: return "MPMC";
    default: return "UNKNOWN";
    }
}
const char *abti_thread_type_name(ABTI_thread_type t)
{
    switch (t) {
    case ABTI_THREAD_PRIMARY: return "PRIMARY";
    case ABTI_THREAD_NAMED: return "NAMED";
    case ABTI_THREAD_DETACHED: return "DETACHED";
    default: return "UNKNOWN";
    }
}

/* Diagnostics must never crash: a NULL stream falls back to stdout. */
inline FILE *abti_stream(FILE *fp)
{
    return fp ? fp : stdout;
}

void abti_print_pool_line(FILE *fp, const char *prefix, ABTI_pool *p)
{
    if (!p) {
        fprintf(fp, "%spool: (null)\n", prefix);
        return;
    }
    size_t size = p->size();
    long blocked = p->num_blocked.load(std::memory_order_relaxed);
    fprintf(fp,
            "%spool: handle=%p id=%llu kind=%s access=%s automatic=%d "
            "user_defined=%d size=%zu blocked=%ld total=%zu num_scheds=%d\n",
            prefix, (void *)p, (unsigned long long)p->id,
            abti_pool_kind_name(p->kind), abti_pool_access_name(p->access),
            (int)p->automatic, (int)p->user, size, blocked,
            size + (size_t)(blocked > 0 ? blocked : 0),
            p->num_scheds.load(std::memory_order_relaxed));
}

void abti_print_sched_line(FILE *fp, const char *prefix, ABTI_sched *s)
{
    if (!s) {
        fprintf(fp, "%ssched: (null)\n", prefix);
        return;
    }
    fprintf(fp,
            "%ssched: handle=%p predef=%s user_def=%d automatic=%d "
            "event_freq=%d num_pools=%zu used_by_rank=%d\n",
            prefix, (void *)s, abti_sched_predef_name(s->predef),
            (int)s->user_def, (int)s->automatic, s->event_freq,
            s->pools.size(), s->used_by ? s->used_by->rank : -1);
}

void abti_print_xstream_line(FILE *fp, const char *prefix, ABTI_xstream *x)
{
    if (!x) {
        fprintf(fp, "%sxstream: (null)\n", prefix);
        return;
    }
    fprintf(fp,
            "%sxstream: handle=%p rank=%d state=%s primary=%d cpubind=%d "
            "finishing=%d finished=%d\n",
            prefix, (void *)x, x->rank,
            abti_xstream_state_name(x->state.load(std::memory_order_relaxed)),
            (int)x->primary, x->cpubind,
            x->finishing.load(std::memory_order_relaxed),
            x->finished.load(std::memory_order_relaxed));
}

void abti_print_thread_line(FILE *fp, const char *prefix, ABTI_thread *t)
{
    if (!t) {
        fprintf(fp, "%sthread: (null)\n", prefix);
        return;
    }
    ABT_thread_state st = (ABT_thread_state)-1;
    ABT_thread_get_state(ABTI_thread_handle(t), &st);
    fprintf(fp,
            "%sthread: handle=%p id=%llu type=%s state=%s pool=%p "
            "last_xstream_rank=%d stacksize=%zu migratable=%d\n",
            prefix, (void *)t, (unsigned long long)t->id,
            abti_thread_type_name(t->type), abti_thread_state_name(st),
            (void *)t->pool, t->last_xstream ? t->last_xstream->rank : -1,
            t->attr.stacksize ? t->attr.stacksize : abti_default_stacksize(),
            (int)t->attr.migratable);
}

const char *ABTI_NO_UNWIND_MSG =
    "abt-reconverse: stack unwinding is not available in this build "
    "(ABT_INFO_QUERY_KIND_ENABLED_STACK_UNWIND == ABT_FALSE)\n";
} /* namespace */

extern "C" {

double ABT_get_wtime(void) { return CmiWallTimer(); }

int ABT_timer_create(ABT_timer *newtimer)
{
    if (!newtimer)
        return ABT_ERR_TIMER;
    *newtimer = ABT_TIMER_NULL; /* the 1.x API clears the handle first */
    ABTI_timer *t = new (std::nothrow) ABTI_timer();
    if (!t)
        return ABT_ERR_MEM;
    t->start = 0.0;
    t->end = 0.0;
    *newtimer = abti_timer_handle(t);
    return ABT_SUCCESS;
}

int ABT_timer_dup(ABT_timer timer, ABT_timer *newtimer)
{
    if (!newtimer)
        return ABT_ERR_TIMER;
    *newtimer = ABT_TIMER_NULL;
    ABTI_timer *src = abti_timer_get(timer);
    if (!src)
        return ABT_ERR_INV_TIMER;
    ABTI_timer *t = new (std::nothrow) ABTI_timer(*src);
    if (!t)
        return ABT_ERR_MEM;
    *newtimer = abti_timer_handle(t);
    return ABT_SUCCESS;
}

int ABT_timer_free(ABT_timer *timer)
{
    if (!timer)
        return ABT_ERR_TIMER;
    ABTI_timer *t = abti_timer_get(*timer);
    if (!t)
        return ABT_ERR_INV_TIMER;
    delete t;
    *timer = ABT_TIMER_NULL;
    return ABT_SUCCESS;
}

int ABT_timer_start(ABT_timer timer)
{
    ABTI_timer *t = abti_timer_get(timer);
    if (!t)
        return ABT_ERR_INV_TIMER;
    t->start = CmiWallTimer();
    return ABT_SUCCESS;
}

int ABT_timer_stop(ABT_timer timer)
{
    ABTI_timer *t = abti_timer_get(timer);
    if (!t)
        return ABT_ERR_INV_TIMER;
    t->end = CmiWallTimer();
    return ABT_SUCCESS;
}

int ABT_timer_read(ABT_timer timer, double *secs)
{
    ABTI_timer *t = abti_timer_get(timer);
    if (!t)
        return ABT_ERR_INV_TIMER;
    if (!secs)
        return ABT_ERR_TIMER;
    *secs = t->end - t->start;
    return ABT_SUCCESS;
}

int ABT_timer_stop_and_read(ABT_timer timer, double *secs)
{
    ABTI_timer *t = abti_timer_get(timer);
    if (!t)
        return ABT_ERR_INV_TIMER;
    if (!secs)
        return ABT_ERR_TIMER;
    t->end = CmiWallTimer();
    *secs = t->end - t->start;
    return ABT_SUCCESS;
}

int ABT_timer_stop_and_add(ABT_timer timer, double *secs)
{
    ABTI_timer *t = abti_timer_get(timer);
    if (!t)
        return ABT_ERR_INV_TIMER;
    if (!secs)
        return ABT_ERR_TIMER;
    t->end = CmiWallTimer();
    *secs += t->end - t->start;
    return ABT_SUCCESS;
}

int ABT_timer_get_overhead(double *overhead)
{
    if (!overhead)
        return ABT_ERR_TIMER;
    ABT_timer h;
    int ret = ABT_timer_create(&h);
    if (ret != ABT_SUCCESS)
        return ret;
    const int iter = 5000;
    double sum = 0.0;
    for (int i = 0; i < iter; i++) {
        double secs = 0.0;
        ABT_timer_start(h);
        ABT_timer_stop(h);
        ABT_timer_read(h, &secs);
        sum += secs;
    }
    ABT_timer_free(&h);
    *overhead = sum / iter;
    return ABT_SUCCESS;
}

/* ---- info queries -------------------------------------------------------
 * Answers describe THIS shim, not native Argobots: the tool interface and
 * stack unwinding are absent (Margo gates its profiler on ENABLED_TOOL),
 * migration and external threads work, stackable schedulers do not.
 * No ABT_init is required: every answer comes from the environment or from
 * ABTI_g when it exists, and both give the same value (info_query.c checks
 * that repeated queries agree). */
int ABT_info_query_config(ABT_info_query_kind query_kind, void *val)
{
    if (!val)
        return ABT_ERR_OTHER;
    switch (query_kind) {
    /* build-time switches: none of these are on */
    case ABT_INFO_QUERY_KIND_ENABLED_DEBUG:
    case ABT_INFO_QUERY_KIND_ENABLED_PRINT_ERRNO:
    case ABT_INFO_QUERY_KIND_ENABLED_LOG:
    case ABT_INFO_QUERY_KIND_ENABLED_VALGRIND:
    case ABT_INFO_QUERY_KIND_ENABLED_CHECK_ERROR:
    case ABT_INFO_QUERY_KIND_ENABLED_CHECK_POOL_PRODUCER:
    case ABT_INFO_QUERY_KIND_ENABLED_CHECK_POOL_CONSUMER:
    case ABT_INFO_QUERY_KIND_ENABLED_PRINT_CONFIG:
        *(ABT_bool *)val = ABT_FALSE;
        break;
    /* fcontext (via Reconverse's CthThread) preserves the FPU state and is
     * what this shim's ULTs are built on; there is no dynamic promotion. */
    case ABT_INFO_QUERY_KIND_ENABLED_PRESERVE_FPU:
    case ABT_INFO_QUERY_KIND_FCONTEXT:
        *(ABT_bool *)val = ABT_TRUE;
        break;
    case ABT_INFO_QUERY_KIND_DYNAMIC_PROMOTION:
        *(ABT_bool *)val = ABT_FALSE;
        break;
    /* cancellation and tasklets are out of scope (PHASE2-PLAN) */
    case ABT_INFO_QUERY_KIND_ENABLED_THREAD_CANCEL:
    case ABT_INFO_QUERY_KIND_ENABLED_TASK_CANCEL:
        *(ABT_bool *)val = ABT_FALSE;
        break;
    case ABT_INFO_QUERY_KIND_ENABLED_MIGRATION:
        *(ABT_bool *)val = ABT_TRUE;
        break;
    case ABT_INFO_QUERY_KIND_ENABLED_STACKABLE_SCHED:
        *(ABT_bool *)val = ABT_FALSE;
        break;
    case ABT_INFO_QUERY_KIND_ENABLED_EXTERNAL_THREAD:
        *(ABT_bool *)val = ABT_TRUE;
        break;
    /* basic_wait sleeps in the PE idle hook; affinity is applied per lease */
    case ABT_INFO_QUERY_KIND_ENABLED_SCHED_SLEEP:
        *(ABT_bool *)val = ABT_TRUE;
        break;
    case ABT_INFO_QUERY_KIND_ENABLED_AFFINITY:
#ifdef __APPLE__
        *(ABT_bool *)val = ABT_FALSE; /* no thread binding on macOS, as native Argobots reports */
#else
        *(ABT_bool *)val =
            (getenv("ABT_SET_AFFINITY") && atoi(getenv("ABT_SET_AFFINITY")) == 0)
                ? ABT_FALSE
                : ABT_TRUE;
#endif
        break;
    case ABT_INFO_QUERY_KIND_MAX_NUM_XSTREAMS:
        *(unsigned int *)val = abti_max_xstreams();
        break;
    case ABT_INFO_QUERY_KIND_DEFAULT_THREAD_STACKSIZE:
        *(size_t *)val = abti_default_stacksize();
        break;
    case ABT_INFO_QUERY_KIND_DEFAULT_SCHED_STACKSIZE:
        *(size_t *)val = 256000;
        break;
    case ABT_INFO_QUERY_KIND_DEFAULT_SCHED_EVENT_FREQ:
        *(uint64_t *)val = 50;
        break;
    case ABT_INFO_QUERY_KIND_DEFAULT_SCHED_SLEEP_NSEC:
        *(uint64_t *)val = 100;
        break;
    case ABT_INFO_QUERY_KIND_ENABLED_TOOL:
        *(ABT_bool *)val = ABT_FALSE;
        break;
    case ABT_INFO_QUERY_KIND_ENABLED_STACK_UNWIND:
        *(ABT_bool *)val = ABT_FALSE;
        break;
    /* int, not ABT_bool: 0 = no stack overflow check */
    case ABT_INFO_QUERY_KIND_ENABLED_STACK_OVERFLOW_CHECK:
        *(int *)val = 0;
        break;
    /* int: 1 = active (spinning) wait policy.  Leased PEs spin in the
     * Converse scheduler; only basic_wait sleeps in the idle hook. */
    case ABT_INFO_QUERY_KIND_WAIT_POLICY:
        *(int *)val = 1;
        break;
    case ABT_INFO_QUERY_KIND_ENABLED_LAZY_STACK_ALLOC:
        *(ABT_bool *)val = ABT_FALSE;
        break;
    default:
        return ABT_ERR_INV_QUERY_KIND;
    }
    return ABT_SUCCESS;
}

int ABT_info_print_config(FILE *fp)
{
    FILE *f = abti_stream(fp);
    fprintf(f, "Argobots-on-Reconverse shim, %u PEs, stacksize %zu\n",
            abti_max_xstreams(), abti_default_stacksize());
    fprintf(f, "  initialized: %s\n", ABTI_initialized() ? "yes" : "no");
    fprintf(f, "  tool interface: disabled, stack unwinding: disabled, "
               "tasklets: unsupported\n");
    fprintf(f, "  migration: enabled, external threads: enabled, "
               "stackable schedulers: unsupported\n");
    if (ABTI_g) {
        unsigned long na = ABTI_unimplemented_count();
        const char *first = ABTI_first_unimplemented();
        fprintf(f, "  unimplemented calls so far: %lu%s%s\n", na,
                first ? ", first: " : "", first ? first : "");
    }
    fflush(f);
    return ABT_SUCCESS;
}

int ABT_info_print_all_xstreams(FILE *fp)
{
    FILE *f = abti_stream(fp);
    if (!ABTI_g) {
        fprintf(f, "abt-reconverse: not initialized, no xstreams\n");
        fflush(f);
        return ABT_SUCCESS;
    }
    std::lock_guard<std::mutex> g(ABTI_g->xm);
    fprintf(f, "# of xstreams: %zu slots\n", ABTI_g->xstreams.size());
    for (size_t i = 0; i < ABTI_g->xstreams.size(); i++) {
        ABTI_xstream *x = ABTI_g->xstreams[i];
        if (!x) {
            fprintf(f, "  [%zu] (free)\n", i);
            continue;
        }
        char prefix[32];
        snprintf(prefix, sizeof prefix, "  [%zu] ", i);
        abti_print_xstream_line(f, prefix, x);
        abti_print_sched_line(f, "        ", x->main_sched);
        if (x->main_sched) {
            for (size_t j = 0; j < x->main_sched->pools.size(); j++)
                abti_print_pool_line(f, "          ", x->main_sched->pools[j]);
        }
    }
    fflush(f);
    return ABT_SUCCESS;
}

int ABT_info_print_xstream(FILE *fp, ABT_xstream xstream)
{
    FILE *f = abti_stream(fp);
    abti_print_xstream_line(f, "", ABTI_xstream_get(xstream));
    fflush(f);
    return ABT_SUCCESS;
}

int ABT_info_print_sched(FILE *fp, ABT_sched sched)
{
    FILE *f = abti_stream(fp);
    abti_print_sched_line(f, "", ABTI_sched_get(sched));
    fflush(f);
    return ABT_SUCCESS;
}

int ABT_info_print_pool(FILE *fp, ABT_pool pool)
{
    FILE *f = abti_stream(fp);
    abti_print_pool_line(f, "", ABTI_pool_get(pool));
    fflush(f);
    return ABT_SUCCESS;
}

int ABT_info_print_thread(FILE *fp, ABT_thread thread)
{
    FILE *f = abti_stream(fp);
    abti_print_thread_line(f, "", ABTI_thread_get(thread));
    fflush(f);
    return ABT_SUCCESS;
}

int ABT_info_print_thread_attr(FILE *fp, ABT_thread_attr attr)
{
    FILE *f = abti_stream(fp);
    ABTI_thread_attr *a = ABTI_attr_get(attr);
    if (!a) {
        fprintf(f, "thread_attr: (null)\n");
    } else {
        fprintf(f,
                "thread_attr: handle=%p stacksize=%zu stackaddr=%p "
                "migratable=%d\n",
                (void *)a, a->stacksize ? a->stacksize : abti_default_stacksize(),
                a->stackaddr, (int)a->migratable);
    }
    fflush(f);
    return ABT_SUCCESS;
}

/* Tasklets are out of scope in this shim, so there is nothing to print. */
int ABT_info_print_task(FILE *fp, ABT_task task)
{
    (void)fp;
    (void)task;
    ABTI_UNIMPLEMENTED("ABT_info_print_task");
}

/*
 * Stack unwinding is unavailable (ENABLED_STACK_UNWIND == ABT_FALSE), but these
 * still return ABT_SUCCESS: Margo calls ABT_info_print_thread_stacks_in_pool()
 * in a diagnostics path and only logs the return code.
 */
int ABT_info_print_thread_stack(FILE *fp, ABT_thread thread)
{
    FILE *f = abti_stream(fp);
    abti_print_thread_line(f, "", ABTI_thread_get(thread));
    fputs(ABTI_NO_UNWIND_MSG, f);
    fflush(f);
    return ABT_SUCCESS;
}

int ABT_info_print_thread_stacks_in_pool(FILE *fp, ABT_pool pool)
{
    FILE *f = abti_stream(fp);
    abti_print_pool_line(f, "", ABTI_pool_get(pool));
    fputs(ABTI_NO_UNWIND_MSG, f);
    fflush(f);
    return ABT_SUCCESS;
}

int ABT_info_trigger_print_all_thread_stacks(FILE *fp, double timeout,
                                             void (*cb_func)(ABT_bool, void *),
                                             void *arg)
{
    (void)timeout;
    FILE *f = abti_stream(fp);
    fputs(ABTI_NO_UNWIND_MSG, f);
    fflush(f);
    /* The documented contract: cb_func runs after printing completes, with
     * ABT_FALSE because no execution stream timed out (nothing was waited on). */
    if (cb_func)
        cb_func(ABT_FALSE, arg);
    return ABT_SUCCESS;
}

/*
 * The tool interface stays unimplemented on purpose: ENABLED_TOOL is ABT_FALSE
 * above, which is the gate Margo (and Argobots' own tests) check first.
 */
int ABT_tool_register_thread_callback(ABT_tool_thread_callback_fn cb_func,
                                      uint64_t event_mask, void *user_arg)
{
    (void)cb_func;
    (void)event_mask;
    (void)user_arg;
    ABTI_UNIMPLEMENTED("ABT_tool_register_thread_callback");
}

int ABT_tool_query_thread(ABT_tool_context context, uint64_t event,
                          ABT_tool_query_kind query_kind, void *val)
{
    (void)context;
    (void)event;
    (void)query_kind;
    (void)val;
    ABTI_UNIMPLEMENTED("ABT_tool_query_thread");
}

} /* extern "C" */
