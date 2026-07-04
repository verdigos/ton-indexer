from __future__ import annotations

import logging
import weakref
from dataclasses import dataclass
from types import SimpleNamespace
from typing import Any

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.basic_matchers import (
    BlockMatcher,
    BlockTypeMatcher,
    ContractMatcher,
    ExclusiveOrMatcher,
    GenericMatcher,
    OrMatcher,
    RecursiveMatcher,
)
from indexer.events.blocks.core import Block
from indexer.events.mch import ast_
from indexer.events.mch.btypes import produces_btype
from indexer.events.mch.eval import BuildProgram, EvalError, apply_flows
from indexer.events.mch.diagnostics import CompileError, DiagnosticBag, Span
from indexer.events.mch.expr_eval import BUILTINS as _EXPR_BUILTINS, eval_where_expr
from indexer.events.mch.expr_ir import emit_expr
from indexer.events.mch.recursion import RecursionStrategy, RecursiveMatcherStrategy
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import (
    CaptureInfo,
    RecursionClass,
    ResolvedFile,
    ResolvedMatcher,
)

logger = logging.getLogger(__name__)


@dataclass
class CompiledMatcher:
    name: str
    matcher: BlockMatcher
    match_cls: type
    # None for declarative matchers (build statements instead of a registered builder).
    build_fn_name: str | None
    produces_cls_name: str
    source_span: Span
    # Build statements (M4), source order; empty tuple for builder-based matchers.
    # Carried for IR emission — the runtime reads its own compiled program instead.
    build_stmts: tuple = ()
    # `shape` directive (§8.7): registered shaper name, None when absent.
    shape_fn_name: str | None = None


@dataclass
class CompileCtx:
    resolved: ResolvedFile
    registries: Registries
    recursion: RecursionStrategy
    bag: DiagnosticBag
    # Names of recursive rules currently being lowered; used to break cycles
    # by treating self-refs inside step/exit branches as a recursion sentinel.
    lowering_recursive: set[str] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.lowering_recursive is None:
            self.lowering_recursive = set()


# Subclass cache: WeakKeyDictionary keyed by Registries -> dict[(name, base) -> subclass].
# Using a weak key lets the cache entry vanish when its Registries is collected, avoiding
# stale closures if a new Registries happens to land at the same address as a dead one.
_GENERATED_SUBCLASSES: "weakref.WeakKeyDictionary[Registries, dict[tuple[str, type], type]]" = (
    weakref.WeakKeyDictionary()
)


class _MchRecursiveMatcher(RecursiveMatcher):
    """RecursiveMatcher specialization that records per-iteration consumed blocks.

    Used by the synthesized walker to populate `list[T]` captures inside
    recursive rule bodies. Each iteration's consumed blocks land in
    `self._mch_iter_blocks` in iteration order BEFORE the engine's `set()`
    dedup step destroys order in the outer `blocks` list. The exit branch's
    consumed blocks (never part of an iteration) land in `_mch_exit_blocks`,
    the entry block in `_mch_entry_block` (only on success — the walker uses
    it to tell "participated in this match" from stale/absent), and the exact
    length of the last successful `try_build` result in `_mch_result_len`
    (lets the walker skip the recursion's set-shuffled slice precisely and
    keep binding captures positioned after it, spec §7.2).

    Non-reentrant: state is reset at the start of every
    `process_child_matcher` call. The engine matches sequentially; a probe
    whose `test_self` fails never reaches `process_child_matcher`, so it
    cannot clobber a previous success (same discipline as pre-T18).
    """

    def __init__(self, repeating_matcher, exit_matcher, optional, has_pure_exit,
                 step_capture_names=frozenset(), exit_capture_names=frozenset()):
        super().__init__(repeating_matcher, exit_matcher, optional)
        self._mch_has_pure_exit = has_pure_exit
        self._mch_step_capture_names = step_capture_names
        self._mch_exit_capture_names = exit_capture_names
        self._mch_iter_blocks: list[list] = []
        self._mch_exit_blocks: list = []
        self._mch_entry_block = None
        self._mch_result_len = 0

    async def try_build(self, block):
        result = await super().try_build(block)
        if result is not None:
            self._mch_result_len = len(result)
        return result

    async def process_child_matcher(self, block, blocks, child_matched):
        self._mch_iter_blocks = []
        self._mch_exit_blocks = []
        self._mch_entry_block = None
        current_blocks = [block]
        blocks_to_add = []
        finished = False
        while True:
            block_for_check = current_blocks[:]
            current_blocks = []
            iter_consumed: list = []
            for c in block_for_check:
                if self._mch_has_pure_exit and self.exit_matcher is not None:
                    r = await self.exit_matcher.try_build(c)
                    if r is not None:
                        blocks_to_add.extend(r)
                        self._mch_exit_blocks = list(r)
                        finished = True
                if finished:
                    break
                r = await self.repeating_matcher.try_build(c)
                if r is not None:
                    iter_consumed.extend(r)
                    blocks_to_add.extend(r)
                    for b in r:
                        current_blocks.extend(b.next_blocks)
                    break
            if iter_consumed:
                self._mch_iter_blocks.append(iter_consumed)
            if finished:
                break
            if len(current_blocks) == 0:
                child_matched = False
                break

        if len(blocks_to_add) > 0:
            blocks.extend(set(blocks_to_add))
            child_matched = True
            self._mch_entry_block = block
        return child_matched


class _MchCyclicRefMatcher(BlockMatcher):
    """Reference site of a cyclic-strategy recursive rule (spec §10.3 cyclic
    descent, [M2R, R11/T28]).

    The rule body lowers ONCE per compile to a shared matcher graph whose
    in-body self-references point straight back to the body root — a true
    back-edge, exactly the legacy hand-built cycle (e.g. ToncoSwapBlockMatcher's
    `peer_swap.child = payout; payout.child = Or([... -> peer_swap, finals])`).
    Matching is plain recursive descent through that cycle, bounded by trace
    depth; there is no frontier walk and no step/exit split, so "hop
    continuation is mandatory" branches disambiguate from same-prefix final
    branches by looking down the actual chain — the shape the frontier
    strategy's exit-first order cannot express.

    Instances are per-reference-site and delegate to the shared body root:
    in-body sites carry `_mch_cyclic_inner=True` (IR: emitted as a plain
    back-edge to the body root's node id), outer sites emit as a
    `{"kind": "recursive", "strategy": "cyclic"}` node. On success the entry
    block and exact result length are recorded so the capture walker can skip
    this reference's set-shuffled consumed slice precisely (same discipline as
    `_MchRecursiveMatcher`). Captures inside cyclic rule bodies are rejected at
    compile time (C012) — builders for cyclic-rule matchers are consumed-set
    driven via `Match._mch_consumed`.
    """

    def __init__(self, target: BlockMatcher | None = None):
        super().__init__()
        self._mch_cyclic_target = target
        self._mch_cyclic_inner = False
        self._mch_entry_block = None
        self._mch_result_len = 0

    def test_self(self, block):
        return self._mch_cyclic_target.test_self(block)

    async def try_build(self, block):
        # Pure delegation: the body root's try_build already handles children,
        # parents and aux auto-appends; re-wrapping would double-append.
        result = await self._mch_cyclic_target.try_build(block)
        if result is not None:
            self._mch_entry_block = block
            self._mch_result_len = len(result)
        return result


def compile_(
    resolved: ResolvedFile,
    registries: Registries,
    recursion: RecursionStrategy | None = None,
) -> list[CompiledMatcher]:
    """Compile a resolved file to a list of CompiledMatcher.

    Function is named `compile_` (trailing underscore) to avoid shadowing the builtin.
    """
    bag = DiagnosticBag()
    ctx = CompileCtx(
        resolved=resolved,
        registries=registries,
        recursion=recursion or RecursiveMatcherStrategy(),
        bag=bag,
    )

    out: list[CompiledMatcher] = []
    for rm in resolved.matchers:
        out.append(_compile_matcher(rm, ctx))

    if bag.has_errors:
        raise CompileError(bag)

    return out


