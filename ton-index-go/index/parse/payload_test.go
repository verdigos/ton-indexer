package parse_test

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"reflect"
	"strings"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/emulated"
	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"
	"github.com/vmihailenco/msgpack/v5"
)

// assertWirePayload checks the emulator row carries exactly the row's payload bytes.
func assertWirePayload(t *testing.T, wireHex, payloadHex string) {
	t.Helper()
	wire, err := hex.DecodeString(wireHex)
	if err != nil {
		t.Fatalf("decode wire hex: %v", err)
	}
	var actions []emulatedAction
	if err := msgpack.Unmarshal(wire, &actions); err != nil {
		t.Fatalf("decode emulator wire: %v", err)
	}
	if len(actions) != 1 {
		t.Fatalf("wire contains %d actions", len(actions))
	}
	payload, err := hex.DecodeString(payloadHex)
	if err != nil {
		t.Fatalf("decode payload hex: %v", err)
	}
	if !bytes.Equal(actions[0].Payload, payload) {
		t.Errorf("wire payload = %x, row payload = %x", actions[0].Payload, payload)
	}
}

type emulatedAction struct {
	Payload []byte `msgpack:"payload"`
}

// TestJvaultClaimRewardMissingMember pins the frozen claim pair shape: a record whose
// member was null in the grouped payload reports that half as an empty string, never null.
func TestJvaultClaimRewardMissingMember(t *testing.T) {
	const address = "0:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
	for _, tc := range []struct {
		name   string
		record map[string]interface{}
		jetton string
		amount string
	}{
		{"both members", map[string]interface{}{"asset": address, "amount": "5"}, address, "5"},
		{"amount missing", map[string]interface{}{"asset": address}, address, ""},
		{"asset missing", map[string]interface{}{"amount": "5"}, "", "5"},
		{"both missing", map[string]interface{}{}, "", ""},
	} {
		t.Run(tc.name, func(t *testing.T) {
			raw := &models.RawAction{Type: "jvault_claim"}
			payload, err := parse.EncodeCompactForTest(map[string]interface{}{
				"claimed_rewards": []interface{}{tc.record},
			})
			if err != nil {
				t.Fatal(err)
			}
			if err := parse.DecodeActionPayload(payload, raw); err != nil {
				t.Fatalf("DecodeActionPayload: %v", err)
			}
			action, err := parse.ParseRawAction(raw)
			if err != nil {
				t.Fatalf("ParseRawAction: %v", err)
			}
			details, ok := action.Details.(models.ActionDetailsJvaultClaim)
			if !ok {
				t.Fatalf("details = %T", action.Details)
			}
			if len(details.ClaimedRewards) != 1 {
				t.Fatalf("claimed rewards = %+v", details.ClaimedRewards)
			}
			pair := details.ClaimedRewards[0]
			if pair.Jetton == nil {
				t.Error("jetton is nil, want an empty string")
			} else if string(*pair.Jetton) != tc.jetton {
				t.Errorf("jetton = %q, want %q", *pair.Jetton, tc.jetton)
			}
			if pair.Amount == nil {
				t.Error("amount is nil, want an empty string")
			} else if *pair.Amount != tc.amount {
				t.Errorf("amount = %q, want %q", *pair.Amount, tc.amount)
			}
			encoded, err := json.Marshal(action)
			if err != nil {
				t.Fatal(err)
			}
			if bytes.Contains(encoded, []byte(`"jetton":null`)) || bytes.Contains(encoded, []byte(`"amount":null`)) {
				t.Errorf("API JSON has a null claim half: %s", encoded)
			}
		})
	}
}

func decodeWireAction(t *testing.T, encoded string) *models.RawAction {
	t.Helper()
	wire, err := hex.DecodeString(encoded)
	if err != nil {
		t.Fatalf("decode wire hex: %v", err)
	}
	var actions []emulated.Action
	if err := msgpack.Unmarshal(wire, &actions); err != nil {
		t.Fatalf("decode emulator wire: %v", err)
	}
	if len(actions) != 1 {
		t.Fatalf("wire contains %d actions", len(actions))
	}
	raw, err := actions[0].ToRawAction()
	if err != nil {
		t.Fatalf("ToRawAction: %v", err)
	}
	return raw
}

func clearPayloadFields(action *models.RawAction) {
	envelope := map[string]bool{
		"TraceId": true, "ActionId": true, "StartLt": true, "EndLt": true,
		"StartUtime": true, "EndUtime": true, "TraceEndLt": true, "TraceEndUtime": true,
		"TraceMcSeqnoEnd": true, "Source": true, "SourceSecondary": true,
		"Destination": true, "DestinationSecondary": true, "Asset": true,
		"AssetSecondary": true, "Asset2": true, "Asset2Secondary": true,
		"Opcode": true, "TxHashes": true, "Type": true, "Value": true, "Amount": true,
		"Success": true, "Finality": true, "TraceExternalHash": true,
		"TraceExternalHashNorm": true, "ExtraCurrencies": true, "AncestorType": true,
		"Accounts": true,
	}
	value := reflect.ValueOf(action).Elem()
	typeOf := value.Type()
	for i := range value.NumField() {
		if !envelope[typeOf.Field(i).Name] {
			value.Field(i).SetZero()
		}
	}
}

