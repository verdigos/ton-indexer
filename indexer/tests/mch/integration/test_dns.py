"""T14 — field-level parity between the legacy DNS matcher and specs/dns.mch.

Unlike test_jettons_family.py, this suite does NOT load recorded trace fixtures:
the entire tests/traces corpus contains zero messages carrying the ChangeDnsRecord
opcode (0x4eb1f0f9) — verified by scanning every fixture's raw message opcodes and
by running the legacy ChangeDnsRecordMatcher across all 147 traces (0 hits). No YAML
test case references change_dns / renew_dns / delete_dns either. Recording new traces
is out of scope for T14, so instead we construct minimal but faithful block trees the
same way the production pipeline does: real Transaction/Message DB models carrying a
genuine ChangeDnsRecord BOC body -> to_tree() -> init_block(). Both the legacy matcher
and the mch-compiled matcher run on two independently built trees; produced blocks are
compared field by field (btype, deep `data`, failed/broken, value_flow, consumed
event-node set), at the same depth as test_jettons_family.py.

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
BOUNCE_OPCODE = 0xffffffff
ZERO_KEY = b"\x00" * 32
NONZERO_KEY = bytes(range(1, 33))

SENDER = Address("0:" + "a1" * 32).to_str(False)
DNS_ITEM = Address("0:" + "b2" * 32).to_str(False)
COLLECTION = Address("0:" + "c3" * 32).to_str(False)
RESOLVER = Address("0:" + "d4" * 32)


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


def _interfaces() -> dict:
    return {
        DNS_ITEM: {
            "NftItem": {
                "address": DNS_ITEM, "init": True, "index": 0,
                "collection_address": COLLECTION, "owner_address": SENDER, "content": None,
            }
        }
    }


# --- harness (mirrors test_jettons_family.py) ---------------------------------

def _dsl_matcher():
    """Build the DSL matcher through the v2 IR engine (compile is a frontend-
    internal step ir_emit needs; no compiled matcher is run as a runtime)."""
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)
    compiled = compile_file(DNS_MCH, dns_registries())
    artifact = emit_ir(compiled, [DNS_MCH.name])
    art = _mch_ir.load_artifact(artifact)
    return list(_mch_ir.build_matchers(art, dns_registries()))[0]


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
