# daos_mock — DAOS's Argobots usage, reproduced against the shim

`daos_mock.c` is a standalone C99 program that makes the same Argobots calls,
in the same order and with the same object shapes, as the DAOS engine. DAOS
itself is Linux-only, so this is the pilot for the engine step of
`../../../daos/DAOS-ABT-USAGE.md` §7: it exercises the one large gap in that
survey — DAOS's **user-defined scheduler** (`ABT_sched_create` with a real
`ABT_sched_def`, plus `ABT_xstream_run_unit`) — together with the pools, the
poll ULTs, the tasklets and the synchronization objects that surround it.

No DAOS code is copied; the control flow is rewritten from the survey and from
reading the DAOS sources. Everything DAOS does with a storage stack behind it
(Mercury progress, SPDK polling, metrics, watchdog reporting) is replaced by a
counter.

* Source read: shallow clone at `../../../daos/daos`, commit
  `d0f733f6e4bb63fa89a41154dd830f1ad5163b8d`, `VERSION` 2.9.100.
* Paths below are relative to that clone (`src/engine/...`).

## Build and run

```
cd abt-reconverse/build && cmake .. && make -j8 && ctest -R daos_mock
```

Registered in `../CMakeLists.txt` alongside `core_smoke` / `sync_smoke`:
linked directly against `abt_static`, `TIMEOUT 180`. One run is ~0.2 s.

Diagnostics: `ABT_RECONVERSE_DEBUG=1` traces runtime events (the per-PE
`runner start` / `runner end` lines are the user scheduler taking and
releasing its PE); `ABT_RECONVERSE_TRACE_NA=1` prints any call that lands on
an unimplemented shim entry point. `daos_mock` prints none.

## Mapping: mock → DAOS

### Setup, per "target" xstream

| mock (`daos_mock.c`) | DAOS |
|---|---|
| `enum { DSS_POOL_NET_POLL, DSS_POOL_NVME_POLL, DSS_POOL_GENERIC, DSS_POOL_CNT }` | `srv_internal.h:23-26` |
| `struct dss_xstream` (ABT fields only) | `srv_internal.h:89-119` |
| `struct sched_cycle` | `sched.c:1792-1799` |
| `struct sched_data` | `sched.c:1801-1806` |
| `SCHED_AGE_NET_MAX` 32, `SCHED_AGE_NVME_MAX` 64 | `sched.c:1823-1824` |
| `sched_create_pools()` — 3 × `ABT_pool_create_basic(ABT_POOL_FIFO, ABT_POOL_ACCESS_MPSC, ABT_TRUE, ...)` | `sched.c:2445-2465`, the `ABT_pool_create_basic` call at `:2458` (MPSC is deliberate: in-pool ULTs create ULTs for *other* xstreams, comment `:2448-2451`) |
| `dss_sched_init()` — `ABT_sched_config_var {idx 0, INT}` / `{idx 1, PTR}`, `ABT_sched_def {ABT_SCHED_TYPE_ULT, sched_init, sched_run, sched_free, NULL}`, `ABT_sched_config_create(&config, event_freq, 512, dx_ptr, dx, ABT_sched_config_var_end)`, `ABT_sched_create(&def, DSS_POOL_CNT, dx->dx_pools, config, &dx->dx_sched)`, `ABT_sched_config_free` | `sched.c:2476-2521`; the def at `:2488-2494`, config create `:2506`, sched create `:2511`, config free `:2513` |
| `ABT_xstream_create_with_rank(dx->dx_sched, t + 1, &dx->dx_xstream)`, and `ABT_xstream_self_rank() == 0` for the primary | `srv.c:839`, comment `:838` "ABT rank 0 is reserved for the primary xstream" |
| `ABT_MAX_NUM_XSTREAMS` set to `1 + NR_TGT` before `ABT_init`; `ABT_init` on the process main thread | `init.c:516-546` (`d_setenv`), `ABT_init` `:551`, `ABT_finalize` `:566` |

The mock creates 3 targets (DAOS: `dss_sys_xs_nr + dss_tgt_nr +
dss_tgt_offload_xs_nr`, `srv_internal.h:337`). DAOS's main thread parks in
`sigwait()` (`init.c:1201`) and runs no ULTs; the mock's primary ULT does the
driving instead — that is the one structural difference from the engine.

### The scheduler callbacks

