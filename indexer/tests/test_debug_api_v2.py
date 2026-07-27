"""Fixture-based test for the recording-based debug payload builder."""
import asyncio
from pathlib import Path

import indexer.core.database  # noqa: F401  (break circular import)
import indexer.events.context as context
from tests.utils.trace_deserializer import load_trace_from_file
from tests.utils.repository import TestInterfaceRepository

FIXTURE = Path("tests/traces/5rfO7FVucVaVGDuEtOK6pW7TN7iYvLdSQ0QoVAYRPXs=.lz4")
# If this fixture fails to load, substitute any .lz4 referenced by a passing
# case in tests/test_cases/*.yaml — the test is fixture-agnostic.


def test_build_event_payload_shape():
    from dev_utils.debug_api_v2 import build_event_payload

    trace, interfaces = load_trace_from_file(FIXTURE)
    context.interface_repository.set(TestInterfaceRepository(interfaces or {}))

    payload = asyncio.run(build_event_payload(trace))

    # snapshots: first is the raw init tree, at least one matcher fired after it
    assert payload["snapshots"][0]["matcher"] == "init"
    assert payload["snapshots"][-1]["matcher"] == "final"
    assert len(payload["snapshots"]) >= 3

    # every tree node carries a block_id for recording <-> graph joins
    root = payload["snapshots"][0]["tree"]
    assert "block_id" in root["data"]
    assert all("block_id" in c["data"] for c in root["children"])

    # recording has matcher info and synthesized attempts
    rec = payload["recording"]
    assert rec["matchers"]
    assert rec["attempts"]
    results = {a["result"] for a in rec["attempts"]}
    assert "MATCHED" in results

    # actions produced
    assert payload["actions"]
