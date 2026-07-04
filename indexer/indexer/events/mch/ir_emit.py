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
from indexer.events.mch.compile import compile_file
from indexer.events.mch.compiler import CompiledMatcher
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries

IR_VERSION = "0.1-poc"


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
            if m.build not in reg.builders:
                async def _stub_builder(match, _name=m.build):
                    return None
                reg.builders[m.build] = _stub_builder
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
    node_id = len(nodes)
    memo[id(m)] = node_id
    rec: dict = {}
    nodes.append(rec)

    if isinstance(m, RecursiveMatcher):
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
    if m.child_matcher is not None:
        rec["child"] = _emit_node(m.child_matcher, nodes, memo)
    if m.children_matchers:
        rec["children"] = [_emit_node(c, nodes, memo) for c in m.children_matchers]
    if m.parent_matcher is not None:
        rec["parent"] = _emit_node(m.parent_matcher, nodes, memo)
    return node_id


def _anchor_record(root: BlockMatcher) -> dict:
    anchor_opcodes = getattr(root, "_ir_anchor_opcodes", None)
    anchor_btypes = getattr(root, "_ir_anchor_btypes", None)
    if anchor_opcodes or anchor_btypes:
        if anchor_opcodes and anchor_btypes:
            raise ValueError(
                "mixed opcode/btype anchor group is not representable in IR "
                f"{IR_VERSION} (anchor.kind is opcode_set XOR btype)"
            )
        if anchor_opcodes:
            return {"kind": "opcode_set", "values": [_unsigned32(o) for o in anchor_opcodes]}
        return {"kind": "btype", "values": list(anchor_btypes)}
    if isinstance(root, ContractMatcher):
        return {"kind": "opcode_set", "values": [_unsigned32(root.opcode)]}
    if isinstance(root, BlockTypeMatcher):
        return {"kind": "btype", "values": [root.block_type]}
    raise ValueError(f"unsupported anchor root type {type(root).__name__}")


# `produces` in .mch names either a host block CLASS or a bare btype string
# (spec §8.1). The IR schema requires btype strings, so class names are
# normalized here. POC: static map; M5 resolves via the real block-type
# registry (class -> instance btype) at emit time.
_CLASS_TO_BTYPE = {
    "JettonTransferBlock": "jetton_transfer",
    "JettonBurnBlock": "jetton_burn",
    "JettonMintBlock": "jetton_mint",
    "JettonSwapBlock": "jetton_swap",
}


def _produces_btype(name: str) -> str:
    return _CLASS_TO_BTYPE.get(name, name)


def emit_ir(compiled: list[CompiledMatcher], source_files: list[str]) -> dict:
    """Build the top-level artifact dict, keys in schema order."""
    matchers_out: list[dict] = []
    nodes: list[dict] = []
    for cm in compiled:
        memo: dict[int, int] = {}
        root_id = _emit_node(cm.matcher, nodes, memo)
        matchers_out.append({
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
            "builder": cm.build_fn_name,
        })
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
