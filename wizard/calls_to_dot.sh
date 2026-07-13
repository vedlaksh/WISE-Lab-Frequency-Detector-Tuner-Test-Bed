#!/usr/bin/env bash
# calls_to_dot.sh -- run Wizard's `calls` monitor on a wasm module and emit a Graphviz .dot
# (and, if `dot` is installed, an .svg) call graph.
#
# Usage:
#   calls_to_dot.sh <module.wasm> [out_basename] [-- <extra wizeng flags>]
#
# Examples:
#   calls_to_dot.sh foo.wasm
#   calls_to_dot.sh call_monitor1.wasm cm1 -- --ext:function-references
#
# Env overrides:
#   WIZENG   path to the wizeng binary   (default: <this dir>/wizard-engine/bin/wizeng.jvm)
#   MONITOR  monitor name                (default: calls)
#   CONV     path to monitor_to_dot.py   (default: <this dir>/monitor_to_dot.py)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WIZENG="${WIZENG:-$HERE/wizard-engine/bin/wizeng.jvm}"
CONV="${CONV:-$HERE/monitor_to_dot.py}"
MONITOR="${MONITOR:-calls}"

# The jvm wizeng needs `java`; add Homebrew openjdk if present (macOS).
[ -d /opt/homebrew/opt/openjdk/bin ] && PATH="/opt/homebrew/opt/openjdk/bin:$PATH"

if [ "$#" -lt 1 ]; then
    echo "usage: calls_to_dot.sh <module.wasm> [out_basename] [-- <extra wizeng flags>]" >&2
    exit 1
fi

MODULE="$1"; shift
OUT="${1:-$(basename "${MODULE%.wasm}")}"
case "${1:-}" in ""|--) : ;; *) shift ;; esac
[ "${1:-}" = "--" ] && shift

"$WIZENG" "$@" --monitors="$MONITOR" --colors=false "$MODULE" \
    | python3 "$CONV" > "$OUT.$MONITOR.dot"
echo "wrote $OUT.$MONITOR.dot"

if command -v dot >/dev/null 2>&1; then
    dot -Tsvg "$OUT.$MONITOR.dot" -o "$OUT.$MONITOR.svg" && echo "wrote $OUT.$MONITOR.svg"
else
    echo "(graphviz 'dot' not found; skipping render)" >&2
fi
