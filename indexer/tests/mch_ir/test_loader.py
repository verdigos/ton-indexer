"""Artifact loading/validation (mch_ir/loader.py) and registry-dependent
checks in build_matchers (mch_ir/engine.py).

The happy path loads the real artifact emitted from indexer/events/mch/specs/
(builder-based matchers plus the declarative vesting matcher) and wires it
against stub registries assembled from the artifact's own referenced names
(builders, shapers, predicates, and the message types / lookup kinds that
declarative build programs reference).
"""
from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

import indexer.events.mch as mch
from indexer.events.mch.ir_emit import emit_dir, render_json
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir import IrLoadError, build_matchers, load_artifact

SPECS_DIR = Path(mch.__file__).parent / "specs"


def _min_doc(**over) -> dict:
    doc = {
        "mch_ir_version": "1.1",
        "frontend": {"generator": "mch-python", "source_files": ["t.mch"]},
        "matchers": [{
            "name": "m",
            "anchor": {"kind": "opcode_set", "values": [1]},
            "root": 0,
            "produces": ["fb"],
            "captures": [{"name": "x", "card": "one"}],
            "include_excess": True,
            "include_bounces": True,
        }],
        "nodes": [{"kind": "contract", "opcode": 1, "capture": "x"}],
        "registration_order": [0],
    }
    doc.update(over)
    return doc


def _collect_lookup_names(expr, out: set) -> None:
    """Walk an IR expression tree collecting every `lookup` kind name."""
    if isinstance(expr, dict):
        if expr.get("k") == "lookup":
            out.add(expr["name"])
        for v in expr.values():
            _collect_lookup_names(v, out)
    elif isinstance(expr, list):
        for v in expr:
            _collect_lookup_names(v, out)


def _collect_call_fn_names(expr, out: set) -> None:
    """Walk an IR expression tree collecting every `call` fn name (builtins
    included; harmless to over-register — builtin names are never consulted in
    reg.fns)."""
    if isinstance(expr, dict):
        if expr.get("k") == "call":
            out.add(expr["fn"])
        for v in expr.values():
            _collect_call_fn_names(v, out)
    elif isinstance(expr, list):
        for v in expr:
            _collect_call_fn_names(v, out)


def _stub_registries(artifact) -> Registries:
    """Permissive registries covering every name the artifact references."""
    reg = Registries()
    for node in artifact.nodes:
        if node["kind"] == "pred":
            reg.predicates.setdefault(node["pred"], lambda _b: True)
        if "where" in node:
            reg.predicates.setdefault(node["where"], lambda _b: True)
    lookup_names: set = set()
    fn_names: set = set()
    for m in artifact.matchers:
        if "builder" in m:
            async def _stub(match):
                return None
            reg.builders.setdefault(m["builder"], _stub)
        if "shape" in m:
            reg.shapers.setdefault(m["shape"], lambda _produced, _match: None)
        # Declarative matchers reference message types (via `parse`), lookup
        # kinds and host fns (via expressions); the loader/build_matchers
        # validate these names, so the permissive stub must cover them too.
        for stmt in m.get("build_program", []):
            if stmt.get("s") == "parse":
                for t in stmt.get("types", []):
                    reg.message_types.setdefault(t, object)
            _collect_lookup_names(stmt, lookup_names)
            _collect_call_fn_names(stmt, fn_names)

    async def _stub_lookup(*_args):
        return None
    for name in lookup_names:
        reg.lookups.setdefault(name, _stub_lookup)

    async def _stub_fn(*_args):
        return None
    for name in fn_names:
        reg.fns.setdefault(name, _stub_fn)
    return reg


# ==========================================================================
# Happy path: the real specs artifact
# ==========================================================================

def test_load_specs_artifact_from_dict_and_file(tmp_path):
    doc = emit_dir(SPECS_DIR)
    art = load_artifact(doc)
    assert art.version == "1.1"
    # The original builder-based spec set has 9 matchers; the count grows as more
    # protocols are ported, so assert the structural invariant, not a magic count.
    assert len(art.matchers) >= 9
    assert art.registration_order == list(range(len(art.matchers)))

    path = tmp_path / "mch-ir.json"
    path.write_text(render_json(doc), encoding="utf-8")
    art2 = load_artifact(path)
    assert art2.matchers == art.matchers
    assert art2.nodes == art.nodes


