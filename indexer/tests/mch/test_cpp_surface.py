"""Guard the A/B twins' C++ registry surface (A3(ii)).

`ton-index-worker/ton-mch-engine/twins/cpp_surface.json` is the single mirror of
the C++ host/parse registries, loaded by every twin. It is emitted by
`ton-mch-engine --surface`. These tests catch drift between the committed mirror
and the live engine registries (output-affecting drift is also caught by the
classify/parse A/B gates, but a benign-superset drift would slip past those).
"""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

ENG = (
    Path(__file__).resolve().parents[3] / "ton-index-worker" / "ton-mch-engine"
)
SURFACE_JSON = ENG / "twins" / "cpp_surface.json"
EXE = (
    Path(__file__).resolve().parents[3]
    / "ton-index-worker" / "cmake-build-debug-visual-studio"
    / "ton-mch-engine" / "ton-mch-engine.exe"
)
KEYS = {"boc_fields", "fns", "lookup_kinds", "parsers", "preds", "shapers"}


def test_committed_surface_is_wellformed():
    if not SURFACE_JSON.is_file():
        pytest.skip("C++ engine twins/ dir not present in this checkout")
    data = json.loads(SURFACE_JSON.read_text(encoding="utf-8"))
    assert set(data) == KEYS
    for k in KEYS:
        assert data[k] == sorted(data[k]), f"{k} must be sorted"


def test_committed_surface_matches_live_engine():
    if not SURFACE_JSON.is_file() or not EXE.is_file():
        pytest.skip("engine exe / surface not present in this checkout")
    live = json.loads(subprocess.check_output([str(EXE), "--surface"]))
    committed = json.loads(SURFACE_JSON.read_text(encoding="utf-8"))
    assert live == committed, (
        "cpp_surface.json is stale — regenerate: "
        "ton-mch-engine --surface > twins/cpp_surface.json"
    )
