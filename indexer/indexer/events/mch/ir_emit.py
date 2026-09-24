"""MCH IR emitter for lowered node graphs.

Walks the ``LNode`` graph the compiler lowers each ``.mch`` matcher into (plain
records, no engine primitives). Node ids are per-matcher DFS pre-order from the
anchor node; the recursion order at each node is: or-branches / recursive step
then exit, then child, then children set items, then parent. Two runs over the
same specs yield byte-identical output.

CLI::

    python -m indexer.events.mch.ir_emit -o <out.json>

The CLI self-provisions permissive Registries: it pre-parses the spec files and
registers predicate and message-type stubs, so specs compile without the
production host registries.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import yaml

from indexer.events.mch import ast_
from indexer.events.mch.btypes import LEAF_BTYPES
from indexer.events.mch.compile import compile_file
from indexer.events.mch.compiler import CompiledMatcher, LNode
from indexer.events.mch.diagnostics import (
    CompileError, DiagnosticBag, R042_MANIFEST_ORDER, R043_UNKNOWN_BTYPE,
    R047_UNKNOWN_DATA_KEY, Span,
    format_diagnostic,
)
from indexer.events.mch.expr_ir import IR_VERSION, emit_expr as _emit_expr
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import RESERVED_ACTION_COLUMNS, _map_fn_children


def _unsigned32(opcode: int) -> int:
    """Canonical unsigned 32-bit opcode (schema rule: hosts compare unsigned)."""
    return opcode & 0xFFFFFFFF


# Stub registries (CLI self-provisioning)


def _collect_expr_message_types(e: ast_.Expr, out: set[str]) -> None:
    def visit(node):
        if isinstance(node, ast_.ParseExpr):
            out.update(node.msg_types)
        return _map_fn_children(node, visit)

    visit(e)


def _collect_pattern_names(p: ast_.PatternExpr, out: set[str], msg_types: set[str]) -> None:
    """Predicate names into `out`, `as T` annotation types into `msg_types`."""
    if isinstance(p, ast_.Node):
        if p.where_predicate is not None:
            out.add(p.where_predicate)
        if isinstance(p.head, ast_.PredHead):
            out.add(p.head.name)
        if p.where_expr is not None:
            _collect_expr_message_types(p.where_expr, msg_types)
        if p.parse_types is not None:
            msg_types.update(p.parse_types)
    for child in ast_.children(p):
        _collect_pattern_names(child, out, msg_types)
    # RuleRef: rule bodies are scanned via File.rules directly.


class _StubMessage:
    """Message-type stand-in for compile-time validation (never parsed here)."""

    def __init__(self, _slice):
        pass


def build_stub_registries(files: list[ast_.File]) -> Registries:
    """Permissive Registries covering every name the given files reference."""
    reg = Registries()
    predicate_names: set[str] = set()
    annotated_types: set[str] = set()

    def stub_expr(e: ast_.Expr) -> None:
        _collect_expr_message_types(e, annotated_types)

    def stub_fields(fields: tuple[ast_.OutField, ...]) -> None:
        for field in fields:
            stub_expr(field.value)

    for f in files:
        for fn in f.fns:
            stub_expr(fn.body)
        for pd in f.predicates:
            predicate_names.add(pd.name)
        for r in f.rules:
            _collect_pattern_names(r.pattern, predicate_names, annotated_types)
        for m in f.matchers:
            _collect_pattern_names(m.pattern, predicate_names, annotated_types)
            if m.shape is not None and m.shape not in reg.shapers:
                reg.shapers[m.shape] = lambda produced, match: None
            for stmt in m.build_stmts:
                if isinstance(stmt, ast_.ParseStmt):
                    for t in stmt.msg_types:
                        reg.message_types.setdefault(t, _StubMessage)
                elif isinstance(stmt, ast_.LetStmt):
                    stub_expr(stmt.value)
                elif isinstance(stmt, (ast_.RejectStmt, ast_.FailedStmt)):
                    stub_expr(stmt.condition)
                elif isinstance(stmt, ast_.OutStmt):
                    stub_fields(stmt.fields)
                elif isinstance(stmt, ast_.SwitchStmt):
                    for branch in stmt.branches:
                        if branch.condition is not None:
                            stub_expr(branch.condition)
                        stub_fields(branch.out.fields)
    for name in sorted(predicate_names):
        reg.predicates[name] = lambda _block: True
    for t in annotated_types:
        reg.message_types.setdefault(t, _StubMessage)
    return reg


# Emission


def _emit_node(n: LNode, nodes: list[dict], memo: dict[int, int]) -> int:
    """Append `n`'s node record (and its subtree) to `nodes`; return its id.

    Already-seen objects return their existing id (back-reference), which both
    handles shared sub-objects and guarantees termination on cyclic graphs.
    """
    existing = memo.get(id(n))
    if existing is not None:
        return existing

    # An in-body cyclic self-reference resolves to the shared body root.
    if n.kind == "cyclic_ref":
        target_id = _emit_node(n.target, nodes, memo)
        memo[id(n)] = target_id
        return target_id

    node_id = len(nodes)
    memo[id(n)] = node_id
    rec: dict = {}
    nodes.append(rec)

    if n.kind == "recursive" and n.strategy == "cyclic":
        # Outer cyclic reference: a `recursive` node with `strategy: "cyclic"`
        # and the body root as `step` (never an `exit`).
        rec["kind"] = "recursive"
        rec["strategy"] = "cyclic"
        rec["step"] = _emit_node(n.step, nodes, memo)
    elif n.kind == "recursive":
        rec["kind"] = "recursive"
        rec["step"] = _emit_node(n.step, nodes, memo)
        if n.exit_ is not None:
            rec["exit"] = _emit_node(n.exit_, nodes, memo)
    elif n.kind == "or":
        rec["kind"] = "or"
        rec["branches"] = [_emit_node(b, nodes, memo) for b in n.branches]
        rec["exclusive"] = n.exclusive
    elif n.kind == "contract":
        rec["kind"] = "contract"
        rec["opcode"] = _unsigned32(n.opcode)
    elif n.kind == "block_type":
        rec["kind"] = "block_type"
        rec["btype"] = n.btype
    elif n.kind == "pred":
        rec["kind"] = "pred"
        rec["pred"] = n.pred
    elif n.kind == "any":
        # Permissive nodes: `any` heads, recursion sentinels, and opcode-set /
        # predicate anchor roots (whose test lives in the matcher record's
        # anchor, see _anchor_record).
        rec["kind"] = "any"
    else:
        raise ValueError(f"unsupported node kind {n.kind!r} in IR emission")

    if n.optional:
        rec["optional"] = True
    if n.peek:
        rec["peek"] = True
    if n.many:
        rec["many"] = True
    if n.max is not None:
        rec["max"] = n.max
    if n.capture is not None:
        rec["capture"] = n.capture
    if n.where is not None:
        rec["where"] = n.where
    if n.where_expr is not None:
        rec["where_expr"] = _emit_expr(n.where_expr)
    if n.child is not None:
        rec["child"] = _emit_node(n.child, nodes, memo)
    if n.children:
        rec["children"] = [_emit_node(c, nodes, memo) for c in n.children]
    if n.parent is not None:
        rec["parent"] = _emit_node(n.parent, nodes, memo)
    return node_id


def _anchor_record(cm: CompiledMatcher) -> dict:
    # Predicate anchors have no opcode/btype prefilter. The anchor test
    # is the named predicate, full-scanned by the engine.
    # A mixed-kind or per-branch-`where` anchor group uses a `mixed`
    # record carrying one branch object per alternative branch, each `{op}` or
    # `{btype}` with an optional `where` predicate name. Membership is the union
    # of all branch heads; the matching branch's `where` (if any) is tested
    # after membership. Pure single-kind groups without `where` keep the
    # opcode_set/btype forms (compiler records exactly one representation).
    if cm.anchor_pred is not None:
        return {"kind": "pred", "pred": cm.anchor_pred}
    if cm.anchor_branches is not None:
        out_branches: list[dict] = []
        for kind, value, where_name in cm.anchor_branches:
            rec = {"op": _unsigned32(value)} if kind == "op" else {"btype": value}
            if where_name is not None:
                rec["where"] = where_name
            out_branches.append(rec)
        return {"kind": "mixed", "branches": out_branches}
    if cm.anchor_opcodes:
        return {"kind": "opcode_set", "values": [_unsigned32(o) for o in cm.anchor_opcodes]}
    if cm.anchor_btypes:
        return {"kind": "btype", "values": list(cm.anchor_btypes)}
    root = cm.root
    if root.kind == "contract":
        return {"kind": "opcode_set", "values": [_unsigned32(root.opcode)]}
    if root.kind == "block_type":
        return {"kind": "btype", "values": [root.btype]}
    raise ValueError(f"unsupported anchor root kind {root.kind!r}")


# Expressions and build programs.


# _emit_expr = expr_ir.emit_expr. The compiler shares that encoding, and the
# import direction avoids a cycle through ir_emit.


def _emit_stmt(stmt: ast_.BuildStmt) -> dict:
    if isinstance(stmt, ast_.ParseStmt):
        return {"s": "parse", "target": stmt.capture, "types": list(stmt.msg_types)}
    if isinstance(stmt, ast_.LetStmt):
        return {"s": "let", "name": stmt.name, "expr": _emit_expr(stmt.value)}
    if isinstance(stmt, ast_.RejectStmt):
        return {"s": "reject", "when": _emit_expr(stmt.condition)}
    if isinstance(stmt, ast_.FailedStmt):
        return {"s": "failed", "when": _emit_expr(stmt.condition)}
    if isinstance(stmt, ast_.OutStmt):
        return {"s": "out", "fields": _emit_out_fields(stmt.fields)}
    if isinstance(stmt, ast_.SwitchStmt):
        return {"s": "switch", "branches": [_emit_switch_branch(b) for b in stmt.branches]}
    raise ValueError(f"unsupported build statement {type(stmt).__name__} in IR emission")


def _emit_out_fields(fields) -> list[dict]:
    out = []
    for f in fields:
        rec = {"name": f.name, "expr": _emit_expr(f.value)}
        # `optional` is emitted only when set, keeping existing artifacts
        # byte-identical (additive-only IR change).
        if f.optional:
            rec["optional"] = True
        out.append(rec)
    return out


def _emit_switch_branch(b: ast_.SwitchBranch) -> dict:
    rec: dict = {"btype": b.btype, "fields": _emit_out_fields(b.out.fields)}
    # The `else` arm (condition is None) omits `when`.
    if b.condition is not None:
        rec["when"] = _emit_expr(b.condition)
    return rec


def emit_ir(compiled: list[CompiledMatcher]) -> dict:
    """Build the top-level artifact dict, keys in schema order."""
    matchers_out: list[dict] = []
    nodes: list[dict] = []
    for cm in compiled:
        memo: dict[int, int] = {}
        root_id = _emit_node(cm.root, nodes, memo)
        rec: dict = {
            "name": cm.name,
            "anchor": _anchor_record(cm),
            "root": root_id,
            "produces": list(cm.produces),
            "captures": [
                {"name": cap_name, "card": card}
                for cap_name, card in cm.captures
            ],
            "include_excess": bool(cm.include_excess),
            "include_bounces": bool(cm.include_bounces),
        }
        if cm.build_stmts:
            rec["build_program"] = [_emit_stmt(s) for s in cm.build_stmts]
        if cm.direct_action:
            rec["direct_action"] = True
            rec["action_types"] = list(cm.action_types)
        # `shape` contains a registered shaper name. Omit it when absent;
        # omit when absent, never emit null.
        if cm.shape_fn_name is not None:
            rec["shape"] = cm.shape_fn_name
        matchers_out.append(rec)
    return {
        "mch_ir_version": IR_VERSION,
        "reserved_columns": list(RESERVED_ACTION_COLUMNS),
        "matchers": matchers_out,
        "nodes": nodes,
    }


def _validate_direct_consumer_keys(
    compiled: list[CompiledMatcher], bag: DiagnosticBag,
) -> None:
    producer_keys: dict[str, set[str]] = {}
    direct_btypes: set[str] = set()
    for matcher in compiled:
        if matcher.direct_action:
            direct_btypes.update(matcher.produces)
        for stmt in matcher.build_stmts:
            if isinstance(stmt, ast_.OutStmt):
                keys = {field.name for field in stmt.fields}
                for btype in matcher.produces:
                    producer_keys.setdefault(btype, set()).update(keys)
            elif isinstance(stmt, ast_.SwitchStmt):
                for branch in stmt.branches:
                    producer_keys.setdefault(branch.btype, set()).update(
                        field.name for field in branch.out.fields
                    )

    for matcher in compiled:
        captures = {name: {btype} for name, btype in matcher.capture_btypes}
        reported: set[tuple[int, str]] = set()

        def check_key(key: str, btypes: set[str], span: Span) -> None:
            relevant = btypes & direct_btypes
            if relevant and not any(key in producer_keys.get(btype, ()) for btype in relevant):
                marker = (span.start_off, key)
                if marker not in reported:
                    reported.add(marker)
                    bag.error(
                        R047_UNKNOWN_DATA_KEY,
                        f"matcher {matcher.name!r} reads unknown data key {key!r} "
                        f"from direct btype {sorted(relevant)!r}",
                        span,
                    )

        def scan_expr(expr: ast_.Expr) -> None:
            def visit(node):
                if (isinstance(node, ast_.FieldAccess)
                        and isinstance(node.target, ast_.FieldAccess)
                        and node.target.field == "data"
                        and isinstance(node.target.target, ast_.NameRef)):
                    check_key(node.field, captures.get(node.target.target.name, set()), node.span)
                return _map_fn_children(node, visit)

            visit(expr)

        visited: set[int] = set()

        def scan_node(node: LNode | None) -> None:
            if node is None or id(node) in visited:
                return
            visited.add(id(node))
            if (node.where_expr is not None and node.kind == "block_type"
                    and node.btype in direct_btypes):
                def visit_where(expr):
                    if isinstance(expr, ast_.FieldRef):
                        check_key(expr.field, {node.btype}, expr.span)
                    return _map_fn_children(expr, visit_where)

                visit_where(node.where_expr)
            for child in (
                node.child, *(node.children or ()), node.parent,
                *(node.branches or ()), node.step, node.exit_, node.target,
            ):
                scan_node(child)

        scan_node(matcher.root)

        for stmt in matcher.build_stmts:
            if isinstance(stmt, ast_.LetStmt):
                scan_expr(stmt.value)
            elif isinstance(stmt, (ast_.RejectStmt, ast_.FailedStmt)):
                scan_expr(stmt.condition)
            elif isinstance(stmt, ast_.OutStmt):
                for field in stmt.fields:
                    scan_expr(field.value)
            elif isinstance(stmt, ast_.SwitchStmt):
                for branch in stmt.branches:
                    if branch.condition is not None:
                        scan_expr(branch.condition)
                    for field in branch.out.fields:
                        scan_expr(field.value)


def _emit_paths(paths: list[Path]) -> dict:
    """Compile the given .mch files in path and declaration order."""
    files = [
        parse(list(tokenize(p.read_text(encoding="utf-8"), str(p))), str(p))
        for p in paths
    ]
    registries = build_stub_registries(files)
    compiled: list[CompiledMatcher] = []
    for p in paths:
        compiled.extend(compile_file(p, registries))
    seen: dict[str, str] = {}
    bag = DiagnosticBag()
    for cm in compiled:
        if cm.name in seen:
            bag.error(
                R042_MANIFEST_ORDER,
                f"duplicate matcher {cm.name!r} in {seen[cm.name]!r} and {cm.source_span.path!r}",
                cm.source_span,
            )
        else:
            seen[cm.name] = cm.source_span.path
    known = LEAF_BTYPES | {p for cm in compiled for p in cm.produces}
    for cm in compiled:
        heads = list(cm.anchor_btypes or ())
        heads.extend(value for kind, value, _ in cm.anchor_branches or () if kind == "btype")
        visited: set[int] = set()
        pending = [cm.root]
        while pending:
            node = pending.pop()
            if id(node) in visited:
                continue
            visited.add(id(node))
            if node.kind == "block_type":
                heads.append(node.btype)
            pending.extend(n for n in (
                node.child, *(node.children or ()), node.parent,
                *(node.branches or ()), node.step, node.exit_, node.target,
            ) if n is not None)
        for btype in heads:
            if btype not in known:
                bag.error(
                    R043_UNKNOWN_BTYPE,
                    f"matcher '{cm.name}': btype '{btype}' is not a leaf btype "
                    "and is not produced by any matcher",
                    cm.source_span,
                )
    _validate_direct_consumer_keys(compiled, bag)
    if bag.items:
        raise CompileError(bag)
    return emit_ir(compiled)


def emit_dir(specs_dir: Path) -> dict:
    """Compile every .mch file in `specs_dir` (sorted by file name) and emit IR."""
    paths = sorted(Path(specs_dir).glob("*.mch"), key=lambda p: p.name)
    if not paths:
        raise FileNotFoundError(f"no .mch files found in {specs_dir}")
    return _emit_paths(paths)


def emit_manifest(manifest_path: Path) -> dict:
    """Compile adjacent .mch files and emit matchers in YAML manifest order."""
    manifest_path = Path(manifest_path)
    data = yaml.safe_load(manifest_path.read_text(encoding="utf-8"))
    order = data.get("order") if isinstance(data, dict) else None
    bag = DiagnosticBag()
    span = Span(str(manifest_path), 1, 1, 1, 1, 0, 0)
    if not isinstance(order, list) or not all(isinstance(name, str) for name in order):
        bag.error(R042_MANIFEST_ORDER, "manifest order must be a list of strings", span)
        raise CompileError(bag)
    artifact = emit_dir(manifest_path.parent)
    matchers = {m["name"]: m for m in artifact["matchers"]}
    seen: set[str] = set()
    for name in order:
        if name in seen:
            bag.error(R042_MANIFEST_ORDER, f"matcher {name!r} is listed twice", span)
        seen.add(name)
        if name not in matchers:
            bag.error(R042_MANIFEST_ORDER, f"listed name {name!r} has no matcher", span)
    for name in sorted(matchers.keys() - seen):
        bag.error(R042_MANIFEST_ORDER, f"matcher {name!r} is missing from manifest order", span)
    if bag.items:
        raise CompileError(bag)
    artifact["matchers"] = [matchers[name] for name in order]
    return artifact


def emit_default() -> dict:
    """Compile every production spec and emit matchers in manifest order."""
    return emit_manifest(Path(__file__).resolve().parent / "specs" / "manifest.yaml")


def render_json(artifact: dict) -> str:
    return json.dumps(artifact, indent=2, sort_keys=False, ensure_ascii=False) + "\n"


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(
        prog="python -m indexer.events.mch.ir_emit",
        description="Emit MCH IR from the production manifest.",
    )
    ap.add_argument("-o", "--output", required=True, help="output JSON path")
    args = ap.parse_args(argv)

    try:
        artifact = emit_default()
    except CompileError as exc:
        for diagnostic in exc.bag.items:
            source = Path(diagnostic.span.path).read_text(encoding="utf-8")
            print(format_diagnostic(diagnostic, source), file=sys.stderr)
        raise SystemExit(1) from None
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write(render_json(artifact))
    print(
        f"wrote {args.output}: {len(artifact['matchers'])} matchers, "
        f"{len(artifact['nodes'])} nodes"
    )


if __name__ == "__main__":
    main()
