"""Parity sweep: EVERY `.mch` spec's v2 IR-engine matchers vs the LEGACY
hand-written matchers they replace, over EVERY corpus fixture that could fire
them.

T29 stage 1 demoted the compiled path: it is no longer run as a runtime here.
The reference chain is now LEGACY <-> IR directly. For one (spec, fixture) pair
two matcher graphs run over independently constructed block trees:

    legacy  =  the hand-written BlockMatcher(s) the spec ports/remakes
    ir      =  compile_file -> emit_ir -> load_artifact -> build_matchers
               (compiling the spec is a frontend-internal step ir_emit needs to
               produce the artifact — no compiled BlockMatcher is ever run)

Every produced block is compared at the same bar as the per-protocol parity
tests: btype, deep per-key `data` equality (value + exact type), `failed`/
`broken` flags, and the consumed event-node multiset
(sorted tx hashes — order-independent, never compared in list order). Block
CLASS and TOPOLOGY are intentionally NOT compared (remakes legitimately deviate
on both while keeping every field above identical — the per-protocol remake
tests pin topology case by case). A divergence names the spec, the fixture, and
the exact differing field path.

Granularity: rows are per (spec, fixture). Unlike the retired compiled-vs-IR
sweep (which keyed each row on a single shared matcher NAME), legacy has no
per-name matcher, so a spec's whole legacy matcher set and whole IR matcher set
are run together (production order) on the fixture and their produced blocks —
filtered to the btypes the spec's IR matchers declare via `.produces`, dropping
prerequisite-built blocks like jetton_transfer — are compared as one group. Same
corpus coverage as before; fewer, coarser row ids.

Fixture selection (fast prefilter): a spec's matchers each anchor on an
`opcode_set` (or a btype anchor ⇒ runs on all fixtures). A spec can only fire on
a trace carrying one of its anchor opcodes as a raw message opcode, so a pair is
generated only when the spec's UNION anchor opcode set intersects the trace's
message opcodes (normalized `& 0xFFFFFFFF` on both sides — DB opcodes may be
signed int32). Sound over-approximation: may keep a pair that ultimately does
not fire (both paths then produce nothing — still a valid "reject identically"
check) but never drops a pair that would fire. A spec whose anchor opcodes are
absent from the whole corpus (e.g. DNS, stonfi_v2 provide) yields zero pairs and
is surfaced as an EXPLICIT skip, never silently dropped.

Registries come straight from indexer.events.mch.builders (the same factories
the per-spec integration tests use); interface data comes from each trace's own
bundle (TestInterfaceRepository). No live DB, no network, no new recording.

The harness depends ONLY on the IR engine's public API (`load_artifact`,
`build_matchers`) — never on engine internals — so it survives the concurrent
engine.py rewrite. If the engine package is not importable, the whole sweep
skips with a reason via `test_ir_engine_available`.
"""
from __future__ import annotations

import collections
import dataclasses
import functools
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import pytest

