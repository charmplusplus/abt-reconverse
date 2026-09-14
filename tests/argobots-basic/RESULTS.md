# Argobots `test/basic` against the shim — conformance run 2

Run 2026-09-13 on the Mac (arm64, 8 cores, AppleClang 16) against shim commit
`6286573`, one pass:

```
cd build && cmake .. && make -j8
ABT_RECONVERSE_TRACE_NA=1 ctest -R argobots -j4 --timeout 60
```

61 of the 62 requested tests exist upstream and are built here; `rwlock_test`
does not exist, so the three real rwlock tests stand in for it. Test sources
are compiled in place from
`/Users/kale/software/argobots-succession/mochi/argobots/test` and are never
modified.

Reading the failures: `ATS_ERROR()` turns `ABT_ERR_FEATURE_NA` into
`printf("Skipped"); exit(77)`, so exit code 77 means "the shim returned
`ABT_ERR_FEATURE_NA` from the call named on that line"; `ABT_RECONVERSE_TRACE_NA=1`
names the call on stderr. Aborts were re-run under `lldb`, hangs were probed
with `lldb -p <pid> -o "thread backtrace all"` and, where the shim's own state
mattered, with `expr (int)ABT_info_print_all_xstreams((void*)0)`.

## Summary

| bucket | run 1 (`0ada515`) | run 2 (`6286573`) |
|---|---|---|
| pass | 28 | **36** |
| fail — out of scope per PHASE2-PLAN | 9 | 11 |
| fail — sync | 0 | 0 |
| fail — core | 14 | 12 |
| hang / timeout | 10 | 2 (both sync, one root cause) |
| **total** | 61 | **61** |

Fixed since run 1: the xstream-join drain deadlock (sched_randws,
thread_create3), the `ABT_init`-after-`ABT_finalize` deadlock (cond_static,
eventual_static, ext_thread_cond, ext_thread_eventual, self_rank_id),
`ABT_thread_migrate`, the unsynchronized key vector (thread_data2), and the
pre-init null handle in `ABT_xstream_self`/`ABT_thread_self` (self_type now
gets six lines further).

New since run 1: `xstream_rank` aborted **once** in the parallel run, after its
own output said "No Errors" — a teardown crash, see core diagnosis C8.
`ext_thread_join` no longer hangs; it now fails on `ABT_thread_create` from an
external pthread (C6), which the re-init fix exposed.

## Update: C1 and C2 applied (`src/sched.cpp`, `src/pool.cpp`)

`ABT_sched_config_*` and `ABT_pool_config_*` are implemented as of the commit
that carries this note, and `ABT_sched_create_basic` now honors
`ABT_sched_basic_freq` and `ABT_sched_config_automatic`. Re-running the six
tests C1/C2 touched (the rest of the suite was not re-run):

| test | before | after |
|---|---|---|
| sched_basic | fail (77) | **pass** |
| sched_config | fail (77) | **pass** |
| pool_config | fail (77) | **pass** |
| sched_prio | fail (77) on `ABT_sched_config_create` | fail: now reaches `ABT_task_create` — group (a), tasklets |
| xstream_set_main_sched | fail (77) on `ABT_sched_config_create` | fail: now reaches `ABT_sched_create` with an `ABT_sched_def` — group (a) |
| sched_user_ws | fail (77) on `ABT_sched_config_create` | fail: now reaches `ABT_sched_create` with an `ABT_sched_def` — group (a) |

So the suite stands at **39/61**, with core C3-C8 and sync S1 outstanding, and
the three tests above moved from core to out of scope. The per-test table below
is otherwise as measured in run 2.

## Per-test results

