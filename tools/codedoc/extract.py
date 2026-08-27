#!/usr/bin/env python3
"""codedoc skeleton extractor.

Parses TUs via libclang and emits a per-target-file skeleton JSON:
symbol inventory (classes, methods, free functions) + mechanical call edges
(callers/callees with file:line). LLM lanes fill prose into these skeletons;
they never invent facts.

Usage:
  extract.py --compile-commands build/compile_commands.json \
             --tu-filter ton-mch-engine ton-mch-index \
             --targets src/ClassifyCore.cpp src/BatchRunner.cpp \
             --out skeletons/
Target matching is by suffix; a .cpp target also claims its .h twin.
"""
import argparse
import collections
import json
import os
import sys

import subprocess

import clang.cindex as ci

REPO_MARK = "/ton-index-worker/"

# pip libclang ships without its builtin headers; borrow the system clang's.
RESOURCE_DIR = subprocess.run(["clang", "-print-resource-dir"],
                              capture_output=True, text=True).stdout.strip()


def rel(path: str) -> str:
    i = path.find(REPO_MARK)
    return path[i + 1:] if i >= 0 else path


def parse_args_from_entry(entry):
    if "arguments" in entry:
        args = list(entry["arguments"])[1:]
    else:
        args = entry["command"].split()[1:]
    out = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-c", "-o"):
            skip = True
            continue
        if a.endswith((".cpp", ".cc", ".c")):
            continue
        out.append(a)
    if RESOURCE_DIR:
        out += ["-resource-dir", RESOURCE_DIR]
    return out


def qname(cursor) -> str:
    parts = [cursor.spelling] if cursor.spelling else []
    c = cursor.semantic_parent
    while c is not None and c.kind != ci.CursorKind.TRANSLATION_UNIT:
        if c.kind in (ci.CursorKind.NAMESPACE, ci.CursorKind.CLASS_DECL,
                      ci.CursorKind.STRUCT_DECL, ci.CursorKind.CLASS_TEMPLATE):
            if c.spelling:
                parts.append(c.spelling)
        c = c.semantic_parent
    return "::".join(reversed(parts))


def signature(cursor) -> str:
    try:
        ret = cursor.result_type.spelling
        params = ", ".join(a.type.spelling + (" " + a.spelling if a.spelling else "")
                           for a in cursor.get_arguments())
        const = " const" if cursor.is_const_method() else ""
        return f"{ret} {cursor.spelling}({params}){const}"
    except Exception:
        return cursor.displayname


