package parse

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"math/big"
	"reflect"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
	"github.com/vmihailenco/msgpack/v5"
)

// EncodeCompactForTest independently encodes decoder-form values as a codec-1 payload.
// Its ascending-id key order matches the engine encoder.
func EncodeCompactForTest(named map[string]interface{}) ([]byte, error) {
	var buf bytes.Buffer
	buf.WriteByte(actionPayloadCodecMsgpack)
	if err := writeCompactRecordForTest(&buf, named); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

type compactTestEntry struct {
	id   uint8
	name string
	info payloadKeyInfo
}

var payloadKeyIDByName = func() map[string]uint8 {
	ids := make(map[string]uint8)
	for id, key := range payloadKeys {
		if key.Name != "" {
			ids[key.Name] = uint8(id)
		}
	}
	return ids
}()

func writeCompactRecordForTest(buf *bytes.Buffer, named map[string]interface{}) error {
	entries := make([]compactTestEntry, 0, len(named))
	for name := range named {
		id, ok := payloadKeyIDByName[name]
		if !ok {
			return fmt.Errorf("unknown payload key %q", name)
		}
		entries = append(entries, compactTestEntry{id: id, name: name, info: payloadKeys[id]})
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].id < entries[j].id })
	writeCompactHeaderForTest(buf, 0x80, 0xde, 0xdf, len(entries))
	for _, entry := range entries {
		writeCompactKeyForTest(buf, entry.id)
		if err := writeCompactValueForTest(buf, entry.info.Type, named[entry.name]); err != nil {
			return fmt.Errorf("payload key %s: %w", entry.name, err)
		}
	}
	return nil
}

// writeCompactKeyForTest writes one key id: a positive fixint below 128, the uint8 form
// from 128 up.
func writeCompactKeyForTest(buf *bytes.Buffer, id uint8) {
	if id < 0x80 {
		buf.WriteByte(id)
		return
	}
	buf.WriteByte(0xcc)
	buf.WriteByte(id)
}

func writeCompactHeaderForTest(buf *bytes.Buffer, fixLow, code16, code32 byte, n int) {
	switch {
	case n < 16:
		buf.WriteByte(fixLow | byte(n))
	case n <= 0xffff:
		buf.WriteByte(code16)
		buf.WriteByte(byte(n >> 8))
		buf.WriteByte(byte(n))
	default:
		buf.WriteByte(code32)
		var raw [4]byte
		binary.BigEndian.PutUint32(raw[:], uint32(n))
		buf.Write(raw[:])
	}
}

func writeCompactValueForTest(buf *bytes.Buffer, kind payloadKeyType, value interface{}) error {
	if kind == payloadTypeOther {
		return writeCompactOtherForTest(buf, value)
	}
	if value == nil {
		buf.WriteByte(0xc0)
		return nil
	}
	switch kind {
	case payloadTypeAddress:
		raw, err := compactTestAddress(value)
		if err != nil {
			return err
		}
		return writeCompactScalarForTest(buf, raw)
	case payloadTypeBoc:
		raw, err := compactTestBase64(value)
		if err != nil {
			return err
		}
		return writeCompactScalarForTest(buf, raw)
	case payloadTypeUint:
		return writeCompactUintTextForTest(buf, value)
	case payloadTypeHash:
		raw, err := compactTestHash(value, "hash", func(text string) ([]byte, error) { return base64.StdEncoding.DecodeString(text) })
		if err != nil {
			return err
		}
		return writeCompactScalarForTest(buf, raw)
	case payloadTypeHashHex:
		raw, err := compactTestHash(value, "hash_hex", hex.DecodeString)
		if err != nil {
			return err
		}
		return writeCompactScalarForTest(buf, raw)
	case payloadTypeHash0x:
		text, ok := value.(string)
		if !ok {
			return fmt.Errorf("expected a string, got %T", value)
		}
		raw, err := compactTestHash(strings.TrimPrefix(text, "0x"), "hash_0x", hex.DecodeString)
		if err != nil {
			return err
		}
		return writeCompactScalarForTest(buf, raw)
	case payloadTypeString:
		text, ok := value.(string)
		if !ok {
			return fmt.Errorf("expected a string, got %T", value)
		}
		return writeCompactScalarForTest(buf, text)
	case payloadTypeBool:
		flag, ok := value.(bool)
		if !ok {
			return fmt.Errorf("expected a bool, got %T", value)
		}
		return writeCompactScalarForTest(buf, flag)
	}
	return fmt.Errorf("unsupported payload key type %d", kind)
}

