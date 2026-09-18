#!/usr/bin/env bash
#
# Keeps lib/API.md honest. Fails when:
#   - API.md names a function (a backticked `name(`) that no lib header declares, or
#   - a lib header declares a public function that API.md never mentions.
# Run via `make check-docs`.

set -euo pipefail
cd "$(dirname "$0")/.."

DOC=lib/API.md
HEADERS=(lib/*.h lib/json/json.h)
status=0

# Function names declared in headers: "type name(" at column 0 (skips typedefs, macros, comments).
declared=$(grep -hE '^[A-Za-z_][A-Za-z0-9_ \*]*[ \*]([a-z][a-z0-9_]*)\(' "${HEADERS[@]}" \
    | grep -v '^typedef\|^#' \
    | sed -E 's/^[^(]*[ \*]([a-z][a-z0-9_]*)\(.*/\1/' | sort -u)

# Names mentioned in the doc: backticked identifiers followed by '(' or a run like `router_get` … also plain `name`.
mentioned=$(grep -oE '`[a-z][a-z0-9_]*' "$DOC" | tr -d '`' | sort -u)

for fn in $declared; do
    if ! grep -qx "$fn" <<<"$mentioned"; then
        echo "check-docs: $fn is declared in a header but not mentioned in $DOC"
        status=1
    fi
done

for fn in $(grep -oE '`[a-z][a-z0-9_]*\(' "$DOC" | tr -d '`(' | sort -u); do
    if ! grep -qx "$fn" <<<"$declared"; then
        echo "check-docs: $DOC mentions $fn( but no header declares it"
        status=1
    fi
done

[ $status -eq 0 ] && echo "check-docs: ok ($(wc -w <<<"$declared") functions covered)"
exit $status
