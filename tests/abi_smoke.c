/*
 * abi_smoke.c - abt-reconverse ABI smoke test.
 *
 * Compiled with a C compiler (not C++) against the INSTALLED prefix via
 * pkg-config.  That combination is the point of the test: it proves abt.h is
 * C-clean, that the installed argobots.pc names the right library, and that
 * the ABT_* entry points really carry C linkage.
 *
 * GENERATED as part of the phase-2 skeleton; the address table samples every
 * 10th function of abt.h's 261 declarations so the linker has to resolve a
 * symbol from every module.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <abt.h>

/* Generic function-pointer type; only the addresses matter here. */
typedef void (*abt_any_fn)(void);

static const struct {
    const char *name;
    abt_any_fn fn;
} abt_sample[] = {
    { "ABT_init", (abt_any_fn)ABT_init },
    { "ABT_xstream_cancel", (abt_any_fn)ABT_xstream_cancel },
    { "ABT_xstream_equal", (abt_any_fn)ABT_xstream_equal },
    { "ABT_xstream_barrier_free", (abt_any_fn)ABT_xstream_barrier_free },
    { "ABT_sched_get_total_size", (abt_any_fn)ABT_sched_get_total_size },
    { "ABT_pool_create_basic", (abt_any_fn)ABT_pool_create_basic },
    { "ABT_pool_push_thread", (abt_any_fn)ABT_pool_push_thread },
    { "ABT_pool_get_id", (abt_any_fn)ABT_pool_get_id },
    { "ABT_pool_config_get", (abt_any_fn)ABT_pool_config_get },
    { "ABT_unit_get_thread", (abt_any_fn)ABT_unit_get_thread },
    { "ABT_thread_join", (abt_any_fn)ABT_thread_join },
    { "ABT_thread_get_unit", (abt_any_fn)ABT_thread_get_unit },
    { "ABT_thread_set_migratable", (abt_any_fn)ABT_thread_set_migratable },
    { "ABT_thread_get_thread_func", (abt_any_fn)ABT_thread_get_thread_func },
    { "ABT_thread_attr_set_callback", (abt_any_fn)ABT_thread_attr_set_callback },
    { "ABT_task_get_xstream", (abt_any_fn)ABT_task_get_xstream },
    { "ABT_self_get_xstream_rank", (abt_any_fn)ABT_self_get_xstream_rank },
    { "ABT_self_get_last_pool_id", (abt_any_fn)ABT_self_get_last_pool_id },
    { "ABT_self_exit_to", (abt_any_fn)ABT_self_exit_to },
    { "ABT_mutex_create", (abt_any_fn)ABT_mutex_create },
    { "ABT_mutex_unlock_de", (abt_any_fn)ABT_mutex_unlock_de },
    { "ABT_cond_timedwait", (abt_any_fn)ABT_cond_timedwait },
    { "ABT_eventual_wait", (abt_any_fn)ABT_eventual_wait },
    { "ABT_future_reset", (abt_any_fn)ABT_future_reset },
    { "ABT_timer_free", (abt_any_fn)ABT_timer_free },
    { "ABT_info_print_xstream", (abt_any_fn)ABT_info_print_xstream },
    { "ABT_tool_query_thread", (abt_any_fn)ABT_tool_query_thread },
};

#define NSAMPLE ((int)(sizeof(abt_sample) / sizeof(abt_sample[0])))

static int failures;

static void check(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond)
        failures++;
}

int main(void)
{
    char buf[256];
    size_t len = 0;
    int ret, i;
    ABT_xstream xstream;

    printf("abt-reconverse ABI smoke test\n");
    printf("ABT_VERSION      = %s\n", ABT_VERSION);
    printf("ABT_NUMVERSION   = %d\n", ABT_NUMVERSION);

    /* 1. ABT_initialized is real already: nothing has been initialized. */
    ret = ABT_initialized();
    printf("ABT_initialized() = %d\n", ret);
    check(ret == ABT_ERR_UNINITIALIZED,
          "ABT_initialized() == ABT_ERR_UNINITIALIZED");

    /* 2. ABT_error_get_str is real already: Margo prints these names. */
    memset(buf, 0, sizeof(buf));
    ret = ABT_error_get_str(ABT_ERR_FEATURE_NA, buf, &len);
    printf("ABT_error_get_str(ABT_ERR_FEATURE_NA) -> \"%s\" (len %d, ret %d)\n",
           buf, (int)len, ret);
    check(ret == ABT_SUCCESS, "ABT_error_get_str returns ABT_SUCCESS");
    check(strcmp(buf, "ABT_ERR_FEATURE_NA") == 0,
          "ABT_error_get_str yields the macro name");
    check(len == strlen("ABT_ERR_FEATURE_NA"),
          "ABT_error_get_str reports the right length");

    /* An out-of-range code must not walk off the table. */
    ret = ABT_error_get_str(9999, buf, &len);
    check(ret == ABT_ERR_OTHER, "ABT_error_get_str rejects an unknown code");

    /* 3. Every other entry point is still a stub. */
    ret = ABT_xstream_self(&xstream);
    check(ret == ABT_ERR_FEATURE_NA, "ABT_xstream_self() == ABT_ERR_FEATURE_NA");
    ret = ABT_init(0, NULL);
    check(ret == ABT_ERR_FEATURE_NA, "ABT_init() == ABT_ERR_FEATURE_NA");

    /* 4. Force the linker to resolve a sample across the whole surface. */
    for (i = 0; i < NSAMPLE; i++) {
        if (abt_sample[i].fn == NULL) {
            printf("FAIL null address for %s\n", abt_sample[i].name);
            failures++;
        }
    }
    printf("ok   %d sampled ABT_* addresses resolved (every 10th of 261)\n",
           NSAMPLE);

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
