# Argobots `test/basic` against the shim — conformance run 4

Run 2026-09-13 on the Mac (arm64, 8 cores, AppleClang 16) against shim
`cc16a96` (tasklets emulated as small ULTs) and reconverse `09595c6`:

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

| bucket | run 1 (`0ada515`) | run 2 (`6286573`) | run 3 (`664a51d`) | run 4 (`cc16a96`) |
|---|---|---|---|---|
| pass | 28 | 36 | 43 | **49** |
| skip (`ABT_ERR_FEATURE_NA`, exit 77) | — | — | 15 | **8** |
| fail | 23 | 23 | 3 | **4** |
| of which hang | 10 | 2 | 0 | **0** |
| **total** | 61 | 61 | 61 | **61** |

Run 3's three failures are fixed (`ABT_thread_exit`, `ABT_task_self`), and
emulating tasklets as small ULTs turned six skips into passes
(cond_timedwait, eventual_test, ext_thread, thread_get_last_xstream,
thread_attr2, thread_exit). Four tests that used to skip at their first
`ABT_task_create` now run their tasklet bodies and assert on tasklet
*semantics*; they are the four failures below.

The 8 remaining skips are unchanged plan exclusions: user `ABT_sched_def`
(main_sched, unit, sched_user_ws, xstream_set_main_sched), `ABT_self_schedule`
(sched_on_thread, core C4), `ABT_pool_user_def_create` (pool_custom, core C3),
`ABT_thread_revive` (thread_create4) and `ABT_self_exit`/`ABT_thread_exit_to`
(ext_thread2).

## The 4 failures: tasklet semantics

All four are in `src/thread.cpp` (plus one in `src/sched.cpp`/`src/pool.cpp`);
none is intrinsic to running a tasklet as a small ULT, and three of the four
are a few lines. Ground truth for each was taken from a scratch program
linked against `build/libabt.a`, not from reading the code.

| test | assertion | tasklet property it relies on | cheap? |
|---|---|---|---|
| self_type | `self_type.c:31` `ret == ABT_ERR_INV_THREAD && thread == ABT_THREAD_NULL` | a tasklet is *not* a ULT for the kind-checked self queries | yes — T1, T2, T3 |
| thread_yield2 | `thread_yield2.c:62` `my_count == g_count` | a tasklet never yields: nothing else runs between two statements of its body | yes — T4 |
| sched_prio | `sched_prio.c:232` `valid == ABT_TRUE` | a PRIO scheduler runs *no* unit from pool *i* while any pool < *i* is non-empty | yes — T5 (but see the nested-scheduler caveat) |
| sched_stack | `sched_stack.c:104` `value == 3 + 2 * num_tasks` | a scheduler pushed into a pool runs as a work unit, and `ABT_finalize` drains the primary pool | partly — T6 + T7 |

### T1. `is_task` is only set for *named* tasklets, and set too late — `src/thread.cpp:489-497`

```c
static int task_create_impl(ABT_pool pool, void (*fn)(void *), void *arg, ABT_task *newtask) {
  ...
  int r = ABT_thread_create(pool, fn, arg, attr, newtask ? &th : nullptr);
  ...
  if (newtask) { ABTI_thread_get(th)->is_task = true; *newtask = th; }   /* <-- */
}
```

An *unnamed* tasklet (`newtask == NULL`) — which is what self_type,
sched_prio, sched_stack and pool-side tests all create — is never marked, so
every kind-checked query treats it as a ULT. Measured in a tasklet created
with `ABT_task_create(pool, fn, NULL, NULL)`:

```
thread_self ret=0 handle_null=0 | task_self ret=18 (INV_TASK) | self_get_type type=0 (THREAD)
```

and with `&task` instead of `NULL`:

```
thread_self ret=16 (INV_THREAD) handle_null=1 | task_self ret=0 | self_get_type type=1 (TASK)
```

That is exactly self_type.c:31. Second defect in the same three lines: even
for a named tasklet the flag is set *after* `ABT_thread_create` has already
made the unit runnable, so a tasklet that starts on another PE can observe
`is_task == false`. Both go away by passing the flag into creation:

