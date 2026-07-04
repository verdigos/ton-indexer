"""MCH build-language evaluator (spec §9, M4).

Runs a matcher's build statements against a match environment and produces
the data/flags/flow effects for the block the declarative build path emits.

Environment
    Names are pattern captures (a Block, a list per §7.2 cardinality — with
    None gaps — or None for an absent optional) and `let` bindings. Two
    well-known names are always provided by the synthesized build path:
    `consumed` (the full consumed block list, anchor first, engine traversal
    order, de-duplicated — same content as the registered-builder path's
    `Match._mch_consumed`) and `anchor` (the anchor block). A pattern capture
    of the same name shadows them.

Host functions
    A call whose callee is not a builtin resolves through `registries.fns`
    (async callables registered via `Registries.register_fn`) — the escape
    hatch for imperative build logic. Unlike builtins and lookups, null
    arguments are passed through to the callable (an absent optional capture
    is a legitimate input); any exception it raises becomes an EvalError.

Accessors on a capture value (Block)
    cap.msg     -> the message envelope, `block.get_message()`; envelope
                   fields (`source`, `destination`, `value`, `opcode`, ...)
                   are then plain attribute access on the Message record.
    cap.body    -> the parsed body; valid only after `parse cap as T`
                   (accessing it un-parsed is an EvalError). On a list
                   capture, `cap.body` is the parallel list of parsed bodies.
    cap.failed / cap.broken / cap.btype -> the block flags / type tag.
    Any other field on a Block value is an EvalError.
    Field access on any non-Block object (lookup results, message records,
    parsed bodies) is getattr; a missing attribute is an EvalError.

Null semantics
    - Field access on null -> null.
    - Builtin call with a null argument -> null (function not invoked).
    - `lookup` with a null argument -> null (repository not queried).
    - `==` / `!=` treat null structurally (null == null, null != non-null)
      without invoking user `__eq__` on None.
    - Ordered comparisons with null on either side -> false.
    - `??` returns the left value unless it is null; right side is lazy.

Booleans
    `and` / `or` / `not` and condition positions (`reject|failed|broken when`,
    ternary) require actual bools; anything else is an EvalError. `and`/`or`
    short-circuit.

Errors
    Every runtime fault raises EvalError. Per spec §10.7 the caller (the
    compiler's synthesized build path) treats it as a match rejection and
    logs a warning; it never aborts trace processing.

Parsing
    `parse` is soft: each declared type's constructor is tried in order on a
    fresh body slice; the first success wins, and if all raise the body is
    null (mirrors the legacy try/except fallbacks). `out` may appear anywhere
    among the statements (single occurrence, enforced by the resolver); all
    statements evaluate in source order, so a `reject when` after `out` still
    cancels the match.

Lookups are awaited sequentially at their source position. Batching them into
the spec §9.7 collect/fetch/evaluate phases is a later optimization.
"""
from __future__ import annotations

import base64
from dataclasses import dataclass
from typing import Any

from indexer.events.blocks.core import Block
from indexer.events.blocks.utils import AccountId, Amount, Asset
from indexer.events.mch import ast_
from indexer.events.mch.registry import Registries


class EvalError(Exception):
    """A build-expression runtime fault; the match is rejected (§10.7)."""


# ---------------------------------------------------------------------------
# Builtins (spec §9.6): name -> (callable, arity). Null arguments never reach
# the callable — the Call evaluator returns null first.
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
# Expression evaluator
# ---------------------------------------------------------------------------