| mock | DAOS |
|---|---|
| `sched_init()` — `calloc`, `ABT_sched_config_read(config, 2, &sd_event_freq, &sd_dx)`, `ABT_sched_set_data` | `sched.c:1827-1846`; read `:1836`, set_data `:1844` |
| `sched_free()` — `ABT_sched_get_data` then free | `sched.c:2421-2430` |
| `need_net_poll()` | `sched.c:1849-1871` |
| `sched_pop_net_poll()` — age bookkeeping, `ABT_pool_pop` | `sched.c:1872-1900`, pop at `:1893` |
| `need_nvme_poll()` | `sched.c:1903-1939` (the `bio_need_nvme_poll()` backlog branch at `:1934` has no analogue here) |
| `sched_pop_nvme_poll()` | `sched.c:1941-1969` |
| `sched_pop_one()` — decrements the cycle count, tolerates a NULL unit | `sched.c:1971-2006`, the NULL-unit comment at `:1988` ("`ABT_thread_join()` could have removed the ULT") |
| `sched_try_relax()` — `ABT_pool_get_size` on all three pools, `ABT_pool_get_total_size` on GENERIC to count blocked ULTs, then either set `dx_timeout` or `usleep(100)` | `sched.c:2020-2098`; total size `:2046`, `dx_timeout` `:2099`, `usleep` `:2101` |
| `sched_start_cycle()` — `ABT_pool_get_size(GENERIC)` sizes the next cycle, then relax | `sched.c:2180-2233`, size read `:2214`, relax `:2224` |
| `sched_watchdog_prep()` — `ABT_unit_get_thread` + `ABT_thread_get_thread_func`, stash the symbol | `sched.c:2280-2298`, calls at `:2293` and `:2295` |
| `sched_run()` — net poll, NVMe poll, `goto start_cycle` when the cycle is empty, one generic ULT, `execute:` → watchdog prep → **`ABT_xstream_run_unit(unit, pool)`** → `start_cycle:` → `check_event:` every 512 iterations `ABT_sched_has_to_stop` (break) else `ABT_xstream_check_events` | `sched.c:2345-2418`; `ABT_sched_get_data` `:2356`, `ABT_sched_get_pools` `:2360`, run_unit `:2397`, has_to_stop `:2409`, check_events `:2415` |

The mock's `sched_run` keeps DAOS's `goto` structure verbatim so the cycle
state machine (`sc_new_cycle` / `sc_cycle_started` / `sc_ults_tot`) is the same
one, including the assertions DAOS makes with `D_ASSERT`.

### The work in those pools

| mock | DAOS |
|---|---|
| `net_poll_ult()` — `usleep(dx_timeout)` (standing in for `crt_progress`), break on the stop flag, else `ABT_thread_yield()`; asserts `ABT_thread_get_last_pool()` is still `DSS_POOL_NET_POLL` | `dss_srv_handler()` `srv.c:406`; `crt_progress(dmi->dmi_ctx, dx->dx_timeout)` `:584`, exit test `:594`, yield `:599`. Created into `DSS_POOL_NET_POLL` at `srv.c:859` |
| named RPC-handler ULTs, `ABT_thread_create` into another xstream's `DSS_POOL_GENERIC`, handle kept | `sched_create_thread()` `srv_internal.h:312-330`, create at `:328`; the progress and chore ULTs keep handles (`srv.c:859`, `ult.c:905`) |
| `ABT_thread_is_unnamed` check on each named handle | `sched.c:1657`, `sched_req_get()` rejects unnamed ULTs |
| unnamed handler ULTs (`newthread == NULL`) | `ult.c:195` (collective fan-out) and `ult.c:655` (`dss_ult_create_all`) |
| `nvme_poll_task()` — `ABT_task_create` into `DSS_POOL_NVME_POLL`, then `ABT_task_free` (which joins) | `sched_create_task()` `srv_internal.h:291-308` (`ABT_task_create` `:307`); the create-then-free idiom is `dss_srv_set_shutting_down()` `srv.c:1517`, `ABT_task_free` `:1520`. **Deviation:** DAOS's two tasklet producers both target `DSS_POOL_GENERIC`; the mock pushes into `DSS_POOL_NVME_POLL` so that the scheduler's NVMe-poll branch (`sched_pop_nvme_poll` → `ABT_xstream_run_unit` on a stackless unit) is exercised at all |
| `g_mutex` + `g_mutex_counter` under contention from every target | `ABT_mutex_create` — 41 lib sites, `lock`/`unlock` 149/187 |
| `g_rmutex`: `ABT_mutex_attr_create` → `ABT_mutex_attr_set_recursive` → `ABT_mutex_create_with_attr` → `ABT_mutex_attr_free`, then a nested lock/lock/unlock/unlock | `vos/sys_db.c:377,383,389,395` — DAOS's only recursive mutex |
| `cond_waiter()` blocking on `ABT_cond_wait` until a flag, woken by one `ABT_cond_broadcast` from the primary | `dss_chore_queue_ult()` `ult.c:806`, blocking at `:840`, broadcast from another xstream `ult.c:765,922` |
| `g_rwlock` readers and a writer per target | 8 distinct rwlocks across `src/pool`, `src/container`, … ; no nesting and no read→write upgrade anywhere, so the mock has none either |
| `ABT_eventual_create(sizeof(int))`, waiter on target *t*, setter on target *(t+1) % N* | the offload-completion idiom, canonical shape `engine/rpc.c:36-56` (create `:36`, set from the callback `:18`, wait `:46`, free `:53`) |
| `ABT_future_create(2, future_cb, &g_future)` with two setter ULTs on two targets and one callback | `ult.c:135` `ABT_future_create(xs_nr + 1, collective_reduce, &future)` |
| `ABT_future_test` on the primary | `srv_internal.h:286` polls `dx_stopping` that way; the one-shot latches themselves are `srv.c:652,658` |
| `ABT_key_create(NULL, &g_key)`, `ABT_key_set` / `ABT_key_get` around a yield | **no DAOS analogue.** `ABT_key_*` has zero hits in DAOS; per-ULT storage is never used (survey §3, "Keys: none"). Included because the shim supports it and DAOS's TLS invariant is the mirror image: `pthread_key_t` per OS thread, `common/tls.c:19,130` |
| `ABT_self_get_xstream_rank()` asserted equal to `dx_xs_id + 1` before *and* after every yield, in every ULT | gap M3: DAOS's TLS is per xstream, so a ULT that resumes on a different xstream silently reads another target's `dss_module_info`. This is the assertion that would catch it |
| `ext_pthread()` — a native pthread that does `ABT_thread_create` into a target's generic pool and `ABT_eventual_set` | DAOS's inverse of the SPDK handoff: `bio_monitor.c:158,86` (`spdk_thread_send_msg` → `ABT_eventual_set`). DAOS's own pthreads (`mgmt/srv_target.c:847,1044`) call no `ABT_*`, so this is deliberately *stricter* than DAOS |

