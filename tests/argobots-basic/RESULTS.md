# Argobots `test/basic` against the shim — first conformance run

Run 2026-09-13 on the Mac (arm64, 8 cores, AppleClang 16), one pass:

```
cd build && ABT_RECONVERSE_TRACE_NA=1 ctest -R argobots -j4 --timeout 60
```

61 of the 62 requested tests exist upstream and are built here; `rwlock_test`
does not exist, so the three real rwlock tests
(`rwlock_reader_incl`, `rwlock_reader_writer_excl`, `rwlock_writer_excl`)
stand in for it. Test sources are compiled in place from
`/Users/kale/software/argobots-succession/mochi/argobots/test` and were not
modified.

**State of the tree when this was run:** `src/sync.cpp` was being rewritten
concurrently and was already functional for ULT-side mutex/cond/eventual/
rwlock — the ULT sync tests pass. Only two failures are attributable to sync.

Reading the failures: `ATS_ERROR()` turns `ABT_ERR_FEATURE_NA` into
`printf("Skipped"); exit(77)`, so an exit code of 77 means "the shim returned
ABT_ERR_FEATURE_NA from the call named on that line". `ABT_RECONVERSE_TRACE_NA=1`
prints the name of each such call to stderr, which is how the "FEATURE_NA from"
column was filled in.

## Summary

| bucket | count |
|---|---|
| pass | 28 |
| fail — core (xstream/pool/thread/sched/init) | 14 |
| fail — out of scope per PHASE2-PLAN (tasklets, user `ABT_sched_def`, `ABT_thread_exit`/`revive`) | 9 |
| fail — sync | 0 |
| hang / timeout | 10 (8 core, 2 sync) |
| **total** | **61** |

Hangs by root cause: 6 × re-init deadlock (core, init), 2 × xstream-join drain
deadlock (core, xstream), 2 × external-pthread mutex (sync).

Counting by root cause rather than by failure mode: **22 core, 9 out of scope,
2 sync.**

## Per-test results

