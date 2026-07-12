"""mch-ir.json loading and structural validation (schema docs/mch-ir-poc.md).

Everything checkable without host registries happens here: version, kind/tag
vocabulary, index bounds, statement/expression shapes, and the where_expr
sync-evaluability rule. Registry-dependent checks (predicate, builder names)
happen in engine.build_matchers.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from indexer.events.mch_ir.expr import BUILTINS

IR_VERSION = "1.0"

NODE_KINDS = frozenset({"contract", "block_type", "pred", "any", "or", "recursive"})
ANCHOR_KINDS = frozenset({"opcode_set", "btype", "mixed", "pred"})
CAPTURE_CARDS = frozenset({"one", "opt", "many"})
EXPR_KINDS = frozenset({
    "int", "str", "bool", "null", "name", "dotfield", "attr", "call",
    "lookup", "unary", "bin", "ternary", "list", "record", "mapc", "quant",
    "parse",
})
UNARY_OPS = frozenset({"not", "-"})
BIN_OPS = frozenset({"and", "or", "==", "!=", "<", "<=", ">", ">=", "??", "+", "-", "*"})
STMT_TAGS = frozenset({"parse", "let", "reject", "failed", "broken", "out", "switch"})


class IrLoadError(Exception):
    """The artifact violates the 1.0 schema (or a host name is missing)."""


@dataclass
class Artifact:
    version: str
    frontend: dict
    matchers: list[dict]
    nodes: list[dict]
    registration_order: list[int]


def load_artifact(source: str | Path | dict) -> Artifact:
    """Parse and validate an mch-ir.json artifact from a path or a raw dict."""
    if isinstance(source, dict):
        doc = source
    else:
        try:
            doc = json.loads(Path(source).read_text(encoding="utf-8"))
        except json.JSONDecodeError as ex:
            raise IrLoadError(f"invalid JSON in {source}: {ex}") from ex
    if not isinstance(doc, dict):
        raise IrLoadError("artifact root must be a JSON object")

    version = doc.get("mch_ir_version")
    if version != IR_VERSION:
        raise IrLoadError(f"unsupported mch_ir_version {version!r} (expected {IR_VERSION!r})")

    matchers = doc.get("matchers")
    nodes = doc.get("nodes")
    if not isinstance(matchers, list) or not isinstance(nodes, list):
        raise IrLoadError("artifact must contain 'matchers' and 'nodes' arrays")

    for i, node in enumerate(nodes):
        _check_node(i, node, len(nodes))
    seen_roots: dict[int, str] = {}
    for m in matchers:
        _check_matcher(m, len(nodes))
        root = m["root"]
        if root in seen_roots:
            raise IrLoadError(
                f"matchers {seen_roots[root]!r} and {m['name']!r} share root node {root}"
            )
        seen_roots[root] = m["name"]

    order = doc.get("registration_order")
    if order is None:
        order = list(range(len(matchers)))
    if not isinstance(order, list):
        raise IrLoadError("registration_order must be an array of matcher indices")
    for mi in order:
        if not isinstance(mi, int) or not 0 <= mi < len(matchers):
            raise IrLoadError(f"registration_order entry {mi!r} out of range")

    return Artifact(
        version=version,
        frontend=doc.get("frontend", {}),
        matchers=matchers,
        nodes=nodes,
        registration_order=order,
    )


def _check_matcher(m: Any, n_nodes: int) -> None:
    if not isinstance(m, dict) or not isinstance(m.get("name"), str):
        raise IrLoadError(f"matcher record must be an object with a 'name': {m!r}")
    name = m["name"]
    where = f"matcher {name!r}"

    anchor = m.get("anchor")
    if not isinstance(anchor, dict) or anchor.get("kind") not in ANCHOR_KINDS:
        raise IrLoadError(f"{where}: anchor kind must be one of {sorted(ANCHOR_KINDS)}")
    if anchor["kind"] == "pred":
        # [M2R, R10] predicate anchor: no prefilter, full-scan test_self.
        if not isinstance(anchor.get("pred"), str):
            raise IrLoadError(f"{where}: pred anchor needs a 'pred' predicate name")
    elif anchor["kind"] == "mixed":
        # [M2R, R09] union-membership group: one branch object per alternative
        # branch, each `{op:int}` or `{btype:str}` with an optional `where` name.
        branches = anchor.get("branches")
        if not isinstance(branches, list) or not branches:
            raise IrLoadError(f"{where}: mixed anchor needs a non-empty 'branches' array")
        for br in branches:
            if not isinstance(br, dict):
                raise IrLoadError(f"{where}: bad mixed anchor branch {br!r}")
            has_op = "op" in br
            has_btype = "btype" in br
            if has_op == has_btype:
                raise IrLoadError(
                    f"{where}: mixed anchor branch needs exactly one of 'op'/'btype': {br!r}"
                )
            if has_op and not isinstance(br["op"], int):
                raise IrLoadError(f"{where}: mixed anchor branch 'op' must be an int: {br!r}")
            if has_btype and not isinstance(br["btype"], str):
                raise IrLoadError(f"{where}: mixed anchor branch 'btype' must be a str: {br!r}")
            if "where" in br and not isinstance(br["where"], str):
                raise IrLoadError(f"{where}: mixed anchor branch 'where' must be a predicate name: {br!r}")
    else:
        values = anchor.get("values")
        if not isinstance(values, list) or not values:
            raise IrLoadError(f"{where}: anchor values must be a non-empty array")
        want = int if anchor["kind"] == "opcode_set" else str
        if not all(isinstance(v, want) for v in values):
            raise IrLoadError(f"{where}: anchor {anchor['kind']} values must be {want.__name__}s")

    root = m.get("root")
    if not isinstance(root, int) or not 0 <= root < n_nodes:
        raise IrLoadError(f"{where}: root {root!r} out of range")

    produces = m.get("produces")
    if not isinstance(produces, list) or not produces or not all(isinstance(p, str) for p in produces):
        raise IrLoadError(f"{where}: produces must be a non-empty array of btype strings")

    for cap in m.get("captures", []):
        if not isinstance(cap, dict) or not isinstance(cap.get("name"), str) \
                or cap.get("card") not in CAPTURE_CARDS:
            raise IrLoadError(f"{where}: bad capture record {cap!r}")

    for flag in ("include_excess", "include_bounces"):
        if flag in m and not isinstance(m[flag], bool):
            raise IrLoadError(f"{where}: {flag} must be a bool")

    if "builder" in m and not isinstance(m["builder"], str):
        raise IrLoadError(f"{where}: builder must be a string")

    if "shape" in m and not isinstance(m["shape"], str):
        raise IrLoadError(f"{where}: shape must be a string")

    # `priority` (§8.6): optional; absent defaults to 100. bool is an int
    # subclass, so reject it explicitly.
    if "priority" in m and (isinstance(m["priority"], bool) or not isinstance(m["priority"], int)):
        raise IrLoadError(f"{where}: priority must be an integer")

    program = m.get("build_program")
    if program is not None:
        if not isinstance(program, list):
            raise IrLoadError(f"{where}: build_program must be an array")
        for stmt in program:
            _check_stmt(where, stmt)


def _check_stmt(where: str, stmt: Any) -> None:
    if not isinstance(stmt, dict) or stmt.get("s") not in STMT_TAGS:
        raise IrLoadError(f"{where}: bad build statement {stmt!r}")
    s = stmt["s"]
    if s == "parse":
        types = stmt.get("types")
        if not isinstance(stmt.get("target"), str) or not isinstance(types, list) \
                or not types or not all(isinstance(t, str) for t in types):
            raise IrLoadError(f"{where}: bad parse statement {stmt!r}")
    elif s == "let":
        if not isinstance(stmt.get("name"), str):
            raise IrLoadError(f"{where}: let statement needs a name")
        _check_expr(where, stmt.get("expr"))
    elif s in ("reject", "failed", "broken"):
        _check_expr(where, stmt.get("when"))
    elif s == "out":
        _check_out_fields(where, stmt.get("fields"))
    elif s == "switch":
        branches = stmt.get("branches")
        if not isinstance(branches, list) or not branches:
            raise IrLoadError(f"{where}: switch statement needs a non-empty branches array")
        for b in branches:
            if not isinstance(b, dict) or not isinstance(b.get("btype"), str):
                raise IrLoadError(f"{where}: switch branch needs a string 'btype'")
            if "when" in b:  # absent => the else arm
                _check_expr(where, b["when"])
            _check_out_fields(where, b.get("fields"))


def _check_out_fields(where: str, fields: Any) -> None:
    if not isinstance(fields, list):
        raise IrLoadError(f"{where}: out fields must be an array")
    for f in fields:
        if not isinstance(f, dict) or not isinstance(f.get("name"), str):
            raise IrLoadError(f"{where}: bad out field {f!r}")
        if "optional" in f and not isinstance(f["optional"], bool):
            raise IrLoadError(f"{where}: out field 'optional' must be a bool")
        _check_expr(where, f.get("expr"))


def _check_expr(where: str, e: Any) -> None:
    if not isinstance(e, dict) or e.get("k") not in EXPR_KINDS:
        raise IrLoadError(f"{where}: bad expression node {e!r}")
    k = e["k"]
    if k in ("int", "str", "bool"):
        want = {"int": int, "str": str, "bool": bool}[k]
        # bool is an int subclass; require exact types.
        if type(e.get("v")) is not want:
            raise IrLoadError(f"{where}: {k} literal needs a {want.__name__} 'v'")
    elif k == "name":
        if not isinstance(e.get("id"), str):
            raise IrLoadError(f"{where}: name node needs an 'id'")
    elif k == "dotfield":
        if not isinstance(e.get("name"), str):
            raise IrLoadError(f"{where}: dotfield node needs a 'name'")
    elif k == "attr":
        if not isinstance(e.get("name"), str):
            raise IrLoadError(f"{where}: attr node needs a 'name'")
        _check_expr(where, e.get("of"))
    elif k in ("call", "lookup"):
        key = "fn" if k == "call" else "name"
        args = e.get("args")
        if not isinstance(e.get(key), str) or not isinstance(args, list):
            raise IrLoadError(f"{where}: bad {k} node {e!r}")
        for a in args:
            _check_expr(where, a)
    elif k == "unary":
        if e.get("op") not in UNARY_OPS:
            raise IrLoadError(f"{where}: bad unary op {e.get('op')!r}")
        _check_expr(where, e.get("x"))
    elif k == "bin":
        if e.get("op") not in BIN_OPS:
            raise IrLoadError(f"{where}: bad binary op {e.get('op')!r}")
        _check_expr(where, e.get("l"))
        _check_expr(where, e.get("r"))
    elif k == "ternary":
        _check_expr(where, e.get("cond"))
        _check_expr(where, e.get("then"))
        _check_expr(where, e.get("else"))
    elif k == "list":
        items = e.get("items")
        if not isinstance(items, list):
            raise IrLoadError(f"{where}: list node needs an 'items' array")
        for it in items:
            _check_expr(where, it)
    elif k == "record":
        fields = e.get("fields")
        if not isinstance(fields, list):
            raise IrLoadError(f"{where}: record node needs a 'fields' array")
        for f in fields:
            if not isinstance(f, dict) or not isinstance(f.get("name"), str):
                raise IrLoadError(f"{where}: record field needs a string 'name'")
            _check_expr(where, f.get("expr"))
    elif k == "mapc":
        if not isinstance(e.get("var"), str):
            raise IrLoadError(f"{where}: mapc node needs a string 'var'")
        _check_expr(where, e.get("xs"))
        _check_expr(where, e.get("body"))
    elif k == "quant":
        if e.get("op") not in ("any", "all"):
            raise IrLoadError(f"{where}: quant node 'op' must be 'any' or 'all'")
        if not isinstance(e.get("var"), str):
            raise IrLoadError(f"{where}: quant node needs a string 'var'")
        _check_expr(where, e.get("xs"))
        _check_expr(where, e.get("body"))
    elif k == "parse":
        types = e.get("types")
        if not isinstance(types, list) or not types or not all(isinstance(t, str) for t in types):
            raise IrLoadError(f"{where}: parse node needs a non-empty string 'types' array")
        _check_expr(where, e.get("x"))


def _check_node(i: int, node: Any, n_nodes: int) -> None:
    if not isinstance(node, dict) or node.get("kind") not in NODE_KINDS:
        raise IrLoadError(f"node {i}: unknown kind {node!r}")
    kind = node["kind"]
    where = f"node {i} ({kind})"

    def check_ref(key: str) -> None:
        if key in node:
            v = node[key]
            if not isinstance(v, int) or not 0 <= v < n_nodes:
                raise IrLoadError(f"{where}: {key} id {v!r} out of range")

    if kind == "contract" and not isinstance(node.get("opcode"), int):
        raise IrLoadError(f"{where}: contract needs an integer opcode")
    if kind == "block_type" and not isinstance(node.get("btype"), str):
        raise IrLoadError(f"{where}: block_type needs a btype string")
    if kind == "pred" and not isinstance(node.get("pred"), str):
        raise IrLoadError(f"{where}: pred needs a predicate name")
    if kind == "or":
        branches = node.get("branches")
        if not isinstance(branches, list) or not branches:
            raise IrLoadError(f"{where}: or needs a non-empty branches array")
        for b in branches:
            if not isinstance(b, int) or not 0 <= b < n_nodes:
                raise IrLoadError(f"{where}: branch id {b!r} out of range")
    if kind == "recursive":
        if "step" not in node:
            raise IrLoadError(f"{where}: recursive requires step")
        check_ref("step")
        check_ref("exit")
        # Recursion strategy (spec §10.3, [M2R, R11/T28]): omitted = frontier
        # walk. "cyclic" = cyclic descent — `step` is the rule-body root whose
        # subgraph may contain back-edges to itself; an exit cannot exist
        # (termination is a non-recursive alternative branch inside the body).
        if "strategy" in node:
            if node["strategy"] != "cyclic":
                raise IrLoadError(
                    f"{where}: unknown recursion strategy {node['strategy']!r} "
                    f"(omit for frontier, or use \"cyclic\")"
                )
            if "exit" in node:
                raise IrLoadError(f"{where}: cyclic recursion cannot carry an exit")
    for key in ("child", "parent"):
        check_ref(key)
    for c in node.get("children", []):
        if not isinstance(c, int) or not 0 <= c < n_nodes:
            raise IrLoadError(f"{where}: children id {c!r} out of range")

    if "optional" in node and not isinstance(node["optional"], bool):
        raise IrLoadError(f"{where}: optional must be a bool")
    if "capture" in node and not isinstance(node["capture"], str):
        raise IrLoadError(f"{where}: capture must be a string")
    if "where" in node and not isinstance(node["where"], str):
        raise IrLoadError(f"{where}: where must be a predicate name")
    if "where" in node and "where_expr" in node:
        raise IrLoadError(f"{where}: where and where_expr are mutually exclusive")
    if "where_expr" in node:
        _check_expr(where, node["where_expr"])
        _check_where_expr_sync(where, node["where_expr"])


def _check_where_expr_sync(where: str, e: dict) -> None:
    """where_expr composes onto the engine's synchronous test_self, so async
    constructs (interface lookups, host fns) can never evaluate there."""
    k = e["k"]
    if k == "lookup":
        raise IrLoadError(f"{where}: where_expr cannot contain lookups (test_self is sync)")
    if k in ("mapc", "quant"):
        raise IrLoadError(
            f"{where}: where_expr cannot contain map/any/all comprehensions "
            f"(build-expression only)"
        )
    if k == "parse":
        raise IrLoadError(
            f"{where}: where_expr cannot contain a `parse` expression "
            f"(build-expression only)"
        )
    if k == "call" and e["fn"] not in BUILTINS:
        raise IrLoadError(
            f"{where}: where_expr can only call builtins, not host fn {e['fn']!r}"
        )
    for key in ("of", "x", "l", "r", "cond", "then", "else"):
        if key in e:
            _check_where_expr_sync(where, e[key])
    for a in e.get("args", []):
        _check_where_expr_sync(where, a)
    for it in e.get("items", []):
        _check_where_expr_sync(where, it)
    for f in e.get("fields", []):
        _check_where_expr_sync(where, f["expr"])
