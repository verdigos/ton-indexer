import re
from pathlib import Path

import pytest

from indexer.events.mch import ir_emit
from indexer.events.mch.btypes import LEAF_BTYPES
from indexer.events.mch.diagnostics import (
    CompileError, R042_MANIFEST_ORDER,
    R043_UNKNOWN_BTYPE,
)


def _matcher(pattern, name="consumer", produces="result"):
    return (
        f"matcher {name} {{\n produces {produces}\n"
        f" {pattern}\n out {{ v: 1 }}\n}}\n"
    )


@pytest.mark.parametrize("pattern", [
    "btype jetton_trasnfer @x",
    "or { btype ton_transfer @x; btype jetton_trasnfer @x }",
    "or { op 1 @x; btype jetton_trasnfer @x }",
    "op 1 @x -> btype jetton_trasnfer",
    "op 1 @x -> { btype jetton_trasnfer; op 2 }",
    "op 1 @x { <- btype jetton_trasnfer }",
    "op 1 @x -> or { btype jetton_trasnfer; op 2 }",
])
def test_unknown_btype_head_reports_matcher_and_name(tmp_path, pattern):
    path = tmp_path / "consumer.mch"
    path.write_text(_matcher(pattern))

    with pytest.raises(CompileError) as exc:
        ir_emit.emit_dir(tmp_path)

    diagnostic, = exc.value.bag.items
    assert diagnostic.code == R043_UNKNOWN_BTYPE
    assert diagnostic.message == (
        "matcher 'consumer': btype 'jetton_trasnfer' is not a leaf btype "
        "and is not produced by any matcher"
    )
    assert diagnostic.span.path == str(path)
    assert diagnostic.span.start_line == 1


@pytest.mark.parametrize("produces, head", [
    ("other_result", "other_result"),
    ("jetton_burn", "jetton_burn"),
])
@pytest.mark.parametrize("manifest", [False, True])
def test_btype_produced_in_another_spec_compiles(tmp_path, produces, head, manifest):
    (tmp_path / "a.mch").write_text(_matcher(f"btype {head} @x"))
    (tmp_path / "z.mch").write_text(_matcher("op 1 @x", "producer", produces))
    if manifest:
        path = tmp_path / "manifest.yaml"
        path.write_text("order: [producer, consumer]\n")
        artifact = ir_emit.emit_manifest(path)
    else:
        artifact = ir_emit.emit_dir(tmp_path)
    assert len(artifact["matchers"]) == 2


@pytest.mark.parametrize("btype", sorted(LEAF_BTYPES))
def test_leaf_btype_head_compiles(tmp_path, btype):
    (tmp_path / "consumer.mch").write_text(_matcher(f"btype {btype} @x"))
    artifact = ir_emit.emit_dir(tmp_path)
    assert artifact["matchers"][0]["anchor"] == {"kind": "btype", "values": [btype]}


def test_leaf_btypes_match_engine_table():
    header = Path(__file__).resolve().parents[3] / "ton-index-worker/ton-mch-engine/src/IrTables.h"
    table = re.search(r"kLeafBtypes\{(.*?)\};", header.read_text(), re.S).group(1)
    assert set(re.findall(r'"(\w+)"', table)) == LEAF_BTYPES


def test_duplicate_names_and_all_unknown_btypes_report_together(tmp_path):
    for name in ("a", "b"):
        (tmp_path / f"{name}.mch").write_text(_matcher(f"btype unknown_{name} @x"))
    with pytest.raises(CompileError) as exc:
        ir_emit.emit_dir(tmp_path)
    assert [d.code for d in exc.value.bag.items] == [
        R042_MANIFEST_ORDER, R043_UNKNOWN_BTYPE, R043_UNKNOWN_BTYPE,
    ]
    assert "unknown_a" in exc.value.bag.items[1].message
    assert "unknown_b" in exc.value.bag.items[2].message


@pytest.mark.parametrize("head", ["ton_transfer", "unknown_hop"])
@pytest.mark.parametrize("cyclic", [False, True])
def test_btype_in_imported_recursive_rule(tmp_path, head, cyclic):
    lib = tmp_path / "lib"
    lib.mkdir()
    rule = (
        f"export rule hop cyclic = or {{ btype {head} -> $hop; op 2 }}\n"
        if cyclic else f"export rule hop = btype {head} -> maybe $hop\n"
    )
    (lib / "hop.mch").write_text(rule)
    (tmp_path / "consumer.mch").write_text(
        'import "lib/hop.mch"\n' + _matcher("op 1 @x -> $hop")
    )
    if head == "ton_transfer":
        assert len(ir_emit.emit_dir(tmp_path)["matchers"]) == 1
    else:
        with pytest.raises(CompileError) as exc:
            ir_emit.emit_dir(tmp_path)
        diagnostic, = exc.value.bag.items
        assert diagnostic.code == R043_UNKNOWN_BTYPE
        assert "unknown_hop" in diagnostic.message


def test_production_btype_heads_are_known():
    specs = Path(ir_emit.__file__).parent / "specs"
    assert len(ir_emit.emit_manifest(specs / "manifest.yaml")["matchers"]) == 110