func writeCompactUintTextForTest(buf *bytes.Buffer, value interface{}) error {
	text, ok := value.(string)
	if !ok {
		return fmt.Errorf("expected a decimal string, got %T", value)
	}
	number, ok := new(big.Int).SetString(text, 10)
	if !ok || text != number.String() {
		return fmt.Errorf("invalid decimal %q", text)
	}
	if number.Sign() < 0 {
		return fmt.Errorf("negative decimal %q", text)
	}
	if number.IsUint64() {
		writeCompactUintForTest(buf, number.Uint64())
		return nil
	}
	// Above uint64 the value travels as a minimal big-endian bin.
	return writeCompactScalarForTest(buf, number.Bytes())
}

// writeCompactUintForTest writes one msgpack uint in its shortest form, the form the
// engine's encoder uses: positive fixint, then uint8, uint16, uint32, uint64.
func writeCompactUintForTest(buf *bytes.Buffer, number uint64) {
	switch {
	case number <= 0x7f:
		buf.WriteByte(byte(number))
	case number <= 0xff:
		buf.WriteByte(0xcc)
		buf.WriteByte(byte(number))
	case number <= 0xffff:
		buf.WriteByte(0xcd)
		buf.WriteByte(byte(number >> 8))
		buf.WriteByte(byte(number))
	case number <= 0xffffffff:
		buf.WriteByte(0xce)
		buf.Write(compactUint32ForTest(uint32(number)))
	default:
		buf.WriteByte(0xcf)
		buf.Write(compactUint32ForTest(uint32(number >> 32)))
		buf.Write(compactUint32ForTest(uint32(number)))
	}
}

func compactUint32ForTest(number uint32) []byte {
	var raw [4]byte
	binary.BigEndian.PutUint32(raw[:], number)
	return raw[:]
}

func compactTestAddress(value interface{}) ([]byte, error) {
	text, ok := value.(string)
	if !ok {
		return nil, fmt.Errorf("expected an address string, got %T", value)
	}
	workchain, hash, ok := strings.Cut(text, ":")
	if !ok {
		return nil, fmt.Errorf("address %q has no workchain", text)
	}
	number, err := strconv.ParseInt(workchain, 10, 8)
	if err != nil {
		return nil, fmt.Errorf("address %q: %w", text, err)
	}
	raw, err := hex.DecodeString(hash)
	if err != nil {
		return nil, fmt.Errorf("address %q: %w", text, err)
	}
	if len(raw) != 32 {
		return nil, fmt.Errorf("address %q carries a %d byte hash", text, len(raw))
	}
	return append([]byte{byte(int8(number))}, raw...), nil
}

func compactTestBase64(value interface{}) ([]byte, error) {
	if raw, ok := value.([]byte); ok {
		return raw, nil
	}
	text, ok := value.(string)
	if !ok {
		return nil, fmt.Errorf("expected base64 text or bytes, got %T", value)
	}
	raw, err := base64.StdEncoding.DecodeString(text)
	if err != nil {
		return nil, fmt.Errorf("boc %q: %w", text, err)
	}
	return raw, nil
}

func compactTestHash(value interface{}, what string, decode func(string) ([]byte, error)) ([]byte, error) {
	text, ok := value.(string)
	if !ok {
		return nil, fmt.Errorf("expected a %s string, got %T", what, value)
	}
	raw, err := decode(text)
	if err != nil {
		return nil, fmt.Errorf("%s %q: %w", what, text, err)
	}
	if len(raw) != 32 {
		return nil, fmt.Errorf("%s %q carries %d bytes", what, text, len(raw))
	}
	return raw, nil
}

