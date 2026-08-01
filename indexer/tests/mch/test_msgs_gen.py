"""Repro guard for the M1 message-schema compiler (indexer/events/mch/msgs_gen.py).

The committed generated TU must be byte-reproducible from the committed schema.
The enriched message vectors are a frozen, independently owned fixture.
"""
from __future__ import annotations

import json

import pytest

from indexer.events.mch import msgs_gen


def _cpp_inputs_present() -> bool:
    return msgs_gen.SCHEMA.is_file() and msgs_gen.CPP_OUT.is_file()


def test_generated_cpp_reproduces_committed():
    if not _cpp_inputs_present():
        pytest.skip("C++ engine ton-mch-engine/ tree not present in this checkout")
    messages = msgs_gen.parse_schema(msgs_gen.SCHEMA.read_text(encoding="utf-8"))
    cpp = msgs_gen.emit_cpp(messages)
    assert cpp == msgs_gen.CPP_OUT.read_text(encoding="utf-8")


def test_frozen_vectors_cover_every_schema_message():
    if not msgs_gen.SCHEMA.is_file() or not msgs_gen.VEC_OUT.is_file():
        pytest.skip("C++ engine ton-mch-engine/ tree not present in this checkout")
    messages = msgs_gen.parse_schema(msgs_gen.SCHEMA.read_text(encoding="utf-8"))
    cases = json.loads(msgs_gen.VEC_OUT.read_text(encoding="utf-8"))["cases"]
    assert len(cases) == 140
    exact_ok = {
        case["type"]
        for case in cases
        if case.get("expect") == "ok" and isinstance(case.get("expect_value"), str)
    }
    assert sum("expect_value" in case for case in cases) == 70
    assert exact_ok == {message.name for message in messages}


def test_generation_deterministic():
    if not msgs_gen.SCHEMA.is_file():
        pytest.skip("schema not present")
    first = msgs_gen.generate()
    second = msgs_gen.generate()
    assert first == second


def test_registered_names_cover_schema():
    """Every schema message must appear as a generated registry row (the C++ side
    merges these into message_parsers())."""
    if not msgs_gen.SCHEMA.is_file():
        pytest.skip("schema not present")
    msgs = msgs_gen.parse_schema(msgs_gen.SCHEMA.read_text(encoding="utf-8"))
    cpp, _ = msgs_gen.generate()
    for m in msgs:
        assert f'{{"{m.name}", gen_parse_' in cpp
