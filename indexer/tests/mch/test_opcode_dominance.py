"""Check the Cocoon matcher anchor in the generated IR."""
from indexer.events.mch.ir_emit import emit_default


def test_cocoon_parent_wallet_anchor_cannot_reach_build():
    artifact = emit_default()
    matcher = next(
        row for row in artifact["matchers"]
        if row["name"] == "cocoon_client_request_refund"
    )
    assert matcher["anchor"] == {
        "kind": "opcode_set", "values": [0x9C69F376, 0xFAFA6CC1],
    }

    root = artifact["nodes"][matcher["root"]]
    assert root["kind"] == "any" and root["capture"] == "refund"
    assert {
        artifact["nodes"][index]["opcode"] for index in root["children"]
    } == {0x2565934C, 0x65448FF4}
    parent = artifact["nodes"][root["parent"]]
    assert parent == {
        "kind": "contract",
        "opcode": 0x9C69F376,
        "optional": True,
        "capture": "wallet",
    }
