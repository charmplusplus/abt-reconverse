# Argobots `test/basic` against the shim — conformance run 3

Run 2026-09-13 on the Mac (arm64, 8 cores, AppleClang 16) against shim
`664a51d` (core `05395d0` + sched/pool configs) and reconverse `09595c6`:

```
cd build && cmake .. && make -j8
ABT_RECONVERSE_TRACE_NA=1 ctest -R argobots -j4 --timeout 60
```

61 of the 62 requested tests exist upstream and are built here; `rwlock_test`
does not exist, so the three real rwlock tests stand in for it. Test sources
are compiled in place from
`/Users/kale/software/argobots-succession/mochi/argobots/test` and are never
modified.

**Skips are now reported as skips.** Argobots' `ATS_ERROR()` prints `Skipped`
and exits **77** when a call returns `ABT_ERR_FEATURE_NA` — that is an upstream
test saying "this build does not have the feature I test". Every `argobots_*`
test now carries ctest's `SKIP_RETURN_CODE 77`, so a FAIL means a real wrong
answer, hang or crash. The `libc++abi: terminating` that used to follow
`Skipped` is **gone**: 0 occurrences in the whole run (reconverse `09595c6`).

Diagnosis method unchanged: `ABT_RECONVERSE_TRACE_NA=1` names the
unimplemented call on stderr, aborts are re-run under `lldb`, hangs are probed
with `lldb -p <pid> -o "thread backtrace all"` and
`expr (int)ABT_info_print_all_xstreams((void*)0)`.

## Summary

| bucket | run 1 (`0ada515`) | run 2 (`6286573`) | run 3 (`664a51d`) |
|---|---|---|---|
| pass | 28 | 36 | **43** |
| skip (`ABT_ERR_FEATURE_NA`, exit 77) | — | — | **15** |
| fail | 23 | 23 | **3** |
| of which hang | 10 | 2 | **0** |
| **total** | 61 | 61 | **61** |

All 15 skips are features PHASE2-PLAN puts out of scope. All 3 failures are
the same two gaps in `src/thread.cpp`: `ABT_thread_exit` and `ABT_task_self`.

## The 3 failures

| test | failure | root cause |
|---|---|---|
| thread_exit | `Assertion failed: (0), thread_exit.c:24` | F1 — `ABT_thread_exit` returns instead of terminating the ULT |
| cond_timedwait | `g_counter = 15 (expected: 8)`, exit 1 | F1 — same; the timed-out ULTs fall through and count twice |
| self_type | `assert(ret == ABT_ERR_INV_TASK && my_task == ABT_TASK_NULL)`, `self_type.c:246` | F2 — `ABT_task_self` returns `ABT_ERR_FEATURE_NA` from a ULT |

### F1. `ABT_thread_exit` returns to its caller — `src/thread.cpp:~431`

**thread_exit** (`thread_exit.c:20-24`) calls `ABT_thread_exit()` (or
`ABT_self_exit()`) *without checking the return value* and then asserts that
control never gets there:

```c
        ABT_thread_exit();
    } else {
        ABT_self_exit();
    }
    assert(0);
```

So this one cannot become a skip: the shim returns `ABT_ERR_FEATURE_NA`, the
ULT keeps running, and `assert(0)` fires.

**cond_timedwait** is the same bug with an arithmetic tail. Each of the 8 ULTs
locks the mutex and calls `ABT_cond_timedwait(cond, mutex, now + 1 s)`:

```c
    ret = ABT_cond_timedwait(cond, mutex, &ts);
    if (ret == ABT_ERR_COND_TIMEDOUT) {
        g_counter++;                      /* (1) counted here … */
        ret = ABT_mutex_unlock(mutex);
        ABT_thread_exit();                /* … and must NOT return */
    }
    ATS_ERROR(ret, "ABT_cond_timedwait"); /* ret is ABT_SUCCESS from unlock */
    g_counter++;                          /* (2) counted a second time */
    ret = ABT_mutex_unlock(mutex);        /* … and unlocks a mutex it does not hold */
```

Which waits *must* time out and which *must* be signaled? **Neither is
required** — that is the point of the test. The main ULT does a single
`ABT_thread_yield()` and then broadcasts, so whether a given ULT has reached
`ABT_cond_timedwait` by then is a race, upstream too; the test is written so
that both outcomes count exactly **one** per ULT (signaled: falls through and
counts once; timed out: counts once and *exits*). `expected = num_threads = 8`
either way.

