"""IR-engine ports of compiled-path behavioral tests (T29 stage 1).

Every behavior here previously ran only against the compiled path
(indexer/events/mch/compiler.py BlockMatcher instances). The compiled runtime
is being retired, so each behavior is re-pinned against the v2 IR engine on the
exact artifact the frontend emits: compile_text -> emit_ir -> load_artifact ->
build_matchers. Synthetic block trees follow the test_engine/test_walker
fixture style (fake event nodes with real lt/utime + optional BOC bodies).

Origin of each group is noted per section (the compiled test it replaces).
"""
from __future__ import annotations

from types import SimpleNamespace

import pytest
from pytoniq_core import begin_cell

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.utils import AccountId, Amount, Asset
from indexer.events.mch.compile import compile_text
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir import IrLoadError, build_matchers, load_artifact

RAW_SRC = "0:" + "11" * 32
RAW_DST = "0:" + "22" * 32
RAW_JETTON = "0:" + "33" * 32


# ==========================================================================
# Synthetic call_contract / block fixtures (fake event nodes -> real
# lt/utime bookkeeping so merge_blocks works on success paths).
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
    def __init__(self, opcode: int, body: bytes | None = None, lt: int = 10,
                 failed: bool = False, **msg_kw):  # pylint: disable=super-init-not-called
        node = _FakeNode(_FakeMsg(opcode, body, lt, **msg_kw))
        Block.__init__(self, "call_contract", [node], None)
        self.opcode = opcode
        self.failed = failed


_LT = [0]


def _cc(opcode: int, **kw) -> _FakeCC:
    _LT[0] += 10
    return _FakeCC(opcode, lt=_LT[0], **kw)


def _bt(btype: str, lt: int = 10, data=None) -> Block:
    return Block(btype, [_FakeNode(_FakeMsg(0, None, lt))], data)


def _link(parent: Block, child: Block) -> None:
    parent.next_blocks.append(child)
    child.previous_block = parent


# ==========================================================================
# Compile helpers
# ==========================================================================

def _capturing_registries(opcodes: dict[str, int], produces: str = "MB",
                          builder: str = "build_m") -> tuple[Registries, list]:
    """Registries whose builder records the match and REJECTS (returns None) so
    the tree stays unmutated and captures/consumed can be inspected."""
    captured: list = []
    reg = Registries()
    reg.opcodes.update(opcodes)
    reg.block_types[produces] = type(produces, (), {})

    async def _build(m):
        captured.append(m)
        return None

    reg.builders[builder] = _build
    return reg, captured


def _ir(src: str, reg: Registries):
    compiled = compile_text(src, "test.mch", reg)
    art = load_artifact(emit_ir(compiled, ["test.mch"]))
    return build_matchers(art, reg)[0]


def _body(op: int, amount: int) -> bytes:
    return begin_cell().store_uint(op, 32).store_uint(amount, 64).end_cell().to_boc()


# ==========================================================================
# Chain edges (origin: test_compiler_behavioral chain/dup-opcode/maybe/any/
# btype/mixed capture-binding tests)
# ==========================================================================

