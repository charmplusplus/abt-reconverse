#!/bin/sh
# Verify that libabt exports exactly the ABT_* surface include/abt.h declares:
# every function declaration and every `extern` data object, and nothing extra.
#
# Useful well past the skeleton: later steps replace stubs with real code, and
# this catches a function that got dropped or renamed on the way.
#
# usage: check_surface.sh <include/abt.h> <path to libabt.a>
set -eu

HDR="$1"
LIB="$2"
TMP=$(mktemp -d)
trap 'rm -rf "${TMP}"' EXIT

# Declarations: every statement in the API region ends with ABT_API_PUBLIC;
# take the identifier that precedes the argument list, or the last identifier
# on an `extern ... ABT_API_PUBLIC;` line.
awk '/^\/\* Init & Finalize \*\//{on=1}
     /^#if defined\(__cplusplus\)/{if(on)on=2}
     on==1 && $0 !~ /^[[:space:]]*\/\*/ && $0 !~ /^[[:space:]]*#/' "${HDR}" \
  | tr '\n' ' ' | tr ';' '\n' \
  | sed -e 's/ABT_API_PUBLIC//' -e 's/ABT_DEPRECATED//' \
  | grep -oE '\bABT_[A-Za-z0-9_]+[[:space:]]*\(' \
  | sed -e 's/[[:space:]]*($//' -e 's/(//' -e 's/[[:space:]]*$//' \
  | sort -u > "${TMP}/decl"

grep -E '^extern .*ABT_API_PUBLIC;' "${HDR}" \
  | sed -e 's/ABT_API_PUBLIC;//' \
  | awk '{print $NF}' | sort -u >> "${TMP}/decl"
sort -u -o "${TMP}/decl" "${TMP}/decl"

# Exported symbols (Mach-O underscore prefix stripped if present).
nm -g "${LIB}" 2>/dev/null \
  | grep -E ' [TDSB] _?ABT_' \
  | sed -e 's/.* [TDSB] //' -e 's/^_//' \
  | sort -u > "${TMP}/sym"

missing=$(comm -23 "${TMP}/decl" "${TMP}/sym")
extra=$(comm -13 "${TMP}/decl" "${TMP}/sym")
status=0

if [ -n "${missing}" ]; then
    echo "check_surface: declared in abt.h but NOT exported by ${LIB}:" >&2
    printf '%s\n' "${missing}" >&2
    status=1
fi
if [ -n "${extra}" ]; then
    echo "check_surface: exported by ${LIB} but not declared in abt.h:" >&2
    printf '%s\n' "${extra}" >&2
    status=1
fi

if [ "${status}" -eq 0 ]; then
    echo "check_surface: OK - $(wc -l < "${TMP}/decl" | tr -d ' ') declared ABT_* names, all exported, no extras"
fi
exit "${status}"