def _compile_matcher(rm: ResolvedMatcher, ctx: CompileCtx) -> CompiledMatcher:
    decl = rm.decl

    _check_no_capture_after_recursion(decl.pattern, decl.span, ctx)
    _check_no_bare_children_block(decl.pattern, ctx)
    if ctx.bag.has_errors:
        raise CompileError(ctx.bag)

    if decl.produces_primary not in ctx.registries.block_types:
        ctx.bag.error(
            "C003_UNKNOWN_BLOCK_TYPE",
            f"matcher {decl.name!r}: produces type {decl.produces_primary!r} is not registered",
            decl.span,
        )
    if decl.build is not None and decl.build not in ctx.registries.builders:
        ctx.bag.error(
            "C002_UNKNOWN_BUILDER",
            f"matcher {decl.name!r}: builder {decl.build!r} is not registered",
            decl.span,
        )
    if decl.shape is not None and decl.shape not in ctx.registries.shapers:
        ctx.bag.error(
            "C011_UNKNOWN_SHAPER",
            f"matcher {decl.name!r}: shaper {decl.shape!r} is not registered",
            decl.span,
        )

    # Lower the pattern relative to the anchor atom. For opcode-set anchors
    # (spec §8.3 M2) the whole exclusive alternative group IS the anchor atom,
    # so chain edges around it wire exactly as around a single anchor node.
    anchor_atom: ast_.PatternExpr = (
        rm.anchor_group if rm.anchor_group is not None else rm.anchor_node
    )
    parent_m, child_m, children_ms = _lower_relative_to_anchor(decl.pattern, anchor_atom, ctx)

    # Build the synthetic subclass based on the anchor head.
    head = rm.anchor_node.head
    if rm.anchor_group is not None:
        # Set-membership anchor: test_self is a membership check over the group's
        # branch heads, mirroring hand-written matchers like JettonMintBlockMatcher
        # (`opcode in [JettonMint.opcode, MinterJettonMint.opcode]`, jettons.py).
        # Heads may mix op/btype (union) and each branch may carry a `where`
        # predicate ([M2R, R09]): membership first, then the matching branch's
        # predicate.
        if rm.anchor_group.exclusive:
            _check_redundant_exclusive_heads(rm.anchor_group, ctx)
        opcode_set, btype_set = _anchor_group_head_sets(rm.anchor_group, ctx)
        branch_descs = _anchor_group_branch_descs(rm.anchor_group, ctx)
        any_where = any(w is not None for _k, _v, w in branch_descs)
        base = GenericMatcher
        init_kwargs: dict[str, Any] = dict(
            test_self_func=(
                _make_head_set_test_with_where(branch_descs, ctx.registries)
                if any_where
                else _make_head_set_test(opcode_set, btype_set)
            ),
            child_matcher=child_m,
            parent_matcher=parent_m,
        )
    elif isinstance(head, ast_.OpHead):
        opcode = head.ref if isinstance(head.ref, int) else ctx.resolved.opcodes.get(head.ref) or ctx.registries.opcodes.get(head.ref)
        base = ContractMatcher
        init_kwargs = dict(
            opcode=opcode,
            child_matcher=child_m,
            parent_matcher=parent_m,
            children_matchers=children_ms,
            include_excess=decl.include_excess,
        )
    elif isinstance(head, ast_.BTypeHead):
        base = BlockTypeMatcher
        init_kwargs = dict(
            block_type=head.name,
            child_matcher=child_m,
            parent_matcher=parent_m,
        )
        # BlockTypeMatcher doesn't accept children_matchers in its __init__; if present,
        # wrap by assigning after construction.
    elif isinstance(head, ast_.PredHead):
        # Predicate anchor ([M2R, R10]): no opcode/btype prefilter — `test_self`
        # IS the predicate, full-scanned over every candidate block. The IR
        # anchor record carries the predicate name (kind "pred"); the root node
        # stays permissive, mirroring the set-membership anchor lowering.
        anchor_pred_fn = ctx.registries.predicates[head.name]
        base = GenericMatcher
        init_kwargs = dict(
            test_self_func=anchor_pred_fn,
            child_matcher=child_m,
            parent_matcher=parent_m,
        )
    else:
        # Resolver should have rejected this; defensive guard.
        ctx.bag.error(
            "C001_RECURSION_SHAPE_UNSUPPORTED",
            f"matcher {decl.name!r}: unsupported anchor head {type(head).__name__}",
            decl.span,
        )
        raise CompileError(ctx.bag)

    subclass = _make_subclass(decl.name, base, rm, ctx)
    instance: BlockMatcher = subclass(**init_kwargs)
    # Some bases don't accept children_matchers in __init__; apply post-hoc.
    if children_ms is not None and getattr(instance, "children_matchers", None) is None:
        instance.children_matchers = children_ms
    # include_bounces lives on the base; ensure flag is propagated.
    instance.include_bounces = decl.include_bounces
    instance.include_excess = decl.include_excess
    # Apply where-predicate on the anchor node, if any. For a set-membership
    # group anchor ([M2R, R09]) the per-branch `where` is already folded into
    # the group's membership test (and the IR `mixed` record); `anchor_node` is
    # only a representative branch, so its `where` must NOT be re-applied to the
    # whole matcher.
    anchor_pred_name = None if rm.anchor_group is not None else rm.anchor_node.where_predicate
    if anchor_pred_name is not None:
        anchor_pred = ctx.registries.predicates[anchor_pred_name]
        original_test = instance.test_self
        def _wrapped_test_self(block, _orig=original_test, _p=anchor_pred):
            return _orig(block) and _p(block)
        instance.test_self = _wrapped_test_self  # type: ignore[method-assign]
    # Tag the anchor instance with its capture name so the walker can bind it.
    if rm.anchor_capture:
        instance._mch_capture_name = rm.anchor_capture  # type: ignore[attr-defined]

    # IR emission metadata (read only by mch/ir_emit.py; no runtime behavior).
    # Lowering erases these into closures/synthesized classes, so stamp them on
    # the instance where the emitter's object-graph walk can recover them.
    instance._ir_produces = tuple(decl.produces)  # type: ignore[attr-defined]
    instance._ir_captures = tuple(  # type: ignore[attr-defined]
        (cname, "many" if ci.list_binding else ("opt" if ci.optional else "one"))
        for cname, ci in rm.captures.items()
    )
    if anchor_pred_name is not None:
        instance._ir_where = anchor_pred_name  # type: ignore[attr-defined]
    # Anchor-node inline `where (expr)` (T33 decision: supported on BOTH paths).
    # Composed onto test_self exactly like a nested node's, and stamped for IR
    # emission — the emitter's root-node record carries it, and the IR engine
    # already evaluates the root's where_expr in _test_head.
    if rm.anchor_group is None and rm.anchor_node.where_expr is not None:
        _compose_where_expr(instance, rm.anchor_node.where_expr, rm.anchor_node.span, ctx)
    if rm.anchor_group is not None:
        # Group-anchor branches carry only named `where` predicates in the IR
        # anchor record ([M2R, R09]); an inline expr there would be silently
        # dropped on both paths — reject instead (spec §6.8 no-silent-drop).
        for b in rm.anchor_group.branches:
            if isinstance(b, ast_.Node) and b.where_expr is not None:
                ctx.bag.error(
                    "C013_WHERE_EXPR_NOT_SYNC",
                    f"matcher {decl.name!r}: inline `where (expr)` is not "
                    f"supported on opcode-set anchor branches; use a named "
                    f"`where <predicate>` (spec §8.3 / [M2R, R09])",
                    b.span,
                )
    if rm.anchor_group is None and isinstance(rm.anchor_node.head, ast_.PredHead):
        # Predicate anchor ([M2R, R10]): IR anchor record kind "pred".
        instance._ir_anchor_pred = rm.anchor_node.head.name  # type: ignore[attr-defined]
    if rm.anchor_group is not None:
        # IR anchor emission ([M2R, R09]): a pure single-kind group with no
        # branch `where` keeps the T10 `opcode_set`/`btype` forms unchanged;
        # a mixed-kind group or any branch `where` emits the `mixed` branch form
        # (see ir_emit._anchor_record). Stamp exactly one representation.
        kinds = {k for k, _v, _w in branch_descs}
        if len(kinds) == 1 and not any_where:
            if kinds == {"op"}:
                instance._ir_anchor_opcodes = tuple(sorted(opcode_set))  # type: ignore[attr-defined]
            else:
                instance._ir_anchor_btypes = tuple(sorted(btype_set))  # type: ignore[attr-defined]
        else:
            instance._ir_anchor_branches = tuple(branch_descs)  # type: ignore[attr-defined]

    return CompiledMatcher(
        name=decl.name,
        matcher=instance,
        match_cls=subclass._mch_match_cls,
        build_fn_name=decl.build,
        produces_cls_name=decl.produces_primary,
        source_span=decl.span,
        build_stmts=tuple(decl.build_stmts),
        shape_fn_name=decl.shape,
    )


def _make_subclass(name: str, base: type, rm: ResolvedMatcher, ctx: CompileCtx) -> type:
    """Synthesize a BlockMatcher subclass named `_MCH_<name>`.

    The Match dataclass and capture-name set are attached as class attributes
    (`_mch_match_cls`, `_mch_capture_names`) so the synthesized `build_block`
    reads them off `type(self)`. Capture binding is done by `_extract_captures`,
    which walks the matcher tree in the engine's traversal order and pairs
    consumed blocks with the captures their sub-matchers own.

    Cached weakly by Registries: each `Registries` gets its own (name, base) ->
    subclass map. When the Registries is collected, its entries go with it, so
    there's no risk of address reuse handing back a stale closure.
    """
    by_key = _GENERATED_SUBCLASSES.setdefault(ctx.registries, {})
    cache_key = (f"_MCH_{name}", base)
    cached = by_key.get(cache_key)
    if cached is not None:
        return cached

    builder_name = rm.decl.build
    shape_name = rm.decl.shape
    registries = ctx.registries
    match_cls = _make_match_dataclass(
        name,
        rm.captures,
        ctx.registries.block_types,
        opcodes=_merged_opcodes(ctx),
    )
    capture_names = tuple(rm.captures.keys())
    capture_infos = rm.captures  # dict[str, CaptureInfo] — bound in closure scope

    if builder_name is None:
        # Declarative build path (§9): no registered builder — the build
        # statements are evaluated against the capture environment and the
        # produced block is a generic Block carrying the resulting data dict.
        program = BuildProgram(rm.decl.build_stmts, registries)
        produced_btype = produces_btype(rm.decl.produces_primary)

        async def build_block(self, block, other_blocks):  # type: ignore[override]
            env = _extract_captures(
                self, block, other_blocks,
                type(self)._mch_capture_names,
                capture_infos,
            )
            # Well-known env names (same content as the registered-builder
            # path's Match._mch_consumed). setdefault: a capture of the same
            # name shadows them.
            env.setdefault("consumed", _dedup_consumed(block, other_blocks))
            env.setdefault("anchor", block)
            try:
                outcome = await program.run(env)
            except EvalError as e:
                # Spec §10.7: an expression runtime error is a rejection,
                # reported through diagnostics — never aborts trace processing.
                logger.warning(
                    "mch matcher %r: build evaluation failed (%s); rejecting match",
                    name, e,
                )
                outcome = None
            if outcome is None:
                self._mch_last_rejected = True
                return []
            self._mch_last_rejected = False
            produced = Block(produced_btype, [], outcome.data)
            produced.failed = outcome.failed
            produced.broken = outcome.broken
            # Same consumption contract as the registered-builder path below:
            # merge wires absorbed blocks' parent linkage; return the new block only.
            produced.merge_blocks([block] + other_blocks)
            apply_flows(produced, outcome.flows)
            # §8.7 shape hook: last build step, after merge and flows.
            _apply_shape(registries, shape_name, produced, SimpleNamespace(**env))
            return [produced]

        return _finalize_subclass(name, base, rm, by_key, cache_key, match_cls,
                                  capture_names, build_block)

    async def build_block(self, block, other_blocks):  # type: ignore[override]
        kwargs = _extract_captures(
            self, block, other_blocks,
            type(self)._mch_capture_names,
            capture_infos,
        )
        m = type(self)._mch_match_cls(**kwargs)
        # Well-known Match attribute (spec §10.7/§11.1 host contract, T15/T18): the
        # full consumed block set, anchor first, in engine traversal order,
        # de-duplicated (first occurrence wins — the engine's RecursiveMatcher
        # reports a recursion's entry block twice, `[block] + blocks` with the
        # entry already in `blocks`). Builders whose legacy counterparts scan
        # `other_blocks` freely (e.g. dedust swap's find_messages/
        # find_call_contracts) need it. Purely additive: builders that ignore
        # it are unaffected.
        m._mch_consumed = _dedup_consumed(block, other_blocks)
        builder = registries.builders.get(builder_name)
        if builder is None:
            raise RuntimeError(f"builder {builder_name!r} is not registered at run time")
        produced = await builder(m)
        if produced is None:
            self._mch_last_rejected = True
            return []
        self._mch_last_rejected = False
        # Mirror hand-written matchers exactly (e.g. ElectionDepositStakeBlockMatcher
        # at elections.py:46-47): call merge_blocks to wire absorbed blocks'
        # parent linkage, and return [produced] only. The return list advertises the
        # logically-new block; absorption is via the merge_blocks side effect on
        # block.parent. Returning the consumed-blocks list would pollute upstream
        # composers' other_blocks with already-absorbed children.
        produced.merge_blocks([block] + other_blocks)
        # §8.7 shape hook: post-merge tree surgery on the produced block's region.
        _apply_shape(registries, shape_name, produced, m)
        return [produced]

    return _finalize_subclass(name, base, rm, by_key, cache_key, match_cls,
                              capture_names, build_block)


