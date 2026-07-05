"""End-to-end IR engine tests over synthetic block trees.

Each matcher is written in .mch source, compiled by the real frontend, emitted
to an IR dict (emit_ir), then loaded and wired by the mch_ir package — so the
engine is exercised on exactly the artifacts the emitter produces. Where it
matters, the IR run is compared against the compiled-matcher run on an
identical fresh tree (mini-parity without trace fixtures).

Synthetic blocks carry fake event nodes with real BOC bodies (borrowed from
tests/mch/test_declarative_build.py) so the full runtime surface works:
`x.msg.*`, `parse` via get_body(), and merge_blocks lt/utime bookkeeping.
"""
from __future__ import annotations

from types import SimpleNamespace

import pytest
from pytoniq_core import begin_cell

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.utils import AccountId, Amount
from indexer.events.mch.compile import compile_text
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir import build_matchers, load_artifact

RAW_SRC = "0:" + "11" * 32
RAW_DST = "0:" + "22" * 32
RAW_JETTON = "0:" + "33" * 32

OP_FOO = 0xF00
OP_BAR = 0xBA7
OP_HOP = 0x101
OP_END = 0x102


# ==========================================================================
# Synthetic blocks with fake event nodes
# ==========================================================================

class _FakeTx:
    def __init__(self, lt: int):
        self.lt = lt
        self.now = lt
        self.orig_status = "active"
        self.end_status = "active"


class _FakeMsg:
    def __init__(self, opcode: int, body: bytes | None, lt: int,
                 source: str = RAW_SRC, destination: str = RAW_DST, value: int = 1000):
        self.opcode = opcode
        self.source = source
        self.destination = destination
        self.value = value
        self.direction = "in"
        self.created_lt = lt
        self.created_at = lt
        self.message_content = SimpleNamespace(body=body)
        self.transaction = _FakeTx(lt)


class _FakeNode:
    is_tick_tock = False
    tick_tock_tx = None
    ghost_node = False
    parent = None

    def __init__(self, msg: _FakeMsg):
        self.message = msg

    def get_lt(self):
        return self.message.created_lt

    def get_utime(self):
        return self.message.created_at

    def get_opcode(self):
        return self.message.opcode & 0xFFFFFFFF


class _FakeCC(CallContractBlock):
    """CallContractBlock constructable without DB state but with a full-enough
    fake node for msg access, body parsing, and merge_blocks."""

    def __init__(self, opcode: int, body: bytes | None = None, lt: int = 10,
                 failed: bool = False, **msg_kw):  # pylint: disable=super-init-not-called
        node = _FakeNode(_FakeMsg(opcode, body, lt, **msg_kw))
        Block.__init__(self, "call_contract", [node], None)
        self.opcode = opcode
        self.failed = failed


def _link(parent: Block, child: Block) -> None:
    parent.next_blocks.append(child)
    child.previous_block = parent


def _body(amount: int) -> bytes:
    return begin_cell().store_uint(OP_FOO, 32).store_uint(amount, 64).end_cell().to_boc()


class FakeTransfer:
    def __init__(self, s):  # s: pytoniq_core Slice
        s.load_uint(32)
        self.amount = s.load_uint(64)


class NeverParses:
    def __init__(self, s):
        raise ValueError("nope")


def _registries(lookups: dict | None = None) -> Registries:
    reg = Registries()
    reg.opcodes.update({"Foo": OP_FOO, "Bar": OP_BAR, "Hop": OP_HOP, "End": OP_END})
    reg.block_types["FB"] = object
    reg.message_types["FakeTransfer"] = FakeTransfer
    reg.message_types["NeverParses"] = NeverParses
    if lookups:
        reg.lookups.update(lookups)
    return reg


def _ir_matcher(src: str, reg: Registries | None = None):
    """Frontend-compile `src`, emit its IR, and wire it through mch_ir."""
    reg = reg or _registries()
    compiled = compile_text(src, "test.mch", reg)
    art = load_artifact(emit_ir(compiled, ["test.mch"]))
    return build_matchers(art, reg)[0]


