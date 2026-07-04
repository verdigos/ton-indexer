# Stonfi v2 provide liquidity (R08 remake) — remade from blocks/liquidity.py
# StonfiV2ProvideLiquidityMatcher.
#
# Architecture realignment (class C): the legacy matcher pairs the two deposit
# branches IN-MATCHER — a GenericMatcher over a previously produced
# dex_deposit_liquidity block hangs off the in-transfer's parent, and the
# second fire absorbs the first block plus performs the EmptyBlock proxy
# insertion inside build_block. Per spec §5.5 pairing/merging is host-side:
# here each deposit leg is matched independently and produces a class-tagged
# block (StonfiV2DepositLiquidity / StonfiV2DepositLiquidityPartial, both
# btype dex_deposit_liquidity like legacy); the host pass
# post_process_stonfi_v2_liquidity (blocks/liquidity.py) replays the legacy
# pairing — data merge, EmptyBlock proxy surgery, and the legacy
# condition-fail absorption — over the produced blocks.
#
# `shape` (§8.7) is NOT used: the proxy insertion happens at PAIRING time in
# legacy, and pairing knowledge only exists in the host pass — a per-matcher
# shaper fires before the partner is known. Legacy placement is followed:
# surgery lives in the pass.
#
# Split rationale: both legs share one structural shape except the pool
# callback subtree (cb_add_liquidity -> LP mint | refund), which only the
# completing leg carries. `final` (callback mandatory) registers FIRST and
# consumes completed legs; `partial` (no callback) picks up the rest —
# including the legacy edge where a cb call exists but has no LP/refund
# child (legacy's optional cb matcher fails and matches the leg without it).
# The partial builder rejects when a full callback subtree is present, so a
# final-builder rejection reproduces the legacy whole-matcher rejection
# instead of downgrading to a partial.
#
# In-transfer arms mirror the legacy OrMatcher order: raw pTON leg
# (JettonNotify <- 0x01f3835d), prebuilt jetton_transfer, and the raw
# notify-chain fallback behind a prebuilt transfer. Runs after
# JettonTransferBlockMatcher / PTonTransferMatcher, like legacy
# (event_processing order).
#
# Builders are consumed-set driven (Match._mch_consumed + previous_block
# navigation, dedust_deposit convention); captures are kept for pattern
# fidelity only. Data building delegates to the shared
# build_stonfi_v2_provide_core (same fn the legacy matcher now calls).

opcode StonfiV2ProvideLiquidity = 0x37c096df
opcode StonfiV2AddLiquidity     = 0x50c6a654
opcode StonfiV2CbAddLiquidity   = 0x06ecd527
opcode JettonNotify             = 0x7362d09c
opcode JettonInternalTransfer   = 0x178d4519
opcode PTonTransfer             = 0x01f3835d

# The incoming deposit transfer feeding the router (legacy OrMatcher order):
# pTON leg (raw notify fronted by the 0x01f3835d call), a prebuilt jetton
# transfer, or a raw notify chain behind a prebuilt transfer.
rule in_leg = (
    op JettonNotify @notify_pton <- op PTonTransfer @in_pton
  | btype jetton_transfer @in_jt
  | op JettonNotify @notify_raw <- op JettonInternalTransfer @internal_raw <- btype jetton_transfer @in_jt_raw
)

matcher stonfi_v2_provide_final {
    produces StonfiV2DepositLiquidity
    build    build_stonfi_v2_provide_final
    entry    @provide

    op StonfiV2ProvideLiquidity @provide
      -> { op StonfiV2AddLiquidity @add
             -> op StonfiV2CbAddLiquidity @cb
                  -> ( op JettonInternalTransfer @lp_transfer
                     | op StonfiV2AddLiquidity @refund ) }
      <- $in_leg
}

matcher stonfi_v2_provide_partial {
    produces StonfiV2DepositLiquidityPartial
    build    build_stonfi_v2_provide_partial
    entry    @provide

    op StonfiV2ProvideLiquidity @provide
      -> { op StonfiV2AddLiquidity @add }
      <- $in_leg
}
