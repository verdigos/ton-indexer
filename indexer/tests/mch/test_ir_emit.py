"""Tests for the MCH IR emitter (docs/mch-ir-poc.md)."""
from __future__ import annotations

from pathlib import Path

from indexer.events.mch.compile import compile_text
from indexer.events.mch.ir_emit import (
    build_stub_registries,
    emit_dir,
    emit_ir,
    emit_manifest,
    render_json,
)
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse

SPECS_DIR = (
    Path(__file__).resolve().parents[2] / "indexer" / "events" / "mch" / "specs"
)
# Committed reproducible-emit contract (A1): manifest + the artifact it produces.
IR_DIR = (
    Path(__file__).resolve().parents[3]
    / "ton-index-worker" / "ton-mch-engine" / "ir"
)


def _emit() -> dict:
    return emit_dir(SPECS_DIR)


def test_manifest_reproduces_committed_artifact():
    """The committed ir/mch_ir.json must be byte-reproducible from its manifest —
    the A1 reproducibility guarantee (no session-scratch emit dir)."""
    manifest = IR_DIR / "emit_manifest.json"
    committed = IR_DIR / "mch_ir.json"
    if not manifest.is_file() or not committed.is_file():
        import pytest

        pytest.skip("C++ engine ir/ dir not present in this checkout")
    emitted = render_json(emit_manifest(manifest)).encode("utf-8")
    assert emitted == committed.read_bytes()


def test_emit_specs_dir_deterministic():
    first = render_json(_emit())
    second = render_json(_emit())
    assert first.encode("utf-8") == second.encode("utf-8")

    artifact = _emit()
    assert artifact["mch_ir_version"] == "1.0"
    assert artifact["frontend"]["generator"] == "mch-python"
    # Every .mch in the specs dir is represented (count-agnostic: the spec set
    # grows as matchers are ported).
    assert len(artifact["frontend"]["source_files"]) == len(list(SPECS_DIR.glob("*.mch")))
    assert artifact["registration_order"] == list(range(len(artifact["matchers"])))
    # Every node reference must be a valid index into the flat nodes array.
    n = len(artifact["nodes"])
    for node in artifact["nodes"]:
        refs = [node.get("child"), node.get("parent"), node.get("step"), node.get("exit")]
        refs += node.get("children", []) + node.get("branches", [])
        for r in refs:
            if r is not None:
                assert 0 <= r < n


def test_jetton_transfer_graph_shape():
    artifact = _emit()
    matcher = next(m for m in artifact["matchers"] if m["name"] == "jetton_transfer")
    nodes = artifact["nodes"]

    # Anchor: unsigned opcode set for JettonTransfer 0x0f8a7ea5.
    assert matcher["anchor"] == {"kind": "opcode_set", "values": [0x0F8A7EA5]}
    # `produces JettonTransferBlock` (class name) normalizes to the btype string.
    assert matcher["produces"] == ["jetton_transfer"]

    root = nodes[matcher["root"]]
    assert root["kind"] == "contract"
    assert root["opcode"] == 0x0F8A7EA5
    assert root["capture"] == "transfer"
    assert not root.get("optional", False)

    internal = nodes[root["child"]]
    assert internal["kind"] == "contract"
    assert internal["opcode"] == 0x178D4519
    assert internal["capture"] == "internal"
    assert not internal.get("optional", False)

    notify = nodes[internal["child"]]
    assert notify["kind"] == "contract"
    assert notify["opcode"] == 0x7362D09C
    assert notify["capture"] == "notify"
    assert notify["optional"] is True


def test_dedust_swap_recursive_step_exit():
    artifact = _emit()
    matcher = next(m for m in artifact["matchers"] if m["name"] == "dedust_swap")
    nodes = artifact["nodes"]

    # Collect the matcher's reachable subgraph from its root.
    seen: set[int] = set()
    stack = [matcher["root"]]
    while stack:
        i = stack.pop()
        if i in seen:
            continue
        seen.add(i)
        node = nodes[i]
        for key in ("child", "parent", "step", "exit"):
            if key in node:
                stack.append(node[key])
        stack.extend(node.get("children", []))
        stack.extend(node.get("branches", []))

    recursive = [nodes[i] for i in seen if nodes[i]["kind"] == "recursive"]
    assert len(recursive) == 1
    rec = recursive[0]
    assert "step" in rec and "exit" in rec

    # $peer_chain step: or over the two swap-hop variants (peer / external).
    step = nodes[rec["step"]]
    assert step["kind"] == "or"
    assert len(step["branches"]) == 2
    step_opcodes = {nodes[b]["opcode"] for b in step["branches"]}
    assert step_opcodes == {0x72ACA8AA, 0x61EE542D}

    # Exit: the terminal DedustPayoutFromPool payout chain.
    exit_node = nodes[rec["exit"]]
    assert exit_node["kind"] == "contract"
    assert exit_node["opcode"] == 0xAD4EB6F5


