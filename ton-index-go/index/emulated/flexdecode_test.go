package emulated

import (
	"bytes"
	"compress/gzip"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"math"
	"reflect"
	"testing"

	"github.com/vmihailenco/msgpack/v5"
)

// stringNumberExclusions lists the top-level fields the string-number form keeps native.
var stringNumberExclusions = map[string]bool{
	"start_lt": true, "end_lt": true, "start_utime": true, "end_utime": true,
	"opcode": true, "finality": true, "trace_end_lt": true,
	"trace_end_utime": true, "mc_seqno_end": true, "trace_mc_seqno_end": true,
}

// stringNumbers reproduces the string-number form: every value but the exclusions above
// arrives as a decimal string.
func stringNumbers(m map[string]interface{}) map[string]interface{} {
	out := make(map[string]interface{}, len(m))
	for k, v := range m {
		if stringNumberExclusions[k] {
			out[k] = v
		} else {
			out[k] = stringNumberValue(v)
		}
	}
	return out
}

func stringNumberValue(v interface{}) interface{} {
	switch t := v.(type) {
	case map[string]interface{}:
		out := make(map[string]interface{}, len(t))
		for k, item := range t {
			out[k] = stringNumberValue(item)
		}
		return out
	case []interface{}:
		out := make([]interface{}, len(t))
		for i, item := range t {
			out[i] = stringNumberValue(item)
		}
		return out
	case bool, string, nil, []string, []byte:
		return v
	default:
		return fmt.Sprint(v)
	}
}

// stringNumbersAction applies the string-number form to the whole row; the payload
// bytes pass through untouched.
func stringNumbersAction(t *testing.T) map[string]interface{} {
	return stringNumbers(naturalAction(t))
}

// naturalAction carries numeric fields in their native msgpack forms.
func naturalAction(t *testing.T) map[string]interface{} {
	return map[string]interface{}{
		"action_id":                "9C7SqQ==",
		"type":                     "jetton_transfer",
		"trace_id":                 "dHJhY2VfaWQ=",
		"trace_external_hash":      "ZXh0X2hhc2g=",
		"trace_external_hash_norm": "ZXh0X2hhc2hfbm9ybQ==",
		"tx_hashes":                []string{"aGFzaDE=", "aGFzaDI="},
		"success":                  true,
		"ancestor_type":            []string{"jvault_stake"},

		// top-level numbers the string-number form keeps native
		"start_lt":           uint64(58913247000001),
		"end_lt":             uint64(58913247000007),
		"start_utime":        uint32(1753800000),
		"end_utime":          uint32(1753800003),
		"opcode":             uint32(0x0f8a7ea5),
		"finality":           1,
		"trace_mc_seqno_end": uint32(48210934),

		// top-level *string fields the string-number form stringifies
		"value":  uint64(1500000000),
		"amount": "115792089237316195423570985008687907853269984665640564039457584007913129639935",

		"source":      "0:9F9C2E1B0D4A6F8E3C5B7A1D2E4F60718293A4B5C6D7E8F90A1B2C3D4E5F6071",
		"destination": "0:1122334455667788990011223344556677889900112233445566778899001122",
		"asset":       "0:AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899",

		// the row carries the same payload bytes as the DB `payload` column
		"payload": payloadBytes(t, jettonTransferPayloadHex),
	}
}

// The two fixture payloads in the compact codec: one jetton_transfer detail set and one
// accounts_added list.
const (
	jettonTransferPayloadHex = "01850da568656c6c6f0ec220ce17d7840059cfffffffffffffffff5dc42100bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	accountsAddedPayloadHex  = "01810191d942303a41414242434344444545464630303131323233333434353536363737383839394141424243434444454546463030313132323333343435353636373738383939"
)

// payloadBytes decodes one of the payload constants above.
func payloadBytes(t *testing.T, encoded string) []byte {
	t.Helper()
	raw, err := hex.DecodeString(encoded)
	if err != nil {
		t.Fatalf("decode payload hex: %v", err)
	}
	return raw
}

func mustMarshal(t *testing.T, v interface{}) []byte {
	t.Helper()
	b, err := msgpack.Marshal(v)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	return b
}

