"""Build-program executor over IR statement records (s-tagged dicts).

Semantic mirror of the frontend's BuildProgram (indexer/events/mch/eval.py):
parse is soft with first-success-wins alternatives; statements run in source
order; `reject when` returns None (match rejection); `failed`/`broken`
accumulate with or; flow effects are recorded and applied to the produced
block's value_flow, skipping any effect with a null argument.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from indexer.events.blocks.core import Block
from indexer.events.blocks.utils import AccountId, Amount
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir.expr import EvalError, Evaluator


@dataclass
class FlowEffect:
    kind: str          # "jetton" | "ton" | "fees"
    args: tuple        # evaluated argument values, arity per spec §9.8


@dataclass
class BuildOutcome:
    data: dict
    failed: bool
    broken: bool
    flows: list[FlowEffect]


class BuildProgram:
    """A matcher record's `build_program`, executable against a match environment.

    `run` returns a BuildOutcome, or None when a `reject when` fired — the
    caller must treat None exactly like a builder returning nothing (§10.7).
    EvalError propagates; the caller rejects and logs.
    """

    def __init__(self, stmts: list[dict], registries: Registries):
        self.stmts = stmts
        self.registries = registries

    async def run(self, env: dict[str, Any]) -> BuildOutcome | None:
        ev = Evaluator(dict(env), self.registries)
        data: dict | None = None
        failed = False
        broken = False
        flows: list[FlowEffect] = []
        for stmt in self.stmts:
            s = stmt.get("s")
            if s == "parse":
                self._do_parse(stmt, ev)
            elif s == "let":
                ev.env[stmt["name"]] = await ev.eval(stmt["expr"])
            elif s == "reject":
                if await ev.eval_bool(stmt["when"]):
                    return None
            elif s == "failed":
                failed = failed or await ev.eval_bool(stmt["when"])
            elif s == "broken":
                broken = broken or await ev.eval_bool(stmt["when"])
            elif s == "flow":
                args = tuple([await ev.eval(a) for a in stmt["args"]])
                flows.append(FlowEffect(kind=stmt["kind"], args=args))
            elif s == "out":
                data = {f["name"]: await ev.eval(f["expr"]) for f in stmt["fields"]}
            else:
                raise EvalError(f"unsupported build statement {s!r}")
        return BuildOutcome(
            data=data if data is not None else {},
            failed=failed,
            broken=broken,
            flows=flows,
        )

    def _do_parse(self, stmt: dict, ev: Evaluator) -> None:
        target = ev.env.get(stmt["target"])
        if target is None:
            return  # absent optional capture: `.body` null-propagates
        if isinstance(target, list):
            for b in target:
                if b is not None:
                    ev.bodies[id(b)] = self._soft_parse(b, stmt["types"])
        else:
            ev.bodies[id(target)] = self._soft_parse(target, stmt["types"])

    def _soft_parse(self, block: Any, msg_types: list[str]) -> Any:
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
