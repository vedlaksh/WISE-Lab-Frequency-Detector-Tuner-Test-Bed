#!/usr/bin/env python3
"""
monitor_to_dot.py -- convert Wizard's `calls` monitor output into a Graphviz .dot call graph.

Usage:
    wizeng --monitors=calls --colors=false <mod.wasm> | monitor_to_dot.py > out.dot
    monitor_to_dot.py <calls_output.txt> > out.dot
    dot -Tsvg out.dot -o out.svg

The `calls` monitor prints, for each executed function, its entry count and its call sites:

    func #N: C                      caller function N, entered C times
      +OFF: call [T]: K             direct call at bytecode offset OFF to func T, K times
      +OFF: return_call [T]: K      tail call to func T
      +OFF: call_indirect:          indirect call site; targets follow (indented)
        <wasm func #X>: K
      +OFF: call_ref:               typed function-reference call; targets follow
        <wasm func #X>: K
      +OFF: return_call_indirect:   tail indirect
      +OFF: return_call_ref:        tail ref

Edges are aggregated per (caller -> callee); the edge label is the total call count.
Dynamic-dispatch edges (call_indirect / call_ref and their tail forms) are drawn dashed;
tail calls use an open arrowhead. Callees that were never entered themselves are drawn
as dashed gray nodes.
"""
import sys
import re
from collections import defaultdict

FUNC_RE   = re.compile(r'^func #(\d+):\s*(\d+)\s*$')
DIRECT_RE = re.compile(r'^\s+\+(\d+):\s*(call|return_call)\s*\[(\d+)\]:\s*(\d+)\s*$')
GROUP_RE  = re.compile(r'^\s+\+(\d+):\s*(call_indirect|call_ref|return_call_indirect|return_call_ref):\s*$')
TARGET_RE = re.compile(r'^\s+<wasm func #(\d+)>:\s*(\d+)\s*$')


def main():
    text = sys.stdin.read() if len(sys.argv) < 2 else open(sys.argv[1]).read()

    entry = {}                                # func index -> entry count
    edges = defaultdict(lambda: {'count': 0, 'dynamic': False, 'tail': False})
    caller = None
    group_kind = None                         # kind of the current indirect/ref group

    for line in text.splitlines():
        m = FUNC_RE.match(line)
        if m:
            caller = int(m.group(1))
            entry[caller] = int(m.group(2))
            group_kind = None
            continue

        m = DIRECT_RE.match(line)
        if m and caller is not None:
            kind, callee, k = m.group(2), int(m.group(3)), int(m.group(4))
            e = edges[(caller, callee)]
            e['count'] += k
            if kind == 'return_call':
                e['tail'] = True
            group_kind = None
            continue

        m = GROUP_RE.match(line)
        if m and caller is not None:
            group_kind = m.group(2)
            continue

        m = TARGET_RE.match(line)
        if m and caller is not None and group_kind is not None:
            callee, k = int(m.group(1)), int(m.group(2))
            e = edges[(caller, callee)]
            e['count'] += k
            e['dynamic'] = True
            if group_kind.startswith('return_'):
                e['tail'] = True
            continue

    nodes = set(entry) | {c for (c, _) in edges} | {t for (_, t) in edges}

    out = ['digraph calls {',
           '  rankdir=LR;',
           '  node [shape=box, style=rounded, fontname="monospace"];',
           '  edge [fontname="monospace", fontsize=10];']
    for n in sorted(nodes):
        if n in entry:
            out.append(f'  f{n} [label="func #{n}\\n{entry[n]}x entered"];')
        else:
            out.append(f'  f{n} [label="func #{n}\\n(not entered)", style="rounded,dashed", color=gray, fontcolor=gray];')
    for (c, t), e in sorted(edges.items()):
        attrs = [f'label="{e["count"]}"']
        if e['dynamic']:
            attrs.append('style=dashed')
        if e['tail']:
            attrs.append('arrowhead=onormal')
        out.append(f'  f{c} -> f{t} [{", ".join(attrs)}];')
    out.append('}')
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == '__main__':
    main()
