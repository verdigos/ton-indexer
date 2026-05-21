from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable, TypeVar

F = TypeVar("F", bound=Callable[..., Any])


@dataclass(eq=False)
class Registries:
    # eq=False keeps identity-based __hash__/__eq__, which lets Registries serve as
    # a weakref key in the compiler's subclass cache.
    opcodes:     dict[str, int]                                    = field(default_factory=dict)
    predicates:  dict[str, Callable[[Any], bool]]                  = field(default_factory=dict)
    block_types: dict[str, type]                                   = field(default_factory=dict)
    builders:    dict[str, Callable[..., Awaitable[Any]]]          = field(default_factory=dict)

    def register_predicate(self, name: str) -> Callable[[F], F]:
        def deco(fn: F) -> F:
            if name in self.predicates:
                raise KeyError(f"predicate {name!r} already registered")
            self.predicates[name] = fn  # type: ignore[assignment]
            return fn
        return deco

    def register_builder(self, name: str) -> Callable[[F], F]:
        def deco(fn: F) -> F:
            if name in self.builders:
                raise KeyError(f"builder {name!r} already registered")
            self.builders[name] = fn  # type: ignore[assignment]
            return fn
        return deco
