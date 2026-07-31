"""Synthetic field and branch coverage for specs/dns.mch.

Minimal real Transaction/Message DB models carrying genuine BOC bodies are lowered
through to_tree() -> init_block(), matching the production pipeline. Recorded action
coverage for the auction tail lives in test_cases/dns.yaml; these synthetic trees pin
the OR guard, lookup misses, the purchase parent's non-consumption, and both legal
conditional-query-id lengths.

Coverage (one synthetic fixture per outcome + a bounce variant + a failed variant):
- change_dns  : has_value=True  (DNSNextResolver value cell)
- renew_dns   : has_value=False, key == zero_key
- delete_dns  : has_value=False, key != zero_key
- change_dns + trailing bounce (op 0xffffffff)  -> bounce consumed into the block
- change_dns + tx aborted -> failed flag propagates
"""
from __future__ import annotations

from pathlib import Path

import pytest
from pytoniq_core import Address, begin_cell

from indexer.core.database import Message, MessageContent, Transaction
from indexer.events import context
from indexer.events.blocks.core import Block
from indexer.events.blocks.dns import ChangeDnsRecordMatcher
from indexer.events.blocks.messages.nft import AuctionFillUp, DnsReleaseBalance
from indexer.events.blocks.utils import AccountId
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders import dns_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository

DNS_MCH = Path(specs_pkg.__file__).parent / "dns.mch"

# --- IR engine gate (T29: the DSL leg is the v2 IR engine, not the compiled path)
try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
except Exception:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False

_SKIP_REASON = "indexer.events.mch_ir not importable; the IR engine is the DSL runtime"

DNS_OPCODE = 0x4eb1f0f9
AUCTION_FILL_UP_OPCODE = 0x370fec51
DNS_RELEASE_OPCODE = 0x4ed14b65
PARENT_OPCODE = 0x12345678
BOUNCE_OPCODE = 0xffffffff
ZERO_KEY = b"\x00" * 32
NONZERO_KEY = bytes(range(1, 33))

SENDER = Address("0:" + "a1" * 32).to_str(False)
DNS_ITEM = Address("0:" + "b2" * 32).to_str(False)
COLLECTION = Address("0:" + "c3" * 32).to_str(False)
RESOLVER = Address("0:" + "d4" * 32)
BUYER = Address("0:" + "e5" * 32).to_str(False)
AUCTION = Address("0:" + "f6" * 32).to_str(False)
DNS_COLLECTION = "0:B774D95EB20543F186C06B371AB88AD704F7E256130CAF96189368A7D0CB6CCF"
DNS_CODE_HASH = "i1/8nr/TkGTY1fVuRlnIJrt1k5I/XKSHKL5NYK9vUfk="


# --- synthetic DNS BOC bodies -------------------------------------------------

def _change_body() -> bytes:
    value_cell = begin_cell().store_uint(0xba93, 16).store_address(RESOLVER).end_cell()
    return (begin_cell().store_uint(DNS_OPCODE, 32).store_uint(7, 64)
            .store_bytes(NONZERO_KEY).store_ref(value_cell).end_cell()).to_boc()


def _renew_body() -> bytes:
    return (begin_cell().store_uint(DNS_OPCODE, 32).store_uint(7, 64)
            .store_bytes(ZERO_KEY).end_cell()).to_boc()


def _delete_body() -> bytes:
    return (begin_cell().store_uint(DNS_OPCODE, 32).store_uint(7, 64)
            .store_bytes(NONZERO_KEY).end_cell()).to_boc()


_BODY_FOR = {"change_dns": _change_body, "renew_dns": _renew_body, "delete_dns": _delete_body}


def _query_body(opcode: int, query_id: int | None = 7) -> bytes:
    b = begin_cell().store_uint(opcode, 32)
    if query_id is not None:
        b.store_uint(query_id, 64)
    return b.end_cell().to_boc()


# --- synthetic trace construction (mirrors the real pipeline path) ------------