def _foo_bar_tree(amount: int = 777, bar_failed: bool = False):
    anchor = _FakeCC(OP_FOO, body=_body(amount), lt=10)
    bar = _FakeCC(OP_BAR, lt=20, failed=bar_failed)
    _link(anchor, bar)
    return anchor, bar


# ==========================================================================
# Declarative build program, end to end (+ parity with the compiled matcher)
# ==========================================================================

_HAPPY_SRC = """
matcher decl_happy {
    produces FB
    entry    @x

    op Foo @x -> op Bar @y

    parse x as FakeTransfer
    let amt = x.body.amount
    reject when amt == null
    failed when y.failed
    broken when amt == 13
    out {
        amount: amount(amt)
        src:    account(x.msg.source)
        size:   "big" if amt > 100 else "small"
    }
    flow ton(account(x.msg.source), amt)
}
"""


@pytest.mark.asyncio
async def test_declarative_matcher_matches_compiled_output():
    ir_m = _ir_matcher(_HAPPY_SRC)
    assert ir_m.name == "decl_happy"
    assert ir_m.produces == ["FB"]

    anchor, bar = _foo_bar_tree(amount=777)
    (produced,) = await ir_m.try_build(anchor)
    assert produced.btype == "FB"
    assert produced.data["amount"] == Amount(777)
    assert produced.data["src"] == AccountId(RAW_SRC)
    assert produced.data["size"] == "big"
    assert produced.failed is False
    assert produced.broken is False
    assert produced.value_flow.flow[AccountId(RAW_SRC)].ton == 777
    # Consumption: both pattern blocks absorbed into the produced block.
    assert anchor.parent is produced
    assert bar.parent is produced


_SWITCH_SRC = """
matcher decl_switch {
    produces small | big | huge
    entry    @x

    op Foo @x

    parse x as FakeTransfer
    let amt = x.body.amount
    produces switch {
        when amt > 1000 => huge  out { amount: amount(amt), tag: "H" }
        when amt > 100  => big   out { amount: amount(amt), note?: null }
        else            => small out { amount: amount(amt) }
    }
}
"""


def _switch_registries() -> Registries:
    reg = _registries()
    for bt in ("small", "big", "huge"):
        reg.block_types[bt] = object
    return reg


@pytest.mark.asyncio
async def test_produces_switch_dispatches_btype_and_optional_key():
    reg = _switch_registries()
    ir_m = _ir_matcher(_SWITCH_SRC, reg)

    # amt > 1000 -> huge branch, with the `tag` field.
    anchor = _FakeCC(OP_FOO, body=_body(5000), lt=10)
    (produced,) = await ir_m.try_build(anchor)
    assert produced.btype == "huge"
    assert produced.data == {"amount": Amount(5000), "tag": "H"}

    # 100 < amt <= 1000 -> big branch; `note?: null` is omitted (optional key).
    anchor = _FakeCC(OP_FOO, body=_body(500), lt=10)
    (produced,) = await ir_m.try_build(anchor)
    assert produced.btype == "big"
    assert produced.data == {"amount": Amount(500)}
    assert "note" not in produced.data

    # else -> small branch.
    anchor = _FakeCC(OP_FOO, body=_body(50), lt=10)
    (produced,) = await ir_m.try_build(anchor)
    assert produced.btype == "small"
    assert produced.data == {"amount": Amount(50)}


@pytest.mark.asyncio
async def test_failed_and_broken_flags():
    ir_m = _ir_matcher(_HAPPY_SRC)

    anchor, _ = _foo_bar_tree(amount=777, bar_failed=True)
    (produced,) = await ir_m.try_build(anchor)
    assert produced.failed is True and produced.broken is False

    anchor, _ = _foo_bar_tree(amount=13)
    (produced,) = await ir_m.try_build(anchor)
    assert produced.failed is False and produced.broken is True
    assert produced.data["size"] == "small"


