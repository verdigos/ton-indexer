package parse

import (
	"bytes"
	"encoding/base64"
	"fmt"
	"io"
	"math/big"
	"reflect"
	"strconv"
	"strings"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
	"github.com/vmihailenco/msgpack/v5"
)

const actionPayloadCodecMsgpack = 1

var actionDataPrefixes = map[string]string{
	"auction_bid":                      "NFTTransfer",
	"auction_outbid":                   "NFTTransfer",
	"change_dns":                       "ChangeDNSRecord",
	"cocoon_client_change_secret_hash": "CocoonClientChangeSecretHash",
	"cocoon_client_increase_stake":     "CocoonClientIncreaseStake",
	"cocoon_client_register":           "CocoonClientRegister",
	"cocoon_client_request_refund":     "CocoonClientRequestRefund",
	"cocoon_client_top_up":             "CocoonClientTopUp",
	"cocoon_client_withdraw":           "CocoonClientWithdraw",
	"cocoon_grant_refund":              "CocoonGrantRefund",
	"cocoon_proxy_charge":              "CocoonProxyCharge",
	"cocoon_proxy_payout":              "CocoonProxyPayout",
	"cocoon_register_proxy":            "CocoonRegisterProxy",
	"cocoon_unregister_proxy":          "CocoonUnregisterProxy",
	"cocoon_worker_payout":             "CocoonWorkerPayout",
	"coffee_create_pool":               "CoffeeCreatePool",
	"coffee_staking_deposit":           "CoffeeStakingDeposit",
	"coffee_staking_withdraw":          "CoffeeStakingWithdraw",
	"dedust_v2_claim_fees":             "DexWithdrawLiquidityData",
	"dedust_v2_claim_reward":           "DexWithdrawLiquidityData",
	"delete_dns":                       "ChangeDNSRecord",
	"dex_deposit_liquidity":            "DexDepositLiquidityData",
	"dex_withdraw_liquidity":           "DexWithdrawLiquidityData",
	"dns_purchase":                     "NFTTransfer",
	"dns_release":                      "NFTTransfer",
	"evaa_liquidate":                   "EvaaLiquidate",
	"evaa_supply":                      "EvaaSupply",
	"evaa_withdraw":                    "EvaaWithdraw",
	"jetton_swap":                      "JettonSwap",
	"jetton_transfer":                  "JettonTransfer",
	"jvault_claim":                     "JvaultClaim",
	"jvault_stake":                     "JvaultStake",
	"layerzero_dvn_verify":             "LayerzeroDvnVerify",
	"layerzero_send":                   "LayerzeroSend",
	"layerzero_send_tokens":            "LayerzeroSend",
	"multisig_approve":                 "MultisigApprove",
	"multisig_create_order":            "MultisigCreateOrder",
	"multisig_execute":                 "MultisigExecute",
	"nft_cancel_auction":               "NFTTransfer",
	"nft_cancel_sale":                  "NFTTransfer",
	"nft_discovery":                    "NFTTransfer",
	"nft_finish_auction":               "NFTTransfer",
	"nft_mint":                         "NFTMint",
	"nft_purchase":                     "NFTTransfer",
	"nft_put_on_auction":               "NFTTransfer",
	"nft_put_on_sale":                  "NFTTransfer",
	"nft_transfer":                     "NFTTransfer",
	"nft_update_sale":                  "NFTTransfer",
	"stake_deposit":                    "StakingData",
	"stake_withdrawal":                 "StakingData",
	"stake_withdrawal_request":         "StakingData",
	"teleitem_cancel_auction":          "NFTTransfer",
	"teleitem_start_auction":           "NFTTransfer",
	"ton_transfer":                     "TonTransfer",
	"tonco_deploy_pool":                "ToncoDeployPool",
	"vesting_add_whitelist":            "VestingAddWhitelist",
	"vesting_send_message":             "VestingSendMessage",
}

