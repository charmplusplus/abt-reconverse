# abt-reconverse skeleton report (phase 2, step 1)

Built 2026-09-13 on the Mac (arm64, AppleClang 16.0.0).
Scope: the skeleton only — CMake, `argobots.pc`, every `abt.h` entry point
present as a C symbol, an installable prefix, and a C-compiled ABI smoke test.
No Argobots semantics were implemented.

## Function count: found vs stubbed

The list was extracted mechanically from `include/abt.h` by
`tools/gen_skeleton.py`: take the region from `/* Init & Finalize */` to the
closing `#if defined(__cplusplus)`, drop comment-only and `#` lines, join and
split on `;`, then parse `<ret> ABT_name(<args>) ABT_API_PUBLIC` (also
tolerating the one `ABT_DEPRECATED`). Declarations spanning several lines are
handled by the join.

| | count |
|---|---|
| function declarations found in `abt.h` | **261** |
| stubbed to `ABT_ERR_FEATURE_NA` | 259 |
| implemented for real (`ABT_initialized`, `ABT_error_get_str`) | 2 |
| exported data objects (`extern` in the header) | 5 |
| total exported `ABT_*` symbols in `libabt.a` | **266** |

**The plan says 262; the header has 261.** Three independent counts agree on
261:

- `grep -c ABT_API_PUBLIC include/abt.h` = 268 = 261 functions + 5 `extern`
  data declarations + the 2 `#define ABT_API_PUBLIC` lines.
- `grep -cE '^(int|void|double|...) ' include/abt.h` (declaration-start lines)
  = 261.
- The native baseline Margo was validated against,
  `mochi/_baseline/install/lib/libabt.a`, exports exactly 261 `ABT_*` text
  symbols, and `diff` against our extracted name list is **empty** — same 261
  names, no more, no less. Same 5 data symbols.

So the surface is complete; "262" in PHASE2-PLAN.md is an off-by-one.