class Evaluator:
    """Evaluates build-language expressions against an environment.

    `bodies` maps id(block) -> parsed message object (or None for a soft-parse
    failure); populated by BuildProgram's `parse` statements.
    """

    def __init__(self, env: dict[str, Any], registries: Registries,
                 bodies: dict[int, Any] | None = None):
        self.env = env
        self.registries = registries
        self.bodies: dict[int, Any] = bodies if bodies is not None else {}

    async def eval(self, e: ast_.Expr) -> Any:
        if isinstance(e, (ast_.IntLit, ast_.StrLit, ast_.BoolLit)):
            return e.value
        if isinstance(e, ast_.NullLit):
            return None
        if isinstance(e, ast_.NameRef):
            if e.name not in self.env:
                raise EvalError(f"name {e.name!r} is not bound")
            return self.env[e.name]
        if isinstance(e, ast_.FieldRef):
            raise EvalError(
                f"leading-dot field '.{e.field}' is only valid inside an "
                f"inline `where (expr)` clause, not in build statements"
            )
        if isinstance(e, ast_.FieldAccess):
            return self._access(await self.eval(e.target), e.field)
        if isinstance(e, ast_.Call):
            return await self._eval_call(e)
        if isinstance(e, ast_.LookupExpr):
            return await self._eval_lookup(e)
        if isinstance(e, ast_.UnaryOp):
            return await self._eval_unary(e)
        if isinstance(e, ast_.BinaryOp):
            return await self._eval_binary(e)
        if isinstance(e, ast_.Ternary):
            if await self.eval_bool(e.cond):
                return await self.eval(e.then)
            return await self.eval(e.orelse)
        raise EvalError(f"unsupported expression node {type(e).__name__}")

    async def eval_bool(self, e: ast_.Expr) -> bool:
        v = await self.eval(e)
        if not isinstance(v, bool):
            raise EvalError(
                f"condition must be a bool, got {type(v).__name__} ({v!r})"
            )
        return v

    def _access(self, obj: Any, field: str) -> Any:
        if obj is None:
            return None
        if isinstance(obj, Block):
            if field == "msg":
                try:
                    return obj.get_message()
                except Exception as ex:
                    raise EvalError(f"block has no message envelope: {ex}") from ex
            if field == "body":
                if id(obj) not in self.bodies:
                    raise EvalError(
                        "`.body` accessed on a capture that was not `parse`d"
                    )
                return self.bodies[id(obj)]
            if field == "failed":
                return obj.failed
            if field == "broken":
                return obj.broken
            if field == "btype":
                return obj.btype
            raise EvalError(
                f"unknown block accessor {field!r} "
                f"(expected msg/body/failed/broken/btype)"
            )
        if isinstance(obj, list):
            if field == "body":
                out = []
                for b in obj:
                    if b is None:
                        out.append(None)
                    elif id(b) not in self.bodies:
                        raise EvalError(
                            "`.body` accessed on a list capture that was not `parse`d"
                        )
                    else:
                        out.append(self.bodies[id(b)])
                return out
            raise EvalError(f"field {field!r} is not defined on a list capture")
        try:
            return getattr(obj, field)
        except AttributeError:
            raise EvalError(
                f"{type(obj).__name__} object has no attribute {field!r}"
            ) from None

    async def _eval_call(self, e: ast_.Call) -> Any:
        if not isinstance(e.callee, ast_.NameRef):
            raise EvalError("only builtin or registered host-fn names are callable")
        entry = BUILTINS.get(e.callee.name)
        if entry is None:
            return await self._eval_host_fn(e)
        fn, arity = entry
        if len(e.args) != arity:
            raise EvalError(
                f"builtin {e.callee.name} takes {arity} argument(s), got {len(e.args)}"
            )
        args = [await self.eval(a) for a in e.args]
        if any(a is None for a in args):
            return None
        try:
            return fn(*args)
        except Exception as ex:
            raise EvalError(f"builtin {e.callee.name} failed: {ex}") from ex

    async def _eval_host_fn(self, e: ast_.Call) -> Any:
        fn = self.registries.fns.get(e.callee.name)
        if fn is None:
            raise EvalError(f"unknown builtin or host fn {e.callee.name!r}")
        # No null short-circuit: an absent optional capture is a legitimate
        # argument for imperative host logic (unlike builtins/lookups).
        args = [await self.eval(a) for a in e.args]
        try:
            return await fn(*args)
        except Exception as ex:
            raise EvalError(f"host fn {e.callee.name} failed: {ex}") from ex

    async def _eval_lookup(self, e: ast_.LookupExpr) -> Any:
        fn = self.registries.lookups.get(e.kind)
        if fn is None:
            raise EvalError(f"lookup kind {e.kind!r} is not registered")
        args = [await self.eval(a) for a in e.args]
        if any(a is None for a in args):
            return None
        try:
            return await fn(*args)
        except Exception as ex:
            raise EvalError(f"lookup {e.kind} failed: {ex}") from ex

    async def _eval_unary(self, e: ast_.UnaryOp) -> Any:
        if e.op == "not":
            return not await self.eval_bool(e.operand)
        if e.op == "-":
            v = await self.eval(e.operand)
            if v is None:
                return None
            try:
                return -v
            except TypeError as ex:
                raise EvalError(f"unary '-' on {type(v).__name__}: {ex}") from ex
        raise EvalError(f"unknown unary operator {e.op!r}")

    async def _eval_binary(self, e: ast_.BinaryOp) -> Any:
        op = e.op
        if op == "and":
            return await self.eval_bool(e.left) and await self.eval_bool(e.right)
        if op == "or":
            return await self.eval_bool(e.left) or await self.eval_bool(e.right)
        if op == "??":
            left = await self.eval(e.left)
            return left if left is not None else await self.eval(e.right)

        left = await self.eval(e.left)
        right = await self.eval(e.right)
        if op in ("==", "!="):
            if left is None or right is None:
                eq = left is None and right is None
            else:
                try:
                    eq = bool(left == right)
                except Exception as ex:
                    raise EvalError(f"'==' failed: {ex}") from ex
            return eq if op == "==" else not eq
        if op in ("<", "<=", ">", ">="):
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
        raise EvalError(f"unknown binary operator {op!r}")


