package parse_test

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"sort"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"
)

type fillPilotAPI struct {
	Key string          `json:"key"`
	API json.RawMessage `json:"api"`
}

func fillPilotType(name string) bool {
	switch name {
	case "multisig_create_order", "tgbtc_mint", "tgbtc_mint_fallback", "nft_transfer":
		return true
	}
	return false
}

func fillPilotRows(t *testing.T, path string) map[string]actionPayloadFixture {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	rows := make(map[string]actionPayloadFixture)
	scanner := bufio.NewScanner(f)
	scanner.Buffer(nil, 16<<20)
	for scanner.Scan() {
		var row actionPayloadFixture
		if err := json.Unmarshal(scanner.Bytes(), &row); err != nil {
			t.Fatal(err)
		}
		if !fillPilotType(row.Type) {
			continue
		}
		key := fmt.Sprintf("%s/%s/%s", row.FixtureID, row.ActionID, row.Type)
		if _, exists := rows[key]; exists {
			t.Fatalf("duplicate row %s", key)
		}
		rows[key] = row
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	return rows
}

func fillPilotJSON(t *testing.T, wire, payload string) json.RawMessage {
	t.Helper()
	raw := decodeWireAction(t, wire)
	clearPayloadFields(raw)
	encoded, err := hex.DecodeString(payload)
	if err != nil {
		t.Fatal(err)
	}
	if err := parse.DecodeActionPayload(encoded, raw); err != nil {
		t.Fatal(err)
	}
	action, err := parse.ParseRawAction(raw)
	if err != nil {
		t.Fatal(err)
	}
	result, err := json.Marshal(action)
	if err != nil {
		t.Fatal(err)
	}
	return result
}

// Run once with the HEAD reader to save expected JSON, then with the moved reader.
func TestFillPilotAPI(t *testing.T) {
	phase := os.Getenv("FILL_PILOT_PHASE")
	if phase == "" {
		t.Skip("pilot inputs live in _analysis-scratch/payload")
	}
	oldRows := fillPilotRows(t, os.Getenv("FILL_PILOT_OLD"))
	if len(oldRows) != 39 {
		t.Fatalf("old fixture has %d pilot rows, want 39", len(oldRows))
	}
	keys := make([]string, 0, len(oldRows))
	for key := range oldRows {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	baselinePath := os.Getenv("FILL_PILOT_BASELINE")
	if phase == "baseline" {
		var out bytes.Buffer
		for _, key := range keys {
			row := oldRows[key]
			line, err := json.Marshal(fillPilotAPI{key, fillPilotJSON(t, row.Wire, row.Payload)})
			if err != nil {
				t.Fatal(err)
			}
			out.Write(line)
			out.WriteByte('\n')
		}
		if err := os.WriteFile(baselinePath, out.Bytes(), 0644); err != nil {
			t.Fatal(err)
		}
		return
	}
	if phase != "compare" {
		t.Fatalf("unknown phase %q", phase)
	}
	newRows := fillPilotRows(t, os.Getenv("FILL_PILOT_NEW"))
	if len(newRows) != len(oldRows) {
		t.Fatalf("new fixture has %d pilot rows, want %d", len(newRows), len(oldRows))
	}
	baseline := make(map[string]json.RawMessage)
	f, err := os.Open(baselinePath)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	scanner := bufio.NewScanner(f)
	scanner.Buffer(nil, 16<<20)
	for scanner.Scan() {
		var row fillPilotAPI
		if err := json.Unmarshal(scanner.Bytes(), &row); err != nil {
			t.Fatal(err)
		}
		baseline[row.Key] = row.API
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if len(baseline) != len(oldRows) {
		t.Fatalf("HEAD reader baseline has %d rows, want %d", len(baseline), len(oldRows))
	}
	counts := make(map[string]int)
	for _, key := range keys {
		old := oldRows[key]
		newRow, ok := newRows[key]
		if !ok {
			t.Errorf("missing new row %s", key)
			continue
		}
		got := fillPilotJSON(t, old.Wire, newRow.Payload)
		if !bytes.Equal(got, baseline[key]) {
			t.Errorf("%s: API changed\nold: %s\nnew: %s", key, baseline[key], got)
		}
		counts[old.Type]++
	}
	t.Logf("compared API JSON rows: multisig_create_order=%d, tgbtc_mint=%d, tgbtc_mint_fallback=%d, nft_transfer=%d",
		counts["multisig_create_order"], counts["tgbtc_mint"], counts["tgbtc_mint_fallback"], counts["nft_transfer"])
}
