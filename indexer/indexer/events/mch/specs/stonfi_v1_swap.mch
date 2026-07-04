# Stonfi V1 swap ([M2R, R12]) — remake of blocks/swaps.py StonfiSwapBlockMatcher.
#
# The legacy hack: the chain matches EVERY payment request (any exit code)
# that carries a jetton transfer, then IN BUILD classifies them by exit code
# and bfs-REMOVES the ok_ref referral-payment subtree from the consumed set
# (build-side consumption surgery). The remake moves the disambiguation to
# match time: the payment-request node is gated on the sender-related exit
# codes (ok 0xc64370e5 / no_liq 0x5ffe1295 / reserve_err 0x38976e9b), so the
# ok_ref (0x45078540) referral subtree is simply never consumed — it then
# classifies on its own as a jetton_transfer action, which is exactly the
# state the legacy surgery produced. The builder still reads the referral
# amount/address from the UNCONSUMED sibling through the tree (the swap
# call's next_blocks) — the R05 nominator precedent of inspecting without
# consuming.
#
# Exit-code parsing needs the message body, so the gate is a host predicate:
# T33 inline `.field` predicates see only block.data and a CallContractBlock
# carries none (the roadmap's hope that T33 alone kills the surgery did not
# survive contact with the body-parsing requirement; the predicate does).
#
# The payment-request edge is multi-match (§4.2): when the referral is paid
# through a second SENDER-RELATED payment request (legacy's out_amt<amount
# branch) both requests match and stay consumed — no surgery in that shape,
# and none here.

opcode StonfiSwap           = 0x25938561
opcode StonfiPaymentRequest = 0xf93bb43f

predicate stonfi_v1_sender_payment

matcher stonfi_v1_swap {
    produces JettonSwapBlock
    build    build_stonfi_v1_swap
    entry    @in_transfer

    btype jetton_transfer @in_transfer
      -> op StonfiSwap @swap
      -> op StonfiPaymentRequest @req where stonfi_v1_sender_payment
      -> btype jetton_transfer @out
}
