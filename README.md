# abt-reconverse: the Argobots 1.2 API on Reconverse

`libabt` + `abt.h` + `argobots.pc` implementing the Argobots 1.2 C ABI on top
of Reconverse (the Charm++ runtime substrate). Mochi (Margo, Thallium, Yokan,
Bedrock) and DAOS build against it from unmodified sources.

Status 2026-09-14: Argobots' own `test/basic` 57 pass / 4 skip / 0 fail;
Margo 28/28, Thallium 24/24, Yokan 25/26 (Mac, 16 PEs; Linux/Anvil at parity);
a mock of DAOS's engine scheduling passes. See `tests/argobots-basic/RESULTS.md`
for the skips (2.0-style user pools, ULT revive/exit_to).

## Build

Requirements: CMake >= 3.20, a C++17 compiler (Apple clang or gcc 11),
hwloc, and a Reconverse install built from branch `abt-pools`
(https://github.com/charmplusplus/reconverse; the hooks this library needs
are listed in that branch's RECONVERSE-CHANGES.txt until they are merged).

    # Reconverse (static archive, position independent so libabt.so can link it)
    git clone -b abt-pools https://github.com/charmplusplus/reconverse
    cmake -S reconverse -B reconverse/build -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DRECONVERSE_AUTOFETCH_LCI2=OFF \
          -DCMAKE_INSTALL_PREFIX=$PWD/reconverse/install
    cmake --build reconverse/build -j8 && cmake --install reconverse/build

    # this library
    cmake -S abt-reconverse -B abt-reconverse/build -DCMAKE_BUILD_TYPE=Release \
          -DRECONVERSE_DIR=$PWD/reconverse/install \
          -DCMAKE_INSTALL_PREFIX=$PWD/abt-reconverse/install
    cmake --build abt-reconverse/build -j8 && cmake --install abt-reconverse/build
    (cd abt-reconverse/build && ctest)          # own tests; add -R argobots for the upstream suite

The upstream Argobots tests are built when an Argobots checkout is at
`../mochi/argobots` (or `-DABT_UPSTREAM_TEST_DIR=<argobots>/test`).
On Linux with `-DRECONVERSE_ENABLE_CPU_AFFINITY=ON` in Reconverse, hwloc is
found through the `hwloc` module or `HWLOC_ROOT`.

## Use with Mochi

Put the install's `lib/pkgconfig` (or `lib64/pkgconfig`) first on
`PKG_CONFIG_PATH` and build Margo/Thallium/Yokan as usual; `pkg-config
--modversion argobots` reports 1.2.0. Nothing in Mochi changes. Pin Mercury
to v2.4.1 for Yokan (Mercury main broke NULL-string encoding).

## Runtime knobs

| variable | meaning |
|---|---|
| `ABT_MAX_NUM_XSTREAMS` | as in Argobots; the runtime starts that many + 1 PEs (floor 16) |
| `ABT_RECONVERSE_NUM_PES` | explicit PE count (default min(max(cores,16),32); cap 256) |
| `ABT_THREAD_STACKSIZE` | default ULT stack (2 MiB; Argobots' is 16 KiB) |
| `ABT_RECONVERSE_DEBUG=1` | event trace on stderr |
| `ABT_RECONVERSE_TRACE_NA=1` | print every unimplemented entry point that is hit |
| `CMI_INIT_TRACE=1` | Reconverse start-up/shutdown phase timestamps per PE |

## Design in one paragraph

An execution stream is a leased Reconverse PE (pthreads fixed at init); a
pool is a FIFO the PE's scheduler table polls (user pools through
`ABT_pool_def`); a ULT is a Reconverse thread whose wake pushes its token into
its own pool from any PE; a tasklet is a stackless handler; predefined
schedulers are table weights, user-defined and stacked schedulers run on a
runner ULT and ULTs they resume return to them through the thread's choose
function; mutex/cond/eventual/rwlock/barrier/future are spinlock + wait
list with the unlock as a post-switch action and Argobots' 64-byte static
layouts. Divergences: PE count fixed at init, no runtime ULT migration
between PEs, no revive/cancel/tool interface/stack unwinding.

## License and provenance

`include/abt.h` is derived from Argobots' `abt.h.in` (Copyright UChicago
Argonne, LLC; BSD-style license in `COPYRIGHT.argobots`, which requires
modifications to be noted: see the header comment). The upstream Argobots
`test/basic` programs are compiled from an Argobots checkout, never copied.
The implementation in `src/` and the tests in `tests/` are new code by the
Parallel Programming Laboratory, UIUC; their license follows Reconverse's
(to be stated here once set by PPL).