@pytest.mark.asyncio
async def test_chain_requires_child_and_binds_all():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    matcher m { produces MB build build_m entry @a
        op A @a -> op B @b }
    """
    reg, captured = _capturing_registries({"A": 0x1, "B": 0x2})
    m = _ir(src, reg)

    a = _cc(0x1); b = _cc(0x2); _link(a, b)
    assert await m.try_build(a) == []          # rejecting builder: match reached
    assert captured[0].a is a and captured[0].b is b

    captured.clear()
    bare = _cc(0x1)
    assert not await m.try_build(bare)          # no B child -> structural mismatch
    assert captured == []


@pytest.mark.asyncio
async def test_duplicate_opcode_chain_binds_distinct_blocks():
    src = """
    opcode A = 0x1
    matcher m { produces MB build build_m entry @first
        op A @first -> op A @second }
    """
    reg, captured = _capturing_registries({"A": 0x1})
    m = _ir(src, reg)

    outer = _cc(0x1); inner = _cc(0x1); _link(outer, inner)
    await m.try_build(outer)
    assert captured[0].first is outer
    assert captured[0].second is inner
    assert captured[0].first is not captured[0].second


@pytest.mark.asyncio
async def test_maybe_child_binds_when_present_none_when_absent():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    matcher m { produces MB build build_m entry @a
        op A @a -> maybe op B @b }
    """
    reg, captured = _capturing_registries({"A": 0x1, "B": 0x2})
    m = _ir(src, reg)

    a = _cc(0x1); b = _cc(0x2); _link(a, b)
    await m.try_build(a)
    assert captured[0].a is a and captured[0].b is b

    captured.clear()
    a2 = _cc(0x1)
    await m.try_build(a2)
    assert captured[0].a is a2 and captured[0].b is None


@pytest.mark.asyncio
async def test_alternative_firing_branch_binds_other_none():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    matcher m { produces MB build build_m entry @a
        op A @a -> ( op B @b | op C @c ) }
    """
    reg, captured = _capturing_registries({"A": 0x1, "B": 0x2, "C": 0x3})
    m = _ir(src, reg)

    a = _cc(0x1); b = _cc(0x2); _link(a, b)
    await m.try_build(a)
    assert captured[0].b is b and captured[0].c is None

    captured.clear()
    a2 = _cc(0x1); c = _cc(0x3); _link(a2, c)
    await m.try_build(a2)
    assert captured[0].b is None and captured[0].c is c


@pytest.mark.asyncio
async def test_named_where_predicate_gates_then_binds_capture():
    src = """
    opcode A = 0x1
    predicate is_special
    matcher m { produces MB build build_m entry @a
        op A @a where is_special }
    """
    reg, captured = _capturing_registries({"A": 0x1})
    reg.predicates["is_special"] = lambda b: bool(getattr(b, "special_flag", False))
    m = _ir(src, reg)

    plain = _cc(0x1)
    assert not await m.try_build(plain)
    assert captured == []

    special = _cc(0x1); special.special_flag = True
    await m.try_build(special)
    assert captured[0].a is special


@pytest.mark.asyncio
async def test_btype_anchor_capture_binding():
    src = """
    matcher m { produces MB build build_m entry @j
        btype jetton_transfer @j }
    """
    reg, captured = _capturing_registries({})
    m = _ir(src, reg)

    tree = _bt("jetton_transfer")
    await m.try_build(tree)
    assert captured[0].j is tree


@pytest.mark.asyncio
async def test_any_head_capture_binding():
    src = """
    opcode A = 0x1
    matcher m { produces MB build build_m entry @a
        op A @a -> any @x }
    """
    reg, captured = _capturing_registries({"A": 0x1})
    m = _ir(src, reg)

    a = _cc(0x1); child = _bt("some_random_btype"); _link(a, child)
    await m.try_build(a)
    assert captured[0].a is a and captured[0].x is child


@pytest.mark.asyncio
async def test_mixed_op_then_btype_chain_capture_binding():
    src = """
    opcode A = 0x1
    matcher m { produces MB build build_m entry @a
        op A @a -> btype jetton_transfer @j }
    """
    reg, captured = _capturing_registries({"A": 0x1})
    m = _ir(src, reg)

    a = _cc(0x1); jt = _bt("jetton_transfer"); _link(a, jt)
    await m.try_build(a)
    assert captured[0].a is a and captured[0].j is jt


# ==========================================================================
# Children set (origin: test_compiler_behavioral children-block binding)
# ==========================================================================

@pytest.mark.asyncio
async def test_children_set_binds_regardless_of_sibling_order_and_ignores_unrelated():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    opcode X = 0x99
    matcher m { produces MB build build_m entry @a
        op A @a -> { op B @b ; op C @c } }
    """
    reg, captured = _capturing_registries({"A": 0x1, "B": 0x2, "C": 0x3, "X": 0x99})
    m = _ir(src, reg)

    # Reversed sibling order in the tree.
    a = _cc(0x1); c = _cc(0x3); b = _cc(0x2)
    _link(a, c); _link(a, b)
    await m.try_build(a)
    assert captured[0].b is b and captured[0].c is c

    # Unrelated leading sibling must not break the set.
    captured.clear()
    a2 = _cc(0x1); x = _cc(0x99); b2 = _cc(0x2); c2 = _cc(0x3)
    _link(a2, x); _link(a2, b2); _link(a2, c2)
    await m.try_build(a2)
    assert captured[0].b is b2 and captured[0].c is c2