func writeCompactOtherForTest(buf *bytes.Buffer, value interface{}) error {
	switch value := value.(type) {
	case nil:
		buf.WriteByte(0xc0)
		return nil
	case map[string]interface{}:
		return writeCompactRecordForTest(buf, value)
	case []interface{}:
		writeCompactHeaderForTest(buf, 0x90, 0xdc, 0xdd, len(value))
		for index, item := range value {
			if err := writeCompactOtherForTest(buf, item); err != nil {
				return fmt.Errorf("element %d: %w", index, err)
			}
		}
		return nil
	default:
		return writeCompactScalarForTest(buf, value)
	}
}

func writeCompactScalarForTest(buf *bytes.Buffer, value interface{}) error {
	raw, err := msgpack.Marshal(value)
	if err != nil {
		return err
	}
	_, err = buf.Write(raw)
	return err
}

// addressHashForTest is a fixed 32 byte hash whose byte i is i.
func addressHashForTest() []byte {
	raw := make([]byte, 32)
	for i := range raw {
		raw[i] = byte(i)
	}
	return raw
}

func compactBodyForTest(t *testing.T, body string) []byte {
	t.Helper()
	raw, err := hex.DecodeString(strings.ReplaceAll(body, " ", ""))
	if err != nil {
		t.Fatalf("payload hex %q: %v", body, err)
	}
	return raw
}

// TestDecodeCompactTypes pins the byte form of every registry type to the text the router
// receives.
func TestDecodeCompactTypes(t *testing.T) {
	hash := addressHashForTest()
	hashHex := hex.EncodeToString(hash)
	addressHex := "00" + hashHex
	addressText := "0:" + strings.ToUpper(hashHex)
	for _, tc := range []struct {
		name string
		body string
		want map[string]interface{}
	}{
		{name: "empty", body: "80", want: map[string]interface{}{}},
		{name: "address workchain 0", body: "81 06 c4 21 " + addressHex, want: map[string]interface{}{"asset": addressText}},
		{name: "address workchain -1", body: "81 06 c4 21 ff" + hashHex, want: map[string]interface{}{"asset": "-1:" + strings.ToUpper(hashHex)}},
		{name: "boc", body: "81 0f c4 03 010203", want: map[string]interface{}{"custom_payload": "AQID"}},
		{name: "empty boc", body: "81 0f c4 00", want: map[string]interface{}{"custom_payload": ""}},
		{name: "uint from a positive fixint", body: "81 02 05", want: map[string]interface{}{"amount": "5"}},
		{name: "uint from uint8", body: "81 02 cc ff", want: map[string]interface{}{"amount": "255"}},
		{name: "uint from uint16", body: "81 02 cd 0100", want: map[string]interface{}{"amount": "256"}},
		{name: "uint from uint32", body: "81 02 ce 00010000", want: map[string]interface{}{"amount": "65536"}},
		{name: "uint from uint64", body: "81 02 cf 0100000000000000", want: map[string]interface{}{"amount": "72057594037927936"}},
		{name: "uint from uint64 max", body: "81 02 cf ffffffffffffffff", want: map[string]interface{}{"amount": "18446744073709551615"}},
		{name: "uint from a 256 bit bin", body: "81 24 c4 20 " + hashHex, want: map[string]interface{}{"initial_price_x96": new(big.Int).SetBytes(hash).String()}},
		{name: "hash", body: "81 47 c4 20 " + hashHex, want: map[string]interface{}{"parent_gasless_action": base64.StdEncoding.EncodeToString(hash)}},
		{name: "hash hex", body: "81 0a c4 20 " + hashHex, want: map[string]interface{}{"btc_txid": hashHex}},
		{name: "hash 0x", body: "81 07 c4 20 " + hashHex, want: map[string]interface{}{"asset_id": "0x" + hashHex}},
		{name: "string", body: "81 0d a5 68656c6c6f", want: map[string]interface{}{"comment": "hello"}},
		{name: "bool", body: "81 0e c3", want: map[string]interface{}{"comment_encrypted": true}},
		{name: "null typed value", body: "81 0d c0", want: map[string]interface{}{}},
		{name: "uint key in the uint8 form", body: "81 cc 7b c0", want: map[string]interface{}{}},
		{name: "nested record", body: "81 13 82 02 05 06 c4 21 " + addressHex, want: map[string]interface{}{
			"dex_incoming_transfer": map[string]interface{}{"amount": "5", "asset": addressText},
		}},
		{name: "null in a nested record", body: "81 13 82 02 05 0d c0", want: map[string]interface{}{
			"dex_incoming_transfer": map[string]interface{}{"amount": "5"},
		}},
		{name: "claimed rewards records", body: "81 0c 91 81 02 07", want: map[string]interface{}{
			"claimed_rewards": []interface{}{map[string]interface{}{"amount": "7"}},
		}},
		{name: "nil leg placeholder", body: "81 2c 92 c0 81 02 07", want: map[string]interface{}{
			"legs": []interface{}{nil, map[string]interface{}{"amount": "7"}},
		}},
		{name: "empty array", body: "81 0c 90", want: map[string]interface{}{"claimed_rewards": []interface{}{}}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, err := decodeCompactPayload(compactBodyForTest(t, tc.body))
			if err != nil {
				t.Fatalf("decodeCompactPayload: %v", err)
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("decoded %#v, want %#v", got, tc.want)
			}
		})
	}
}

