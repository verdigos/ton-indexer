# Cocoon — the whole 12-matcher family, ported from blocks/cocoon.py.
#
# Cocoon is a proxied-compute marketplace: a Root registers Proxy contracts,
# Clients stake against a Proxy and are charged per token consumed, Workers are
# paid out for the compute they did. Every matcher here is opcode-anchored,
# depth 2-3, no recursion, no alternatives beyond the two anchor sets, no
# `where` predicates and ZERO interface lookups — structurally the cheapest
# family in the backlog, which is why all twelve ship as one file.
#
# ORDERING / ANCHORS (mandatory note — see the tonstakers and ethena headers for
# why this is written down every time).
# -----------------------------------------------------------------------------
# The nineteen opcodes below are claimed by NO other spec: a grep of every
# `opcode` declaration in specs/ + specs_decl/ returns cocoon alone for each of
# them, and no cocoon matcher anchors on a `btype` (so nothing here can steal a
# block another family builds, and nothing here needs another family to have run
# first). Consequently this file carries the DEFAULT priority 100 and sits LAST
# in ir/emit_manifest.json, mirroring event_processing.matchers where the twelve
# Cocoon matchers are the final twelve entries. Neither the slot nor the
# priority is observable; both are chosen to mirror the reference anyway.
#
# Matcher order WITHIN the file mirrors the legacy list. It is not load-bearing
# either: the two anchor SETS are disjoint from each other and from the ten
# single-opcode anchors, and no cocoon matcher binds a block another cocoon
# matcher produces. The three shared inner opcodes (Payout 0xC59A7CD3,
# ReturnExcessesBack 0x2565934C, ClientProxyRequest 0x65448FF4) appear as
# CHILDREN of several matchers, never as an anchor, so they cannot collide.
#
# PARITY TRAPS honoured here (docs/mch-port-audit.md:310-313)
# -----------------------------------------------------------------------------
#  - `failed` comes from `<x>.msg.transaction.aborted`, NEVER from `<x>.failed`.
#    Every legacy build_block ends `new_block.failed = <msg>.transaction.aborted`,
#    and CallContractBlock force-CLEARS its own `.failed` for an aborted
#    zero-value call (basic_blocks.py:86), so the two disagree exactly on the
#    aborted ClientProxyRequest legs this family is full of. Note WHICH message:
#    worker_payout and grant_refund read the PAYOUT leg's transaction, everything
#    else reads the anchor's.
#  - `msg.opcode` is SIGNED as stored (ExprRuntime.cpp:279) while
#    `block.data.opcode` is the masked u32 on both engines (tree_utils.py:47 does
#    `& 0xFFFFFFFF`, BlockTree.cpp:373 mirrors it). The one place this file needs
#    to discriminate an anchor opcode — `payout_type` — therefore reads
#    `.data.opcode`. Never write `x.msg.opcode == 0xF5F26A36`.
#  - Legacy writes `AccountId(...)` UNCONDITIONALLY for every address it stores,
#    so a missing address becomes addr_none (present-but-empty), not null. Every
#    address field below is `account(x) ?? addr_none()` to match (§9.6).
#
# HOST FNS (2, both in builders/cocoon.py / src/host/HostCocoon.cpp)
# -----------------------------------------------------------------------------
#  - cocoon_expected_address(addr) — the `expectedMyAddress` field of the three
#    payload messages is an ADDRESS THAT IS ROUTINELY EXTERNAL (the fixtures
#    carry `260;8da2…`), and the legacy builds render it as a STRING with two
#    different spellings: `f"{len};{hex(value)[2:]}"` for an ExternalAddress and
#    `to_str(False).upper()` for a std one. The expression language has no string
#    formatting by design (§9.6), so this is a host fn. Shared by
#    cocoon_worker_payout, cocoon_proxy_charge and cocoon_grant_refund.
#  - cocoon_withdraw_amount(client_request) — a NESTED payload parse:
#    ClientProxyRequest.payload is a `maybe_ref` whose cell must then be
#    `begin_parse`d as ClientProxyRefundGranted to reach the coins. `parse
#    CAPTURE as T` reads only the captured block's OWN body (§9.1), so the nested
#    hop needs a host fn. It is TOLERANT by design — legacy wraps the whole thing
#    in try/except and falls back to `withdraw_amount = 0` — so it returns null
#    on any failure and the spec coalesces.
#
# WHAT IS NOT HERE: the `state_data` ref and the `payload` maybe-ref of
# ClientProxyRequest are never read by any legacy build, so ClientProxyRequest
# has no registered ABI row at all; the one build that needs its payload goes
# through the host fn above, which parses the body by hand exactly as legacy does.