func decodeAction(t *testing.T, m map[string]interface{}) Action {
	t.Helper()
	var a Action
	if err := msgpack.Unmarshal(mustMarshal(t, m), &a); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	return a
}

// TestStringNumbersAndNativeDecodeIdentically checks both numeric wire representations.
func TestStringNumbersAndNativeDecodeIdentically(t *testing.T) {
	old := decodeAction(t, stringNumbersAction(t))
	new_ := decodeAction(t, naturalAction(t))

	if !reflect.DeepEqual(old, new_) {
		t.Fatalf("string-number and native-style decode differ:\nold = %+v\nnew = %+v", old, new_)
	}

	// String-number scalar values decode without changing their values.
	if old.ActionId != "9C7SqQ==" || old.Type != "jetton_transfer" || !old.Success {
		t.Errorf("scalars: %+v", old)
	}
	if got := old.StartLt; got == nil || *got != 58913247000001 {
		t.Errorf("start_lt = %v", got)
	}
	if got := old.Opcode; got == nil || *got != 0x0f8a7ea5 {
		t.Errorf("opcode = %v", got)
	}
	if old.Finality != 1 {
		t.Errorf("finality = %v", old.Finality)
	}
	if got := old.Value; got == nil || *got != "1500000000" {
		t.Errorf("value = %v", got)
	}
	if got := old.Amount; got == nil || *got != "115792089237316195423570985008687907853269984665640564039457584007913129639935" {
		t.Errorf("amount = %v", got)
	}
	if !reflect.DeepEqual(old.TxHashes, []string{"aGFzaDE=", "aGFzaDI="}) {
		t.Errorf("tx_hashes = %v", old.TxHashes)
	}

	// The payload travels as bytes and decodes through the shared reader.
	if !reflect.DeepEqual(old.Payload, new_.Payload) {
		t.Errorf("payload bytes differ: %x / %x", old.Payload, new_.Payload)
	}
	// The wire ancestry list is copied onto the RawAction the API embeds as raw_action.
	raw, err := old.ToRawAction()
	if err != nil {
		t.Fatalf("ToRawAction: %v", err)
	}
	if len(raw.AncestorType) != 1 || raw.AncestorType[0] != "jvault_stake" {
		t.Errorf("raw ancestor_type = %v", raw.AncestorType)
	}
}

// TestPayloadDecodeError checks a malformed payload fails the conversion.
func TestPayloadDecodeError(t *testing.T) {
	a := decodeAction(t, map[string]interface{}{
		"action_id": "a", "type": "jetton_transfer", "payload": []byte{9, 0},
	})
	if _, err := a.ToRawAction(); err == nil {
		t.Fatal("unsupported payload codec accepted")
	}
}

// TestFlexDecodeRejectsNonNumbers checks invalid input and nil handling.
func TestFlexDecodeRejectsNonNumbers(t *testing.T) {
	for _, bad := range []interface{}{"abc", "1.5", "", "0x10", true} {
		var a Action
		err := msgpack.Unmarshal(mustMarshal(t, map[string]interface{}{
			"start_lt": bad,
		}), &a)
		if err == nil {
			t.Errorf("start_lt = %#v decoded to %v, want error", bad, *a.StartLt)
		}
	}

	a := decodeAction(t, map[string]interface{}{
		"value":     nil,
		"opcode":    nil,
		"end_utime": uint32(4294967295),
	})
	if a.Value != nil {
		t.Errorf("value = %v, want nil", *a.Value)
	}
	if a.Opcode != nil {
		t.Errorf("opcode = %v, want nil", *a.Opcode)
	}
	if *a.EndUtime != 4294967295 {
		t.Errorf("end_utime = %v, want 4294967295", *a.EndUtime)
	}
}

// TestControlStockDecoderStillRejectsStrings checks exact-type registration.
func TestControlStockDecoderStillRejectsStrings(t *testing.T) {
	type stockInt64 int64
	var v struct {
		StartLt *stockInt64 `msgpack:"start_lt"`
	}
	blob := mustMarshal(t, map[string]interface{}{"start_lt": "3"})
	if err := msgpack.Unmarshal(blob, &v); err == nil {
		t.Fatalf("stock decoder accepted %q -> %v", "3", *v.StartLt)
	}
	var flex struct {
		StartLt *int64 `msgpack:"start_lt"`
	}
	if err := msgpack.Unmarshal(blob, &flex); err != nil {
		t.Fatalf("flex decoder rejected %q: %v", "3", err)
	}
}

