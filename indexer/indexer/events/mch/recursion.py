from __future__ import annotations

from typing import TYPE_CHECKING, Any, Protocol

if TYPE_CHECKING:
    from indexer.events.blocks.basic_matchers import BlockMatcher
    from indexer.events.mch.resolver import ResolvedMatcher


class RecursionStrategy(Protocol):
    """Lowers a recursive rule reference to an engine matcher.

    v0 default implementation lives at the bottom of this module.
    Future swap-in: BoundedUnrollingStrategy, SubclassPerRuleStrategy.
    """

    def lower_recursive_rule(
        self,
        rule_name: str,
        ctx: Any,                  # CompileCtx, typed at the call site
    ) -> "BlockMatcher":
        ...


class RecursiveMatcherStrategy:
    """v0: lowers a recursive rule reference to a RecursiveMatcher instance.

    Implementation lives in compiler.py for now (it needs the compiler's lowering helpers).
    This class is a placeholder that the compiler instantiates and uses; the actual
    behavior is invoked through compiler.CompileCtx.
    """
    pass
