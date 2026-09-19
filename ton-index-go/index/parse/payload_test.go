package parse_test

import (
	"bufio"
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
			wireRaw := decodeWireAction(t, fixture.Wire)
			if string(wireRaw.ActionId) != fixture.ActionID || wireRaw.Type != fixture.Type {
				t.Fatalf("fixture identity = %s/%s, wire = %s/%s", fixture.ActionID, fixture.Type, wireRaw.ActionId, wireRaw.Type)
			}

			payload, err := hex.DecodeString(fixture.Payload)
			if err != nil {
				t.Fatalf("decode payload hex: %v", err)
			}
			payloadRaw := *wireRaw
			clearPayloadFields(&payloadRaw)
			if err := parse.DecodeActionPayload(payload, &payloadRaw); err != nil {
				t.Fatalf("DecodeActionPayload: %v", err)
			}
			assertLegacyIsPurchase(t, fixture.Legacy, payloadRaw.NFTTransferIsPurchase)

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