def _apply_shape(registries: Registries, shape_name: str | None, produced, match_obj) -> None:
    """§8.7 `shape` hook: invoke the declared shaper on the produced block, as
    the LAST build step (after merge, and after flows on declarative matchers).
    Both engines call it at the same point so topology parity holds. Shapers
    are sync `(produced_block, match) -> None`; the contract restricts mutation
    to the produced block's own region."""
    if shape_name is None:
        return
    shaper = registries.shapers.get(shape_name)
    if shaper is None:
        raise RuntimeError(f"shaper {shape_name!r} is not registered at run time")
    shaper(produced, match_obj)


def _dedup_consumed(block, other_blocks) -> list:
    """The full consumed block set, anchor first, engine traversal order,
    de-duplicated by identity (first occurrence wins — the engine's
    RecursiveMatcher reports a recursion's entry block twice)."""
    consumed: list = []
    seen_ids: set[int] = set()
    for b in [block] + list(other_blocks):
        if id(b) not in seen_ids:
            seen_ids.add(id(b))
            consumed.append(b)
    return consumed


def _finalize_subclass(
    name: str,
    base: type,
    rm: ResolvedMatcher,
    by_key: dict,
    cache_key: tuple,
    match_cls: type,
    capture_names: tuple[str, ...],
    build_block,
) -> type:
    """Assemble and cache the `_MCH_<name>` subclass around a build_block body
    (shared by the registered-builder and declarative build paths)."""

    async def try_build(self, block):  # type: ignore[override]
        self._mch_last_rejected = False
        result = await base.try_build(self, block)
        if getattr(self, "_mch_last_rejected", False):
            # Builder rejected: strip any excess/bounce blocks that the base
            # try_build auto-appended to our empty [] return. Return [] so
            # callers checking `not result` see a clean rejection, but callers
            # checking `is not None` still distinguish from a hard structural
            # mismatch (which returns None from the base).
            return []
        return result

    new_cls = type(
        f"_MCH_{name}",
        (base,),
        {
            "__mch_name__": name,
            "__mch_source__": rm.decl.span,
            "_mch_match_cls": match_cls,
            "_mch_capture_names": capture_names,
            "build_block": build_block,
            "try_build": try_build,
        },
    )
    by_key[cache_key] = new_cls
    return new_cls


def _merged_opcodes(ctx: CompileCtx) -> dict[str, int]:
    """File-declared opcodes overlaid on the host registry's opcodes."""
    merged = dict(ctx.registries.opcodes)
    merged.update(ctx.resolved.opcodes)
    return merged


def _make_match_dataclass(
    matcher_name: str,
    captures: dict[str, CaptureInfo],
    block_types: dict[str, type],
    *,
    opcodes: dict[str, int],
) -> type:
    """Build a runtime dataclass with one field per capture."""
    from dataclasses import make_dataclass

    fields: list[tuple[str, Any]] = []
    for cap_name in captures:
        fields.append((cap_name, Any))
    return make_dataclass(f"{matcher_name}Match", fields)


def _extract_captures(
    root: BlockMatcher,
    anchor_block: Any,
    other_blocks: list[Any],
    capture_names: tuple[str, ...],
    capture_infos: dict[str, CaptureInfo],
) -> dict[str, Any]:
    """Walk the lowered matcher tree against the consumed-blocks list and bind captures.

    The engine's `try_build` returns `[anchor_block] + other_blocks`, where the order
    of `other_blocks` follows a fixed traversal of the matcher tree:
    child_matcher's blocks first, then children_matchers in list order, then
    parent_matcher's blocks — each contribution itself recursively expanded.

    This walker mirrors that traversal. list_binding captures (those defined
    inside a recursive rule body) are initialized to [] and populated via
    per-iteration walks against `_MchRecursiveMatcher._mch_iter_blocks`.
    Scalar captures bind via the Phase 1/2 positional walker.
    """
    all_blocks = [anchor_block] + list(other_blocks)
    cursor = [0]
    captures: dict[str, Any] = {
        name: ([] if capture_infos[name].list_binding else None)
        for name in capture_names
    }
    _walk_matcher(root, all_blocks, cursor, captures, capture_infos, in_recursion=False)
    return captures


def _snapshot_captures(captures: dict[str, Any]) -> dict[str, Any]:
    """Savepoint for an Or-branch trial. list_binding captures are copied so a
    rolled-back branch cannot leave partial appends behind; scalars copy by
    reference (blocks are immutable identities here)."""
    return {k: (list(v) if isinstance(v, list) else v) for k, v in captures.items()}


def _restore_captures(captures: dict[str, Any], snap: dict[str, Any]) -> None:
    """Restore a captures dict in place from a `_snapshot_captures` savepoint."""
    for k, v in snap.items():
        captures[k] = list(v) if isinstance(v, list) else v


