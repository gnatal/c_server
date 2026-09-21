#!/usr/bin/env bash
#
# Keeps lib/API.md honest. Fails when:
#   - a lib/*.h engine header declares a public function that API.md never mentions, or
#   - API.md names a function (a backticked `name(`) that neither an engine header nor the
#     vendored yyjson header declares.
# yyjson's own API (hundreds of functions) is NOT required to be listed: API.md documents the subset
# the engine and demo use, and only that subset is checked for typos.
# Run via `make check-docs`.

set -euo pipefail
cd "$(dirname "$0")/.."

DOC=lib/API.md
ENGINE_HEADERS=(lib/*.h)
ALL_HEADERS=(lib/*.h lib/vendor/yyjson/yyjson.h)
status=0

# Function names declared in headers: "type name(" at column 0 (skips typedefs, macros, comments).
declared_in() {
    grep -hE '^[A-Za-z_][A-Za-z0-9_ \*]*[ \*]([a-z][a-z0-9_]*)\(' "$@" \
        | grep -v '^typedef\|^#' \
        | sed -E 's/^[^(]*[ \*]([a-z][a-z0-9_]*)\(.*/\1/' | sort -u
}
engine_declared=$(declared_in "${ENGINE_HEADERS[@]}")
all_declared=$(declared_in "${ALL_HEADERS[@]}")

# Names mentioned in the doc: backticked identifiers followed by '(' or a run like `router_get` … also plain `name`.
mentioned=$(grep -oE '`[a-z][a-z0-9_]*' "$DOC" | tr -d '`' | sort -u)

for fn in $engine_declared; do
    if ! grep -qx "$fn" <<<"$mentioned"; then
        echo "check-docs: $fn is declared in a header but not mentioned in $DOC"
        status=1
    fi
done

for fn in $(grep -oE '`[a-z][a-z0-9_]*\(' "$DOC" | tr -d '`(' | sort -u); do
    if ! grep -qx "$fn" <<<"$all_declared"; then
        echo "check-docs: $DOC mentions $fn( but no header declares it"
        status=1
    fi
done

[ $status -eq 0 ] && echo "check-docs: ok ($(wc -w <<<"$engine_declared") engine functions covered)"
exit $status
