# Argobots `test/basic` against the shim

The upstream Argobots conformance tests, compiled **in place** from
`/Users/kale/software/argobots-succession/mochi/argobots/test` (absolute paths;
nothing is copied into this repo and no test source is ever modified) and
linked against the shim's `abt_static`. `test/util/abttest.c` is built once as
a shared helper library.

## How to run

```sh
cd build && cmake .. && make -j8
ctest -R argobots -j4 --timeout 60              # the whole suite
ctest -R argobots -LE divergence                # only what the shim promises
ctest -R argobots -L sched_def                  # the tests of one divergence
ABT_RECONVERSE_TRACE_NA=1 ctest -R argobots_foo --output-on-failure
```

- `ABT_RECONVERSE_TRACE_NA=1` prints every `ABT_ERR_FEATURE_NA` return to
  stderr with the name of the call — the fastest way to see why a test skips.
- A test that exits **77** is a ctest **skip**: Argobots' `ATS_ERROR()` prints
  `Skipped` and exits 77 when a call returns `ABT_ERR_FEATURE_NA`, which is an
  upstream test saying "this build does not have the feature I exercise". A
  **fail** therefore always means a wrong answer, a hang or a crash.
- Each test runs with `ABT_MAX_NUM_XSTREAMS=6` and a 60 s timeout. Arguments
  are per-test: tests whose `main()` reads positional arguments get numbers
  (`4 8`), tests that use `ATS_get_arg_val()` get getopt flags
  (`-e 4 -u 8 [-t 4] [-i 20]`). The two forms are not interchangeable —
  `atoi("-e")` is 0 xstreams.
- Tests blocked by a documented divergence carry the ctest label `divergence`
  plus a label naming it (`sched_def`, `stackable_sched`, `user_pool`,
  `revive`, `exit_to`). `WILL_FAIL` is deliberately never set, so the ctest
  summary always shows what really happened.
- `-DABT_BUILD_ARGOBOTS_TESTS=OFF` drops the whole subdirectory.
- Debugging aid: the shim's own `ABT_info_print_all_xstreams` can be called on
  a live (or hung) process —
  `lldb -b -p <pid> -o 'expr (int)ABT_info_print_all_xstreams((void*)0)' -o detach` —
  and prints every xstream's rank/state/finishing flags with each pool's
  size/blocked counts. It is what identified the run-1 join deadlock.

## Final result

Shim `ade0c97`, reconverse `09595c6`, Mac (arm64, 8 cores, AppleClang 16).
Five full runs; the last three identical:

| bucket | count |
|---|---|
| pass | **52** |
| skip (documented divergence) | **8** |
| fail | **1** (sched_stack, documented divergence D2) |
| hang | 0 |
| **total** | **61** |

Every one of the 9 non-passing tests is a documented divergence below; nothing
is an unexplained failure. History: run 1 `0ada515` 28 pass / 23 fail / 10
hang → run 2 `6286573` 36 / 23 / 2 → run 3 `664a51d` 43 pass / 15 skip / 3
fail → run 4 `cc16a96` 49 / 8 / 4 → **run 5-6 `ade0c97` 52 / 8 / 1**.

(61 of the 62 tests named in the phase-2 plan exist upstream; `rwlock_test`
does not, so the three real rwlock tests stand in for it.)

## Documented divergences

Each is a deliberate limit of the shim, not a defect to chase. None is used by
Margo, Thallium or Yokan, which is the bar phase 3 has to clear.