Our number is `8 + (number that timed out)`. A verbose run
(`ATS_VERBOSE=1 ./cond_timedwait -e 4 -u 8`) shows every timed-out ULT printing
both lines:

```
[U9:E3] cond timed out
[U9:E3] cond waken up      <- the fall-through after ABT_thread_exit() returned
...
g_counter = 15 (expected: 8)     /* 7 timed out x 2 + 1 signaled x 1 */
```

Earlier runs gave 10 and 11 (2 and 3 timeouts). So the yield-poll in
`src/sync.cpp` is **not** at fault: `realtime_to_wall()` converts the absolute
`CLOCK_REALTIME` deadline into `CmiWallTimer`'s clock correctly (relative
offset added to `CmiWallTimer()`), the waiters wait the full second, the
timeout return value is right, and `cond_wait_impl` re-acquires the mutex
before returning `ABT_ERR_COND_TIMEDOUT` as it must. The only defect is that
`ABT_thread_exit` comes back. (The second `ABT_mutex_unlock` on a mutex this
ULT no longer owns is a knock-on effect of the same thing: it releases the
mutex out from under whoever holds it.)

That most ULTs time out is expected for this test at `-e 4 -u 8`: the broadcast
is issued a few microseconds after the ULTs are created, before they run.

**Proposed fix** (in `src/thread.cpp` and `src/abti.h`, both yours — *not
applied*). Reconverse exposes no "terminate the current thread" primitive
(`CthFree`/`CthSuspend` do not unwind), so the natural implementation is a
`setjmp` at the ULT entry frame:

```diff
--- a/src/abti.h
+++ b/src/abti.h
+#include <csetjmp>
@@ struct ABTI_thread {
   std::vector<void *> keys;
   bool freed_by_exit;
+  std::jmp_buf exit_jmp;   /* ABT_thread_exit() longjmps to thread_main */
+  bool exit_armed;
 };
--- a/src/thread.cpp
+++ b/src/thread.cpp
 static void thread_main(void *arg) {
   ABTI_thread *t = static_cast<ABTI_thread *>(arg);
-  t->fn(t->arg);
+  if (setjmp(t->exit_jmp) == 0) {
+    t->exit_armed = true;
+    t->fn(t->arg);       /* ABT_thread_exit() jumps back here with 1 */
+  }
+  t->exit_armed = false;
   run_key_destructors(t);
 }
@@
-int ABT_thread_exit(void) { ABTI_UNIMPLEMENTED("ABT_thread_exit"); }
+int ABT_thread_exit(void) {
+  ABTI_CHECK_INITIALIZED();
+  ABTI_thread *self = ABTI_self_thread();
+  if (!self) return ABTI_on_pe() ? ABT_ERR_INV_THREAD : ABT_ERR_INV_XSTREAM;
+  /* the primary ULT is not ours to unwind, and neither is a ULT whose entry
+   * frame is gone */
+  if (self->type == ABTI_THREAD_PRIMARY || !self->exit_armed) return ABT_ERR_INV_THREAD;
+  longjmp(self->exit_jmp, 1);  /* does not return */
+}
```

with `ABT_self_exit()` becoming `return ABT_thread_exit();`. Two caveats worth
weighing: `longjmp` skips destructors of C++ objects live on the ULT's stack
(the Argobots API is C and the shim's own wait paths use raw spinlocks, so
nothing in-tree is affected), and the jump must happen on the ULT's own stack —
which holds even after migration, because a reconverse stack travels with its
`CthThread`. `ABT_thread_exit_to` (ext_thread2) would additionally have to
schedule the named target and stays out of scope.

Fixing F1 turns thread_exit and cond_timedwait green and moves ext_thread2's
first blocker from `ABT_thread_exit` to `ABT_self_exit`/`ABT_thread_exit_to`.

### F2. `ABT_task_self` from a ULT must be `ABT_ERR_INV_TASK` — `src/thread.cpp:473-478`

C7 was applied only to the uninitialized case, so `self_type.c:213` passes now
and `:246` fails:

```c
int ABT_task_self(ABT_task *task) {
  if (task) *task = ABT_TASK_NULL;
  ABTI_CHECK_INITIALIZED();
  ABTI_UNIMPLEMENTED("ABT_task_self");   /* ABT_ERR_FEATURE_NA */
}
```