# ---------------------------------------------------------------------------
# Build program
# ---------------------------------------------------------------------------


@dataclass
class FlowEffect:
    kind: str          # "jetton" | "ton" | "fees"
    args: tuple        # evaluated argument values, arity per §9.8


@dataclass
class BuildOutcome:
    data: dict
    failed: bool
    broken: bool
    flows: list[FlowEffect]


class BuildProgram:
    """A matcher's build statements, executable against a match environment.

    `run` returns a BuildOutcome, or None when a `reject when` fired — the
    caller must treat None exactly like a builder returning nothing (§10.7).
    EvalError propagates; the caller rejects and logs.
    """

    def __init__(self, stmts: tuple[ast_.BuildStmt, ...], registries: Registries):
        self.stmts = stmts
        self.registries = registries

    async def run(self, env: dict[str, Any]) -> BuildOutcome | None:
        ev = Evaluator(dict(env), self.registries)
        data: dict | None = None
        failed = False
        broken = False
        flows: list[FlowEffect] = []
        for stmt in self.stmts:
            if isinstance(stmt, ast_.ParseStmt):
                self._do_parse(stmt, ev)
            elif isinstance(stmt, ast_.LetStmt):
                ev.env[stmt.name] = await ev.eval(stmt.value)
            elif isinstance(stmt, ast_.RejectStmt):
                if await ev.eval_bool(stmt.condition):
                    return None
            elif isinstance(stmt, ast_.FailedStmt):
                failed = failed or await ev.eval_bool(stmt.condition)
            elif isinstance(stmt, ast_.BrokenStmt):
                broken = broken or await ev.eval_bool(stmt.condition)
            elif isinstance(stmt, ast_.FlowStmt):
                args = tuple([await ev.eval(a) for a in stmt.args])
                flows.append(FlowEffect(kind=stmt.kind, args=args))
            elif isinstance(stmt, ast_.OutStmt):
                data = {f.name: await ev.eval(f.value) for f in stmt.fields}
            else:
                raise EvalError(f"unsupported build statement {type(stmt).__name__}")
        return BuildOutcome(
            data=data if data is not None else {},
            failed=failed,
            broken=broken,
            flows=flows,
        )

    def _do_parse(self, stmt: ast_.ParseStmt, ev: Evaluator) -> None:
        target = ev.env.get(stmt.capture)
        if target is None:
            return  # absent optional capture: `.body` null-propagates
        if isinstance(target, list):
            for b in target:
                if b is not None:
                    ev.bodies[id(b)] = self._soft_parse(b, stmt.msg_types)
        else:
            ev.bodies[id(target)] = self._soft_parse(target, stmt.msg_types)

    def _soft_parse(self, block: Any, msg_types: tuple[str, ...]) -> Any:
        for name in msg_types:
            cls = self.registries.message_types.get(name)
            if cls is None:
                raise EvalError(f"message type {name!r} is not registered")
            try:
                return cls(block.get_body())
            except Exception:
                continue  # soft: try the next alternative
        return None


def apply_flows(block: Block, flows: list[FlowEffect]) -> None:
    """Apply evaluated flow effects to a produced block's value_flow.

    An effect with any null argument is skipped: flow statements are
    unconditional in the language, so a leg tied to an absent optional
    capture must degrade to a no-op rather than reject the match.
    """
    for eff in flows:
        if any(a is None for a in eff.args):
            continue
        if eff.kind == "jetton":
            owner, master, amt = eff.args
            block.value_flow.add_jetton(_as_account(owner), _as_account(master), _as_int(amt))
        elif eff.kind == "ton":
            account, amt = eff.args
            block.value_flow.add_ton(_as_account(account), _as_int(amt))
        elif eff.kind == "fees":
            account, amt = eff.args
            block.value_flow.add_fees(_as_account(account), _as_int(amt))
        else:
            raise EvalError(f"unknown flow kind {eff.kind!r}")


def _as_account(x: Any) -> AccountId:
    return x if isinstance(x, AccountId) else AccountId(x)


def _as_int(x: Any) -> int:
    if isinstance(x, Amount):
        return x.value
    try:
        return int(x)
    except (TypeError, ValueError) as ex:
        raise EvalError(f"flow amount must be numeric, got {type(x).__name__}") from ex