| id | divergence | tests | rationale |
|---|---|---|---|
| **D1** | User-defined schedulers (`ABT_sched_create` with an `ABT_sched_def`) are unsupported | main_sched, unit, sched_user_ws, xstream_set_main_sched | A user scheduler owns the run loop; on Reconverse the loop belongs to `CsdScheduler` and a scheduler is a poll table, not a callable. The predefined schedulers (DEFAULT/BASIC/BASIC_WAIT/PRIO/RANDWS) are all implemented, which is what the Mochi stack uses. |
| **D2** | Stackable schedulers: `ABT_pool_add_sched`, `ABT_self_schedule`, `ABT_xstream_run_unit` | sched_stack (**fail**), sched_on_thread (skip) | Running a scheduler as a work unit means resuming a ULT from inside another ULT's context, which Reconverse's scheduler does not support without a nested run loop. `ABT_INFO_QUERY_KIND_ENABLED_STACKABLE_SCHED` already answers `ABT_FALSE`. sched_stack fails rather than skips because `ABT_pool_add_sched` returns `ABT_SUCCESS` after doing nothing: its sub-pool tasks never run, so `task_func` sees `v != value + 1` at `sched_stack.c:24`. |
| **D3** | The 2.0-style user pool interface (`ABT_pool_user_def_create` and friends) | pool_custom | The 1.x `ABT_pool_def` path that Margo's custom pools use *is* implemented (`ABT_pool_create`); only the newer definition object is missing. pool_custom also needs D1 and D4. |
| **D4** | `ABT_thread_revive` / `ABT_thread_exit_to` / `ABT_self_exit` / cancellation | thread_create4, ext_thread2 | Reviving a finished ULT means re-entering a stack Reconverse has already retired, and `*_exit_to` additionally has to schedule a named successor. Plain `ABT_thread_exit` *is* implemented; cancellation is reported as unavailable through the info queries. |
| **D5** | A user-provided stack address is accepted but not used | thread_attr2 (passes) | `ABT_thread_attr_set_stack(attr, addr, size)` honors the *size* and reports the address back, but Reconverse allocates the stack. A caller who hands over specific memory (registered/pinned buffers, guard pages, a pool of reused stacks) silently does not get it, and the memory is allocated twice. Margo and Thallium only ever set a stack size. |
| **D6** | eventual_timedwait's counter race | eventual_timedwait (passes) | `g_success_counter++` is a plain `volatile int` incremented by every waiter the single `ABT_eventual_set` wakes, so with several ESs the updates are lost and the test reports `success_counter = 3 (expected: 4)` — measured 3 failures in 6 runs at `-e 4`, 0 in 8 at `-e 1`. The race is the test's own and exists upstream; the harness therefore runs it at the upstream default of **one** execution stream, where what is actually under test (the timed wait) still runs. |

**Known intermittent (not a divergence, not reproduced):** `self_rank_id`
aborted once in five full `-j4` runs and never in 44 targeted runs (20 solo,
24 four-way concurrent); `xstream_rank` did the same once in run 2 with
`std::system_error: mutex lock failed: Invalid argument`. Both tests free an
execution stream and then finalize, so the suspected window is the old core
item C8 — `ABT_xstream_free` destroying a scheduler's pools while the released
PE is still in the old poll table. If either goes red again, capture it with
`ctest --output-on-failure` and `lldb` before assuming a regression elsewhere.

Tasklets are **not** a divergence any more: they are emulated as small ULTs
that cannot yield, and all the tasklet tests in this subset pass
(thread_yield2, sched_prio, eventual_test, ext_thread, thread_get_last_xstream,
self_type).

## Per-test results