| test | result | reason |
|---|---|---|
| init_finalize | pass | |
| xstream_create | pass | |
| xstream_rank | **fail (abort)** | **core C8**: passes, then `libc++abi: … std::system_error: mutex lock failed: Invalid argument` during teardown. Passed 15/15 solo and 92/92 concurrent runs afterwards; seen once under `-j4` |
| xstream_set_main_sched | fail (77) | **core C1**: FEATURE_NA from `ABT_sched_config_create` (`xstream_set_main_sched.c:141`); then needs user `ABT_sched_def` (out of scope) |
| main_sched | fail (77) | out of scope: `ABT_sched_create` with an `ABT_sched_def` (`main_sched.c:119`) |
| sched_set_main | pass | |
| sched_user_ws | fail (77) | **core C1**: `ABT_sched_config_create` (`sched_user_ws.c:152`); then needs user `ABT_sched_def` |
| sched_basic | fail (77) | **core C1**: `ABT_sched_config_create` (`sched_basic.c:48`, `ABT_sched_basic_freq`) |
| sched_basic_wait | pass | |
| sched_randws | pass | was a hang in run 1 |
| sched_config | fail (77) | **core C1**: `ABT_sched_config_create` (`sched_config.c:102`); this test is the config API itself |
| sched_on_thread | fail (77) | **core C4**: `ABT_self_schedule` (`sched_on_thread.c:66`); `ABT_xstream_run_unit` (`:60`) is unimplemented too |
| sched_prio | fail (77) | **core C1**: `ABT_sched_config_create` (`sched_prio.c:122`); then needs tasklets |
| sched_stack | fail (77) | out of scope: `ABT_task_create` (`sched_stack.c:78`) |
| pool_config | fail (77) | **core C2**: `ABT_pool_config_create` (`pool_config.c:70`) |
| pool_custom | fail (77) | **core C3**: `ABT_pool_user_def_create` (`pool_custom.c:554`); then needs `ABT_sched_def` + `ABT_thread_revive` |
| unit | fail (77) | out of scope: `ABT_sched_create` with an `ABT_sched_def` (`unit.c:118`) |
| thread_create | pass | |
| thread_create2 | pass | |
| thread_create3 | pass | was a hang in run 1 |
| thread_create4 | fail (77) | out of scope: `ABT_thread_revive` (`thread_create4.c:53`) |
| thread_create_on_xstream | pass | |
| thread_yield | pass | |
| thread_yield2 | fail (77) | out of scope: `ABT_task_create` (`thread_yield2.c:128`) |
| thread_yield_to | pass | |
| thread_exit | fail (abort) | out of scope: `ABT_thread_exit` (`thread_exit.c:20`), then `assert(0)` at `:24` |
| thread_self_suspend_resume | pass | |
| thread_get_last_xstream | fail (77) | out of scope: `ABT_task_create_on_xstream` (`thread_get_last_xstream.c:90`) |
| thread_migrate | pass | was a core failure in run 1 |
| thread_data | pass | |
| thread_data2 | pass | was an abort (heap corruption) in run 1 |
| thread_id | pass | |
| thread_attr | pass | |
| thread_attr2 | fail (77) | out of scope: `ABT_thread_attr_set_stack` with a user-provided stack (`thread_attr2.c:167`) |
| mutex | pass | |
| mutex_spinlock | pass | |
| mutex_static | **hang** | **sync S1**: recursive mutex locked by an external pthread self-deadlocks |
| mutex_recursive | pass | |
| cond_test | pass | |
| cond_join | pass | (carries the `expected_fail` label, passes anyway) |
| cond_static | pass | was a hang in run 1 |
| cond_timedwait | fail (1) | out of scope: `ABT_thread_exit` NA, so the ULTs never exit early and the test's own check fails: `g_counter = 11 (expected: 8)` |
| eventual_create | pass | |
| eventual_test | fail (77) | out of scope: `ABT_task_create` (`eventual_test.c:163`) |
| eventual_static | pass | was a hang in run 1 |
| eventual_timedwait | pass | |
| sync_no_contention | pass | |
| rwlock_reader_incl | pass | |
| rwlock_reader_writer_excl | pass | |
| rwlock_writer_excl | pass | |
| ext_thread | fail (abort) | **core C5**: `ABT_ERR_INV_XSTREAM_RANK` from `ABT_xstream_create` (`ext_thread.c:117`) |
| ext_thread2 | fail (77) | out of scope: `ABT_self_exit` (`ext_thread2.c:31`) / `ABT_thread_exit` (`:35`) |
| ext_thread_mutex | **hang** | **sync S1**: same as mutex_static |
| ext_thread_cond | pass | was a hang in run 1 |
| ext_thread_eventual | pass | was a hang in run 1 |
| ext_thread_join | fail (1) | **core C6**: `ABT_ERR_INV_XSTREAM` from `ABT_thread_create` on an external pthread (`ext_thread_join.c:59`) |
| info_query | pass | |
| error | pass | |
| timer | pass | |
| self_rank_id | pass | was a hang in run 1 |
| self_type | fail (abort) | **core C7**: `assert(ret == ABT_ERR_UNINITIALIZED && my_task == ABT_TASK_NULL)` at `self_type.c:213`; then needs tasklets |

## (a) Out of scope per PHASE2-PLAN — list only

Eleven tests, no diagnosis needed: each dies on the first call to a feature the
plan excludes (tasklets, user `ABT_sched_def`, `ABT_thread_exit`/`revive`,
`ABT_xstream_revive`, user-provided stacks).

| test | the excluded call |
|---|---|
| main_sched | `ABT_sched_create` with `ABT_sched_def` |
| unit | `ABT_sched_create` with `ABT_sched_def` |
| sched_stack | `ABT_task_create` |
| thread_yield2 | `ABT_task_create` |
| thread_get_last_xstream | `ABT_task_create_on_xstream` |
| eventual_test | `ABT_task_create` |
| thread_create4 | `ABT_thread_revive` |
| thread_exit | `ABT_thread_exit` |
| ext_thread2 | `ABT_self_exit` / `ABT_thread_exit` |
| cond_timedwait | `ABT_thread_exit` (the ULTs then run to completion and the counter check fails) |
| thread_attr2 | `ABT_thread_attr_set_stack` with a non-NULL `stackaddr` |