// TestDecodeCompactErrors pins every rejection: a non-id key, a wrong value type, a wrong
// bin length, a hostile declared length and a malformed frame. An id this table does not
// know is NOT a rejection - see TestDecodeCompactSkipsUnknownIds.
func TestDecodeCompactErrors(t *testing.T) {
	hash31 := hex.EncodeToString(addressHashForTest()[:31])
	hash32 := hex.EncodeToString(addressHashForTest())
	for _, tc := range []struct {
		name string
		body string
		want string
	}{
		{name: "string key", body: "81 a4 64617461 80", want: "payload map key of type string is not a registry id"},
		{name: "string key in a nested record", body: "81 13 81 a4 64617461 80", want: "payload map key of type string is not a registry id"},
		{name: "short address", body: "81 06 c4 20 " + hash32, want: "expected a 33 byte address, got 32 bytes"},
		{name: "long address", body: "81 06 c4 22 00" + hash32 + "00", want: "expected a 33 byte address, got 34 bytes"},
		{name: "address as text", body: "81 06 a3 666f6f", want: "expected a 33 byte bin address, got string"},
		{name: "short hash", body: "81 47 c4 1f " + hash31, want: "expected a 32 byte hash, got 31 bytes"},
		{name: "hash as text", body: "81 47 a4 666f6f64", want: "expected a 32 byte bin hash, got string"},
		{name: "uint from text", body: "81 02 a1 35", want: "expected a msgpack uint or a bin, got string"},
		{name: "uint from a negative integer", body: "81 02 ff", want: "expected a msgpack uint or a bin, got int64"},
		{name: "string from an integer", body: "81 0d 05", want: "expected a msgpack string, got int64"},
		{name: "bool from text", body: "81 0e a4 74727565", want: "expected a msgpack bool, got string"},
		{name: "body is an array", body: "90", want: "expected a map"},
		{name: "body is nil", body: "c0", want: "expected a map, got nil"},
		{name: "trailing data", body: "80 00", want: "trailing data"},
		{name: "truncated value", body: "81 06", want: "map declares 1 entries but only 1 bytes remain"},
		{name: "truncated key", body: "81", want: "map declares 1 entries but only 0 bytes remain"},
		{name: "bin past the input", body: "81 06 c5 ff ff", want: "declares 65535 bytes but only 0 bytes remain"},
		{name: "unsupported code", body: "81 06 c1", want: "unsupported msgpack code 0xc1"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, err := decodeCompactPayload(compactBodyForTest(t, tc.body))
			if err == nil {
				t.Fatalf("body accepted, want an error containing %q", tc.want)
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Errorf("error = %q, want it to contain %q", err, tc.want)
			}
		})
	}
}

// recordBytesForTest builds one record (an id-keyed map) from the test encoder's bytes for
// `named` plus hand-written extra entry bytes: it is how a payload of a NEWER engine looks
// to this table - known fields plus ids it does not know.
func recordBytesForTest(t *testing.T, named map[string]interface{}, extra ...string) []byte {
	t.Helper()
	payload, err := EncodeCompactForTest(named)
	if err != nil {
		t.Fatalf("EncodeCompactForTest: %v", err)
	}
	if payload[0] != actionPayloadCodecMsgpack || payload[1] != 0x80|byte(len(named)) {
		t.Fatalf("unexpected encoded prefix %x", payload[:2])
	}
	var buf bytes.Buffer
	writeCompactHeaderForTest(&buf, 0x80, 0xde, 0xdf, len(named)+len(extra))
	buf.Write(payload[2:])
	for _, raw := range extra {
		buf.Write(compactBodyForTest(t, raw))
	}
	return buf.Bytes()
}

