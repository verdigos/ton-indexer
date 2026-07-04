"""End-to-end declarative build path (M4 §9): a matcher with build statements
and no `build` directive, compiled and run over synthetic block trees.

Covers: parse (incl. alternatives + soft failure), let/lookup, reject,
failed/broken flags, out data, flow effects, runtime-error rejection (§10.7),
and the new resolver diagnostics R011-R018.

Synthetic blocks here carry fake event nodes with real BOC bodies so the full
runtime surface works: `x.msg.*` (message envelope), `parse` via get_body(),
and merge_blocks (which needs lt/utime bookkeeping on the nodes).
"""
from __future__ import annotations

from types import SimpleNamespace

import pytest

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.utils import AccountId, Amount, Asset
from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import resolve
from pytoniq_core import begin_cell

RAW_SRC = "0:" + "11" * 32
RAW_DST = "0:" + "22" * 32
RAW_JETTON = "0:" + "33" * 32

OP_FOO = 0xF00
OP_BAR = 0xBA7


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


# ==========================================================================
# Fake message parsers
# ==========================================================================

class FakeTransfer:
    def __init__(self, s):  # s: pytoniq_core Slice
        s.load_uint(32)
        self.amount = s.load_uint(64)


class NeverParses:
    def __init__(self, s):
        raise ValueError("nope")


def _registries(lookups: dict | None = None) -> Registries:
    reg = Registries()
    reg.opcodes.update({"Foo": OP_FOO, "Bar": OP_BAR})
    reg.block_types["FB"] = object
    reg.message_types["FakeTransfer"] = FakeTransfer
    reg.message_types["NeverParses"] = NeverParses
    if lookups:
        reg.lookups.update(lookups)
    return reg


def _compile(src: str, reg: Registries | None = None):
    reg = reg or _registries()
    return compile_text(src, "test.mch", reg)[0].matcher


def _foo_bar_tree(amount: int = 777, bar_failed: bool = False):
    anchor = _FakeCC(OP_FOO, body=_body(amount), lt=10)
    bar = _FakeCC(OP_BAR, lt=20, failed=bar_failed)
    _link(anchor, bar)
    return anchor, bar


# ==========================================================================
# Happy path
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
async def test_declarative_matcher_produces_block_with_data():
    matcher = _compile(_HAPPY_SRC)
    anchor, bar = _foo_bar_tree(amount=777)

    result = await matcher.try_build(anchor)
    assert result is not None and len(result) == 1
    produced = result[0]
    assert produced.btype == "FB"
    assert produced.data["amount"] == Amount(777)
    assert produced.data["src"] == AccountId(RAW_SRC)
    assert produced.data["size"] == "big"
    assert produced.failed is False
    assert produced.broken is False
    # Consumption: both pattern blocks absorbed into the produced block.
    assert anchor.parent is produced
    assert bar.parent is produced


@pytest.mark.asyncio
async def test_flow_effect_lands_on_value_flow():
    matcher = _compile(_HAPPY_SRC)
    anchor, _bar = _foo_bar_tree(amount=777)

    (produced,) = await matcher.try_build(anchor)
    assert produced.value_flow.flow[AccountId(RAW_SRC)].ton == 777


@pytest.mark.asyncio
async def test_failed_and_broken_flags():
    matcher = _compile(_HAPPY_SRC)

    anchor, _ = _foo_bar_tree(amount=777, bar_failed=True)
    (produced,) = await matcher.try_build(anchor)
    assert produced.failed is True
    assert produced.broken is False

    anchor, _ = _foo_bar_tree(amount=13)
    (produced,) = await matcher.try_build(anchor)
    assert produced.failed is False
    assert produced.broken is True
    assert produced.data["size"] == "small"


@pytest.mark.asyncio
async def test_produces_class_name_is_normalized_to_btype():
    src = """
    matcher decl_norm {
        produces JettonTransferBlock
        entry    @x

        op Foo @x
        out { v: 1 }
    }
    """
    reg = _registries()
    reg.block_types["JettonTransferBlock"] = object
    matcher = _compile(src, reg)
    anchor = _FakeCC(OP_FOO, lt=10)
    (produced,) = await matcher.try_build(anchor)
    assert produced.btype == "jetton_transfer"


# ==========================================================================
# Reject and runtime-error paths
# ==========================================================================

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
    matcher = _compile(src)
    anchor, bar = _foo_bar_tree(amount=42)

    result = await matcher.try_build(anchor)
    assert result == []          # clean rejection, not a structural mismatch
    assert anchor.parent is None  # nothing consumed
    assert bar.parent is None

    # Non-matching reject condition still builds.
    anchor2, _ = _foo_bar_tree(amount=43)
    result2 = await matcher.try_build(anchor2)
    assert len(result2) == 1 and result2[0].data == {"v": 1}


@pytest.mark.asyncio
async def test_reject_after_out_still_cancels():
    src = """
    matcher decl_late_reject {
        produces FB
        entry    @x

        op Foo @x

        out { v: 1 }
        reject when true
    }
    """
    matcher = _compile(src)
    anchor = _FakeCC(OP_FOO, lt=10)
    assert await matcher.try_build(anchor) == []
    assert anchor.parent is None


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
    matcher = _compile(src)
    anchor = _FakeCC(OP_FOO, lt=10)
    with caplog.at_level("WARNING", logger="indexer.events.mch.compiler"):
        assert await matcher.try_build(anchor) == []
    assert anchor.parent is None
    assert any("rejecting match" in r.message for r in caplog.records)


