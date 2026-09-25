package parse

import (
	"encoding/base64"
	"fmt"
	"math/big"
	"reflect"
	"strconv"
	"strings"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
)

const actionPayloadCodecMsgpack = 1

// actionDataPrefixes lists the RawAction field prefixes the flat `data` map of a
// stored type resolves against, in priority order. A key takes the first prefix
// that owns the field, so a type whose payload mixes families (LayerZero packet
// plus send/commit fields, NFT transfer plus listing fields) reads one map.
var actionDataPrefixes = map[string][]string{
	"auction_bid":                      {"NFTTransfer"},
	"auction_outbid":                   {"TonTransfer", "NFTTransfer"},
	"change_dns":                       {"ChangeDNSRecord"},
	"cocoon_client_change_secret_hash": {"CocoonClientChangeSecretHash"},
	"cocoon_client_increase_stake":     {"CocoonClientIncreaseStake"},
	"cocoon_client_register":           {"CocoonClientRegister"},
	"cocoon_client_request_refund":     {"CocoonClientRequestRefund"},
	"cocoon_client_top_up":             {"CocoonClientTopUp"},
	"cocoon_client_withdraw":           {"CocoonClientWithdraw"},
	"cocoon_grant_refund":              {"CocoonGrantRefund"},
	"cocoon_proxy_charge":              {"CocoonProxyCharge"},
	"cocoon_proxy_payout":              {"CocoonProxyPayout"},
	"cocoon_register_proxy":            {"CocoonRegisterProxy"},
	"cocoon_unregister_proxy":          {"CocoonUnregisterProxy"},
	"cocoon_worker_payout":             {"CocoonWorkerPayout"},
	"coffee_create_pool":               {"CoffeeCreatePool"},
	"coffee_create_pool_creator":       {"CoffeeCreatePool"},
	"coffee_staking_deposit":           {"CoffeeStakingDeposit"},
	"coffee_staking_withdraw":          {"CoffeeStakingWithdraw"},
	"dedust_v2_claim_fees":             {"DexWithdrawLiquidityData"},
	"dedust_v2_claim_reward":           {"DexWithdrawLiquidityData"},
	"delete_dns":                       {"ChangeDNSRecord"},
	"dex_deposit_liquidity":            {"DexDepositLiquidityData"},
	"dex_withdraw_liquidity":           {"DexWithdrawLiquidityData"},
	"dns_purchase":                     {"NFTTransfer"},
	"dns_release":                      {"NFTTransfer"},
	"evaa_liquidate":                   {"EvaaLiquidate"},
	"evaa_supply":                      {"EvaaSupply"},
	"evaa_withdraw":                    {"EvaaWithdraw"},
	"jetton_swap":                      {"JettonSwap"},
	"jetton_transfer":                  {"JettonTransfer"},
	"jvault_claim":                     {"JvaultClaim"},
	"jvault_stake":                     {"JvaultStake"},
	"jvault_unstake":                   {"Jvault"},
	"jvault_unstake_request":           {"Jvault"},
	"layerzero_commit_packet":          {"LayerzeroPacket"},
	"layerzero_dvn_verify":             {"LayerzeroDvnVerify"},
	"layerzero_receive":                {"LayerzeroPacket"},
	"layerzero_send":                   {"LayerzeroSend", "LayerzeroPacket"},
	"layerzero_send_tokens":            {"LayerzeroSend", "LayerzeroPacket"},
	"multisig_approve":                 {"MultisigApprove"},
	"multisig_create_order":            {"MultisigCreateOrder"},
	"multisig_execute":                 {"MultisigExecute"},
	"nft_cancel_auction":               {"NFTTransfer"},
	"nft_cancel_sale":                  {"NFTTransfer"},
	"nft_discovery":                    {"NFTTransfer"},
	"nft_finish_auction":               {"NFTTransfer"},
	"nft_mint":                         {"NFTMint"},
	"nft_purchase":                     {"NFTTransfer"},
	"nft_put_on_auction":               {"NFTListing", "NFTTransfer"},
	"nft_put_on_sale":                  {"NFTListing", "NFTTransfer"},
	"nft_transfer":                     {"NFTTransfer", "NFTListing"},
	"nft_update_sale":                  {"NFTListing", "NFTTransfer"},
	"stake_deposit":                    {"StakingData"},
	"stake_withdrawal":                 {"StakingData"},
	"stake_withdrawal_request":         {"StakingData"},
	"teleitem_cancel_auction":          {"NFTTransfer"},
	"teleitem_start_auction":           {"NFTListing", "NFTTransfer"},
	"ton_transfer":                     {"TonTransfer"},
	"tonco_deploy_pool":                {"ToncoDeployPool"},
	"vesting_add_whitelist":            {"VestingAddWhitelist"},
	"vesting_send_message":             {"VestingSendMessage"},
}