# ==========================================================================
# Parent chains (origin: test_compiler_behavioral two/three-hop parent chains)
# ==========================================================================

@pytest.mark.asyncio
async def test_two_hop_parent_chain_anchor_head_binds():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    matcher m { produces MB build build_m entry @a
        op A @a <- op B @b <- op C @c }
    """
    reg, captured = _capturing_registries({"A": 0x1, "B": 0x2, "C": 0x3})
    m = _ir(src, reg)

    c = _cc(0x3); b = _cc(0x2); a = _cc(0x1)
    _link(c, b); _link(b, a)
    await m.try_build(a)
    assert captured[0].a is a and captured[0].b is b and captured[0].c is c


@pytest.mark.asyncio
async def test_three_hop_parent_chain_anchor_head_binds():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    opcode D = 0x4
    matcher m { produces MB build build_m entry @a
        op A @a <- op B @b <- op C @c <- op D @d }
    """
    reg, captured = _capturing_registries({"A": 0x1, "B": 0x2, "C": 0x3, "D": 0x4})
    m = _ir(src, reg)

    d = _cc(0x4); c = _cc(0x3); b = _cc(0x2); a = _cc(0x1)
    _link(d, c); _link(c, b); _link(b, a)
    await m.try_build(a)
    assert captured[0].a is a and captured[0].b is b
    assert captured[0].c is c and captured[0].d is d


@pytest.mark.asyncio
async def test_two_hop_parent_chain_partial_match_rejected():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    opcode X = 0x99
    matcher m { produces MB build build_m entry @a
        op A @a <- op B @b <- op C @c }
    """
    reg, captured = _capturing_registries({"A": 0x1, "B": 0x2, "C": 0x3, "X": 0x99})
    m = _ir(src, reg)

    x = _cc(0x99); b = _cc(0x2); a = _cc(0x1)   # root is X, not C
    _link(x, b); _link(b, a)
    assert not await m.try_build(a)
    assert captured == []


# ==========================================================================
# Opcode-set anchors + XOR capture binding
# (origin: test_compiler_behavioral T10/T02 same-capture XOR)
# ==========================================================================

@pytest.mark.asyncio
async def test_opcode_set_anchor_required_child_fails_without_it():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    matcher m { produces MB build build_m entry @x
        (op A @x ^ op B @x) -> op C @c }
    """
    reg, captured = _capturing_registries({"A": 0x1, "B": 0x2, "C": 0x3})
    m = _ir(src, reg)

    a = _cc(0x1); c = _cc(0x3); _link(a, c)
    await m.try_build(a)
    assert captured[0].x is a and captured[0].c is c

    captured.clear()
    b_bare = _cc(0x2)
    assert not await m.try_build(b_bare)
    assert captured == []