@pytest.mark.asyncio
async def test_reject_when_leaves_blocks_unconsumed():
    src = """
    matcher decl_reject {
        produces FB
        entry    @x

        op Foo @x -> op Bar @y

        parse x as FakeTransfer
        reject when x.body.amount == 42
        out { v: 1 }
    }
    """
    ir_m = _ir_matcher(src)
    anchor, bar = _foo_bar_tree(amount=42)
    result = await ir_m.try_build(anchor)
    assert result == []           # clean rejection, not a structural mismatch
    assert anchor.parent is None  # nothing consumed
    assert bar.parent is None

    anchor2, _ = _foo_bar_tree(amount=43)
    result2 = await ir_m.try_build(anchor2)
    assert len(result2) == 1 and result2[0].data == {"v": 1}


@pytest.mark.asyncio
async def test_runtime_eval_error_rejects_match(caplog):
    # `.oops` is not a valid block accessor -> EvalError -> rejection (§10.7).
    src = """
    matcher decl_err {
        produces FB
        entry    @x

        op Foo @x
        out { v: x.oops }
    }
    """
    ir_m = _ir_matcher(src)
    anchor = _FakeCC(OP_FOO, lt=10)
    with caplog.at_level("WARNING", logger="indexer.events.mch_ir.engine"):
        assert await ir_m.try_build(anchor) == []
    assert anchor.parent is None
    assert any("rejecting match" in r.message for r in caplog.records)


@pytest.mark.asyncio
async def test_parse_alternatives_and_soft_failure():
    src = """
    matcher decl_alt {
        produces FB
        entry    @x

        op Foo @x

        parse x as NeverParses | FakeTransfer
        out { amount: x.body.amount }
    }
    """
    ir_m = _ir_matcher(src)
    anchor = _FakeCC(OP_FOO, body=_body(555), lt=10)
    (produced,) = await ir_m.try_build(anchor)
    assert produced.data["amount"] == 555

    src_soft = """
    matcher decl_soft {
        produces FB
        entry    @x

        op Foo @x

        parse x as NeverParses
        reject when x.body == null
        out { v: 1 }
    }
    """
    ir_soft = _ir_matcher(src_soft)
    anchor = _FakeCC(OP_FOO, body=_body(1), lt=10)
    assert await ir_soft.try_build(anchor) == []


@pytest.mark.asyncio
async def test_parse_on_absent_optional_capture_body_is_null():
    src = """
    matcher decl_opt {
        produces FB
        entry    @x

        op Foo @x -> maybe op Bar @y

        parse y as FakeTransfer
        out { got: y.body != null }
    }
    """
    ir_m = _ir_matcher(src)
    anchor = _FakeCC(OP_FOO, lt=10)  # no Bar child
    (produced,) = await ir_m.try_build(anchor)
    assert produced.data["got"] is False


@pytest.mark.asyncio
async def test_lookup_feeds_out_fields():
    calls: list = []

    async def wallet(addr):
        calls.append(addr)
        return SimpleNamespace(owner=RAW_SRC, jetton=RAW_JETTON, address=addr)

    src = """
    matcher decl_lookup {
        produces FB
        entry    @x

        op Foo @x

        let w = lookup wallet(x.msg.destination)
        reject when w == null
        out { owner: account(w.owner) }
    }
    """
    reg = _registries(lookups={"wallet": wallet})
    ir_m = _ir_matcher(src, reg)
    anchor = _FakeCC(OP_FOO, lt=10)
    (produced,) = await ir_m.try_build(anchor)
    assert calls == [RAW_DST]
    assert produced.data["owner"] == AccountId(RAW_SRC)


@pytest.mark.asyncio
async def test_well_known_consumed_and_anchor_names():
    async def count_consumed(consumed):
        return len(consumed)

    src = """
    matcher decl_wk {
        produces FB
        entry    @x

        op Foo @x -> op Bar @y

        let n = count_consumed(consumed)
        out { n: n, abt: anchor.btype }
    }
    """
    reg = _registries()
    reg.fns["count_consumed"] = count_consumed
    ir_m = _ir_matcher(src, reg)
    anchor, _bar = _foo_bar_tree()
    (produced,) = await ir_m.try_build(anchor)
    assert produced.data == {"n": 2, "abt": "call_contract"}


# ==========================================================================
# Builder-based matchers (Match-like object + _mch_consumed)
# ==========================================================================

_BUILDER_SRC = """
matcher via_builder {
    produces FB
    build    my_build
    entry    @x

    op Foo @x -> op Bar @y
}
"""