def _walk_matcher(
    m: BlockMatcher,
    all_blocks: list[Any],
    cursor: list[int],
    captures: dict[str, Any],
    capture_infos: dict[str, CaptureInfo],
    in_recursion: bool,
) -> bool:
    """Positional walker. `in_recursion=True` when called for a per-iteration walk
    inside a recursive rule body — disables the list-binding overwrite guard
    because the per-iter capture dict is scalar-style (list assembly happens in
    the outer caller's _MchRecursiveMatcher branch).

    Returns True when this subtree matched consistently at the cursor (required
    nodes found their blocks; optional nodes matched or were legitimately
    absent), False when a REQUIRED node mismatched the block at the cursor. The
    return value is load-bearing only inside the Or-branch trial below — every
    other caller runs against the exact consumed-block list the engine already
    produced, so the walk it commits never fails; the bool is ignored there and
    the cursor/binding side effects are unchanged."""
    if cursor[0] >= len(all_blocks):
        # No block left to match. Success iff the matcher was optional (nothing
        # required at this position); a required matcher with no block fails.
        return bool(getattr(m, "optional", False))

    if isinstance(m, _MchCyclicRefMatcher):
        # Cyclic-descent rule reference (§10.3 cyclic strategy). Captures inside
        # cyclic bodies are compile-rejected (C012), so nothing binds here — the
        # walker only needs cursor honesty: skip exactly the reference's consumed
        # slice so captures positioned AFTER it (later children-set items, the
        # parent chain) keep binding. Unlike the recursion branch below, a
        # mandatory reference that did not participate FAILS the walk — this
        # keeps Or-branch trials from committing a hop branch when the engine
        # actually matched a final branch.
        if in_recursion:
            cursor[0] = len(all_blocks)
            return True
        entry = m._mch_entry_block
        if entry is None or not any(b is entry for b in all_blocks[cursor[0]:]):
            return bool(getattr(m, "optional", False))
        if all_blocks[cursor[0]] is entry and m._mch_result_len > 0:
            cursor[0] += m._mch_result_len
        else:
            # Misaligned (stale success from a failed sibling attempt, or an
            # earlier optional mis-skip): consume the rest — the pre-T18
            # conservative fallback, affects capture completeness only.
            cursor[0] = len(all_blocks)
        return True

    if isinstance(m, _MchRecursiveMatcher):
        if in_recursion:
            # Nested recursion (a recursive rule inside another's step body):
            # per-iter capture dicts are scalar-style, list assembly cannot
            # nest. Out of scope; consume the remaining per-iter blocks.
            cursor[0] = len(all_blocks)
            return True
        # Participation check: the recorded entry block must lie ahead of the
        # cursor in THIS match's consumed list. Otherwise the recursion did not
        # take part (optional and absent, or the state is stale from another
        # probe/trace) — bind nothing and leave the cursor for the next sibling.
        entry = m._mch_entry_block
        if entry is None or not any(b is entry for b in all_blocks[cursor[0]:]):
            return True
        # Per-iteration extraction via the matcher's recorded iteration blocks.
        # Only captures owned by this rule's STEP branches get the one-element-
        # per-unrolling padding (spec §7.2 gap alignment).
        for iter_blocks in m._mch_iter_blocks:
            if not iter_blocks:
                continue
            iter_cursor = [0]
            iter_captures: dict[str, Any] = {name: None for name in captures.keys()}
            _walk_matcher(
                m.repeating_matcher, list(iter_blocks), iter_cursor,
                iter_captures, capture_infos, in_recursion=True,
            )
            for name, info in capture_infos.items():
                if info.list_binding and name in m._mch_step_capture_names:
                    captures[name].append(iter_captures.get(name))
        # Exit-branch captures bind at most once — on the terminating match of
        # the pure exit (spec §7.2). No per-unrolling padding.
        if m.exit_matcher is not None and m._mch_exit_blocks:
            exit_captures: dict[str, Any] = {name: None for name in captures.keys()}
            _walk_matcher(
                m.exit_matcher, list(m._mch_exit_blocks), [0],
                exit_captures, capture_infos, in_recursion=True,
            )
            for name, info in capture_infos.items():
                if info.list_binding and name in m._mch_exit_capture_names:
                    captures[name].append(exit_captures.get(name))
        # Skip exactly the recursion's contribution (its try_build result was
        # extended verbatim into the outer blocks list) so captures positioned
        # AFTER it in traversal order — later children-set items, the parent
        # chain — keep binding. The slice's internal order is set-shuffled,
        # but its length is exact. If the entry is not at the cursor (cursor
        # misalignment from an earlier optional mis-skip, or several sibling
        # contributions of the same instance), fall back to consuming the rest
        # — the pre-T18 conservative behavior.
        if all_blocks[cursor[0]] is entry and m._mch_result_len > 0:
            cursor[0] += m._mch_result_len
        else:
            cursor[0] = len(all_blocks)
        return True

    if isinstance(m, RecursiveMatcher):
        # Non-_Mch RecursiveMatcher (defensive — shouldn't occur post-Phase 3).
        cursor[0] = len(all_blocks)
        return True

    if isinstance(m, OrMatcher):
        # Align with the v2 IR engine's _match_or (mch_ir/engine.py): try each
        # branch's FULL positional walk against the remaining consumed slice;
        # the first branch that fully matches wins, rolling the cursor and
        # captures back between attempts. This disambiguates same-head XOR
        # branches (e.g. evaa's two `op EvaaWithdrawCollateralized` arms) that
        # head-test identically — the previous head-test-only selection always
        # picked the first head match and bound the wrong branch's captures on
        # traces shaped for a later branch (@coll_ok on FAIL traces).
        start = cursor[0]
        snap = _snapshot_captures(captures)
        for sub in m.matchers:
            if _walk_matcher(sub, all_blocks, cursor, captures, capture_infos, in_recursion):
                return True
            cursor[0] = start
            _restore_captures(captures, snap)
        # No branch fully re-traced. Fall back to the first head-matching
        # branch, committed the legacy way (walk it, keep whatever it binds).
        # This never regresses below the pre-alignment behavior when the
        # positional heuristic cannot fully re-trace even the winning branch's
        # subtree; it only strengthens the multi-branch same-head case above.
        block = all_blocks[cursor[0]]
        for sub in m.matchers:
            if sub.test_self(block):
                _walk_matcher(sub, all_blocks, cursor, captures, capture_infos, in_recursion)
                return True
        return bool(getattr(m, "optional", False))

    block = all_blocks[cursor[0]]
    if not m.test_self(block):
        # Head mismatch at this position. If the matcher is optional the engine
        # legitimately skipped it (cursor stays, success); otherwise this is a
        # structural mismatch — observable only during an Or-branch trial, since
        # the committed walk always runs against blocks the engine consumed.
        return bool(getattr(m, "optional", False))

    cap = getattr(m, "_mch_capture_name", None)
    if cap is not None and cap in captures:
        info = capture_infos.get(cap)
        # CORRECTNESS GUARD: at the OUTER walk (in_recursion=False), do NOT
        # overwrite a list_binding capture — the list is populated only via
        # the _MchRecursiveMatcher branch above. Inside a per-iter walk
        # (in_recursion=True), the iter_captures dict is scalar-style; bind
        # unconditionally so per-iter values get collected.
        if in_recursion or (info is None or not info.list_binding):
            captures[cap] = block
    cursor[0] += 1

    ok = True
    if m.child_matcher is not None:
        r = _walk_matcher(m.child_matcher, all_blocks, cursor, captures, capture_infos, in_recursion)
        ok = ok and r
    if m.children_matchers:
        for cm in m.children_matchers:
            r = _walk_matcher(cm, all_blocks, cursor, captures, capture_infos, in_recursion)
            ok = ok and r
    if m.parent_matcher is not None:
        r = _walk_matcher(m.parent_matcher, all_blocks, cursor, captures, capture_infos, in_recursion)
        ok = ok and r
    return ok


def _lower_relative_to_anchor(
    pattern: ast_.PatternExpr,
    anchor: ast_.PatternExpr,
    ctx: CompileCtx,
) -> tuple[BlockMatcher | None, BlockMatcher | None, list[BlockMatcher] | None]:
    """Walk the pattern to figure out (parent_matcher, child_matcher, children_matchers).

    `anchor` is compared by identity: a single anchor Node, or the whole
    exclusive Alternative for opcode-set anchors (spec §8.3 M2).
    """
    parents = _find_anchor_path(pattern, anchor)
    if parents is None:
        return None, None, None
    # `parents` ends with the anchor itself; its immediate parent is parents[-2].
    if len(parents) < 2:
        return None, None, None
    immediate_parent = parents[-2]

    child_chain, children_ms = _lower_child_chain_from(immediate_parent, pattern, anchor, ctx)
    parent_chain = _lower_parent_chain_from(immediate_parent, pattern, anchor, ctx)
    return parent_chain, child_chain, children_ms


def _find_anchor_path(pattern: ast_.PatternExpr, anchor: ast_.PatternExpr) -> list[ast_.PatternExpr] | None:
    if pattern is anchor:
        return None

    def search(p: ast_.PatternExpr, path: list[ast_.PatternExpr]) -> list[ast_.PatternExpr] | None:
        if p is anchor:
            return path + [p]
        if isinstance(p, ast_.Sequence):
            r = search(p.head, path + [p])
            if r:
                return r
            for _e, atom in p.tail:
                r = search(atom, path + [p])
                if r:
                    return r
        elif isinstance(p, ast_.Maybe):
            return search(p.inner, path + [p])
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                r = search(b, path + [p])
                if r:
                    return r
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                r = search(it, path + [p])
                if r:
                    return r
        return None

    return search(pattern, [])


def _is_cb_atom(a: ast_.PatternExpr) -> bool:
    """True for a ChildrenBlock or `maybe {...}` atom — the two forms that attach
    to the preceding matcher without advancing the chain cursor (spec §6.5)."""
    return isinstance(a, ast_.ChildrenBlock) or (
        isinstance(a, ast_.Maybe) and isinstance(a.inner, ast_.ChildrenBlock)
    )


def _lower_child_chain_from(
    parent: ast_.PatternExpr,
    pattern: ast_.PatternExpr,
    anchor: ast_.PatternExpr,
    ctx: CompileCtx,
) -> tuple[BlockMatcher | None, list[BlockMatcher] | None]:
    if isinstance(parent, ast_.Sequence):
        # Collect CHILD-edged atoms in the anchor's downstream slice. Stop at
        # the first PARENT-edged atom — atoms appearing after a PARENT edge
        # belong to the parent chain (`_lower_parent_chain_from` consumes them).
        # This implements Q5: in `op A <- op B -> {C; D}`, {C; D} routes to B's
        # parent chain, NOT to anchor A's children_matchers.
        downstream_atoms: list[ast_.PatternExpr] = []
        if parent.head is anchor:
            tail_slice = parent.tail
        else:
            if isinstance(parent.head, ast_.ChildrenBlock):
                ctx.bag.error(
                    "C008_CHILDREN_BLOCK_BARE",
                    "children block '{...}' cannot appear as the first element of a sequence; "
                    "it must follow a node (e.g. 'op A -> {op B; op C}')",
                    parent.head.span,
                )
                raise CompileError(ctx.bag)
            anchor_idx = None
            for i, (_e, atom) in enumerate(parent.tail):
                if atom is anchor:
                    anchor_idx = i
                    break
            if anchor_idx is None:
                return None, None
            tail_slice = parent.tail[anchor_idx + 1:]
        for edge, atom in tail_slice:
            if edge is ast_.Edge.PARENT:
                if downstream_atoms and not all(_is_cb_atom(a) for a in downstream_atoms):
                    # `op A -> op B <- op C`: the parent edge's left side is a
                    # non-anchor node (B) reached through a preceding child edge.
                    # Its only faithful lowering sets B.parent_matcher = C, which
                    # at runtime tests B.previous_block (== the anchor A) against
                    # opcode C and can never be satisfied. Reject rather than
                    # silently drop the `<- C` segment (spec §6.4).
                    # Exception (T15): if everything before the parent edge is
                    # ChildrenBlocks, the chain cursor never left the anchor
                    # (spec §6.5) — `op A -> {B; C} <- op P` is A with both
                    # children_matchers and parent_matcher.
                    ctx.bag.error(
                        "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED",
                        "a parent edge ('<-') following a child edge in the same "
                        "chain is unsupported: the parent would test the anchor "
                        "block and can never match; split the pattern or re-anchor",
                        atom.span,
                    )
                    raise CompileError(ctx.bag)
                break
            downstream_atoms.append(atom)
        if not downstream_atoms:
            return None, None
        first = downstream_atoms[0]
        if isinstance(first, ast_.ChildrenBlock) or (
            isinstance(first, ast_.Maybe) and isinstance(first.inner, ast_.ChildrenBlock)
        ):
            # Anchor-adjacent ChildrenBlock fast path. Greedily consume any
            # consecutive bare/maybe ChildrenBlocks so back-to-back `{B;C} {D;E}`
            # all attach to the anchor's children_matchers in source order.
            children_ms: list[BlockMatcher] = []
            rest_idx = 0
            while rest_idx < len(downstream_atoms):
                atom = downstream_atoms[rest_idx]
                if isinstance(atom, ast_.ChildrenBlock):
                    children_ms.extend(_lower_atom(it, ctx) for it in atom.items)
                    rest_idx += 1
                    continue
                if isinstance(atom, ast_.Maybe) and isinstance(atom.inner, ast_.ChildrenBlock):
                    for it in atom.inner.items:
                        lowered_it = _lower_atom(it, ctx)
                        lowered_it.optional = True
                        children_ms.append(lowered_it)
                    rest_idx += 1
                    continue
                break
            rest = downstream_atoms[rest_idx:]
            if rest:
                pairs = [(ast_.Edge.CHILD, a) for a in rest]
                child = _lower_chain_with_children_attach(pairs, ctx)
                return child, children_ms
            return None, children_ms
        pairs = [(ast_.Edge.CHILD, a) for a in downstream_atoms]
        child = _lower_chain_with_children_attach(pairs, ctx)
        return child, None

    return None, None


