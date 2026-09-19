from __future__ import annotations

import json
from pathlib import Path

import lz4.frame
import pytest

from tests.utils.fixture_json import pack, unpack

TRACES = sorted((Path(__file__).parent / "traces").glob("*.lz4"))


@pytest.mark.parametrize("trace", TRACES, ids=lambda trace: trace.stem)
def test_fixture_json_round_trip(trace: Path, tmp_path: Path) -> None:
    repacked = tmp_path / trace.name
    pack(json.loads(json.dumps(unpack(trace))), repacked)
    # LZ4 frame headers differ between fixture writers; the loader reads the msgpack payload.
    assert lz4.frame.decompress(repacked.read_bytes()) == lz4.frame.decompress(trace.read_bytes())
