#!/usr/bin/env python3
"""Generate the abt-reconverse SKELETON sources mechanically from include/abt.h.

This produced src/abti.h and src/{init,xstream,sched,pool,thread,sync,misc}.cpp
for phase 2 step 1.  It is kept for provenance and for regenerating the stub
surface if abt.h is ever re-substituted.

DESTRUCTIVE: it overwrites those files wholesale, so it would erase any real
implementation written after the skeleton.  Requires --force.
"""
import re, os, json, sys, collections

if "--force" not in sys.argv:
    sys.exit("refusing to overwrite src/*.cpp; pass --force if you really mean it")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(ROOT, "include", "abt.h")
SRC = os.path.join(ROOT, "src")

# ---------------------------------------------------------------- parse header
lines = open(HDR).read().split("\n")
start = next(i for i, l in enumerate(lines) if l.strip() == "/* Init & Finalize */")
end = next(i for i, l in enumerate(lines)
           if i > start and l.startswith("#if defined(__cplusplus)"))
region = lines[start:end]

kept = []
for l in region:
    s = l.strip()
    if (s.startswith("/*") and s.endswith("*/")) or s.startswith("#"):
        continue
    kept.append(l)

blob = re.sub(r"\s+", " ", " ".join(kept))
funcs = []
for d in [x.strip() for x in blob.split(";") if x.strip()]:
    assert "ABT_API_PUBLIC" in d, d
    d2 = d.replace("ABT_API_PUBLIC", "").replace("ABT_DEPRECATED", "").strip()
    m = re.match(r"^(.*?\b)(ABT_[A-Za-z0-9_]+)\s*\((.*)\)$", d2)
    assert m, d2
    funcs.append({"ret": m.group(1).strip(), "name": m.group(2),
                  "args": m.group(3).strip()})

# error codes, mechanically from the ABT_ERR_*/ABT_SUCCESS defines
errs = []
for l in lines:
    m = re.match(r"^#define\s+(ABT_SUCCESS|ABT_ERR_[A-Z0-9_]+)\s+(\d+)\s*$", l)
    if m:
        errs.append((m.group(1), int(m.group(2))))
errs.sort(key=lambda e: e[1])

# extern data symbols
externs = [l for l in lines if l.startswith("extern ") and "ABT_API_PUBLIC" in l]

# ---------------------------------------------------------------- module split
def module_of(name):
    p = "_".join(name.split("_")[:2])
    return {
        "ABT_init": "init", "ABT_finalize": "init", "ABT_initialized": "init",
        "ABT_xstream": "xstream",
        "ABT_sched": "sched",
        "ABT_pool": "pool", "ABT_unit": "pool",
        "ABT_thread": "thread", "ABT_task": "thread", "ABT_self": "thread",
        "ABT_key": "thread",
        "ABT_mutex": "sync", "ABT_cond": "sync", "ABT_rwlock": "sync",
        "ABT_eventual": "sync", "ABT_future": "sync", "ABT_barrier": "sync",
        "ABT_info": "misc", "ABT_tool": "misc", "ABT_error": "misc",
        "ABT_timer": "misc", "ABT_get": "misc",
    }[p]

mods = collections.OrderedDict(
    (m, []) for m in ["init", "xstream", "sched", "pool", "thread", "sync", "misc"])
for f in funcs:
    mods[module_of(f["name"])].append(f)

# functions that are NOT plain stubs in the skeleton
REAL = {"ABT_initialized", "ABT_error_get_str"}

# non-int return defaults
DEFAULT_RET = {"double": "0.0"}

HEAD = """\
/*
 * abt-reconverse: an Argobots (ABT) C ABI implemented on top of Reconverse.
 *
 * GENERATED SKELETON (phase 2, step 1).  Every public Argobots entry point is
 * present as a C symbol; the bodies are stubs that record the call and return
 * ABT_ERR_FEATURE_NA.  Real semantics land in later steps; replace stubs in
 * place, keeping the extern "C" block.
 */
"""

PRAGMA = """\
/* Stub bodies deliberately ignore their arguments. */
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
"""