opcode CocoonPayoutPayload              = 0xA040AD28
opcode CocoonLastPayoutPayload          = 0xF5F26A36
opcode CocoonWorkerProxyRequest         = 0x4D725D2C
opcode CocoonPayout                     = 0xC59A7CD3
opcode CocoonReturnExcessesBack         = 0x2565934C
opcode CocoonExtProxyPayoutRequest      = 0x7610E6EB
opcode CocoonChargePayload              = 0xBB63FF93
opcode CocoonClientProxyRequest         = 0x65448FF4
opcode CocoonExtClientTopUp             = 0xF172E6C2
opcode CocoonRegisterProxy              = 0x927C7CB5
opcode CocoonUnregisterProxy            = 0x6D49EAF2
opcode CocoonOwnerClientRegister        = 0xC45F9F3B
opcode CocoonOwnerClientChangeSecretHash = 0xA9357034
opcode CocoonOwnerWalletSendMessage     = 0x9C69F376
opcode CocoonOwnerClientRequestRefund   = 0xFAFA6CC1
opcode CocoonGrantRefundPayload         = 0xEFD711E1
opcode CocoonOwnerClientIncreaseStake   = 0x6A1F6A60
opcode CocoonOwnerClientWithdraw        = 0xDA068E78

# -----------------------------------------------------------------------------
# cocoon_worker_payout — blocks/cocoon.py:67 CocoonWorkerPayoutMatcher.
#
#   ext-in -> PayoutPayload|LastPayoutPayload (Proxy -> Worker)
#               -> WorkerProxyRequest (Worker -> Proxy)
#                    -> { Payout (Worker -> Worker Owner) ; maybe excesses }
#
# ONE matcher for BOTH anchor opcodes rather than the mixed-anchor split (C013),
# because the two payloads are LAYOUT-IDENTICAL — `PayoutPayloadData{queryId,
# newTokens, expectedMyAddress}` under both wrappers — so a single `out` block
# serves both and only the `payout_type` literal differs. The alternative parse
# `as CocoonLastPayoutPayload | CocoonPayoutPayload` is what makes the shared
# anchor legal on BOTH engines: the Python leg runs the legacy classes, whose
# `assert body.load_uint(32) == opcode` rejects the wrong wrapper and falls
# through to the second arm, while the C++ generated parsers skip the opcode
# unchecked and the first arm always wins — identical field values either way,
# because the layouts are the same bits.
#
# `payout_type` is the ONE opcode discriminator in the file, hence the
# `.data.opcode` read (see the signed-opcode trap in the header).
#
# `failed` is the PAYOUT leg's transaction, not the anchor's: legacy
# `new_block.failed = payout_msg.transaction.aborted`.
# -----------------------------------------------------------------------------
matcher cocoon_worker_payout {
    produces cocoon_worker_payout
    entry    @start

    ( op CocoonPayoutPayload @start ^ op CocoonLastPayoutPayload @start )
      -> op CocoonWorkerProxyRequest @wreq
         -> { op CocoonPayout @payout
            ; maybe op CocoonReturnExcessesBack @excesses }

    parse start as CocoonLastPayoutPayload | CocoonPayoutPayload
    parse wreq  as CocoonWorkerProxyRequest
    failed when (payout.msg.transaction.aborted ?? false)
    out {
        proxy_contract:   account(start.msg.source) ?? addr_none()
        worker_contract:  account(start.msg.destination) ?? addr_none()
        worker_owner:     account(wreq.body.owner_address) ?? addr_none()
        payout_type:      "last" if start.data.opcode == 0xF5F26A36 else "regular"
        query_id:         start.body.query_id
        new_tokens:       start.body.new_tokens
        expected_address: cocoon_expected_address(start.body.expected_my_address)
        payout_amount:    amount(payout.msg.value)
        worker_state:     wreq.body.state
        worker_tokens:    wreq.body.tokens
    }
}

