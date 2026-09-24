# Action payload key registry

Codec 1 stores one msgpack map with plain string contract names. The payload
has one outer `data` key whose value is the flat protocol map; the Redis row
keeps these codec-prefixed bytes in its binary `payload` field. Row columns
such as `source`, `destination`, `asset`, `amount` and `opcode` are not payload
keys when they occur at the row level. The 124 rows below are the union of all
per-stored-type key sets; absent keys are omitted.

| Key | Shape | Uses |
|---|---|---:|
| `accounts_added` | array | vesting |
| `amount` | integer_string | nested legs |
| `amount_in` | integer_string | jetton swap |
| `amount_out` | integer_string | jetton swap |
| `approvals_num` | integer_string | multisig execute |
| `asset` | ton_address | nested transfers and records |
| `asset_id` | string | EVAA |
| `asset_in` | ton_address | jetton swap |
| `asset_out` | ton_address | jetton swap |
| `btc_txid` | string | tgBTC |
| `channel` | ton_address | LayerZero |
| `claimed_rewards` | array | JVault |
| `comment` | string | transfers, auction outbid |
| `comment_encrypted` | boolean | transfers |
| `custom_payload` | bytes_base64 | NFT and jetton transfers |
| `debt_amount` | integer_string | EVAA |
| `destination` | ton_address | nested swap transfers |
| `destination_jetton_wallet` | ton_address | nested swap transfers |
| `dex_incoming_transfer` | object | jetton swap |
| `dex_jetton_wallet` | ton_address | DEX legs |
| `dex_outgoing_transfer` | object | jetton swap |
| `dex_wallet` | ton_address | DEX legs |
| `dst_eid` | integer_string | LayerZero |
| `dst_oapp` | string | LayerZero |
| `dvn` | ton_address | LayerZero |
| `endpoint` | ton_address | LayerZero |
| `exit_code` | integer_string | JVault |
| `expected_address` | string | Cocoon |
| `expiration_date` | integer_string | multisig |
| `fail_reason` | string | EVAA |
| `flags` | integer_string | DNS |
| `forward_amount` | integer_string | NFT and jetton transfers |
| `forward_payload` | bytes_base64 | NFT and jetton transfers |
| `full_price` | integer_string | NFT sales |
| `guid` | string | LayerZero |
| `initial_price_x96` | integer_string | Tonco |
| `initiator` | ton_address | Coffee legs |
| `is_created_by_signer` | boolean | multisig |
| `is_purchase` | boolean | NFT |
| `is_refund` | boolean | DEX claims and withdrawals |
| `is_signed_by_creator` | boolean | multisig |
| `is_ton` | boolean | EVAA |
| `key` | string | DNS |
| `legs` | array | DEX, Coffee, Tonco |
| `lp_fee_base` | integer_string | Tonco |
| `lp_fee_current` | integer_string | Tonco |
| `marketplace_address` | ton_address | NFT markets |
| `marketplace_fee` | integer_string | NFT sales |
| `marketplace_fee_address` | ton_address | NFT markets |
| `master` | ton_address | EVAA |
| `master_jetton_wallet` | ton_address | EVAA |
| `max_bid` | integer_string | NFT auctions |
| `message` | string | LayerZero |
| `message_boc` | bytes_base64 | vesting |
| `min_bid` | integer_string | NFT auctions |
| `min_out_amount` | integer_string | jetton swap |
| `mp_fee_base` | integer_string | NFT auctions |
| `mp_fee_factor` | integer_string | NFT auctions |
| `msglib` | string | LayerZero |
| `msglib_connection` | ton_address | LayerZero |
| `msglib_manager` | string | LayerZero |
| `native_fee` | integer_string | LayerZero |
| `new_secret_hash` | string | Cocoon |
| `new_tokens` | integer_string | Cocoon |
| `new_tokens_used` | integer_string | Cocoon |
| `nft_address` | ton_address | NFT, DEX, staking |
| `nft_index` | integer_string | NFT, DEX, Coffee |
| `nonce` | integer_string | LayerZero and Cocoon |
| `order_boc` | bytes_base64 | multisig |
| `order_seqno` | integer_string | multisig |
| `parent_gasless_action` | hash_base64 | all action types when present |
| `payout_amount` | integer_string | NFT |
| `payout_comment` | string | NFT |
| `payout_comment_encoded` | boolean | NFT |
| `payout_comment_encrypted` | boolean | NFT |
| `payout_type` | string | Cocoon |
| `peer_swaps` | array | jetton swap |
| `period` | integer_string | JVault |
| `points` | integer_string | Coffee |
| `pool_active` | boolean | Tonco |
| `pool_first` | ton_address | Coffee |
| `pool_second` | ton_address | Coffee |
| `price` | integer_string | NFT |
| `protocol` | string | DEX and staking |
| `protocol_fee` | integer_string | Tonco |
| `provided_asset` | ton_address | Coffee |
| `proxy` | ton_address | LayerZero |
| `pubkey` | string | tgBTC |
| `query_id` | integer_string | protocol actions, DNS release |
| `real_prev_owner` | ton_address | NFT |
| `recipient_jetton_wallet` | ton_address | EVAA |
| `record_value` | string | DNS |
| `response_destination` | ton_address | NFT and jetton transfers |
| `royalty_address` | ton_address | NFT markets |
| `royalty_amount` | integer_string | NFT markets |
| `royalty_fee_base` | integer_string | NFT auctions |
| `send_request_id` | integer_string | LayerZero |
| `sender` | ton_address | jetton swap |
| `seqno` | integer_string | Cocoon |
| `signer_index` | integer_string | multisig |
| `signers_hash` | hash_base64 | multisig |
| `source` | ton_address | nested swap transfers |
| `source_jetton_wallet` | ton_address | jetton swap |
| `src_eid` | integer_string | LayerZero |
| `src_oapp` | string | LayerZero |
| `stake_wallet` | ton_address | JVault |
| `status` | string | LayerZero |
| `target_amount` | integer_string | DEX legs |
| `target_asset` | ton_address | DEX legs |
| `tick_lower` | integer_string | DEX |
| `tick_spacing` | integer_string | Tonco |
| `tick_upper` | integer_string | DEX |
| `tokens_burnt` | integer_string | DEX and staking |
| `tokens_minted` | integer_string | DEX and staking |
| `timestamp` | integer_string | tgBTC |
| `uln` | ton_address | LayerZero |
| `uln_connection` | ton_address | LayerZero |
| `user_jetton_wallet` | ton_address | DEX legs |
| `value_schema` | string | DNS |
| `vault_excesses` | array | DEX |
| `via_wallet` | boolean | Cocoon |
| `worker_state` | integer_string | Cocoon |
| `worker_tokens` | integer_string | Cocoon |
| `zro_fee` | integer_string | LayerZero |

Nested record members (`asset`, `amount`, wallet fields, and the target fields)
are scoped to their containing `legs`, transfer, peer swap, or excess record.
Null fields are omitted. `parent_gasless_action` is added by the engine when a
row is produced from a gasless request.

`dns_release` permits both `query_id` and `nft_index` in its flat data map.