def _tx(hash_: str, lt: int, account: str, aborted: bool = False) -> Transaction:
    tx = Transaction(
        hash=hash_, lt=lt, account=account, now=1000 + lt, total_fees=0,
        orig_status="active", end_status="active", descr="ord",
        aborted=aborted, emulated=False,
    )
    tx.messages = []
    return tx


def _msg(msg_hash, tx, direction, opcode, src, dst, body_boc, value=10 ** 9, bounced=False) -> Message:
    m = Message(
        msg_hash=msg_hash, tx_hash=tx.hash, tx_lt=tx.lt, direction=direction,
        source=src, destination=dst, value=value, fwd_fee=0, ihr_fee=0,
        created_lt=tx.lt, created_at=tx.now, opcode=opcode, bounce=False,
        bounced=bounced, value_extra_currencies=None,
    )
    m.message_content = MessageContent(hash="c", body=body_boc)
    m.transaction = tx
    return m


def _build_dns_transactions(outcome: str, with_bounce: bool, aborted: bool) -> list[Transaction]:
    """A sender wallet tx (external-in) forwarding a ChangeDnsRecord to the DNS item
    tx; optionally the item bounces (op 0xffffffff) back to the sender."""
    body = _BODY_FOR[outcome]()

    root_tx = _tx("TXROOT", 100, SENDER)
    dns_tx = _tx("TXDNS", 200, DNS_ITEM, aborted=aborted)

    root_tx.messages = [
        _msg("EXT", root_tx, "in", None, None, SENDER, _renew_body(), value=None),
        _msg("MDNS", root_tx, "out", DNS_OPCODE, SENDER, DNS_ITEM, body),
    ]
    dns_in = _msg("MDNS", dns_tx, "in", DNS_OPCODE, SENDER, DNS_ITEM, body)
    dns_tx.messages = [dns_in]

    txs = [root_tx, dns_tx]

    if with_bounce:
        bounce_tx = _tx("TXBOUNCE", 300, SENDER)
        dns_tx.messages = [
            dns_in,
            _msg("MB", dns_tx, "out", BOUNCE_OPCODE, DNS_ITEM, SENDER, _renew_body(), value=0, bounced=True),
        ]
        bounce_tx.messages = [
            _msg("MB", bounce_tx, "in", BOUNCE_OPCODE, DNS_ITEM, SENDER, _renew_body(), value=0, bounced=True),
        ]
        txs.append(bounce_tx)

    return txs


def _build_auction_transactions(matcher_name: str) -> list[Transaction]:
    if matcher_name == "dns_release":
        tx = _tx("TXANCHOR", 400, DNS_ITEM)
        tx.messages = [_msg("RELEASE", tx, "in", DNS_RELEASE_OPCODE, SENDER,
                            DNS_ITEM, _query_body(DNS_RELEASE_OPCODE), value=123)]
        return [tx]

    parent_tx = _tx("TXPARENT", 400, DNS_ITEM)
    anchor_tx = _tx("TXANCHOR", 500, AUCTION)
    parent = _msg("PARENT", parent_tx, "in", PARENT_OPCODE, BUYER, DNS_ITEM,
                  _query_body(PARENT_OPCODE), value=456)
    anchor_out = _msg("ANCHOR", parent_tx, "out", AUCTION_FILL_UP_OPCODE,
                      DNS_ITEM, AUCTION, _query_body(AUCTION_FILL_UP_OPCODE), value=123)
    anchor_in = _msg("ANCHOR", anchor_tx, "in", AUCTION_FILL_UP_OPCODE,
                     DNS_ITEM, AUCTION, _query_body(AUCTION_FILL_UP_OPCODE), value=123)
    parent_tx.messages = [parent, anchor_out]
    anchor_tx.messages = [anchor_in]
    return [parent_tx, anchor_tx]


