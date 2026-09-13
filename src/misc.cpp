/*
 * abt-reconverse: an Argobots (ABT) C ABI implemented on top of Reconverse.
 *
 * GENERATED SKELETON (phase 2, step 1).  Every public Argobots entry point is
 * present as a C symbol; the bodies are stubs that record the call and return
 * ABT_ERR_FEATURE_NA.  Real semantics land in later steps; replace stubs in
 * place, keeping the extern "C" block.
 */

#include "abti.h"

/* Error strings, timers, info queries, and the tool interface. */

/* Stub bodies deliberately ignore their arguments. */
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif


#include <cstring>

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

extern "C" {

double ABT_get_wtime(void) { return CmiWallTimer(); }

int ABT_timer_create(ABT_timer *newtimer)
{
    ABTI_UNIMPLEMENTED("ABT_timer_create");
}

int ABT_timer_dup(ABT_timer timer, ABT_timer *newtimer)
{
    ABTI_UNIMPLEMENTED("ABT_timer_dup");
}

int ABT_timer_free(ABT_timer *timer)
{
    ABTI_UNIMPLEMENTED("ABT_timer_free");
}

int ABT_timer_start(ABT_timer timer)
{
    ABTI_UNIMPLEMENTED("ABT_timer_start");
}

int ABT_timer_stop(ABT_timer timer)
{
    ABTI_UNIMPLEMENTED("ABT_timer_stop");
}

int ABT_timer_read(ABT_timer timer, double *secs)
{
    ABTI_UNIMPLEMENTED("ABT_timer_read");
}

int ABT_timer_stop_and_read(ABT_timer timer, double *secs)
{
    ABTI_UNIMPLEMENTED("ABT_timer_stop_and_read");
}

int ABT_timer_stop_and_add(ABT_timer timer, double *secs)
{
    ABTI_UNIMPLEMENTED("ABT_timer_stop_and_add");
}

int ABT_timer_get_overhead(double *overhead)
{
    ABTI_UNIMPLEMENTED("ABT_timer_get_overhead");
}

int ABT_info_query_config(ABT_info_query_kind query_kind, void *val)
{
    ABTI_UNIMPLEMENTED("ABT_info_query_config");
}

int ABT_info_print_config(FILE *fp)
{
    ABTI_UNIMPLEMENTED("ABT_info_print_config");
}

int ABT_info_print_all_xstreams(FILE *fp)
{
    ABTI_UNIMPLEMENTED("ABT_info_print_all_xstreams");
}

int ABT_info_print_xstream(FILE *fp, ABT_xstream xstream)
{
    ABTI_UNIMPLEMENTED("ABT_info_print_xstream");
}

int ABT_info_print_sched(FILE *fp, ABT_sched sched)
{
    ABTI_UNIMPLEMENTED("ABT_info_print_sched");
}

int ABT_info_print_pool(FILE* fp, ABT_pool pool)
{
    ABTI_UNIMPLEMENTED("ABT_info_print_pool");
}

int ABT_info_print_thread(FILE* fp, ABT_thread thread)
{
    ABTI_UNIMPLEMENTED("ABT_info_print_thread");
}

int ABT_info_print_thread_attr(FILE* fp, ABT_thread_attr attr)
{
    ABTI_UNIMPLEMENTED("ABT_info_print_thread_attr");
}

int ABT_info_print_task(FILE* fp, ABT_task task)
{
    ABTI_UNIMPLEMENTED("ABT_info_print_task");
}

int ABT_info_print_thread_stack(FILE *fp, ABT_thread thread)
{
    ABTI_UNIMPLEMENTED("ABT_info_print_thread_stack");
}

int ABT_info_print_thread_stacks_in_pool(FILE *fp, ABT_pool pool)
{
    ABTI_UNIMPLEMENTED("ABT_info_print_thread_stacks_in_pool");
}

int ABT_info_trigger_print_all_thread_stacks(FILE *fp, double timeout, void (*cb_func)(ABT_bool, void *), void *arg)
{
    ABTI_UNIMPLEMENTED("ABT_info_trigger_print_all_thread_stacks");
}

int ABT_tool_register_thread_callback(ABT_tool_thread_callback_fn cb_func, uint64_t event_mask, void *user_arg)
{
    ABTI_UNIMPLEMENTED("ABT_tool_register_thread_callback");
}

int ABT_tool_query_thread(ABT_tool_context context, uint64_t event, ABT_tool_query_kind query_kind, void *val)
{
    ABTI_UNIMPLEMENTED("ABT_tool_query_thread");
}


} /* extern "C" */
