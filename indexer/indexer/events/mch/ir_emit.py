"""MCH IR emitter (POC) — serializes lowered matcher graphs per docs/mch-ir-poc.md.

Walks the instantiated engine-object graph produced by the compiler (approach:
the compiler stamps lightweight ``_ir_*`` attributes at its lowering sites, so
the emitted IR is faithful to the actual lowering by construction). Node ids
are per-matcher DFS pre-order from the anchor node; the recursion order at each
node is: or-branches / recursive step then exit, then child, then children set
items, then parent. Two runs over the same specs yield byte-identical output.

CLI::

    python -m indexer.events.mch.ir_emit <specs_dir> -o <out.json>

The CLI self-provisions permissive Registries: it pre-parses the spec files and
registers always-true predicate stubs, no-op async builder stubs, and dummy
block types for every referenced name, so specs compile without the production
host registries.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from indexer.events.blocks.basic_matchers import (
    BlockMatcher,
    BlockTypeMatcher,
    ContractMatcher,
    ExclusiveOrMatcher,
    GenericMatcher,
    OrMatcher,
    RecursiveMatcher,
)
from indexer.events.mch import ast_
from indexer.events.mch.btypes import produces_btype
from indexer.events.mch.compile import compile_file
from indexer.events.mch.compiler import CompiledMatcher
from indexer.events.mch.expr_ir import IR_VERSION, emit_expr as _emit_expr
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries

def _unsigned32(opcode: int) -> int:
    """Canonical unsigned 32-bit opcode (schema rule: hosts compare unsigned)."""
    return opcode & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Stub registries (CLI self-provisioning)
# ---------------------------------------------------------------------------


def _collect_predicate_names(p: ast_.PatternExpr, out: set[str]) -> None:
    if isinstance(p, ast_.Node):
        if p.where_predicate is not None:
            out.add(p.where_predicate)
        if isinstance(p.head, ast_.PredHead):
            out.add(p.head.name)
    elif isinstance(p, ast_.Maybe):
        _collect_predicate_names(p.inner, out)
    elif isinstance(p, ast_.Sequence):
        _collect_predicate_names(p.head, out)
        for _edge, atom in p.tail:
            _collect_predicate_names(atom, out)
    elif isinstance(p, ast_.Alternative):
        for b in p.branches:
            _collect_predicate_names(b, out)
    elif isinstance(p, ast_.ChildrenBlock):
        for it in p.items:
            _collect_predicate_names(it, out)
    # RuleRef: rule bodies are scanned via File.rules directly.


class _StubMessage:
    """Message-type stand-in for compile-time validation (never parsed here)."""

    def __init__(self, _slice):
        pass


def build_stub_registries(files: list[ast_.File]) -> Registries:
    """Permissive Registries covering every name the given files reference."""
    reg = Registries()
    predicate_names: set[str] = set()
    for f in files:
        for pd in f.predicates:
            predicate_names.add(pd.name)
        for r in f.rules:
            _collect_predicate_names(r.pattern, predicate_names)
        for m in f.matchers:
            _collect_predicate_names(m.pattern, predicate_names)
            for produced in m.produces:
                reg.block_types.setdefault(produced, object)
            if m.build is not None and m.build not in reg.builders:
                async def _stub_builder(match, _name=m.build):
                    return None
                reg.builders[m.build] = _stub_builder
            if m.shape is not None and m.shape not in reg.shapers:
                reg.shapers[m.shape] = lambda produced, match: None
            for stmt in m.build_stmts:
                if isinstance(stmt, ast_.ParseStmt):
                    for t in stmt.msg_types:
                        reg.message_types.setdefault(t, _StubMessage)
    for name in sorted(predicate_names):
        reg.predicates[name] = lambda _block: True
    return reg


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------


def _emit_node(m: BlockMatcher, nodes: list[dict], memo: dict[int, int]) -> int:
    """Append `m`'s node record (and its subtree) to `nodes`; return its id.

    Already-seen objects return their existing id (back-reference), which both
    handles shared sub-objects and guarantees termination on cyclic graphs.
    """
    existing = memo.get(id(m))
    if existing is not None:
        return existing

    # Cyclic-strategy rule references (§10.3 cyclic descent, [M2R, R11/T28]).
    # An in-body self-reference is a true back-edge: no node of its own, it
    # resolves to the shared body root's id (the memo guarantees the cycle
    # terminates). An outer reference emits a `recursive` node with
    # `strategy: "cyclic"` and the body root as `step` (never an `exit`).
    cyclic_target = getattr(m, "_mch_cyclic_target", None)
    if cyclic_target is not None and getattr(m, "_mch_cyclic_inner", False):
        target_id = _emit_node(cyclic_target, nodes, memo)
        memo[id(m)] = target_id
        return target_id

    node_id = len(nodes)
    memo[id(m)] = node_id
    rec: dict = {}
    nodes.append(rec)

    if cyclic_target is not None:
        rec["kind"] = "recursive"
        rec["strategy"] = "cyclic"
        rec["step"] = _emit_node(cyclic_target, nodes, memo)
    elif isinstance(m, RecursiveMatcher):
        rec["kind"] = "recursive"
        rec["step"] = _emit_node(m.repeating_matcher, nodes, memo)
        if m.exit_matcher is not None:
            rec["exit"] = _emit_node(m.exit_matcher, nodes, memo)
    elif isinstance(m, OrMatcher):
        rec["kind"] = "or"
        rec["branches"] = [_emit_node(b, nodes, memo) for b in m.matchers]
        rec["exclusive"] = isinstance(m, ExclusiveOrMatcher)
    elif isinstance(m, ContractMatcher):
        rec["kind"] = "contract"
        rec["opcode"] = _unsigned32(m.opcode)
    elif isinstance(m, BlockTypeMatcher):
        rec["kind"] = "block_type"
        rec["btype"] = m.block_type
    elif isinstance(m, GenericMatcher):
        pred_name = getattr(m, "_ir_pred", None)
        if pred_name is not None:
            rec["kind"] = "pred"
            rec["pred"] = pred_name
        else:
            # Permissive matchers: `any` heads, recursion sentinels, and
            # opcode-set anchor roots (whose test lives in the matcher record's
            # anchor, see _anchor_record).
            rec["kind"] = "any"
    else:
        raise ValueError(f"unsupported matcher type {type(m).__name__} in IR emission")

    if m.optional:
        rec["optional"] = True
    capture = getattr(m, "_mch_capture_name", None)
    if capture is not None:
        rec["capture"] = capture
    where = getattr(m, "_ir_where", None)
    if where is not None:
        rec["where"] = where
    where_expr = getattr(m, "_ir_where_expr", None)
    if where_expr is not None:
        rec["where_expr"] = _emit_expr(where_expr)
    if m.child_matcher is not None:
        rec["child"] = _emit_node(m.child_matcher, nodes, memo)
    if m.children_matchers:
        rec["children"] = [_emit_node(c, nodes, memo) for c in m.children_matchers]
    if m.parent_matcher is not None:
        rec["parent"] = _emit_node(m.parent_matcher, nodes, memo)
    return node_id


def _anchor_record(root: BlockMatcher) -> dict:
    # Mixed-kind or per-branch-`where` anchor group ([M2R, R09]): a `mixed`
    # record carrying one branch object per alternative branch, each `{op}` or
    # `{btype}` with an optional `where` predicate name. Membership is the union
    # of all branch heads; the matching branch's `where` (if any) is tested
    # after membership. Pure single-kind no-`where` groups keep the T10
    # opcode_set/btype forms below (compiler stamps exactly one representation).
    # Predicate anchor ([M2R, R10]): no opcode/btype prefilter; the anchor test
    # is the named predicate, full-scanned by the engine.
    anchor_pred = getattr(root, "_ir_anchor_pred", None)
    if anchor_pred is not None:
        return {"kind": "pred", "pred": anchor_pred}
    anchor_branches = getattr(root, "_ir_anchor_branches", None)
    if anchor_branches is not None:
        out_branches: list[dict] = []
        for kind, value, where_name in anchor_branches:
            rec = {"op": _unsigned32(value)} if kind == "op" else {"btype": value}
            if where_name is not None:
                rec["where"] = where_name
            out_branches.append(rec)
        return {"kind": "mixed", "branches": out_branches}
    anchor_opcodes = getattr(root, "_ir_anchor_opcodes", None)
    anchor_btypes = getattr(root, "_ir_anchor_btypes", None)
    if anchor_opcodes:
        return {"kind": "opcode_set", "values": [_unsigned32(o) for o in anchor_opcodes]}
    if anchor_btypes:
        return {"kind": "btype", "values": list(anchor_btypes)}
    if isinstance(root, ContractMatcher):
        return {"kind": "opcode_set", "values": [_unsigned32(root.opcode)]}
    if isinstance(root, BlockTypeMatcher):
        return {"kind": "btype", "values": [root.block_type]}
    raise ValueError(f"unsupported anchor root type {type(root).__name__}")


# The IR schema requires btype strings; class names are normalized via the
# shared map in btypes.py (also used by the declarative build path).
_produces_btype = produces_btype


# ---------------------------------------------------------------------------
# Expressions & build programs (M4 §9). See docs/mch-ir-poc.md.
# ---------------------------------------------------------------------------


# _emit_expr = expr_ir.emit_expr (moved there in T33 so the compiler can share
# the encoding; ir_emit imports the compiler, so the reverse import is barred).


def _emit_stmt(stmt: ast_.BuildStmt) -> dict:
    if isinstance(stmt, ast_.ParseStmt):
        return {"s": "parse", "target": stmt.capture, "types": list(stmt.msg_types)}
    if isinstance(stmt, ast_.LetStmt):
        return {"s": "let", "name": stmt.name, "expr": _emit_expr(stmt.value)}
    if isinstance(stmt, ast_.RejectStmt):
        return {"s": "reject", "when": _emit_expr(stmt.condition)}
    if isinstance(stmt, ast_.FailedStmt):
        return {"s": "failed", "when": _emit_expr(stmt.condition)}
    if isinstance(stmt, ast_.BrokenStmt):
        return {"s": "broken", "when": _emit_expr(stmt.condition)}
    if isinstance(stmt, ast_.FlowStmt):
        return {"s": "flow", "kind": stmt.kind, "args": [_emit_expr(a) for a in stmt.args]}
    if isinstance(stmt, ast_.OutStmt):
        return {
            "s": "out",
            "fields": [{"name": f.name, "expr": _emit_expr(f.value)} for f in stmt.fields],
        }
    raise ValueError(f"unsupported build statement {type(stmt).__name__} in IR emission")


def emit_ir(
    compiled: list[CompiledMatcher],
    source_files: list[str],
    build_stmts: list[tuple[ast_.BuildStmt, ...]] | None = None,
) -> dict:
    """Build the top-level artifact dict, keys in schema order.

    Build statements come from `CompiledMatcher.build_stmts`; the optional
    `build_stmts` parameter (parallel to `compiled`) overrides them and is kept
    for callers that pre-extracted statements from parsed `ast_.File`s.
    """
    if build_stmts is None:
        build_stmts = [cm.build_stmts for cm in compiled]
    matchers_out: list[dict] = []
    nodes: list[dict] = []
    for cm, stmts in zip(compiled, build_stmts):
        memo: dict[int, int] = {}
        root_id = _emit_node(cm.matcher, nodes, memo)
        rec: dict = {
            "name": cm.name,
            "anchor": _anchor_record(cm.matcher),
            "root": root_id,
            "produces": [_produces_btype(p) for p in cm.matcher._ir_produces],
            "captures": [
                {"name": cap_name, "card": card}
                for cap_name, card in cm.matcher._ir_captures
            ],
            "include_excess": bool(cm.matcher.include_excess),
            "include_bounces": bool(cm.matcher.include_bounces),
        }
        # Declarative matchers (build statements, no registered builder) have
        # no builder name; per the container rule (no nulls for absent
        # fields), omit rather than emit "builder": null.
        if cm.build_fn_name is not None:
            rec["builder"] = cm.build_fn_name
        if stmts:
            rec["build_program"] = [_emit_stmt(s) for s in stmts]
        # `shape` directive (§8.7): registered shaper name; container rule —
        # omit when absent, never emit null.
        if cm.shape_fn_name is not None:
            rec["shape"] = cm.shape_fn_name
        matchers_out.append(rec)
    return {
        "mch_ir_version": IR_VERSION,
        "frontend": {"generator": "mch-python", "source_files": source_files},
        "matchers": matchers_out,
        "nodes": nodes,
        "registration_order": list(range(len(matchers_out))),
    }


def emit_dir(specs_dir: Path) -> dict:
    """Compile every .mch file in `specs_dir` (sorted by file name) and emit IR."""
    paths = sorted(Path(specs_dir).glob("*.mch"), key=lambda p: p.name)
    if not paths:
        raise FileNotFoundError(f"no .mch files found in {specs_dir}")
    files = [
        parse(list(tokenize(p.read_text(encoding="utf-8"), str(p))), str(p))
        for p in paths
    ]
    registries = build_stub_registries(files)
    compiled: list[CompiledMatcher] = []
    for p in paths:
        compiled.extend(compile_file(p, registries))
    return emit_ir(compiled, [p.name for p in paths])


def render_json(artifact: dict) -> str:
    return json.dumps(artifact, indent=2, sort_keys=False, ensure_ascii=False) + "\n"


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(
        prog="python -m indexer.events.mch.ir_emit",
        description="Emit MCH IR (0.1-poc) from a directory of .mch spec files.",
    )
    ap.add_argument("specs_dir", help="directory containing .mch spec files")
    ap.add_argument("-o", "--output", required=True, help="output JSON path")
    args = ap.parse_args(argv)

    artifact = emit_dir(Path(args.specs_dir))
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write(render_json(artifact))
    print(
        f"wrote {args.output}: {len(artifact['matchers'])} matchers, "
        f"{len(artifact['nodes'])} nodes"
    )


if __name__ == "__main__":
    main()