def emit_stub(f):
    out = []
    if f["name"] in REAL:
        return None
    body = ('    ABTI_UNIMPLEMENTED("%s");' % f["name"]) if f["ret"] == "int" else (
        '    ABTI_note_unimplemented("%s");\n    return %s;'
        % (f["name"], DEFAULT_RET[f["ret"]]))
    out.append("%s %s(%s)\n{\n%s\n}" % (f["ret"], f["name"], f["args"], body))
    return "\n".join(out)

# ------------------------------------------------------------------- src/abti.h
abti_h = HEAD + """
#ifndef ABTI_H_INCLUDED
#define ABTI_H_INCLUDED

/* C++17.  Every .cpp in this library includes this header first. */
#if !defined(__cplusplus) || __cplusplus < 201703L
#error "abt-reconverse must be compiled as C++17"
#endif

#include "abt.h"       /* the public C ABI; already wrapped in extern "C" */
#include "converse.h"  /* the Reconverse runtime this shim is built on */

#include <cstddef>
#include <cstdint>

/*
 * extern "C" guidance
 * -------------------
 * abt.h wraps its declarations in extern "C" when compiled as C++, so a
 * definition that follows the declaration already gets C linkage.  Define
 * every public entry point inside an explicit
 *
 *     extern "C" { ... }
 *
 * block anyway: a typo in a signature then fails to link against the
 * declaration instead of quietly defining a second, name-mangled overload.
 * CMake's post-build check (nm -g on the static library) enforces this: no
 * ABT_* symbol may appear with a __Z prefix.
 *
 * Internal helpers stay OUT of those blocks and keep C++ linkage; give them an
 * ABTI_ prefix.  The real internal structs (abt_pool, abt_thread, ...) are
 * added here in step 2; this header is deliberately small for now.
 */

/*
 * Records the name of an unimplemented entry point.  The FIRST such call wins a
 * process-wide slot (thread-safe, lock-free); every later call only bumps the
 * counter.  `name` must be a string literal or otherwise immortal storage.
 * Always returns ABT_ERR_FEATURE_NA so callers can `return` it directly.
 */
int ABTI_note_unimplemented(const char *name) noexcept;

/* Name of the first unimplemented entry point called, or nullptr. */
const char *ABTI_first_unimplemented(void) noexcept;

/* Total number of unimplemented entry-point calls so far. */
unsigned long ABTI_unimplemented_count(void) noexcept;

/* Stub body for an int-returning Argobots function. */
#define ABTI_UNIMPLEMENTED(name) return ABTI_note_unimplemented(name)

#endif /* ABTI_H_INCLUDED */
"""
open(os.path.join(SRC, "abti.h"), "w").write(abti_h)

# ------------------------------------------------------------------ src/init.cpp
init_extra = '''
#include <atomic>

namespace {
std::atomic<const char *> g_first_unimplemented{nullptr};
std::atomic<unsigned long> g_unimplemented_calls{0};
} /* namespace */

int ABTI_note_unimplemented(const char *name) noexcept
{
    g_unimplemented_calls.fetch_add(1, std::memory_order_relaxed);
    const char *expected = nullptr;
    /* Only the first caller installs a name; the rest see a non-null slot. */
    g_first_unimplemented.compare_exchange_strong(expected, name,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed);
    return ABT_ERR_FEATURE_NA;
}

const char *ABTI_first_unimplemented(void) noexcept
{
    return g_first_unimplemented.load(std::memory_order_acquire);
}

unsigned long ABTI_unimplemented_count(void) noexcept
{
    return g_unimplemented_calls.load(std::memory_order_relaxed);
}

extern "C" {

/*
 * Not a stub: Margo and Thallium call this before ABT_init to decide whether
 * they own the Argobots lifetime.  Until ABT_init is real (step 2), the answer
 * is always "not initialized".
 */
int ABT_initialized(void)
{
    return ABT_ERR_UNINITIALIZED;
}

} /* extern "C" */
'''

