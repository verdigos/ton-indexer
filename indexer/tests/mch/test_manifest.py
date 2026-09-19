from pathlib import Path

import pytest
import yaml

from indexer.events.mch import ir_emit
from indexer.events.mch.diagnostics import CompileError, R042_MANIFEST_ORDER


def _matcher(name):
    return f"matcher {name} {{ produces result\n op 1 @x\n out {{ v: 1 }} }}\n"


@pytest.mark.parametrize("order, message", [
    ([], "'m' is missing"),
    (["m", "m"], "'m' is listed twice"),
    (["m", "unknown"], "'unknown' has no matcher"),
    ("m", "must be a list of strings"),
    ({"m": 1}, "must be a list of strings"),
    (None, "must be a list of strings"),
    (["m", 1], "must be a list of strings"),
])
def test_manifest_errors(tmp_path, order, message):
    (tmp_path / "m.mch").write_text(_matcher("m"))
    manifest = tmp_path / "manifest.yaml"
    manifest.write_text(yaml.safe_dump({"order": order}))
    with pytest.raises(CompileError) as exc:
        ir_emit.emit_manifest(manifest)
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == R042_MANIFEST_ORDER
    assert message in diagnostic.message
    assert diagnostic.span.path == str(manifest)


@pytest.mark.parametrize("use_default", [True, False])
def test_duplicate_matcher_names_across_spec_files_rejected(tmp_path, monkeypatch, use_default):
    specs = tmp_path / "specs"
    specs.mkdir()
    paths = [specs / "a.mch", specs / "b.mch"]
    for path in paths:
        path.write_text(_matcher("m"))
    (specs / "manifest.yaml").write_text(yaml.safe_dump({"order": ["m"]}))
    monkeypatch.setattr(ir_emit, "__file__", str(tmp_path / "ir_emit.py"))

    with pytest.raises(CompileError) as exc:
        if use_default:
            ir_emit.emit_default()
        else:
            ir_emit._emit_paths(paths)
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == R042_MANIFEST_ORDER
    assert "'m'" in diagnostic.message
    assert all(path.name in diagnostic.message for path in paths)


def test_default_compiles_all_direct_specs_in_manifest_order(tmp_path, monkeypatch):
    specs = tmp_path / "specs"
    specs.mkdir()
    (specs / "a.mch").write_text(_matcher("first") + _matcher("second"))
    (specs / "z.mch").write_text(_matcher("third"))
    (specs / "lib").mkdir()
    (specs / "lib" / "unused.mch").write_text("not a matcher")
    (tmp_path / "specs_decl").mkdir()
    (tmp_path / "specs_decl" / "unused.mch").write_text("not a matcher")
    order = ["third", "second", "first"]
    (specs / "manifest.yaml").write_text(yaml.safe_dump({"order": order}))
    monkeypatch.setattr(ir_emit, "__file__", str(tmp_path / "ir_emit.py"))

    artifact = ir_emit.emit_default()

    assert [m["name"] for m in artifact["matchers"]] == order


def test_production_manifest_order():
    specs = Path(ir_emit.__file__).parent / "specs"
    order = yaml.safe_load((specs / "manifest.yaml").read_text())["order"]
    artifact = ir_emit.emit_default()
    assert [m["name"] for m in artifact["matchers"]] == order
