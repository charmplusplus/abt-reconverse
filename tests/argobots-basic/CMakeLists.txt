# Conformance harness: the Argobots reference tests (upstream test/basic)
# compiled IN PLACE against this shim -- no sources are copied into this repo,
# every path below is absolute into the Argobots checkout.  The point is to
# measure the shim against the tests it must eventually pass, so the test
# sources are never modified; failures are recorded in RESULTS.md instead.
#
# Argument conventions differ per test and were read out of each main():
#   - "positional": the test does atoi(argv[1]), atoi(argv[2]), ...
#   - "getopt":     the test calls ATS_get_arg_val(), so ATS_read_args parses
#                   -e <xstreams> -u <ULTs> -t <tasklets> -i <iterations>.
# Passing the wrong form is not harmless: a getopt-style "-e" fed to a
# positional test becomes atoi("-e") == 0 xstreams.
#
# ABT_MAX_NUM_XSTREAMS=6 is set for every test (xstreams used + 1).  Most tests
# then override it: ATS_init() putenv()s the number of xstreams it was given.

set(ABT_UPSTREAM_TEST_DIR
    "/Users/kale/software/argobots-succession/mochi/argobots/test"
    CACHE PATH "Argobots source checkout's test/ directory")

foreach(_needed util/abttest.c util/abttest.h basic/init_finalize.c)
  if(NOT EXISTS "${ABT_UPSTREAM_TEST_DIR}/${_needed}")
    message(FATAL_ERROR
            "ABT_UPSTREAM_TEST_DIR=${ABT_UPSTREAM_TEST_DIR} has no ${_needed}")
  endif()
endforeach()

