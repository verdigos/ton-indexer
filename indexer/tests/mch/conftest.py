from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

import pytest

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block


class _FakeCallContractBlock(CallContractBlock):
    """A CallContractBlock instance constructable without database state.

    Real CallContractBlock requires an EventNode; the lowering tests need only
    `opcode`, `btype`, `next_blocks`, `previous_block`, `event_nodes`. We bypass
    the production constructor by creating a bare Block and setting attributes.
    """

    def __init__(self, opcode: int):  # pylint: disable=super-init-not-called
        # Skip CallContractBlock.__init__ — it needs message bodies.
        Block.__init__(self, "call_contract", [], None)
        self.opcode = opcode
        self.next_blocks = []
        self.previous_block = None


def _new_block(opcode: int | None = None, btype: str | None = None) -> Block:
    if opcode is not None:
        return _FakeCallContractBlock(opcode)
    assert btype is not None
    b = Block(btype, [], None)
    return b


@dataclass
class BlockSpec:
    opcode: int | None = None
    btype: str | None = None
    children: list["BlockSpec"] = field(default_factory=list)


@pytest.fixture
def build_block_tree():
    """Return a function that builds a Block tree from a BlockSpec.

    Usage:
        b = build_block_tree(BlockSpec(opcode=0x0f8a7ea5, children=[
            BlockSpec(opcode=0x178d4519, children=[
                BlockSpec(opcode=0x7362d09c),
            ]),
        ]))
    """
    def build(spec: BlockSpec, parent: Any = None) -> Any:
        b = _new_block(spec.opcode, spec.btype)
        b.previous_block = parent
        b.next_blocks = []
        for c in spec.children:
            child = build(c, parent=b)
            b.next_blocks.append(child)
        return b
    return build
