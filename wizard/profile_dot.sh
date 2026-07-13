#!/usr/bin/env bash
# profile_dot.sh -- run Wizard's `profile` monitor in its native .dot mode (a calling-context
# tree weighted by self-time %), writing a Graphviz .dot (and .svg if `dot` is installed).
#
# The `profile` monitor emits Graphviz directly via its `dot` flag -- no converter needed.
#
# Usage:
#   profile_dot.sh <module.wasm> [out_basename] [-- <extra wizeng flags>]
#
# Examples:
#   profile_dot.sh profile_monitor0.wasm
#   profile_dot.sh call_monitor1.wasm cm1 -- --ext:function-references
#
# Env overrides:
#   WIZENG        path to wizeng           (default: <this dir>/wizard-engine/bin/wizeng.jvm)
#   PROFILE_OPTS  options inside profile{} (default: dot; e.g. PROFILE_OPTS='dot,depth=4')
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WIZENG="${WIZENG:-$HERE/wizard-engine/bin/wizeng.jvm}"
OPTS="${PROFILE_OPTS:-dot}"

# The jvm wizeng needs `java`; add Homebrew openjdk if present (macOS).
[ -d /opt/homebrew/opt/openjdk/bin ] && PATH="/opt/homebrew/opt/openjdk/bin:$PATH"

if [ "$#" -lt 1 ]; then
    echo "usage: profile_dot.sh <module.wasm> [out_basename] [-- <extra wizeng flags>]" >&2
    exit 1
fi

MODULE="$1"; shift
OUT="${1:-$(basename "${MODULE%.wasm}")}"
case "${1:-}" in ""|--) : ;; *) shift ;; esac
[ "${1:-}" = "--" ] && shift

"$WIZENG" "$@" --monitors="profile{$OPTS}" --colors=false "$MODULE" > "$OUT.profile.dot"
echo "wrote $OUT.profile.dot"

if command -v dot >/dev/null 2>&1; then
    dot -Tsvg "$OUT.profile.dot" -o "$OUT.profile.svg" && echo "wrote $OUT.profile.svg"
else
    echo "(graphviz 'dot' not found; skipping render)" >&2
fi