```diff
-static int task_create_impl(ABT_pool pool, void (*fn)(void *), void *arg, ABT_task *newtask) {
-  ABT_thread th;
-  ABT_thread_attr attr; ABT_thread_attr_create(&attr);
-  ABT_thread_attr_set_stacksize(attr, 64 * 1024);
-  int r = ABT_thread_create(pool, fn, arg, attr, newtask ? &th : nullptr);
-  ABT_thread_attr_free(&attr);
-  if (r != ABT_SUCCESS) return r;
-  if (newtask) { ABTI_thread_get(th)->is_task = true; *newtask = th; }
-  return ABT_SUCCESS;
-}
+/* thread_create_impl() takes the flag and sets t->is_task before
+ * CthAwakenIfBlocked(), so the tasklet can never observe it unset. */
+static int task_create_impl(ABT_pool pool, void (*fn)(void *), void *arg, ABT_task *newtask) {
+  ABT_thread th;
+  ABT_thread_attr attr; ABT_thread_attr_create(&attr);
+  ABT_thread_attr_set_stacksize(attr, 64 * 1024);
+  int r = ABTI_thread_create_impl(pool, fn, arg, attr, &th, /*is_task=*/true);
+  ABT_thread_attr_free(&attr);
+  if (r != ABT_SUCCESS) return r;
+  if (newtask) *newtask = th;   /* unnamed tasklets are detached, but still tasks */
+  return ABT_SUCCESS;
+}
```

(`ABT_thread_create` already has the body this needs; it only has to take the
flag and apply it between `CthSetUserData` and `CthAwakenIfBlocked`. An
unnamed tasklet must still be detached — the handle is simply not returned.)

### T2. `ABT_self_get_thread` must return the current work unit whatever its kind — `src/thread.cpp:386`

`ABT_self_get_task` is a *macro alias* for `ABT_self_get_thread`
(`include/abt.h:2569`), and self_type checks both directions:

```c
/* in a ULT  */ ABT_task_self(&task);      assert(ret == ABT_ERR_INV_TASK && task == ABT_TASK_NULL);
                ABT_self_get_task(&task2); ATS_ERROR(ret, ...); assert(thread == task2);   /* :95-97 */
/* in a task */ ABT_thread_self(&thread);  assert(ret == ABT_ERR_INV_THREAD && thread == ABT_THREAD_NULL);
                ABT_self_get_thread(&thread2); ATS_ERROR(ret, ...);
                assert(task == task2 && task == thread2);                                   /* :34-43 */
```

So `ABT_thread_self`/`ABT_task_self` are kind-checked, while
`ABT_self_get_thread`/`ABT_self_get_task` are not: they return the current
unit and only fail when there is none. The shim aliases the unchecked one to
the checked one:

```diff
-int ABT_self_get_thread(ABT_thread *thread) { return ABT_thread_self(thread); }
+/* the unit, whatever its kind: ABT_self_get_task is the same function
+ * (abt.h makes it a macro alias), and self_type.c:43,97 requires both to
+ * answer in a ULT and in a tasklet with the same handle */
+int ABT_self_get_thread(ABT_thread *thread) {
+  if (thread) *thread = ABT_THREAD_NULL;
+  ABTI_CHECK_INITIALIZED();
+  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM;
+  ABTI_thread *t = ABTI_self_thread();
+  if (!t) return ABT_ERR_INV_THREAD;
+  *thread = ABTI_thread_handle(t); return ABT_SUCCESS;
+}
```

`ABT_self_get_thread_id` (aliased to `ABT_self_get_task_id`) needs the same
treatment.

### T3. `ABT_self_is_primary` from a tasklet is `ABT_ERR_INV_THREAD` — `src/thread.cpp:413`

