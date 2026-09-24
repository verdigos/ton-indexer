package parse_test

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"encoding/json"
	"os"
	"reflect"
	"strings"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/emulated"
	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"
	"github.com/vmihailenco/msgpack/v5"
)

type actionPayloadFixture struct {
	FixtureID string          `json:"fixture_id"`
	ActionID  string          `json:"action_id"`
	Type      string          `json:"type"`
	Legacy    json.RawMessage `json:"legacy"`
	Payload   string          `json:"payload_hex"`
	Wire      string          `json:"wire_hex"`
}

func TestActionPayloadFixture(t *testing.T) {
	f, err := os.Open("testdata/action_payload.jsonl")
	if err != nil {
		t.Fatalf("read fixture: %v", err)
	}
	defer f.Close()

	count := 0
	scanner := bufio.NewScanner(f)
	scanner.Buffer(nil, 16<<20)
	for scanner.Scan() {
		count++
		var fixture actionPayloadFixture
		if err := json.Unmarshal(scanner.Bytes(), &fixture); err != nil {
			t.Fatalf("line %d: %v", count, err)
		}
		t.Run(fixture.FixtureID+"/"+fixture.ActionID, func(t *testing.T) {
			// The emulator row must carry the payload column of the same row, so the
			// two paths cannot drift into separate readers.
			assertWirePayload(t, fixture.Wire, fixture.Payload)
			wireRaw := decodeWireAction(t, fixture.Wire)
			if string(wireRaw.ActionId) != fixture.ActionID || wireRaw.Type != fixture.Type {
				t.Fatalf("fixture identity = %s/%s, wire = %s/%s", fixture.ActionID, fixture.Type, wireRaw.ActionId, wireRaw.Type)
			}
			assertLegacyIsPurchase(t, fixture.Payload, fixture.Legacy, wireRaw.NFTTransferIsPurchase)

			payload, err := hex.DecodeString(fixture.Payload)
			if err != nil {
				t.Fatalf("decode payload hex: %v", err)
			}
			payloadRaw := *wireRaw
			clearPayloadFields(&payloadRaw)
			if err := parse.DecodeActionPayload(payload, &payloadRaw); err != nil {
				t.Fatalf("DecodeActionPayload: %v", err)
			}

			got, err := parse.ParseRawAction(&payloadRaw)
			if err != nil {
				t.Fatalf("ParseRawAction(payload): %v", err)
			}
			expected, err := parse.ParseRawAction(wireRaw)
			if err != nil {
				t.Fatalf("ParseRawAction(wire): %v", err)
			}
			// The emulator fills null is_purchase; the SQL-backed payload preserves it.
			clearAPIPurchase(got)
			clearAPIPurchase(expected)
			if !reflect.DeepEqual(got, expected) {
				t.Errorf("payload API = %#v\nwire API = %#v", got, expected)
			}
		})
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if count == 0 {
		t.Fatal("fixture is empty")
	}
}

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

// assertLegacyIsPurchase checks the emulator's is_purchase against the legacy column of
// the fixture row. That column is a stale pre-wave copy, so a row whose payload no longer
// carries the key has nothing to compare - `dns_purchase` stopped emitting is_purchase.
func assertLegacyIsPurchase(t *testing.T, payloadHex string, legacyJSON json.RawMessage, got *bool) {
	t.Helper()
	payload, err := hex.DecodeString(payloadHex)
	if err != nil {
		t.Fatalf("decode payload hex: %v", err)
	}
	// The key reaches NFTTransfer.IsPurchase for every type that carries it, so decoding
	// the payload under that prefix answers whether the payload has the key at all.
	raw := &models.RawAction{Type: "nft_transfer"}
	if err := parse.DecodeActionPayload(payload, raw); err != nil {
		t.Fatalf("decode payload: %v", err)
	}
	if raw.NFTTransferIsPurchase == nil {
		return
	}
	var legacy struct {
		NFTTransferData *struct {
			IsPurchase *bool `json:"is_purchase"`
		} `json:"nft_transfer_data"`
	}
	if err := json.Unmarshal(legacyJSON, &legacy); err != nil {
		t.Fatalf("decode legacy JSON: %v", err)
	}
	var expected *bool
	if legacy.NFTTransferData != nil {
		expected = legacy.NFTTransferData.IsPurchase
	}
	if !reflect.DeepEqual(got, expected) {
		t.Errorf("is_purchase: payload=%v legacy=%v", got, expected)
	}
}

func clearAPIPurchase(action *models.Action) {
	if details, ok := action.Details.(*models.ActionDetailsNftTransfer); ok {
		details.IsPurchase = nil
	}
}

func TestActionPayloadUnknownCodec(t *testing.T) {
	err := parse.DecodeActionPayload([]byte{2}, &models.RawAction{})
	if err == nil || err.Error() != "unsupported payload codec 2" {
		t.Fatalf("got %v", err)
	}
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

// TestActionPayloadRejectsLegacyGroups pins the rejection of the previous payload format:
// its body was a map of named groups (the flat `data` group plus per-family groups), so its
// keys are strings, not registry ids.
func TestActionPayloadRejectsLegacyGroups(t *testing.T) {
	for _, tc := range []struct {
		name    string
		payload map[string]interface{}
	}{
		{
			name: "grouped body",
			payload: map[string]interface{}{
				"data":    map[string]interface{}{"nft_index": "1"},
				"listing": map[string]interface{}{"nft_index": "1"},
			},
		},
		{
			name:    "flat named body",
			payload: map[string]interface{}{"data": map[string]interface{}{"nft_index": "1"}},
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			payload, err := msgpack.Marshal(tc.payload)
			if err != nil {
				t.Fatal(err)
			}
			err = parse.DecodeActionPayload(append([]byte{1}, payload...), &models.RawAction{Type: "nft_transfer"})
			if err == nil {
				t.Fatal("legacy payload accepted")
			}
			if !strings.Contains(err.Error(), "is not a registry id") {
				t.Errorf("error = %q, want it to name the rejected key", err)
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
// id-keyed map. The empty payload is `01 80`, an id this table does not know is skipped,
// and anything else about the frame is rejected.
func TestDecodeActionPayloadEnvelope(t *testing.T) {
	for _, tc := range []struct {
		name    string
		payload string
		wantErr string
	}{
		{name: "empty payload", payload: "0180"},
		{name: "unknown key id is skipped", payload: "0181ccc8c0"},
		{name: "nil body", payload: "01c0", wantErr: "expected a map, got nil"},
		{name: "body is an array", payload: "0190", wantErr: "expected a map"},
		{name: "truncated map header", payload: "0181", wantErr: "map declares 1 entries"},
		{name: "truncated value", payload: "018106", wantErr: "map declares 1 entries but only 1 bytes remain"},
		{name: "legacy name-keyed body", payload: "0181a46461746180", wantErr: "is not a registry id"},
		{name: "trailing data", payload: "018000", wantErr: "trailing data"},
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

// TestDecodeActionPayloadEmptyLegFromCppBytes decodes the exact bytes the C++ encoder
// produced for `legs = [nil, {amount, asset, target_amount, target_asset,
// user_jetton_wallet}]` (DeDust partial block, `build_action` + `encode_action_payload` in
// `harness/RedisActionSerializeTest.cpp:295-340`, printed as `LEGS-ONE-SIDED-HEX` by
// `ton-mch-engine --actions-msgpack-test`): the API must report every value in slot 2 and
// leave slot 1 empty. The bytes are the engine's, not the test encoder's: a change of the
// wire format breaks this case.
func TestDecodeActionPayloadEmptyLegFromCppBytes(t *testing.T) {
	const engineHex = "01822c92c08502cd01c806c42100aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa6ccd" +
		"01c96dc42100bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb76c42100cccccccccccc" +
		"cccccccccccccccccccccccccccccccccccccccccccccccccccc54a6646564757374"
	payload, err := hex.DecodeString(engineHex)
	if err != nil {
		t.Fatalf("decode payload hex: %v", err)
	}
	raw := &models.RawAction{Type: "dex_deposit_liquidity"}
	if err := parse.DecodeActionPayload(payload, raw); err != nil {
		t.Fatalf("DecodeActionPayload: %v", err)
	}
	action, err := parse.ParseRawAction(raw)
	if err != nil {
		t.Fatalf("ParseRawAction: %v", err)
	}
	details, ok := action.Details.(*models.ActionDetailsDexDepositLiquidity)
	if !ok {
		t.Fatalf("details = %T", action.Details)
	}
	if details.Dex == nil || *details.Dex != "dedust" {
		t.Errorf("dex = %v, want dedust", details.Dex)
	}
	for name, value := range map[string]*models.AccountAddress{
		"asset_1": details.Asset1, "user_jetton_wallet_1": details.UserJettonWallet1, "target_asset_1": details.TargetAsset1,
	} {
		if value != nil {
			t.Errorf("%s = %v, want nil (the empty leg owns slot 1)", name, value)
		}
	}
	for name, value := range map[string]*string{"amount_1": details.Amount1, "target_amount_1": details.TargetAmount1} {
		if value != nil {
			t.Errorf("%s = %v, want nil (the empty leg owns slot 1)", name, value)
		}
	}
	if details.Amount2 == nil || *details.Amount2 != "456" {
		t.Errorf("amount_2 = %v, want 456", details.Amount2)
	}
	if details.TargetAmount2 == nil || *details.TargetAmount2 != "457" {
		t.Errorf("target_amount_2 = %v, want 457", details.TargetAmount2)
	}
	if details.Asset2 == nil || !strings.HasPrefix(strings.ToUpper(string(*details.Asset2)), "0:AAAA") {
		t.Errorf("asset_2 = %v, want the leg asset", details.Asset2)
	}
	if details.UserJettonWallet2 == nil || !strings.HasPrefix(strings.ToUpper(string(*details.UserJettonWallet2)), "0:CCCC") {
		t.Errorf("user_jetton_wallet_2 = %v, want the leg wallet", details.UserJettonWallet2)
	}
	if details.TargetAsset2 == nil || !strings.HasPrefix(strings.ToUpper(string(*details.TargetAsset2)), "0:BBBB") {
		t.Errorf("target_asset_2 = %v, want the leg target asset", details.TargetAsset2)
	}
}
