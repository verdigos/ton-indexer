package parse_test

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"
)

const contractRows = 1224

var contractPath = flag.String("payload-contract-file", "testdata/payload_contract.jsonl.gz", "API contract baseline")

type payloadContractRow struct {
	FixtureID string          `json:"fixture_id"`
	ActionID  string          `json:"action_id"`
	Type      string          `json:"type"`
	Payload   string          `json:"payload_hex"`
	Wire      string          `json:"wire_hex"`
	API       json.RawMessage `json:"api"`
}

// checkAPIBaseline decodes every row of a gzip jsonl contract baseline through the payload
// decoder and requires the API JSON of each row byte for byte. It returns the row keys in
// file order.
func checkAPIBaseline(t *testing.T, path string, rows int) []string {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	z, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	defer z.Close()
	keys := make([]string, 0, rows)
	s := bufio.NewScanner(z)
	s.Buffer(nil, 16<<20)
	for s.Scan() {
		var row payloadContractRow
		if err := json.Unmarshal(s.Bytes(), &row); err != nil {
			t.Fatalf("row %d: %v", len(keys)+1, err)
		}
		key := fmt.Sprintf("%s/%s/%s", row.FixtureID, row.ActionID, row.Type)
		keys = append(keys, key)
		t.Run(key, func(t *testing.T) {
			if len(row.API) == 0 {
				t.Fatal("missing API JSON")
			}
			wire := decodeWireAction(t, row.Wire)
			if string(wire.ActionId) != row.ActionID || wire.Type != row.Type {
				t.Fatalf("wire identity = %s/%s", wire.ActionId, wire.Type)
			}
			// The emulator row and the DB column carry the same payload bytes.
			assertWirePayload(t, row.Wire, row.Payload)
			raw := *wire
			clearPayloadFields(&raw)
			payload, err := hex.DecodeString(row.Payload)
			if err != nil {
				t.Fatal(err)
			}
			if err := parse.DecodeActionPayload(payload, &raw); err != nil {
				t.Fatal(err)
			}
			action, err := parse.ParseRawAction(&raw)
			if err != nil {
				t.Fatal(err)
			}
			got, err := json.Marshal(action)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(got, row.API) {
				t.Errorf("API changed\nwant: %s\ngot:  %s", row.API, got)
			}
		})
	}
	if err := s.Err(); err != nil {
		t.Fatal(err)
	}
	if len(keys) != rows {
		t.Fatalf("baseline has %d rows, want %d", len(keys), rows)
	}
	return keys
}

// TestPayloadContract requires the API JSON of the current decoder over the committed
// baseline, whose rows must be sorted and free of duplicates.
func TestPayloadContract(t *testing.T) {
	keys := checkAPIBaseline(t, *contractPath, contractRows)
	seen := make(map[string]bool, len(keys))
	lastKey := ""
	for _, key := range keys {
		if seen[key] || key <= lastKey {
			t.Fatalf("duplicate or unsorted row %q after %q", key, lastKey)
		}
		seen[key] = true
		lastKey = key
	}
}