The 1.x contract (asserted at `self_type.c:90`, `:155`, `:213`, `:246`): handle
nulled first, then `ABT_ERR_UNINITIALIZED` / `ABT_ERR_INV_XSTREAM` from an
external thread / `ABT_ERR_INV_TASK` from a ULT. None of it needs tasklets:

```diff
-  ABTI_UNIMPLEMENTED("ABT_task_self");
+  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM;
+  return ABT_ERR_INV_TASK;   /* a ULT is never a tasklet */
 }
-int ABT_task_self_id(ABT_unit_id *id) { ABTI_UNIMPLEMENTED("ABT_task_self_id"); }
+int ABT_task_self_id(ABT_unit_id *id) {
+  (void)id;
+  ABTI_CHECK_INITIALIZED();
+  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM;
+  return ABT_ERR_INV_TASK;
+}
```

self_type then reaches its tasklet section (`ABT_task_create`,
`self_type.c:115`) and becomes a skip.

## The 15 skips (out of scope per PHASE2-PLAN)

Each stops at the first call to an excluded feature; the call named is what
`ABT_RECONVERSE_TRACE_NA=1` recorded.

| test | first unimplemented call | feature |
|---|---|---|
| main_sched | `ABT_sched_create` | user `ABT_sched_def` |
| unit | `ABT_sched_create` | user `ABT_sched_def` |
| sched_user_ws | `ABT_sched_create` | user `ABT_sched_def` |
| xstream_set_main_sched | `ABT_sched_create` | user `ABT_sched_def` |
| sched_on_thread | `ABT_self_schedule` | stackable scheduler (C4) |
| pool_custom | `ABT_pool_user_def_create` | 2.0-style user pool (C3) |
| sched_prio | `ABT_task_create` | tasklets |
| sched_stack | `ABT_task_create` | tasklets |
| thread_yield2 | `ABT_task_create` | tasklets |
| eventual_test | `ABT_task_create` | tasklets |
| ext_thread | `ABT_task_create` | tasklets |
| thread_get_last_xstream | `ABT_task_create_on_xstream` | tasklets |
| thread_create4 | `ABT_thread_revive` | revive |
| ext_thread2 | `ABT_thread_exit` | thread exit |
| thread_attr2 | *(no trace: `ABT_thread_attr_set_stack` returns the error directly, `src/thread.cpp:337`)* | user-provided stack |

Two of these are core gaps rather than plan exclusions and keep their run-2
diagnoses: **C3** (`ABT_pool_user_def_create`, pool_custom — which also needs
`ABT_sched_def` and `ABT_thread_revive`) and **C4** (`ABT_self_schedule` /
`ABT_xstream_run_unit`, sched_on_thread, with a proposed patch in the run-2
notes below). ext_thread and ext_thread_join were core failures in run 2 (C5,
C6) and are now a skip and a pass.

### Does a user-provided stack have to be used, or only reported?

(thread_attr2, asked separately.) Upstream `ABT_thread_attr_set_stack` says
"the memory pointed to by `stackaddr` will be used as the stack area for a
created ULT" and makes freeing it the caller's job, so native Argobots really
runs the ULT there. **The test cannot tell.** `thread_attr2.c`'s `thread_func`
only checks that `ABT_thread_get_attr` reports the same *stacksize* it asked
for, and then consumes half of `stacksize` by recursion (`dummy_rec`) to prove
the stack is really that big; it never compares an address, and none of the
other tests in this subset call `ABT_thread_get_stack`.

So the shim has a conforming-enough option: accept a non-NULL `stackaddr`,
honor `stacksize` by letting reconverse allocate the stack, store the address
and hand it back from `ABT_thread_attr_get_stack`/`ABT_thread_get_stack`. That
makes thread_attr2 pass. What it gives up: a caller who hands over *specific*
memory (registered/pinned buffers, a guard-page arrangement, a pool of reused
stacks) silently does not get it, and the memory is allocated twice. Margo and
Thallium only ever set a stack *size*, so the risk is theoretical for phase 3 —
but if the shortcut is taken it should be documented, not silent.

## Per-test results