Five more tests reach an out-of-scope call *after* the core blocker named
below: sched_user_ws and xstream_set_main_sched (user `ABT_sched_def`),
sched_prio and self_type (tasklets), pool_custom (`ABT_sched_def` +
`ABT_thread_revive`).

## (b) Sync

### S1. A recursive mutex locked by an external pthread self-deadlocks — `src/sync.cpp:96,107,141`

Both remaining hangs, **mutex_static** and **ext_thread_mutex**, are this one
bug. Evidence (current build, `lldb -p`, all 12 threads):

```
* thread #1   __ulock_wait → _pthread_join → main + 536          (waiting for the 4 pthreads)
  thread #2-7 __psynch_cvwait → CsdIdleSleepMaybe → CsdScheduler  (every PE asleep, no ULT work)
  thread #8   swtch_pri → cthread_yield → mutex_lock + 468 → ABT_mutex_lock → thread_func + 364
  thread #9   … mutex_lock + 468 → ABT_mutex_lock → thread_func + 348
  thread #10  … same
  thread #11  … same
```

All four external pthreads sit in `mutex_lock`'s waiter spin
(`while (!w.signaled) sched_yield()` — `src/sync.cpp:63`), the PEs are idle,
and no ULT exists: this is the "use the mutex before `ABT_init()`" block that
both tests run *after* their throwaway `ABT_init`/`ABT_finalize` pair, so the
runtime is deliberately down and the ULT side is not involved at all.

Why they never wake: `mutex_static.c`/`ext_thread_mutex.c` `thread_func` locks
each mutex `is_recursive ? 5 : 1` times in a row, and

```c
static inline uint64_t self_id(ABTI_thread *self) { return self ? self->id : (uint64_t)-1; }
...
if ((m->attrs & 1) && m->owner == me && me != (uint64_t)-1) { m->nesting++; ... }
```

gives **every** external thread the same id `(uint64_t)-1` and then explicitly
excludes that id from the recursive fast path. So the second
`ABT_mutex_lock()` by the pthread that already holds the recursive mutex does
not recurse: it pushes itself on the waitlist and waits for an unlock that only
it could perform. The other three pile up behind it. (The same `me != -1`
guard is in `ABT_mutex_trylock`, `src/sync.cpp:141`, and `ABT_mutex_spinlock`
goes through `mutex_lock`, so all three entry points are affected — the test
rotates through `ABT_mutex_lock`, `_lock_high`, `_lock_low`, `trylock` and
`_spinlock`.)

The guard cannot simply be deleted: with a shared id of `-1`, external thread B
would "recursively" acquire a mutex held by external thread A. External threads
need a *unique* identity instead. Proposed patch (compiles; **not applied** —
`src/sync.cpp` belongs to the sync engineer):

```diff
--- a/src/sync.cpp
+++ b/src/sync.cpp
@@
-static inline uint64_t self_id(ABTI_thread *self) { return self ? self->id : (uint64_t)-1; }
+/* Recursive mutexes need an owner identity for external threads too, and it
+ * must be unique per pthread: with one shared sentinel, thread B would pass
+ * the recursive check on a mutex held by thread A, and the holder itself
+ * cannot recurse.  ULT ids come from ABTI_g->next_id (small, increasing), so
+ * setting the top bit cannot collide with one. */
+static inline uint64_t self_id(ABTI_thread *self) {
+  if (self) return self->id;
+  return ((uint64_t)(uintptr_t)pthread_self()) | (1ULL << 63);
+}
@@ static int mutex_lock(ABTI_mutex *m, bool spin_only) {
-    if ((m->attrs & 1) && m->owner == me && me != (uint64_t)-1) { m->nesting++; spin_release(m->slock); return ABT_SUCCESS; }
+    if ((m->attrs & 1) && m->owner == me) { m->nesting++; spin_release(m->slock); return ABT_SUCCESS; }
@@ int ABT_mutex_trylock(ABT_mutex mutex) {
-  if ((m->attrs & 1) && m->owner == me && me != (uint64_t)-1) { m->nesting++; spin_release(m->slock); return ABT_SUCCESS; }
+  if ((m->attrs & 1) && m->owner == me) { m->nesting++; spin_release(m->slock); return ABT_SUCCESS; }
```

