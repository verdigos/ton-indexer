"""R01 remake — field + topology parity between the legacy
TelegramNftPurchaseBlockMatcher and specs/telegram_nft.mch, plus the IR round
trip (compile -> emit_ir -> load_artifact -> build_matchers).

Same assertion depth as test_evaa_withdraw / test_dns: btype, per-key `data`
equality, failed/broken flags, value_flow, consumed event-node set, AND block
topology (children_blocks / next_blocks multisets) — the M2R remake gate binds
on topology because the action hierarchy derives from it.

Fixture (no new recording): the one corpus fragment purchase,
`nft.yaml::Fragment purchase`
  aMUaDa0Ml9lnk7KzHKpkW1UK6FPyYod2qotieVVWMd8=
A teleitem bid win: an NftOwnershipAssigned call whose parent is the funding
ton_transfer, which fans out to two AuctionFillUp payouts (royalty then seller
payout, by created_lt). Exercises the two-payout core branch and the full
`op NftOwnershipAssigned <- btype ton_transfer -> {AuctionFillUp; AuctionFillUp}`
pattern.

Corpus gaps (no recording per task rules): no single-payout fragment purchase
(the `maybe op AuctionFillUp @payout_2` / core `len == 1` branch), no
external-source-call parent (telemint mint variant), no non-teleitem orphan
NftOwnershipAssigned. All three stay covered by the shared core through the
legacy wrapper (tests/test_actions.py); the mch pattern targets the
ton_transfer-funded purchase shape only.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.nft import TelegramNftPurchaseBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders.telegram_nft import telegram_nft_registries
from indexer.events.mch.ir_emit import emit_ir, render_json
import indexer.events.mch.specs as specs_pkg

# --- IR engine gate (same convention as test_evaa_withdraw) ------------------
try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
except Exception:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False

_SKIP_REASON = (
    "indexer.events.mch_ir not importable; the legacy-vs-compiled parity and "
    "the IR emit above still ran"
)

TELEGRAM_MCH = Path(specs_pkg.__file__).parent / "telegram_nft.mch"


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace) -> list:
    root = _build_block_tree(trace)
    matches: list = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return [b for b in matches if b.btype == "nft_transfer"]


def _values_equal(a, b) -> bool:
    if a is None and b is None:
        return True
    if type(a) is not type(b):
        return False
    return bool(a == b)


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _sort_key(block: Block):
    return (block.min_lt, tuple(_node_hashes(block)))


def _topology(block: Block) -> tuple[list[tuple[str, tuple]], list[tuple[str, tuple]]]:
    """(children_blocks, next_blocks) as (btype, sorted-node-hashes) multisets."""
    def sig(bs):
        return sorted((b.btype, tuple(_node_hashes(b))) for b in bs)
    return sig(block.children_blocks), sig(block.next_blocks)


def _assert_parity(actual: list[Block], expected: list[Block],
                   actual_tag: str, expected_tag: str) -> None:
    assert len(expected) > 0, f"{expected_tag} produced no nft_transfer blocks - bad fixture"
    assert len(actual) == len(expected), (
        f"count mismatch: {actual_tag}={len(actual)} {expected_tag}={len(expected)}"
    )
    for a, e in zip(sorted(actual, key=_sort_key), sorted(expected, key=_sort_key)):
        assert a.btype == e.btype == "nft_transfer"
        assert a.failed == e.failed, f"failed flag: {actual_tag}={a.failed} {expected_tag}={e.failed}"
        assert a.broken == e.broken, f"broken flag: {actual_tag}={a.broken} {expected_tag}={e.broken}"

        assert set(a.data.keys()) == set(e.data.keys()), (
            f"data keys: {actual_tag}={sorted(a.data)} {expected_tag}={sorted(e.data)}"
        )
        for key in sorted(e.data):
            av, ev = a.data[key], e.data[key]
            assert _values_equal(av, ev), (
                f"data[{key!r}]: {actual_tag}={av!r} ({type(av).__name__}) "
                f"{expected_tag}={ev!r} ({type(ev).__name__})"
            )

        assert a.value_flow.to_dict() == e.value_flow.to_dict(), (
            f"value_flow: {actual_tag}={a.value_flow.to_dict()} {expected_tag}={e.value_flow.to_dict()}"
        )
        assert _node_hashes(a) == _node_hashes(e), (
            f"consumed event nodes: {actual_tag}={_node_hashes(a)} {expected_tag}={_node_hashes(e)}"
        )
        assert _topology(a) == _topology(e), (
            f"block topology: {actual_tag}={_topology(a)} {expected_tag}={_topology(e)}"
        )


async def _run_parity(trace) -> list[Block]:
    """legacy vs the v2 IR engine (the compiled path is retired — T29). Returns
    legacy blocks."""
    hw_blocks = await _run_matcher(TelegramNftPurchaseBlockMatcher(), trace)

    # Compiling is a frontend-internal step ir_emit needs; no compiled matcher is
    # run as a runtime. IR emit + JSON render always run (a break here must error,
    # not skip).
    compiled = compile_file(TELEGRAM_MCH, telegram_nft_registries())
    artifact = emit_ir(compiled, [TELEGRAM_MCH.name])
    render_json(artifact)
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)

    art = _mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in _mch_ir.build_matchers(art, telegram_nft_registries())}
    ir_blocks = await _run_matcher(ir_by_name["telegram_nft_purchase"], trace)
    _assert_parity(ir_blocks, hw_blocks, "ir", "legacy")
    return hw_blocks


@pytest.mark.asyncio
@pytest.mark.trace_file("aMUaDa0Ml9lnk7KzHKpkW1UK6FPyYod2qotieVVWMd8=.lz4")
async def test_telegram_nft_fragment_purchase_parity(trace_data):
    trace, _ = trace_data
    hw = await _run_parity(trace)
    # Semantic pins on the fixture: fragment purchase with royalty + payout split.
    assert hw[0].data["is_purchase"] is True
    assert hw[0].data["marketplace"] == "fragment"
    assert hw[0].data["real_prev_owner"] is None
    assert "royalty_amount" in hw[0].data and "payout_amount" in hw[0].data