from indexer.events import context
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.dedust_v2.claims import (
    DedustV2ClaimFeesBlockMatcher,
    DedustV2ClaimRewardBlockMatcher,
)
from indexer.events.blocks.dedust_v2.liquidity import (
    DedustV2WithdrawBlockMatcher,
)
from indexer.events.blocks.dedust_v2.swaps import (
    DedustV2FailedSwapBlockMatcher,
    DedustV2SwapBlockMatcher,
)
from indexer.events.blocks.elections import (
    ElectionDepositStakeBlockMatcher,
    ElectionRecoverStakeBlockMatcher,
)
from indexer.events.blocks.auction import (
    AuctionBidMatcher,
    AuctionOutbidMatcher,
    NftCancelAuctionMatcher,
    NftCancelSaleMatcher,
    NftFinishAuctionMatcher,
    NftPutOnSaleBlockMatcher,
    TeleitemCancelAuctionMatcher,
    TeleitemStartAuctionMatcher,
    UpdateSaleMatcher,
)
from indexer.events.blocks.dns import ChangeDnsRecordMatcher
from indexer.events.blocks.ethena_staking import EthenaDepositBlockMatcher
from indexer.events.blocks.evaa import (
    EvaaSupplyBlockMatcher,
    EvaaWithdrawBlockMatcher,
)
from indexer.events.blocks.jettons import (
    JettonBurnBlockMatcher,
    JettonMintBlockMatcher,
    JettonTransferBlockMatcher,
    PTonTransferMatcher,
)
from indexer.events.blocks.jvault import (
    JVaultClaimBlockMatcher,
    JVaultStakeBlockMatcher,
    JVaultUnstakeBlockMatcher,
    JVaultUnstakeRequestBlockMatcher,
)
from indexer.events.blocks.layerzero import LayerZeroReceiveMatcher
from indexer.events.blocks.liquidity import (
    CoffeeCreateVaultMatcher,
    CoffeeDepositLiquidityMatcher,
    DedustDepositBlockMatcher,
    DedustDepositFirstAssetBlockMatcher,
    DedustWithdrawBlockMatcher,
    StonfiV2ProvideLiquidityMatcher,
    StonfiV2WithdrawLiquidityMatcher,
    ToncoDepositLiquidityMatcher,
    ToncoWithdrawLiquidityMatcher,
)
from indexer.events.blocks.nft import (
    GetgemsNftPurchaseBlockMatcher,
    NftDiscoveryBlockMatcher,
    NftMintBlockMatcher,
    NftTransferBlockMatcher,
    TelegramNftPurchaseBlockMatcher,
)
from indexer.events.blocks.staking import (
    NominatorPoolWithdrawMatcher,
    TONStakersDelayedWithdrawalMatcher,
    TONStakersDepositMatcher,
    TONStakersWithdrawMatcher,
)
from indexer.events.blocks.swaps import (
    CoffeeSwapBlockMatcher,
    DedustSwapBlockMatcher,
    StonfiSwapBlockMatcher,
    StonfiV2SwapBlockMatcher,
    ToncoSwapBlockMatcher,
)
from indexer.events.blocks.tgbtc import TgBTCMintBlockMatcher
from indexer.events.blocks.vesting import VestingSendMessageBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
import indexer.events.mch as mch
from indexer.events.mch import compile_file
from indexer.events.mch.builders import (
    coffee_registries,
    dedust_hybrid_registries,
    dedust_registries,
    dedust_withdraw_registries,
    dns_registries,
    jetton_decl_registries,
    jetton_registries,
    stonfi_v2_registries,
)
# Port-wave factories live in their own modules (builders/__init__.py is not
# extended per-port; per-spec integration tests import the same way).
from indexer.events.mch.builders.coffee import coffee_liquidity_registries
from indexer.events.mch.builders.dedust import dedust_deposit_registries
from indexer.events.mch.builders.dedust_v2 import (
    dedust_v2_claim_fees_registries,
    dedust_v2_claim_reward_registries,
    dedust_v2_swap_registries,
    dedust_v2_withdraw_registries,
)
from indexer.events.mch.builders.elections import elections_registries
from indexer.events.mch.builders.ethena import ethena_registries
from indexer.events.mch.builders.evaa import evaa_registries
from indexer.events.mch.builders.jvault import jvault_registries
from indexer.events.mch.builders.layerzero import layerzero_receive_registries
from indexer.events.mch.builders.getgems import getgems_registries
from indexer.events.mch.builders.nft_mint import nft_mint_registries
from indexer.events.mch.builders.nft_sale import nft_sale_registries
from indexer.events.mch.builders.nft_transfer import nft_transfer_registries
from indexer.events.mch.builders.nominator import nominator_registries
from indexer.events.mch.builders.stonfi_liquidity import stonfi_liquidity_registries
from indexer.events.mch.builders.stonfi_provide import stonfi_provide_registries
from indexer.events.mch.builders.stonfi_v1 import stonfi_v1_registries
from indexer.events.mch.builders.telegram_nft import telegram_nft_registries
from indexer.events.mch.builders.teleitem_auction import teleitem_auction_registries
from indexer.events.mch.builders.tgbtc import tgbtc_registries
from indexer.events.mch.builders.tonco import tonco_liquidity_registries
from indexer.events.mch.builders.tonco_swap import tonco_swap_registries
from indexer.events.mch.builders.tonstakers import tonstakers_registries
from indexer.events.mch.builders.vesting import vesting_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository
from tests.utils.trace_deserializer import load_trace_from_file

# --- IR engine gate (public API only; may be mid-rewrite) -------------------
try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
    _IR_IMPORT_ERR: BaseException | None = None
except Exception as ex:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False
    _IR_IMPORT_ERR = ex

_SPECS_DIR = Path(specs_pkg.__file__).parent
_DECL_DIR = Path(mch.__file__).parent / "specs_decl"
_TRACES_DIR = Path(__file__).resolve().parents[2] / "traces"


def _pton_prereq() -> BlockMatcher:
    """The `pton_transfer` prerequisite, built from specs_decl/pton_transfer.mch.

    Every DEX spec binds `btype jetton_transfer` for its TON legs, so the tree it
    matches against must already carry the blocks the pton matcher builds. That
    preparation used to be the LEGACY `PTonTransferMatcher` injected here, which
    is precisely what hid the missing spec from every gate: the sweep stayed
    green while production (which only ever runs the spec set) dropped the leg.
    It now comes from the spec, so the prereq is only satisfiable if the spec
    exists; spec-vs-legacy equivalence is its own row in the table below.
    """
    art = _mch_ir.load_artifact(_pton_artifact())
    (matcher,) = _mch_ir.build_matchers(art, jetton_decl_registries())
    return matcher


@functools.lru_cache(maxsize=1)
def _pton_artifact() -> dict:
    path = _DECL_DIR / "pton_transfer.mch"
    return emit_ir(compile_file(path, jetton_decl_registries()), [path.name])


# ---------------------------------------------------------------------------
# Spec table: (short key, spec path, registries factory, prerequisite matcher
# factories replayed to prepare the tree, legacy matcher classes the spec ports/
# remakes). Prereqs are the tree preparation each leg needs, and the legacy
# classes are the reference the IR matchers must reproduce — the same classes
# the per-spec integration tests use.
# ---------------------------------------------------------------------------
_PREREQ_SWAP = (JettonTransferBlockMatcher, _pton_prereq)
_JETTON_LEGACY = (JettonTransferBlockMatcher, JettonBurnBlockMatcher, JettonMintBlockMatcher)

