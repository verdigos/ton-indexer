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

const contractRows = 1218

var contractPath = flag.String("payload-contract-file", "testdata/payload_contract.jsonl.gz", "API contract baseline")

type payloadContractRow struct {
	actionPayloadFixture
	API json.RawMessage `json:"api"`
}

// Any exception needs a row key and the reason the API difference is accepted.
var allowedPayloadContractDifferences = map[string]string{}

func TestPayloadContract(t *testing.T) {
	f, err := os.Open(*contractPath)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	z, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	defer z.Close()
	s := bufio.NewScanner(z)
	s.Buffer(nil, 16<<20)
	seen := make(map[string]bool)
	lastKey := ""
	count := 0
	for s.Scan() {
		var row payloadContractRow
		if err := json.Unmarshal(s.Bytes(), &row); err != nil {
			t.Fatalf("row %d: %v", count+1, err)
		}
		key := fmt.Sprintf("%s/%s/%s", row.FixtureID, row.ActionID, row.Type)
		if seen[key] || key <= lastKey {
			t.Fatalf("duplicate or unsorted row %q after %q", key, lastKey)
		}
		seen[key] = true
		lastKey = key
		count++
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
				if reason := allowedPayloadContractDifferences[key]; reason != "" {
					t.Logf("accepted API change: %s", reason)
					return
				}
				t.Errorf("API changed\nwant: %s\ngot:  %s", row.API, got)
			}
		})
	}
	if err := s.Err(); err != nil {
		t.Fatal(err)
	}
	if count != contractRows {
		t.Fatalf("baseline has %d rows, want %d", count, contractRows)
	}
	for key, reason := range allowedPayloadContractDifferences {
		if !seen[key] || reason == "" {
			t.Errorf("invalid allowed difference %q: %q", key, reason)
		}
	}
}
