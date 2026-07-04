"""Expression evaluator over IR JSON trees (k-tagged dicts, docs/mch-ir-poc.md).

Semantic mirror of the frontend evaluator (indexer/events/mch/eval.py) — same
null propagation, bool-only conditions, builtin set/arity, lookup and host-fn
resolution, and EvalError-as-rejection contract — but it dispatches on the IR
encoding directly instead of ast_ nodes, so it needs nothing from the frontend.

Two evaluation surfaces:

- `Evaluator` — async, used by build programs. Env names are captures / lets /
  well-known names; `dotfield` is an error here (build statements have no
  current block).
- `eval_where_expr` — sync, used to compose a node's inline `where (expr)`
  onto `test_self`. `dotfield` reads the current block's `data` field; env
  names are unbound; async constructs (lookup, host fns) are rejected at
  artifact load time (see loader._check_where_expr). Any EvalError or non-bool
  result makes the test fail (False), never aborts matching.
"""
from __future__ import annotations

import base64
from typing import Any

from indexer.events.blocks.core import Block
from indexer.events.blocks.utils import AccountId, Amount, Asset
from indexer.events.mch.registry import Registries


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


def _builtin_b64(x: Any) -> str:
    return base64.b64encode(x).decode("utf-8")


BUILTINS: dict[str, tuple[Any, int]] = {
    "account":   (_builtin_account, 1),
    "amount":    (_builtin_amount, 1),
    "asset":     (_builtin_asset, 1),
    "ton_asset": (_builtin_ton_asset, 0),
    "b64":       (_builtin_b64, 1),
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
        raise EvalError(
            f"unknown block accessor {field!r} (expected msg/body/failed/broken/btype)"
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
    try:
        return getattr(obj, field)
    except AttributeError:
        raise EvalError(f"{type(obj).__name__} object has no attribute {field!r}") from None


# ---------------------------------------------------------------------------
# Async evaluator (build programs)
# ---------------------------------------------------------------------------


class Evaluator:
    """Evaluates IR expression trees against an environment.

    `bodies` maps id(block) -> parsed message object (or None for a soft-parse
    failure); populated by BuildProgram's `parse` statements.
    """

    def __init__(self, env: dict[str, Any], registries: Registries,
                 bodies: dict[int, Any] | None = None):
        self.env = env
        self.registries = registries
        self.bodies: dict[int, Any] = bodies if bodies is not None else {}

    async def eval(self, e: dict) -> Any:
        k = e.get("k")
        if k in ("int", "str", "bool"):
            return e["v"]
        if k == "null":
            return None
        if k == "name":
            name = e["id"]
            if name not in self.env:
                raise EvalError(f"name {name!r} is not bound")
            return self.env[name]
        if k == "dotfield":
            raise EvalError(
                f"leading-dot field '.{e['name']}' is only valid inside an "
                f"inline `where (expr)` clause, not in build statements"
            )
        if k == "attr":
            return _access(await self.eval(e["of"]), e["name"], self.bodies)
        if k == "call":
            return await self._eval_call(e)
        if k == "lookup":
            return await self._eval_lookup(e)
        if k == "unary":
            return await self._eval_unary(e)
        if k == "bin":
            return await self._eval_binary(e)
        if k == "ternary":
            if await self.eval_bool(e["cond"]):
                return await self.eval(e["then"])
            return await self.eval(e["else"])
        raise EvalError(f"unsupported expression kind {k!r}")

    async def eval_bool(self, e: dict) -> bool:
        return _require_bool(await self.eval(e))

    async def _eval_call(self, e: dict) -> Any:
        name = e["fn"]
        if name not in BUILTINS:
            return await self._eval_host_fn(e)
        args = [await self.eval(a) for a in e["args"]]
        return _call_builtin(name, args)

    async def _eval_host_fn(self, e: dict) -> Any:
        name = e["fn"]
        fn = self.registries.fns.get(name)
        if fn is None:
            raise EvalError(f"unknown builtin or host fn {name!r}")
        # No null short-circuit: an absent optional capture is a legitimate
        # argument for imperative host logic (unlike builtins/lookups).
        args = [await self.eval(a) for a in e["args"]]
        try:
            return await fn(*args)
        except Exception as ex:
            raise EvalError(f"host fn {name} failed: {ex}") from ex

    async def _eval_lookup(self, e: dict) -> Any:
        name = e["name"]
        fn = self.registries.lookups.get(name)
        if fn is None:
            raise EvalError(f"lookup kind {name!r} is not registered")
        args = [await self.eval(a) for a in e["args"]]
        if any(a is None for a in args):
            return None
        try:
            return await fn(*args)
        except Exception as ex:
            raise EvalError(f"lookup {name} failed: {ex}") from ex

    async def _eval_unary(self, e: dict) -> Any:
        if e["op"] == "not":
            return not await self.eval_bool(e["x"])
        if e["op"] == "-":
            return _neg_result(await self.eval(e["x"]))
        raise EvalError(f"unknown unary operator {e['op']!r}")

    async def _eval_binary(self, e: dict) -> Any:
        op = e["op"]
        if op == "and":
            return await self.eval_bool(e["l"]) and await self.eval_bool(e["r"])
        if op == "or":
            return await self.eval_bool(e["l"]) or await self.eval_bool(e["r"])
        if op == "??":
            left = await self.eval(e["l"])
            return left if left is not None else await self.eval(e["r"])

        left = await self.eval(e["l"])
        right = await self.eval(e["r"])
        if op in ("==", "!="):
            return _eq_result(op, left, right)
        if op in ("<", "<=", ">", ">="):
            return _ord_result(op, left, right)
        raise EvalError(f"unknown binary operator {op!r}")


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