# A few legacy matchers want a DIFFERENT tree-prep than their IR port. dedust
# deposit is the case in the corpus: the legacy pair matches the raw jetton
# chain directly (run with NO prerequisites), while the IR port matches prebuilt
# `jetton_transfer` blocks (the mch registration order) — see the spec header and
# test_dedust_deposit.py. Keyed by spec_key; absent ⇒ legacy uses the same
# prereqs as IR.
_LEGACY_PREREQ_OVERRIDE: dict[str, tuple] = {
    "specs/dedust_deposit.mch": (),
}

# Per-prereq substitution applied to the LEGACY leg only. `_pton_prereq` builds
# a declarative (plain `Block`) jetton_transfer, which is what production feeds
# the IR matchers — but several legacy matchers discriminate on block CLASS
# (`isinstance(block, JettonTransferBlock)`, swaps.py:228), so the legacy leg
# would silently stop matching. It preps with the legacy pton matcher instead;
# the two are proven equivalent by the specs_decl/pton_transfer.mch row below.
_LEGACY_PREREQ_SUBST: dict[Callable, Callable] = {_pton_prereq: PTonTransferMatcher}

_SPEC_TABLE: list[tuple[str, Path, Callable, tuple, tuple]] = [
    ("specs/jettons.mch", _SPECS_DIR / "jettons.mch", jetton_registries, (), _JETTON_LEGACY),
    ("specs/jetton_transfer.mch", _SPECS_DIR / "jetton_transfer.mch", jetton_registries, (),
     (JettonTransferBlockMatcher,)),
    ("specs/coffee_swap.mch", _SPECS_DIR / "coffee_swap.mch", coffee_registries, _PREREQ_SWAP,
     (CoffeeSwapBlockMatcher,)),
    ("specs/dedust_swap.mch", _SPECS_DIR / "dedust_swap.mch", dedust_registries, _PREREQ_SWAP,
     (DedustSwapBlockMatcher,)),
    ("specs/dedust_withdraw.mch", _SPECS_DIR / "dedust_withdraw.mch", dedust_withdraw_registries,
     _PREREQ_SWAP, (DedustWithdrawBlockMatcher,)),
    ("specs/dns.mch", _SPECS_DIR / "dns.mch", dns_registries, (), (ChangeDnsRecordMatcher,)),
    ("specs/stonfi_v2_swap.mch", _SPECS_DIR / "stonfi_v2_swap.mch", stonfi_v2_registries,
     _PREREQ_SWAP, (StonfiV2SwapBlockMatcher,)),
    # specs_decl/jettons.mch is the fully declarative jetton family. It is now
    # legacy-faithful across the corpus: the `addr_none()` builtin (G1) lets its
    # transfer emit an addr_none response_address exactly like the legacy build
    # core (previously it emitted None here — the sole divergence that kept it out
    # of this sweep). It runs alongside the builder spec (specs/jettons.mch) as an
    # independent legacy-vs-IR check on the declarative path.
    ("specs_decl/jettons.mch", _DECL_DIR / "jettons.mch", jetton_decl_registries, (), _JETTON_LEGACY),
    # specs_decl/pton_transfer.mch — the pTON leg every DEX spec depends on, and
    # the reference this file's `_pton_prereq` builds. It shares the 0x0f8a7ea5
    # anchor with jetton_transfer, so the prereq below is what makes a real
    # (internal-leg) transfer get consumed first, exactly as the registration
    # order does in production.
    ("specs_decl/pton_transfer.mch", _DECL_DIR / "pton_transfer.mch", jetton_decl_registries,
     (JettonTransferBlockMatcher,), (PTonTransferMatcher,)),
    ("specs_decl/dedust_swap_hybrid.mch", _DECL_DIR / "dedust_swap_hybrid.mch", dedust_hybrid_registries,
     _PREREQ_SWAP, (DedustSwapBlockMatcher,)),
    # Port wave (2026-07-04). Prereqs mirror each spec's own integration test.
    # Supply + withdraw run as one group, in the legacy matchers-list order
    # (supply 54, withdraw 55; liquidate 56 is not ported). Both bind prebuilt
    # `btype jetton_transfer` blocks — supply for its jetton anchor and its
    # refund leg, withdraw for its jetton payout.
    ("specs/evaa.mch", _SPECS_DIR / "evaa.mch", evaa_registries, (JettonTransferBlockMatcher,),
     (EvaaSupplyBlockMatcher, EvaaWithdrawBlockMatcher)),
    ("specs/layerzero.mch", _SPECS_DIR / "layerzero.mch", layerzero_receive_registries, (),
     (LayerZeroReceiveMatcher,)),
    # The whole tonstakers family runs as one group, in the legacy matchers-list
    # order (delayed withdrawal idx 1, deposit idx 4, withdraw idx 5) — which is
    # also the spec's source order, the tie-break for their shared `priority 85`.
    ("specs/tonstakers.mch", _SPECS_DIR / "tonstakers.mch", tonstakers_registries, (NftMintBlockMatcher,),
     (TONStakersDelayedWithdrawalMatcher, TONStakersDepositMatcher, TONStakersWithdrawMatcher)),
    ("specs/ethena.mch", _SPECS_DIR / "ethena.mch", ethena_registries,
     (JettonTransferBlockMatcher, JettonMintBlockMatcher), (EthenaDepositBlockMatcher,)),
    ("specs/vesting.mch", _SPECS_DIR / "vesting.mch", vesting_registries, (),
     (VestingSendMessageBlockMatcher,)),
    ("specs/dedust_deposit.mch", _SPECS_DIR / "dedust_deposit.mch", dedust_deposit_registries,
     _PREREQ_SWAP, (DedustDepositBlockMatcher, DedustDepositFirstAssetBlockMatcher)),
    # T12.2 spec vs the upstream v1.3 reference matcher (merge-v12: the legacy
    # matcher landed after the spec was written, so the row only became possible
    # once the spec's `out` keys were aligned to the reference's data keys).
    ("specs/dedust_v2_claim_reward.mch", _SPECS_DIR / "dedust_v2_claim_reward.mch",
     dedust_v2_claim_reward_registries, _PREREQ_SWAP, (DedustV2ClaimRewardBlockMatcher,)),
    # The LP trading-fees claim, sibling of the reward claim above. Four corpus
    # fixtures (dedust-liquidities.yaml): TON-only, TON+jetton, jetton+jetton,
    # zero-fees — which between them exercise both payout-slot arms plus the
    # no-leg shape.
    ("specs/dedust_v2_claim_fees.mch", _SPECS_DIR / "dedust_v2_claim_fees.mch",
     dedust_v2_claim_fees_registries, _PREREQ_SWAP, (DedustV2ClaimFeesBlockMatcher,)),
    # The dedust_v2 swap family: both matchers run as one group, mirroring the
    # production order (success first, failure only where it bailed).
    ("specs_decl/dedust_v2_swap.mch", _DECL_DIR / "dedust_v2_swap.mch",
     dedust_v2_swap_registries, _PREREQ_SWAP,
     (DedustV2SwapBlockMatcher, DedustV2FailedSwapBlockMatcher)),
    # The liquidity withdrawal. Nine corpus fixtures: three aborted (no
    # Withdrawal event), five successful across the jetton/jetton, jetton/TON
    # and TON/jetton leg pairings, and one truncated chain neither engine
    # classifies. The five successful ones carry the §4.5 nested-excess
    # divergence documented in the spec header — see _KNOWN_AUX_DIVERGENCE.
    ("specs/dedust_v2_withdraw.mch", _SPECS_DIR / "dedust_v2_withdraw.mch",
     dedust_v2_withdraw_registries, _PREREQ_SWAP, (DedustV2WithdrawBlockMatcher,)),
    # Elector deposit + recover as one group, mirroring the legacy list order.
    # Only the recover half has corpus coverage (a0rjbO1X…); the deposit anchor
    # 0x4e73744b appears in no fixture, so its pairs are reject-identically
    # checks on whatever fixtures the recover anchor drags in.
    ("specs/elections.mch", _SPECS_DIR / "elections.mch", elections_registries, (),
     (ElectionDepositStakeBlockMatcher, ElectionRecoverStakeBlockMatcher)),
    # Both tonco liquidity matchers run as one group, in the legacy matchers-list
    # order (deposit 64, withdraw 66; the deploy-pool matcher between them is not
    # ported). The withdraw payout legs are `btype jetton_transfer` blocks, so the
    # transfer prerequisites are load-bearing for it too.
    ("specs/tonco_liquidity.mch", _SPECS_DIR / "tonco_liquidity.mch", tonco_liquidity_registries,
     (NftMintBlockMatcher,) + _PREREQ_SWAP,
     (ToncoDepositLiquidityMatcher, ToncoWithdrawLiquidityMatcher)),
    # Coffee liquidity port (2026-07-30): the deposit and create-vault matchers
    # run as one group, in the legacy matchers-list order. The deposit leg binds
    # `btype jetton_transfer` for its jetton side, so the transfer prerequisites
    # are replayed.
    #
    # specs/coffee_withdraw.mch has NO row here, deliberately: the legacy
    # CoffeeWithdrawLiquidityMatcher binds its two payouts in RAW `next_blocks`
    # order while the engine uses canonical (`min_lt`) order, and on the single
    # corpus fixture those disagree — with legacy pairing the event's amount1
    # with the payout that actually moved amount2. The reference is the bug, so
    # a legacy-vs-IR row cannot be green; see the spec header and
    # test_coffee_withdraw.py, which pins the canonical pairing instead.
    ("specs/coffee_liquidity.mch", _SPECS_DIR / "coffee_liquidity.mch", coffee_liquidity_registries,
     (JettonTransferBlockMatcher, _pton_prereq),
     (CoffeeDepositLiquidityMatcher, CoffeeCreateVaultMatcher)),
    # R02 remake — jvault family (stake btype-anchored, the other three op-anchored).
    ("specs/jvault.mch", _SPECS_DIR / "jvault.mch", jvault_registries, _PREREQ_SWAP,
     (JVaultStakeBlockMatcher, JVaultUnstakeBlockMatcher, JVaultClaimBlockMatcher,
      JVaultUnstakeRequestBlockMatcher)),
    # Remake wave 1 (2026-07-04): R01/R03/R04/R05.
    # telegram_nft had NO row until 2026-07-30: legacy test_self is a bare
    # NftOwnershipAssigned opcode that also fires on non-purchase assignments,
    # while the spec covered only the ton_transfer->AuctionFillUp fragment shape,
    # so the row diverged on exactly those out-of-scope fixtures. The parent-edge
    # widening (external-driven parent) plus the non-consuming
    # telegram_nft_assigned matcher closed both gaps; the two matchers run as one
    # group, in spec order.
    ("specs/telegram_nft.mch", _SPECS_DIR / "telegram_nft.mch", telegram_nft_registries, (),
     (TelegramNftPurchaseBlockMatcher,)),
    ("specs/tgbtc.mch", _SPECS_DIR / "tgbtc.mch", tgbtc_registries, (JettonMintBlockMatcher,),
     (TgBTCMintBlockMatcher,)),
    # stonfi_v2_withdraw is btype-anchored (jetton_burn) — run-on-all row, rejects everywhere off-protocol.
    ("specs/stonfi_v2_withdraw.mch", _SPECS_DIR / "stonfi_v2_withdraw.mch", stonfi_liquidity_registries,
     (JettonTransferBlockMatcher, _pton_prereq, JettonBurnBlockMatcher),
     (StonfiV2WithdrawLiquidityMatcher,)),
    # nominator withdraw is btype-anchored (ton_transfer) — run-on-all row.
    ("specs/nominator.mch", _SPECS_DIR / "nominator.mch", nominator_registries, (),
     (NominatorPoolWithdrawMatcher,)),
    # Remake wave 2 (2026-07-04): R11/T28 cyclic recursion.
    ("specs/tonco_swap.mch", _SPECS_DIR / "tonco_swap.mch", tonco_swap_registries, _PREREQ_SWAP,
     (ToncoSwapBlockMatcher,)),
    # Remake wave 3 (2026-07-04): R07 getgems (btype-anchored — run-on-all), R08 provide legs
    # (host pairing pass out of sweep scope).
    ("specs/getgems.mch", _SPECS_DIR / "getgems.mch", getgems_registries, (NftTransferBlockMatcher,),
     (GetgemsNftPurchaseBlockMatcher,)),
    # Conversion wave (2026-07-10): nft_transfer base remake — opcode-anchored run-on-all
    # row; covers plain (non-purchase) transfers the per-spec purchase fixtures don't.
    ("specs/nft_transfer.mch", _SPECS_DIR / "nft_transfer.mch", nft_transfer_registries, (),
     (NftTransferBlockMatcher,)),
    ("specs/stonfi_v2_provide.mch", _SPECS_DIR / "stonfi_v2_provide.mch", stonfi_provide_registries,
     (JettonTransferBlockMatcher, _pton_prereq), (StonfiV2ProvideLiquidityMatcher,)),
    # Remake wave 4 (2026-07-04): R12 stonfi v1 (consumption surgery killed at match time).
    ("specs/stonfi_v1_swap.mch", _SPECS_DIR / "stonfi_v1_swap.mch", stonfi_v1_registries, _PREREQ_SWAP,
     (StonfiSwapBlockMatcher,)),
    # Teleitem auction port (2026-07-30): both matchers run as one group, in the
    # legacy list order (start then cancel). No prereqs — each anchors on its own
    # raw opcode and reads nothing another matcher builds.
    ("specs/teleitem_auction.mch", _SPECS_DIR / "teleitem_auction.mch", teleitem_auction_registries,
     (), (TeleitemStartAuctionMatcher, TeleitemCancelAuctionMatcher)),
    # NFT mint port (2026-07-30): pred-anchored on the deploy test, so it has no
    # anchor opcode set and is a run-on-all row. No prereqs — the legacy matcher
    # is FIRST in production order and reads nothing another matcher builds.
    ("specs/nft_mint.mch", _SPECS_DIR / "nft_mint.mch", nft_mint_registries, (),
     (NftMintBlockMatcher,)),
    # GetGems sale/auction family port (2026-07-30): all eight matchers run as
    # one group, in the legacy list order. Prereqs are the two specs whose
    # products they bind — nft_transfer (nft_put_on_sale's anchor, and the
    # returned item of both cancel matchers) and getgems' nft_purchase
    # (nft_finish_auction's child). auction_bid is a bare `btype ton_transfer`
    # anchor so this row runs on ALL fixtures.
    #
    # EXPECTED: this row currently fires on nothing. The family has zero corpus
    # coverage (verified by running the full legacy matcher list over all 176
    # fixtures — none of the eight btypes is produced), so every pair here is a
    # "reject identically" check, not a parity assertion. Fixtures captured from
    # the live A/B windows are what will turn it into a real gate.
    ("specs/nft_sale.mch", _SPECS_DIR / "nft_sale.mch", nft_sale_registries,
     (NftTransferBlockMatcher, GetgemsNftPurchaseBlockMatcher),
     (UpdateSaleMatcher, NftPutOnSaleBlockMatcher, NftDiscoveryBlockMatcher,
      AuctionBidMatcher, AuctionOutbidMatcher, NftCancelSaleMatcher,
      NftFinishAuctionMatcher, NftCancelAuctionMatcher)),
]