// TestParseFlexBoundaries checks rounded float bounds and exact decimal maxima.
func TestParseFlexBoundaries(t *testing.T) {
	// 2^63-1024 and 2^64-2048: the largest value of each type float64 holds exactly
	for _, tc := range []struct {
		in   string
		want int64
	}{
		{"9223372036854774784.0", 9223372036854774784},
		{"-9223372036854775808.0", math.MinInt64},
		// MinInt64 is exactly representable and remains in range.
		{"-9223372036854775809.0", math.MinInt64},
	} {
		got, err := parseFlexInt(tc.in)
		if err != nil || got != tc.want {
			t.Errorf("parseFlexInt(%q) = %d, %v; want %d, nil", tc.in, got, err, tc.want)
		}
	}
	// -2^63-2048 is the next float64 below MinInt64.
	for _, in := range []string{"9223372036854775807.0", "9223372036854775808.0", "-9223372036854777856.0"} {
		if got, err := parseFlexInt(in); err == nil {
			t.Errorf("parseFlexInt(%q) = %d, want error", in, got)
		}
	}

	if got, err := parseFlexUint("18446744073709549568.0"); err != nil || got != 18446744073709549568 {
		t.Errorf("parseFlexUint = %d, %v; want 18446744073709549568, nil", got, err)
	}
	for _, in := range []string{"18446744073709551615.0", "18446744073709551616.0", "-1.0"} {
		if got, err := parseFlexUint(in); err == nil {
			t.Errorf("parseFlexUint(%q) = %d, want error", in, got)
		}
	}

	// Plain decimal maxima use exact ParseInt and ParseUint paths.
	if got, err := parseFlexInt("9223372036854775807"); err != nil || got != math.MaxInt64 {
		t.Errorf("parseFlexInt(MaxInt64) = %d, %v", got, err)
	}
	if got, err := parseFlexUint("18446744073709551615"); err != nil || got != math.MaxUint64 {
		t.Errorf("parseFlexUint(MaxUint64) = %d, %v", got, err)
	}
}

