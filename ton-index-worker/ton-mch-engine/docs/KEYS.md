# Action payload key registry

Codec 1 is byte `01` followed by one MessagePack map. Keys are registry ids,
sorted ascending at every map level. Ids 1..127 use positive fixint; ids
128..255 use uint8. The empty payload is `01 80`. Nested records use the
same ids. Arrays preserve order and positional nil entries in `legs`.
Null fields are omitted. Names in action goldens are unchanged.

Reuse an existing key first; a new key needs written justification, then
a hand edit to `payload-keys.yaml` with the next id.

`address` is 33 binary bytes (signed workchain byte plus 32 hash bytes);
`boc` and the three hash forms are binary. `uint` is a MessagePack unsigned
integer up to uint64, then minimal big-endian binary. `string` and `bool`
keep their MessagePack forms. `other` covers containers and pass-through
scalars; keys inside maps still use ids and their declared types.

For classified actions, `tx_hashes` starts with the initiating transaction,
or the root-level block's earliest transaction by `tx->lt`, followed by the
remaining hashes sorted ascending.

| id | Key | Type | Shape | Uses |
|---:|---|---|---|---|
| 1 | `accounts_added` | other | array | vesting |
| 2 | `amount` | uint | integer_string | nested legs |
| 3 | `amount_in` | uint | integer_string | jetton swap |
| 4 | `amount_out` | uint | integer_string | jetton swap |
| 5 | `approvals_num` | uint | integer_string | multisig execute |
| 6 | `asset` | address | ton_address | nested transfers and records |
| 7 | `asset_id` | hash_0x | string | EVAA |
| 8 | `asset_in` | address | ton_address | jetton swap |
| 9 | `asset_out` | address | ton_address | jetton swap |
| 10 | `btc_txid` | hash_hex | string | tgBTC |
| 11 | `channel` | address | ton_address | LayerZero |
| 12 | `claimed_rewards` | other | array | JVault |
| 13 | `comment` | string | string | transfers, auction outbid |
| 14 | `comment_encrypted` | bool | boolean | transfers |
| 15 | `custom_payload` | boc | bytes_base64 | NFT and jetton transfers |
| 16 | `debt_amount` | uint | integer_string | EVAA |
| 17 | `destination` | address | ton_address | nested swap transfers |
| 18 | `destination_jetton_wallet` | address | ton_address | nested swap transfers |
| 19 | `dex_incoming_transfer` | other | object | jetton swap |
| 20 | `dex_jetton_wallet` | address | ton_address | DEX legs |
| 21 | `dex_outgoing_transfer` | other | object | jetton swap |
| 22 | `dex_wallet` | address | ton_address | DEX legs |
| 23 | `dst_eid` | uint | integer_string | LayerZero |
| 24 | `dst_oapp` | string | string | LayerZero |
| 25 | `dvn` | address | ton_address | LayerZero |
| 26 | `endpoint` | address | ton_address | LayerZero |
| 27 | `exit_code` | string | integer_string | JVault |
| 28 | `expected_address` | string | string | Cocoon |
| 29 | `expiration_date` | string | integer_string | multisig |
| 30 | `fail_reason` | string | string | EVAA |
| 31 | `flags` | uint | integer_string | DNS |
| 32 | `forward_amount` | uint | integer_string | NFT and jetton transfers |
| 33 | `forward_payload` | boc | bytes_base64 | NFT and jetton transfers |
| 34 | `full_price` | string | integer_string | NFT sales |
| 35 | `guid` | string | string | LayerZero |
| 36 | `initial_price_x96` | uint | integer_string | Tonco |
| 37 | `initiator` | address | ton_address | Coffee legs |
| 38 | `is_created_by_signer` | bool | boolean | multisig |
| 39 | `is_purchase` | bool | boolean | NFT transfer and purchase actions |
| 40 | `is_refund` | bool | boolean | DEX claims and withdrawals |
| 41 | `is_signed_by_creator` | bool | boolean | multisig |
| 42 | `is_ton` | bool | boolean | EVAA |
| 43 | `key` | hash_hex | string | DNS |
| 44 | `legs` | other | array | DEX, Coffee, Tonco |
| 45 | `lp_fee_base` | uint | integer_string | Tonco |
| 46 | `lp_fee_current` | uint | integer_string | Tonco |
| 47 | `marketplace_address` | address | ton_address | NFT markets |
| 48 | `marketplace_fee` | string | integer_string | NFT sales |
| 49 | `marketplace_fee_address` | address | ton_address | NFT markets |
| 50 | `master` | address | ton_address | EVAA |
| 51 | `master_jetton_wallet` | address | ton_address | EVAA |
| 52 | `max_bid` | string | integer_string | NFT auctions |
| 53 | `message` | string | string | LayerZero |
| 54 | `message_boc` | boc | bytes_base64 | vesting |
| 55 | `min_bid` | string | integer_string | NFT auctions |
| 56 | `min_out_amount` | uint | integer_string | jetton swap |
| 57 | `mp_fee_base` | string | integer_string | NFT auctions |
| 58 | `mp_fee_factor` | string | integer_string | NFT auctions |
| 59 | `msglib` | string | string | LayerZero |
| 60 | `msglib_connection` | address | ton_address | LayerZero |
| 61 | `msglib_manager` | string | string | LayerZero |
| 62 | `native_fee` | uint | integer_string | LayerZero |
| 63 | `new_secret_hash` | hash_hex | string | Cocoon |
| 64 | `new_tokens` | uint | integer_string | Cocoon |
| 65 | `new_tokens_used` | uint | integer_string | Cocoon |
| 66 | `nft_address` | address | ton_address | NFT, DEX, staking |
| 67 | `nft_index` | string | integer_string | NFT, DEX, Coffee |
| 68 | `nonce` | uint | integer_string | LayerZero and Cocoon |
| 69 | `order_boc` | boc | bytes_base64 | multisig |
| 70 | `order_seqno` | uint | integer_string | multisig |
| 71 | `parent_gasless_action` | hash | hash_base64 | all action types when present |
| 72 | `payout_amount` | uint | integer_string | NFT |
| 73 | `payout_comment` | string | string | NFT |
| 74 | `payout_comment_encoded` | bool | boolean | NFT |
| 75 | `payout_comment_encrypted` | bool | boolean | NFT |
| 76 | `payout_type` | string | string | Cocoon |
| 77 | `peer_swaps` | other | array | jetton swap |
| 78 | `period` | uint | integer_string | JVault |
| 79 | `points` | uint | integer_string | Coffee |
| 80 | `pool_active` | bool | boolean | Tonco |
| 81 | `pool_first` | address | ton_address | Coffee |
| 82 | `pool_second` | address | ton_address | Coffee |
| 83 | `price` | string | integer_string | NFT |
| 84 | `protocol` | string | string | DEX and staking |
| 85 | `protocol_fee` | uint | integer_string | Tonco |
| 86 | `provided_asset` | address | ton_address | Coffee |
| 87 | `proxy` | address | ton_address | LayerZero |
| 88 | `pubkey` | hash_hex | string | tgBTC |
| 89 | `query_id` | uint | integer_string | protocol actions, DNS release |
| 90 | `real_prev_owner` | address | ton_address | NFT |
| 91 | `recipient_jetton_wallet` | address | ton_address | EVAA |
| 92 | `record_value` | string | string | DNS |
| 93 | `response_destination` | address | ton_address | NFT and jetton transfers |
| 94 | `royalty_address` | address | ton_address | NFT markets |
| 95 | `royalty_amount` | string | integer_string | NFT markets |
| 96 | `royalty_fee_base` | string | integer_string | NFT auctions |
| 97 | `send_request_id` | uint | integer_string | LayerZero |
| 98 | `sender` | address | ton_address | jetton swap |
| 99 | `seqno` | uint | integer_string | Cocoon |
| 100 | `signer_index` | string | integer_string | multisig |
| 101 | `signers_hash` | hash | hash_base64 | multisig |
| 102 | `source` | address | ton_address | nested swap transfers |
| 103 | `source_jetton_wallet` | address | ton_address | jetton swap |
| 104 | `src_eid` | uint | integer_string | LayerZero |
| 105 | `src_oapp` | string | string | LayerZero |
| 106 | `stake_wallet` | address | ton_address | JVault |
| 107 | `status` | string | string | LayerZero |
| 108 | `target_amount` | uint | integer_string | DEX legs |
| 109 | `target_asset` | address | ton_address | DEX legs |
| 110 | `tick_lower` | string | integer_string | DEX |
| 111 | `tick_spacing` | string | integer_string | Tonco |
| 112 | `tick_upper` | string | integer_string | DEX |
| 113 | `timestamp` | string | integer_string | tgBTC fallback actions |
| 114 | `tokens_burnt` | uint | integer_string | DEX and staking |
| 115 | `tokens_minted` | uint | integer_string | DEX and staking |
| 116 | `uln` | address | ton_address | LayerZero |
| 117 | `uln_connection` | address | ton_address | LayerZero |
| 118 | `user_jetton_wallet` | address | ton_address | DEX legs |
| 119 | `value_schema` | string | string | DNS |
| 120 | `vault_excesses` | other | array | DEX |
| 121 | `via_wallet` | bool | boolean | Cocoon |
| 122 | `worker_state` | string | integer_string | Cocoon |
| 123 | `worker_tokens` | uint | integer_string | Cocoon |
| 124 | `zro_fee` | uint | integer_string | LayerZero |

Encoded payloads omit null fields and list elements recursively, except `legs`
retains null placeholders before filled legs, trims trailing placeholders, and
omits the key when all legs are empty. `parent_gasless_action` is added by
the engine when a row is produced from a gasless request.

`dns_release` permits both `query_id` and `nft_index` in its flat data map.
