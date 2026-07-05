"""Sync IR-expression evaluator — shared leaf module (T33).

Home of the single implementation of node-level inline `where (expr)`
evaluation, composed onto `test_self` by BOTH engines:

- the compiled path (mch/compiler.py) converts the ast_ expression to its IR
  encoding at compile time (mch/expr_ir.py) and wraps `test_self` with
  `eval_where_expr`;
- the v2 IR engine (mch_ir/engine.py) evaluates the artifact's `where_expr`
  node field with the same function.

Import boundary: mch_ir must not import the frontend (parser/ast_/compiler)
and the frontend must not import mch_ir, so the evaluator lives here as a leaf
that depends only on `blocks.core`/`blocks.utils` (the same footing as
mch/registry.py, which mch_ir already imports). mch_ir/expr.py re-exports
these names, keeping its public API unchanged.

Semantics (docs/mch-ir-poc.md): sync, dispatches on k-tagged IR dicts,
`dotfield` reads the candidate block's `data`, missing field / any EvalError /
non-bool result fails the test (False) — never raises out of matching. Async
constructs (lookup, host fns) are rejected before run time: by the IR loader
(loader._check_where_expr_sync) and by the frontend compiler (C013).

The strict operator cores, builtins table and `_access` also live here because
the async build-program evaluator (mch_ir/expr.py Evaluator) shares them.
"""
from __future__ import annotations

import base64
from typing import Any

from pytoniq_core import Cell

from indexer.events.blocks.core import Block
from indexer.events.blocks.utils import AccountId, Amount, Asset


class EvalError(Exception):
    """A build-expression runtime fault; the match is rejected (spec §10.7)."""


# ---------------------------------------------------------------------------
# Builtins (spec §9.6): name -> (callable, arity). Null arguments never reach
# the callable — the call evaluator returns null first.
# ---------------------------------------------------------------------------


def _builtin_account(x: Any) -> AccountId:
    return AccountId(x)


def _builtin_amount(x: Any) -> Amount:
    return Amount(x.value if isinstance(x, Amount) else int(x))


def _builtin_asset(jetton_master: Any) -> Asset:
    return Asset(is_ton=False, jetton_address=jetton_master)


def _builtin_ton_asset() -> Asset:
    return Asset(is_ton=True)


def _builtin_addr_none() -> AccountId:
    # The addr_none AccountId — distinct from a null (missing) address. Mirrors
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


# ---------------------------------------------------------------------------
# List combinators (spec §9.6). Operate on list captures / list literals. Edge
# semantics are fixed and total (no host-specific behaviour):
#   - a null list argument never reaches these (the call evaluator returns null
#     first), so every argument here is a real list;
#   - first/last on an empty list -> null; sum/len on empty -> 0 / 0;
#   - sum coerces each element to an integer (Amount -> .value, int -> itself,
#     anything else -> fault) and is integer-valued;
#   - zip truncates to the shorter input (pairs are 2-element lists);
#   - map projects one field per element (dict key lookup, attribute fallback) —
#     no lambdas. Each combinator is a one-line loop in C++ (vector/map ops).
# ---------------------------------------------------------------------------


def _require_list(name: str, xs: Any) -> list:
    if not isinstance(xs, list):
        raise EvalError(f"{name} expects a list, got {type(xs).__name__}")
    return xs


def _as_amount_int(x: Any) -> int:
    if isinstance(x, Amount):
        return x.value
    if isinstance(x, bool):  # guard: bool is an int subclass, reject it
        raise EvalError("sum: bool is not summable")
    if isinstance(x, int):
        return x
    raise EvalError(f"sum: element is not an integer or Amount ({type(x).__name__})")


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


def _builtin_len(xs: Any) -> int:
    return len(_require_list("len", xs))


def _builtin_sum(xs: Any) -> int:
    return sum(_as_amount_int(x) for x in _require_list("sum", xs))


def _builtin_zip(xs: Any, ys: Any) -> list:
    xs = _require_list("zip", xs)
    ys = _require_list("zip", ys)
    return [[a, b] for a, b in zip(xs, ys)]


