"""Shared synchronous evaluator for IR expressions.

Home of the Python implementation of node-level inline `where (expr)`
evaluation. The IR engine evaluates each node's encoded expression with the
candidate block and the matcher's entry capture.

Import boundary: mch_ir must not import the frontend (parser/ast_/compiler)
and the frontend must not import mch_ir, so the evaluator lives here as a leaf
that depends only on `blocks.core`/`blocks.utils` (the same footing as
mch/registry.py, which mch_ir already imports). mch_ir/expr.py re-exports
these names, keeping its public API unchanged.

The evaluator dispatches on k-tagged IR dictionaries.
`dotfield` reads the candidate block's `data`, missing field / any EvalError /
non-bool result fails the test and never raises out of matching. Async
constructs (lookup, host fns) are rejected before run time: by the IR loader
(loader._check_where_expr_sync) and by the frontend compiler (C013).

The strict operator cores, builtins table and `_access` also live here because
the async build-program evaluator (mch_ir/expr.py Evaluator) shares them.
"""
from __future__ import annotations

import base64
import inspect
from types import SimpleNamespace
from typing import Any

from pytoniq_core import Address, Cell, ExternalAddress

from indexer.events.blocks.core import Block
from indexer.events.blocks.utils import AccountId, Amount, Asset
from indexer.events.mch.builtin_signatures import BUILTIN_SIGNATURES


class EvalError(Exception):
    """A build-expression runtime fault; the match is rejected."""


# Builtins: name -> (callable, arity). Null arguments never reach
# the callable. The call evaluator returns null first.


def _builtin_account(x: Any) -> AccountId:
    return AccountId(x)


def _builtin_amount(x: Any) -> Amount:
    return Amount(x.value if isinstance(x, Amount) else int(x))


def _builtin_asset(jetton_master: Any) -> Asset:
    return Asset(is_ton=False, jetton_address=jetton_master)


def _builtin_ton_asset() -> Asset:
    return Asset(is_ton=True)


def _builtin_addr_none() -> AccountId:
    # The addr_none AccountId is distinct from a null address. It mirrors
    # the host's `AccountId(None)`: repr "addr_none", as_str() -> None. Needed
    # because `account(null)` short-circuits to null; a field that must carry a
    # present-but-empty address (e.g. a jetton transfer's absent response) uses
    # `account(x) ?? addr_none()`. C++-portable: a compile-time constant value.
    return AccountId(None)


def _builtin_b64(x: Any) -> str:
    # BOC-encode a raw Cell before base64 (mirrors host `cell.to_boc()`); a
    # field already carrying boc bytes (parsers that pre-encode payloads) passes
    # straight through. C++-portable: cell serialization is a host primitive.
    if isinstance(x, Cell):
        x = x.to_boc()
    return base64.b64encode(x).decode("utf-8")


_ASSET_TON_TAG = "AssetTon"
_ASSET_JETTON_TAG = "AssetJetton"

_TAIL_TAGS = {
    "JettonForwardPayloadRef": "ref",
    "JettonForwardPayloadInline": "bits",
    "PTonForwardPayloadRef": "ref",
    "PTonForwardPayloadInline": "refs",
}


def _faithful_obj_tag(name: str, x: Any) -> str:
    if not isinstance(x, SimpleNamespace):
        raise EvalError(f"{name} expects an ABI object, got {type(x).__name__}")
    try:
        tag = getattr(x, "$")
    except AttributeError:
        raise EvalError(f"{name}: ABI object has no '$' tag") from None
    if not isinstance(tag, str):
        raise EvalError(f"{name}: ABI object '$' tag must be a string")
    return tag


def _builtin_asset_of(x: Any) -> Asset:
    tag = _faithful_obj_tag("asset_of", x)
    if tag == _ASSET_TON_TAG:
        return Asset(is_ton=True)
    if tag != _ASSET_JETTON_TAG:
        raise EvalError(f"asset_of: unknown ABI arm {tag!r}")
    try:
        workchain = getattr(x, "workchain")
        hash_cell = getattr(x, "hash")
    except AttributeError as ex:
        raise EvalError(f"asset_of: jetton arm has no {ex.name!r} field") from None
    if isinstance(workchain, bool) or not isinstance(workchain, int):
        raise EvalError("asset_of: jetton workchain must be an integer")
    if not isinstance(hash_cell, Cell):
        raise EvalError("asset_of: jetton hash must be a cell")
    if len(hash_cell.bits) != 256 or len(hash_cell.refs) != 0:
        raise EvalError("asset_of: jetton hash cell must have exactly 256 bits and no refs")
    hash_bytes = hash_cell.begin_parse().load_bytes(32)
    raw = f"{workchain}:{hash_bytes.hex().upper()}"
    return Asset(is_ton=False, jetton_address=raw)


