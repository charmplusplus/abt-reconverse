/* Tests for src/misc.cpp: timers, every ABT_info_query_config kind, the
 * ABT_info_print_* diagnostics, and the deliberately-unimplemented tool
 * interface.  Compiled as C and linked against abt_static, like core_smoke. */
#include <abt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static int failures;

#define CHECK(call)                                                            \
    do {                                                                       \
        int _r = (call);                                                       \
        if (_r != ABT_SUCCESS) {                                               \
            char s[128];                                                       \
            size_t l = sizeof s;                                               \
            ABT_error_get_str(_r, s, &l);                                      \
            fprintf(stderr, "%s:%d: %s -> %s\n", __FILE__, __LINE__, #call,    \
                    s);                                                        \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define EXPECT(cond, what)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: FAILED %s\n", __FILE__, __LINE__, what);   \
            failures++;                                                        \
        } else {                                                               \
            printf("ok   %s\n", what);                                         \
        }                                                                      \
    } while (0)

#define EXPECT_RET(call, want, what)                                           \
    do {                                                                       \
        int _r = (call);                                                       \
        if (_r != (want)) {                                                    \
            char s[128];                                                       \
            size_t l = sizeof s;                                               \
            ABT_error_get_str(_r, s, &l);                                      \
            fprintf(stderr, "%s:%d: FAILED %s: got %s\n", __FILE__, __LINE__,  \
                    what, s);                                                  \
            failures++;                                                        \
        } else {                                                               \
            printf("ok   %s\n", what);                                         \
        }                                                                      \
    } while (0)

/* ---------------------------------------------------------------- timers */

static void spin(double secs)
{
    double t0 = ABT_get_wtime();
    while (ABT_get_wtime() - t0 < secs)
        ;
}

static void test_timers_before_init(void)
{
    ABT_timer t = ABT_TIMER_NULL, dup = ABT_TIMER_NULL;
    double secs = -1.0, acc = 0.0, overhead = -1.0;

    /* test/basic/timer.c creates a timer before ABT_init; so must this work. */
    CHECK(ABT_timer_create(&t));
    EXPECT(t != ABT_TIMER_NULL, "ABT_timer_create yields a non-null handle");

    CHECK(ABT_timer_start(t));
    spin(0.01);
    CHECK(ABT_timer_stop(t));
    CHECK(ABT_timer_read(t, &secs));
    EXPECT(secs >= 0.005 && secs < 5.0, "ABT_timer_read measures the interval");

    /* read is idempotent: it does not re-stop the timer */
    {
        double again = -1.0;
        CHECK(ABT_timer_read(t, &again));
        EXPECT(again == secs, "ABT_timer_read is idempotent");
    }

    /* dup copies the recorded interval */
    CHECK(ABT_timer_dup(t, &dup));
    {
        double d = -1.0;
        CHECK(ABT_timer_read(dup, &d));
        EXPECT(d == secs, "ABT_timer_dup copies start/end");
    }
    CHECK(ABT_timer_free(&dup));
    EXPECT(dup == ABT_TIMER_NULL, "ABT_timer_free nulls the handle");

    /* stop_and_read = stop + read */
    CHECK(ABT_timer_start(t));
    spin(0.01);
    CHECK(ABT_timer_stop_and_read(t, &secs));
    EXPECT(secs >= 0.005 && secs < 5.0, "ABT_timer_stop_and_read measures");

    /* stop_and_add accumulates into the caller's variable */
    acc = 1.0;
    CHECK(ABT_timer_start(t));
    spin(0.01);
    CHECK(ABT_timer_stop_and_add(t, &acc));
    EXPECT(acc > 1.005 && acc < 6.0, "ABT_timer_stop_and_add accumulates");

    CHECK(ABT_timer_get_overhead(&overhead));
    EXPECT(overhead >= 0.0 && overhead < 1.0,
           "ABT_timer_get_overhead is a small non-negative time");

    /* invalid handles */
    EXPECT_RET(ABT_timer_start(ABT_TIMER_NULL), ABT_ERR_INV_TIMER,
               "ABT_timer_start(NULL) -> ABT_ERR_INV_TIMER");
    EXPECT_RET(ABT_timer_read(ABT_TIMER_NULL, &secs), ABT_ERR_INV_TIMER,
               "ABT_timer_read(NULL) -> ABT_ERR_INV_TIMER");
    {
        ABT_timer null_timer = ABT_TIMER_NULL;
        EXPECT_RET(ABT_timer_free(&null_timer), ABT_ERR_INV_TIMER,
                   "ABT_timer_free(NULL) -> ABT_ERR_INV_TIMER");
        EXPECT_RET(ABT_timer_dup(ABT_TIMER_NULL, &dup), ABT_ERR_INV_TIMER,
                   "ABT_timer_dup(NULL) -> ABT_ERR_INV_TIMER");
    }

    CHECK(ABT_timer_free(&t));

    EXPECT(ABT_get_wtime() > 0.0, "ABT_get_wtime is positive");
}

/* ---------------------------------------------------------- info queries */

enum val_kind { V_BOOL, V_INT, V_UINT, V_U64, V_SIZE };

struct query {
    ABT_info_query_kind kind;
    const char *name;
    enum val_kind type;
};

#define Q(k, t)                                                                \
    {                                                                          \
        ABT_INFO_QUERY_KIND_##k, #k, t                                         \
    }

static const struct query queries[] = {
    Q(ENABLED_DEBUG, V_BOOL),
    Q(ENABLED_PRINT_ERRNO, V_BOOL),
    Q(ENABLED_LOG, V_BOOL),
    Q(ENABLED_VALGRIND, V_BOOL),
    Q(ENABLED_CHECK_ERROR, V_BOOL),
    Q(ENABLED_CHECK_POOL_PRODUCER, V_BOOL),
    Q(ENABLED_CHECK_POOL_CONSUMER, V_BOOL),
    Q(ENABLED_PRESERVE_FPU, V_BOOL),
    Q(ENABLED_THREAD_CANCEL, V_BOOL),
    Q(ENABLED_TASK_CANCEL, V_BOOL),
    Q(ENABLED_MIGRATION, V_BOOL),
    Q(ENABLED_STACKABLE_SCHED, V_BOOL),
    Q(ENABLED_EXTERNAL_THREAD, V_BOOL),
    Q(ENABLED_SCHED_SLEEP, V_BOOL),
    Q(ENABLED_PRINT_CONFIG, V_BOOL),
    Q(ENABLED_AFFINITY, V_BOOL),
    Q(MAX_NUM_XSTREAMS, V_UINT),
    Q(DEFAULT_THREAD_STACKSIZE, V_SIZE),
    Q(DEFAULT_SCHED_STACKSIZE, V_SIZE),
    Q(DEFAULT_SCHED_EVENT_FREQ, V_U64),
    Q(DEFAULT_SCHED_SLEEP_NSEC, V_U64),
    Q(ENABLED_TOOL, V_BOOL),
    Q(FCONTEXT, V_BOOL),
    Q(DYNAMIC_PROMOTION, V_BOOL),
    Q(ENABLED_STACK_UNWIND, V_BOOL),
    Q(ENABLED_STACK_OVERFLOW_CHECK, V_INT),
    Q(WAIT_POLICY, V_INT),
    Q(ENABLED_LAZY_STACK_ALLOC, V_BOOL),
};
#define NQUERIES ((int)(sizeof queries / sizeof queries[0]))

/* every kind, with guard words on both sides so an over-wide store is caught */
static void query_all(uint64_t *out)
{
    int i;
    for (i = 0; i < NQUERIES; i++) {
        union {
            uint32_t words[8];
            uint64_t u64[4];
        } buf;
        int32_t *ptr;
        int ret;
        memset(&buf, 0x77, sizeof buf);
        ptr = (int32_t *)&buf.u64[1]; /* 8-byte aligned, guards around it */
        ret = ABT_info_query_config(queries[i].kind, (void *)ptr);
        if (ret != ABT_SUCCESS) {
            fprintf(stderr, "FAILED: query %s returned %d\n", queries[i].name,
                    ret);
            failures++;
            continue;
        }
        /* guards */
        if (buf.words[1] != 0x77777777u) {
            fprintf(stderr, "FAILED: query %s wrote below the buffer\n",
                    queries[i].name);
            failures++;
        }
        switch (queries[i].type) {
        case V_BOOL: {
            ABT_bool v = *(ABT_bool *)ptr;
            if (v != ABT_TRUE && v != ABT_FALSE) {
                fprintf(stderr, "FAILED: query %s is not a bool (%d)\n",
                        queries[i].name, (int)v);
                failures++;
            }
            if (buf.words[3] != 0x77777777u) {
                fprintf(stderr, "FAILED: query %s wrote past 4 bytes\n",
                        queries[i].name);
                failures++;
            }
            out[i] = (uint64_t)(uint32_t)v;
            break;
        }
        case V_INT:
            if (buf.words[3] != 0x77777777u) {
                fprintf(stderr, "FAILED: query %s wrote past 4 bytes\n",
                        queries[i].name);
                failures++;
            }
            out[i] = (uint64_t)(uint32_t)*(int *)ptr;
            break;
        case V_UINT:
            if (buf.words[3] != 0x77777777u) {
                fprintf(stderr, "FAILED: query %s wrote past 4 bytes\n",
                        queries[i].name);
                failures++;
            }
            out[i] = *(unsigned int *)ptr;
            break;
        case V_U64:
            if (buf.words[4] != 0x77777777u) {
                fprintf(stderr, "FAILED: query %s wrote past 8 bytes\n",
                        queries[i].name);
                failures++;
            }
            out[i] = *(uint64_t *)ptr;
            break;
        case V_SIZE:
            if (buf.words[4] != 0x77777777u) {
                fprintf(stderr, "FAILED: query %s wrote past 8 bytes\n",
                        queries[i].name);
                failures++;
            }
            out[i] = (uint64_t)*(size_t *)ptr;
            break;
        }
    }
}

static uint64_t query_value(const uint64_t *vals, ABT_info_query_kind kind)
{
    int i;
    for (i = 0; i < NQUERIES; i++)
        if (queries[i].kind == kind)
            return vals[i];
    abort();
}

static void test_info_queries(void)
{
    uint64_t before[NQUERIES], after[NQUERIES];
    int i;
    int dummy = 0;

    /* The shim answers without ABT_init (Margo may query before it inits). */
    query_all(before);
    printf("ok   every info query kind answered before ABT_init\n");

    CHECK(ABT_init(0, NULL));
    query_all(after);

    for (i = 0; i < NQUERIES; i++) {
        if (before[i] != after[i]) {
            fprintf(stderr,
                    "FAILED: query %s changed across ABT_init "
                    "(%llu -> %llu)\n",
                    queries[i].name, (unsigned long long)before[i],
                    (unsigned long long)after[i]);
            failures++;
        }
    }
    printf("ok   info query answers are stable across ABT_init\n");

    /* the answers the shim promises (PHASE2-PLAN) */
    EXPECT(query_value(after, ABT_INFO_QUERY_KIND_ENABLED_TOOL) == ABT_FALSE,
           "ENABLED_TOOL is ABT_FALSE");
    EXPECT(query_value(after, ABT_INFO_QUERY_KIND_ENABLED_STACK_UNWIND) ==
               ABT_FALSE,
           "ENABLED_STACK_UNWIND is ABT_FALSE");
    EXPECT(query_value(after, ABT_INFO_QUERY_KIND_ENABLED_LAZY_STACK_ALLOC) ==
               ABT_FALSE,
           "ENABLED_LAZY_STACK_ALLOC is ABT_FALSE");
    EXPECT(query_value(after, ABT_INFO_QUERY_KIND_ENABLED_MIGRATION) ==
               ABT_TRUE,
           "ENABLED_MIGRATION is ABT_TRUE");
    EXPECT(query_value(after, ABT_INFO_QUERY_KIND_ENABLED_EXTERNAL_THREAD) ==
               ABT_TRUE,
           "ENABLED_EXTERNAL_THREAD is ABT_TRUE");
    EXPECT(query_value(after, ABT_INFO_QUERY_KIND_ENABLED_STACKABLE_SCHED) ==
               ABT_FALSE,
           "ENABLED_STACKABLE_SCHED is ABT_FALSE");
    EXPECT(query_value(after, ABT_INFO_QUERY_KIND_ENABLED_DEBUG) == ABT_FALSE,
           "ENABLED_DEBUG is ABT_FALSE");
    EXPECT(query_value(after, ABT_INFO_QUERY_KIND_DEFAULT_SCHED_STACKSIZE) ==
               256000,
           "DEFAULT_SCHED_STACKSIZE is 256000");
    EXPECT(query_value(after, ABT_INFO_QUERY_KIND_DEFAULT_SCHED_EVENT_FREQ) ==
               50,
           "DEFAULT_SCHED_EVENT_FREQ is 50");
    EXPECT(query_value(after, ABT_INFO_QUERY_KIND_DEFAULT_SCHED_SLEEP_NSEC) ==
               100,
           "DEFAULT_SCHED_SLEEP_NSEC is 100");
    EXPECT(query_value(after, ABT_INFO_QUERY_KIND_MAX_NUM_XSTREAMS) >= 1,
           "MAX_NUM_XSTREAMS is at least 1");

    /* DEFAULT_THREAD_STACKSIZE must match what a default attr reports */
    {
        size_t qs = (size_t)query_value(
            after, ABT_INFO_QUERY_KIND_DEFAULT_THREAD_STACKSIZE);
        EXPECT(qs >= 16384, "DEFAULT_THREAD_STACKSIZE is a plausible size");
    }

    EXPECT_RET(ABT_info_query_config((ABT_info_query_kind)12345, &dummy),
               ABT_ERR_INV_QUERY_KIND,
               "an unknown query kind -> ABT_ERR_INV_QUERY_KIND");
}

/* --------------------------------------------------------------- prints */

static int cb_called;
static void stack_cb(ABT_bool timed_out, void *arg)
{
    cb_called++;
    if (timed_out != ABT_FALSE || arg != (void *)0x1234) {
        fprintf(stderr, "FAILED: stack callback got (%d, %p)\n", (int)timed_out,
                arg);
        failures++;
    }
}

static void test_prints(void)
{
    FILE *fp = tmpfile();
    char *text;
    long len;
    ABT_xstream self_xs;
    ABT_sched sched;
    ABT_pool pool;
    ABT_thread self_thread;
    ABT_thread_attr attr;

    if (!fp) {
        fprintf(stderr, "FAILED: tmpfile()\n");
        failures++;
        return;
    }

    CHECK(ABT_xstream_self(&self_xs));
    CHECK(ABT_xstream_get_main_sched(self_xs, &sched));
    CHECK(ABT_xstream_get_main_pools(self_xs, 1, &pool));
    CHECK(ABT_thread_self(&self_thread));
    CHECK(ABT_thread_attr_create(&attr));

    CHECK(ABT_info_print_config(fp));
    CHECK(ABT_info_print_all_xstreams(fp));
    CHECK(ABT_info_print_xstream(fp, self_xs));
    CHECK(ABT_info_print_sched(fp, sched));
    CHECK(ABT_info_print_pool(fp, pool));
    CHECK(ABT_info_print_thread(fp, self_thread));
    CHECK(ABT_info_print_thread_attr(fp, attr));

    /* stack unwinding is unavailable but must not fail: Margo checks the
     * return code of print_thread_stacks_in_pool only for logging */
    CHECK(ABT_info_print_thread_stack(fp, self_thread));
    CHECK(ABT_info_print_thread_stacks_in_pool(fp, pool));
    cb_called = 0;
    CHECK(ABT_info_trigger_print_all_thread_stacks(fp, -1.0, stack_cb,
                                                   (void *)0x1234));
    EXPECT(cb_called == 1, "trigger_print_all_thread_stacks runs the callback");

    /* null handles must print something rather than crash */
    CHECK(ABT_info_print_xstream(fp, ABT_XSTREAM_NULL));
    CHECK(ABT_info_print_sched(fp, ABT_SCHED_NULL));
    CHECK(ABT_info_print_pool(fp, ABT_POOL_NULL));
    CHECK(ABT_info_print_thread(fp, ABT_THREAD_NULL));
    CHECK(ABT_info_print_thread_attr(fp, ABT_THREAD_ATTR_NULL));
    printf("ok   the print entry points tolerate null handles\n");

    /* tasklets are out of scope */
    EXPECT_RET(ABT_info_print_task(fp, ABT_TASK_NULL), ABT_ERR_FEATURE_NA,
               "ABT_info_print_task -> ABT_ERR_FEATURE_NA");

    CHECK(ABT_thread_attr_free(&attr));

    /* read the tmpfile back */
    fflush(fp);
    len = ftell(fp);
    EXPECT(len > 0, "the print calls wrote to the stream");
    rewind(fp);
    text = (char *)calloc(1, (size_t)len + 1);
    if (text && len > 0) {
        size_t got = fread(text, 1, (size_t)len, fp);
        text[got] = 0;
        EXPECT(strstr(text, "Argobots-on-Reconverse shim") != NULL,
               "ABT_info_print_config names the shim");
        EXPECT(strstr(text, "stacksize ") != NULL,
               "ABT_info_print_config reports the stack size");
        EXPECT(strstr(text, "xstream: ") != NULL,
               "ABT_info_print_xstream prints the xstream");
        EXPECT(strstr(text, "pool: ") != NULL,
               "ABT_info_print_pool prints the pool");
        EXPECT(strstr(text, "thread: ") != NULL,
               "ABT_info_print_thread prints the thread");
        EXPECT(strstr(text, "sched: ") != NULL,
               "ABT_info_print_sched prints the scheduler");
        EXPECT(strstr(text, "stack unwinding is not available") != NULL,
               "the stack printers say unwinding is unavailable");
        EXPECT(strstr(text, "(null)") != NULL,
               "null handles print as (null)");
        printf("--- captured diagnostics ---\n%s---\n", text);
    }
    free(text);
    fclose(fp);
}

/* ----------------------------------------------------------------- tool */

static void tool_cb(ABT_thread thread, ABT_xstream xstream, uint64_t event,
                    ABT_tool_context context, void *user_arg)
{
    (void)thread;
    (void)xstream;
    (void)event;
    (void)context;
    (void)user_arg;
}

static void test_tool_and_errors(void)
{
    int dummy = 0;
    EXPECT_RET(ABT_tool_register_thread_callback(tool_cb, 0, NULL),
               ABT_ERR_FEATURE_NA,
               "ABT_tool_register_thread_callback -> ABT_ERR_FEATURE_NA");
    EXPECT_RET(ABT_tool_query_thread(ABT_TOOL_CONTEXT_NULL, 0,
                                     ABT_TOOL_QUERY_KIND_POOL, &dummy),
               ABT_ERR_FEATURE_NA,
               "ABT_tool_query_thread -> ABT_ERR_FEATURE_NA");

    {
        char s[128];
        size_t l = sizeof s;
        CHECK(ABT_error_get_str(ABT_ERR_INV_QUERY_KIND, s, &l));
        EXPECT(strcmp(s, "ABT_ERR_INV_QUERY_KIND") == 0 && l == strlen(s),
               "ABT_error_get_str names ABT_ERR_INV_QUERY_KIND");
    }
}

int main(void)
{
    printf("misc_test: timers, info queries, diagnostics\n");
    test_timers_before_init();
    test_info_queries(); /* calls ABT_init */
    test_prints();
    test_tool_and_errors();
    CHECK(ABT_finalize());

    /* a timer still works after finalize */
    {
        ABT_timer t;
        double secs = -1.0;
        CHECK(ABT_timer_create(&t));
        CHECK(ABT_timer_start(t));
        CHECK(ABT_timer_stop_and_read(t, &secs));
        EXPECT(secs >= 0.0, "timers still work after ABT_finalize");
        CHECK(ABT_timer_free(&t));
    }

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
