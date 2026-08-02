"""Guard the generated A/B twins' C++ registry surface (A3(ii))."""
from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest

BUILD_ENG = (
    Path(__file__).resolve().parents[3]
    / "ton-index-worker" / "cmake-build-debug-visual-studio"
    / "ton-mch-engine"
)
SURFACE_JSON = Path(
    os.environ.get(
        "MCH_CPP_SURFACE_JSON",
        str(BUILD_ENG / "generated" / "cpp_surface.json"),
    )
)
EXE = BUILD_ENG / "ton-mch-engine.exe"
KEYS = {"boc_fields", "fns", "lookup_kinds", "parsers", "preds", "shapers"}


def test_generated_surface_is_wellformed():
    if not SURFACE_JSON.is_file():
        pytest.skip("mch_generate_surface output not present in this build tree")
    data = json.loads(SURFACE_JSON.read_text(encoding="utf-8"))
    assert set(data) == KEYS
    for k in KEYS:
        assert data[k] == sorted(data[k]), f"{k} must be sorted"


def test_generated_surface_matches_live_engine():
    if not SURFACE_JSON.is_file() or not EXE.is_file():
        pytest.skip("engine exe / surface not present in this checkout")
    live = json.loads(subprocess.check_output([str(EXE), "--surface"]))
    generated = json.loads(SURFACE_JSON.read_text(encoding="utf-8"))
    assert live == generated, (
        "cpp_surface.json is stale — rebuild the mch_generate_surface target"
    )
