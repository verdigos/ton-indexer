#!/usr/bin/env python3
"""codedoc scaffolder: skeleton JSON -> .md pages with facts filled and prose
slots empty (FILL markers). One page per module (basename), .h + .cpp merged.

Facts (signatures, decl sites, callers, calls) are machine-generated - LLM
lanes fill only the FILL slots and never edit fact lines.

Usage: gen_md.py --skeletons DIR --out DIR [--commit HASH]
"""
import argparse
import collections
import json
import os
import re
import subprocess

NOISE = re.compile(r"^(std::|__gnu|.*::__|.*::operator=$|.*basic_string)")


def module_of(path: str) -> str:
    base = os.path.basename(path)
    return re.sub(r"\.(h|hpp|cpp|cc)$", "", base)


def anchor(qname: str) -> str:
    short = qname.split("::")[-1]
    return re.sub(r"[^a-z0-9_]+", "-", short.lower())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skeletons", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--commit", default=None)
    args = ap.parse_args()

    commit = args.commit or subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        capture_output=True, text=True).stdout.strip()

    modules = collections.defaultdict(lambda: {"files": [], "classes": [], "functions": []})
    for name in sorted(os.listdir(args.skeletons)):
        if not name.endswith(".json"):
            continue
        d = json.load(open(os.path.join(args.skeletons, name)))
        m = modules[module_of(d["file"])]
        m["files"].append(d["file"])
        m["classes"] += d.get("classes", [])
        m["functions"] += d.get("functions", [])

    # map: source file -> module page, for cross-links
    file_to_module = {}
    for mod, m in modules.items():
        for f in m["files"]:
            file_to_module[f] = mod

    os.makedirs(args.out, exist_ok=True)
    for mod, m in sorted(modules.items()):
        # dedupe functions by qname+signature (h decl vs cpp def)
        seen = {}
        for fn in m["functions"]:
            key = (fn["qname"], fn["signature"])
            prev = seen.get(key)
            if prev is None or (not prev["callers"] and fn["callers"]):
                merged = dict(fn)
                if prev:
                    merged["callers"] = prev["callers"] or fn["callers"]
                    merged["calls"] = sorted(set(prev["calls"]) | set(fn["calls"]))
                seen[key] = merged
        fns = sorted(seen.values(), key=lambda x: x["decl"])

        lines = []
        lines.append("---")
        lines.append(f"module: {mod}")
        for f in sorted(set(m["files"])):
            lines.append(f"source: {f}")
        lines.append(f"source-commit: {commit}")
        lines.append("---")
        lines.append("")
        lines.append(f"# {mod}")
        lines.append("")
        lines.append("<!-- FILL:module-overview one paragraph: purpose, role in the "
                     "pipeline, key invariants, threading/ownership model -->")
        lines.append("")
        for cls in sorted({c["qname"]: c for c in m["classes"]}.values(),
                          key=lambda c: c["decl"]):
            lines.append(f"## class {cls['qname']}")
            lines.append("")
            lines.append(f"- decl: `{cls['decl']}`")
            lines.append("")
            lines.append("<!-- FILL:class-brief one line -->")
            lines.append("")
            lines.append("<!-- FILL:class-detail lifetime/ownership, invariants, "
                         "how it is used; omit if trivial -->")
            lines.append("")
        for fn in fns:
            callers = [c for c in fn["callers"] if not NOISE.match(c["qname"])]
            calls = [c for c in fn["calls"] if not NOISE.match(c)]
            lines.append(f"## {fn['qname'].split('::')[-1]}")
            lines.append("")
            lines.append(f"`{fn['signature']}`")
            lines.append("")
            lines.append(f"- decl: `{fn['decl']}`")
            if callers:
                parts = []
                for c in callers:
                    src = c["at"].rsplit(":", 1)[0]
                    tgt = file_to_module.get(src)
                    label = f"{c['qname']} ({c['at']})"
                    if tgt and tgt != mod:
                        parts.append(f"[{label}]({tgt}.md#{anchor(c['qname'])})")
                    else:
                        parts.append(label)
                lines.append(f"- callers: {'; '.join(parts)}")
            else:
                lines.append("- callers: none found in parsed TU set")
            if calls:
                lines.append(f"- calls: {', '.join('`'+c+'`' for c in calls)}")
            lines.append("")
            lines.append("<!-- FILL:brief one line -->")
            lines.append("")
            lines.append("<!-- FILL:detail purpose, behavior, error/exception "
                         "contract, gotchas; 2-6 sentences, omit noise; plain prose, "
                         "no backticks/code formatting -->")
            lines.append("")
            if fn.get("body_lines", 0) >= 40:
                lines.append("<!-- FILL:diagram mermaid flowchart of the control "
                             "flow (```mermaid fenced); only if it genuinely aids "
                             "understanding, else delete this comment -->")
                lines.append("")
        with open(os.path.join(args.out, mod + ".md"), "w") as fh:
            fh.write("\n".join(lines))
        print(f"{mod}.md: {len(fns)} fns, {len(m['classes'])} classes")


if __name__ == "__main__":
    main()