Needs `#include <pthread.h>` in `src/sync.cpp` if it is not already pulled in.
`ABT_mutex_unlock` setting `m->owner = 0` stays correct: 0 is neither a ULT id
(they start at 1) nor a top-bit-set pthread id. The comment on
`ABTI_mutex::owner` (`src/sync.cpp:90`) should be updated too.

## (c) Core

### C1. `ABT_sched_config_*` is unimplemented — `src/sched.cpp:145-149`

Blocks five tests: **sched_basic**, **sched_config**, **sched_prio**,
**sched_user_ws**, **xstream_set_main_sched**. All five die on the first call,
e.g. `sched_basic.c:48`

```c
ret = ABT_sched_config_create(&config, ABT_sched_basic_freq, 10,
                              ABT_sched_config_var_end);
```

→ `ABT_ERR_FEATURE_NA` → `exit(77)`. `ABT_sched_create_basic` also currently
ignores a non-NULL config (`src/sched.cpp:82`, "config objects are not
supported"); it must at least not reject one, and should honor
`ABT_sched_basic_freq` (idx −4) → `s->event_freq` and
`ABT_sched_config_automatic` (idx −3) → `s->automatic`.

Fixing this alone makes sched_basic and sched_config pass (sched_config *is*
the config API test); sched_prio then needs tasklets, and sched_user_ws /
xstream_set_main_sched need user `ABT_sched_def`.

The contract the tests check (`sched_config.c` `check_val`): `..._read` reads
positionally by index 0..num_vars−1, skipping NULL pointers and leaving unset
entries untouched; `..._set` with `val == NULL` **erases** the entry; `..._get`
on a missing key returns non-`ABT_SUCCESS` **and leaves both outputs
untouched**. Proposed patch (syntax- and type-checked against `abti.h`; **not
applied**):

```diff
--- a/src/sched.cpp
+++ b/src/sched.cpp
+#include <cstdarg>
+#include <map>
+
+/* Scheduler config: a small typed key/value map.  Keys are ABT_sched_config_var
+ * idx values; the predefined vars use negative idx (-1 end, -2 access,
+ * -3 automatic, -4 basic_freq), user vars use 0,1,2,... */
+namespace {
+struct ABTI_sched_config_val { ABT_sched_config_type type; int i; double d; const void *p; };
+}
+struct ABTI_sched_config { std::map<int, ABTI_sched_config_val> vals; };
+static ABTI_sched_config *SC(ABT_sched_config h) { return ABTI_obj<ABTI_sched_config>(h); }
+
-int ABT_sched_config_create(ABT_sched_config *config, ...) { ABTI_UNIMPLEMENTED("ABT_sched_config_create"); }
-int ABT_sched_config_read(ABT_sched_config config, int num_vars, ...) { ABTI_UNIMPLEMENTED("ABT_sched_config_read"); }
-int ABT_sched_config_free(ABT_sched_config *config) { ABTI_UNIMPLEMENTED("ABT_sched_config_free"); }
-int ABT_sched_config_set(ABT_sched_config config, int idx, ABT_sched_config_type type, const void *val) { ABTI_UNIMPLEMENTED("ABT_sched_config_set"); }
-int ABT_sched_config_get(ABT_sched_config config, int idx, ABT_sched_config_type *p_type, void *val) { ABTI_UNIMPLEMENTED("ABT_sched_config_get"); }
+int ABT_sched_config_create(ABT_sched_config *config, ...) {
+  if (!config) return ABT_ERR_INV_SCHED_CONFIG;
+  ABTI_sched_config *c = new ABTI_sched_config();
+  va_list ap; va_start(ap, config);
+  for (;;) {
+    ABT_sched_config_var var = va_arg(ap, ABT_sched_config_var);
+    if (var.idx == ABT_sched_config_var_end.idx) break;
+    ABTI_sched_config_val v{}; v.type = var.type;
+    if (var.type == ABT_SCHED_CONFIG_INT) v.i = va_arg(ap, int);
+    else if (var.type == ABT_SCHED_CONFIG_DOUBLE) v.d = va_arg(ap, double);
+    else v.p = va_arg(ap, void *);
+    c->vals[var.idx] = v;
+  }
+  va_end(ap);
+  *config = reinterpret_cast<ABT_sched_config>(c);
+  return ABT_SUCCESS;
+}
+int ABT_sched_config_read(ABT_sched_config config, int num_vars, ...) {
+  ABTI_sched_config *c = SC(config); if (!c) return ABT_ERR_INV_SCHED_CONFIG;
+  va_list ap; va_start(ap, num_vars);
+  for (int i = 0; i < num_vars; i++) {
+    void *dst = va_arg(ap, void *);
+    auto it = c->vals.find(i);
+    if (!dst || it == c->vals.end()) continue;   /* unset entries stay untouched */
+    if (it->second.type == ABT_SCHED_CONFIG_INT) *(int *)dst = it->second.i;
+    else if (it->second.type == ABT_SCHED_CONFIG_DOUBLE) *(double *)dst = it->second.d;
+    else *(const void **)dst = it->second.p;
+  }
+  va_end(ap);
+  return ABT_SUCCESS;
+}
+int ABT_sched_config_free(ABT_sched_config *config) {
+  if (!config) return ABT_ERR_INV_SCHED_CONFIG;
+  ABTI_sched_config *c = SC(*config); if (!c) return ABT_ERR_INV_SCHED_CONFIG;
+  delete c; *config = ABT_SCHED_CONFIG_NULL; return ABT_SUCCESS;
+}
+int ABT_sched_config_set(ABT_sched_config config, int idx, ABT_sched_config_type type, const void *val) {
+  ABTI_sched_config *c = SC(config); if (!c) return ABT_ERR_INV_SCHED_CONFIG;
+  if (!val) { c->vals.erase(idx); return ABT_SUCCESS; }  /* NULL deletes */
+  ABTI_sched_config_val v{}; v.type = type;
+  if (type == ABT_SCHED_CONFIG_INT) v.i = *(const int *)val;
+  else if (type == ABT_SCHED_CONFIG_DOUBLE) v.d = *(const double *)val;
+  else v.p = *(void *const *)val;
+  c->vals[idx] = v; return ABT_SUCCESS;
+}
+int ABT_sched_config_get(ABT_sched_config config, int idx, ABT_sched_config_type *p_type, void *val) {
+  ABTI_sched_config *c = SC(config); if (!c) return ABT_ERR_INV_SCHED_CONFIG;
+  auto it = c->vals.find(idx);
+  if (it == c->vals.end()) return ABT_ERR_INV_SCHED_CONFIG;  /* outputs untouched */
+  if (p_type) *p_type = it->second.type;
+  if (val) {
+    if (it->second.type == ABT_SCHED_CONFIG_INT) *(int *)val = it->second.i;
+    else if (it->second.type == ABT_SCHED_CONFIG_DOUBLE) *(double *)val = it->second.d;
+    else *(const void **)val = it->second.p;
+  }
+  return ABT_SUCCESS;
+}
```

and in `ABT_sched_create_basic`, replacing the "not supported" comment:

```diff
-  /* config objects are not supported (Margo passes ABT_SCHED_CONFIG_NULL) */
+  if (ABTI_sched_config *c = SC(config)) {
+    auto it = c->vals.find(ABT_sched_basic_freq.idx);
+    if (it != c->vals.end() && it->second.type == ABT_SCHED_CONFIG_INT && it->second.i > 0)
+      s->event_freq = it->second.i;
+    it = c->vals.find(ABT_sched_config_automatic.idx);
+    if (it != c->vals.end() && it->second.type == ABT_SCHED_CONFIG_INT)
+      s->automatic = it->second.i ? ABT_TRUE : ABT_FALSE;
+  }
```

### C2. `ABT_pool_config_*` is unimplemented — `src/pool.cpp:316-319`

**pool_config** dies at `pool_config.c:70` on `ABT_pool_config_create`. The
test only exercises the config object (create/set/get/free over int, double and
pointer values, with the key varied 1..9 to stress the hash table), so
implementing the object makes it pass — `ABT_pool_create` need not consume it
yet.

Same shape as C1, keyed by `key` instead of `idx`, with
`ABT_ERR_INV_POOL_CONFIG` as the error and `ABT_POOL_CONFIG_*` as the types:

```diff
-int ABT_pool_config_create(ABT_pool_config *config) { ABTI_UNIMPLEMENTED("ABT_pool_config_create"); }
-int ABT_pool_config_free(ABT_pool_config *config) { ABTI_UNIMPLEMENTED("ABT_pool_config_free"); }
-int ABT_pool_config_set(ABT_pool_config config, int key, ABT_pool_config_type type, const void *val) { ABTI_UNIMPLEMENTED("ABT_pool_config_set"); }
-int ABT_pool_config_get(ABT_pool_config config, int key, ABT_pool_config_type *type, void *val) { ABTI_UNIMPLEMENTED("ABT_pool_config_get"); }
+/* identical to the ABT_sched_config map in src/sched.cpp; create() takes no
+ * variadic list here, so it is even simpler */
+namespace { struct ABTI_pool_config_val { ABT_pool_config_type type; int i; double d; const void *p; }; }
+struct ABTI_pool_config { std::map<int, ABTI_pool_config_val> vals; };
+static ABTI_pool_config *PC(ABT_pool_config h) { return ABTI_obj<ABTI_pool_config>(h); }
+int ABT_pool_config_create(ABT_pool_config *config) {
+  if (!config) return ABT_ERR_INV_POOL_CONFIG;
+  *config = reinterpret_cast<ABT_pool_config>(new ABTI_pool_config());
+  return ABT_SUCCESS;
+}
+int ABT_pool_config_free(ABT_pool_config *config) {
+  if (!config) return ABT_ERR_INV_POOL_CONFIG;
+  ABTI_pool_config *c = PC(*config); if (!c) return ABT_ERR_INV_POOL_CONFIG;
+  delete c; *config = ABT_POOL_CONFIG_NULL; return ABT_SUCCESS;
+}
+int ABT_pool_config_set(ABT_pool_config config, int key, ABT_pool_config_type type, const void *val) {
+  ABTI_pool_config *c = PC(config); if (!c) return ABT_ERR_INV_POOL_CONFIG;
+  if (!val) { c->vals.erase(key); return ABT_SUCCESS; }
+  ABTI_pool_config_val v{}; v.type = type;
+  if (type == ABT_POOL_CONFIG_INT) v.i = *(const int *)val;
+  else if (type == ABT_POOL_CONFIG_DOUBLE) v.d = *(const double *)val;
+  else v.p = *(void *const *)val;
+  c->vals[key] = v; return ABT_SUCCESS;
+}
+int ABT_pool_config_get(ABT_pool_config config, int key, ABT_pool_config_type *type, void *val) {
+  ABTI_pool_config *c = PC(config); if (!c) return ABT_ERR_INV_POOL_CONFIG;
+  auto it = c->vals.find(key);
+  if (it == c->vals.end()) return ABT_ERR_INV_POOL_CONFIG;  /* outputs untouched */
+  if (type) *type = it->second.type;
+  if (val) {
+    if (it->second.type == ABT_POOL_CONFIG_INT) *(int *)val = it->second.i;
+    else if (it->second.type == ABT_POOL_CONFIG_DOUBLE) *(double *)val = it->second.d;
+    else *(const void **)val = it->second.p;
+  }
+  return ABT_SUCCESS;
+}
```

### C3. `ABT_pool_user_def_*` is unimplemented — `src/pool.cpp:320-323`

**pool_custom** dies at `pool_custom.c:554` on `ABT_pool_user_def_create`. This
is the 2.0-style user pool interface (create/free-unit, is_empty, pop, push
function pointers, plus optional init/free), a different entry point from the
1.x `ABT_pool_def` that `ABT_pool_create` already accepts and that PHASE2-PLAN
puts in scope. Not a few lines: `ABT_pool_user_def` is its own heap object that
`ABT_pool_create` must then accept alongside `ABT_pool_def`. Worth doing only
if Margo/Yokan use it — this test needs `ABT_sched_def` and
`ABT_thread_revive` afterwards anyway, so it cannot pass regardless.

### C4. `ABT_self_schedule` and `ABT_xstream_run_unit` are unimplemented — `src/thread.cpp:456`, `src/xstream.cpp:308`

**sched_on_thread** runs its own scheduling loop inside a ULT: it pops a unit
from a pool and executes it in place (`sched_on_thread.c:60` via
`ABT_xstream_run_unit`, `:66` and `:73` via `ABT_self_schedule`). Trace shows
`ABT_self_schedule not implemented`.

This is the stackable-scheduler feature that
`ABT_INFO_QUERY_KIND_ENABLED_STACKABLE_SCHED` already reports as `ABT_FALSE`,
and PHASE2-PLAN does not list it in scope — **if it is meant to be out of
scope, say so and the test moves to group (a)**. If it is meant to work, the
shim already has the primitive: `ABTI_pool_run_thread()` (`src/pool.cpp:123`)
is exactly "run this ULT on this PE now", which is what the poll function does.

```diff
--- a/src/thread.cpp
+++ b/src/thread.cpp
-int ABT_self_schedule(ABT_thread thread, ABT_pool pool) { ABTI_UNIMPLEMENTED("ABT_self_schedule"); }
+int ABT_self_schedule(ABT_thread thread, ABT_pool pool) {
+  ABTI_CHECK_INITIALIZED();
+  ABTI_thread *t = ABTI_thread_get(thread); ABTI_CHECK_NULL(t, ABT_ERR_INV_THREAD);
+  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM;
+  ABTI_pool *p = ABTI_pool_get(pool);
+  if (p) ABTI_pool_associate(t, p); /* non-NULL pool becomes the unit's "last pool" */
+  ABTI_pool_run_thread(t);
+  return ABT_SUCCESS;
+}
--- a/src/xstream.cpp
-int ABT_xstream_run_unit(ABT_unit unit, ABT_pool pool) { ABTI_UNIMPLEMENTED("ABT_xstream_run_unit"); }
+int ABT_xstream_run_unit(ABT_unit unit, ABT_pool pool) {
+  ABT_thread th;
+  int r = ABT_unit_get_thread(unit, &th);
+  if (r != ABT_SUCCESS) return r;
+  return ABT_self_schedule(th, pool);
+}
```

Caveat the core engineer should check before adopting this: it resumes a ULT
from inside another ULT's context rather than from the scheduler context, so
the resumed ULT's next block/yield returns into *this* ULT's stack.
`ABTI_pool_run_thread` is only ever called from the poll function today.

### C5. The xstream count is still capped by the PE count — `src/init.cpp:57-65`

**ext_thread** fails with `ABT_ERR_INV_XSTREAM_RANK` from `ABT_xstream_create`
at `ext_thread.c:117`. `ATS_init(argc, argv, 1)` putenvs
`ABT_MAX_NUM_XSTREAMS=1`, so `ABTI_num_pes_rule()` returns `1 + 1 = 2` PEs, and
the test then creates **two** secondary xstreams on top of the primary — three
in all — so `ABTI_xstream_lease` runs out of ranks (`src/xstream.cpp`, "more
xstreams than ABT_MAX_NUM_XSTREAMS PEs").

`max + 1` is not the rule: in Argobots `ABT_MAX_NUM_XSTREAMS` only sizes an
internal array and creating more ESs than that is legal. Since the Converse
runtime starts once per process (`g_runtime_started`), the PE count cannot be
raised later, so it has to start generous — which the existing comment already
argues for in the unset case:

```diff
--- a/src/init.cpp
+++ b/src/init.cpp
-  long n = maxx > 0 ? maxx + 1 : (cores * 2 < 16 ? 16 : cores * 2);
+  /* ABT_MAX_NUM_XSTREAMS is a hint: Argobots only uses it to size an array and
+   * lets a program create more execution streams (ext_thread does:
+   * ATS_init(...,1) then two ABT_xstream_create plus the primary).  The PE
+   * count is frozen by the first ABT_init in the process, so never go below
+   * the generous default; unleased PEs sleep. */
+  long floor_pes = (cores * 2 < 16 ? 16 : cores * 2);
+  long n = maxx > 0 ? (maxx + 1 > floor_pes ? maxx + 1 : floor_pes) : floor_pes;
```

`src/misc.cpp` needs no change: `abti_max_xstreams()` reports
`ABTI_num_pes_rule()` before init and `ABTI_g->num_pes` after, so the
`MAX_NUM_XSTREAMS` query stays self-consistent (which is all `info_query.c`
checks) whatever the rule becomes.

### C6. `ABT_thread_create` from an external pthread returns `ABT_ERR_INV_XSTREAM` — `src/thread.cpp:~120`

**ext_thread_join** now fails here (it used to hang in the re-init):

```
ABT_ERR_INV_XSTREAM (4): ABT_thread_create (ext_thread_join.c:59)
```

The test's whole point is that an external pthread creates ULTs, joins them,
and creates/frees xstreams. The shim refuses up front:

```c
if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM; /* stacks and tokens are PE-owned */
```

while `ABT_INFO_QUERY_KIND_ENABLED_EXTERNAL_THREAD` reports `ABT_TRUE`, and
Margo creates ULTs from non-Argobots threads, so this one matters beyond the
test suite.

Not a few lines, so no patch — but the machinery exists: `src/xstream.cpp`
already messages a PE for the lease and affinity operations
(`send_pe_msg(rank, …, ABTI_OP_LEASE/ABTI_OP_AFFINITY)`). Suggested shape: an
`ABTI_OP_CREATE` message carrying `{pool, fn, arg, attr, result slot,
std::atomic<int> done}`; the external caller posts it to the pool's "home" PE
(any leased PE, e.g. rank 0), then spins on `done` the same way
`ABTI_thread_join_impl` already spins for external joiners. `CthCreate` and
`CthSetUserData` then run on a PE, as today.

### C7. Pre-init `ABT_task_self` and `ABT_self_get_type` — `src/thread.cpp:463-464,390`

**self_type** now gets past the two handles fixed in `6286573` and stops at
`self_type.c:213`:

```c
ret = ABT_task_self(&my_task);
assert(ret == ABT_ERR_UNINITIALIZED && my_task == ABT_TASK_NULL);
ret = ABT_self_get_type(&type);
assert(ret == ABT_ERR_UNINITIALIZED && type == ABT_UNIT_TYPE_EXT);
```

`ABT_task_self` is `ABTI_UNIMPLEMENTED` (returns `ABT_ERR_FEATURE_NA` and never
writes the handle), and `ABT_self_get_type` returns the right error but does
not write `*type`. Neither needs tasklets: 1.x defines `ABT_task_self` on a ULT
as `ABT_ERR_INV_TASK` with the handle nulled (asserted at `self_type.c:90` and
`:242`) and on an external thread as `ABT_ERR_INV_XSTREAM` (`:155`).

```diff
--- a/src/thread.cpp
+++ b/src/thread.cpp
-int ABT_task_self(ABT_task *task) { ABTI_UNIMPLEMENTED("ABT_task_self"); }
-int ABT_task_self_id(ABT_unit_id *id) { ABTI_UNIMPLEMENTED("ABT_task_self_id"); }
+/* No tasklets in this shim, but the 1.x error contract is still well defined:
+ * the handle is nulled first, then uninitialized / external / ULT are
+ * distinguished (test/basic/self_type.c:90,155,213,242). */
+int ABT_task_self(ABT_task *task) {
+  if (task) *task = ABT_TASK_NULL;
+  ABTI_CHECK_INITIALIZED();
+  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM;
+  return ABT_ERR_INV_TASK;
+}
+int ABT_task_self_id(ABT_unit_id *id) {
+  (void)id;
+  ABTI_CHECK_INITIALIZED();
+  if (!ABTI_on_pe()) return ABT_ERR_INV_XSTREAM;
+  return ABT_ERR_INV_TASK;
+}
@@ int ABT_self_get_type(ABT_unit_type *type) {
-  ABTI_CHECK_INITIALIZED();
-  *type = ABTI_self_thread() ? ABT_UNIT_TYPE_THREAD : ABT_UNIT_TYPE_EXT;  return ABT_SUCCESS;
+  if (type) *type = ABT_UNIT_TYPE_EXT; /* written even on failure (Argobots does) */
+  ABTI_CHECK_INITIALIZED();
+  *type = ABTI_self_thread() ? ABT_UNIT_TYPE_THREAD : ABT_UNIT_TYPE_EXT;
+  return ABT_SUCCESS;
```

With this, self_type reaches its tasklet section (`ABT_task_create`,
`self_type.c:115`) and then belongs in group (a).

### C8. Teardown crash: a `std::mutex` locked after it is destroyed — `src/xstream.cpp` / `src/init.cpp`

**xstream_rank** aborted once, in the `-j4` run only, *after* printing its own
"No Errors" (i.e. after `ATS_finalize` returned):

```
26/61 Test  #9: argobots_xstream_rank .......Subprocess aborted***Exception: 0.03 sec
libc++abi: terminating due to uncaught exception of type std::__1::system_error:
           mutex lock failed: Invalid argument
```

Not reproduced afterwards: 15/15 solo runs and 72/72 six-way concurrent runs
passed, plus the standalone ctest rerun. `pthread_mutex_lock` returning
`EINVAL` under `std::mutex::lock` on macOS means the mutex memory is no longer
a live mutex — a use-after-free, and the only `std::mutex`es on that path are
`ABTI_pool::m` and `ABTI_global::xm`.

The suspicious window is lease release versus pool destruction:
`ABTI_xstream_idle_hook` installs the empty table on its own PE and *then*
publishes `finished` and wakes the joiner; the joiner (on another PE) returns
from `ABT_xstream_join` and calls `ABT_xstream_free`, which destroys the
automatic scheduler and with it the pools — while the releasing PE may still be
finishing the current `CsdScheduler` iteration over the *old* table, i.e.
inside `ABTI_poll_pool` → `ABTI_pool::pop()` → `m.lock()` on the pool that was
just deleted. `ABT_finalize` has the same shape for the primary's scheduler.

No patch proposed (it needs a design choice, and the window is in the core's
own lifetime rules). The two obvious shapes: (a) retire scheduler/pool objects
onto a list freed at `ABT_finalize` instead of deleting them in
`ABT_xstream_free`, or (b) have the releasing PE itself perform the
destruction — it is the only thread that knows it has left the old table — by
messaging the freed objects to it.

## Notes on the harness

- Each test is registered as `argobots_<name>` with a 60 s timeout and
  `ABT_MAX_NUM_XSTREAMS=6` in its environment (most tests override it:
  `ATS_init` putenvs the xstream count it was given). Tests whose upstream
  `main()` reads positional arguments get numbers (`4 8`); tests that use
  `ATS_get_arg_val()` get getopt flags (`-e 4 -u 8 [-t 4] [-i 20]`).
- 17 tests carry the ctest label `expected_fail`; `WILL_FAIL` is deliberately
  not set. cond_join, sched_prio, thread_get_last_xstream and friends show
  their real status; cond_join passes despite the label.
- `ABT_BUILD_ARGOBOTS_TESTS=OFF` drops the whole subdirectory.
- Diagnosing hangs: `ABT_info_print_all_xstreams` can be called on a live
  process with
  `lldb -b -p <pid> -o 'expr (int)ABT_info_print_all_xstreams((void*)0)' -o detach`;
  it prints each xstream's rank/state/finishing flags and every pool's
  size/blocked counts to the process's stdout. That is what identified the
  join-drain deadlock in run 1.