@pytest.mark.asyncio
async def test_same_name_capture_across_xor_binds_winning_branch():
    src = """
    opcode R = 0x1
    opcode A = 0x2
    opcode B = 0x3
    matcher m { produces MB build build_m entry @r
        op R @r -> (op A @x ^ op B @x) }
    """
    reg, captured = _capturing_registries({"R": 0x1, "A": 0x2, "B": 0x3})
    m = _ir(src, reg)

    r = _cc(0x1); a = _cc(0x2); _link(r, a)
    await m.try_build(r)
    assert captured[0].r is r and captured[0].x is a

    captured.clear()
    r2 = _cc(0x1); b = _cc(0x3); _link(r2, b)
    await m.try_build(r2)
    assert captured[0].x is b


@pytest.mark.asyncio
async def test_xor_fires_correct_branch_binds_only_winner():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    matcher m { produces MB build build_m entry @a
        op A @a -> (op B @b ^ op C @c) }
    """
    reg, captured = _capturing_registries({"A": 0x1, "B": 0x2, "C": 0x3})
    m = _ir(src, reg)

    a = _cc(0x1); b = _cc(0x2); _link(a, b)
    await m.try_build(a)
    assert captured[0].b is b and captured[0].c is None


@pytest.mark.asyncio
async def test_same_head_xor_second_branch_binds_its_capture():
    """Shared head `op A` on both branches: a trace shaped for the SECOND branch
    binds @y, not @x (full positional walk per branch, first-full-match wins)."""
    src = """
    opcode R = 0x1
    opcode A = 0x2
    opcode B = 0x3
    opcode C = 0x4
    matcher m { produces MB build build_m entry @r
        op R @r -> ( op A -> op B @x ^ op A -> op C @y ) }
    """
    reg, captured = _capturing_registries({"R": 0x1, "A": 0x2, "B": 0x3, "C": 0x4})
    m = _ir(src, reg)

    # Second-branch shape: R -> A -> C.
    r = _cc(0x1); a = _cc(0x2); c = _cc(0x4); _link(r, a); _link(a, c)
    await m.try_build(r)
    assert captured[0].y is c and captured[0].x is None

    # First-branch shape: R -> A -> B.
    captured.clear()
    r2 = _cc(0x1); a2 = _cc(0x2); b = _cc(0x3); _link(r2, a2); _link(a2, b)
    await m.try_build(r2)
    assert captured[0].x is b and captured[0].y is None


@pytest.mark.asyncio
async def test_same_head_xor_shared_head_capture_rolls_back_losing_branch():
    """Captures on the SHARED head itself: a second-branch trace binds @y and
    leaves @x unbound (losing branch's binding rolled back)."""
    src = """
    opcode R = 0x1
    opcode A = 0x2
    opcode B = 0x3
    opcode C = 0x4
    matcher m { produces MB build build_m entry @r
        op R @r -> ( op A @x -> op B ^ op A @y -> op C ) }
    """
    reg, captured = _capturing_registries({"R": 0x1, "A": 0x2, "B": 0x3, "C": 0x4})
    m = _ir(src, reg)

    r = _cc(0x1); a = _cc(0x2); c = _cc(0x4); _link(r, a); _link(a, c)
    await m.try_build(r)
    assert captured[0].y is a and captured[0].x is None


# ==========================================================================
# T15 children-set-then-parent shape (origin: test_compiler_behavioral)
# ==========================================================================

@pytest.mark.asyncio
async def test_children_set_then_parent_edge_matches_binds_and_consumes():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    opcode P = 0x4
    matcher m { produces MB build build_m entry @a
        op A @a -> {op B @b; maybe op C @c} <- op P @p }
    """
    reg, captured = _capturing_registries({"A": 0x1, "B": 0x2, "C": 0x3, "P": 0x4})
    m = _ir(src, reg)

    # P -> A -> {B, C}; anchor is the mid-tree A, its parent P must also match.
    p = _cc(0x4); a = _cc(0x1); b = _cc(0x2); c = _cc(0x3)
    _link(p, a); _link(a, b); _link(a, c)
    await m.try_build(a)
    mm = captured[0]
    assert mm.a is a and mm.b is b and mm.c is c and mm.p is p
    assert {id(x) for x in mm._mch_consumed} == {id(a), id(b), id(c), id(p)}

    # Without the parent, the match must fail.
    captured.clear()
    a2 = _cc(0x1); b2 = _cc(0x2); _link(a2, b2)
    assert not await m.try_build(a2)
    assert captured == []


# ==========================================================================
# Declarative build gaps (origin: test_declarative_build compiled-runtime half)
# ==========================================================================

class _FakeTransfer:
    def __init__(self, s):  # s: pytoniq_core Slice
        s.load_uint(32)
        self.amount = s.load_uint(64)


def _decl_registries(lookups: dict | None = None) -> Registries:
    reg = Registries()
    reg.opcodes.update({"Foo": 0xF00, "Bar": 0xBA7})
    reg.block_types["FB"] = object
    reg.block_types["JettonTransferBlock"] = object
    reg.message_types["FakeTransfer"] = _FakeTransfer
    if lookups:
        reg.lookups.update(lookups)
    return reg


@pytest.mark.asyncio
async def test_produces_class_name_normalizes_to_btype():
    src = """
    matcher decl_norm { produces JettonTransferBlock entry @x
        op Foo @x
        out { v: 1 } }
    """
    m = _ir(src, _decl_registries())
    anchor = _cc(0xF00)
    (produced,) = await m.try_build(anchor)
    assert produced.btype == "jetton_transfer"


@pytest.mark.asyncio
async def test_reject_after_out_still_cancels():
    src = """
    matcher decl_late_reject { produces FB entry @x
        op Foo @x
        out { v: 1 }
        reject when true }
    """
    m = _ir(src, _decl_registries())
    anchor = _cc(0xF00)
    assert await m.try_build(anchor) == []
    assert anchor.parent is None


@pytest.mark.asyncio
async def test_lookup_feeds_asset_and_account_out_fields():
    async def wallet(addr):
        return SimpleNamespace(owner=RAW_SRC, jetton=RAW_JETTON, address=addr)

    src = """
    matcher decl_lookup { produces FB entry @x
        op Foo @x
        let w = lookup wallet(x.msg.destination)
        reject when w == null
        out {
            owner: account(w.owner)
            asset: asset(w.jetton)
        } }
    """
    m = _ir(src, _decl_registries(lookups={"wallet": wallet}))
    anchor = _cc(0xF00)
    (produced,) = await m.try_build(anchor)
    assert produced.data["owner"] == AccountId(RAW_SRC)
    asset = produced.data["asset"]
    assert isinstance(asset, Asset) and asset.jetton_address == AccountId(RAW_JETTON)


@pytest.mark.asyncio
async def test_lookup_null_result_supports_reject():
    async def wallet(_addr):
        return None

    src = """
    matcher decl_lookup_null { produces FB entry @x
        op Foo @x
        let w = lookup wallet(x.msg.destination)
        reject when w == null
        out { owner: w.owner } }
    """
    m = _ir(src, _decl_registries(lookups={"wallet": wallet}))
    anchor = _cc(0xF00)
    assert await m.try_build(anchor) == []
    assert anchor.parent is None


# ==========================================================================
# Host functions (origin: test_host_fns compiled-runtime half)
# ==========================================================================

_FN_SRC = """
matcher fn_host { produces FB entry @x
    op Foo @x -> maybe op Bar @y
    let r = summarize(consumed, anchor, y)
    reject when r == null
    out {
        n:      r.n
        anchor_is_x: r.anchor_ok
        y_null: r.y_null
    } }
"""


def _fn_registries() -> Registries:
    reg = Registries()
    reg.opcodes.update({"Foo": 0xF00, "Bar": 0xBA7})
    reg.block_types["FB"] = object
    return reg


@pytest.mark.asyncio
async def test_fn_receives_consumed_anchor_and_null_args():
    seen: dict = {}

    async def summarize(consumed, anchor, y):
        seen["consumed"] = consumed
        seen["anchor"] = anchor
        seen["y"] = y
        return SimpleNamespace(n=len(consumed), anchor_ok=anchor is consumed[0],
                               y_null=y is None)

    reg = _fn_registries()
    reg.register_fn("summarize")(summarize)
    m = _ir(_FN_SRC, reg)

    anchor = _cc(0xF00); bar = _cc(0xBA7); _link(anchor, bar)
    (produced,) = await m.try_build(anchor)
    assert produced.data == {"n": 2, "anchor_is_x": True, "y_null": False}
    assert seen["anchor"] is anchor
    assert seen["consumed"][0] is anchor and seen["consumed"][1] is bar

    anchor2 = _cc(0xF00)  # optional Bar absent -> fn still receives null y
    (produced2,) = await m.try_build(anchor2)
    assert produced2.data == {"n": 1, "anchor_is_x": True, "y_null": True}
    assert seen["y"] is None


@pytest.mark.asyncio
async def test_fn_returning_null_supports_reject():
    async def summarize(consumed, anchor, y):
        return None

    reg = _fn_registries()
    reg.register_fn("summarize")(summarize)
    m = _ir(_FN_SRC, reg)
    anchor = _cc(0xF00)
    assert await m.try_build(anchor) == []
    assert anchor.parent is None


@pytest.mark.asyncio
async def test_fn_exception_rejects_match(caplog):
    async def summarize(consumed, anchor, y):
        raise RuntimeError("boom")

    reg = _fn_registries()
    reg.register_fn("summarize")(summarize)
    m = _ir(_FN_SRC, reg)
    anchor = _cc(0xF00)
    with caplog.at_level("WARNING", logger="indexer.events.mch_ir.engine"):
        assert await m.try_build(anchor) == []
    assert any("rejecting match" in r.message for r in caplog.records)


def test_unknown_host_fn_is_a_load_error():
    """A build-program call to an unregistered host fn hard-fails at
    build_matchers time — the registry model (spec §9.10), aligned with how
    builders/shapers/predicates/message-types are checked, rather than a silent
    runtime rejection."""
    src = """
    matcher fn_missing { produces FB entry @x
        op Foo @x
        out { v: nosuch(x) } }
    """
    with pytest.raises(IrLoadError):
        _ir(src, _fn_registries())


# ==========================================================================
# Shaper on the declarative build path (origin: test_shape compiled-runtime)
# ==========================================================================

@pytest.mark.asyncio
async def test_shaper_runs_on_declarative_build_path_with_capture_env():
    """A declarative matcher (out {}, no builder) with a `shape` directive: the
    shaper runs post-merge and receives the full capture env incl. the
    well-known `anchor` name (spec §8.7)."""
    from indexer.events.blocks.core import EmptyBlock

    calls: list = []

    def insert_proxy(produced, match):
        proxy = EmptyBlock()
        match.a.insert_between([match.b], proxy)
        produced.children_blocks.append(proxy)
        proxy.parent = produced
        calls.append((produced, match))

    reg = Registries()
    reg.opcodes.update({"A": 0x1, "B": 0x2})
    reg.block_types["MB"] = type("MB", (), {})
    reg.register_shaper("insert_proxy")(insert_proxy)

    src = """
    opcode A = 0x1
    opcode B = 0x2
    matcher shaped_decl {
        produces MB
        shape    insert_proxy
        entry    @a
        op A @a -> op B @b
        out { kind: "x" }
    }
    """
    m = _ir(src, reg)
    a = _cc(0x1); b = _cc(0x2); _link(a, b)
    (produced,) = await m.try_build(a)

    assert len(calls) == 1
    produced_seen, match = calls[0]
    assert produced_seen is produced
    assert match.a is a and match.b is b and match.anchor is a
    assert isinstance(a.next_blocks[0], EmptyBlock)