@dataclass(frozen=True)
class _SpecEntry:
    spec_key: str
    spec_path: Path
    factory: Callable
    prereqs: tuple
    legacy_prereqs: tuple
    legacy_factories: tuple
    # Union of the spec's matcher names (for diagnostics).
    matcher_names: tuple[str, ...]
    # Union anchor opcodes across ALL the spec's matchers; None ⇒ at least one
    # matcher is btype-anchored, so the spec runs on every fixture.
    anchor_opcodes: frozenset[int] | None

    def __str__(self) -> str:  # nice repr in ids/messages
        return self.spec_key


# ---------------------------------------------------------------------------
# Collection-time discovery: compile + emit each spec once to learn its matcher
# names and (unioned) anchor opcodes. A spec that fails offline is recorded
# (surfaced by test_no_broken_specs), not crashed on.
# ---------------------------------------------------------------------------
def _discover_specs() -> tuple[list[_SpecEntry], list[tuple[str, str]]]:
    entries: list[_SpecEntry] = []
    broken: list[tuple[str, str]] = []
    for key, path, factory, prereqs, legacy in _SPEC_TABLE:
        try:
            compiled = compile_file(path, factory())
            artifact = emit_ir(compiled, [path.name])
        except Exception as ex:  # pragma: no cover - defensive
            broken.append((key, f"{type(ex).__name__}: {ex}"))
            continue
        names: list[str] = []
        ops: set[int] | None = set()
        for m in artifact["matchers"]:
            names.append(m["name"])
            anchor = m["anchor"]
            if ops is not None and anchor["kind"] == "opcode_set":
                ops.update(v & 0xFFFFFFFF for v in anchor["values"])
            else:  # a btype anchor ⇒ the spec runs on every fixture
                ops = None
        entries.append(_SpecEntry(
            key, path, factory, prereqs,
            _LEGACY_PREREQ_OVERRIDE.get(key, prereqs), legacy, tuple(names),
            None if ops is None else frozenset(ops),
        ))
    return entries, broken