def _builtin_tail_unwrap(x: Any) -> Cell | None:
    tag = _faithful_obj_tag("tail_unwrap", x)
    policy = _TAIL_TAGS.get(tag)
    if policy is None:
        raise EvalError(f"tail_unwrap: unknown ABI arm {tag!r}")
    try:
        value = getattr(x, "value")
    except AttributeError:
        raise EvalError("tail_unwrap: ABI arm has no 'value' field") from None
    if not isinstance(value, Cell):
        raise EvalError("tail_unwrap: ABI arm value must be a cell")
    if policy == "bits" and len(value.bits) == 0:
        return None
    if policy == "refs" and len(value.refs) == 0:
        return None
    return value


def _builtin_bytes_of(x: Any) -> bytes:
    if isinstance(x, bytes):
        return x
    if not isinstance(x, Cell):
        raise EvalError(f"bytes_of expects bytes or a cell, got {type(x).__name__}")
    if len(x.bits) % 8 != 0 or len(x.refs) != 0:
        raise EvalError("bytes_of: cell must be byte-aligned and have no refs")
    return x.begin_parse().load_bytes(len(x.bits) // 8)


# List combinators operate on list captures and literals. Null propagates before
# these functions run. Empty first/last return null; empty len returns zero.
# Zip truncates to the shorter input.
# Map projects a field by dictionary lookup, then attribute lookup; it has no
# lambda form.


def _require_list(name: str, xs: Any) -> list:
    if not isinstance(xs, list):
        raise EvalError(f"{name} expects a list, got {type(xs).__name__}")
    return xs


def _project(x: Any, field: Any) -> Any:
    if not isinstance(field, str):
        raise EvalError(f"map field must be a string, got {type(field).__name__}")
    if x is None:
        return None
    if isinstance(x, dict):
        if field in x:
            return x[field]
        raise EvalError(f"map: element has no field {field!r}")
    try:
        return getattr(x, field)
    except AttributeError:
        raise EvalError(f"map: {type(x).__name__} has no field {field!r}") from None


def _builtin_first(xs: Any) -> Any:
    xs = _require_list("first", xs)
    return xs[0] if xs else None


def _builtin_last(xs: Any) -> Any:
    xs = _require_list("last", xs)
    return xs[-1] if xs else None


def _builtin_at(xs: Any, i: Any) -> Any:
    xs = _require_list("at", xs)
    if isinstance(i, bool) or not isinstance(i, int):
        raise EvalError(f"at index must be an integer, got {type(i).__name__}")
    if i < 0 or i >= len(xs):
        return None
    return xs[i]


def _builtin_len(xs: Any) -> int:
    return len(_require_list("len", xs))


def _builtin_zip(xs: Any, ys: Any) -> list:
    xs = _require_list("zip", xs)
    ys = _require_list("zip", ys)
    return [[a, b] for a, b in zip(xs, ys)]


def _builtin_concat(xs: Any, ys: Any) -> list:
    xs = _require_list("concat", xs)
    ys = _require_list("concat", ys)
    return xs + ys


def _builtin_map(xs: Any, field: Any) -> list:
    return [_project(x, field) for x in _require_list("map", xs)]


def _builtin_contains(hay: Any, needle: Any) -> bool:
    # The language's string operation is a substring test; it provides no
    # slicing, case folding, or concatenation. Both arguments must be strings.
    # A non-string is a fault rather than False, and the test is case-sensitive.
    # The URIs and content keys it serves are exact. An
    # empty needle is contained in everything, like Python's `in` and C++'s
    # std::string::find. Nulls never reach here (the call evaluator returns
    # null first).
    if not isinstance(hay, str):
        raise EvalError(f"contains expects a string haystack, got {type(hay).__name__}")
    if not isinstance(needle, str):
        raise EvalError(f"contains expects a string needle, got {type(needle).__name__}")
    return needle in hay


def _require_object(x: Any) -> None:
    if isinstance(x, (Block, Amount, AccountId, Asset, Cell)) or not (
        hasattr(x, "__dict__") or hasattr(type(x), "__slots__")
    ):
        raise EvalError(f"expected an attribute object, got {type(x).__name__}")


def _builtin_get(record: Any, key: Any) -> Any:
    if not isinstance(key, str):
        raise EvalError(f"get key must be a string, got {type(key).__name__}")
    if not isinstance(record, dict):
        _require_object(record)
    value = record.get(key) if isinstance(record, dict) else getattr(record, key, None)
    return None if callable(value) else value


def _message_envelope(x: Any) -> Any:
    if isinstance(x, Block):
        return _access(x, "msg", {})
    _require_object(x)
    return x


def _builtin_tx_aborted(x: Any) -> bool | None:
    tx = getattr(_message_envelope(x), "transaction", None)
    return None if tx is None else _require_bool(tx.aborted)


def _builtin_exit_code(x: Any) -> int | None:
    tx = getattr(_message_envelope(x), "transaction", None)
    return getattr(tx, "compute_exit_code", None)


def _builtin_uint_hex(x: Any) -> str:
    if isinstance(x, bool) or not isinstance(x, int) or x < 0:
        raise EvalError("uint_hex expects a nonnegative integer")
    return hex(x)[2:]


def _builtin_txid_le_hex(x: Any) -> str:
    if isinstance(x, bool) or not isinstance(x, int) or x < 0:
        raise EvalError("txid_le_hex expects an unsigned 256-bit integer")
    return x.to_bytes(32, "little").hex()


def _builtin_ext_addr_str(a: Any) -> str | None:
    if isinstance(a, AccountId):
        return a.as_str()
    if isinstance(a, Address):
        return a.to_str(False).upper()
    if isinstance(a, ExternalAddress):
        return f"{a.len};{hex(a.external_address)[2:]}"
    raise EvalError(f"ext_addr_str expects an address, got {type(a).__name__}")


def _builtin_addr_hash_hex(a: Any) -> str | None:
    if isinstance(a, AccountId):
        address = a.as_str()
        return None if address is None else hex(int(address.split(":", 1)[1], 16))
    if isinstance(a, Address):
        return hex(int.from_bytes(a.hash_part, "big"))
    raise EvalError(f"addr_hash_hex expects an account, got {type(a).__name__}")

def _out_body_hash(m: Any) -> bytes | None:
    content = getattr(m, "message_content", None)
    body = getattr(content, "body", None) if content is not None else None
    if not body:
        return None
    stored = getattr(m, "body_hash", None)
    if stored is None:
        stored = getattr(content, "hash", None)
    if stored is None:
        return None
    if isinstance(stored, bytes):
        return stored
    if not isinstance(stored, str):
        return None
    try:
        return base64.b64decode(stored)
    except Exception:
        return None


def _out_message_record(m: Any) -> SimpleNamespace:
    return SimpleNamespace(
        source=getattr(m, "source", None),
        destination=getattr(m, "destination", None),
        value=getattr(m, "value", None),
        opcode=getattr(m, "opcode", None),
        created_lt=getattr(m, "created_lt", None),
        created_at=getattr(m, "created_at", None),
        bounce=getattr(m, "bounce", None),
        bounced=getattr(m, "bounced", False),
        msg_hash=getattr(m, "msg_hash", None),
        direction=getattr(m, "direction", None),
        body_hash=_out_body_hash(m),
    )


def _builtin_out_messages(x: Any) -> list:
    if not isinstance(x, Block):
        raise EvalError(f"out_messages expects a block, got {type(x).__name__}")
    try:
        msg = x.get_message()
    except Exception:
        return []
    if msg is None:
        return []
    tx = getattr(msg, "transaction", None)
    if tx is None:
        return []
    out = []
    for m in getattr(tx, "messages", None) or []:
        if getattr(m, "direction", None) != "out":
            continue
        out.append(_out_message_record(m))
    return out



_BUILTIN_CALLABLES: dict[str, Any] = {
    symbol.removeprefix("_builtin_"): value
    for symbol, value in globals().items()
    if symbol.startswith("_builtin_") and callable(value)
}

if _BUILTIN_CALLABLES.keys() != BUILTIN_SIGNATURES.keys():
    raise RuntimeError("builtin callable names disagree with builtin signatures")
for _name, _arity in BUILTIN_SIGNATURES.items():
    if len(inspect.signature(_BUILTIN_CALLABLES[_name]).parameters) != _arity:
        raise RuntimeError(f"builtin callable arity disagrees for {_name!r}")

BUILTINS: dict[str, tuple[Any, int]] = {
    name: (_BUILTIN_CALLABLES[name], arity)
    for name, arity in BUILTIN_SIGNATURES.items()
}


# Operator cores shared by the async and sync evaluators. Lazy operators
# (and/or/??/ternary) stay in each evaluator; these are the strict ones.


def _eq_result(op: str, left: Any, right: Any) -> bool:
    """`==` / `!=` with structural null handling (never calls user __eq__ on None)."""
    if left is None or right is None:
        eq = left is None and right is None
    elif (isinstance(left, Amount) and isinstance(left.value, float)) or \
            (isinstance(right, Amount) and isinstance(right.value, float)):
        # The language has no floats. Comparing a float-backed Amount faults
        # rather than silently comparing float values.
        raise EvalError("'==' on a float-backed Amount (the language has no floats)")
    else:
        try:
            eq = bool(left == right)
        except Exception as ex:
            raise EvalError(f"'==' failed: {ex}") from ex
    return eq if op == "==" else not eq


def _ord_result(op: str, left: Any, right: Any) -> bool:
    """Ordered comparison; null on either side compares False."""
    if left is None or right is None:
        return False
    try:
        if op == "<":
            return bool(left < right)
        if op == "<=":
            return bool(left <= right)
        if op == ">":
            return bool(left > right)
        return bool(left >= right)
    except TypeError as ex:
        raise EvalError(
            f"cannot order {type(left).__name__} and {type(right).__name__}"
        ) from ex


def _neg_result(v: Any) -> Any:
    if v is None:
        return None
    try:
        return -v
    except TypeError as ex:
        raise EvalError(f"unary '-' on {type(v).__name__}: {ex}") from ex


def _arith_int(v: Any) -> int:
    """Coerce an arithmetic operand to int: Amount -> its value, int -> itself
    (bool is rejected because it is an int subclass); anything else is a fault.
    This lets `a.msg.value - b.msg.value` work whether the envelope value is an
    int or an Amount."""
    if isinstance(v, Amount):
        if isinstance(v.value, float):
            raise EvalError(
                "arithmetic operand is a float-backed Amount (the language has no floats)")
        return v.value
    if isinstance(v, bool):
        raise EvalError("arithmetic operand must be an integer, got bool")
    if isinstance(v, int):
        return v
    raise EvalError(f"arithmetic operand must be an integer or Amount, got {type(v).__name__}")


def _arith_result(op: str, left: Any, right: Any) -> Any:
    """Integer arithmetic `+`/`-`/`*`. Null on either side
    propagates to null; operands coerce to int (Amount -> value); the result is
    a plain int. Division is not supported."""
    if left is None or right is None:
        return None
    a, b = _arith_int(left), _arith_int(right)
    if op == "+":
        return a + b
    if op == "-":
        return a - b
    if op == "*":
        return a * b
    raise EvalError(f"unknown arithmetic operator {op!r}")


def _call_builtin(name: str, args: list[Any]) -> Any:
    fn, arity = BUILTINS[name]
    if len(args) != arity:
        raise EvalError(f"builtin {name} takes {arity} argument(s), got {len(args)}")
    if any(a is None for a in args):
        return None
    try:
        return fn(*args)
    except Exception as ex:
        raise EvalError(f"builtin {name} failed: {ex}") from ex


def _require_bool(v: Any) -> bool:
    if not isinstance(v, bool):
        raise EvalError(f"condition must be a bool, got {type(v).__name__} ({v!r})")
    return v


def _access(obj: Any, field: str, bodies: dict[int, Any]) -> Any:
    """Field access, mirroring eval.py Evaluator._access.

    Block values expose only msg/body/failed/broken/btype; `body` requires a
    prior `parse` (recorded in `bodies` by block identity). List captures
    expose only `body` (parallel list). Everything else is getattr, with a
    missing attribute raising EvalError.
    """
    if obj is None:
        return None
    if isinstance(obj, Block):
        if field == "msg":
            try:
                return obj.get_message()
            except Exception as ex:
                raise EvalError(f"block has no message envelope: {ex}") from ex
        if field == "body":
            if id(obj) not in bodies:
                raise EvalError("`.body` accessed on a capture that was not `parse`d")
            return bodies[id(obj)]
        if field == "failed":
            return obj.failed
        if field == "broken":
            return obj.broken
        if field == "btype":
            return obj.btype
        if field == "data":
            # A captured block's produced data (set by a prior matcher). Composed
            # here so a declarative build can read a child block's fields, e.g.
            # `transfer.data.sender`. Key access below is a plain map lookup.
            return obj.data
        raise EvalError(
            f"unknown block accessor {field!r} (expected msg/body/data/failed/broken/btype)"
        )
    if isinstance(obj, list):
        if field == "body":
            out = []
            for b in obj:
                if b is None:
                    out.append(None)
                elif id(b) not in bodies:
                    raise EvalError(
                        "`.body` accessed on a list capture that was not `parse`d"
                    )
                else:
                    out.append(bodies[id(b)])
            return out
        raise EvalError(f"field {field!r} is not defined on a list capture")
    if isinstance(obj, dict):
        # Block `data` map (or any record dict): pure key lookup, a missing key
        # is a fault (mirrors the host's `data[key]` KeyError -> match rejection).
        if field in obj:
            return obj[field]
        raise EvalError(f"data has no field {field!r}")
    if isinstance(obj, Amount):
        # `Amount.value`: unwrap the wrapper to its raw int, the
        # inverse of the `amount(x)` builtin. This is the sanctioned way to feed
        # a jetton/DEX block's `Amount`-typed `data` scalar into a raw-int output
        # field. This fixed accessor also matches the C++ Value model, where
        # Amount carries `num`. `Amount(None)`
        # carries `value is None`, so `.value` yields null there.
        if field == "value":
            if isinstance(obj.value, float):
                # The language has no floats; a float-backed Amount (getgems
                # price) is render-only, so `.value` faults instead of leaking
                # a float into an integer output field.
                raise EvalError("`.value` on a float-backed Amount (the language has no floats)")
            return obj.value
        raise EvalError(f"unknown Amount accessor {field!r} (expected value)")
    try:
        return getattr(obj, field)
    except AttributeError:
        raise EvalError(f"{type(obj).__name__} object has no attribute {field!r}") from None


def parse_value(target: Any, types: list[str], nullable: bool,
                message_types: dict[str, type]) -> Any:
    """Parse a block, cell, or base64 BOC against types in source order."""
    if target is None:
        if nullable:
            return None
        raise EvalError("parse: target is null")

    get_body = getattr(target, "get_body", None)
    if get_body is not None:
        body_factory = get_body
    elif isinstance(target, Cell):
        body_factory = target.begin_parse
    elif isinstance(target, str):
        try:
            raw = base64.b64decode(target, validate=True)
            cell = Cell.from_boc(raw)[0]
        except Exception as ex:
            if nullable:
                return None
            raise EvalError(f"parse: target string is not a valid base64 BOC ({ex})") from ex
        body_factory = cell.begin_parse
    else:
        if nullable:
            return None
        raise EvalError(
            f"parse: target is not a block, cell, or string "
            f"(got {type(target).__name__})"
        )

    last_exc: Exception | None = None
    for name in types:
        cls = message_types.get(name)
        if cls is None:
            raise EvalError(f"message type {name!r} is not registered")
        try:
            return cls(body_factory())
        except Exception as ex:  # soft across type alternatives; total failure faults
            last_exc = ex
            continue
    if nullable:
        return None
    raise EvalError(
        f"parse: no message type in {list(types)} parsed the body ({last_exc})"
    )


# Sync evaluator (node-level `where_expr`)


def _sync_eval(
    e: dict,
    block: Block,
    entry_name: str | None = None,
    entry_block: Block | None = None,
    message_types: dict[str, type] | None = None,
) -> Any:
    k = e.get("k")
    if k in ("int", "str", "bool"):
        return e["v"]
    if k == "null":
        return None
    if k == "dotself":
        return block
    if k == "parse":
        return parse_value(
            _sync_eval(e["x"], block, entry_name, entry_block, message_types),
            e["types"], e.get("nullable", False), message_types or {},
        )
    if k == "dotfield":
        data = block.data
        if data is None:
            return None
        if isinstance(data, dict):
            if e["name"] not in data:
                raise EvalError(f"block data has no field {e['name']!r}")
            return data[e["name"]]
        return _access(data, e["name"], {})
    if k == "name":
        if entry_block is None or e["id"] != entry_name:
            raise EvalError(f"name {e['id']!r} is not bound in a `where` clause")
        return entry_block
    if k == "attr":
        return _access(
            _sync_eval(e["of"], block, entry_name, entry_block, message_types), e["name"], {}
        )
    if k == "call":
        # Loader guarantees builtin-only calls in where_expr (host fns and
        # lookups are async and rejected at load time).
        name = e["fn"]
        if name not in BUILTINS:
            raise EvalError(f"host fn {name!r} is not callable in a `where` clause")
        return _call_builtin(
            name,
            [_sync_eval(a, block, entry_name, entry_block, message_types) for a in e["args"]],
        )
    if k == "unary":
        if e["op"] == "not":
            return not _require_bool(_sync_eval(e["x"], block, entry_name, entry_block, message_types))
        if e["op"] == "-":
            return _neg_result(_sync_eval(e["x"], block, entry_name, entry_block, message_types))
        raise EvalError(f"unknown unary operator {e['op']!r}")
    if k == "bin":
        op = e["op"]
        if op == "and":
            return _require_bool(
                _sync_eval(e["l"], block, entry_name, entry_block, message_types)
            ) and _require_bool(_sync_eval(e["r"], block, entry_name, entry_block, message_types))
        if op == "or":
            return _require_bool(
                _sync_eval(e["l"], block, entry_name, entry_block, message_types)
            ) or _require_bool(_sync_eval(e["r"], block, entry_name, entry_block, message_types))
        if op == "??":
            left = _sync_eval(e["l"], block, entry_name, entry_block, message_types)
            return left if left is not None else _sync_eval(
                e["r"], block, entry_name, entry_block, message_types
            )
        left = _sync_eval(e["l"], block, entry_name, entry_block, message_types)
        right = _sync_eval(e["r"], block, entry_name, entry_block, message_types)
        if op in ("==", "!="):
            return _eq_result(op, left, right)
        if op in ("<", "<=", ">", ">="):
            return _ord_result(op, left, right)
        if op in ("+", "-", "*"):
            return _arith_result(op, left, right)
        raise EvalError(f"unknown binary operator {op!r}")
    if k == "ternary":
        if _require_bool(_sync_eval(e["cond"], block, entry_name, entry_block, message_types)):
            return _sync_eval(e["then"], block, entry_name, entry_block, message_types)
        return _sync_eval(e["else"], block, entry_name, entry_block, message_types)
    if k == "list":
        return [_sync_eval(it, block, entry_name, entry_block, message_types) for it in e["items"]]
    if k == "record":
        return {
            f["name"]: _sync_eval(f["expr"], block, entry_name, entry_block, message_types)
            for f in e["fields"]
        }
    if k == "lookup":
        raise EvalError("lookup is not evaluable in a `where` clause")
    raise EvalError(f"unsupported expression kind {k!r}")


def eval_where_expr(
    e: dict,
    block: Block,
    entry_name: str | None = None,
    entry_block: Block | None = None,
    message_types: dict[str, type] | None = None,
) -> bool:
    """Evaluate a node's inline `where (expr)` against the candidate block.

    Composed onto test_self, so it must never raise: an evaluation fault or a
    non-bool result simply fails the test.
    """
    try:
        return _require_bool(_sync_eval(e, block, entry_name, entry_block, message_types))
    except EvalError:
        return False