func TestNormAddr(t *testing.T) {
	for _, tc := range []struct{ in, want string }{
		{"0:9f9c2e1b0d4a6f8e3c5b7a1d2e4f60718293a4b5c6d7e8f90a1b2c3d4e5f6071",
			"0:9F9C2E1B0D4A6F8E3C5B7A1D2E4F60718293A4B5C6D7E8F90A1B2C3D4E5F6071"},
		{"0:9F9C2E1B0D4A6F8E3C5B7A1D2E4F60718293A4B5C6D7E8F90A1B2C3D4E5F6071",
			"0:9F9C2E1B0D4A6F8E3C5B7A1D2E4F60718293A4B5C6D7E8F90A1B2C3D4E5F6071"},
		{"-1:3333333333333333333333333333333333333333333333333333333333333333",
			"-1:3333333333333333333333333333333333333333333333333333333333333333"},
		// not raw hex: left alone
		{"EQAvlWFDfGHYPGZbFCPnfnbeSuBnpXpvOMNTHKHNqm6ZKlU-", "EQAvlWFDfGHYPGZbFCPnfnbeSuBnpXpvOMNTHKHNqm6ZKlU-"},
		{"", ""},
		{"0:", "0:"},
		{"addr_none", "addr_none"},
	} {
		if got := normAddr(tc.in); got != tc.want {
			t.Errorf("normAddr(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}

	const (
		lower = "0:aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899"
		upper = "0:AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899"
	)
	a := decodeAction(t, map[string]interface{}{
		"action_id": "a", "type": "vesting_add_whitelist",
		"source":      lower,
		"destination": upper,
		"accounts":    []string{lower, upper},
		"payload":     payloadBytes(t, accountsAddedPayloadHex),
	})
	raw, err := a.ToRawAction()
	if err != nil {
		t.Fatalf("ToRawAction: %v", err)
	}
	if raw.Source == nil || string(*raw.Source) != string(*raw.Destination) {
		t.Errorf("source %v not normalized to destination %v", raw.Source, raw.Destination)
	}
	// the envelope account list bypasses ptrAccountAddress and is normalized
	if len(raw.Accounts) != 2 || string(raw.Accounts[0]) != upper || string(raw.Accounts[1]) != upper {
		t.Errorf("accounts = %v", raw.Accounts)
	}
	if len(raw.VestingAddWhitelistAccountsAdded) != 1 || string(raw.VestingAddWhitelistAccountsAdded[0]) != upper {
		t.Errorf("accounts_added = %v", raw.VestingAddWhitelistAccountsAdded)
	}
	if raw.Asset != nil {
		t.Errorf("absent asset = %v, want nil", *raw.Asset)
	}
}

// TestToTransactionAccount checks the node conversion returns the node's normalized
// account. The fixture carries a compute phase because ToTransaction type-asserts
// ComputePh.Data.
func TestToTransactionAccount(t *testing.T) {
	const lower = "0:aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899"
	const upper = "0:AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899"
	node := TraceNode{Transaction: transaction{
		Account:     lower,
		Description: transactionDescr{ComputePh: computePhaseVar{Data: trComputePhaseSkipped{}}},
	}}
	tx, err := node.ToTransaction()
	if err != nil {
		t.Fatalf("ToTransaction: %v", err)
	}
	if string(tx.Account) != upper {
		t.Errorf("transaction account = %q", tx.Account)
	}
}

// TestConvertHSetBothEncodings checks numeric encodings and compression states.
func TestConvertHSetBothEncodings(t *testing.T) {
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

	gzipBytes := func(b []byte) []byte {
		var buf bytes.Buffer
		w := gzip.NewWriter(&buf)
		if _, err := w.Write(b); err != nil {
			t.Fatal(err)
		}
		if err := w.Close(); err != nil {
			t.Fatal(err)
		}
		return buf.Bytes()
	}

	build := func(actionMap map[string]interface{}, nodeMap map[string]interface{}, zip bool) map[string]string {
		actions := mustMarshal(t, []interface{}{actionMap})
		if zip {
			actions = gzipBytes(actions)
		}
		return map[string]string{
			"root_node": rootKey,
			rootKey:     string(mustMarshal(t, nodeMap)),
			"actions":   string(actions),
		}
	}

	natural := naturalAction(t)
	oldTrace, err := ConvertHSet(build(stringNumbersAction(t), node, false), rootKey)
	if err != nil {
		t.Fatalf("string-number ConvertHSet: %v", err)
	}
	newTrace, err := ConvertHSet(build(natural, node, true), rootKey)
	if err != nil {
		t.Fatalf("natural-style gzipped ConvertHSet: %v", err)
	}
	if !reflect.DeepEqual(oldTrace, newTrace) {
		t.Fatalf("traces differ:\nold = %+v\nnew = %+v", oldTrace, newTrace)
	}

	if !oldTrace.Classified || len(oldTrace.Actions) != 1 || len(oldTrace.Nodes) != 1 {
		t.Fatalf("trace = %+v", oldTrace)
	}
	n := oldTrace.Nodes[0].Transaction
	if n.Lt != 58913247000007 || n.Now != 1753800003 || n.TotalFees != 3040000 {
		t.Errorf("node numerics: lt=%d now=%d fees=%d", n.Lt, n.Now, n.TotalFees)
	}
	if oldTrace.Nodes[0].BlockId.Shard != 0x8000000000000000 || oldTrace.Nodes[0].McBlockSeqno != 48210934 {
		t.Errorf("block_id = %+v", oldTrace.Nodes[0].BlockId)
	}
	if oldTrace.TraceId == nil || *oldTrace.TraceId != rootKey {
		t.Errorf("trace_id = %v", oldTrace.TraceId)
	}
	if got := oldTrace.Actions[0].TraceEndLt; got == nil || *got != 58913247000007 {
		t.Errorf("trace_end_lt = %v", got)
	}

	// the whole action still converts, payload included
	if _, err := oldTrace.Actions[0].ToRawAction(); err != nil {
		t.Fatalf("ToRawAction: %v", err)
	}
}