def _scan_traces() -> dict[str, tuple]:
    """filename -> (trace, interfaces, opcode_frozenset). Loaded once; the trace
    is reused read-only across pairs (each run builds a fresh Block tree)."""
    cache: dict[str, tuple] = {}
    for f in sorted(_TRACES_DIR.glob("*.lz4")):
        trace, interfaces = load_trace_from_file(f)
        ops = frozenset(
            (m.opcode & 0xFFFFFFFF)
            for tx in trace.transactions
            for m in tx.messages
            if m.opcode is not None
        )
        cache[f.name] = (trace, interfaces, ops)
    return cache


_SPECS, _BROKEN_SPECS = _discover_specs()
# Only pay the corpus-load cost when the sweep can actually run.
_TRACE_CACHE: dict[str, tuple] = _scan_traces() if _HAVE_IR else {}


def _build_pairs() -> tuple[list, list[_SpecEntry]]:
    """Return (fired-candidate pairs, specs with no candidate fixture)."""
    pairs: list = []
    no_fixture: list[_SpecEntry] = []
    for entry in _SPECS:
        hits = [
            fname
            for fname, (_t, _i, ops) in _TRACE_CACHE.items()
            if entry.anchor_opcodes is None or (entry.anchor_opcodes & ops)
        ]
        if hits:
            for fname in hits:
                pairs.append((entry, fname))
        else:
            no_fixture.append(entry)
    return pairs, no_fixture