def _interfaces() -> dict:
    return {
        DNS_ITEM: {
            "NftItem": {
                "address": DNS_ITEM, "init": True, "index": 0,
                "collection_address": COLLECTION, "owner_address": SENDER, "content": None,
            }
        }
    }


def _auction_interfaces(*, code_hash: str, collection: str) -> dict:
    return {DNS_ITEM: {"NftItem": {
        "address": DNS_ITEM, "init": True, "index": 42.0,
        "collection_address": collection, "owner_address": SENDER,
        "content": None, "code_hash": code_hash,
    }}}


# --- harness (mirrors test_jettons_family.py) ---------------------------------

def _dsl_matchers():
    """Build the DSL matcher through the v2 IR engine (compile is a frontend-
    internal step ir_emit needs; no compiled matcher is run as a runtime)."""
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)
    compiled = compile_file(DNS_MCH, dns_registries())
    artifact = emit_ir(compiled, [DNS_MCH.name])
    art = _mch_ir.load_artifact(artifact)
    return {m.name: m for m in _mch_ir.build_matchers(art, dns_registries())}


def _dsl_matcher():
    return _dsl_matchers()["change_dns"]


def _build_block_tree(txs) -> Block:
    node = to_tree(txs)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher, txs) -> list:
    root = _build_block_tree(txs)
    matches: list = []
    for b in root.bfs_iter():
        if b.btype == "root":
            continue
        if b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return matches


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


def _assert_field_parity(dsl_blocks, hw_blocks, btype: str) -> None:
    assert len(hw_blocks) > 0, f"legacy matcher produced no {btype} blocks - bad fixture"
    assert len(dsl_blocks) == len(hw_blocks), (
        f"count mismatch: dsl={len(dsl_blocks)} legacy={len(hw_blocks)}"
    )
    for d, h in zip(sorted(dsl_blocks, key=_sort_key), sorted(hw_blocks, key=_sort_key)):
        assert d.btype == h.btype == btype
        assert d.failed == h.failed, f"failed flag: dsl={d.failed} legacy={h.failed}"
        assert d.broken == h.broken, f"broken flag: dsl={d.broken} legacy={h.broken}"

        assert set(d.data.keys()) == set(h.data.keys()), (
            f"data keys: dsl={sorted(d.data)} legacy={sorted(h.data)}"
        )
        for key in sorted(h.data):
            dv, hv = d.data[key], h.data[key]
            assert _values_equal(dv, hv), (
                f"data[{key!r}]: dsl={dv!r} ({type(dv).__name__}) "
                f"legacy={hv!r} ({type(hv).__name__})"
            )

        assert d.value_flow.to_dict() == h.value_flow.to_dict(), (
            f"value_flow: dsl={d.value_flow.to_dict()} legacy={h.value_flow.to_dict()}"
        )
        assert _node_hashes(d) == _node_hashes(h), (
            f"consumed event nodes: dsl={_node_hashes(d)} legacy={_node_hashes(h)}"
        )


async def _run_parity(outcome: str, *, with_bounce: bool = False, aborted: bool = False):
    context.interface_repository.set(TestInterfaceRepository(_interfaces()))
    hw_results = await _run_matcher(ChangeDnsRecordMatcher(), _build_dns_transactions(outcome, with_bounce, aborted))
    context.interface_repository.set(TestInterfaceRepository(_interfaces()))
    dsl_results = await _run_matcher(_dsl_matcher(), _build_dns_transactions(outcome, with_bounce, aborted))

    hw_blocks = [b for b in hw_results if b.btype == outcome]
    dsl_blocks = [b for b in dsl_results if b.btype == outcome]
    _assert_field_parity(dsl_blocks, hw_blocks, outcome)
    return hw_blocks, dsl_blocks


@pytest.mark.asyncio
async def test_change_dns_parity():
    await _run_parity("change_dns")


@pytest.mark.asyncio
async def test_renew_dns_parity():
    await _run_parity("renew_dns")