| test | result | reason |
|---|---|---|
| init_finalize | pass | |
| xstream_create | pass | |
| xstream_rank | pass | |
| sched_set_main | pass | |
| xstream_set_main_sched | fail (77) | **core**: FEATURE_NA from `ABT_sched_config_create` (`xstream_set_main_sched.c:141`); also needs user `ABT_sched_def` (out of scope) |
| main_sched | fail (77) | out of scope: FEATURE_NA from `ABT_sched_create` with an `ABT_sched_def` (`main_sched.c:119`) |
| sched_user_ws | fail (77) | **core**: FEATURE_NA from `ABT_sched_config_create` (`sched_user_ws.c:152`); also needs user `ABT_sched_def` |
| sched_basic | fail (77) | **core**: FEATURE_NA from `ABT_sched_config_create` (`sched_basic.c:48`, the `ABT_sched_basic_freq` config) |
| sched_basic_wait | pass | |
| sched_randws | **hang** | **core**: xstream-join drain deadlock (diagnosis 1) |
| sched_config | fail (77) | **core**: FEATURE_NA from `ABT_sched_config_create` (`sched_config.c:102`) |
| sched_on_thread | fail (77) | **core**: FEATURE_NA from `ABT_self_schedule` (`sched_on_thread.c:66`) |
| sched_prio | fail (77) | **core**: FEATURE_NA from `ABT_sched_config_create` (`sched_prio.c:122`), reached before the tasklet part |
| sched_stack | fail (77) | out of scope: FEATURE_NA from `ABT_task_create` (`sched_stack.c:78`) |
| pool_config | fail (77) | **core**: FEATURE_NA from `ABT_pool_config_create` (`pool_config.c:70`) |
| pool_custom | fail (77) | **core**: FEATURE_NA from `ABT_pool_user_def_create` (`pool_custom.c:554`); also needs `ABT_sched_def` + `ABT_thread_revive` |
| unit | fail (77) | out of scope: FEATURE_NA from `ABT_sched_create` with an `ABT_sched_def` (`unit.c:118`) |
| thread_create | pass | |
| thread_create2 | pass | |
| thread_create3 | **hang** | **core**: xstream-join drain deadlock (diagnosis 1) |
| thread_create4 | fail (77) | out of scope: FEATURE_NA from `ABT_thread_revive` (`thread_create4.c:53`) |
| thread_create_on_xstream | pass | |
| thread_yield | pass | |
| thread_yield2 | fail (77) | out of scope: FEATURE_NA from `ABT_task_create` (`thread_yield2.c:128`) |
| thread_yield_to | pass | |
| thread_exit | fail (abort) | out of scope: FEATURE_NA from `ABT_thread_exit`, then `assert(0)` at `thread_exit.c:24` |
| thread_self_suspend_resume | pass | |
| thread_get_last_xstream | fail (77) | out of scope: FEATURE_NA from `ABT_task_create_on_xstream` (`thread_get_last_xstream.c:90`) |
| thread_migrate | fail (77) | **core**: FEATURE_NA from `ABT_thread_migrate` (`thread_migrate.c:27`) (diagnosis 3) |
| thread_data | pass | |
| thread_data2 | fail (abort) | **core**: heap corruption + `assert` at `thread_data2.c:44` (diagnosis 4) |
| thread_id | pass | |
| thread_attr | pass | |
| thread_attr2 | fail (77) | **core**: FEATURE_NA from `ABT_thread_attr_set_stack` with a user-provided stack, `thread_attr2.c:167` (diagnosis 5) |
| mutex | pass | |
| mutex_spinlock | pass | |
| mutex_static | **hang** | sync: 4 external pthreads stuck in `mutex_lock()` on an `ABT_MUTEX_INITIALIZER` memory mutex, main in `pthread_join` |
| mutex_recursive | pass | |
| cond_test | pass | |
| cond_join | pass | (labelled expected_fail for `ABT_thread_exit`, but passes) |
| cond_static | **hang** | **core**: re-init deadlock (diagnosis 2) |
| cond_timedwait | fail (1) | out of scope: FEATURE_NA from `ABT_thread_exit`, so the ULTs do not exit early and the test's own check fails: `g_counter = 10 (expected: 8)` |
| eventual_create | pass | |
| eventual_test | fail (77) | out of scope: FEATURE_NA from `ABT_task_create` (`eventual_test.c:163`) |
| eventual_static | **hang** | **core**: re-init deadlock (diagnosis 2) |
| eventual_timedwait | pass | |
| sync_no_contention | pass | |
| rwlock_reader_incl | pass | |
| rwlock_reader_writer_excl | pass | |
| rwlock_writer_excl | pass | |
| ext_thread | fail (1) | **core**: `ABT_ERR_INV_XSTREAM_RANK` from `ABT_xstream_create` at `ext_thread.c:117` (diagnosis 6) |
| ext_thread2 | fail (abort) | **core**: same, `ext_thread2.c:105` (diagnosis 6) |
| ext_thread_mutex | **hang** | sync: external pthreads stuck in `mutex_lock()`, main in `pthread_join` |
| ext_thread_cond | **hang** | **core**: re-init deadlock (diagnosis 2) |
| ext_thread_eventual | **hang** | **core**: re-init deadlock (diagnosis 2) |
| ext_thread_join | **hang** | **core**: re-init deadlock (diagnosis 2) |
| info_query | pass | every query kind, before/after init, consistency |
| error | pass | |
| timer | pass | |
| self_rank_id | **hang** | **core**: re-init deadlock (diagnosis 2) |
| self_type | fail (abort) | **core**: `assert` at `self_type.c:207` (diagnosis 7); also needs tasklets later |

## Core diagnoses

These are for the core engineer (xstream/pool/thread/sched/init); nothing here
is sync or misc. Nothing in `src/` was changed to produce them.

### 1. `ABT_xstream_join` deadlocks when the joining ULT's pool is also in the joined xstream's scheduler — `src/xstream.cpp`

Affects **sched_randws** and **thread_create3** (both hang; both build the
rotated pool sets that most Argobots scheduler tests use).

`join_impl()` sets `finishing` and blocks the joiner with
`ABTI_thread_block()`, which does `self->pool->num_blocked++`
(`src/thread.cpp:63`). The finishing PE's idle hook then calls
`pools_drained(xs->main_sched)`, which is false while **any** pool of that
scheduler has `size() != 0 || num_blocked != 0` (`src/xstream.cpp`,
`pools_drained`). When the joined xstream's scheduler contains the pool the
joiner is blocked in — which is exactly what happens when pools are shared
between schedulers — the joiner's own blocked count keeps the drain condition
false forever: the lease is never released, `finished` is never set, and the
joiner never wakes.

