# Coffee withdraw liquidity — ported from blocks/liquidity.py
# CoffeeWithdrawLiquidityMatcher. FULLY DECLARATIVE build (no host builder, no
# lookup, no parser gap).
#
# The LP burn (the prebuilt `jetton_burn` block the priority-90 jetton family
# produces) whose pool answers with two 0xc0ffee21 payouts and the 0xc0ffee35
# withdrawal log. Same `jetton_burn`-anchored, positionally-bound shape as
# specs/stonfi_v2_withdraw.mch, which this file follows field for field.
#
# Each payout terminates in a jetton payout (a prebuilt `btype jetton_transfer`),
# a TON payout (0xc0ffee32) or a notification (0xc0ffee36). Splitting the
# alternative into per-arm capture names lets the build branch on which arm
# bound, reproducing the legacy `for next_block in payout.next_blocks` scan whose
# two arms are jetton_transfer and 0xc0ffee32 and whose fall-through (a
# notification, or no child at all) leaves the leg at its TON/null defaults.
#
# Anchor claim: `btype jetton_burn` is also claimed by specs/stonfi_v2_withdraw.mch
# and specs/tonstakers.mch reads the same burn opcode. The three are structurally
# disjoint (coffee needs 2x 0xc0ffee21 + 0xc0ffee35 children), so this file's
# position in ir/emit_manifest.json carries no observable tie-break.
#
# ===========================================================================
# LEG ORDER — a KNOWN divergence from the legacy matcher, and why this spec has
# no row in tests/mch/integration/test_parity_sweep.py
# ===========================================================================
# `amount1_out`/`amount2_out` come from the withdrawal event, while
# `asset*_out` / `dex_*wallet_*` / `wallet*` come from payout_1 / payout_2. The
# legacy matcher binds those two payouts by walking `block.next_blocks` in RAW
# order — i.e. the order the trace's message rows happen to carry — while the
# engine walks candidates in CANONICAL order (ascending `min_lt`, spec §10.5,
# the determinism property the whole IR engine is built on).
#
# On the one corpus fixture (`b0QdK2TPX7Pv7NiXvpJxf_0PEumbW1FMewsnbFi-nqs=`,
# tests/test_cases/coffee.yaml "Coffee withdraw liquidity (jetton - jetton)")
# those two orders DISAGREE — the pool's two payouts are stored 0004 before
# 0003 — and the legacy binding is the WRONG one:
#
#   event.amount1 = 100000      payout created_lt ...0003, body amount 100000
#                               -> jetton_transfer asset 0:B113A9.. amount 100000
#   event.amount2 = 100000000   payout created_lt ...0004, body amount 100000000
#                               -> jetton_transfer asset 0:F6EB37.. amount 100000000
#
# Legacy emits `amount1=100000` next to `asset1_out=0:F6EB37..` — pairing the
# small amount with the leg that actually moved the large one. The canonical
# order pairs each event amount with the payout that carries it. The port is
# therefore RIGHT and legacy is WRONG on this trace, which is exactly why a
# legacy-vs-IR sweep row cannot be green: the reference is the bug.
#
# Nothing is worked around here. The fix belongs on the legacy side (order the
# two payout blocks by `min_lt` in CoffeeWithdrawLiquidityMatcher.build_block,
# then re-record the coffee.yaml expectation) and is a lead-level call because it
# changes classified production output. Until then this spec is covered by
# tests/mch/integration/test_coffee_withdraw.py, which pins the canonical
# pairing, asserts every order-independent field against the legacy matcher, and
# fails the moment the legacy leg assignment changes.

opcode CoffeePayoutInternal           = 0xc0ffee21
opcode CoffeePayout                   = 0xc0ffee32
opcode CoffeeLiquidityWithdrawalEvent = 0xc0ffee35
opcode CoffeeNotification             = 0xc0ffee36

matcher coffee_withdraw_liquidity {
    produces dex_withdraw_liquidity
    entry    @burn

    btype jetton_burn @burn
      -> { op CoffeePayoutInternal @payout_1
             -> ( btype jetton_transfer @jt_1 ^ op CoffeePayout @ton_1 ^ op CoffeeNotification )
         ; op CoffeePayoutInternal @payout_2
             -> ( btype jetton_transfer @jt_2 ^ op CoffeePayout @ton_2 ^ op CoffeeNotification )
         ; op CoffeeLiquidityWithdrawalEvent @event }

    parse event as CoffeeLiquidityWithdrawalEvent
    out {
        dex:                 "coffee"
        sender:              burn.data.owner
        sender_wallet:       burn.data.jetton_wallet
        pool:                account(event.msg.source)
        asset:               burn.data.asset
        lp_tokens_burnt:     burn.data.amount
        is_refund:           false
        amount1_out:         amount(event.body.amount1)
        asset1_out:          jt_1.data.asset if jt_1 != null else ton_asset()
        dex_wallet_1:        account(payout_1.msg.source)
        dex_jetton_wallet_1: jt_1.data.sender_wallet if jt_1 != null else null
        wallet1:             jt_1.data.receiver_wallet if jt_1 != null else (account(ton_1.msg.destination) if ton_1 != null else null)
        amount2_out:         amount(event.body.amount2)
        asset2_out:          jt_2.data.asset if jt_2 != null else ton_asset()
        dex_wallet_2:        account(payout_2.msg.source)
        dex_jetton_wallet_2: jt_2.data.sender_wallet if jt_2 != null else null
        wallet2:             jt_2.data.receiver_wallet if jt_2 != null else (account(ton_2.msg.destination) if ton_2 != null else null)
    }
}