// TestDecodeCompactSkipsUnknownIds pins the cross-version rule: an id this build does not
// know has its value skipped - at the top level and inside records - and the rest of the
// action decodes, exactly as the previous reader ignored an unknown data key.
func TestDecodeCompactSkipsUnknownIds(t *testing.T) {
	hashHex := hex.EncodeToString(addressHashForTest())
	addressHex := "00" + hashHex
	addressText := "0:" + strings.ToUpper(hashHex)
	named := map[string]interface{}{
		"amount": "7", "asset": addressText, "comment": "hello",
	}
	bigBin := strings.Repeat("AB", 4096)
	// A record nested inside `legs` that itself carries an unknown id.
	leg := recordBytesForTest(t, map[string]interface{}{"amount": "8"}, "7d 80")
	var legBody bytes.Buffer
	writeCompactHeaderForTest(&legBody, 0x80, 0xde, 0xdf, 2)
	legBody.WriteByte(payloadKeyIDByName["comment"])
	if err := writeCompactScalarForTest(&legBody, "hello"); err != nil {
		t.Fatal(err)
	}
	legBody.WriteByte(payloadKeyIDByName["legs"])
	legBody.WriteByte(0x92)
	legBody.WriteByte(0xc0)
	legBody.Write(leg)

	for _, tc := range []struct {
		name string
		body []byte
		want map[string]interface{}
	}{
		{
			name: "unknown top-level id with a record value",
			body: recordBytesForTest(t, named, "7d 82 02 05 06 c4 21 "+addressHex),
			want: named,
		},
		{
			name: "unknown id 200 in the uint8 form with a bin value",
			body: recordBytesForTest(t, named, "cc c8 c5 10 00 "+bigBin),
			want: named,
		},
		{
			name: "unknown id inside a nested record",
			body: legBody.Bytes(),
			want: map[string]interface{}{"comment": "hello", "legs": []interface{}{nil, map[string]interface{}{"amount": "8"}}},
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, err := decodeCompactPayload(tc.body)
			if err != nil {
				t.Fatalf("decodeCompactPayload: %v", err)
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("decoded %#v, want %#v", got, tc.want)
			}
		})
	}

	// The same record still rejects a key that is not an id at all.
	if _, err := decodeCompactPayload(recordBytesForTest(t, named, `a4 64617461 80`)); err == nil {
		t.Error("a string key was accepted")
	}
}

// TestDecodeCompactDepthLimit pins the nesting bound: 32 nested arrays decode, 33 are
// rejected before the recursion can exhaust the stack.
func TestDecodeCompactDepthLimit(t *testing.T) {
	body := func(levels int) []byte {
		var buf bytes.Buffer
		buf.WriteByte(0x81)
		buf.WriteByte(payloadKeyIDByName["legs"])
		for i := 0; i < levels; i++ {
			buf.WriteByte(0x91)
		}
		buf.WriteByte(0xc0)
		return buf.Bytes()
	}
	if _, err := decodeCompactPayload(body(32)); err != nil {
		t.Errorf("32 nested arrays rejected: %v", err)
	}
	_, err := decodeCompactPayload(body(33))
	if err == nil {
		t.Fatal("33 nested arrays accepted")
	}
	if !strings.Contains(err.Error(), "nests deeper than 32 levels") {
		t.Errorf("error = %q, want the depth limit", err)
	}
}