def _lower_parent_chain_from(
    parent: ast_.PatternExpr,
    pattern: ast_.PatternExpr,
    anchor: ast_.PatternExpr,
    ctx: CompileCtx,
) -> BlockMatcher | None:
    """Lower consecutive PARENT edges from the anchor outward into a chain of
    parent_matchers. Handles anchor-is-head and both anchor-in-tail variants.
    """
    if not isinstance(parent, ast_.Sequence):
        return None

    prepend_previous = False
    start_idx = 0

    if parent.head is anchor:
        # Skip anchor-attached ChildrenBlocks — they do not advance the chain
        # cursor (spec §6.5), so a parent edge after them still belongs to the
        # anchor: `op A -> {B; C} <- op P` (T15).
        idx = 0
        while (
            idx < len(parent.tail)
            and parent.tail[idx][0] is ast_.Edge.CHILD
            and _is_cb_atom(parent.tail[idx][1])
        ):
            idx += 1
        if idx >= len(parent.tail) or parent.tail[idx][0] is not ast_.Edge.PARENT:
            return None
        start_idx = idx
    else:
        anchor_idx = next(
            (i for i, (_e, a) in enumerate(parent.tail) if a is anchor),
            None,
        )
        if anchor_idx is None:
            return None
        edge_at_anchor, _ = parent.tail[anchor_idx]
        if edge_at_anchor is ast_.Edge.PARENT:
            # Variant B.1: anchor's incoming edge is PARENT; the "previous" atom
            # acts as the first parent link.
            prepend_previous = True
            start_idx = anchor_idx + 1
        else:
            # Variant B.2: anchor's incoming edge is CHILD, i.e. an atom upstream
            # of the anchor (`… -> op Anchor`) is the anchor's parent. Lowering
            # only walks downstream from the anchor, so every upstream atom would
            # be silently dropped (T04/T16 `$rule -> anchor…` finding). Reject
            # rather than misbehave; re-anchor or restructure the pattern.
            upstream_atom = (
                parent.head if anchor_idx == 0 else parent.tail[anchor_idx - 1][1]
            )
            ctx.bag.error(
                "C010_UPSTREAM_ATOMS_UNSUPPORTED",
                "upstream atoms before the anchor are not lowered; re-anchor the "
                "pattern or restructure — see specs/stonfi_v2_swap.mch",
                upstream_atom.span if hasattr(upstream_atom, "span") else anchor.span,
            )
            raise CompileError(ctx.bag)

    # Build the parent chain as a single interleaved walk from the anchor's first
    # parent link outward. PARENT edges extend the chain; CHILD-edged children
    # blocks attach to the current chain tail (they do not advance the cursor,
    # spec §6.5), so a `<- op X -> {CB} <- Y` shape wires X with BOTH
    # children_matchers and parent_matcher — identical to the grouped form
    # `<- ( op X -> {CB} <- Y )`. A CHILD edge with a non-CB atom (a child branch
    # off a parent node) is out of scope in v0 and stops the walk.
    chain_pairs: list[tuple[ast_.Edge, ast_.PatternExpr]] = []
    if prepend_previous:
        anchor_idx = next(i for i, (_e, a) in enumerate(parent.tail) if a is anchor)
        previous_atom = parent.head if anchor_idx == 0 else parent.tail[anchor_idx - 1][1]
        chain_pairs.append((ast_.Edge.PARENT, previous_atom))

    saw_parent_link = prepend_previous
    for edge, atom in parent.tail[start_idx:]:
        if edge is ast_.Edge.PARENT:
            chain_pairs.append((ast_.Edge.PARENT, atom))
            saw_parent_link = True
        elif edge is ast_.Edge.CHILD and saw_parent_link and _is_cb_atom(atom):
            # Children block attaching to the current chain tail.
            chain_pairs.append((ast_.Edge.CHILD, atom))
        else:
            # A non-PARENT edge before any parent link, or a non-CB child branch
            # off a parent node: out of scope in v0. Stop consuming.
            break

    if not saw_parent_link:
        return None

    return _lower_chain_with_children_attach(chain_pairs, ctx)


def _lower_chain_with_children_attach(
    pairs: list[tuple[ast_.Edge, ast_.PatternExpr]],
    ctx: CompileCtx,
) -> BlockMatcher | None:
    """Walk an ordered list of `(edge, atom)` pairs and build a chained matcher.

    ChildrenBlock atoms attach their items to the immediately-preceding matcher's
    `children_matchers`. `Maybe(ChildrenBlock)` does the same with per-item
    `optional=True`. Bare ChildrenBlock at the head (no left sibling) emits
    `C008_CHILDREN_BLOCK_BARE`. Non-CB atoms chain via `_attach_as_child` (CHILD
    edge) or `_attach_as_parent` (PARENT edge).
    """
    head_matcher: BlockMatcher | None = None
    previous_lowered: BlockMatcher | None = None

    for edge, atom in pairs:
        # Detect bare or maybe-wrapped ChildrenBlock attach-to-left.
        cb_items: list[ast_.PatternExpr] | None = None
        optional_items = False
        if isinstance(atom, ast_.ChildrenBlock):
            cb_items = list(atom.items)
        elif isinstance(atom, ast_.Maybe) and isinstance(atom.inner, ast_.ChildrenBlock):
            cb_items = list(atom.inner.items)
            optional_items = True

        if cb_items is not None:
            if previous_lowered is None:
                # Bare ChildrenBlock with no left sibling.
                span = atom.span if isinstance(atom, ast_.ChildrenBlock) else atom.inner.span
                ctx.bag.error(
                    "C008_CHILDREN_BLOCK_BARE",
                    "children block '{...}' must follow a node in a sequence "
                    "(e.g. 'op A -> {op B; op C}'); bare children blocks are not allowed",
                    span,
                )
                raise CompileError(ctx.bag)
            if previous_lowered.children_matchers is None:
                previous_lowered.children_matchers = []
            for item in cb_items:
                lowered_item = _lower_atom(item, ctx)
                if optional_items:
                    lowered_item.optional = True
                previous_lowered.children_matchers.append(lowered_item)
            # Do NOT update previous_lowered — a CB is a constraint on the
            # preceding matcher, not a chain link.
            continue

        lowered = _lower_atom(atom, ctx)
        if head_matcher is None:
            head_matcher = lowered
        else:
            if edge is ast_.Edge.PARENT:
                _attach_as_parent(previous_lowered, lowered)
            else:
                _attach_as_child(previous_lowered, lowered)
        previous_lowered = lowered

    return head_matcher


def _branches_eligible_for_exclusive_heuristic(
    branches: tuple[ast_.PatternExpr, ...],
    ctx: CompileCtx,
) -> bool:
    """Return True iff all branches are bare concrete Nodes with distinct resolved heads and no predicates."""
    seen_opcodes: set[int] = set()
    seen_btypes: set[str] = set()
    for b in branches:
        if not isinstance(b, ast_.Node):
            return False
        head = b.head
        if not isinstance(head, (ast_.OpHead, ast_.BTypeHead)):
            return False
        if b.where_predicate is not None or b.where_expr is not None:
            return False
        if isinstance(head, ast_.OpHead):
            ref = head.ref
            opcode_int = ref if isinstance(ref, int) else (
                ctx.resolved.opcodes.get(ref) or ctx.registries.opcodes.get(ref)
            )
            if opcode_int is None:
                return False
            if opcode_int in seen_opcodes:
                return False
            seen_opcodes.add(opcode_int)
        elif isinstance(head, ast_.BTypeHead):
            if head.name in seen_btypes:
                return False
            seen_btypes.add(head.name)
    return True


def _check_redundant_exclusive_heads(p: ast_.Alternative, ctx: CompileCtx) -> None:
    """Emit W202 if two branches share the same concrete head (non-blocking warning)."""
    seen: dict[object, object] = {}
    for b in p.branches:
        if not isinstance(b, ast_.Node):
            continue
        head = b.head
        if isinstance(head, ast_.OpHead):
            ref = head.ref
            opcode_int = ref if isinstance(ref, int) else (
                ctx.resolved.opcodes.get(ref) or ctx.registries.opcodes.get(ref)
            )
            key = ("op", opcode_int)
        elif isinstance(head, ast_.BTypeHead):
            key = ("btype", head.name)
        else:
            continue
        if key in seen:
            ctx.bag.warning(
                "W202_EXCLUSIVE_OR_REDUNDANT_HEADS",
                f"two branches of this exclusive alternative share the same head "
                f"({'opcode' if key[0] == 'op' else 'btype'} {key[1]!r}); "
                f"ExclusiveOrMatcher's sum-check will never return True for this head",
                b.span,
            )
        else:
            seen[key] = b.span