# -----------------------------------------------------------------------------
# cocoon_proxy_payout — blocks/cocoon.py:241 CocoonProxyPayoutMatcher.
#
#   ext-in -> ExtProxyPayoutRequest (-> Proxy)
#               -> { Payout (Proxy -> Proxy Owner) ; excesses }
#
# Both children REQUIRED. `failed` is the payout leg's transaction.
# -----------------------------------------------------------------------------
matcher cocoon_proxy_payout {
    produces cocoon_proxy_payout
    entry    @req

    op CocoonExtProxyPayoutRequest @req
      -> { op CocoonPayout @payout
         ; op CocoonReturnExcessesBack @excesses }

    parse req as CocoonExtProxyPayoutRequest
    failed when (payout.msg.transaction.aborted ?? false)
    out {
        proxy_contract:     account(req.msg.destination) ?? addr_none()
        proxy_owner:        account(payout.msg.destination) ?? addr_none()
        excesses_recipient: account(req.body.send_excesses_to) ?? addr_none()
        query_id:           req.body.query_id
    }
}

# -----------------------------------------------------------------------------
# cocoon_proxy_charge — blocks/cocoon.py:336 CocoonProxyChargeMatcher.
#
#   ext-in -> ChargePayload (-> Proxy)
#               -> { excesses ; ClientProxyRequest (Proxy -> Client) }
#
# The ClientProxyRequest leg is usually ABORTED (the client is out of balance —
# that is what the charge discovers), which is exactly why `failed` must read the
# ANCHOR's transaction here and not any child's: legacy `new_block.failed =
# start_msg.transaction.aborted`.
#
# The only cocoon matcher that has ever produced a row in production.
# -----------------------------------------------------------------------------
matcher cocoon_proxy_charge {
    produces cocoon_proxy_charge
    entry    @charge

    op CocoonChargePayload @charge
      -> { op CocoonReturnExcessesBack @excesses
         ; op CocoonClientProxyRequest @client_request }

    parse charge as CocoonChargePayload
    failed when (charge.msg.transaction.aborted ?? false)
    out {
        proxy_contract:   account(charge.msg.destination) ?? addr_none()
        client_contract:  account(client_request.msg.destination) ?? addr_none()
        query_id:         charge.body.query_id
        new_tokens_used:  charge.body.new_tokens_used
        expected_address: cocoon_expected_address(charge.body.expected_my_address)
    }
}

# -----------------------------------------------------------------------------
# cocoon_client_top_up — blocks/cocoon.py:435 CocoonClientTopUpMatcher.
#
#   ext-in -> ExtClientTopUp (-> Client)
#               -> ClientProxyRequest (Client -> Proxy)
#                    -> { excesses }
#
# Depth 3. The ClientProxyRequest body is NEVER parsed by the legacy build (only
# its envelope destination is read), so no ABI row is needed for it.
# -----------------------------------------------------------------------------
matcher cocoon_client_top_up {
    produces cocoon_client_top_up
    entry    @topup

    op CocoonExtClientTopUp @topup
      -> op CocoonClientProxyRequest @preq
         -> { op CocoonReturnExcessesBack @excesses }

    parse topup as CocoonExtClientTopUp
    failed when (topup.msg.transaction.aborted ?? false)
    out {
        client_contract: account(topup.msg.destination) ?? addr_none()
        proxy_contract:  account(preq.msg.destination) ?? addr_none()
        sender:          account(topup.body.send_excesses_to) ?? addr_none()
        query_id:        topup.body.query_id
        top_up_amount:   amount(topup.body.top_up_amount)
    }
}

# -----------------------------------------------------------------------------
# cocoon_register_proxy — blocks/cocoon.py:527 CocoonRegisterProxyMatcher.
# The whole build is one field. `proxyInfo` (the message's trailing
# RemainingBitsAndRefs) is bound by the legacy parser and never read.
# -----------------------------------------------------------------------------
matcher cocoon_register_proxy {
    produces cocoon_register_proxy
    entry    @reg

    op CocoonRegisterProxy @reg -> op CocoonReturnExcessesBack @excesses

    parse reg as CocoonRegisterProxy
    failed when (reg.msg.transaction.aborted ?? false)
    out {
        root_contract: account(reg.msg.destination) ?? addr_none()
        query_id:      reg.body.query_id
    }
}

# -----------------------------------------------------------------------------
# cocoon_unregister_proxy — blocks/cocoon.py:597 CocoonUnregisterProxyMatcher.
# Same two-node shape as the register above, plus the seqno.
# -----------------------------------------------------------------------------
matcher cocoon_unregister_proxy {
    produces cocoon_unregister_proxy
    entry    @unreg

    op CocoonUnregisterProxy @unreg -> op CocoonReturnExcessesBack @excesses

    parse unreg as CocoonUnregisterProxy
    failed when (unreg.msg.transaction.aborted ?? false)
    out {
        root_contract: account(unreg.msg.destination) ?? addr_none()
        query_id:      unreg.body.query_id
        seqno:         unreg.body.seqno
    }
}