// actionPayloadExtraKeys are engine-owned payload keys. They carry engine metadata
// rather than a typed detail field, so they land in RawAction.Extra where the API reads
// them through extraString.
var actionPayloadExtraKeys = map[string]bool{
	"parent_gasless_action": true,
	"btc_txid":              true,
	"pubkey":                true,
	"timestamp":             true,
}

// actionPayloadLegFieldNames expands one `legs[i]` record member into the indexed
// RawAction field of the stored type's prefix (%d is the 1-based leg index).
var actionPayloadLegFieldNames = map[string]map[string]string{
	"CoffeeCreatePool": {
		"amount": "Amount%d", "initiator": "Initiator%d",
	},
	"DexDepositLiquidityData": {
		"amount": "Amount%d", "asset": "Asset%d", "user_jetton_wallet": "UserJettonWallet%d",
		"target_asset": "TargetAsset%d", "target_amount": "TargetAmount%d",
	},
	"DexWithdrawLiquidityData": {
		"amount": "Amount%d", "asset": "Asset%dOut", "user_jetton_wallet": "UserJettonWallet%d",
		"dex_jetton_wallet": "DexJettonWallet%d", "dex_wallet": "DexWallet%d",
	},
	"ToncoDeployPool": {
		"asset": "Jetton%dMinter", "dex_jetton_wallet": "Jetton%dRouterWallet",
	},
}

// actionPayloadLegIndexBase maps a prefix whose legs are numbered from 0 (Tonco's
// jetton0/jetton1) instead of the payload's 1-based suffix order.
var actionPayloadLegIndexBase = map[string]int{
	"ToncoDeployPool": 0,
}

var actionPayloadFieldNames = map[string]string{
	"ChangeDNSRecord.record_value":   "Value",
	"CoffeeCreatePool.tokens_minted": "LpTokensMinted",
	// These two write the envelope fields the API reads; the values come from the
	// payload, not from the row.
	"CoffeeCreatePool.pool_first":           ".Asset2",
	"CoffeeCreatePool.pool_second":          ".Asset2Secondary",
	"Jvault.exit_code":                      ".Opcode",
	"LayerzeroPacket.channel":               ".Asset",
	"LayerzeroPacket.msglib_connection":     ".AssetSecondary",
	"JvaultClaim.claimed_rewards":           "ClaimedJettons",
	"CoffeeStakingDeposit.nft_address":      "MintedItemAddress",
	"CoffeeStakingDeposit.nft_index":        "MintedItemIndex",
	"CoffeeStakingWithdraw.nft_address":     "NftAddress",
	"CoffeeStakingWithdraw.nft_index":       "NftIndex",
	"DexDepositLiquidityData.protocol":      "Dex",
	"DexDepositLiquidityData.tokens_minted": "LpTokensMinted",
	"DexWithdrawLiquidityData.nft_address":  "BurnedNFTAddress",
	"DexWithdrawLiquidityData.nft_index":    "BurnedNFTIndex",
	"DexWithdrawLiquidityData.protocol":     "Dex",
	"DexWithdrawLiquidityData.tokens_burnt": "LpTokensBurnt",
	"JettonSwap.protocol":                   "Dex",
	"JettonTransfer.comment_encrypted":      "IsEncryptedComment",
	"MultisigCreateOrder.signer_index":      "CreatorIndex",
	"NFTListing.mp_fee_base":                "MarketplaceFeeBase",
	"NFTListing.mp_fee_factor":              "MarketplaceFeeFactor",
	"NFTListing.nft_index":                  "NFTItemIndex",
	"NFTMint.nft_index":                     "NFTItemIndex",
	"NFTTransfer.nft_index":                 "NFTItemIndex",
	"StakingData.nft_address":               "TsNft",
	"StakingData.protocol":                  "Provider",
	"TonTransfer.comment":                   "Content",
	"TonTransfer.comment_encrypted":         "Encrypted",
}