def _anchor_group_head_sets(
    group: ast_.Alternative,
    ctx: CompileCtx,
) -> tuple[frozenset[int], frozenset[str]]:
    """Collect the branch heads of an opcode-set anchor group (spec §8.3 M2).

    The resolver guarantees every branch is a bare Node with a concrete
    `op`/`btype` head and no `where` clause.
    """
    opcode_set: set[int] = set()
    btype_set: set[str] = set()
    for b in group.branches:
        head = b.head
        if isinstance(head, ast_.OpHead):
            opcode = head.ref if isinstance(head.ref, int) else (
                ctx.resolved.opcodes.get(head.ref) or ctx.registries.opcodes.get(head.ref)
            )
            opcode_set.add(opcode)
        else:
            btype_set.add(head.name)
    return frozenset(opcode_set), frozenset(btype_set)


def _anchor_group_branch_descs(
    group: ast_.Alternative,
    ctx: CompileCtx,
) -> list[tuple[str, Any, str | None]]:
    """Per-branch descriptors of a set-membership anchor group, source order
    ([M2R, R09]). Each is `(kind, value, where_name)` where kind is `"op"`
    (value = resolved opcode int) or `"btype"` (value = btype name), and
    where_name is the branch's `where` predicate name or None. The resolver
    guarantees every branch is a bare concrete `op`/`btype` node."""
    descs: list[tuple[str, Any, str | None]] = []
    for b in group.branches:
        head = b.head
        if isinstance(head, ast_.OpHead):
            opcode = head.ref if isinstance(head.ref, int) else (
                ctx.resolved.opcodes.get(head.ref) or ctx.registries.opcodes.get(head.ref)
            )
            descs.append(("op", opcode, b.where_predicate))
        else:
            descs.append(("btype", head.name, b.where_predicate))
    return descs


def _make_head_set_test(opcode_set: frozenset[int], btype_set: frozenset[str]):
    """Membership `test_self` for an opcode/btype-set anchor."""
    def test(block) -> bool:
        if isinstance(block, CallContractBlock) and block.opcode in opcode_set:
            return True
        return block.btype in btype_set
    return test


def _make_head_set_test_with_where(
    branch_descs: list[tuple[str, Any, str | None]],
    registries: Registries,
):
    """Membership `test_self` with per-branch `where` predicates ([M2R, R09]).

    A block matches iff some branch's head matches AND that branch's `where`
    predicate (if any) passes — union membership evaluated first, then the
    branch predicate. Branches are tried in source order; a head match whose
    predicate fails falls through to any later branch sharing the head."""
    compiled: list[tuple[str, Any, Any]] = [
        (kind, value, registries.predicates[w] if w is not None else None)
        for kind, value, w in branch_descs
    ]

    def test(block) -> bool:
        for kind, value, pred in compiled:
            if kind == "op":
                head_ok = isinstance(block, CallContractBlock) and block.opcode == value
            else:
                head_ok = block.btype == value
            if head_ok and (pred is None or pred(block)):
                return True
        return False
    return test


def _lower_atom(p: ast_.PatternExpr, ctx: CompileCtx) -> BlockMatcher:
    if isinstance(p, ast_.Node):
        return _lower_node(p, ctx)
    if isinstance(p, ast_.Maybe):
        # Bare `maybe {...}` (Maybe wrapping a ChildrenBlock with no chain
        # context) is rejected. In-chain `op A -> maybe {B; C}` is handled by
        # `_lower_chain_with_children_attach` BEFORE reaching here.
        if isinstance(p.inner, ast_.ChildrenBlock):
            ctx.bag.error(
                "C008_CHILDREN_BLOCK_BARE",
                "'maybe {...}' must follow a node in a sequence "
                "(e.g. 'op A -> maybe {op B; op C}'); bare maybe-children blocks are not allowed",
                p.inner.span,
            )
            raise CompileError(ctx.bag)
        inner = _lower_atom(p.inner, ctx)
        inner.optional = True
        return inner
    if isinstance(p, ast_.Alternative):
        lowered_branches = [_lower_atom(b, ctx) for b in p.branches]
        if p.exclusive:
            _check_redundant_exclusive_heads(p, ctx)
            return ExclusiveOrMatcher(lowered_branches)
        if _branches_eligible_for_exclusive_heuristic(p.branches, ctx):
            return ExclusiveOrMatcher(lowered_branches)
        return OrMatcher(lowered_branches)
    if isinstance(p, ast_.Sequence):
        pairs: list[tuple[ast_.Edge, ast_.PatternExpr]] = [(ast_.Edge.CHILD, p.head)]
        pairs.extend(p.tail)
        result = _lower_chain_with_children_attach(pairs, ctx)
        if result is None:
            # Defensive — should not occur in well-formed AST.
            raise AssertionError("Sequence lowered to None")
        return result
    if isinstance(p, ast_.ChildrenBlock):
        ctx.bag.error(
            "C008_CHILDREN_BLOCK_BARE",
            "children block '{...}' cannot appear as a standalone expression; "
            "it must follow a node in a sequence (e.g. 'op A -> {op B; op C}')",
            p.span,
        )
        raise CompileError(ctx.bag)
    if isinstance(p, ast_.RuleRef):
        target = next((r for r in ctx.resolved.raw.rules if r.name == p.name), None)
        if target is None:
            raise CompileError(ctx.bag)
        # Self-reference inside the body of a CYCLIC rule being lowered: a true
        # back-edge — a per-site delegating reference whose target (the shared
        # body root) is patched in by _lower_cyclic_rule once the body finishes
        # lowering.
        cyclic_pending = getattr(ctx, _CYCLIC_PENDING_KEY, None)
        if cyclic_pending is not None and p.name in cyclic_pending:
            ref = _MchCyclicRefMatcher()
            ref._mch_cyclic_inner = True
            cyclic_pending[p.name].append(ref)
            return ref
        # Self-reference inside the body of a recursive rule we're already lowering:
        # the RecursiveMatcher handles the repetition, so we represent the self-ref
        # as a permissive sentinel.
        if p.name in ctx.lowering_recursive:
            return GenericMatcher(test_self_func=lambda _b: True)
        rec_class = ctx.resolved.rule_recursion.get(p.name)
        if rec_class is RecursionClass.INDIRECT_RECURSIVE:
            ctx.bag.error(
                "C005_INDIRECT_RECURSION_UNSUPPORTED",
                f"rule {p.name!r}: indirect (mutual) recursion is not supported in v0; "
                f"flatten the rule cycle or use a single self-referential rule",
                p.span,
            )
            raise CompileError(ctx.bag)
        if rec_class is RecursionClass.DIRECT_RECURSIVE:
            if target.strategy == "cyclic":
                return _lower_cyclic_rule(p.name, ctx)
            return _lower_recursive_rule(p.name, ctx)
        return _lower_atom(target.pattern, ctx)
    raise AssertionError(f"unhandled pattern node: {type(p).__name__}")


def _check_where_expr_sync(e: ast_.Expr, span: Span, ctx: CompileCtx) -> bool:
    """Compile-time mirror of the IR loader's _check_where_expr_sync (T33).

    An inline `where (expr)` composes onto the engine's synchronous test_self,
    so async constructs (lookups, host fns) are compile errors here — the IR
    loader rejects the same artifacts, keeping both paths failing at the same
    stage instead of the compiled path silently never matching. Returns True
    when the expression is sync-evaluable."""
    if isinstance(e, ast_.LookupExpr):
        ctx.bag.error(
            "C013_WHERE_EXPR_NOT_SYNC",
            "`where (expr)` cannot contain lookups (test_self is sync)",
            span,
        )
        return False
    if isinstance(e, ast_.Call):
        callee = e.callee
        if not isinstance(callee, ast_.NameRef) or callee.name not in _EXPR_BUILTINS:
            shown = callee.name if isinstance(callee, ast_.NameRef) else type(callee).__name__
            ctx.bag.error(
                "C013_WHERE_EXPR_NOT_SYNC",
                f"`where (expr)` can only call builtins, not host fn {shown!r}",
                span,
            )
            return False
        return all(_check_where_expr_sync(a, span, ctx) for a in e.args)
    if isinstance(e, ast_.FieldAccess):
        return _check_where_expr_sync(e.target, span, ctx)
    if isinstance(e, ast_.UnaryOp):
        return _check_where_expr_sync(e.operand, span, ctx)
    if isinstance(e, ast_.BinaryOp):
        return (_check_where_expr_sync(e.left, span, ctx)
                and _check_where_expr_sync(e.right, span, ctx))
    if isinstance(e, ast_.Ternary):
        return (_check_where_expr_sync(e.cond, span, ctx)
                and _check_where_expr_sync(e.then, span, ctx)
                and _check_where_expr_sync(e.orelse, span, ctx))
    return True


def _compose_where_expr(m: BlockMatcher, expr: ast_.Expr, span: Span,
                        ctx: CompileCtx) -> None:
    """Inline `where (expr)` (spec §6.8 M4, T33): compile the ast expression to
    its IR encoding and AND it onto test_self via the shared sync evaluator —
    the same bytes and the same evaluator the IR engine uses (expr_eval.py),
    so the two paths cannot drift. Missing field / eval fault => False."""
    if not _check_where_expr_sync(expr, span, ctx):
        return
    ir_expr = emit_expr(expr)
    original_test = m.test_self
    def _test_with_where_expr(block, _orig=original_test, _e=ir_expr):
        return _orig(block) and eval_where_expr(_e, block)
    m.test_self = _test_with_where_expr  # type: ignore[method-assign]
    m._ir_where_expr = expr  # type: ignore[attr-defined]


