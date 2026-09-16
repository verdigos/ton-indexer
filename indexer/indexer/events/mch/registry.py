from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable, TypeVar

F = TypeVar("F", bound=Callable[..., Any])


@dataclass(eq=False)
class Registries:
    # eq=False keeps identity-based __hash__/__eq__, which lets Registries serve as
    # a weakref key in the compiler's subclass cache.
    opcodes:       dict[str, int]                                    = field(default_factory=dict)
    predicates:    dict[str, Callable[[Any], bool]]                  = field(default_factory=dict)
    # message_types: message parser class by name; constructed from a pytoniq_core Slice.
    # lookups: async interface resolvers, e.g. await lookups["jetton_wallet"](addr).
    # Both empty by default; populate via default_message_types()/default_lookups().
    message_types: dict[str, type]                                   = field(default_factory=dict)
    lookups:       dict[str, Callable[..., Awaitable[Any]]]          = field(default_factory=dict)
    # fns: host-function escape hatch for build expressions — async callables
    # invoked by plain call syntax when the name is not a builtin. Unlike
    # builtins/lookups, null arguments are passed through (an absent optional
    # capture is a legitimate input to imperative host logic).
    fns:           dict[str, Callable[..., Awaitable[Any]]]          = field(default_factory=dict)
    # shapers: post-build tree surgery hooks (`shape IDENT`) — SYNC
    # callables `(produced_block, match) -> None` invoked by the engine after
    # the produced block's merge. May mutate only the produced block's region.
    shapers:       dict[str, Callable[..., Any]]                     = field(default_factory=dict)

    def register_predicate(self, name: str) -> Callable[[F], F]:
        def deco(fn: F) -> F:
            if name in self.predicates:
                raise KeyError(f"predicate {name!r} already registered")
            self.predicates[name] = fn  # type: ignore[assignment]
            return fn
        return deco

    def register_lookup(self, name: str) -> Callable[[F], F]:
        def deco(fn: F) -> F:
            if name in self.lookups:
                raise KeyError(f"lookup {name!r} already registered")
            self.lookups[name] = fn  # type: ignore[assignment]
            return fn
        return deco

    def register_fn(self, name: str) -> Callable[[F], F]:
        def deco(fn: F) -> F:
            if name in self.fns:
                raise KeyError(f"fn {name!r} already registered")
            self.fns[name] = fn  # type: ignore[assignment]
            return fn
        return deco

    def register_shaper(self, name: str) -> Callable[[F], F]:
        def deco(fn: F) -> F:
            if name in self.shapers:
                raise KeyError(f"shaper {name!r} already registered")
            self.shapers[name] = fn  # type: ignore[assignment]
            return fn
        return deco


def default_message_types() -> dict[str, type]:
    """Message parser classes keyed by class name (jetton family + swap messages
    referenced by the spec files in mch/specs/).

    Lazy import: pulling blocks.messages at module-import time would drag the full
    protocol/DB/context stack into the otherwise lightweight mch package.
    """
    from indexer.events.blocks.messages import jettons, swaps

    classes = (
        jettons.JettonTransfer,
        jettons.JettonInternalTransfer,
        jettons.JettonNotify,
        jettons.JettonBurn,
        jettons.JettonBurnNotification,
        jettons.JettonMint,
        jettons.MinterJettonMint,
        swaps.DedustSwap,
        swaps.DedustSwapExternal,
        swaps.DedustSwapPeer,
        swaps.DedustSwapNotification,
        swaps.DedustPayoutFromPool,
        swaps.DedustPayout,
        swaps.StonfiV2PayTo,
        swaps.PTonTransfer,
    )
    return {cls.__name__: cls for cls in classes}


def account_key(address: Any) -> str | None:
    """Normalize an AccountId or raw string to an interface-repository key."""
    from indexer.events.blocks.utils import AccountId

    return address.as_str() if isinstance(address, AccountId) else address.upper()


def default_lookups() -> dict[str, Callable[..., Awaitable[Any]]]:
    """Async interface resolvers backed by the interface_repository contextvar.

    AccountId and raw string addresses resolve to uppercase raw keys.
    """
    from indexer.events import context

    async def jetton_wallet(address: Any) -> Any:
        repo = context.interface_repository.get()
        if repo is None:
            return None
        return await repo.get_jetton_wallet(account_key(address))

    async def nft_item(address: Any) -> Any:
        repo = context.interface_repository.get()
        if repo is None:
            return None
        return await repo.get_nft_item(account_key(address))

    return {
        "jetton_wallet": jetton_wallet,
        "nft_item": nft_item,
    }