# -----------------------------------------------------------------------------
# cocoon_client_register — blocks/cocoon.py:670 CocoonClientRegisterMatcher.
#
#   ext-in -> OwnerClientRegister (-> Client)
#               -> { excesses ; ClientProxyRequest (usually aborted) }
#
# The shape shared by the next three matchers (change-secret-hash,
# request-refund, increase-stake): one owner-signed message to the Client, two
# required children. Only the anchor opcode and the payload fields differ.
# -----------------------------------------------------------------------------
matcher cocoon_client_register {
    produces cocoon_client_register
    entry    @reg

    op CocoonOwnerClientRegister @reg
      -> { op CocoonReturnExcessesBack @excesses
         ; op CocoonClientProxyRequest @client_request }

    parse reg as CocoonOwnerClientRegister
    failed when (reg.msg.transaction.aborted ?? false)
    out {
        client_contract: account(reg.msg.destination) ?? addr_none()
        owner:           account(reg.body.send_excesses_to) ?? addr_none()
        query_id:        reg.body.query_id
        nonce:           reg.body.nonce
    }
}

# -----------------------------------------------------------------------------
# cocoon_client_change_secret_hash — blocks/cocoon.py:752.
# `new_secret_hash` stays the raw uint256 INT in the block data; the
# hex-without-0x rendering the DB composite wants is the serializer's job
# (block_tree_serializer.py `hex(int(...))[2:]`, ActionBuild.cpp av_hex_bare) —
# not the spec's, since the language has no string formatting.
# -----------------------------------------------------------------------------
matcher cocoon_client_change_secret_hash {
    produces cocoon_client_change_secret_hash
    entry    @chg

    op CocoonOwnerClientChangeSecretHash @chg
      -> { op CocoonReturnExcessesBack @excesses
         ; op CocoonClientProxyRequest @client_request }

    parse chg as CocoonOwnerClientChangeSecretHash
    failed when (chg.msg.transaction.aborted ?? false)
    out {
        client_contract: account(chg.msg.destination) ?? addr_none()
        owner:           account(chg.body.send_excesses_to) ?? addr_none()
        query_id:        chg.body.query_id
        new_secret_hash: chg.body.new_secret_hash
    }
}

# -----------------------------------------------------------------------------
# cocoon_client_request_refund — blocks/cocoon.py:834.
#
#   direct:      ext-in -> OwnerClientRequestRefund (-> Client) -> { … }
#   via wallet:  ext-in -> OwnerWalletSendMessage (-> Wallet)
#                            -> OwnerClientRequestRefund (-> Client) -> { … }
#
# `via_wallet` is simply "the optional parent matched".
#
# THE ANCHOR SET IS TRANSCRIBED VERBATIM from legacy `test_self` (opcode in
# [OwnerWalletSendMessage, OwnerClientRequestRefund]) even though the wallet arm
# is DEAD, and it is dead twice over:
#   1. STRUCTURALLY, on both engines and here: anchoring on the wallet message
#      would require ReturnExcessesBack AND ClientProxyRequest among the WALLET's
#      children, and the wallet's only child is the refund call itself. The
#      children set rejects before any build statement runs.
#   2. In legacy's build, additionally: it parses the anchor body as
#      OwnerClientRequestRefund unconditionally, and that class asserts its own
#      opcode, so a wallet body raises and the build returns [].
# Only (1) is reproduced here — (2) is unreachable, and note it would NOT be
# reached by the parse because the required children reject it before build.
# Do not "fix" the dead arm by
# dropping it: keeping it is the faithful transcription, and (1) makes it inert.
#
# EDGE ORDER IS LOAD-BEARING (compiler trap T04 / §6.5): the children block must
# come BEFORE the parent edge. A children block written AFTER a parent edge
# attaches to the PARENT matcher, not the anchor — `op A <- op B -> {C; D}` gives
# B the children. Written in this order, `op A -> {C; D} <- op B` gives A both
# its children set and its parent, which is the legacy structure.
# -----------------------------------------------------------------------------
matcher cocoon_client_request_refund {
    produces cocoon_client_request_refund
    entry    @refund

    ( op CocoonOwnerWalletSendMessage @refund
    ^ op CocoonOwnerClientRequestRefund @refund )
      -> { op CocoonReturnExcessesBack @excesses
         ; op CocoonClientProxyRequest @client_request }
      <- maybe op CocoonOwnerWalletSendMessage @wallet

    parse refund as CocoonOwnerClientRequestRefund
    failed when (refund.msg.transaction.aborted ?? false)
    out {
        client_contract: account(refund.msg.destination) ?? addr_none()
        owner:           account(refund.body.send_excesses_to) ?? addr_none()
        query_id:        refund.body.query_id
        via_wallet:      wallet != null
    }
}