| test | run 3 | note |
|---|---|---|
| init_finalize | pass | |
| xstream_create | pass | |
| xstream_rank | pass | the run-2 teardown crash (C8) did not recur |
| xstream_set_main_sched | skip | `ABT_sched_create` (user def) |
| main_sched | skip | `ABT_sched_create` (user def) |
| sched_set_main | pass | |
| sched_user_ws | skip | `ABT_sched_create` (user def) |
| sched_basic | pass | fixed by the sched-config implementation |
| sched_basic_wait | pass | |
| sched_randws | pass | |
| sched_config | pass | fixed by the sched-config implementation |
| sched_on_thread | skip | `ABT_self_schedule` (C4) |
| sched_prio | skip | `ABT_task_create` |
| sched_stack | skip | `ABT_task_create` |
| pool_config | pass | fixed by the pool-config implementation |
| pool_custom | skip | `ABT_pool_user_def_create` (C3) |
| unit | skip | `ABT_sched_create` (user def) |
| thread_create | pass | |
| thread_create2 | pass | |
| thread_create3 | pass | |
| thread_create4 | skip | `ABT_thread_revive` |
| thread_create_on_xstream | pass | |
| thread_yield | pass | |
| thread_yield2 | skip | `ABT_task_create` |
| thread_yield_to | pass | |
| thread_exit | **fail** | F1 |
| thread_self_suspend_resume | pass | |
| thread_get_last_xstream | skip | `ABT_task_create_on_xstream` |
| thread_migrate | pass | |
| thread_data | pass | |
| thread_data2 | pass | |
| thread_id | pass | |
| thread_attr | pass | |
| thread_attr2 | skip | user-provided stack |
| mutex | pass | |
| mutex_spinlock | pass | |
| mutex_static | pass | was a hang (S1) |
| mutex_recursive | pass | |
| cond_test | pass | |
| cond_join | pass | |
| cond_static | pass | |
| cond_timedwait | **fail** | F1 |
| eventual_create | pass | |
| eventual_test | skip | `ABT_task_create` |
| eventual_static | pass | |
| eventual_timedwait | pass | |
| sync_no_contention | pass | |
| rwlock_reader_incl | pass | |
| rwlock_reader_writer_excl | pass | |
| rwlock_writer_excl | pass | |
| ext_thread | skip | `ABT_task_create`; the run-2 C5 rank failure is fixed |
| ext_thread2 | skip | `ABT_thread_exit` |
| ext_thread_mutex | pass | was a hang (S1) |
| ext_thread_cond | pass | |
| ext_thread_eventual | pass | |
| ext_thread_join | pass | was C6 |
| info_query | pass | |
| error | pass | |
| timer | pass | |
| self_rank_id | pass | |
| self_type | **fail** | F2 |

## Still-open core items from run 2

- **C3** `ABT_pool_user_def_create` unimplemented (pool_custom) — described in
  the run-2 notes; no patch, and the test needs `ABT_sched_def` +
  `ABT_thread_revive` afterwards.
- **C4** `ABT_self_schedule` / `ABT_xstream_run_unit` unimplemented
  (sched_on_thread) — patch proposed in run 2 via `ABTI_pool_run_thread`, with
  the caveat that it resumes a ULT from another ULT's context. If stackable
  schedulers are meant to stay out of scope, say so and the test is a plain
  skip like the others.
- **C8** the teardown use-after-free of a pool `std::mutex` (xstream_rank
  aborted once under `-j4` in run 2, has not recurred since) — root cause not
  confirmed; the suspected window is `ABT_xstream_free` destroying pools while
  the releasing PE is still in the old scheduler table.

## Notes on the harness

- Each test is `argobots_<name>` with a 60 s timeout, `SKIP_RETURN_CODE 77`,
  and `ABT_MAX_NUM_XSTREAMS=6` in its environment (most tests override it:
  `ATS_init` putenvs the xstream count it was given). Tests whose upstream
  `main()` reads positional arguments get numbers (`4 8`); tests that use
  `ATS_get_arg_val()` get getopt flags (`-e 4 -u 8 [-t 4] [-i 20]`).
- 17 tests carry the label `expected_fail`; `WILL_FAIL` is deliberately not
  set. With skips reported properly the label is now mostly redundant — the
  ones that still *fail* (thread_exit, cond_timedwait) are the interesting
  ones.
- `ABT_BUILD_ARGOBOTS_TESTS=OFF` drops the whole subdirectory.
- Diagnosing hangs: `ABT_info_print_all_xstreams` can be called on a live
  process with
  `lldb -b -p <pid> -o 'expr (int)ABT_info_print_all_xstreams((void*)0)' -o detach`;
  it prints each xstream's rank/state/finishing flags and every pool's
  size/blocked counts to the process's stdout.
