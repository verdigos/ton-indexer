# Matches multisig v2 order creation, approval by opcode or comment, and execution.

opcode MultisigNewOrder       = 0xF718510F
opcode MultisigInitOrder      = 0x9C73FBA2
opcode MultisigApprove        = 0xA762230F
opcode MultisigApproveAccepted = 0x82609BF6
opcode MultisigApproveRejected = 0xAFAF283E
opcode MultisigExecute        = 0x75097F5D

matcher multisig_create_order {
    produces multisig_create_order
    action direct

    op MultisigNewOrder @new as MultisigNewOrder -> op MultisigInitOrder @init as MultisigInitOrder

    require order = lookup multisig_order(init.msg.destination)
    let signers = map(order.signers as signer => account(signer))
    out {
        source:                new.msg.source
        destination:           new.msg.destination
        destination_secondary: init.msg.destination
        accounts:              signers
        query_id:              new.body.query_id
        order_seqno:           new.body.order_seqno
        is_created_by_signer:  new.body.is_signer
        is_signed_by_creator:  init.body.approve_on_init
        signer_index:          new.body.singer_index
        expiration_date:       new.body.expiration_date
        order_boc:             b64(init.body.order)
    }
}

matcher multisig_approve {
    produces multisig_approve

    op MultisigApprove @vote as MultisigApprove
      -> xor {
             op MultisigApproveAccepted @accepted
             op MultisigApproveRejected @rejected as MultisigApproveRejected
         }

    require order = lookup multisig_order(vote.msg.destination)
    out {
        signer:       vote.msg.source
        order:        vote.msg.destination
        success:      accepted != null
        signer_index: vote.body.signer_index
        exit_code:    rejected.body.exit_code ?? 0
        signers:      order.signers ?? []
    }
}

matcher multisig_approve_comment {
    produces multisig_approve

    btype ton_transfer @vote where (.comment == "approve")
      -> xor {
             op MultisigApproveAccepted @accepted
             op MultisigApproveRejected @rejected as MultisigApproveRejected
         }

    require order = lookup multisig_order(vote.msg.destination)
    out {
        signer:       vote.msg.source
        order:        vote.msg.destination
        success:      accepted != null
        # Comment approvals lack a 64 bit query id, so the signer index stays minus one.
        signer_index: -1
        exit_code:    rejected.body.exit_code ?? 0
        signers:      order.signers ?? []
    }
}

matcher multisig_execute {
    produces multisig_execute

    op MultisigExecute @exec as MultisigExecute

    require order = lookup multisig_order(exec.msg.source)
    out {
        query_id:               exec.body.query_id
        multisig:               exec.msg.destination
        success:                not exec.failed
        order_contract_address: exec.msg.source
        order_seqno:            exec.body.order_seqno
        expiration_date:        exec.body.expiration_date
        approvals_num:          exec.body.approvals_num
        signers_hash_str:       b64(bytes_of(exec.body.signers_hash))
        order_boc_str:          b64(exec.body.order)
        signers:                order.signers ?? []
    }
}