def test_build_specs_artifact_registration_order_and_metadata():
    art = load_artifact(emit_dir(SPECS_DIR))
    matchers = build_matchers(art, _stub_registries(art))
    assert [m.name for m in matchers] == [rec["name"] for rec in art.matchers]
    # Duplicate names are legal (jetton_transfer appears twice) and both run.
    assert [m.name for m in matchers].count("jetton_transfer") == 2
    for m, rec in zip(matchers, art.matchers):
        assert m.produces == rec["produces"]
        assert m.produces and all(isinstance(p, str) for p in m.produces)


# ==========================================================================
# Structural validation errors
# ==========================================================================

def test_version_mismatch():
    with pytest.raises(IrLoadError, match="mch_ir_version"):
        load_artifact(_min_doc(mch_ir_version="0.2"))
    with pytest.raises(IrLoadError):
        load_artifact({"matchers": [], "nodes": []})


def test_missing_arrays_and_bad_json(tmp_path):
    with pytest.raises(IrLoadError):
        load_artifact(_min_doc(matchers=None))
    with pytest.raises(IrLoadError):
        load_artifact(_min_doc(nodes=None))
    bad = tmp_path / "bad.json"
    bad.write_text("{not json", encoding="utf-8")
    with pytest.raises(IrLoadError, match="invalid JSON"):
        load_artifact(bad)


def test_unknown_node_kind_and_missing_kind_fields():
    with pytest.raises(IrLoadError, match="unknown kind"):
        load_artifact(_min_doc(nodes=[{"kind": "teleport"}]))
    with pytest.raises(IrLoadError, match="opcode"):
        load_artifact(_min_doc(nodes=[{"kind": "contract"}]))
    with pytest.raises(IrLoadError, match="btype"):
        load_artifact(_min_doc(nodes=[{"kind": "block_type"}]))
    with pytest.raises(IrLoadError, match="branches"):
        load_artifact(_min_doc(nodes=[{"kind": "or"}]))
    with pytest.raises(IrLoadError, match="requires step"):
        load_artifact(_min_doc(nodes=[{"kind": "recursive"}]))


def test_edge_ids_out_of_range():
    with pytest.raises(IrLoadError, match="child id"):
        load_artifact(_min_doc(nodes=[{"kind": "contract", "opcode": 1, "child": 5}]))
    with pytest.raises(IrLoadError, match="branch id"):
        load_artifact(_min_doc(nodes=[{"kind": "or", "branches": [7]}]))
    with pytest.raises(IrLoadError, match="children id"):
        load_artifact(_min_doc(nodes=[{"kind": "contract", "opcode": 1, "children": [-1]}]))


def test_bad_matcher_records():
    doc = _min_doc()
    doc["matchers"][0]["root"] = 3
    with pytest.raises(IrLoadError, match="root"):
        load_artifact(doc)

    doc = _min_doc()
    doc["matchers"][0]["anchor"] = {"kind": "regex", "values": ["x"]}
    with pytest.raises(IrLoadError, match="anchor kind"):
        load_artifact(doc)

    doc = _min_doc()
    doc["matchers"][0]["produces"] = []
    with pytest.raises(IrLoadError, match="produces"):
        load_artifact(doc)

    doc = _min_doc()
    doc["matchers"][0]["captures"] = [{"name": "x", "card": "some"}]
    with pytest.raises(IrLoadError, match="capture"):
        load_artifact(doc)


def test_mixed_anchor_schema():
    # [M2R, R09] valid mixed anchor with a per-branch where loads.
    doc = _min_doc()
    doc["matchers"][0]["anchor"] = {
        "kind": "mixed",
        "branches": [{"op": 1}, {"btype": "jetton_transfer", "where": "p"}],
    }
    doc["nodes"] = [{"kind": "any", "capture": "x"}]
    art = load_artifact(doc)
    assert art.matchers[0]["anchor"]["kind"] == "mixed"

    # Empty branches array.
    doc = _min_doc(); doc["matchers"][0]["anchor"] = {"kind": "mixed", "branches": []}
    with pytest.raises(IrLoadError, match="non-empty 'branches'"):
        load_artifact(doc)

    # Branch carries neither op nor btype.
    doc = _min_doc(); doc["matchers"][0]["anchor"] = {"kind": "mixed", "branches": [{"where": "p"}]}
    with pytest.raises(IrLoadError, match="exactly one of 'op'/'btype'"):
        load_artifact(doc)

    # Branch carries both op and btype.
    doc = _min_doc()
    doc["matchers"][0]["anchor"] = {"kind": "mixed", "branches": [{"op": 1, "btype": "x"}]}
    with pytest.raises(IrLoadError, match="exactly one of 'op'/'btype'"):
        load_artifact(doc)

    # Wrong op type.
    doc = _min_doc(); doc["matchers"][0]["anchor"] = {"kind": "mixed", "branches": [{"op": "1"}]}
    with pytest.raises(IrLoadError, match="'op' must be an int"):
        load_artifact(doc)