| test | result | note |
|---|---|---|
| init_finalize | pass |  |
| xstream_create | pass |  |
| xstream_rank | pass |  |
| xstream_set_main_sched | **skip** | user `ABT_sched_def` (D1) |
| main_sched | **skip** | user `ABT_sched_def` (D1) |
| sched_set_main | pass |  |
| sched_user_ws | **skip** | user `ABT_sched_def` (D1) |
| sched_basic | pass |  |
| sched_basic_wait | pass |  |
| sched_randws | pass |  |
| sched_config | pass |  |
| sched_on_thread | **skip** | `ABT_self_schedule` — stackable schedulers (D2) |
| sched_prio | pass |  |
| sched_stack | **fail** | stackable schedulers (D2): `ABT_pool_add_sched` is a no-op, so the sub-pool tasks never run and `task_func` sees the wrong order at `sched_stack.c:24` |
| pool_config | pass |  |
| pool_custom | **skip** | `ABT_pool_user_def_create` (D3) |
| unit | **skip** | user `ABT_sched_def` (D1) |
| thread_create | pass |  |
| thread_create2 | pass |  |
| thread_create3 | pass |  |
| thread_create4 | **skip** | `ABT_thread_revive` (D4) |
| thread_create_on_xstream | pass |  |
| thread_yield | pass |  |
| thread_yield2 | pass |  |
| thread_yield_to | pass |  |
| thread_exit | pass |  |
| thread_self_suspend_resume | pass |  |
| thread_get_last_xstream | pass |  |
| thread_migrate | pass |  |
| thread_data | pass |  |
| thread_data2 | pass |  |
| thread_id | pass |  |
| thread_attr | pass |  |
| thread_attr2 | pass | passes: stack *size* honored, user address ignored (D5) |
| mutex | pass |  |
| mutex_spinlock | pass |  |
| mutex_static | pass |  |
| mutex_recursive | pass |  |
| cond_test | pass |  |
| cond_join | pass |  |
| cond_static | pass |  |
| cond_timedwait | pass |  |
| eventual_create | pass |  |
| eventual_test | pass |  |
| eventual_static | pass |  |
| eventual_timedwait | pass | run at one ES, the upstream default: the test's counter race (D6) |
| sync_no_contention | pass |  |
| rwlock_reader_incl | pass |  |
| rwlock_reader_writer_excl | pass |  |
| rwlock_writer_excl | pass |  |
| ext_thread | pass |  |
| ext_thread2 | **skip** | `ABT_self_exit` / `ABT_thread_exit_to` (D4) |
| ext_thread_mutex | pass |  |
| ext_thread_cond | pass |  |
| ext_thread_eventual | pass |  |
| ext_thread_join | pass |  |
| info_query | pass |  |
| error | pass |  |
| timer | pass |  |
| self_rank_id | pass |  |
| self_type | pass |  |

## How each failure was diagnosed

For the record, the method that produced the fixes in runs 2-5 — worth reusing
when a red test appears:

1. Re-run the one test with `ABT_RECONVERSE_TRACE_NA=1` and
   `--output-on-failure`: an exit 77 names the missing call directly.
2. For an abort, re-run under `lldb -b -o run -o "bt all"` and read the
   assertion's file:line in the *upstream* test, then read that test's source
   to learn which Argobots property it relies on.
3. For a hang, attach with `lldb -p <pid>`, take `thread backtrace all` (which
   separates "external pthread spinning" from "every PE idle" from "blocked
   ULT"), and dump the shim's own view with
   `expr (int)ABT_info_print_all_xstreams((void*)0)`.
4. Confirm the mechanism with a scratch program linked against
   `build/libabt.a` before proposing a patch — twice during this work the
   reading of the code and the measured behavior disagreed (the stale
   `libabt.dylib` in one case, the `is_task` flag set only for named tasklets
   in the other).

Earlier runs' full diagnoses (the join-drain deadlock, the re-init deadlock,
the unsynchronized key vector, the external-thread recursive mutex, the
tasklet self queries, the finalize drain) are in this file's git history.

## Update 2026-09-14 (shim after 784d777): stackable schedulers implemented

`ABT_pool_add_sched` and `ABT_self_schedule` are implemented (divergence D2
closed): a scheduler added to a pool becomes a ULT unit of that pool whose
body runs the scheduler's policy over the scheduler's own pools and returns
when they hold no unit (blocked ULTs counted for pools it consumes alone, as
Argobots' `ABTI_sched_has_unit`); while only blocked ULTs remain it holds the
PE, parked in `CsdIdleWait` until a push. A resumed ULT returns to the ULT
that resumed it: `ABTI_pool_run_thread` records the caller as the ULT's
`parent` at every resume and `ABTI_choose_fn` returns it (the PE's
scheduling thread when there is none). `ENABLED_STACKABLE_SCHED` answers
`ABT_TRUE`.

Result on the Mac (16 PEs): **57 pass, 4 skip, 0 fail of 61**. Newly passing:
`sched_stack`, `sched_on_thread`, `xstream_set_main_sched`, `thread_create3`.
Remaining skips: `pool_custom` (D3), `unit` (D1: still needs the 2.0 user
pool interface), `thread_create4` and `ext_thread2` (D4: revive / exit_to).
