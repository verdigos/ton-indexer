# LayerZero receive — full port of blocks/layerzero.py LayerZeroReceiveMatcher.
#
# Structure (a single anchor with a nested children set, no recursion):
#
#   op ChannelLzReceivePrepare @prepare          # anchor (test_self)
#     -> { maybe ton_transfer                     # excesses_transfer (optional)
#        ; op LayerzeroLzReceivePrepare
#            -> op ChannelLzReceiveLock
#            -> op LayerzeroLzReceiveExecute @execute
#                 -> { maybe op LayerZeroEventMsgBody          # even_1  (optional)
#                    ; op LayerZeroOappExecuteCallback @oapp_callback
#                        -> { maybe op LayerZeroEventMsgBody   # event_2 (optional)
#                           ; maybe ton_transfer } } }         # excess_transfer_2
#
# DUP-LABEL TRAP (the reason there are two capture names here): the legacy file
# reuses the label "execute_callback" on TWO nested, non-exclusive nodes —
# layerzero.py:363 (LayerzeroLzReceiveExecute, 0x0c7b8418) and layerzero.py:368
# (LayerZeroOappExecuteCallback, 0xcaae25a1). Same-name captures are only legal
# across exclusive branches (R005), so the two nodes get DISTINCT names here:
# @execute (outer) and @oapp_callback (inner). The legacy `get_labeled` returns
# the FIRST label in traversal order — the OUTER @execute — so the builder maps
# @execute to the block the legacy build_block consumed (verified on both
# corpus fixtures). @oapp_callback is bound for pattern fidelity but the legacy
# build never read the inner one.
#
# Captures bind as scalars (non-recursive pattern, spec §7.1/§7.2); the builder
# reads @execute off the Match and the anchor off _mch_consumed[0].

opcode ChannelLzReceivePrepare      = 0x22f3ac09
opcode LayerzeroLzReceivePrepare    = 0x97df404c
opcode ChannelLzReceiveLock         = 0xb7680bc6
opcode LayerzeroLzReceiveExecute    = 0x0c7b8418
opcode LayerZeroOappExecuteCallback = 0xcaae25a1
opcode LayerZeroEventMsgBody        = 0xe33b9873

matcher layerzero_receive {
    produces LayerZeroReceiveBlock
    build    build_layerzero_receive
    entry    @prepare

    op ChannelLzReceivePrepare @prepare
      -> { maybe btype ton_transfer
         ; op LayerzeroLzReceivePrepare
             -> op ChannelLzReceiveLock
             -> op LayerzeroLzReceiveExecute @execute
                  -> { maybe op LayerZeroEventMsgBody
                     ; op LayerZeroOappExecuteCallback @oapp_callback
                         -> { maybe op LayerZeroEventMsgBody
                            ; maybe btype ton_transfer } } }
}