# ------------------------------------------------------------------ src/misc.cpp
err_cases = "\n".join(
    '    case %s: return "%s";' % (n, n) for n, v in errs)
misc_extra = '''
#include <cstring>

namespace {
/* Generated from the ABT_SUCCESS / ABT_ERR_* defines in abt.h. */
const char *abti_error_name(int err) noexcept
{
    switch (err) {
%s
    default: return nullptr;
    }
}
} /* namespace */

extern "C" {

/*
 * Not a stub: Margo prints these names when a call fails, so the shim must
 * answer correctly from day one.  ABT_ENABLE_VER_20_API is 0 in this header, so
 * an unknown code is ABT_ERR_OTHER (the 2.0 API would say ABT_ERR_INV_ARG).
 */
int ABT_error_get_str(int err, char *str, size_t *len)
{
    const char *name = abti_error_name(err);
    if (!name)
        return ABT_ERR_OTHER;
    if (str)
        strcpy(str, name);
    if (len)
        *len = strlen(name);
    return ABT_SUCCESS;
}

} /* extern "C" */
''' % err_cases

# ----------------------------------------------------------------- data symbols
sched_data = '''
extern "C" {

/*
 * Predefined scheduler configuration variables.  Values must match Argobots
 * (mochi/argobots/src/sched/sched_config.c): the negative indices are the
 * runtime's reserved keys and Margo passes these objects by value.
 */
ABT_sched_config_var ABT_sched_config_var_end = { -1, ABT_SCHED_CONFIG_INT };
ABT_sched_config_var ABT_sched_config_access = { -2, ABT_SCHED_CONFIG_INT };
ABT_sched_config_var ABT_sched_config_automatic = { -3, ABT_SCHED_CONFIG_INT };
ABT_sched_config_var ABT_sched_basic_freq = { -4, ABT_SCHED_CONFIG_INT };

} /* extern "C" */
'''

pool_data = '''
extern "C" {

/*
 * Predefined pool configuration variable; value from Argobots
 * (mochi/argobots/src/pool/pool_config.c).  Note it is const.
 */
const ABT_pool_config_var ABT_pool_config_automatic = { -2,
                                                       ABT_POOL_CONFIG_INT };

} /* extern "C" */
'''

TITLES = {
    "init": "ABT_init / ABT_finalize / ABT_initialized, plus the "
            "unimplemented-call bookkeeping shared by every module.",
    "xstream": "Execution streams (leased Reconverse PEs) and the ES barrier.",
    "sched": "Schedulers, scheduler configs, and the predefined config vars.",
    "pool": "Pools (built-in and user-defined) and work units.",
    "thread": "ULTs, tasklets, thread attributes, ULT-specific keys, "
              "and the ABT_self_* family.",
    "sync": "Mutexes, condition variables, rwlocks, eventuals, futures, "
            "barriers.",
    "misc": "Error strings, timers, info queries, and the tool interface.",
}

EXTRA = {"init": init_extra, "misc": misc_extra, "sched": sched_data,
         "pool": pool_data}

counts = {}
for mod, fl in mods.items():
    body = []
    body.append(HEAD)
    body.append('#include "abti.h"\n')
    body.append("/* %s */\n" % TITLES[mod])
    body.append(PRAGMA)
    extra = EXTRA.get(mod)
    if extra:
        body.append(extra)
    stubs = [s for s in (emit_stub(f) for f in fl) if s]
    body.append("extern \"C\" {\n")
    body.append("\n\n".join(stubs))
    body.append("\n\n} /* extern \"C\" */\n")
    open(os.path.join(SRC, mod + ".cpp"), "w").write("\n".join(body))
    counts[mod] = (len(fl), len(stubs))

print("functions parsed:", len(funcs))
print("error codes:", len(errs), "max", max(v for _, v in errs))
print("extern data decls:", len(externs))
for m, (n, s) in counts.items():
    print("  %-8s decls=%3d stubs=%3d" % (m, n, s))
print("total stubs:", sum(s for _, s in counts.values()),
      "+ real:", len(REAL))