### Shutdown

| mock | DAOS |
|---|---|
| join/free the named ULTs, wait out the unnamed ones | `ABT_thread_join` 17 lib sites, `ABT_thread_free` 38 |
| set `dx_stopping` so the poll ULTs exit, then join/free them | `dss_srv_set_shutting_down()` `srv.c:1517`; `dss_xstream_exiting()` breaks the progress loop at `srv.c:594` |
| `ABT_xstream_join(dx->dx_xstream)` then `ABT_xstream_free(&dx->dx_xstream)` | `srv.c:889-890` and `:943-944` |
| `ABT_sched_free(&dx->dx_sched)`, whose `sched_free` reads the data back | `dss_sched_fini()` `sched.c:2467-2474`, comment `:2470` "Pools will be automatically freed by `ABT_sched_free()`" |
| `sched_create_pools` + `sched_free_pools` on a throwaway `dss_xstream`, to exercise `ABT_pool_free` | `sched_free_pools()` `sched.c:2432-2443`, DAOS's `dss_sched_init` error path. It is **not** called on the success path, and the mock does not call it there either: the pools are `automatic`, so `ABT_sched_free` has already destroyed them |
| free the sync objects, `ABT_finalize()` | `server_fini()` → `abt_fini()` `init.c:566` |

## What it asserts

Every counter, after `ABT_finalize`: named and unnamed handlers run
(3×4 and 3×3), per-ULT key reads (12), cond waiters entered and woken (3/3),
eventual values (3, cross-xstream), the future callback fired exactly once with
both compartment values, the pthread-created ULT ran once, tasklets run
(15), net polls > 3 and exactly 3 poll-ULT exits, the mutex and recursive-mutex
counters exactly, reader/writer counts and the rwlock's even-value invariant,
`ABT_xstream_run_unit` calls ≥ units accounted for, and per-target:
`dx_units_run > 0`, `dx_cycles > 0`, `dx_relax > 0`,
`dx_watchdog_prep == dx_units_run`, `dx_sched_freed == 1`.

Inside the scheduler it additionally asserts that `ABT_sched_get_pools` hands
back the same three pool handles `ABT_pool_create_basic` produced, and that
`ABT_pool_get_total_size >= ABT_pool_get_size` for the generic pool.

## Result and observations

10/10 ctest passes (30/30 direct runs), ~0.2 s each, no
`ABT_RECONVERSE_TRACE_NA` output — that is, the mock hits no unimplemented shim
entry point. The `ABT_RECONVERSE_DEBUG=1` trace shows one `runner start` /
`runner end` per target PE: each user scheduler's `run` owns its PE for the
life of the xstream and returns through `ABT_sched_has_to_stop`, which is what
`ABT_xstream_join` waits on.

Two things worth recording, neither a failure:

* **PE floor (gap M2 in the survey).** With `ABT_MAX_NUM_XSTREAMS=4` the
  runtime still starts 16 PEs — `ABTI_num_pes_rule()` applies a floor of
  `max(16, 2 × cores)` (`src/init.cpp:57`). Unleased PEs sleep, so this costs
  memory, not CPU. The hard cap of 128 is the half that matters for a real
  engine (a 64-target engine needs ~132).
* **Idle relax is the mock's own design.** DAOS relaxes by setting
  `dx->dx_timeout` so the net-poll ULT blocks inside Mercury; with no Mercury
  here the poll ULT `usleep()`s for `dx_timeout` instead, and the scheduler
  `usleep()`s directly only when even the net-poll pool is empty. Without that,
  the always-runnable poll ULT keeps each target PE at 100%, exactly as it
  would in a DAOS engine whose relax logic never engages
  (`sched.c:2166`, the "xs N is inactive" warning, is DAOS's own detector for
  that).