@pytest.mark.asyncio
async def test_delete_dns_parity():
    await _run_parity("delete_dns")


@pytest.mark.asyncio
async def test_change_dns_parity_with_bounce():
    """The optional trailing bounce (op 0xffffffff) is consumed identically by both
    matchers — pins the excess/bounce directive decision (defaults + explicit child)."""
    hw_blocks, dsl_blocks = await _run_parity("change_dns", with_bounce=True)
    assert "TXBOUNCE" in _node_hashes(hw_blocks[0]), "legacy did not consume the bounce"
    assert "TXBOUNCE" in _node_hashes(dsl_blocks[0]), "dsl did not consume the bounce"


@pytest.mark.asyncio
async def test_change_dns_parity_failed():
    """Aborted DNS tx - exercises the failed-flag path of the shared build core."""
    hw_blocks, _ = await _run_parity("change_dns", with_bounce=True, aborted=True)
    assert hw_blocks[0].failed is True


@pytest.mark.asyncio
async def test_dns_purchase_reads_but_does_not_consume_parent():
    context.interface_repository.set(TestInterfaceRepository(
        _auction_interfaces(code_hash=DNS_CODE_HASH, collection=DNS_COLLECTION)))
    root = _build_block_tree(_build_auction_transactions("dns_purchase"))
    anchor = next(
        b for b in root.bfs_iter()
        if getattr(b, "opcode", None) == AUCTION_FILL_UP_OPCODE
    )
    parent = anchor.previous_block
    result = await _dsl_matchers()["dns_purchase"].try_build(anchor)
    product = next(b for b in result if b.btype == "dns_purchase")
    assert _node_hashes(product) == ["TXANCHOR"]
    assert product.data["new_owner"] == AccountId(BUYER)
    assert product.previous_block is parent
    assert parent.btype == "call_contract" and parent.parent is None
    assert _node_hashes(parent) == ["TXPARENT"]


@pytest.mark.asyncio
@pytest.mark.parametrize("matcher_name", ["dns_purchase", "dns_release"])
@pytest.mark.parametrize(
    ("code_hash", "collection"),
    [(DNS_CODE_HASH, COLLECTION), ("different", DNS_COLLECTION)],
    ids=["code-hash-only", "collection-only"],
)
async def test_dns_auction_guard_is_or(matcher_name, code_hash, collection):
    context.interface_repository.set(TestInterfaceRepository(
        _auction_interfaces(code_hash=code_hash, collection=collection)))
    blocks = await _run_matcher(
        _dsl_matchers()[matcher_name], _build_auction_transactions(matcher_name))
    assert [b.btype for b in blocks] == [matcher_name]


@pytest.mark.asyncio
@pytest.mark.parametrize("matcher_name", ["dns_purchase", "dns_release"])
async def test_dns_auction_lookup_miss_rejects(matcher_name):
    context.interface_repository.set(TestInterfaceRepository({}))
    opcode = (AUCTION_FILL_UP_OPCODE if matcher_name == "dns_purchase"
              else DNS_RELEASE_OPCODE)
    root = _build_block_tree(_build_auction_transactions(matcher_name))
    anchor = next(b for b in root.bfs_iter() if getattr(b, "opcode", None) == opcode)
    assert not await _dsl_matchers()[matcher_name].try_build(anchor)
    assert anchor.btype == "call_contract" and anchor.parent is None


@pytest.mark.parametrize(
    ("message_type", "opcode"),
    [(AuctionFillUp, AUCTION_FILL_UP_OPCODE), (DnsReleaseBalance, DNS_RELEASE_OPCODE)],
)
@pytest.mark.parametrize("query_id", [None, 0x123456789ABCDEF0])
def test_dns_auction_query_id_lengths(message_type, opcode, query_id):
    body = begin_cell().store_uint(opcode, 32)
    if query_id is not None:
        body.store_uint(query_id, 64)
    parsed = message_type(body.end_cell().begin_parse())
    assert parsed.query_id == query_id
