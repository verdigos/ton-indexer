from pathlib import Path

import pytest

from indexer.events.mch import ir_emit
from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import (
    CompileError,
    R045_DIRECT_ACTION_TYPE,
    R046_DIRECT_ACTION_RESERVED,
    R047_UNKNOWN_DATA_KEY,
)
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import RESERVED_ACTION_COLUMNS


def _direct(out: str, marker: str = "action direct") -> str:
    return (
        "matcher m {\n"
        "  produces result\n"
        f"  {marker}\n"
        "  op 1 @x\n"
        f"  out {{ {out} }}\n"
        "}\n"
    )


def _codes(source: str) -> list[str]:
    try:
        compile_text(source, "test.mch", Registries())
    except CompileError as exc:
        return [item.code for item in exc.bag.items]
    return []


def test_direct_action_ir_carries_mode_types_and_reserved_columns(tmp_path: Path):
    (tmp_path / "direct.mch").write_text(
        _direct('type: "z" if true else "a"; source: x.msg.source; _scratch: 1')
    )

    artifact = ir_emit.emit_dir(tmp_path)
    matcher = artifact["matchers"][0]

    assert artifact["reserved_columns"] == list(RESERVED_ACTION_COLUMNS)
    assert matcher["direct_action"] is True
    assert matcher["action_types"] == ["a", "z"]


def test_direct_action_without_type_uses_produced_btype(tmp_path: Path):
    (tmp_path / "direct.mch").write_text(_direct("source: x.msg.source"))

    matcher = ir_emit.emit_dir(tmp_path)["matchers"][0]

    assert matcher["action_types"] == ["result"]


@pytest.mark.parametrize("value", ["x", '"ok" if true else x', "1"])
def test_direct_action_type_rejects_non_literal_leaves(value: str):
    assert R045_DIRECT_ACTION_TYPE in _codes(_direct(f"type: {value}"))


def test_engine_owned_parent_key_is_rejected():
    assert R046_DIRECT_ACTION_RESERVED in _codes(
        _direct("parent_gasless_action: null")
    )


def test_duplicate_direct_out_key_uses_existing_diagnostic():
    assert "R014_DUP_OUT_FIELD" in _codes(_direct("source: null; source: null"))


def test_non_direct_matcher_omits_direct_metadata(tmp_path: Path):
    (tmp_path / "legacy.mch").write_text(_direct("type: 1", marker=""))

    matcher = ir_emit.emit_dir(tmp_path)["matchers"][0]

    assert "direct_action" not in matcher
    assert "action_types" not in matcher


def test_production_direct_matchers_publish_their_action_types():
    matchers = {row["name"]: row for row in ir_emit.emit_default()["matchers"]}

    assert {
        name: row["action_types"]
        for name, row in matchers.items()
        if row.get("direct_action")
    } == {
        "multisig_create_order": ["multisig_create_order"],
        "nft_transfer": ["nft_transfer"],
        "tgbtc_mint": ["tgbtc_mint", "tgbtc_mint_fallback"],
        "tgbtc_mint_log_only": ["tgbtc_mint", "tgbtc_mint_fallback"],
    }


def test_consumer_can_read_reserved_and_block_only_direct_keys(tmp_path: Path):
    (tmp_path / "producer.mch").write_text(
        "matcher producer {\n"
        "  produces direct_result\n"
        "  action direct\n"
        "  op 1 @x\n"
        "  out { source: x.msg.source; _private: 1 }\n"
        "}\n"
    )
    (tmp_path / "consumer.mch").write_text(
        "matcher consumer {\n"
        "  produces final_result\n"
        "  btype direct_result @x where (.source != null)\n"
        "  out { value: x.data._private }\n"
        "}\n"
    )

    assert len(ir_emit.emit_dir(tmp_path)["matchers"]) == 2


@pytest.mark.parametrize("read", ["x.data.typo", ".typo"])
def test_consumer_rejects_unknown_direct_key(tmp_path: Path, read: str):
    (tmp_path / "producer.mch").write_text(
        "matcher producer {\n"
        "  produces direct_result\n"
        "  action direct\n"
        "  op 1 @x\n"
        "  out { source: x.msg.source }\n"
        "}\n"
    )
    where = f" where ({read})" if read.startswith(".") else ""
    out = "1" if where else read
    (tmp_path / "consumer.mch").write_text(
        "matcher consumer {\n"
        "  produces final_result\n"
        f"  btype direct_result @x{where}\n"
        f"  out {{ value: {out} }}\n"
        "}\n"
    )

    with pytest.raises(CompileError) as exc:
        ir_emit.emit_dir(tmp_path)

    assert R047_UNKNOWN_DATA_KEY in [item.code for item in exc.value.bag.items]