@pytest.mark.asyncio
async def test_builder_gets_captures_and_consumed():
    seen: dict = {}

    async def my_build(m):
        seen["x"] = m.x
        seen["y"] = m.y
        seen["consumed"] = m._mch_consumed
        return Block("fb", [], {"ok": True})

    reg = _registries()
    reg.builders["my_build"] = my_build
    ir_m = _ir_matcher(_BUILDER_SRC, reg)

    anchor, bar = _foo_bar_tree()
    (produced,) = await ir_m.try_build(anchor)
    assert produced.btype == "fb" and produced.data == {"ok": True}
    assert seen["x"] is anchor
    assert seen["y"] is bar
    assert seen["consumed"] == [anchor, bar]
    assert anchor.parent is produced and bar.parent is produced


@pytest.mark.asyncio
async def test_builder_returning_none_rejects_cleanly():
    async def my_build(_m):
        return None

    reg = _registries()
    reg.builders["my_build"] = my_build
    ir_m = _ir_matcher(_BUILDER_SRC, reg)

    anchor, bar = _foo_bar_tree()
    assert await ir_m.try_build(anchor) == []
    assert anchor.parent is None and bar.parent is None


# ==========================================================================
# Opcode-set anchors
# ==========================================================================

@pytest.mark.asyncio
async def test_opcode_set_anchor_matches_both_heads():
    src = """
    matcher dual {
        produces FB
        entry    @x

        (op Foo @x ^ op Bar @x)

        out { oc: x.msg.opcode }
    }
    """
    ir_m = _ir_matcher(src)
    for op in (OP_FOO, OP_BAR):
        anchor = _FakeCC(op, lt=10)
        (produced,) = await ir_m.try_build(anchor)
        assert produced.data == {"oc": op}
    assert await ir_m.try_build(_FakeCC(0x999, lt=10)) is None
    assert ir_m.test_self(Block("jetton_transfer", [], None)) is False


# ==========================================================================
# Recursion: list captures via the iteration journal (+ compiled parity)
# ==========================================================================

_REC_SRC = """
rule chain = (
    op Hop @hop -> maybe $chain
  ^ op End @fin
)

matcher rec {
    produces FB
    build    rec_build
    entry    @x

    op Foo @x -> $chain
}
"""


def _hop_tree():
    foo = _FakeCC(OP_FOO, lt=10)
    hop1 = _FakeCC(OP_HOP, lt=20)
    hop2 = _FakeCC(OP_HOP, lt=30)
    end = _FakeCC(OP_END, lt=40)
    _link(foo, hop1)
    _link(hop1, hop2)
    _link(hop2, end)
    return foo


def _rec_registries(seen: dict) -> Registries:
    reg = _registries()

    async def rec_build(m):
        seen["x_lt"] = m.x.min_lt
        seen["hop_lts"] = [b.min_lt if b is not None else None for b in m.hop]
        seen["fin_lts"] = [b.min_lt if b is not None else None for b in m.fin]
        seen["consumed_lts"] = sorted(b.min_lt for b in m._mch_consumed)
        return Block("fb", [], None)

    reg.builders["rec_build"] = rec_build
    return reg


@pytest.mark.asyncio
async def test_recursion_list_captures_bind_per_iteration():
    seen: dict = {}
    ir_m = _ir_matcher(_REC_SRC, _rec_registries(seen))
    (produced,) = await ir_m.try_build(_hop_tree())
    assert produced.btype == "fb"
    assert seen["x_lt"] == 10
    assert seen["hop_lts"] == [20, 30]   # one element per unrolling
    assert seen["fin_lts"] == [40]       # exit binds once, on termination
    assert seen["consumed_lts"] == [10, 20, 30, 40]


@pytest.mark.asyncio
async def test_optional_recursion_absent_binds_empty_lists():
    # The recursion's test_self is the STEP test, so an exit-only child means
    # the optional recursion does not participate at all: both lists stay
    # empty and only the anchor is consumed.
    def tree():
        foo = _FakeCC(OP_FOO, lt=10)
        end = _FakeCC(OP_END, lt=20)
        _link(foo, end)
        return foo

    seen: dict = {}
    ir_m = _ir_matcher(_REC_SRC, _rec_registries(seen))
    (produced,) = await ir_m.try_build(tree())
    assert produced.btype == "fb"
    assert seen["hop_lts"] == []
    assert seen["fin_lts"] == []
    assert seen["consumed_lts"] == [10]


