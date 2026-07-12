"""Repro guard for the M1 message-schema compiler (indexer/events/mch/msgs_gen.py).

The committed generated TU + vectors must be byte-reproducible from the committed
schema — the A1 discipline (test_ir_emit.py), extended to the parser generator.
"""
from __future__ import annotations

import pytest

from indexer.events.mch import msgs_gen


def _outputs_present() -> bool:
    return msgs_gen.SCHEMA.is_file() and msgs_gen.CPP_OUT.is_file() and msgs_gen.VEC_OUT.is_file()


def test_generated_cpp_reproduces_committed():
    if not _outputs_present():
        pytest.skip("C++ engine ton-mch-engine/ tree not present in this checkout")
    cpp, _ = msgs_gen.generate()
    assert cpp == msgs_gen.CPP_OUT.read_text(encoding="utf-8")


def test_generated_vectors_reproduce_committed():
    if not _outputs_present():
        pytest.skip("C++ engine ton-mch-engine/ tree not present in this checkout")
    _, vec = msgs_gen.generate()
    assert vec == msgs_gen.VEC_OUT.read_text(encoding="utf-8")


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
