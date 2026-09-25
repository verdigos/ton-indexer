package crud

import (
	"encoding/base64"
	"errors"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/emulated"
	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"
	"github.com/vmihailenco/msgpack/v5"
)

// pendingTraceHSet builds the Redis hset shape FillFromRawData reads: the root node key,
// the node map and the `actions` msgpack list.
func pendingTraceHSet(t *testing.T, actions []interface{}) (string, map[string]map[string]string) {
	t.Helper()
	rootHash := make([]byte, 32)
	for i := range rootHash {
		rootHash[i] = byte(i)
	}
	rootKey := base64.StdEncoding.EncodeToString(rootHash)
	node := map[string]interface{}{
		"transaction": map[string]interface{}{
			"hash":       rootHash,
			"account":    "0:9F9C2E1B0D4A6F8E3C5B7A1D2E4F60718293A4B5C6D7E8F90A1B2C3D4E5F6071",
			"lt":         uint64(58913247000007),
			"now":        uint32(1753800003),
			"total_fees": uint64(3040000),
			"out_msgs":   []interface{}{},
			// ToTransaction type-asserts the compute phase, so the node needs one.
			"description": map[string]interface{}{
				"compute_ph": []interface{}{0, map[string]interface{}{"reason": 0}},
			},
		},
		"emulated":       false,
		"mc_block_seqno": uint32(48210934),
		"finality":       1,
		"block_id": map[string]interface{}{
			"workchain": int32(0),
			"shard":     uint64(0x8000000000000000),
			"seqno":     uint32(51230012),
		},
	}
	marshal := func(value interface{}) []byte {
		raw, err := msgpack.Marshal(value)
		if err != nil {
			t.Fatalf("marshal: %v", err)
		}
		return raw
	}
	traceKey := base64.StdEncoding.EncodeToString(rootHash)
	hset := map[string]string{
		"root_node": rootKey,
		rootKey:     string(marshal(node)),
		"actions":   string(marshal(actions)),
	}
	return traceKey, map[string]map[string]string{traceKey: hset}
}

// compactActionPayload is a codec-1 payload: `{amount: 7, comment: "hello"}` in registry
// ids (2 and 13), worst case the same bytes the engine writes.
func compactActionPayload() []byte {
	return []byte{1, 0x82, 2, 0x07, 13, 0xa5, 'h', 'e', 'l', 'l', 'o'}
}

// TestFillFromRawDataSkipsUndecodablePayload verifies that malformed or missing
// payloads drop only their actions; the pending trace and its decodable actions remain.
func TestFillFromRawDataSkipsUndecodablePayload(t *testing.T) {
	legacy := []byte{1, 0x81, 0xa4, 'd', 'a', 't', 'a', 0x80} // codec 1 + the old wrapped body
	traceKey, rawData := pendingTraceHSet(t, []interface{}{
		map[string]interface{}{"action_id": "good", "type": "jetton_transfer", "payload": compactActionPayload()},
		map[string]interface{}{"action_id": "legacy", "type": "jetton_transfer", "payload": legacy},
		map[string]interface{}{"action_id": "empty", "type": "jetton_transfer"},
	})

	// The undecodable action carries the marker the stored path skips on.
	bad := emulated.Action{ActionId: "legacy", Type: "jetton_transfer", Payload: legacy}
	if _, err := bad.ToRawAction(); err == nil || !errors.Is(err, parse.ErrPayloadDecode) {
		t.Fatalf("ToRawAction error = %v, want ErrPayloadDecode", err)
	}
	// A row without a payload column is undecodable too.
	empty := emulated.Action{ActionId: "empty", Type: "jetton_transfer"}
	if _, err := empty.ToRawAction(); err == nil || !errors.Is(err, parse.ErrPayloadDecode) {
		t.Fatalf("ToRawAction(no payload) error = %v, want ErrPayloadDecode", err)
	}

	context := NewEmptyContext(false)
	if err := context.FillFromRawData(rawData); err != nil {
		t.Fatalf("FillFromRawData: %v", err)
	}
	ids := map[string]bool{}
	for _, action := range context.GetAllActions() {
		ids[string(action.ActionId)] = true
	}
	if !ids["good"] {
		t.Errorf("actions = %v, want the decodable one", ids)
	}
	for _, dropped := range []string{"legacy", "empty"} {
		if ids[dropped] {
			t.Errorf("the undecodable action %q is still in the response: %v", dropped, ids)
		}
	}
	if len(context.GetTraces()) != 1 || string(*context.GetTraces()[0].TraceId) != traceKey {
		t.Errorf("traces = %+v, want the one pending trace", context.GetTraces())
	}
}