FUNC_KINDS = {
    ci.CursorKind.FUNCTION_DECL,
    ci.CursorKind.CXX_METHOD,
    ci.CursorKind.CONSTRUCTOR,
    ci.CursorKind.DESTRUCTOR,
    ci.CursorKind.FUNCTION_TEMPLATE,
}
CLASS_KINDS = {
    ci.CursorKind.CLASS_DECL,
    ci.CursorKind.STRUCT_DECL,
    ci.CursorKind.CLASS_TEMPLATE,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compile-commands", required=True)
    ap.add_argument("--tu-filter", nargs="+", required=True,
                    help="substrings; a TU is parsed if its path contains any")
    ap.add_argument("--targets", nargs="+", required=True,
                    help="path suffixes of files to document")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    cc = json.load(open(args.compile_commands))
    tus = [e for e in cc
           if "/external/" not in e["file"]
           and any(f in e["file"] for f in args.tu_filter)]
    print(f"parsing {len(tus)} TUs", file=sys.stderr)

    def is_target(path: str) -> bool:
        r = rel(path)
        for t in args.targets:
            base = t[:-4] if t.endswith(".cpp") else t
            if r.endswith(t) or (t.endswith(".cpp") and
                                 (r.endswith(base + ".h") or r.endswith(base + ".hpp"))):
                return True
        return False

    index = ci.Index.create()
    # symbols[usr] = {...}; edges: calls[(caller_usr, callee_usr)] = [locs]
    symbols = {}
    classes = {}
    calls = collections.defaultdict(set)
    caller_info = {}

    def note_symbol(cur):
        usr = cur.get_usr()
        if not usr or usr in symbols:
            return usr
        loc = cur.location
        symbols[usr] = {
            "qname": qname(cur),
            "kind": str(cur.kind).split(".")[-1],
            "signature": signature(cur) if cur.kind in FUNC_KINDS else cur.spelling,
            "file": rel(loc.file.name) if loc.file else "?",
            "line": loc.line,
            "is_target": bool(loc.file and is_target(loc.file.name)),
        }
        return usr

    def walk(cur, current_fn_usr):
        if cur.kind in FUNC_KINDS and cur.is_definition():
            u = note_symbol(cur)
            loc = cur.location
            if u and loc.file:
                caller_info[u] = {"file": rel(loc.file.name), "line": loc.line}
                ext = cur.extent
                symbols[u]["body_lines"] = max(
                    symbols[u].get("body_lines", 0),
                    (ext.end.line - ext.start.line) if ext else 0)
            current_fn_usr = u
        elif cur.kind in CLASS_KINDS and cur.is_definition():
            loc = cur.location
            if loc.file and is_target(loc.file.name):
                classes.setdefault(cur.get_usr(), {
                    "qname": qname(cur),
                    "file": rel(loc.file.name),
                    "line": loc.line,
                })
        elif cur.kind == ci.CursorKind.CALL_EXPR and current_fn_usr:
            ref = cur.referenced
            if ref is not None:
                ru = note_symbol(ref)
                if ru:
                    loc = cur.location
                    calls[(current_fn_usr, ru)].add(
                        (rel(loc.file.name) if loc.file else "?", loc.line))
        for ch in cur.get_children():
            walk(ch, current_fn_usr)

    parsed = 0
    for e in tus:
        try:
            tu = index.parse(e["file"], args=parse_args_from_entry(e),
                             options=ci.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES * 0)
            walk(tu.cursor, None)
            parsed += 1
        except Exception as ex:
            print(f"WARN parse failed {e['file']}: {ex}", file=sys.stderr)
    print(f"parsed {parsed}/{len(tus)}; {len(symbols)} symbols, {len(calls)} edges",
          file=sys.stderr)

    # assemble per-target-file output
    out_files = collections.defaultdict(lambda: {"classes": [], "functions": []})
    callers_of = collections.defaultdict(list)
    callees_of = collections.defaultdict(list)
    def is_ours(sym) -> bool:
        f = sym["file"]
        return f.startswith("ton-index-worker/") and "/external/" not in f

    for (cu, ru), locs in calls.items():
        for f, l in sorted(locs):
            if is_ours(symbols[cu]):
                callers_of[ru].append({"qname": symbols[cu]["qname"], "at": f"{f}:{l}"})
            if is_ours(symbols[ru]):
                callees_of[cu].append({"qname": symbols[ru]["qname"], "at": f"{f}:{l}"})

    for usr, s in symbols.items():
        if not s["is_target"] or s["kind"] == "FUNCTION_TEMPLATE" and not s["signature"]:
            continue
        entry = {
            "qname": s["qname"], "kind": s["kind"], "signature": s["signature"],
            "decl": f"{s['file']}:{s['line']}",
            "body_lines": s.get("body_lines", 0),
            "callers": sorted(callers_of.get(usr, []), key=lambda x: x["at"]),
            "calls": sorted({c["qname"] for c in callees_of.get(usr, [])}),
        }
        out_files[s["file"]]["functions"].append(entry)
    for usr, c in classes.items():
        out_files[c["file"]]["classes"].append(
            {"qname": c["qname"], "decl": f"{c['file']}:{c['line']}"})

    os.makedirs(args.out, exist_ok=True)
    for f, data in out_files.items():
        data["functions"].sort(key=lambda x: x["decl"])
        name = f.replace("/", "__") + ".json"
        with open(os.path.join(args.out, name), "w") as fh:
            json.dump({"file": f, **data}, fh, indent=1)
        print(f"{f}: {len(data['functions'])} fns, {len(data['classes'])} classes")


if __name__ == "__main__":
    main()