def _builtin_map(xs: Any, field: Any) -> list:
    return [_project(x, field) for x in _require_list("map", xs)]


BUILTINS: dict[str, tuple[Any, int]] = {
    "account":   (_builtin_account, 1),
    "amount":    (_builtin_amount, 1),
    "asset":     (_builtin_asset, 1),
    "ton_asset": (_builtin_ton_asset, 0),
    "addr_none": (_builtin_addr_none, 0),
    "b64":       (_builtin_b64, 1),
    "first":     (_builtin_first, 1),
    "last":      (_builtin_last, 1),
    "len":       (_builtin_len, 1),
    "sum":       (_builtin_sum, 1),
    "zip":       (_builtin_zip, 2),
    "map":       (_builtin_map, 2),
}


# ---------------------------------------------------------------------------
# Operator cores shared by the async and sync evaluators. Lazy operators
# (and/or/??/ternary) stay in each evaluator; these are the strict ones.
# ---------------------------------------------------------------------------


def _eq_result(op: str, left: Any, right: Any) -> bool:
    """`==` / `!=` with structural null handling (never calls user __eq__ on None)."""
    if left is None or right is None:
        eq = left is None and right is None
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
    try:
        return getattr(obj, field)
    except AttributeError:
        raise EvalError(f"{type(obj).__name__} object has no attribute {field!r}") from None


# ---------------------------------------------------------------------------
# Sync evaluator (node-level `where_expr`)
# ---------------------------------------------------------------------------


def _sync_eval(e: dict, block: Block) -> Any:
    k = e.get("k")
    if k in ("int", "str", "bool"):
        return e["v"]
    if k == "null":
        return None
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
        # No environment exists at test_self time; captures are not yet bound.
        raise EvalError(f"name {e['id']!r} is not bound in a `where` clause")
    if k == "attr":
        return _access(_sync_eval(e["of"], block), e["name"], {})
    if k == "call":
        # Loader guarantees builtin-only calls in where_expr (host fns and
        # lookups are async and rejected at load time).
        name = e["fn"]
        if name not in BUILTINS:
            raise EvalError(f"host fn {name!r} is not callable in a `where` clause")
        return _call_builtin(name, [_sync_eval(a, block) for a in e["args"]])
    if k == "unary":
        if e["op"] == "not":
            return not _require_bool(_sync_eval(e["x"], block))
        if e["op"] == "-":
            return _neg_result(_sync_eval(e["x"], block))
        raise EvalError(f"unknown unary operator {e['op']!r}")
    if k == "bin":
        op = e["op"]
        if op == "and":
            return _require_bool(_sync_eval(e["l"], block)) and _require_bool(_sync_eval(e["r"], block))
        if op == "or":
            return _require_bool(_sync_eval(e["l"], block)) or _require_bool(_sync_eval(e["r"], block))
        if op == "??":
            left = _sync_eval(e["l"], block)
            return left if left is not None else _sync_eval(e["r"], block)
        left = _sync_eval(e["l"], block)
        right = _sync_eval(e["r"], block)
        if op in ("==", "!="):
            return _eq_result(op, left, right)
        if op in ("<", "<=", ">", ">="):
            return _ord_result(op, left, right)
        raise EvalError(f"unknown binary operator {op!r}")
    if k == "ternary":
        if _require_bool(_sync_eval(e["cond"], block)):
            return _sync_eval(e["then"], block)
        return _sync_eval(e["else"], block)
    if k == "list":
        return [_sync_eval(it, block) for it in e["items"]]
    if k == "record":
        return {f["name"]: _sync_eval(f["expr"], block) for f in e["fields"]}
    if k == "lookup":
        raise EvalError("lookup is not evaluable in a `where` clause")
    raise EvalError(f"unsupported expression kind {k!r}")


def eval_where_expr(e: dict, block: Block) -> bool:
    """Evaluate a node's inline `where (expr)` against the candidate block.

    Composed onto test_self, so it must never raise: an evaluation fault or a
    non-bool result simply fails the test.
    """
    try:
        return _require_bool(_sync_eval(e, block))
    except EvalError:
        return False
