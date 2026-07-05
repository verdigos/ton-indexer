from __future__ import annotations

from typing import Any, Protocol


class RecursionStrategy(Protocol):
    """Lowers a recursive rule reference to a lowered IR node.

    v0 default implementation lives at the bottom of this module.
    Future swap-in: BoundedUnrollingStrategy, SubclassPerRuleStrategy.
    """

    def lower_recursive_rule(
        self,
        rule_name: str,
        ctx: Any,                  # CompileCtx, typed at the call site
    ) -> Any:                      # compiler.LNode, typed at the call site
        ...


class RecursiveMatcherStrategy:
    """v0: lowers a recursive rule reference to a frontier `recursive` IR node.

    Implementation lives in compiler.py (it needs the compiler's lowering
    helpers). This class is a placeholder that the compiler instantiates; the
    actual behavior is invoked through compiler.CompileCtx.
    """
    pass