def _lower_node(n: ast_.Node, ctx: CompileCtx) -> BlockMatcher:
    head = n.head
    if isinstance(head, ast_.OpHead):
        opcode = head.ref if isinstance(head.ref, int) else (
            ctx.resolved.opcodes.get(head.ref) or ctx.registries.opcodes.get(head.ref)
        )
        m: BlockMatcher = ContractMatcher(opcode=opcode)
    elif isinstance(head, ast_.BTypeHead):
        m = BlockTypeMatcher(block_type=head.name)
    elif isinstance(head, ast_.PredHead):
        pred = ctx.registries.predicates[head.name]
        m = GenericMatcher(test_self_func=pred)
        m._ir_pred = head.name  # type: ignore[attr-defined]
    elif isinstance(head, ast_.AnyHead):
        m = GenericMatcher(test_self_func=lambda _b: True)
    else:
        raise AssertionError(f"unknown node head: {type(head).__name__}")
    if n.where_predicate is not None:
        pred = ctx.registries.predicates[n.where_predicate]
        original_test = m.test_self
        def combined(block, _orig=original_test, _p=pred):
            return _orig(block) and _p(block)
        m.test_self = combined  # type: ignore[assignment]
        m._ir_where = n.where_predicate  # type: ignore[attr-defined]
    if n.where_expr is not None:
        # Inline `where (expr)` (T33): evaluated at match time, composed onto
        # test_self; the same call also stamps _ir_where_expr for IR emission.
        _compose_where_expr(m, n.where_expr, n.span, ctx)
    if n.capture is not None:
        # Tag the lowered matcher with its capture name. The synthesized
        # build_block walks the matcher tree and binds captures positionally
        # by consulting this attribute.
        m._mch_capture_name = n.capture  # type: ignore[attr-defined]
    return m


def _chain(matchers_iter) -> BlockMatcher | None:
    matchers = [m for m in matchers_iter if m is not None]
    if not matchers:
        return None
    first = matchers[0]
    current = first
    for nxt in matchers[1:]:
        _attach_as_child(current, nxt)
        current = nxt
    return first


def _attach_as_child(host: BlockMatcher, child: BlockMatcher) -> None:
    if host.child_matcher is None:
        host.child_matcher = child
    else:
        _attach_as_child(host.child_matcher, child)


def _attach_as_parent(host: BlockMatcher, parent: BlockMatcher) -> None:
    if host.parent_matcher is None:
        host.parent_matcher = parent
    else:
        _attach_as_parent(host.parent_matcher, parent)


def _check_no_capture_after_recursion(
    pattern: ast_.PatternExpr,
    decl_span: Span,
    ctx: CompileCtx,
) -> None:
    """Reject captures placed AFTER a direct-recursive rule reference in the
    same sequence — the walker cannot reach them due to engine `set()` dedup."""

    def is_direct_recursive_ref(p: ast_.PatternExpr) -> bool:
        if isinstance(p, ast_.RuleRef):
            return ctx.resolved.rule_recursion.get(p.name) is RecursionClass.DIRECT_RECURSIVE
        if isinstance(p, ast_.Maybe):
            return is_direct_recursive_ref(p.inner)
        return False

    def contains_capture(p: ast_.PatternExpr) -> bool:
        if isinstance(p, ast_.Node):
            return p.capture is not None
        if isinstance(p, ast_.Maybe):
            return contains_capture(p.inner)
        if isinstance(p, ast_.Sequence):
            if contains_capture(p.head):
                return True
            return any(contains_capture(a) for _e, a in p.tail)
        if isinstance(p, ast_.Alternative):
            return any(contains_capture(b) for b in p.branches)
        if isinstance(p, ast_.ChildrenBlock):
            return any(contains_capture(it) for it in p.items)
        if isinstance(p, ast_.RuleRef):
            target = next((r for r in ctx.resolved.raw.rules if r.name == p.name), None)
            return target is not None and contains_capture(target.pattern)
        return False

    def walk(p: ast_.PatternExpr) -> None:
        if isinstance(p, ast_.Sequence):
            recursion_seen = False
            atoms = [(None, p.head)] + list(p.tail)
            for _edge, atom in atoms:
                if recursion_seen and contains_capture(atom):
                    ctx.bag.error(
                        "C007_CAPTURE_AFTER_RECURSION",
                        "captures placed after a recursive-rule reference in the "
                        "same sequence are not bound at runtime; restructure the pattern",
                        atom.span if hasattr(atom, "span") else decl_span,
                    )
                if is_direct_recursive_ref(atom):
                    recursion_seen = True
                walk(atom)
        elif isinstance(p, ast_.Maybe):
            walk(p.inner)
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                walk(b)
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                walk(it)

    walk(pattern)


def _check_no_bare_children_block(
    pattern: ast_.PatternExpr,
    ctx: CompileCtx,
) -> None:
    """Reject ChildrenBlock atoms that lack a left sibling in their enclosing chain.

    A ChildrenBlock (`{A; B}`) or `maybe {A; B}` must follow a node atom in a
    Sequence's tail. Bare positions emit `C008_CHILDREN_BLOCK_BARE`:

    - Sequence head (`{A; B} -> X`).
    - Alternative branch (`(op A | {B; C})`).
    - Maybe inner that itself is at a bare position (`maybe {A; B}` standalone).
    - Standalone pattern (`{A; B}` as the whole matcher pattern).

    Inside a sequence tail with a preceding atom, ChildrenBlock is well-formed
    and the lowering path attaches its items to the left sibling.
    """

    def is_cb_or_maybe_cb(p: ast_.PatternExpr) -> tuple[bool, ast_.Span]:
        if isinstance(p, ast_.ChildrenBlock):
            return True, p.span
        if isinstance(p, ast_.Maybe) and isinstance(p.inner, ast_.ChildrenBlock):
            return True, p.inner.span
        return False, p.span if hasattr(p, "span") else None  # type: ignore[return-value]

    def emit_c008(span: ast_.Span) -> None:
        ctx.bag.error(
            "C008_CHILDREN_BLOCK_BARE",
            "children block '{...}' must follow a node in a sequence "
            "(e.g. 'op A -> {op B; op C}'); bare children blocks are not allowed",
            span,
        )

    def walk(p: ast_.PatternExpr, in_chain_left: bool) -> None:
        """`in_chain_left` is True when this atom has a non-CB left sibling in its
        enclosing Sequence tail (i.e. it's a valid attach-to-left position).
        """
        is_cb, span = is_cb_or_maybe_cb(p)
        if is_cb and not in_chain_left:
            emit_c008(span)
        if isinstance(p, ast_.Sequence):
            # Head has no left sibling; if it's a CB, that's bare.
            walk(p.head, in_chain_left=False)
            previous_was_atom = not is_cb_or_maybe_cb(p.head)[0]
            for _edge, atom in p.tail:
                walk(atom, in_chain_left=previous_was_atom)
                # CBs don't update "left sibling" — next atom still chains off
                # the same preceding node.
                if not is_cb_or_maybe_cb(atom)[0]:
                    previous_was_atom = True
        elif isinstance(p, ast_.Maybe):
            # Maybe wraps a non-CB inner: recurse with no chain context for inner.
            if not isinstance(p.inner, ast_.ChildrenBlock):
                walk(p.inner, in_chain_left=False)
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                walk(b, in_chain_left=False)
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                walk(it, in_chain_left=False)
        elif isinstance(p, ast_.RuleRef):
            # Check the rule body once at its definition site, with no chain
            # context — its body must be self-contained or referenced from a
            # Sequence (in which case body's own bareness is independent).
            target = next((r for r in ctx.resolved.raw.rules if r.name == p.name), None)
            if target is not None and not _seen_rule(p.name, ctx):
                _mark_rule_seen(p.name, ctx)
                walk(target.pattern, in_chain_left=False)

    walk(pattern, in_chain_left=False)


_RULE_VALIDATED_KEY = "__mch_rule_validated__"


def _seen_rule(name: str, ctx: CompileCtx) -> bool:
    seen: set[str] = getattr(ctx, _RULE_VALIDATED_KEY, set())
    return name in seen


def _mark_rule_seen(name: str, ctx: CompileCtx) -> None:
    seen: set[str] = getattr(ctx, _RULE_VALIDATED_KEY, set())
    seen.add(name)
    setattr(ctx, _RULE_VALIDATED_KEY, seen)


def _strip_trailing_self_ref(branch: ast_.PatternExpr, rule_name: str) -> ast_.PatternExpr | None:
    """Strip trailing `maybe $rule_name` from a step branch.

    Returns the stripped pattern, or None if the branch is just `maybe $rule_name`
    alone (the branch carries no actual content and should be discarded).
    """
    if isinstance(branch, ast_.Maybe) and isinstance(branch.inner, ast_.RuleRef) and branch.inner.name == rule_name:
        return None
    if isinstance(branch, ast_.Sequence):
        if branch.tail:
            last_edge, last_atom = branch.tail[-1]
            if (
                isinstance(last_atom, ast_.Maybe)
                and isinstance(last_atom.inner, ast_.RuleRef)
                and last_atom.inner.name == rule_name
            ):
                new_tail = branch.tail[:-1]
                if not new_tail:
                    return branch.head
                return ast_.Sequence(head=branch.head, tail=new_tail, span=branch.span)
    return branch


# Dynamic CompileCtx attributes for the cyclic strategy (same pattern as
# _RULE_VALIDATED_KEY — avoids touching the CompileCtx definition):
# pending back-references collected while a cyclic body is being lowered, and
# the per-compile cache of lowered body roots (shared across reference sites).
_CYCLIC_PENDING_KEY = "_mch_cyclic_pending"   # dict[str, list[_MchCyclicRefMatcher]]
_CYCLIC_ROOTS_KEY = "_mch_cyclic_roots"       # dict[str, BlockMatcher]