# ==========================================================================
# Parse semantics
# ==========================================================================

@pytest.mark.asyncio
async def test_parse_alternatives_first_success_wins():
    src = """
    matcher decl_alt {
        produces FB
        entry    @x

        op Foo @x

        parse x as NeverParses | FakeTransfer
        out { amount: x.body.amount }
    }
    """
    matcher = _compile(src)
    anchor = _FakeCC(OP_FOO, body=_body(555), lt=10)
    (produced,) = await matcher.try_build(anchor)
    assert produced.data["amount"] == 555


@pytest.mark.asyncio
async def test_soft_parse_failure_yields_null_body():
    src = """
    matcher decl_soft {
        produces FB
        entry    @x

        op Foo @x

        parse x as NeverParses
        reject when x.body == null
        out { v: 1 }
    }
    """
    matcher = _compile(src)
    anchor = _FakeCC(OP_FOO, body=_body(1), lt=10)
    assert await matcher.try_build(anchor) == []


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
    matcher = _compile(src)
    anchor = _FakeCC(OP_FOO, lt=10)  # no Bar child
    (produced,) = await matcher.try_build(anchor)
    assert produced.data["got"] is False


# ==========================================================================
# Lookups
# ==========================================================================

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
        out {
            owner: account(w.owner)
            asset: asset(w.jetton)
        }
    }
    """
    matcher = _compile(src, _registries(lookups={"wallet": wallet}))
    anchor = _FakeCC(OP_FOO, lt=10)
    (produced,) = await matcher.try_build(anchor)
    assert calls == [RAW_DST]
    assert produced.data["owner"] == AccountId(RAW_SRC)
    asset = produced.data["asset"]
    assert isinstance(asset, Asset) and asset.jetton_address == AccountId(RAW_JETTON)


@pytest.mark.asyncio
async def test_lookup_null_result_supports_reject_flow():
    async def wallet(_addr):
        return None

    src = """
    matcher decl_lookup_null {
        produces FB
        entry    @x

        op Foo @x

        let w = lookup wallet(x.msg.destination)
        reject when w == null
        out { owner: w.owner }
    }
    """
    matcher = _compile(src, _registries(lookups={"wallet": wallet}))
    anchor = _FakeCC(OP_FOO, lt=10)
    assert await matcher.try_build(anchor) == []


# ==========================================================================
# Resolver diagnostics (R011-R018)
# ==========================================================================

def _codes(src: str, reg: Registries | None = None) -> list[str]:
    reg = reg or _registries()
    with pytest.raises(CompileError) as exc:
        resolve(parse(list(tokenize(src, "t.mch")), "t.mch"), reg)
    return [d.code for d in exc.value.bag.items]


_DIAG_HEAD = "matcher m {\n  produces FB\n  entry @x\n\n  op Foo @x\n"


def _diag_src(stmts: str, head: str = _DIAG_HEAD) -> str:
    return "opcode Foo = 0xF00\n" + head + stmts + "\n}"


def test_r011_parse_target_must_be_capture():
    codes = _codes(_diag_src("parse ghost as FakeTransfer\nout { v: 1 }"))
    assert "R011_PARSE_TARGET_NOT_CAPTURE" in codes


def test_r012_unknown_message_type():
    codes = _codes(_diag_src("parse x as NoSuchType\nout { v: 1 }"))
    assert "R012_UNKNOWN_MESSAGE_TYPE" in codes


def test_r013_duplicate_let_and_capture_shadowing():
    codes = _codes(_diag_src("let a = 1\nlet a = 2\nout { v: a }"))
    assert "R013_DUP_LET" in codes
    codes = _codes(_diag_src("let x = 1\nout { v: x }"))
    assert "R013_DUP_LET" in codes


def test_r014_duplicate_out_field():
    codes = _codes(_diag_src("out { v: 1, v: 2 }"))
    assert "R014_DUP_OUT_FIELD" in codes


def test_r015_build_directive_plus_statements_is_ambiguous():
    src = (
        "opcode Foo = 0xF00\n"
        "matcher m {\n  produces FB\n  build b\n  entry @x\n\n"
        "  op Foo @x\n  out { v: 1 }\n}"
    )
    assert "R015_BUILD_AMBIGUOUS" in _codes(src)


def test_r016_build_statements_without_out():
    codes = _codes(_diag_src("let a = 1"))
    assert "R016_BUILD_NO_OUT" in codes


def test_r017_multiple_out_blocks():
    codes = _codes(_diag_src("out { v: 1 }\nout { w: 2 }"))
    assert "R017_MULTIPLE_OUT" in codes


def test_r018_flow_kind_and_arity():
    codes = _codes(_diag_src("flow bogus(1)\nout { v: 1 }"))
    assert "R018_BAD_FLOW" in codes
    codes = _codes(_diag_src("flow ton(1)\nout { v: 1 }"))
    assert "R018_BAD_FLOW" in codes


def test_declarative_matcher_resolves_clean():
    # Control: the declarative form itself carries no errors.
    src = _diag_src(
        "parse x as FakeTransfer\n"
        "let amt = x.body.amount\n"
        "reject when amt == null\n"
        "flow ton(account(x.msg.source), amt)\n"
        "out { amount: amt }"
    )
    rf = resolve(parse(list(tokenize(src, "t.mch")), "t.mch"), _registries())
    assert rf.matchers[0].decl.build is None
    assert len(rf.matchers[0].build_stmts) == 5
