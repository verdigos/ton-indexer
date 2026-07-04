# tgBTC mint — remake of blocks/tgbtc.py TgBTCMintBlockMatcher (roadmap R03).
#
# Legacy shape vs. this remake
# ----------------------------
# The mint trace is a single linear call chain of arbitrary length:
#
#     op 0x3F781D24 (head) -> <N arbitrary-opcode hops> -> op 0xE42A879B (tail)
#                                                            -> { success log ;
#                                                                 jetton_mint }
#
# The intermediate hops carry protocol opcodes that vary per mint (short mints
# jump head -> ... -> tail in 3 hops; "very long" mints thread ~11 hops through
# opcodes 0xE92BD172 / 0xD303C8F4 / 0xEFFE02DA / 0x0A48515A that no fixed head
# could enumerate). The legacy matcher anchors on the TAIL (0xE42A879B) and then
# imperatively walks `previous_block` up to 20 hops to rediscover the head — a
# scan the remake gate lets us drop. This remake anchors on the distinctive
# HEAD (0x3F781D24) and descends the chain with an `any`-headed recursion whose
# exclusive exit branch is the tail plus its two-item children set. Same
# consumed set, same produced block.
#
# Deviation from legacy (fixture-safe): the legacy walk also collects the
# non-path children (siblings) of every ancestor it passes. None of the four
# mint fixtures branch off the trunk — the chain is strictly linear — so the
# consumed set is identical. A hypothetical mint that forks mid-chain would have
# its off-trunk siblings consumed by legacy but not here; recording such a trace
# is out of scope (no live-DB access), and no known mint produces one.
#
# Builder: consumed-set-driven (Match._mch_consumed), like specs/evaa.mch. The
# three blocks the core needs each carry a unique signature within one match's
# consumed set — the head is the sole 0x3F781D24 call, the success log the sole
# 0x77A80EF3 call, the mint the sole `jetton_mint` block — so derivation is
# unambiguous. Captures are declared for pattern fidelity but not read (the
# exit-branch captures @tail/@log/@mint would be list bindings under recursion,
# §7.2, and the compiled path's positional walker mis-binds same-position exit
# captures — the same reason evaa/stonfi_v2/dedust_swap drive off the consumed
# set).
#
# Capture -> legacy role map:
#   @head  (anchor, 0x3F781D24)  sender = head.msg.source
#   @tail  (0xE42A879B)          structural — the chain terminus
#   @log   (0x77A80EF3)          TgBTCMintEvent: amount/recipient/txid/teleport
#   @mint  (jetton_mint)         asset + recipient_wallet
#   @mid   (any)                 the intermediate hops (structural only)

opcode TgBTCMintHead       = 0x3F781D24
opcode TgBTCMintTail       = 0xE42A879B
opcode TgBTCMintSuccessLog = 0x77A80EF3

matcher tgbtc_mint {
    produces TgBTCMintBlock
    build    build_tgbtc_mint
    entry    @head

    op TgBTCMintHead @head -> $mint_chain
}

# Descend the trunk: each hop is any block that is not the tail; recursion
# terminates on the tail node (exclusive exit branch), which carries the success
# log and the jetton_mint block as a children set.
rule mint_chain = (
    any @mid -> maybe $mint_chain
  ^ op TgBTCMintTail @tail
      -> { op TgBTCMintSuccessLog @log ; btype jetton_mint @mint }
)