def _check_cyclic_rule_body(target: ast_.RuleDecl, ctx: CompileCtx) -> None:
    """C012 shape rules for `rule NAME cyclic = …` bodies (spec §10.3):
    no captures (they can never bind — the walker does not descend into the
    back-edge cycle), self-references only as the last atom of their chain
    (anything chained after one is structurally ignored by the engine), and no
    references to other recursive rules (nested recursion, out of scope exactly
    as for the frontier strategy)."""
    name = target.name

    def is_self_ref(a: ast_.PatternExpr) -> bool:
        if isinstance(a, ast_.Maybe):
            return is_self_ref(a.inner)
        return isinstance(a, ast_.RuleRef) and a.name == name

    def err(msg: str, span: ast_.Span) -> None:
        ctx.bag.error("C012_CYCLIC_RULE_SHAPE", f"rule {name!r}: {msg}", span)
        raise CompileError(ctx.bag)

    def walk(p: ast_.PatternExpr) -> None:
        if isinstance(p, ast_.Node):
            if p.capture is not None:
                err(
                    f"capture @{p.capture} inside a cyclic rule body never binds; "
                    f"drive the builder from Match._mch_consumed",
                    p.span,
                )
        elif isinstance(p, ast_.Sequence):
            atoms = [p.head] + [a for _e, a in p.tail]
            for i, atom in enumerate(atoms):
                if is_self_ref(atom) and i != len(atoms) - 1:
                    err(
                        "a self-reference must be the last atom of its chain "
                        "(atoms after it would be structurally ignored)",
                        atom.span,
                    )
                walk(atom)
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                walk(b)
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                walk(it)
        elif isinstance(p, ast_.Maybe):
            if is_self_ref(p.inner):
                err(
                    "`maybe` on a self-reference inside a cyclic body is not "
                    "supported (an IR back-edge carries no per-site optionality); "
                    "add a non-recursive alternative branch instead",
                    p.span,
                )
            walk(p.inner)
        elif isinstance(p, ast_.RuleRef):
            if p.name == name:
                return
            rc = ctx.resolved.rule_recursion.get(p.name)
            if rc in (RecursionClass.DIRECT_RECURSIVE, RecursionClass.INDIRECT_RECURSIVE):
                err(
                    f"reference to recursive rule {p.name!r} inside a cyclic "
                    f"rule body is not supported (nested recursion)",
                    p.span,
                )
            other = next((r for r in ctx.resolved.raw.rules if r.name == p.name), None)
            if other is not None:
                walk(other.pattern)  # non-recursive rules inline; hold them to the same rules

    walk(target.pattern)


def _lower_cyclic_rule(name: str, ctx: CompileCtx) -> BlockMatcher:
    """Lower a reference to a `rule NAME cyclic = …` rule (spec §10.3 cyclic
    descent, [M2R, R11/T28]).

    The body lowers once per compile into a shared matcher graph; in-body
    self-references become back-edges to the body root (patched here after the
    body finishes lowering — the exact legacy hand-built cycle, e.g.
    ToncoSwapBlockMatcher). Every reference site, this outer one included, gets
    its own `_MchCyclicRefMatcher` delegating to the shared root, so per-site
    `optional` flags and the capture walker's slice bookkeeping stay per-site.
    """
    target = next(r for r in ctx.resolved.raw.rules if r.name == name)
    roots: dict[str, BlockMatcher] = getattr(ctx, _CYCLIC_ROOTS_KEY, None) or {}
    if not hasattr(ctx, _CYCLIC_ROOTS_KEY):
        setattr(ctx, _CYCLIC_ROOTS_KEY, roots)
    root = roots.get(name)
    if root is None:
        _check_cyclic_rule_body(target, ctx)
        pending: dict = getattr(ctx, _CYCLIC_PENDING_KEY, None)
        if pending is None:
            pending = {}
            setattr(ctx, _CYCLIC_PENDING_KEY, pending)
        pending[name] = []
        try:
            root = _lower_atom(target.pattern, ctx)
        finally:
            back_refs = pending.pop(name)
        if isinstance(root, _MchCyclicRefMatcher):
            ctx.bag.error(
                "C012_CYCLIC_RULE_SHAPE",
                f"rule {name!r}: body reduces to a bare self-reference",
                target.span,
            )
            raise CompileError(ctx.bag)
        for ref in back_refs:
            ref._mch_cyclic_target = root
        roots[name] = root
    return _MchCyclicRefMatcher(root)


def _lower_recursive_rule(name: str, ctx: CompileCtx) -> BlockMatcher:
    """Lower a recursive rule via the configured RecursionStrategy.

    Phase 3 change: strip trailing `maybe $self` from step branches; pass
    `exit_matcher=None` when there are no pure-exit branches (to avoid the v0
    'always-one-iteration' bug where an `exit_matcher=always_true` short-circuits
    the loop). Use `_MchRecursiveMatcher` to record per-iteration consumed blocks
    for list-capture extraction.
    """
    target = next(r for r in ctx.resolved.raw.rules if r.name == name)
    pattern = target.pattern

    step_branches, exit_branches = _split_recursion_branches(pattern, name)
    has_pure_exit = bool(exit_branches)

    # Strip trailing self-refs from step branches.
    stripped_step_branches: list[ast_.PatternExpr] = []
    for b in step_branches:
        s = _strip_trailing_self_ref(b, name)
        if s is not None:
            stripped_step_branches.append(s)

    if not stripped_step_branches:
        # Fallback: stripping discarded every step branch (e.g. a rule shape like
        # `op A -> ( maybe $self | op B )` where _split_recursion_branches sees
        # only the Alternative branches and the `maybe $self` is the entire step).
        # Preserve v0 behavior by keeping the original branches; the permissive
        # sentinel will fire on the self-ref. The recursion will be bounded by
        # the engine's per-iteration block exhaustion rather than the strip.
        if not step_branches:
            ctx.bag.error(
                "C001_RECURSION_SHAPE_UNSUPPORTED",
                f"rule {name!r}: no non-recursive step branch found",
                target.span,
            )
            raise CompileError(ctx.bag)
        stripped_step_branches = list(step_branches)

    ctx.lowering_recursive.add(name)
    try:
        if len(stripped_step_branches) == 1:
            step = _lower_atom(stripped_step_branches[0], ctx)
        else:
            step = OrMatcher([_lower_atom(b, ctx) for b in stripped_step_branches])

        if has_pure_exit:
            if len(exit_branches) == 1:
                exit_m: BlockMatcher | None = _lower_atom(exit_branches[0], ctx)
            else:
                exit_m = OrMatcher([_lower_atom(b, ctx) for b in exit_branches])
        else:
            exit_m = None
    finally:
        ctx.lowering_recursive.discard(name)

    return _MchRecursiveMatcher(
        repeating_matcher=step,
        exit_matcher=exit_m,
        optional=True,
        has_pure_exit=has_pure_exit,
        step_capture_names=_collect_capture_names(step),
        exit_capture_names=_collect_capture_names(exit_m),
    )


def _collect_capture_names(m: BlockMatcher | None) -> frozenset[str]:
    """All `_mch_capture_name` tags reachable in a lowered matcher subtree.

    Used to attribute a recursive rule's captures to its step vs exit branches
    so the walker pads exactly the right lists (spec §7.2 alignment rule)."""
    names: set[str] = set()
    seen: set[int] = set()

    def rec(x: BlockMatcher | None) -> None:
        if x is None or id(x) in seen:
            return
        seen.add(id(x))
        if isinstance(x, _MchCyclicRefMatcher):
            # Cyclic rule bodies carry no captures (C012); do not descend into
            # the back-edge cycle.
            return
        cap = getattr(x, "_mch_capture_name", None)
        if cap is not None:
            names.add(cap)
        rec(getattr(x, "child_matcher", None))
        rec(getattr(x, "parent_matcher", None))
        for cm in getattr(x, "children_matchers", None) or []:
            rec(cm)
        if isinstance(x, OrMatcher):
            for sub in x.matchers:
                rec(sub)
        if isinstance(x, RecursiveMatcher):
            rec(x.repeating_matcher)
            rec(x.exit_matcher)

    rec(m)
    return frozenset(names)


def _split_recursion_branches(
    pattern: ast_.PatternExpr,
    rule_name: str,
) -> tuple[list[ast_.PatternExpr], list[ast_.PatternExpr]]:
    """Heuristic: separate (step, exit) branches.

    - Branches that *do not* reference `$rule_name` are exits.
    - Branches that reference `$rule_name` outside `maybe` are steps.
    - Branches whose only $rule_name reference is inside `maybe` are also steps
      (the maybe is the termination marker).

    Returns (step_branches, exit_branches). For non-Alternative patterns we return
    one step branch (the whole pattern) and no exits.
    """
    if isinstance(pattern, ast_.Alternative):
        steps: list[ast_.PatternExpr] = []
        exits: list[ast_.PatternExpr] = []
        for b in pattern.branches:
            if _contains_rule_ref(b, rule_name):
                steps.append(b)
            else:
                exits.append(b)
        return steps, exits
    # Sequence with an alternative inside its tail: search recursively.
    if isinstance(pattern, ast_.Sequence):
        # Look for an Alternative within the tail; if present, split it.
        for _e, atom in pattern.tail:
            if isinstance(atom, ast_.Alternative):
                return _split_recursion_branches(atom, rule_name)
    return [pattern], []


def _contains_rule_ref(p: ast_.PatternExpr, name: str) -> bool:
    if isinstance(p, ast_.RuleRef):
        return p.name == name
    if isinstance(p, ast_.Maybe):
        return _contains_rule_ref(p.inner, name)
    if isinstance(p, ast_.Sequence):
        if _contains_rule_ref(p.head, name):
            return True
        return any(_contains_rule_ref(a, name) for _e, a in p.tail)
    if isinstance(p, ast_.Alternative):
        return any(_contains_rule_ref(b, name) for b in p.branches)
    if isinstance(p, ast_.ChildrenBlock):
        return any(_contains_rule_ref(it, name) for it in p.items)
    return False