// TestDecodeCompactTruncation checks that no prefix of a real payload panics or decodes:
// every truncated body is an error, the whole one decodes.
func TestDecodeCompactTruncation(t *testing.T) {
	payload, err := EncodeCompactForTest(map[string]interface{}{
		"amount": "7", "asset": "0:" + strings.Repeat("AB", 32), "comment": "hello",
		"nft_index":       "123456789012345678901234567890123456789",
		"legs":            []interface{}{nil, map[string]interface{}{"amount": "8", "asset": "0:" + strings.Repeat("CD", 32)}},
		"claimed_rewards": []interface{}{map[string]interface{}{"amount": "9"}},
	})
	if err != nil {
		t.Fatalf("EncodeCompactForTest: %v", err)
	}
	body := payload[1:]
	for length := 0; length < len(body); length++ {
		got, err := decodeCompactPayload(body[:length])
		if err == nil {
			t.Errorf("prefix of %d/%d bytes decoded to %#v", length, len(body), got)
		}
	}
	if _, err := decodeCompactPayload(body); err != nil {
		t.Fatalf("whole body rejected: %v", err)
	}
}

// TestDecodeCompactHostileLengths pins the allocation bound: a declared count or length
// the input cannot hold is rejected before anything is reserved, so a tiny crafted body
// cannot make the process allocate.
func TestDecodeCompactHostileLengths(t *testing.T) {
	for _, tc := range []struct {
		name string
		body string
		want string
	}{
		{name: "map32 with a four billion count", body: "df ff ff ff ff", want: "map declares 4294967295 entries but only 0 bytes remain"},
		{name: "map32 with a hundred count", body: "df 00 00 00 64", want: "map declares 100 entries but only 0 bytes remain"},
		{name: "array32 in a record", body: "81 2c dd ff ff ff ff", want: "payload key legs (id 44)"},
		{name: "array32 in an unknown id", body: "81 7d dd ff ff ff ff", want: "payload key id 125"},
		{name: "bin32 with a four billion length", body: "81 06 c6 ff ff ff ff", want: "declares 4294967295 bytes"},
		{name: "str32 with a four billion length", body: "81 0d db ff ff ff ff", want: "declares 4294967295 bytes"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var before, after runtime.MemStats
			runtime.GC()
			runtime.ReadMemStats(&before)
			_, err := decodeCompactPayload(compactBodyForTest(t, tc.body))
			runtime.ReadMemStats(&after)
			if err == nil {
				t.Fatalf("body accepted, want an error containing %q", tc.want)
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Errorf("error = %q, want it to contain %q", err, tc.want)
			}
			if allocated := after.TotalAlloc - before.TotalAlloc; allocated > 1<<20 {
				t.Errorf("decoding a %d byte body allocated %d bytes", len(tc.body), allocated)
			}
		})
	}
}

// TestEncodeCompactRoundTrip checks the test encoder and the decoder agree on every type:
// the encoder output of a named map decodes back to that same map.
func TestEncodeCompactRoundTrip(t *testing.T) {
	hashHex := hex.EncodeToString(addressHashForTest())
	named := map[string]interface{}{
		"asset":                 "0:" + strings.ToUpper(hashHex),
		"asset_in":              "-1:" + strings.ToUpper(hashHex),
		"custom_payload":        base64.StdEncoding.EncodeToString([]byte{1, 2, 3}),
		"amount":                "5",
		"nft_index":             "44539844875053679214736801996276540153801342669507034858554041330399478546432",
		"initial_price_x96":     "18446744073709551616",
		"order_seqno":           "0",
		"parent_gasless_action": base64.StdEncoding.EncodeToString(addressHashForTest()),
		"btc_txid":              hashHex,
		"asset_id":              "0x" + hashHex,
		"comment":               "hello",
		"comment_encrypted":     true,
		"claimed_rewards":       []interface{}{map[string]interface{}{"asset": "0:" + strings.ToUpper(hashHex), "amount": "7"}},
		"legs":                  []interface{}{nil, map[string]interface{}{"amount": "8", "asset": "0:" + strings.ToUpper(hashHex)}},
		"dex_incoming_transfer": map[string]interface{}{"amount": "9"},
		"accounts_added":        []interface{}{"0:" + strings.ToUpper(hashHex)},
		"peer_swaps":            []interface{}{map[string]interface{}{"amount_in": "10"}},
		"timestamp":             "1753800000",
		"worker_state":          "state",
	}
	payload, err := EncodeCompactForTest(named)
	if err != nil {
		t.Fatalf("EncodeCompactForTest: %v", err)
	}
	got, err := decodeCompactPayload(payload[1:])
	if err != nil {
		t.Fatalf("decodeCompactPayload: %v", err)
	}
	// The empty bin of a zero uint never reaches the wire: the decoder returns "0" and the
	// encoder writes uint 0, so the map round trips unchanged.
	if !reflect.DeepEqual(got, named) {
		t.Errorf("round trip = %#v\nwant %#v", got, named)
	}
}

