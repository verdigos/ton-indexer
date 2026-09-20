# LayerZero receive packets encode the destination OApp as minimal hexadecimal without a workchain prefix.

opcode ChannelLzReceivePrepare      = 0x22f3ac09
opcode LayerzeroLzReceivePrepare    = 0x97df404c
opcode ChannelLzReceiveLock         = 0xb7680bc6
opcode LayerzeroLzReceiveExecute    = 0x0c7b8418
opcode LayerZeroOappExecuteCallback = 0xcaae25a1
opcode LayerZeroEventMsgBody        = 0xe33b9873

opcode EndpointEndpointSend          = 0xdd4ea3b4
opcode ChannelChannelSend            = 0x536feb09
opcode MsglibConnectionSend          = 0x4002b790
opcode UlnUlnSend                    = 0x5de68393
opcode ChannelMsglibSendCallback     = 0x421c1a25
opcode LayerzeroChannelSendCallback  = 0xa2b5fbae

opcode UlnUlnCommitPacket                 = 0x28b97077
opcode UlnConnectionCommitPacket          = 0xf9d37b80
opcode EndpointEndpointCommitPacket       = 0x5dab749a
opcode ChannelCommitPacket                = 0x5388cd88
opcode MsglibConnectionCommitPacketCallback = 0x5e178f33

opcode DvnVerify                    = 0xa0d51a8d
opcode ProxyCallContract            = 0x09f047d9
opcode UlnUlnVerify                 = 0x994aaf4e
opcode UlnConnectionVerify          = 0x4ec8f80a
opcode UlnConnectionVerifyCallback  = 0x3cb38090

matcher layerzero_receive {
    produces layerzero_receive

    op ChannelLzReceivePrepare @prepare
      -> { maybe btype ton_transfer
         ; op LayerzeroLzReceivePrepare
             -> op ChannelLzReceiveLock
             -> op LayerzeroLzReceiveExecute @execute as LayerZeroOappExecuteCallback
                  -> { maybe op LayerZeroEventMsgBody
                     ; op LayerZeroOappExecuteCallback
                         -> { maybe op LayerZeroEventMsgBody
                            ; maybe btype ton_transfer } } }

    reject when not (contains(execute.body.packet.path.dst_oapp, addr_hash_hex(execute.msg.destination)) ?? false)
    out {
        sender:  prepare.msg.source
        channel: execute.msg.source
        oapp:    execute.msg.destination
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

matcher layerzero_send {
    produces layerzero_send

    op EndpointEndpointSend @endpoint_send
      -> op ChannelChannelSend @channel_send
        -> op MsglibConnectionSend
          -> op UlnUlnSend @uln_send
            -> op ChannelMsglibSendCallback
              -> { op LayerZeroEventMsgBody
                 ; many[0..] btype ton_transfer
                 ; op LayerzeroChannelSendCallback @oapp_callback as LayerzeroChannelSendCallback
                     -> { maybe op LayerZeroEventMsgBody
                        ; maybe btype ton_transfer } }

    out {
        initiator: endpoint_send.msg.source
        packet_data: {
            src_oapp: oapp_callback.body.lz_send.packet.path.src_oapp
            dst_oapp: oapp_callback.body.lz_send.packet.path.dst_oapp
            src_eid:  oapp_callback.body.lz_send.packet.path.src_eid
            dst_eid:  oapp_callback.body.lz_send.packet.path.dst_eid
            nonce:    oapp_callback.body.lz_send.packet.nonce
            guid:     oapp_callback.body.lz_send.packet.guid
            message:  oapp_callback.body.lz_send.packet.message
        }
        send_request_id: oapp_callback.body.lz_send.send_request_id
        native_fee:      oapp_callback.body.lz_send.native_fee
        zro_fee:         oapp_callback.body.lz_send.zro_fee
        endpoint:        endpoint_send.msg.destination
        channel:         channel_send.msg.destination
        uln:             uln_send.msg.destination
        msglib_manager:  oapp_callback.body.lz_send.send_msglib_manager
        msglib:          oapp_callback.body.lz_send.send_msglib
    }
}

matcher layerzero_send_tokens {
    produces layerzero_send_tokens

    btype layerzero_send @send
      <- btype jetton_transfer @jetton
        -> { many op LayerZeroEventMsgBody }

    out {
        sender:              jetton.data.sender
        oapp:                jetton.data.receiver
        sender_wallet:       jetton.data.sender_wallet
        oapp_wallet:         jetton.data.receiver_wallet
        asset:               jetton.data.asset
        amount:              jetton.data.amount
        layerzero_send_data: send.data
    }
}

matcher layerzero_commit_packet {
    produces layerzero_commit_packet

    op UlnUlnCommitPacket @uln_commit
      -> op UlnConnectionCommitPacket @uln_connection_commit
        -> op EndpointEndpointCommitPacket @endpoint_commit
          -> op ChannelCommitPacket @channel_commit as ChannelCommitPacket
            -> { maybe op LayerZeroEventMsgBody
               ; maybe op MsglibConnectionCommitPacketCallback @msglib_callback
                   -> maybe btype ton_transfer }

    reject when msglib_callback == null
    out {
        sender:            uln_commit.msg.source
        uln:               uln_connection_commit.msg.destination
        uln_connection:    uln_connection_commit.msg.destination
        endpoint:          endpoint_commit.msg.destination
        channel:           channel_commit.msg.destination
        msglib_connection: msglib_callback.msg.destination
        packet_data: {
            src_oapp: channel_commit.body.packet.path.src_oapp
            dst_oapp: channel_commit.body.packet.path.dst_oapp
            src_eid:  channel_commit.body.packet.path.src_eid
            dst_eid:  channel_commit.body.packet.path.dst_eid
            nonce:    channel_commit.body.packet.nonce
            guid:     channel_commit.body.packet.guid
            message:  channel_commit.body.packet.message
        }
    }
}

matcher layerzero_dvn_verify {
    produces layerzero_dvn_verify

    op DvnVerify @dvn_verify
      -> op ProxyCallContract @proxy_call
        -> op UlnUlnVerify @uln_verify
          -> op UlnConnectionVerify @uln_connection_verify
            -> { maybe op LayerZeroEventMsgBody
               ; maybe op UlnConnectionVerifyCallback @verify_callback as UlnConnectionVerifyCallbackParser
                   -> maybe btype ton_transfer }

    reject when verify_callback == null
    out {
        sender:         dvn_verify.msg.source
        nonce:          verify_callback.body.nonce
        status:         verify_callback.body.status
        dvn:            dvn_verify.msg.destination
        proxy:          proxy_call.msg.destination
        uln:            uln_verify.msg.destination
        uln_connection: uln_connection_verify.msg.destination
    }
}