Per-module split (as the plan's layout prescribes; prefix-based):

| file | declarations | stubs |
|---|---|---|
| `src/init.cpp` | 3 | 2 (+ `ABT_initialized` real, + the unimplemented-call bookkeeping) |
| `src/xstream.cpp` | 29 (`ABT_xstream_*`, incl. the ES barrier) | 29 |
| `src/sched.cpp` | 17 (`ABT_sched_*`) | 17 (+ 4 config vars) |
| `src/pool.cpp` | 43 (`ABT_pool_*`, `ABT_unit_*`) | 43 (+ 1 config var) |
| `src/thread.cpp` | 98 (`ABT_thread_*`, `ABT_task_*`, `ABT_self_*`, `ABT_key_*`) | 98 |
| `src/sync.cpp` | 46 (mutex, cond, rwlock, eventual, future, barrier) | 46 |
| `src/misc.cpp` | 25 (`ABT_info_*`, `ABT_tool_*`, `ABT_timer_*`, `ABT_get_wtime`, `ABT_error_get_str`) | 24 (+ `ABT_error_get_str` real) |

## Non-int return types and their defaults

Only one function in the whole API does not return `int`:

| function | return type | stub behavior |
|---|---|---|
| `ABT_get_wtime(void)` | `double` | records the call, returns `0.0` |

There are no `void`-, `ABT_bool`- or `size_t`-returning entry points: every
other function returns an Argobots error code, so its stub is
`ABTI_UNIMPLEMENTED("ABT_xxx")`. Two functions are variadic
(`ABT_sched_config_create`, `ABT_sched_config_read`) and one is deprecated
(`ABT_pool_pop_timedwait`); both cases stub without special handling.

## The two non-stubs

- `ABT_initialized()` returns `ABT_ERR_UNINITIALIZED`. Margo and Thallium call
  it before `ABT_init` to decide who owns the Argobots lifetime.
- `ABT_error_get_str(err, str, len)` returns the macro name of the error code.
  The table is generated from the `ABT_SUCCESS` / `ABT_ERR_*` `#define`s in
  `abt.h` (58 codes, values 0..57) as a `switch`, so it is keyed by value, not
  by declaration order. Unknown code → `ABT_ERR_OTHER`, which is the 1.x
  behavior; this header has `ABT_ENABLE_VER_20_API 0`, so `ABT_ERR_INV_ARG` is
  not used. Verified by diffing the full output for codes −1..59 against the
  native baseline `libabt`: **identical**.

## `ABTI_UNIMPLEMENTED`

`src/abti.h` is deliberately small (no internal structs yet): it includes
`abt.h` and `converse.h`, asserts C++17, documents the `extern "C"` rule, and
declares three helpers implemented in `init.cpp`:

```
int ABTI_note_unimplemented(const char *name) noexcept;   /* always ABT_ERR_FEATURE_NA */
const char *ABTI_first_unimplemented(void) noexcept;
unsigned long ABTI_unimplemented_count(void) noexcept;
#define ABTI_UNIMPLEMENTED(name) return ABTI_note_unimplemented(name)
```

The first-call slot is a `std::atomic<const char *>` filled by a
`compare_exchange_strong` against `nullptr`, so the first caller wins and later
callers only bump a relaxed counter. Lock-free and safe from any thread,
including a non-PE pthread. Names are string literals, so the slot never
dangles.

## Exported data symbols and values

Taken from `mochi/argobots/src/sched/sched_config.c` and
`src/pool/pool_config.c`, defined in `src/sched.cpp` and `src/pool.cpp`:

| symbol | value | note |
|---|---|---|
| `ABT_sched_config_var_end` | `{ .idx = -1, ABT_SCHED_CONFIG_INT }` | |
| `ABT_sched_config_access` | `{ .idx = -2, ABT_SCHED_CONFIG_INT }` | ignored by Argobots |
| `ABT_sched_config_automatic` | `{ .idx = -3, ABT_SCHED_CONFIG_INT }` | |
| `ABT_sched_basic_freq` | `{ .idx = -4, ABT_SCHED_CONFIG_INT }` | |
| `ABT_pool_config_automatic` | `{ .key = -2, ABT_POOL_CONFIG_INT }` | **`const`** in the header |

A C program that prints all five fields gives byte-identical output when linked
against this shim and against the native baseline `libabt`.

## What `libreconverse.a` needed to link

`RECONVERSE_DIR` defaults to
`/Users/kale/software/argobots-succession/reconverse/build-abt-pools/install`;
`find_package` is not usable (Reconverse ships no CMake package config), so the
build uses `${RECONVERSE_DIR}/include`, `${RECONVERSE_DIR}/include/reconverse-internal`
(the `MPMCQueue` templates the built-in pools will need from step 2) and
`${RECONVERSE_DIR}/lib/libreconverse.a` directly, and fatal-errors if any of the
three is missing.

`nm -u libreconverse.a` (304 undefined names) references, beyond libc/libc++:

- `pthread_*` → `Threads::Threads` (`THREADS_PREFER_PTHREAD_FLAG ON`).
- `hwloc_*` (12 symbols: `hwloc_topology_init/load`, `hwloc_set_thread_cpubind`,
  `hwloc_bitmap_*`, …) → `find_library(HWLOC_LIBRARY hwloc)`, which resolves to
  the MacPorts `/opt/local/lib/libhwloc.dylib` — the same one the Reconverse
  build used (`HWLOC_LIBRARY:FILEPATH=/opt/local/lib/libhwloc.dylib` in its
  `CMakeCache.txt`).
- **Nothing else.** No `fi_*` (libfabric), no `lci*`/`lc_*`, no MPI, no UCX.
  That Reconverse build has `LCI_DIR:PATH=LCI_DIR-NOTFOUND` and
  `RECONVERSE_AUTOFETCH_LCI2:BOOL=OFF`, so the LCI backend is not compiled in.
  `make_fcontext`/`jump_fcontext` are defined *inside* the archive.

Both `libabt.a` and `libabt.dylib` are built from one `OBJECT` library with
`POSITION_INDEPENDENT_CODE ON`, and both link `libreconverse.a`, hwloc and
Threads, so a consumer never names Reconverse. The Reconverse include dirs are
added as `SYSTEM`: `converse.h` declares `static` functions it does not define
(`CthThreadFree`, `CopyMsg`), which `-Wall` reports in every translation unit.
With that, the whole build is **0 warnings** under `-Wall -Wextra`.

The dylib mirrors the Argobots install names (`libabt.1.2.0.dylib`,
`libabt.1.dylib`, `libabt.dylib`; `SOVERSION 1`) and carries an absolute
`INSTALL_NAME_DIR`, like the baseline build, so Margo needs no
`DYLD_LIBRARY_PATH`.

`argobots.pc` (from `argobots.pc.in`): `Name: argobots`, `Version: 1.2.0`,
`Cflags: -I${includedir}`, `Libs: -L${libdir} -labt`, and
`Libs.private: -L<reconverse>/lib -lreconverse -L/opt/local/lib -lhwloc -lpthread -lc++`.
A static link driven by `pkg-config --static` was verified to work with a C
compiler. Default `CMAKE_INSTALL_PREFIX` is `${CMAKE_BINARY_DIR}/install`.

## The nm check

`cmake/check_c_symbols.sh` runs as a POST_BUILD step on `abt_static` and again
as the ctest `c_linkage` against the *installed* archive. It requires
`ABT_init`, `ABT_thread_create`, `ABT_eventual_set` and
`ABT_sched_config_automatic` to be present as plain C symbols and rejects any
`__Z…ABT_…` mangled name (the failure mode where a definition escapes its
`extern "C"` block: it still compiles and still links inside C++, leaving the C
symbol undefined for Margo to discover).

```
check_c_symbols: OK - 266 exported ABT_* C symbols, none mangled
```

A second script, `cmake/check_surface.sh` (ctest `abi_surface`), is stricter and
is meant to keep paying off through steps 2–5: it re-derives the declared names
from `abt.h` and compares them set-wise with `nm -g` output, failing on anything
declared-but-missing or exported-but-undeclared.

```
check_surface: OK - 266 declared ABT_* names, all exported, no extras
```

`nm -g libabt.a | grep ' T _ABT_'` also diffs empty against the same extraction
from the native baseline `libabt.a`.

## Test results

Clean `cmake .. && make -j8 && make install && ctest`, 0 compiler warnings:

```
1/3 Test #1: abi_smoke ........................   Passed    0.63 sec
2/3 Test #2: c_linkage ........................   Passed    0.04 sec
3/3 Test #3: abi_surface ......................   Passed    0.04 sec
100% tests passed, 0 tests failed out of 3
```

`tests/abi_smoke.c` is compiled by `cc` (C99, `-Wall -Wextra -Werror`), not by
the C++ compiler, against the *installed* prefix through
`PKG_CONFIG_PATH=<prefix>/lib/pkgconfig` — the same path Margo takes. The
driver `tests/run_abi_smoke.sh.in` also asserts
`pkg-config --atleast-version=1.2 argobots`, which is the predicate in Margo's
configure. Its output:

```
pkg-config --modversion argobots: 1.2.0
abt-reconverse ABI smoke test
ABT_VERSION      = 1.2rc1
ABT_NUMVERSION   = 10200201
ABT_initialized() = 1
ok   ABT_initialized() == ABT_ERR_UNINITIALIZED
ABT_error_get_str(ABT_ERR_FEATURE_NA) -> "ABT_ERR_FEATURE_NA" (len 18, ret 0)
ok   ABT_error_get_str returns ABT_SUCCESS
ok   ABT_error_get_str yields the macro name
ok   ABT_error_get_str reports the right length
ok   ABT_error_get_str rejects an unknown code
ok   ABT_xstream_self() == ABT_ERR_FEATURE_NA
ok   ABT_init() == ABT_ERR_FEATURE_NA
ok   27 sampled ABT_* addresses resolved (every 10th of 261)
PASSED: 0 failure(s)
```

Note the ctest for `abi_smoke` needs `make install` first (it builds against the
prefix on purpose) and passes `-isysroot` explicitly, because ctest runs the
script with an environment that has no `SDKROOT` and `cc` then cannot find
`stdio.h`.

## Files

```
abt-reconverse/
  CMakeLists.txt              static + shared libabt, install, argobots.pc
  argobots.pc.in
  include/abt.h               unchanged (supplied)
  src/abti.h                  C++17 guard, extern "C" rule, ABTI_UNIMPLEMENTED
  src/init.cpp                ABT_init/finalize + real ABT_initialized + bookkeeping
  src/xstream.cpp src/sched.cpp src/pool.cpp src/thread.cpp src/sync.cpp
  src/misc.cpp                + real ABT_error_get_str
  cmake/check_c_symbols.sh    C-linkage / no-mangling check
  cmake/check_surface.sh      declared-vs-exported set comparison
  tests/CMakeLists.txt tests/abi_smoke.c tests/run_abi_smoke.sh.in
  tools/gen_skeleton.py       the mechanical generator (needs --force; it
                              overwrites src/*.cpp, so it must not be re-run
                              once real code lands)
  build/                      configured build + build/install prefix
```

## Notes for step 2

- Stubs are plain function bodies inside one `extern "C" { }` block per module;
  replace them in place and keep the block.
- Each generated .cpp carries `#pragma GCC diagnostic ignored
  "-Wunused-parameter"` because stub bodies ignore their arguments. **Remove
  that pragma from a module once its functions actually use their parameters**,
  or real unused-parameter bugs will stay hidden.
- `ABTI_first_unimplemented()` / `ABTI_unimplemented_count()` are already there
  for a "what did Margo call that we do not have?" probe.
- `src/abti.h` has no internal structs yet; that is where `abt_pool`,
  `abt_thread`, the TLS current-xstream and the error macros go.