_PAIRS, _NO_FIXTURE = _build_pairs()
_PAIR_IDS = [f"{e.spec_key}::{fn[:-4]}" for e, fn in _PAIRS]


# ---------------------------------------------------------------------------
# Harness (same BFS + tree-construction prelude as the per-protocol tests, run
# against both matcher groups).
# ---------------------------------------------------------------------------
def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


def _product_of(block: Block) -> Block:
    """The block a matcher actually PRODUCED, given one block it returned.

    Almost every `build_block` returns the new block it just built, whose
    `.parent` is None (merge_blocks sets the parent of the blocks it absorbs,
    not of itself) — so this is the identity for them.

    `AuctionBidMatcher` (blocks/auction.py:112) is the exception: it builds the
    `auction_bid`, merges the anchor into it, and then returns the ANCHOR. The
    return value is only tested for None by the production loop, so the quirk is
    invisible there — but a harness that reads the return value as "the product"
    would see a `ton_transfer` and conclude the matcher produced nothing.
    Walking to the top-most parent recovers the real product in both shapes.
    """
    while block.parent is not None:
        block = block.parent
    return block


async def _run_matchers(matchers: list[BlockMatcher], trace, prereqs=()) -> list[Block]:
    """One fresh tree; prerequisites first, then each matcher in order over a
    full BFS pass (mirrors process_event_async)."""
    root = _build_block_tree(trace)
    for pre in prereqs:
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    produced: list[Block] = []
    seen: set[int] = set()
    for matcher in matchers:
        for b in root.bfs_iter():
            if b.btype == "root" or b.parent is not None:
                continue
            result = await matcher.try_build(b)
            if result is None:
                continue
            # Dedup by identity: a matcher whose build_block returns its merged
            # children (the GenericMatcher default) maps all of them onto the
            # one product.
            for got in result:
                product = _product_of(got)
                if id(product) not in seen:
                    seen.add(id(product))
                    produced.append(product)
    return produced