// DecodeActionPayload applies the compact payload's details to an action whose Type is set.
// Every error carries ErrPayloadDecode so a caller can skip one undecodable action.
func DecodeActionPayload(payload []byte, action *models.RawAction) error {
	if len(payload) == 0 {
		return fmt.Errorf("%w: empty action payload", ErrPayloadDecode)
	}
	if payload[0] != actionPayloadCodecMsgpack {
		return fmt.Errorf("%w: unsupported payload codec %d", ErrPayloadDecode, payload[0])
	}
	values, err := decodeCompactPayload(payload[1:])
	if err != nil {
		return fmt.Errorf("%w: %v", ErrPayloadDecode, err)
	}
	if err := applyActionPayload(action, values); err != nil {
		return fmt.Errorf("%w: %v", ErrPayloadDecode, err)
	}
	return nil
}

// applyActionPayload routes a decoded payload map (names -> text values, one entry per
// key of the payload) into the action's typed fields.
func applyActionPayload(action *models.RawAction, values map[string]interface{}) error {
	target := reflect.ValueOf(action).Elem()
	return fillActionPayload(target, action, actionDataPrefixes[action.Type], "", values)
}

func fillActionPayload(target reflect.Value, action *models.RawAction, prefixes []string, parent string, values map[string]interface{}) error {
	for key, value := range values {
		path := key
		if parent != "" {
			path = parent + "." + key
		}
		// A null value is an absent key: the payload drops null placeholders and the
		// destination field stays at its zero value either way.
		if value == nil {
			continue
		}
		if nested, ok := value.(map[string]interface{}); ok {
			if err := fillActionPayload(target, action, prefixes, path, nested); err != nil {
				return err
			}
			continue
		}
		if parent == "" {
			if actionPayloadExtraKeys[key] {
				if action.Extra == nil {
					action.Extra = map[string]interface{}{}
				}
				action.Extra[key] = value
				continue
			}
			if key == "legs" {
				if err := fillActionPayloadLegs(target, prefixes, value); err != nil {
					return err
				}
				continue
			}
		}
		field := resolvePayloadField(target, prefixes, path)
		if !field.IsValid() {
			continue
		}
		if err := setPayloadValue(field, value); err != nil {
			return fmt.Errorf("field %s: %w", path, err)
		}
	}
	return nil
}

func resolvePayloadField(target reflect.Value, prefixes []string, path string) reflect.Value {
	for _, prefix := range prefixes {
		name := actionPayloadFieldName(prefix, path)
		// A leading dot names a RawAction field directly: the moved row columns decode
		// into the envelope fields the frozen API already reads.
		if strings.HasPrefix(name, ".") {
			name = name[1:]
		} else {
			name = prefix + name
		}
		if candidate := target.FieldByName(name); candidate.IsValid() {
			return candidate
		}
	}
	return reflect.Value{}
}

// fillActionPayloadLegs expands the legs list into the type's indexed fields: leg 0 fills
// suffix 1, leg 1 fills suffix 2.
func fillActionPayloadLegs(target reflect.Value, prefixes []string, value interface{}) error {
	items, ok := value.([]interface{})
	if !ok {
		return fmt.Errorf("field legs: expected array, got %T", value)
	}
	for index, item := range items {
		// An empty leg is a placeholder: it keeps the slot its index gives it, so the
		// legs after it stay in their own slots.
		if item == nil {
			continue
		}
		record, ok := item.(map[string]interface{})
		if !ok {
			return fmt.Errorf("field legs: expected map, got %T", item)
		}
		for member, memberValue := range record {
			if memberValue == nil {
				continue
			}
			field := resolvePayloadLegField(target, prefixes, index, member)
			if !field.IsValid() {
				continue
			}
			if err := setPayloadValue(field, memberValue); err != nil {
				return fmt.Errorf("field legs[%d].%s: %w", index, member, err)
			}
		}
	}
	return nil
}

