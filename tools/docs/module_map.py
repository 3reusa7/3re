"""Module map for docs/ARCHITECTURE.md from a graphify code graph.

Run graphify on a copy of the code (it writes graphify-out/ next to it), then aggregate:
    graphify update <copy-with-apps-core-tools>
    python -I tools/docs/module_map.py <copy>/graphify-out/graph.json > module-map.md
A module is a folder such as apps/gui/studio or core/pixel3d/lowpoly. Edges between symbols of different
modules (calls, uses, inherits, ...) are counted; the strongest ones become the Mermaid diagram.
"""
import collections
import json
import re
import sys

LEVELS = {"apps": 2, "core": 3, "tools": 2}  # apps/gui, core/pixel3d/lowpoly, tools/trellis


def module_of(source_file):
    if not source_file:
        return None
    parts = source_file.replace("\\", "/").split("/")
    for i, p in enumerate(parts):
        if p in LEVELS:
            depth = LEVELS[p]
            mod = parts[i:i + depth]
            if len(mod) < depth or "." in mod[-1]:  # a file directly in the folder
                mod = [m for m in mod if "." not in m]
            if p == "apps" and len(parts) > i + 3 and parts[i + 1] == "gui" and "." not in parts[i + 2]:
                mod = parts[i:i + 3]  # apps/gui/studio, apps/gui/shell, ...
            return "/".join(mod)
    return None


def main():
    g = json.load(open(sys.argv[1], encoding="utf-8"))
    mod = {n["id"]: module_of(n.get("source_file")) for n in g["nodes"]}
    files = collections.defaultdict(set)
    for n in g["nodes"]:
        m = mod[n["id"]]
        if m:
            files[m].add(n.get("source_file"))
    symbols = collections.Counter(m for m in mod.values() if m)
    edges = collections.Counter()
    for e in g["links"]:
        a, b = mod.get(e["source"]), mod.get(e["target"])
        if a and b and a != b:
            edges[(a, b)] += 1
    print(f"Graph: {len(g['nodes'])} symbols, {len(g['links'])} relations; {len(symbols)} modules.\n")
    print("| Module | Files | Symbols | Depends most on |")
    print("|---|---|---|---|")
    for m, count in sorted(symbols.items()):
        out = sorted(((c, b) for (a, b), c in edges.items() if a == m), reverse=True)[:3]
        print(f"| `{m}` | {len(files[m])} | {count} | {', '.join(f'`{b}` ({c})' for c, b in out) or '—'} |")
    print("\n```mermaid\nflowchart LR")
    ids = {m: re.sub(r"[^A-Za-z0-9]", "_", m) for m in symbols}
    for (a, b), c in edges.most_common(40):
        print(f"  {ids[a]}[\"{a}\"] -->|{c}| {ids[b]}[\"{b}\"]")
    print("```")


main()
