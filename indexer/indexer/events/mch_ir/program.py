"""Build-program executor over IR statement records (s-tagged dicts).

Semantic mirror of the frontend's BuildProgram (indexer/events/mch/eval.py):
parse is soft with first-success-wins alternatives; statements run in source
order; `reject when` returns None (match rejection); `failed`/`broken`
accumulate with or.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import Any

from indexer.events.mch.registry import Registries
from indexer.events.mch_ir.expr import EvalError, Evaluator

logger = logging.getLogger(__name__)


@dataclass
class BuildOutcome:
    data: dict
    failed: bool
    broken: bool
    # Produced btype selected by a `produces switch`; None means the matcher's
    # declared produces[0] is used (the non-switch case).
    btype: str | None = None


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
        btype: str | None = None
        failed = False
        broken = False
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
            elif s == "out":
                data = await self._eval_out_fields(stmt["fields"], ev)
            elif s == "switch":
                btype, data = await self._eval_switch(stmt["branches"], ev)
            else:
                raise EvalError(f"unsupported build statement {s!r}")
        return BuildOutcome(
            data=data if data is not None else {},
            failed=failed,
            broken=broken,
            btype=btype,
        )

    @staticmethod
    async def _eval_out_fields(fields: list[dict], ev: Evaluator) -> dict:
        """Evaluate `out` fields into a data map. An `optional` field whose value
        is null is omitted from the map (present-vs-absent key, §9.5)."""
        data: dict = {}
        for f in fields:
            value = await ev.eval(f["expr"])
            if f.get("optional") and value is None:
                continue
            data[f["name"]] = value
        return data

    async def _eval_switch(self, branches: list[dict], ev: Evaluator) -> tuple[str, dict]:
        """Evaluate a `produces switch`: the first branch whose `when` is true
        (or the `else` arm, which has no `when`) selects the produced btype and
        its `out` fields. No branch matching is a fault (reject)."""
        for b in branches:
            if "when" not in b or await ev.eval_bool(b["when"]):
                return b["btype"], await self._eval_out_fields(b["fields"], ev)
        raise EvalError("produces switch: no branch matched and no `else` arm")

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
        last_exc: Exception | None = None
        for name in msg_types:
            cls = self.registries.message_types.get(name)
            if cls is None:
                raise EvalError(f"message type {name!r} is not registered")
            try:
                return cls(block.get_body())
            except Exception as ex:
                last_exc = ex
                continue  # soft: try the next alternative
        # Every alternative failed. The body stays null (soft, §6.1) and an
        # expression handles it with `??`/`reject`; but the swallow used to be
        # invisible (report F1/F6). A debug signal turns "why is .body null?"
        # into one grep. The compile-time R023 diagnostic catches the common
        # cause (parsing a composed `btype` capture) at author time.
        logger.debug(
            "mch soft-parse failed for all of %s (btype=%s): %s",
            msg_types, getattr(block, "btype", "?"), last_exc,
        )
        return None