# ---------------------------------------------------------------------------
# Expressions & build programs (M4 §9) — inline matcher, not part of specs/
# (which currently declares no build programs).
# ---------------------------------------------------------------------------

_BUILD_PROGRAM_SRC = """\
opcode TestBuildOp  = 0x1001
opcode TestBuildOp2 = 0x1002

matcher test_build_program {
    produces test_build_out
    entry    @a

    op TestBuildOp @a -> op TestBuildOp2 @b where (.value > 0)

    parse a as TestMsg
    let w = lookup jetton_wallet(a.msg.destination)
    reject when w == null
    out {
        sender: a.msg.source if a.msg.source != null else b.msg.source
        amount: amount(a.msg.value)
    }
    flow ton(a.msg.source, a.msg.value)
}
"""


def _emit_build_program() -> dict:
    path = "test_build_program.mch"
    file_ast = parse(list(tokenize(_BUILD_PROGRAM_SRC, path)), path)
    registries = build_stub_registries([file_ast])
    compiled = compile_text(_BUILD_PROGRAM_SRC, path, registries)
    build_stmts = [m.build_stmts for m in file_ast.matchers]
    return emit_ir(compiled, [path], build_stmts)


def test_build_program_encoding():
    artifact = _emit_build_program()
    matcher = artifact["matchers"][0]
    nodes = artifact["nodes"]

    # Node-level `where_expr` on the non-anchor node (`b`), alongside the
    # existing named-predicate `where` field (mutually exclusive per node).
    anchor = nodes[matcher["root"]]
    assert anchor == {"kind": "contract", "opcode": 0x1001, "capture": "a", "child": 1}
    assert nodes[1] == {
        "kind": "contract",
        "opcode": 0x1002,
        "capture": "b",
        "where_expr": {
            "k": "bin", "op": ">",
            "l": {"k": "dotfield", "name": "value"},
            "r": {"k": "int", "v": 0},
        },
    }

    def attr(base: dict, *names: str) -> dict:
        e = base
        for n in names:
            e = {"k": "attr", "of": e, "name": n}
        return e

    a_msg_source = attr({"k": "name", "id": "a"}, "msg", "source")
    a_msg_value = attr({"k": "name", "id": "a"}, "msg", "value")
    a_msg_destination = attr({"k": "name", "id": "a"}, "msg", "destination")
    b_msg_source = attr({"k": "name", "id": "b"}, "msg", "source")

    assert matcher["build_program"] == [
        {"s": "parse", "target": "a", "types": ["TestMsg"]},
        {
            "s": "let", "name": "w",
            "expr": {"k": "lookup", "name": "jetton_wallet", "args": [a_msg_destination]},
        },
        {
            "s": "reject",
            "when": {"k": "bin", "op": "==", "l": {"k": "name", "id": "w"}, "r": {"k": "null"}},
        },
        {
            "s": "out",
            "fields": [
                {
                    "name": "sender",
                    "expr": {
                        "k": "ternary",
                        "cond": {"k": "bin", "op": "!=", "l": a_msg_source, "r": {"k": "null"}},
                        "then": a_msg_source,
                        "else": b_msg_source,
                    },
                },
                {
                    "name": "amount",
                    "expr": {"k": "call", "fn": "amount", "args": [a_msg_value]},
                },
            ],
        },
        {"s": "flow", "kind": "ton", "args": [a_msg_source, a_msg_value]},
    ]
    # Declarative matcher (build statements, no registered builder): "builder"
    # is omitted entirely, not emitted as null (container rule).
    assert "builder" not in matcher


def test_build_program_deterministic():
    first = render_json(_emit_build_program())
    second = render_json(_emit_build_program())
    assert first.encode("utf-8") == second.encode("utf-8")