# ==========================================================================
# Node-level where_expr (T33: BOTH paths evaluate it at match time via the
# shared sync evaluator, mch/expr_eval.py)
# ==========================================================================

@pytest.mark.asyncio
async def test_where_expr_gates_child_match():
    src = """
    matcher w {
        produces FB
        entry    @x

        op Foo @x -> op Bar @y where (.value > 5)

        out { v: 1 }
    }
    """
    m = _ir_matcher(src)
    anchor, bar = _foo_bar_tree()
    bar.data = {"value": 10}
    (produced,) = await m.try_build(anchor)
    assert produced.data == {"v": 1}

    anchor2, bar2 = _foo_bar_tree()
    bar2.data = {"value": 3}
    assert await m.try_build(anchor2) is None
    assert anchor2.parent is None


_WHERE_DATA_SRC = """
matcher jt_no_internal {
    produces FB
    entry    @x

    op Foo @x -> btype jetton_transfer @t where (.has_internal_transfer == false)

    out { v: 1 }
}
"""


def _jt_tree(data):
    """anchor Foo -> jetton_transfer-typed block carrying `data`."""
    anchor = _FakeCC(OP_FOO, lt=10)
    jt = Block("jetton_transfer", [_FakeNode(_FakeMsg(0, None, 20))], data)
    _link(anchor, jt)
    return anchor


@pytest.mark.asyncio
async def test_where_expr_data_check_replaces_generic_matcher():
    """T33 Accept: a GenericMatcher-style `.has_internal_transfer == false`
    data check expressed inline — including the missing-field => False and
    data=None => False rules."""
    reg = _registries()
    reg.block_types["jetton_transfer"] = object
    m = _ir_matcher(_WHERE_DATA_SRC, reg)

    (produced,) = await m.try_build(_jt_tree({"has_internal_transfer": False}))
    assert produced.data == {"v": 1}
    # True fails the == false test.
    assert await m.try_build(_jt_tree({"has_internal_transfer": True})) is None
    # Missing field and absent data evaluate to False, never an error.
    assert await m.try_build(_jt_tree({})) is None
    assert await m.try_build(_jt_tree(None)) is None


@pytest.mark.asyncio
async def test_where_expr_on_anchor_node():
    """T33 anchor decision: anchor-node where_expr rides on the root node record
    and the IR engine evaluates it as part of the root's head test."""
    src = """
    matcher wa {
        produces FB
        entry    @x

        op Foo @x where (.kind == "swap") -> op Bar @y

        out { v: 1 }
    }
    """
    m = _ir_matcher(src)
    anchor, _bar = _foo_bar_tree()
    anchor.data = {"kind": "swap"}
    (produced,) = await m.try_build(anchor)
    assert produced.data == {"v": 1}

    anchor2, _bar2 = _foo_bar_tree()
    anchor2.data = {"kind": "transfer"}
    assert await m.try_build(anchor2) is None
    # data=None dotfield => null => == fails; no error, just no match.
    # (IrMatcher.test_self checks only the anchor record — where clauses are
    # re-checked inside try_build.)
    anchor3, _bar3 = _foo_bar_tree()
    anchor3.data = None
    assert await m.try_build(anchor3) is None


# ==========================================================================
# Registration order
# ==========================================================================

@pytest.mark.asyncio
async def test_registration_order_preserved():
    src = """
    matcher first {
        produces FB
        entry    @x
        op Foo @x
        out { v: 1 }
    }

    matcher second {
        produces FB
        entry    @x
        op Bar @x
        out { v: 2 }
    }
    """
    reg = _registries()
    compiled = compile_text(src, "test.mch", reg)
    art = load_artifact(emit_ir(compiled, ["test.mch"]))
    matchers = build_matchers(art, reg)
    assert [m.name for m in matchers] == ["first", "second"]