// TestPayloadFieldTablesResolve checks the decoder tables against RawAction: every
// mapped key and every legs template must name a real field of its prefix.
func TestPayloadFieldTablesResolve(t *testing.T) {
	rawType := reflect.TypeOf(models.RawAction{})
	for key, name := range actionPayloadFieldNames {
		prefix := key[:strings.LastIndex(key, ".")]
		if strings.HasPrefix(name, ".") {
			// absolute target: the dotted name reuses a RawAction envelope field
			if !hasNamedField(rawType, name[1:]) {
				t.Errorf("actionPayloadFieldNames[%q] = %q does not resolve to a RawAction field", key, name[1:])
			}
			continue
		}
		if !hasNamedField(rawType, prefix+name) {
			t.Errorf("actionPayloadFieldNames[%q] = %q does not resolve to a RawAction field", key, prefix+name)
		}
	}
	for prefix, members := range actionPayloadLegFieldNames {
		for member, template := range members {
			field := prefix + fmt.Sprintf(template, 1)
			if !hasNamedField(rawType, field) {
				t.Errorf("actionPayloadLegFieldNames[%q][%q] = %q does not resolve", prefix, member, field)
			}
		}
	}
	for stored, prefixes := range actionDataPrefixes {
		for _, prefix := range prefixes {
			if !hasFieldPrefix(rawType, prefix) {
				t.Errorf("actionDataPrefixes[%q] prefix %q owns no RawAction field", stored, prefix)
			}
		}
	}
}

func hasFieldPrefix(typ reflect.Type, prefix string) bool {
	for i := range typ.NumField() {
		if strings.HasPrefix(typ.Field(i).Name, prefix) {
			return true
		}
	}
	return false
}

func hasNamedField(typ reflect.Type, name string) bool {
	for typ.Kind() == reflect.Ptr || typ.Kind() == reflect.Slice || typ.Kind() == reflect.Array {
		typ = typ.Elem()
	}
	if typ.Kind() != reflect.Struct {
		return false
	}
	for i := range typ.NumField() {
		field := typ.Field(i)
		if field.Name == name || hasNamedField(field.Type, name) {
			return true
		}
	}
	return false
}

func TestPayloadDecodesAndMarshalsMaxLayerZeroWidths(t *testing.T) {
	const maxUint128 = "340282366920938463463374607431768211455"
	payload, err := EncodeCompactForTest(map[string]interface{}{
		"dst_eid":    "4294967295",
		"nonce":      "18446744073709551615",
		"native_fee": maxUint128,
	})
	if err != nil {
		t.Fatal(err)
	}
	raw := &models.RawAction{Type: "layerzero_send"}
	if err := DecodeActionPayload(payload, raw); err != nil {
		t.Fatalf("DecodeActionPayload: %v", err)
	}
	if raw.LayerzeroPacketDstEid == nil || *raw.LayerzeroPacketDstEid != math.MaxUint32 {
		t.Fatalf("dst_eid = %v", raw.LayerzeroPacketDstEid)
	}
	if raw.LayerzeroPacketNonce == nil || *raw.LayerzeroPacketNonce != math.MaxUint64 {
		t.Fatalf("nonce = %v", raw.LayerzeroPacketNonce)
	}
	if raw.LayerzeroSendNativeFee == nil || raw.LayerzeroSendNativeFee.String() != maxUint128 {
		t.Fatalf("native_fee = %v", raw.LayerzeroSendNativeFee)
	}

	action, err := ParseRawAction(raw)
	if err != nil {
		t.Fatalf("ParseRawAction: %v", err)
	}
	encodedJSON, err := json.Marshal(action)
	if err != nil {
		t.Fatalf("json.Marshal: %v", err)
	}
	for _, number := range []string{"\"dst_eid\":4294967295", "\"nonce\":18446744073709551615", "\"native_fee\":" + maxUint128} {
		if !strings.Contains(string(encodedJSON), number) {
			t.Errorf("JSON missing numeric token %s: %s", number, encodedJSON)
		}
	}
}
