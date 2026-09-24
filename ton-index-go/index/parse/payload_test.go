package parse_test

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"encoding/json"
	"os"
	"reflect"
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
			assertLegacyIsPurchase(t, fixture.Legacy, wireRaw.NFTTransferIsPurchase)

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

func assertLegacyIsPurchase(t *testing.T, legacyJSON json.RawMessage, got *bool) {
	t.Helper()
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
			payload, err := msgpack.Marshal(map[string]interface{}{
				"data": map[string]interface{}{"claimed_rewards": []interface{}{tc.record}},
			})
			if err != nil {
				t.Fatal(err)
			}
			if err := parse.DecodeActionPayload(append([]byte{1}, payload...), raw); err != nil {
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

func TestActionPayloadRejectsLegacyGroups(t *testing.T) {
	payload, err := msgpack.Marshal(map[string]interface{}{
		"data":    map[string]interface{}{"nft_index": "1"},
		"listing": map[string]interface{}{"nft_index": "1"},
	})
	if err != nil {
		t.Fatal(err)
	}
	err = parse.DecodeActionPayload(append([]byte{1}, payload...), &models.RawAction{Type: "nft_transfer"})
	if err == nil {
		t.Fatal("legacy group accepted")
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
	for i := 0; i < value.NumField(); i++ {
		if !envelope[typeOf.Field(i).Name] {
			value.Field(i).SetZero()
		}
	}
}