Captured state of a hung `thread_create3 2` (obtained by attaching lldb and
calling the new `ABT_info_print_all_xstreams`):

```
  [0] xstream: rank=0 state=RUNNING primary=1 finishing=0 finished=0
        sched: predef=DEFAULT num_pools=2
          pool: id=3 size=0 blocked=1 total=1 num_scheds=2   <-- the primary ULT,
          pool: id=4 size=0 blocked=0 total=0 num_scheds=2       blocked in xstream_join
  [1] xstream: rank=1 state=RUNNING primary=0 finishing=1 finished=0
        sched: predef=DEFAULT num_pools=2
          pool: id=4 size=0 blocked=0 total=0 num_scheds=2
          pool: id=3 size=0 blocked=1 total=1 num_scheds=2   <-- same pool, so never "drained"
```

All PEs spin in `CsdScheduler` → `ABTI_poll_pool` with empty pools.
`thread_create3 1` (one xstream, no join) passes, `thread_create3 2` and
`thread_create3 4` hang, which isolates it to the join path rather than the
create/yield path. Note `ABT_thread_create_to` (unimplemented) is never reached
in this test — no FEATURE_NA is traced before the hang.

Suggestion: the drain condition should ignore work units that are blocked
rather than runnable, or count only units whose `last_xstream` is the finishing
one; counting `num_blocked` of shared pools cannot work.

### 2. `ABT_init` after `ABT_finalize` deadlocks inside `ConverseInit` — `src/init.cpp`

Affects **cond_static, eventual_static, ext_thread_cond, ext_thread_eventual,
ext_thread_join, self_rank_id** — all six hang, all six for this reason.

Argobots 1.x requires initialization for `ABT_info_query_config`, so a large
family of upstream tests opens with

```c
ABT_init(0, NULL);
ABT_info_query_config(ABT_INFO_QUERY_KIND_ENABLED_EXTERNAL_THREAD, &flag);
ABT_finalize();
...
ATS_init(argc, argv, num_xstreams);   /* the real ABT_init */
```

The second `ABT_init` hangs. Backtrace of the hung main thread (identical in
all six):

```
frame #0: CmiInitState(int) + 924
frame #1: converseRunPe(int, int) + 124
frame #2: ConverseInit(int, char**, void (*)(int, char**), int, int) + 1488
frame #3: ABT_init + 544
frame #4: ATS_init + 92
frame #5: main
```

with the worker PE threads parked in the same `CmiInitState` barrier and one
thread in `comm_backend::progress()`. In `self_rank_id` and `ext_thread_join`,
where the process got far enough to be probed, the shim's xstream table after
the second `ABT_init` shows every slot free — the primary xstream is not
re-registered.

