"""Tests for the MCH IR emitter (docs/mch-ir-poc.md)."""
from __future__ import annotations

from pathlib import Path

from indexer.events.mch.ir_emit import emit_dir, render_json

SPECS_DIR = (
    Path(__file__).resolve().parents[2] / "indexer" / "events" / "mch" / "specs"
)


def _emit() -> dict:
    return emit_dir(SPECS_DIR)


def test_emit_specs_dir_deterministic():
    first = render_json(_emit())
    second = render_json(_emit())
    assert first.encode("utf-8") == second.encode("utf-8")

    artifact = _emit()
    assert artifact["mch_ir_version"] == "0.1-poc"
    assert artifact["frontend"]["generator"] == "mch-python"
    assert len(artifact["frontend"]["source_files"]) == 7
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
