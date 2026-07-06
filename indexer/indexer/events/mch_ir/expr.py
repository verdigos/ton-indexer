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

The sync evaluator, builtins table and strict operator cores live in the
shared leaf `indexer/events/mch/expr_eval.py` (T33): the compiled path
composes the SAME `eval_where_expr` onto its `test_self`, and neither package
may import the other — see that module's docstring. Re-exported here so this
module's public API is unchanged.
"""
from __future__ import annotations

from typing import Any

from indexer.events.mch.expr_eval import (  # noqa: F401 — re-exports
    BUILTINS,
    EvalError,
    _access,
    _arith_result,
    _call_builtin,
    _eq_result,
    _neg_result,
    _ord_result,
    _require_bool,
    _sync_eval,
    eval_where_expr,
)
from indexer.events.mch.registry import Registries


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
        if k == "list":
            return [await self.eval(it) for it in e["items"]]
        if k == "record":
            return {f["name"]: await self.eval(f["expr"]) for f in e["fields"]}
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
        if op in ("+", "-", "*"):
            return _arith_result(op, left, right)
        raise EvalError(f"unknown binary operator {op!r}")
