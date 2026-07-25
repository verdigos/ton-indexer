"""Pin the ABI-path Python twin parsers against the committed ABI declarations.

The mch A/B gate only agrees if a `parse`d body exposes the SAME attribute names
in the SAME order on both sides: the C++ adapter emits an ABI struct's fields in
declaration order under their tolk (camelCase) names, and the hand-written Python
twin in `indexer/events/mch/builders/*.py` has to mirror that exactly. A rename
or reorder in the .tolk decl would otherwise only surface as a full-corpus twin
diff (or, worse, as a silently-null field).

These tests compare each twin class's declared `ABI_FIELDS` against
`ton-index-worker/ton-abi/abi/<contract>.abi.json`, and against the attributes
the class actually sets.
"""
from __future__ import annotations

import inspect
import json
from pathlib import Path

import pytest

from indexer.events.mch.builders import dedust_v2

ABI_DIR = (
    Path(__file__).resolve().parents[3] / "ton-index-worker" / "ton-abi" / "abi"
)


def _abi_struct_fields(abi_stem: str) -> dict[str, tuple[str, ...]]:
    """{struct name: field names in declaration order} from a committed ABI."""
    abi = json.loads((ABI_DIR / f"{abi_stem}.abi.json").read_text(encoding="utf-8"))
    return {
        d["name"]: tuple(f["name"] for f in d["fields"])
        for d in abi["declarations"]
        if d["kind"] == "struct"
    }


@pytest.mark.parametrize("name", sorted(dedust_v2.ABI_FIELDS))
def test_dedust_v2_fields_match_abi(name: str) -> None:
    """Twin field names + order == the committed dedust_v2 ABI declaration."""
    assert _abi_struct_fields("dedust_v2")[name] == dedust_v2.ABI_FIELDS[name]


@pytest.mark.parametrize("name", sorted(dedust_v2.ABI_FIELDS))
def test_dedust_v2_class_sets_declared_fields(name: str) -> None:
    """The twin class actually assigns exactly its ABI_FIELDS, in that order."""
    src = inspect.getsource(dedust_v2.CLAIM_REWARD_MESSAGE_TYPES[name].__init__)
    assigned = tuple(
        line.split("=", 1)[0].strip().removeprefix("self.")
        for line in (raw.strip() for raw in src.splitlines())
        if line.startswith("self.")
    )
    assert assigned == dedust_v2.ABI_FIELDS[name]


def test_abi_fields_covers_every_registered_twin() -> None:
    """No twin parser class may be registered without a pinned field tuple."""
    assert set(dedust_v2.CLAIM_REWARD_MESSAGE_TYPES) == set(dedust_v2.ABI_FIELDS)
