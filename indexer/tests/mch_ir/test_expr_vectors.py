"""T42 — expression conformance vector runner.

Executes every vector in vectors/expr_vectors.json through the REAL expression
evaluators and checks the result against the vector's `expect`:

- the async build-expression evaluator (`mch_ir/expr.Evaluator`) — the primary
  expression VM and the C++ contract — runs EVERY vector;
- the sync where-clause evaluator (`mch/expr_eval._sync_eval`) additionally runs
  every vector that is sync-legal (no `name`/`dotfield`/`lookup`), asserting the
  two evaluators agree on the shared operator/builtin/accessor cores.

Value encoding (env inputs + `expect`) is the tagged convention documented in
docs/mch-ir-spec.md §12. A `{"$fault": true}` expectation means the expression
must reject (raise EvalError). Regenerate the JSON with vectors/_generate.py.
"""
from __future__ import annotations

import base64
import json
from pathlib import Path
from types import SimpleNamespace

import pytest
from pytoniq_core import Cell

from indexer.events.blocks.core import Block
from indexer.events.blocks.utils import AccountId, Amount, Asset
from indexer.events.mch.expr_eval import EvalError, _sync_eval
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir.expr import Evaluator

_VECTORS = json.loads(
    (Path(__file__).parent / "vectors" / "expr_vectors.json").read_text(encoding="utf-8")
)["vectors"]

_FAULT = {"$fault": True}


# --------------------------------------------------------------------------
# Value codec (docs/mch-ir-spec.md §12)
# --------------------------------------------------------------------------
def _decode(v):
    if isinstance(v, dict):
        if "$amount" in v:
            return Amount(v["$amount"])
        if "$amount_float" in v:
            return Amount(float(v["$amount_float"]))  # render-only; faults at use
        if "$account" in v:
            return AccountId(v["$account"])          # null -> addr_none
        if "$asset_ton" in v:
            return Asset(is_ton=True)
        if "$asset_jetton" in v:
            return Asset(is_ton=False, jetton_address=v["$asset_jetton"])
        if "$bytes_b64" in v:
            return base64.b64decode(v["$bytes_b64"])
        if "$cell_b64" in v:
            return Cell.one_from_boc(base64.b64decode(v["$cell_b64"]))
        if "$obj" in v:
            return SimpleNamespace(**{k: _decode(x) for k, x in v["$obj"].items()})
        if "$list" in v:
            return [_decode(x) for x in v["$list"]]
        if "$dict" in v:
            return {k: _decode(x) for k, x in v["$dict"].items()}
        return {k: _decode(x) for k, x in v.items()}  # plain record
    if isinstance(v, list):
        return [_decode(x) for x in v]
    return v


def _equal(actual, expected) -> bool:
    """Structural equality against a DECODED expected value."""
    if actual is None or expected is None:
        return actual is None and expected is None
    if isinstance(expected, bool):
        return isinstance(actual, bool) and actual == expected
    if isinstance(expected, (AccountId, Amount, Asset)):
        return type(actual) is type(expected) and bool(actual == expected)
    if isinstance(expected, bytes):
        return isinstance(actual, bytes) and actual == expected
    if isinstance(expected, list):
        return isinstance(actual, list) and len(actual) == len(expected) \
            and all(_equal(a, e) for a, e in zip(actual, expected))
    if isinstance(expected, dict):
        return isinstance(actual, dict) and set(actual) == set(expected) \
            and all(_equal(actual[k], expected[k]) for k in expected)
    return type(actual) is type(expected) and actual == expected


def _registries(vec) -> Registries:
    reg = Registries()
    for name, canned in vec.get("lookups", {}).items():
        result = _decode(canned)

        async def _stub(*_args, _result=result):
            return _result
        reg.lookups[name] = _stub
    return reg


def _sync_legal(expr) -> bool:
    """True when the expression contains no name/dotfield/lookup (the sync
    where-clause evaluator has no environment and rejects lookups)."""
    if isinstance(expr, dict):
        if expr.get("k") in ("name", "dotfield", "lookup"):
            return False
        return all(_sync_legal(v) for v in expr.values())
    if isinstance(expr, list):
        return all(_sync_legal(v) for v in expr)
    return True


def _env(vec) -> dict:
    return {k: _decode(v) for k, v in vec.get("env", {}).items()}


@pytest.mark.asyncio
@pytest.mark.parametrize("vec", _VECTORS, ids=[v["name"] for v in _VECTORS])
async def test_expr_vector_async(vec):
    ev = Evaluator(_env(vec), _registries(vec))
    if vec["expect"] == _FAULT:
        with pytest.raises(EvalError):
            await ev.eval(vec["expr"])
        return
    got = await ev.eval(vec["expr"])
    expected = _decode(vec["expect"])
    assert _equal(got, expected), (
        f"{vec['name']}: {vec.get('src')!r} -> {got!r} ({type(got).__name__}), "
        f"expected {expected!r} ({type(expected).__name__})"
    )


@pytest.mark.parametrize(
    "vec",
    [v for v in _VECTORS if _sync_legal(v["expr"])],
    ids=[v["name"] for v in _VECTORS if _sync_legal(v["expr"])],
)
def test_expr_vector_sync_parity(vec):
    """Sync-legal vectors must give the identical result through the sync
    where-clause evaluator (shared operator/builtin cores)."""
    block = Block("t", [], None)
    if vec["expect"] == _FAULT:
        with pytest.raises(EvalError):
            _sync_eval(vec["expr"], block)
        return
    got = _sync_eval(vec["expr"], block)
    assert _equal(got, _decode(vec["expect"])), f"{vec['name']} sync mismatch: {got!r}"