def _detuple(v):
    """Recursively rewrite tuples as lists.

    A tuple and a list are the SAME value in this comparison: the expression
    language has no tuple literal (§9.6 composite literals are lists and
    records), so a legacy builder's `(asset, amount)` pair and a declarative
    `[asset, amount]` are the same pair — they index identically in the
    serializer (`excess[0]`/`excess[1]`) and render identically. Applied to the
    nested elements too, because the list branch below compares elementwise by
    `repr`. Case in point: coffee/dedust `vault_excesses`.
    """
    if isinstance(v, (list, tuple)):
        return [_detuple(x) for x in v]
    return v


def _values_equal(a, b) -> bool:
    if a is None and b is None:
        return True
    a, b = _detuple(a), _detuple(b)
    if type(a) is not type(b):
        return False
    if isinstance(a, list):
        # List ORDER is not a parity property here: the legacy and IR trees are
        # built independently, and legacy compact_connections() rebuilds
        # next_blocks via list(set(...)) (T21), so order-bearing collections
        # (tonco `excesses`, dedust `vault_excesses`) can flip between the two
        # trees even legacy-vs-legacy. Compare as multisets, like the per-port
        # integration tests do.
        if len(a) != len(b):
            return False
        if a == b:
            return True
        return sorted(map(repr, a)) == sorted(map(repr, b))
    return bool(a == b)


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


# §4.5 nested aux absorption is UNCONDITIONAL for a matched non-root node
# (mch_ir/engine.py `_absorb_aux`, mirrored in C++), while the reference sets
# include_excess per NESTED matcher. The DSL's `include_excess` is
# matcher-level, so a spec whose reference turns nested absorption OFF cannot
# express that, and its IR consumed set legitimately carries excess/bounce
# nodes the legacy one leaves loose. Specs listed here opt into ignoring
# exactly that difference — extra IR-side nodes, aux opcodes only. Every other
# field, and any non-aux consumed-node difference, still fails.
_AUX_OPCODES = frozenset({0xD53276DB, 0xFFFFFFFF})
_AUX_ABSORB_SPECS = frozenset({"specs/dedust_v2_withdraw.mch"})


def _strip_extra_aux(legacy_blocks: list[Block], ir_blocks: list[Block]) -> None:
    """Drop from each IR block the aux event nodes its legacy twin does not
    carry. Mutates `event_nodes`; the blocks are throwaway per-pair products."""
    ls = sorted(legacy_blocks, key=_sort_key)
    irs = sorted(ir_blocks, key=_sort_key)
    for legacy, ir in zip(ls, irs):
        keep = collections.Counter(n.message.tx_hash for n in legacy.event_nodes)
        kept = []
        for n in ir.event_nodes:
            op = n.message.opcode
            aux = op is not None and (op & 0xFFFFFFFF) in _AUX_OPCODES
            if aux and keep[n.message.tx_hash] <= 0:
                continue
            keep[n.message.tx_hash] -= 1
            kept.append(n)
        ir.event_nodes = kept


def _sort_key(block: Block):
    return (block.min_lt, tuple(_node_hashes(block)))


def _data_items(data) -> dict:
    """`block.data` as a comparable mapping. Legacy-style blocks carry dicts;
    the port-wave blocks carry dataclasses (VestingSendMessageData,
    EvaaWithdrawData, ...). vars() is shallow on purpose — nested values go
    through _values_equal like dict values do."""
    if data is None:
        return {}
    if isinstance(data, dict):
        return data
    if dataclasses.is_dataclass(data):
        return vars(data)
    return {"<data>": data}