// TestDecodeActionPayloadEnvelope pins the payload envelope: codec byte 1 followed by ONE
// id-keyed map. The empty payload is `01 80`, an id this table does not know is skipped.
// Body errors are pinned by TestDecodeCompactErrors; here only the codec layer is checked.
func TestDecodeActionPayloadEnvelope(t *testing.T) {
	for _, tc := range []struct {
		name    string
		payload string
		wantErr string
	}{
		{name: "empty payload", payload: "0180"},
		{name: "unknown key id is skipped", payload: "0181ccc8c0"},
		{name: "wrong codec", payload: "0280", wantErr: "unsupported payload codec 2"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			payload, err := hex.DecodeString(tc.payload)
			if err != nil {
				t.Fatalf("decode payload hex: %v", err)
			}
			raw := &models.RawAction{Type: "call_contract"}
			err = parse.DecodeActionPayload(payload, raw)
			if tc.wantErr == "" {
				if err != nil {
					t.Fatalf("DecodeActionPayload: %v", err)
				}
				return
			}
			if err == nil {
				t.Fatalf("payload accepted, want error containing %q", tc.wantErr)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("error = %q, want it to contain %q", err.Error(), tc.wantErr)
			}
		})
	}
}

// TestDecodeActionPayloadLegsPosition pins the empty-leg placeholder: a nil element of
// `legs` is skipped but keeps its slot, so the leg after it stays in its own slot. Only
// a nil is tolerated - any other non-map element is still a decode error.
func TestDecodeActionPayloadLegsPosition(t *testing.T) {
	const (
		asset          = "0:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
		targetAsset    = "0:BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
		userWallet     = "0:CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
		legAmount      = "456"
		legTargetValue = "457"
	)
	record := map[string]interface{}{
		"asset":              asset,
		"amount":             legAmount,
		"user_jetton_wallet": userWallet,
		"target_asset":       targetAsset,
		"target_amount":      legTargetValue,
	}
	for _, tc := range []struct {
		name         string
		legs         []interface{}
		wantErr      string
		firstFilled  bool
		secondFilled bool
	}{
		{name: "empty leg before a filled one", legs: []interface{}{nil, record}, secondFilled: true},
		{name: "empty trailing leg", legs: []interface{}{record, nil}, firstFilled: true},
		{name: "only a placeholder", legs: []interface{}{nil}},
		{name: "non-map element", legs: []interface{}{1, record}, wantErr: "expected map"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			payload, err := parse.EncodeCompactForTest(map[string]interface{}{"legs": tc.legs})
			if err != nil {
				t.Fatal(err)
			}
			raw := &models.RawAction{Type: "dex_deposit_liquidity"}
			err = parse.DecodeActionPayload(payload, raw)
			if tc.wantErr != "" {
				if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
					t.Fatalf("error = %v, want it to contain %q", err, tc.wantErr)
				}
				return
			}
			if err != nil {
				t.Fatalf("DecodeActionPayload: %v", err)
			}
			assertLegSlot(t, "slot 1", raw.DexDepositLiquidityDataAmount1, raw.DexDepositLiquidityDataAsset1, raw.DexDepositLiquidityDataUserJettonWallet1, tc.firstFilled)
			assertLegSlot(t, "slot 2", raw.DexDepositLiquidityDataAmount2, raw.DexDepositLiquidityDataAsset2, raw.DexDepositLiquidityDataUserJettonWallet2, tc.secondFilled)
		})
	}
}

func assertLegSlot(t *testing.T, name string, amount *string, asset, wallet *models.AccountAddress, filled bool) {
	t.Helper()
	if !filled {
		if amount != nil || asset != nil || wallet != nil {
			t.Errorf("%s is filled: %v %v %v", name, amount, asset, wallet)
		}
		return
	}
	if amount == nil || *amount != "456" {
		t.Errorf("%s amount = %v, want 456", name, amount)
	}
	if asset == nil || !strings.HasPrefix(strings.ToUpper(string(*asset)), "0:AAAA") {
		t.Errorf("%s asset = %v, want the leg asset", name, asset)
	}
	if wallet == nil || !strings.HasPrefix(strings.ToUpper(string(*wallet)), "0:CCCC") {
		t.Errorf("%s user jetton wallet = %v, want the leg wallet", name, wallet)
	}
}
