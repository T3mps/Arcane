"""List everything the Arcane knowledge graph connects to a symbol or concept.

Usage (repo root, any Python 3.9+, stdlib only):
    python .claude/skills/arcane-graph/neighbors.py ViewTransform [--exact] [--docs-only]

A symbol usually has several nodes: the code node from the AST pass plus concept nodes that
docs, specs and plans mint for it. This script unions every node whose label matches, then
prints their direct neighbours grouped into code files and docs, so reverse dependents and the
governing docs come out in one list. Matching is a case-insensitive substring by default;
--exact requires the label (minus trailing "()") to equal the name.
"""
import json
import sys
from collections import defaultdict
from pathlib import Path


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    if not args:
        print(__doc__)
        return 2
    name = args[0].lower()
    graph_path = Path("graphify-out/graph.json")
    if not graph_path.exists():
        print("graphify-out/graph.json not found: run from the repo root, or build it with /graphify .")
        return 1
    g = json.loads(graph_path.read_text(encoding="utf-8"))
    nodes = {n["id"]: n for n in g["nodes"]}
    links = g.get("links", g.get("edges", []))

    def matches(n):
        label = (n.get("label") or "").lower().removesuffix("()")
        return label == name if "--exact" in flags else name in label

    seeds = {nid for nid, n in nodes.items() if matches(n)}
    if not seeds:
        print(f"no node label matches {args[0]!r}")
        return 1

    by_file = defaultdict(set)
    for e in links:
        s, t = e["source"], e["target"]
        for a, b in ((s, t), (t, s)):
            if a in seeds and b not in seeds and b in nodes:
                nb = nodes[b]
                src = nb.get("source_file") or "(external: std/ThirdParty)"
                by_file[src].add(f'{nb.get("label")}  [{e.get("relation")}, {e.get("confidence")}]')

    docs = {f: v for f, v in by_file.items() if f.replace("\\", "/").startswith("docs/") or f.endswith(".md")}
    code = {f: v for f, v in by_file.items() if f not in docs}
    print(f"{len(seeds)} matching node(s); {len(code)} code file(s), {len(docs)} doc file(s)\n")
    sections = [("DOCS", docs)] if "--docs-only" in flags else [("CODE", code), ("DOCS", docs)]
    for title, group in sections:
        print(f"== {title}")
        for f in sorted(group):
            print(f"  {f}")
            for item in sorted(group[f])[:6]:
                print(f"      {item}")
            if len(group[f]) > 6:
                print(f"      ... {len(group[f]) - 6} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