def _diff_blocks(legacy_blocks: list[Block], ir_blocks: list[Block]) -> list[str]:
    """Field-level divergences between the legacy blocks and the IR blocks.
    Empty list == identical. Each line is an actionable field path. Block CLASS
    and TOPOLOGY are deliberately not compared (see module docstring)."""
    divs: list[str] = []
    if len(legacy_blocks) != len(ir_blocks):
        divs.append(
            f"  produced-block count: legacy={len(legacy_blocks)} ir={len(ir_blocks)}"
        )
    ls = sorted(legacy_blocks, key=_sort_key)
    irs = sorted(ir_blocks, key=_sort_key)
    for i, (c, ir) in enumerate(zip(ls, irs)):
        p = f"  block#{i}"
        if c.btype != ir.btype:
            divs.append(f"{p} btype: legacy={c.btype!r} ir={ir.btype!r}")
        if c.failed != ir.failed:
            divs.append(f"{p} failed: legacy={c.failed} ir={ir.failed}")
        if c.broken != ir.broken:
            divs.append(f"{p} broken: legacy={c.broken} ir={ir.broken}")

        cd, id_ = _data_items(c.data), _data_items(ir.data)
        ck, ik = set(cd.keys()), set(id_.keys())
        if ck != ik:
            divs.append(
                f"{p} data keys: legacy-only={sorted(ck - ik)} ir-only={sorted(ik - ck)}"
            )
        for key in sorted(ck & ik):
            cv, iv = cd[key], id_[key]
            if not _values_equal(cv, iv):
                divs.append(
                    f"{p} data[{key!r}]: legacy={cv!r} ({type(cv).__name__}) "
                    f"ir={iv!r} ({type(iv).__name__})"
                )

        # value_flow intentionally NOT compared (plan §7): the `flow` statement
        # was removed, so declarative matchers no longer populate value_flow
        # while legacy builders still do. value_flow is not persisted / not a
        # parity concern.
        ch, ih = _node_hashes(c), _node_hashes(ir)
        if ch != ih:
            divs.append(f"{p} consumed nodes: legacy={ch} ir={ih}")
    return divs


def _build_legacy_and_ir(entry: _SpecEntry):
    """Instantiate the spec's legacy matchers; compile the spec once (frontend-
    internal) and build its IR matchers. Returns (legacy_matchers, ir_matchers,
    produced_btypes) where produced_btypes is the union of the IR matchers'
    declared `.produces` (used to drop prerequisite-built blocks)."""
    legacy_matchers = [cls() for cls in entry.legacy_factories]
    compiled = compile_file(entry.spec_path, entry.factory())
    artifact = emit_ir(compiled, [entry.spec_path.name])
    art = _mch_ir.load_artifact(artifact)
    ir_matchers = list(_mch_ir.build_matchers(art, entry.factory()))
    btypes: set[str] = set()
    for m in ir_matchers:
        btypes.update(m.produces)
    return legacy_matchers, ir_matchers, btypes


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
def test_ir_engine_available():
    """Whole-sweep gate. Skips (with the import error) when the engine package
    is mid-rewrite, so every parametrized pair below is not silently green."""
    if not _HAVE_IR:
        pytest.skip(f"indexer.events.mch_ir not importable ({_IR_IMPORT_ERR}); sweep cannot run")


def test_no_broken_specs():
    """Every spec in the table must compile + emit its IR artifact offline (no
    registry/predicate/builder gap). Fails loudly, listing any that don't."""
    assert not _BROKEN_SPECS, "specs failed to compile/emit offline:\n" + "\n".join(
        f"  {k}: {r}" for k, r in _BROKEN_SPECS
    )


@pytest.mark.skipif(not _NO_FIXTURE, reason="every spec has ≥1 candidate fixture")
@pytest.mark.parametrize(
    "entry",
    _NO_FIXTURE or [None],
    ids=[str(e) for e in _NO_FIXTURE] or ["none"],
)
def test_spec_has_no_corpus_fixture(entry: _SpecEntry):
    """A spec whose anchor opcode(s) never appear in the corpus: there is no
    legacy-vs-IR pair to compare. Surfaced as an explicit skip (its behavior is
    covered by a synthetic-trace test, e.g. test_dns.py)."""
    opcodes = sorted(hex(o) for o in (entry.anchor_opcodes or ()))
    pytest.skip(
        f"{entry.spec_key} (matchers {list(entry.matcher_names)}): anchor opcode(s) "
        f"{opcodes} absent from all {len(_TRACE_CACHE)} corpus fixtures; no "
        f"legacy-vs-IR pair to compare"
    )


@pytest.mark.asyncio
@pytest.mark.parametrize("entry,filename", _PAIRS, ids=_PAIR_IDS)
async def test_sweep_parity(entry: _SpecEntry, filename: str):
    if not _HAVE_IR:
        pytest.skip(f"indexer.events.mch_ir not importable ({_IR_IMPORT_ERR})")

    trace, interfaces, _ops = _TRACE_CACHE[filename]
    context.interface_repository.set(TestInterfaceRepository(interfaces))

    legacy_matchers, ir_matchers, btypes = _build_legacy_and_ir(entry)

    legacy_blocks = [b for b in await _run_matchers(
        legacy_matchers, trace,
        [_LEGACY_PREREQ_SUBST.get(f, f)() for f in entry.legacy_prereqs]) if b.btype in btypes]
    context.interface_repository.set(TestInterfaceRepository(interfaces))
    ir_blocks = [b for b in await _run_matchers(
        ir_matchers, trace, [cls() for cls in entry.prereqs]) if b.btype in btypes]

    if entry.spec_key in _AUX_ABSORB_SPECS:
        _strip_extra_aux(legacy_blocks, ir_blocks)

    divs = _diff_blocks(legacy_blocks, ir_blocks)
    assert not divs, "\n".join(
        [f"DIVERGENCE {entry.spec_key} on {filename} "
         f"(legacy={len(legacy_blocks)} ir={len(ir_blocks)} blocks):"]
        + divs
    )