// resolvePayloadLegField is the one resolution of a `legs[index].member` entry: the
// member's indexed RawAction field for the first prefix that owns it.
func resolvePayloadLegField(target reflect.Value, prefixes []string, index int, member string) reflect.Value {
	for _, prefix := range prefixes {
		template := actionPayloadLegFieldNames[prefix][member]
		if template == "" {
			continue
		}
		index1 := index + 1
		if base, ok := actionPayloadLegIndexBase[prefix]; ok {
			index1 = index + base
		}
		if candidate := target.FieldByName(prefix + fmt.Sprintf(template, index1)); candidate.IsValid() {
			return candidate
		}
	}
	return reflect.Value{}
}

func actionPayloadFieldName(prefix, path string) string {
	if name := actionPayloadFieldNames[prefix+"."+path]; name != "" {
		return name
	}
	parts := strings.FieldsFunc(path, func(r rune) bool { return r == '_' || r == '.' })
	for i, part := range parts {
		if part == "nft" {
			parts[i] = "NFT"
		} else if part != "" {
			parts[i] = strings.ToUpper(part[:1]) + part[1:]
		}
	}
	return strings.Join(parts, "")
}

func setPayloadValue(target reflect.Value, value interface{}) error {
	if value == nil {
		target.SetZero()
		return nil
	}
	if target.Kind() == reflect.Ptr {
		v := reflect.New(target.Type().Elem())
		if err := setPayloadValue(v.Elem(), value); err != nil {
			return err
		}
		target.Set(v)
		return nil
	}
	if target.Type() == reflect.TypeOf(big.Int{}) {
		value, ok := value.(string)
		if !ok {
			return fmt.Errorf("expected integer string, got %T", value)
		}
		n, ok := new(big.Int).SetString(value, 10)
		if !ok {
			return fmt.Errorf("invalid base-10 integer")
		}
		target.Set(reflect.ValueOf(*n))
		return nil
	}

	switch target.Kind() {
	case reflect.String:
		switch value := value.(type) {
		case string:
			target.SetString(value)
		case []byte:
			target.SetString(base64.StdEncoding.EncodeToString(value))
		default:
			return fmt.Errorf("expected string, got %T", value)
		}
	case reflect.Bool:
		value, ok := value.(bool)
		if !ok {
			return fmt.Errorf("expected bool, got %T", value)
		}
		target.SetBool(value)
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		value, ok := value.(string)
		if !ok {
			return fmt.Errorf("expected integer string, got %T", value)
		}
		n, err := strconv.ParseInt(value, 10, target.Type().Bits())
		if err != nil {
			return err
		}
		target.SetInt(n)
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		value, ok := value.(string)
		if !ok {
			return fmt.Errorf("expected integer string, got %T", value)
		}
		n, err := strconv.ParseUint(value, 10, target.Type().Bits())
		if err != nil {
			return err
		}
		target.SetUint(n)
	case reflect.Slice:
		values, ok := value.([]interface{})
		if !ok {
			return fmt.Errorf("expected array, got %T", value)
		}
		result := reflect.MakeSlice(target.Type(), len(values), len(values))
		for i := range values {
			if err := setPayloadValue(result.Index(i), values[i]); err != nil {
				return err
			}
		}
		target.Set(result)
	case reflect.Struct:
		values, ok := value.(map[string]interface{})
		if !ok {
			return fmt.Errorf("expected map, got %T", value)
		}
		for key, value := range values {
			field := target.FieldByName(actionPayloadFieldName("", key))
			if field.IsValid() {
				if err := setPayloadValue(field, value); err != nil {
					return err
				}
			}
		}
	default:
		return fmt.Errorf("unsupported destination %s", target.Type())
	}
	return nil
}