`self_type.c:48-53` expects `ABT_ERR_INV_THREAD` with `flag == ABT_FALSE`
from inside a tasklet (1.x: a tasklet is not a ULT, so "is this the primary
ULT" has no answer); the shim returns `ABT_SUCCESS` + `ABT_FALSE`.

```diff
   ABTI_thread *t = ABTI_self_thread();
+  if (t && t->is_task) return ABT_ERR_INV_THREAD;   /* flag stays ABT_FALSE */
   *is_primary = (t && t->type == ABTI_THREAD_PRIMARY) ? ABT_TRUE : ABT_FALSE; return ABT_SUCCESS;
```

`ABT_self_on_primary_xstream` must keep returning `ABT_SUCCESS` there
(`self_type.c:55`) — it is about the ES, not the unit.

### T4. A tasklet must not yield — `src/thread.cpp:193-199`

thread_yield2's `task_func` is

```c
    my_count = ++g_count;
    ret = ABT_thread_yield();      /* 1.x: "ABT_thread_yield() does nothing" in a tasklet */
    ATS_ERROR(ret, "ABT_thread_yield");
    assert(my_count == g_count);   /* :62 */
```

One ES, eight named tasklets created before any of them runs. In Argobots a
tasklet cannot yield, so nothing can run between the two statements. Our
`ABT_thread_yield` calls `CthYield()` for anything that is not an external
thread, so tasklet *i* yields, tasklets *i+1…* run and bump `g_count`, and the
assertion fails when *i* resumes. (`ABT_self_yield` must return an error
there, but the test only checks that when `ENABLED_CHECK_ERROR` is true, which
this build reports as `ABT_FALSE`.)

```diff
 int ABT_thread_yield(void) {
   ABTI_CHECK_INITIALIZED();
-  if (!ABTI_self_thread()) return ABT_SUCCESS; /* external thread: no-op (Argobots 1.x) */
+  ABTI_thread *self = ABTI_self_thread();
+  /* an external thread and a tasklet both no-op in 1.x: a tasklet has no
+   * scheduling point, and code after the call must run before anything else */
+  if (!self || self->is_task) return ABT_SUCCESS;
   CthYield();
   return ABT_SUCCESS;
 }
```

`ABT_thread_yield_to` and `ABT_self_yield`/`ABT_self_yield_to` route through
this function, so they follow. Note this makes the emulation *stricter*, which
is the right direction: the shim's tasklets otherwise silently allow something
Argobots forbids.

### T5. `ABT_SCHED_PRIO` is weighted round-robin, not strict priority — `src/sched.cpp` (`ABTI_sched_build_table`)

sched_prio's `verify_exec_order(es_id, my_prio)` demands that when a unit of
priority *p* starts, **every** pool 0..*p*−1 of that ES is empty — i.e. a
strict priority scheduler. The shim's table gives pool *i* the Converse poll
weight `32 >> i`:

```c
case ABT_SCHED_PRIO: freq = (unsigned)(32 >> (i < 5 ? i : 5)); ...
```

so a low-priority pool is still polled while a high-priority pool has work.
The assertion fires in `task_func` only because `gen_work` creates the
tasklets at even indices; `thread_func` has the identical check and fails the
same way — this is not a tasklet issue at all, it only became reachable now.

Cheap fix: keep one entry per pool but give the poll function a
`{sched, index}` context and let it decline while a higher-priority pool has
work:

```diff
+/* strict priority: pool i may only run when pools 0..i-1 are empty */
+struct ABTI_prio_ctx { ABTI_sched *s; int idx; };   /* allocated with the sched */
+int ABTI_poll_prio_pool(void *ctx) {
+  ABTI_prio_ctx *c = static_cast<ABTI_prio_ctx *>(ctx);
+  for (int i = 0; i < c->idx; i++)
+    if (c->s->pools[i]->size() != 0) return 0;
+  return ABTI_poll_pool(c->s->pools[c->idx]);
+}
 CsdSchedTable ABTI_sched_build_table(ABTI_sched *s) {
-    case ABT_SCHED_PRIO: freq = (unsigned)(32 >> (i < 5 ? i : 5)); if (freq == 0) freq = 1; break;
+    case ABT_SCHED_PRIO: /* equal weights; precedence is enforced in the poll fn */ break;
```

with the PRIO entries using `ABTI_poll_prio_pool` and the per-index contexts
owned by the `ABTI_sched` (freed in `ABTI_sched_destroy`). Cost per poll is a
scan of at most *i* pool sizes; `ABTI_pool::size()` takes the pool mutex, so a
relaxed `std::atomic<size_t>` count maintained by push/pop would be worth
adding at the same time.

Caveat: for four of its six scheduler configurations sched_prio also stacks
the PRIO scheduler into the ES's main pool with `ABT_pool_add_sched`, which is
the same nested-scheduler gap as T6 — with `ABT_pool_add_sched` a no-op those
units simply never run (no assertion, but no coverage either).

### T6/T7. sched_stack needs a runnable sub-scheduler *and* a draining `ABT_finalize`

sched_stack builds one ES whose main pool receives, in order: task(1), 8 tasks
in subsched1's pool, `ABT_pool_add_sched(main_pool, subsched1)`, task(10), 8
tasks in subsched2's pool, `ABT_pool_add_sched(main_pool, subsched2)`,
task(19) — then calls `ATS_finalize` **without ever yielding**. Each
`task_func` asserts `v == value + 1`, and main asserts `value == 3 + 2*num_tasks`
(19 with our args).

- **T6** `ABT_pool_add_sched` is a no-op in the shim (`src/pool.cpp:251`: it
  only bumps `num_scheds`), so the 16 sub-pool tasks never run.
- **T7** `ABT_finalize` does not drain the primary ES's pools, so even the
  three main-pool tasks never run. Measured directly (scratch program, no
  yield anywhere):

  ```
  before finalize: pool size 2, ran_ult 0 ran_task 0
  finalize -> 0;   ran_ult 0 ran_task 0          <- units silently dropped
  ```

  With a single `ABT_thread_yield()` before `ABT_finalize` the same program
  reports `size 0 ran_ult 1 ran_task 1`, so the scheduling path is fine — it
  is only `ABT_finalize` that walks away from queued work. Argobots' finalize
  joins the primary ES, which means running its main scheduler until its pools
  are empty. **This is worth fixing regardless of sched_stack**: any program
  that ends with `ABT_finalize()` while work is still queued (Margo's shutdown
  path does exactly that) silently loses it today.

  Shape: before tearing anything down, `ABT_finalize` should run the primary
  ES's scheduler until `ABT_sched_get_total_size(main_sched) == 0` — the
  primary ULT can simply loop over `ABTI_poll_pool` on its own pools (the same
  poll the table uses), which is what the standin scheduler does when the
  primary blocks.

T6 is emulatable without the full stackable-scheduler machinery: make
`ABT_pool_add_sched(pool, sched)` push a ULT into `pool` whose body drains
`sched`'s pools until they are empty and then returns. For this test that
gives exactly the required order — 1, then 2..9 from subsched1, then 10, then
11..18, then 19 — because the outer pool is FIFO. It is *not* full Argobots
semantics (a real sub-scheduler can yield back and be re-entered), so if
stackable schedulers stay out of scope, sched_stack should be marked a
documented divergence and skipped; that decision is yours.

## Run-3 failures, now fixed

| test | run 3 | run 4 |
|---|---|---|
| thread_exit | fail (F1, `ABT_thread_exit` returned) | pass |
| cond_timedwait | fail (F1, double count) | pass |
| self_type | fail (F2, `ABT_task_self` from a ULT) | fails later, at the tasklet body (T1) |

## The 8 skips (out of scope per PHASE2-PLAN)

Each stops at the first call to an excluded feature; the call named is what
`ABT_RECONVERSE_TRACE_NA=1` recorded. Seven of the run-3 skips became passes
once tasklets were emulated.

| test | first unimplemented call | feature |
|---|---|---|
| main_sched | `ABT_sched_create` | user `ABT_sched_def` |
| unit | `ABT_sched_create` | user `ABT_sched_def` |
| sched_user_ws | `ABT_sched_create` | user `ABT_sched_def` |
| xstream_set_main_sched | `ABT_sched_create` | user `ABT_sched_def` |
| sched_on_thread | `ABT_self_schedule` | stackable scheduler (core C4) |
| pool_custom | `ABT_pool_user_def_create` | 2.0-style user pool (core C3) |
| thread_create4 | `ABT_thread_revive` | revive |
| ext_thread2 | `ABT_self_exit` / `ABT_thread_exit_to` | exit-to |

### Does a user-provided stack have to be used, or only reported?

(thread_attr2 — it passes now that the shim accepts a non-NULL `stackaddr`.)
Upstream `ABT_thread_attr_set_stack` says "the memory pointed to by
`stackaddr` will be used as the stack area for a created ULT" and makes
freeing it the caller's job, so native Argobots really runs the ULT there.
**The test cannot tell.** `thread_attr2.c`'s `thread_func` only checks that
`ABT_thread_get_attr` reports the same *stacksize* it asked for, then consumes
half of `stacksize` by recursion (`dummy_rec`) to prove the stack is really
that big; it never compares an address, and nothing else in this subset calls
`ABT_thread_get_stack`. Honoring the size while ignoring the address — what
the shim does — passes; what it gives up is a caller who hands over *specific*
memory (registered/pinned buffers, guard pages, a pool of reused stacks), and
the memory is then allocated twice. Margo and Thallium only ever set a stack
*size*, so the risk is theoretical for phase 3, but it should be documented
rather than silent.

## Per-test results

| test | run 4 | note |
|---|---|---|
| init_finalize | pass |  |
| xstream_create | pass |  |
| xstream_rank | pass | the run-2 teardown crash (C8) has not recurred |
| xstream_set_main_sched | skip | `ABT_sched_create` (user def) |
| main_sched | skip | `ABT_sched_create` (user def) |
| sched_set_main | pass |  |
| sched_user_ws | skip | `ABT_sched_create` (user def) |
| sched_basic | pass | sched-config implementation |
| sched_basic_wait | pass |  |
| sched_randws | pass |  |
| sched_config | pass | sched-config implementation |
| sched_on_thread | skip | `ABT_self_schedule` (C4) |
| sched_prio | **fail** | T5 — PRIO must be strict, not weighted |
| sched_stack | **fail** | T6 + T7 — sub-scheduler unit, draining finalize |
| pool_config | pass | pool-config implementation |
| pool_custom | skip | `ABT_pool_user_def_create` (C3) |
| unit | skip | `ABT_sched_create` (user def) |
| thread_create | pass |  |
| thread_create2 | pass |  |
| thread_create3 | pass |  |
| thread_create4 | skip | `ABT_thread_revive` |
| thread_create_on_xstream | pass |  |
| thread_yield | pass |  |
| thread_yield2 | **fail** | T1 + T4 — tasklets must not yield |
| thread_yield_to | pass |  |
| thread_exit | pass | was F1 |
| thread_self_suspend_resume | pass |  |
| thread_get_last_xstream | pass | tasklets |
| thread_migrate | pass |  |
| thread_data | pass |  |
| thread_data2 | pass |  |
| thread_id | pass |  |
| thread_attr | pass |  |
| thread_attr2 | pass | user stack accepted; size honored, address ignored |
| mutex | pass |  |
| mutex_spinlock | pass |  |
| mutex_static | pass | was a hang (S1) |
| mutex_recursive | pass |  |
| cond_test | pass |  |
| cond_join | pass |  |
| cond_static | pass |  |
| cond_timedwait | pass | was F1 |
| eventual_create | pass |  |
| eventual_test | pass | tasklets |
| eventual_static | pass |  |
| eventual_timedwait | pass |  |
| sync_no_contention | pass |  |
| rwlock_reader_incl | pass |  |
| rwlock_reader_writer_excl | pass |  |
| rwlock_writer_excl | pass |  |
| ext_thread | pass | was C5, then a tasklet skip |
| ext_thread2 | skip | `ABT_self_exit` / `ABT_thread_exit_to` |
| ext_thread_mutex | pass | was a hang (S1) |
| ext_thread_cond | pass |  |
| ext_thread_eventual | pass |  |
| ext_thread_join | pass | was C6 |
| info_query | pass |  |
| error | pass |  |
| timer | pass |  |
| self_rank_id | pass |  |
| self_type | **fail** | T1/T2/T3 — tasklet self queries |
## Still-open core items from run 2

- **C3** `ABT_pool_user_def_create` unimplemented (pool_custom) — described in
  the run-2 notes; no patch, and the test needs `ABT_sched_def` +
  `ABT_thread_revive` afterwards.
- **C4** `ABT_self_schedule` / `ABT_xstream_run_unit` unimplemented
  (sched_on_thread) — patch proposed in run 2 via `ABTI_pool_run_thread`, with
  the caveat that it resumes a ULT from another ULT's context. If stackable
  schedulers are meant to stay out of scope, say so and the test is a plain
  skip like the others. T6 (sched_stack's `ABT_pool_add_sched`) is the same
  question asked a second way.
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
  set. With skips reported properly the label has outlived its use: of the
  tests that still fail, three (sched_prio, sched_stack, thread_yield2) carry
  it and one (self_type) does not. The ctest status is now the honest signal;
  the label can go when the four tasklet items above are settled.
- `ABT_BUILD_ARGOBOTS_TESTS=OFF` drops the whole subdirectory.
- Diagnosing hangs: `ABT_info_print_all_xstreams` can be called on a live
  process with
  `lldb -b -p <pid> -o 'expr (int)ABT_info_print_all_xstreams((void*)0)' -o detach`;
  it prints each xstream's rank/state/finishing flags and every pool's
  size/blocked counts to the process's stdout.

## Note 2026-09-13 20:10 — eventual_timedwait flakiness is the test's own race

`g_success_counter++` (a plain `volatile int`) is incremented by several
waiter ULTs that the single `ABT_eventual_set` wakes at once; on the shim
they resume on different xstreams simultaneously and a lost update gives
"success_counter = 3 (expected: 4)" in roughly 1 run in 5 under `-j4`. The
same race exists upstream; native Argobots' wake timing just makes it rare.
Not a shim defect.
