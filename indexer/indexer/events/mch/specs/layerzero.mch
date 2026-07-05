# LayerZero receive — port of blocks/layerzero.py LayerZeroReceiveMatcher, now
# DECLARATIVE build (M4 §9). Structure unchanged (single anchor + nested children
# set, no recursion); only the build body moved from builders/layerzero.py into
# the spec.
#
# Structure:
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
# DUP-LABEL TRAP (why two capture names): the legacy file reuses the label
# "execute_callback" on TWO nested, non-exclusive nodes — layerzero.py:363
# (LayerzeroLzReceiveExecute, 0x0c7b8418) and layerzero.py:368
# (LayerZeroOappExecuteCallback, 0xcaae25a1). Same-name captures are only legal
# across exclusive branches (R005), so the two nodes get DISTINCT names here:
# @execute (outer) and @oapp_callback (inner). The legacy `get_labeled` returns
# the FIRST label in traversal order — the OUTER @execute — so the build reads
# @execute (channel source, receiver-OApp destination) for the packet + envelope.
# @oapp_callback is bound for pattern fidelity but the legacy build never read it.
#
# Captures bind as scalars (non-recursive pattern, §7.1/§7.2). The build parses
# @execute's body (the OApp callback carries the LayerZero packet) and maps every
# LayerZeroReceiveData field straight off the two envelopes + the parsed packet.
#
# GAP (address-form normalization): the legacy safety check compares the execute
# destination against the packet's dst_oapp with `dest[2:].lower() ==
# dst_oapp[2:].lower()`. The two carry the same 256-bit hash in DIFFERENT string
# forms — the envelope destination is `0:<hex>` (a TON address), the packet
# dst_oapp is `0x<hex>` (a bare 256-bit hash). `account()` cannot bridge them:
# `account("0x<hex>")` raises AddressError (verified), so `account(a) == account(b)`
# rejects every real trace. String slicing/lowercasing is out of the expression
# language by design (§9.6, no string manipulation), so this stays a host `fn`.
# Smallest C++-implementable design: a `bool same_hash(addr_str, hash_str)` that
# strips the two prefixes and compares the hex case-insensitively.

opcode ChannelLzReceivePrepare      = 0x22f3ac09
opcode LayerzeroLzReceivePrepare    = 0x97df404c
opcode ChannelLzReceiveLock         = 0xb7680bc6
opcode LayerzeroLzReceiveExecute    = 0x0c7b8418
opcode LayerZeroOappExecuteCallback = 0xcaae25a1
opcode LayerZeroEventMsgBody        = 0xe33b9873

matcher layerzero_receive {
    produces LayerZeroReceiveBlock
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

    parse execute as LayerZeroOappExecuteCallback
    # Safety check from build_layerzero_receive_core: the execute destination must
    # be the same account as the packet's dst_oapp (see GAP note above).
    reject when not layerzero_dst_oapp_matches(execute.msg.destination, execute.body.packet.path.dst_oapp)
    out {
        sender:  account(prepare.msg.source)
        channel: account(execute.msg.source)
        oapp:    account(execute.msg.destination)
        packet_data: {
            src_oapp: execute.body.packet.path.src_oapp
            dst_oapp: execute.body.packet.path.dst_oapp
            src_eid:  execute.body.packet.path.src_eid
            dst_eid:  execute.body.packet.path.dst_eid
            nonce:    execute.body.packet.nonce
            guid:     execute.body.packet.guid
            message:  execute.body.packet.message
        }
    }
}