# -----------------------------------------------------------------------------
# cocoon_grant_refund — blocks/cocoon.py:1123 CocoonGrantRefundMatcher.
#
#   ext-in -> GrantRefundPayload (A -> B, B = Proxy)
#               -> ClientProxyRequest (B -> C)
#                    -> { Payout (C -> D) ; excesses (C -> A) }
#
# Depth 3, both grandchildren required. `failed` is the PAYOUT leg's transaction
# (like worker_payout, unlike everything else in the file).
# -----------------------------------------------------------------------------
matcher cocoon_grant_refund {
    produces cocoon_grant_refund
    entry    @grant

    op CocoonGrantRefundPayload @grant
      -> op CocoonClientProxyRequest @client_request
         -> { op CocoonPayout @payout
            ; op CocoonReturnExcessesBack @excesses }

    parse grant as CocoonGrantRefundPayload
    failed when (payout.msg.transaction.aborted ?? false)
    out {
        proxy_contract:   account(grant.msg.destination) ?? addr_none()
        client_contract:  account(client_request.msg.destination) ?? addr_none()
        refund_recipient: account(payout.msg.destination) ?? addr_none()
        query_id:         grant.body.query_id
        new_tokens_used:  grant.body.new_tokens_used
        expected_address: cocoon_expected_address(grant.body.expected_my_address)
        payout_amount:    amount(payout.msg.value)
    }
}

# -----------------------------------------------------------------------------
# cocoon_client_increase_stake — blocks/cocoon.py:933.
# The client-register shape again; `newStake` is a coins field the legacy parser
# reads as `load_coins() or 0`, which the `coins` leaf already reproduces (a zero
# length nibble yields 0 on both sides).
# -----------------------------------------------------------------------------
matcher cocoon_client_increase_stake {
    produces cocoon_client_increase_stake
    entry    @inc

    op CocoonOwnerClientIncreaseStake @inc
      -> { op CocoonReturnExcessesBack @excesses
         ; op CocoonClientProxyRequest @client_request }

    parse inc as CocoonOwnerClientIncreaseStake
    failed when (inc.msg.transaction.aborted ?? false)
    out {
        owner:           account(inc.body.send_excesses_to) ?? addr_none()
        client_contract: account(inc.msg.destination) ?? addr_none()
        query_id:        inc.body.query_id
        new_stake:       amount(inc.body.new_stake)
    }
}

# -----------------------------------------------------------------------------
# cocoon_client_withdraw — blocks/cocoon.py:1013.
#
#   ext-in -> OwnerClientWithdraw (-> Client)
#               -> ClientProxyRequest (Client -> Proxy)
#                    -> { maybe excesses ; Payout }
#
# Children in legacy declaration order, and their optionality differs per item —
# `maybe` on the excesses item only, NOT `maybe { … }` (which would make both
# optional and let the required payout silently vanish).
#
# `withdraw_amount` is the nested-payload host fn (see the header). It returns
# null on a missing/undecodable payload and the `?? 0` reproduces legacy's
# "warn and write 0" fallback. Note the outer ClientProxyRequest parse lives
# INSIDE the host fn rather than as a `parse` statement: a `parse` failure
# REJECTS the whole match (§9.1) while legacy tolerates it and still emits the
# row with amount 0.
# -----------------------------------------------------------------------------
matcher cocoon_client_withdraw {
    produces cocoon_client_withdraw
    entry    @wd

    op CocoonOwnerClientWithdraw @wd
      -> op CocoonClientProxyRequest @client_request
         -> { maybe op CocoonReturnExcessesBack @excesses
            ; op CocoonPayout @payout }

    parse wd as CocoonOwnerClientWithdraw
    failed when (wd.msg.transaction.aborted ?? false)
    out {
        owner:           account(wd.body.send_excesses_to) ?? addr_none()
        client_contract: account(wd.msg.destination) ?? addr_none()
        query_id:        wd.body.query_id
        withdraw_amount: amount(cocoon_withdraw_amount(client_request) ?? 0)
    }
}