# The helper library, built once and shared by every test program.
add_library(abttest STATIC "${ABT_UPSTREAM_TEST_DIR}/util/abttest.c")
target_include_directories(abttest PUBLIC
                           "${ABT_UPSTREAM_TEST_DIR}/util"
                           "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(abttest PUBLIC abt_static)
set_target_properties(abttest PROPERTIES C_STANDARD 99)
# Upstream test code is not warning-clean under this compiler and is not ours
# to fix; keep the noise out of the shim's own build log.
target_compile_options(abttest PRIVATE -w)

# abt_basic_test(<name> [ARGS ...] [EXPECTED_FAIL])
#   Builds <name>.c from test/basic and registers ctest "argobots_<name>".
#   EXPECTED_FAIL only adds the ctest label "expected_fail" -- WILL_FAIL is
#   deliberately NOT set, so the real pass/fail shows up in the ctest summary.
function(abt_basic_test name)
  cmake_parse_arguments(T "EXPECTED_FAIL" "" "ARGS" ${ARGN})
  set(_src "${ABT_UPSTREAM_TEST_DIR}/basic/${name}.c")
  if(NOT EXISTS "${_src}")
    message(WARNING "skipping ${name}: ${_src} does not exist")
    return()
  endif()
  add_executable(abt_basic_${name} "${_src}")
  target_link_libraries(abt_basic_${name} PRIVATE abttest)
  set_target_properties(abt_basic_${name} PROPERTIES
                        C_STANDARD 99
                        OUTPUT_NAME ${name})
  target_compile_options(abt_basic_${name} PRIVATE -w)
  add_test(NAME argobots_${name}
           COMMAND abt_basic_${name} ${T_ARGS})
  set_tests_properties(argobots_${name} PROPERTIES
                       TIMEOUT 60
                       ENVIRONMENT "ABT_MAX_NUM_XSTREAMS=6")
  if(T_EXPECTED_FAIL)
    set_tests_properties(argobots_${name} PROPERTIES LABELS "expected_fail")
  endif()
endfunction()

# ---------------------------------------------------------------- init/ES --
abt_basic_test(init_finalize          ARGS -e 4 -i 4)          # getopt: -e is # of pthreads
abt_basic_test(xstream_create         ARGS -e 4)               # getopt
abt_basic_test(xstream_rank           ARGS -e 4)               # getopt
abt_basic_test(sched_set_main         ARGS -e 4 -u 8)          # getopt

# ----------------------------------------------------------- schedulers ----
# user-defined ABT_sched_def schedulers are out of scope (PHASE2-PLAN)
abt_basic_test(xstream_set_main_sched ARGS -e 4 -u 8 EXPECTED_FAIL)
abt_basic_test(main_sched             ARGS -e 4     EXPECTED_FAIL)
abt_basic_test(sched_user_ws          ARGS -e 4 -u 8 EXPECTED_FAIL)
abt_basic_test(sched_basic            ARGS 4 8)                # positional
abt_basic_test(sched_basic_wait       ARGS 4 8)                # positional
abt_basic_test(sched_randws           ARGS -e 4 -u 8)          # getopt
abt_basic_test(sched_config)                                   # no args
abt_basic_test(sched_on_thread        ARGS 4 8)                # positional
abt_basic_test(sched_prio             ARGS 6 EXPECTED_FAIL)    # positional; tasklets
abt_basic_test(sched_stack            ARGS 8 EXPECTED_FAIL)    # positional; tasklets

# ---------------------------------------------------------------- pools ----
abt_basic_test(pool_config)                                    # no args
# ABT_pool_def pools are in scope, but this test also builds an ABT_sched_def
# scheduler and calls ABT_thread_revive
abt_basic_test(pool_custom            ARGS -e 4 -u 16 EXPECTED_FAIL)
abt_basic_test(unit                   ARGS -e 4 -u 16 EXPECTED_FAIL)

# -------------------------------------------------------------- threads ----
abt_basic_test(thread_create             ARGS 4 8)             # positional
abt_basic_test(thread_create2            ARGS 4 8)             # positional
abt_basic_test(thread_create3            ARGS 4)               # positional
abt_basic_test(thread_create4            EXPECTED_FAIL)        # ABT_thread_revive
abt_basic_test(thread_create_on_xstream  ARGS 4 8)             # positional
abt_basic_test(thread_yield              ARGS 4 8)             # positional
abt_basic_test(thread_yield2             EXPECTED_FAIL)        # tasklets
abt_basic_test(thread_yield_to           ARGS 2 4)             # positional
abt_basic_test(thread_exit               EXPECTED_FAIL)        # ABT_thread_exit
abt_basic_test(thread_self_suspend_resume ARGS -e 4 -u 8 -i 10)
abt_basic_test(thread_get_last_xstream   ARGS 4 8 EXPECTED_FAIL) # tasklets
abt_basic_test(thread_migrate            ARGS 4 8)             # positional
abt_basic_test(thread_data               ARGS -e 4 -u 8)       # getopt
abt_basic_test(thread_data2              ARGS -e 4 -u 8)       # getopt
abt_basic_test(thread_id                 ARGS 4 8)             # positional
abt_basic_test(thread_attr               ARGS 4 8)             # positional
abt_basic_test(thread_attr2)                                   # no args

# ----------------------------------------------------------------- sync ----
abt_basic_test(mutex              ARGS -e 4 -u 8 -i 20)
abt_basic_test(mutex_spinlock     ARGS -e 4 -u 8 -i 20)
abt_basic_test(mutex_static       ARGS -e 4 -u 8 -i 20)
abt_basic_test(mutex_recursive    ARGS -e 4 -u 8)
abt_basic_test(cond_test          ARGS 4 8)                    # positional
abt_basic_test(cond_join          ARGS 4 EXPECTED_FAIL)        # ABT_thread_exit
abt_basic_test(cond_static        ARGS -i 20)                  # getopt
abt_basic_test(cond_timedwait     ARGS -e 4 -u 8 EXPECTED_FAIL) # ABT_thread_exit
abt_basic_test(eventual_create)                                # no args
abt_basic_test(eventual_test      ARGS -e 4 -u 8 -t 4 -i 20 EXPECTED_FAIL) # tasklets
abt_basic_test(eventual_static    ARGS -e 4 -u 8 -i 20)
abt_basic_test(eventual_timedwait ARGS -e 4 -u 8)
abt_basic_test(sync_no_contention)                             # no args
# "rwlock_test" does not exist upstream; the rwlock tests are these three
abt_basic_test(rwlock_reader_incl        ARGS 4 4 4 20)        # positional
abt_basic_test(rwlock_reader_writer_excl ARGS 4 4 4 20)        # positional
abt_basic_test(rwlock_writer_excl        ARGS 4 4 4 20)        # positional

# ------------------------------------------------------- external threads --
abt_basic_test(ext_thread          EXPECTED_FAIL)              # tasklets
abt_basic_test(ext_thread2         EXPECTED_FAIL)              # ABT_thread_exit
abt_basic_test(ext_thread_mutex    ARGS -e 4 -u 8 -i 200)
abt_basic_test(ext_thread_cond     ARGS -i 50)
abt_basic_test(ext_thread_eventual ARGS -e 4 -u 8 -i 50)
abt_basic_test(ext_thread_join     ARGS -e 4 -u 8 -i 20)

# ------------------------------------------------------------ misc/self ----
abt_basic_test(info_query)                                     # no args
abt_basic_test(error)                                          # no args
abt_basic_test(timer         ARGS 4 8)                         # positional
abt_basic_test(self_rank_id)                                   # no args
abt_basic_test(self_type     EXPECTED_FAIL)                    # tasklets