var actionPayloadFieldNames = map[string]string{
	"ChangeDNSRecord.record_value":         "Value",
	"CoffeeStakingDeposit.nft_address":     "MintedItemAddress",
	"CoffeeStakingDeposit.nft_index":       "MintedItemIndex",
	"CoffeeStakingWithdraw.nft_address":    "NftAddress",
	"CoffeeStakingWithdraw.nft_index":      "NftIndex",
	"DexWithdrawLiquidityData.asset1":      "Asset1Out",
	"DexWithdrawLiquidityData.asset2":      "Asset2Out",
	"DexWithdrawLiquidityData.nft_address": "BurnedNFTAddress",
	"DexWithdrawLiquidityData.nft_index":   "BurnedNFTIndex",
	"JettonTransfer.comment_encrypted":     "IsEncryptedComment",
	"MultisigCreateOrder.signer_index":     "CreatorIndex",
	"NFTListing.mp_fee_base":               "MarketplaceFeeBase",
	"NFTListing.mp_fee_factor":             "MarketplaceFeeFactor",
	"NFTListing.nft_index":                 "NFTItemIndex",
	"NFTMint.nft_index":                    "NFTItemIndex",
	"NFTTransfer.nft_index":                "NFTItemIndex",
	"StakingData.ts_nft":                   "TsNft",
	"TonTransfer.comment":                  "Content",
	"TonTransfer.comment_encrypted":        "Encrypted",
	"ToncoDeployPool.asset1":               "Jetton0Minter",
	"ToncoDeployPool.asset2":               "Jetton1Minter",
	"ToncoDeployPool.dex_jetton_wallet_1":  "Jetton0RouterWallet",
	"ToncoDeployPool.dex_jetton_wallet_2":  "Jetton1RouterWallet",
}

// DecodeActionPayload applies payload details to an action whose Type is set.
func DecodeActionPayload(payload []byte, action *models.RawAction) error {
	if len(payload) == 0 {
		return fmt.Errorf("empty action payload")
	}
	if payload[0] != actionPayloadCodecMsgpack {
		return fmt.Errorf("unsupported payload codec %d", payload[0])
	}

	dec := msgpack.NewDecoder(bytes.NewReader(payload[1:]))
	groups, err := dec.DecodeMap()
	if err != nil {
		return fmt.Errorf("decode action payload: %w", err)
	}
	if groups == nil {
		return fmt.Errorf("decode action payload: expected map")
	}
	if _, err := dec.DecodeInterface(); err != io.EOF {
		return fmt.Errorf("decode action payload: trailing data")
	}

	target := reflect.ValueOf(action).Elem()
	_, groupedData := groups["data"]
	_, groupedExtra := groups["extra"]
	if !groupedData && !groupedExtra && (action.Type == "multisig_create_order" ||
		action.Type == "nft_transfer" || action.Type == "tgbtc_mint" ||
		action.Type == "tgbtc_mint_fallback") {
		return fillDirectActionPayload(target, action, groups)
	}
	for group, value := range groups {
		if group == "extra" {
			m, ok := value.(map[string]interface{})
			if ok {
				action.Extra = normalizePayloadMap(m)
			}
			continue
		}
		prefix := actionPayloadPrefix(action.Type, group)
		m, ok := value.(map[string]interface{})
		if prefix == "" || !ok {
			continue
		}
		if err := fillActionPayload(target, prefix, "", m); err != nil {
			return err
		}
	}
	return nil
}

func fillDirectActionPayload(target reflect.Value, action *models.RawAction, values map[string]interface{}) error {
	for key, value := range values {
		if key == "parent_gasless_action" || key == "btc_txid" {
			if action.Extra == nil {
				action.Extra = make(map[string]interface{})
			}
			action.Extra[key] = value
			continue
		}
		prefix := actionDataPrefixes[action.Type]
		if action.Type == "nft_transfer" && (key == "marketplace_fee_address" || key == "royalty_address") {
			prefix = "NFTListing"
		}
		if prefix == "" {
			continue
		}
		if err := fillActionPayload(target, prefix, "", map[string]interface{}{key: value}); err != nil {
			return err
		}
	}
	return nil
}

func actionPayloadPrefix(actionType, group string) string {
	switch group {
	case "data":
		return actionDataPrefixes[actionType]
	case "listing":
		return "NFTListing"
	case "packet":
		return "LayerzeroPacket"
	case "comment_data":
		return "TonTransfer"
	default:
		return ""
	}
}

func fillActionPayload(target reflect.Value, prefix, parent string, values map[string]interface{}) error {
	for key, value := range values {
		path := key
		if parent != "" {
			path = parent + "." + key
		}
		if nested, ok := value.(map[string]interface{}); ok {
			if err := fillActionPayload(target, prefix, path, nested); err != nil {
				return err
			}
			continue
		}
		name := prefix + actionPayloadFieldName(prefix, path)
		field := target.FieldByName(name)
		if !field.IsValid() {
			continue
		}
		if err := setPayloadValue(field, value); err != nil {
			return fmt.Errorf("decode action payload field %s: %w", path, err)
		}
	}
	return nil
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

func normalizePayloadMap(values map[string]interface{}) map[string]interface{} {
	result := make(map[string]interface{}, len(values))
	for key, value := range values {
		switch value := value.(type) {
		case []byte:
			result[key] = base64.StdEncoding.EncodeToString(value)
		case map[string]interface{}:
			result[key] = normalizePayloadMap(value)
		default:
			result[key] = value
		}
	}
	return result
}
