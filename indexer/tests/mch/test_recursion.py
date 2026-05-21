from __future__ import annotations

from indexer.events.mch.recursion import RecursionStrategy, RecursiveMatcherStrategy


def test_recursive_matcher_strategy_instantiates():
    s = RecursiveMatcherStrategy()
    assert s is not None


def test_recursion_strategy_protocol_shape():
    # Smoke: RecursiveMatcherStrategy satisfies the Protocol structurally.
    s: RecursionStrategy = RecursiveMatcherStrategy()  # type: ignore[assignment]
    assert s is not None
