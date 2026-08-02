"""Evaluate k-tagged IR expression trees.

This mirrors the frontend evaluator (indexer/events/mch/eval.py), including the
null propagation, bool-only conditions, builtin set/arity, lookup and host-fn
resolution and EvalError-as-rejection contract. It dispatches on the IR
encoding directly instead of ast_ nodes, so it needs nothing from the frontend.

Two evaluation surfaces:

- `Evaluator` is async and used by build programs. Env names are captures, lets,
  well-known names; `dotfield` is an error here (build statements have no
  current block).
- `eval_where_expr` is synchronous and composes a node's inline `where (expr)`
  onto `test_self`. `dotfield` reads the current block's `data` field; env
  names are unbound; async constructs (lookup, host fns) are rejected at
  artifact load time (see loader._check_where_expr). Any EvalError or non-bool
  result makes the test fail (False), never aborts matching.

The sync evaluator, builtins table and strict operator cores live in the
shared leaf `indexer/events/mch/expr_eval.py`. The compiled path
composes the same `eval_where_expr` onto its `test_self`, and neither package
imports the other. The names are re-exported here so this
module's public API is unchanged.
"""
from __future__ import annotations

from typing import Any

from indexer.events.mch.expr_eval import (  # noqa: F401; re-exports
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
        if k == "mapc":
            return await self._eval_mapc(e)
        if k == "quant":
            return await self._eval_quant(e)
        if k == "parse":
            return await self._eval_parse(e)
        raise EvalError(f"unsupported expression kind {k!r}")

    async def eval_bool(self, e: dict) -> bool:
        return _require_bool(await self.eval(e))

    async def _eval_mapc(self, e: dict) -> Any:
        """`map(xs as VAR => body)`. null xs -> null; non-list xs ->
        fault; empty -> []. Binds VAR in `self.env` per element (a new scope
        frame), restoring the prior binding afterward so `body` may shadow an
        outer capture named VAR."""
        xs = await self.eval(e["xs"])
        if xs is None:
            return None
        if not isinstance(xs, list):
            raise EvalError(f"map comprehension expects a list, got {type(xs).__name__}")
        var, body = e["var"], e["body"]
        with self._bind_element(var):
            out = []
            for el in xs:
                self.env[var] = el
                out.append(await self.eval(body))
        return out

    async def _eval_quant(self, e: dict) -> Any:
        """`any|all(xs as VAR => body)`. null xs -> null; non-list xs
        -> fault; empty -> false (any) / true (all). Short-circuits like
        std::any_of / std::all_of. `body` must be a bool."""
        xs = await self.eval(e["xs"])
        if xs is None:
            return None
        op = e["op"]
        if not isinstance(xs, list):
            raise EvalError(f"{op} comprehension expects a list, got {type(xs).__name__}")
        var, body = e["var"], e["body"]
        result = op == "all"  # all of empty = True, any of empty = False
        with self._bind_element(var):
            for el in xs:
                self.env[var] = el
                b = _require_bool(await self.eval(body))
                if op == "any" and b:
                    result = True
                    break
                if op == "all" and not b:
                    result = False
                    break
        return result

    def _bind_element(self, var: str):
        """Context manager: introduce comprehension element var `var`, restoring
        the previous binding (or absence) on exit. See _eval_mapc/_eval_quant."""
        env = self.env
        _UNSET = object()

        class _Frame:
            def __enter__(self_):
                self_.saved = env.get(var, _UNSET)
                return self_

            def __exit__(self_, *exc):
                if self_.saved is _UNSET:
                    env.pop(var, None)
                else:
                    env[var] = self_.saved
                return False

        return _Frame()

    async def _eval_parse(self, e: dict) -> Any:
        """Parse the target block's body against message types in source order.

        The first success wins. The expression faults and rejects the build on
        a null/non-block target or when no type parses. There is no
        soft-null variant. Returns the parsed message object."""
        target = await self.eval(e["x"])
        if target is None:
            raise EvalError("parse: target is null")
        get_body = getattr(target, "get_body", None)
        if get_body is None:
            raise EvalError(f"parse: target is not a block (got {type(target).__name__})")
        body = get_body()
        last_exc: Exception | None = None
        for name in e["types"]:
            cls = self.registries.message_types.get(name)
            if cls is None:
                raise EvalError(f"message type {name!r} is not registered")
            try:
                return cls(body)
            except Exception as ex:  # soft across type alternatives; total failure faults
                last_exc = ex
                continue
        raise EvalError(
            f"parse: no message type in {list(e['types'])} parsed the body ({last_exc})"
        )

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