PHASE2-PLAN says "one cycle per process", but a second `ABT_init` currently
hangs rather than failing, and Margo/Thallium decide ownership with
`ABT_initialized()`, so a clean second cycle (or at minimum a non-hanging
error) is needed. Note `ABT_MAX_NUM_XSTREAMS` differs between the two cycles
(the first `ABT_init(0, NULL)` sees the environment's value, the second sees
what `ATS_init` putenv'd), so the second `ConverseInit` also asks for a
different PE count.

### 3. `ABT_thread_migrate` is unimplemented — `src/thread.cpp:~430`

**thread_migrate** exits 77 on the first call (`thread_migrate.c:27`). `ABT_thread_migrate_to_pool`
works (core_smoke exercises it); the test uses the "any xstream" form
`ABT_thread_migrate(thread)`, which can be expressed as a migration to the
target xstream's main-scheduler pool[0].

### 4. Unsynchronized ULT key storage corrupts the heap — `src/thread.cpp:293-303`, `src/abti.h`

**thread_data2** aborts. First the allocator complains
(`malloc: *** error for object 0x…: pointer being freed was not allocated`),
then `assert(check == NULL || check == (void *)(intptr_t)i)` fails at
`thread_data2.c:44`.

`ABTI_thread::keys` is a plain `std::vector<void *>` with no lock, and
`ABT_thread_set_specific` grows it (`t->keys.resize(k->id + 1, nullptr)`,
`src/thread.cpp:297`) while the *target* ULT is running on another PE and
calling `ABT_key_get` / `ABT_key_set` on the same vector (`src/thread.cpp:303`).
thread_data2 does exactly this on purpose: the main ULT calls
`ABT_thread_set_specific(threads[i], ...)` for keys 3..7 while those ULTs read
and write the same keys. Two concurrent reallocations of one vector free the
same buffer twice, hence the malloc error.

Fix needs either a per-thread lock around the key vector, or a fixed-size /
atomically-published array so `set_specific` from another ULT is safe.

### 5. `ABT_thread_attr_set_stack` rejects a user-provided stack — `src/thread.cpp:337`

**thread_attr2** exits 77 at `thread_attr2.c:167` (`Case 4: use
ABT_thread_attr_set_stack() with stack`). The shim returns
`ABT_ERR_FEATURE_NA` whenever `stackaddr != NULL`. User-provided stacks are not
listed as out of scope in PHASE2-PLAN. If Reconverse cannot adopt a caller's
stack, the pragmatic conformance behavior is to accept the attribute, use the
requested *size* and ignore the address (Argobots itself only promises the size
is honored); silently ignoring it is what upstream does for some platforms.

### 6. The number of xstreams is hard-capped at the PE count fixed by `ABT_init` — `src/xstream.cpp` / `src/init.cpp`

**ext_thread** fails with `ABT_ERR_INV_XSTREAM_RANK` from `ABT_xstream_create`
at `ext_thread.c:117`; **ext_thread2** the same at `ext_thread2.c:105`.

`ABT_init` sizes `ABTI_g->xstreams` to `ABT_MAX_NUM_XSTREAMS` (default: online
cores) and every xstream is a leased PE, so `ABT_xstream_create` fails once the
PEs run out. In Argobots, `ABT_MAX_NUM_XSTREAMS` only sizes an internal array;
creating more execution streams than that is legal, and the upstream tests do
it routinely: `ATS_init(argc, argv, N)` putenvs `ABT_MAX_NUM_XSTREAMS=N` and
then the test creates `N` *secondary* xstreams **plus** the primary
(ext_thread: `ATS_init(..., 1)` then two `ABT_xstream_create`).

Because `ATS_init` putenvs its own value before `ABT_init`, the ctest
`ENVIRONMENT ABT_MAX_NUM_XSTREAMS=6` cannot paper over this. Either reserve
`max + 1` PEs (or a couple of spares) at init, or let a lease oversubscribe an
existing PE when the ranks run out.

### 7. Pre-init `ABT_xstream_self` must null the output handle — `src/xstream.cpp`

**self_type** aborts at `self_type.c:207`:

```c
ret = ABT_xstream_self(&xstreams[0]);
assert(ret == ABT_ERR_UNINITIALIZED && xstreams[0] == ABT_XSTREAM_NULL);
```

The shim returns `ABT_ERR_UNINITIALIZED` (correct) but leaves `*xstream`
untouched. In the 1.x API these self-query routines write the null handle
before the error check — the same applies to `ABT_thread_self` (asserted on the
next line) and `ABT_task_self`. This is a one-line-per-function fix and it
gates every later assertion in that test (which then needs tasklets anyway).

## Notes on the harness

- Each test is registered as `argobots_<name>` with a 60 s timeout and
  `ABT_MAX_NUM_XSTREAMS=6` in its environment. Tests whose upstream `main()`
  reads positional arguments get numbers (`4 8`); tests that use
  `ATS_get_arg_val()` get getopt flags (`-e 4 -u 8 [-t 4] [-i 20]`). Feeding
  the wrong form matters: `atoi("-e")` is 0 xstreams.
- 17 tests carry the ctest label `expected_fail` (tasklets, user
  `ABT_sched_def`, `ABT_thread_exit`/`revive`). `WILL_FAIL` is deliberately not
  set, so the ctest summary shows what actually happens — and indeed
  `cond_join` carries the label but passes.
- `ABT_BUILD_ARGOBOTS_TESTS=OFF` drops the whole subdirectory.
