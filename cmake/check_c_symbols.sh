#!/bin/sh
# Verify that the Argobots entry points in a freshly built libabt carry C
# linkage.  A definition that escapes its extern "C" block still compiles and
# still links inside C++, but leaves the C symbol undefined; Margo would be the
# one to discover that.  Check here instead.
#
# usage: check_c_symbols.sh <path to libabt.a>
set -eu

LIB="$1"
SYMS=$(nm -g "$LIB")

status=0

# Representative symbols, one per module, plus an exported data object.
for s in ABT_init ABT_thread_create ABT_eventual_set ABT_sched_config_automatic; do
    # Mach-O prefixes C symbols with an underscore; ELF does not.
    if printf '%s\n' "$SYMS" | grep -Eq "(^| )_?${s}\$"; then
        :
    else
        echo "check_c_symbols: MISSING C symbol ${s} in ${LIB}" >&2
        status=1
    fi
done

# Any mangled ABT_* symbol means a definition escaped its extern "C" block.
mangled=$(printf '%s\n' "$SYMS" | grep -E '__?Z[0-9]+ABT_' || true)
if [ -n "$mangled" ]; then
    echo "check_c_symbols: C++-mangled ABT_* symbols found in ${LIB}:" >&2
    printf '%s\n' "$mangled" >&2
    status=1
fi

if [ "$status" -eq 0 ]; then
    n=$(printf '%s\n' "$SYMS" | grep -cE '(^| )[TDSB] _?ABT_' || true)
    echo "check_c_symbols: OK - ${n} exported ABT_* C symbols, none mangled"
fi
exit "$status"