def test_mixed_anchor_branch_predicate_must_be_registered():
    # build_matchers rejects an anchor-branch where predicate absent from the
    # host registries (same discipline as node-level where predicates).
    doc = _min_doc()
    doc["matchers"][0]["anchor"] = {
        "kind": "mixed",
        "branches": [{"op": 1}, {"op": 2, "where": "unregistered_pred"}],
    }
    doc["nodes"] = [{"kind": "any", "capture": "x"}]
    art = load_artifact(doc)
    with pytest.raises(IrLoadError, match="anchor branch predicate"):
        build_matchers(art, Registries())


def test_shared_root_is_rejected():
    doc = _min_doc()
    doc["matchers"].append(copy.deepcopy(doc["matchers"][0]))
    doc["matchers"][1]["name"] = "m2"
    doc["registration_order"] = [0, 1]
    with pytest.raises(IrLoadError, match="share root"):
        load_artifact(doc)


def test_registration_order_out_of_range():
    with pytest.raises(IrLoadError, match="registration_order"):
        load_artifact(_min_doc(registration_order=[0, 1]))


def test_bad_build_program_and_expr_tags():
    doc = _min_doc()
    doc["matchers"][0]["build_program"] = [{"s": "explode"}]
    with pytest.raises(IrLoadError, match="build statement"):
        load_artifact(doc)

    doc = _min_doc()
    doc["matchers"][0]["build_program"] = [
        {"s": "let", "name": "a", "expr": {"k": "wat"}}
    ]
    with pytest.raises(IrLoadError, match="expression node"):
        load_artifact(doc)

    doc = _min_doc()
    doc["matchers"][0]["build_program"] = [
        {"s": "bogus", "kind": "ton", "args": []}  # unknown statement tag
    ]
    with pytest.raises(IrLoadError, match="bad build statement"):
        load_artifact(doc)


def test_where_and_where_expr_are_mutually_exclusive():
    node = {"kind": "contract", "opcode": 1, "where": "p",
            "where_expr": {"k": "bool", "v": True}}
    with pytest.raises(IrLoadError, match="mutually exclusive"):
        load_artifact(_min_doc(nodes=[node]))


def test_where_expr_must_be_sync_evaluable():
    lookup_expr = {
        "k": "bin", "op": "!=",
        "l": {"k": "lookup", "name": "jetton_wallet", "args": [{"k": "dotfield", "name": "w"}]},
        "r": {"k": "null"},
    }
    node = {"kind": "contract", "opcode": 1, "where_expr": lookup_expr}
    with pytest.raises(IrLoadError, match="sync"):
        load_artifact(_min_doc(nodes=[node]))

    hostfn_expr = {"k": "call", "fn": "my_residue", "args": []}
    node = {"kind": "contract", "opcode": 1, "where_expr": hostfn_expr}
    with pytest.raises(IrLoadError, match="builtins"):
        load_artifact(_min_doc(nodes=[node]))


# ==========================================================================
# Registry-dependent checks (build_matchers)
# ==========================================================================

def test_missing_predicate_is_a_load_error():
    art = load_artifact(_min_doc(nodes=[{"kind": "pred", "pred": "unported"}]))
    with pytest.raises(IrLoadError, match="unported"):
        build_matchers(art, Registries())

    art = load_artifact(_min_doc(
        nodes=[{"kind": "contract", "opcode": 1, "where": "unported_where"}]
    ))
    with pytest.raises(IrLoadError, match="unported_where"):
        build_matchers(art, Registries())


def test_missing_builder_is_a_load_error():
    doc = _min_doc()
    doc["matchers"][0]["builder"] = "build_nothing"
    art = load_artifact(doc)
    with pytest.raises(IrLoadError, match="build_nothing"):
        build_matchers(art, Registries())


def test_emitted_json_roundtrip_is_stable():
    first = render_json(emit_dir(SPECS_DIR))
    art = load_artifact(json.loads(first))
    assert len(art.nodes) > 0
