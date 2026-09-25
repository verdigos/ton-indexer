package parse

import (
	"bytes"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"math/big"
	"strconv"
	"strings"

	"github.com/vmihailenco/msgpack/v5"
	"github.com/vmihailenco/msgpack/v5/msgpcode"
)

const (
	// compactMaxDepth bounds the nesting of records and arrays: a deeper payload is
	// rejected before the recursion can exhaust the stack.
	compactMaxDepth = 32
)

// The compact payload (codec byte 1) is ONE msgpack map whose keys are registry ids and
// whose values are typed by those ids, at every nesting level. decodeCompactPayload turns
// such a body (everything after the codec byte) into the named map the routing layer and
// setPayloadValue consume, in the text forms that layer expects: decimal strings for
// integers, `W:HEX` for addresses, standard padded base64 for BOCs and base64 hashes,
// 64 lowercase hex digits (with or without the `0x` prefix) for the hash forms, msgpack
// str and bool as is.
//
// An id this table does not know (the engine is newer) has its value skipped and the rest
// of the action decoded. A key that is not an integer id at all is rejected.
func decodeCompactPayload(body []byte) (map[string]interface{}, error) {
	reader := bytes.NewReader(body)
	d := &compactDecoder{dec: msgpack.NewDecoder(reader), in: reader}
	values, err := d.record()
	if err != nil {
		return nil, err
	}
	// A payload is one value: anything left is trailing data.
	if left := d.in.Len(); left != 0 {
		return nil, fmt.Errorf("trailing data: %d bytes after the payload map", left)
	}
	return values, nil
}

// compactDecoder walks the body. The msgpack decoder reads straight from `in` (it buffers
// only a reader that cannot seek back a byte), so `in.Len()` is always the exact number of
// bytes left, and every declared count or length is checked against it before a map, an
// array or a byte slice is allocated.
type compactDecoder struct {
	dec   *msgpack.Decoder
	in    *bytes.Reader
	depth int
}

// record decodes one id-keyed map: the payload body or a nested record (`legs[]` element,
// claim reward, swap record, `peer_swaps[]`, `vault_excesses[]`, any nested map anywhere).
// A later entry of a duplicated id wins, the way Go map assignment resolves it.
func (d *compactDecoder) record() (map[string]interface{}, error) {
	n, err := d.dec.DecodeMapLen()
	if err != nil {
		return nil, fmt.Errorf("expected a map: %w", err)
	}
	if n < 0 {
		return nil, fmt.Errorf("expected a map, got nil")
	}
	// A map entry is at least a key and a value, so two bytes.
	if err := d.checkCount(n, 2, "map"); err != nil {
		return nil, err
	}
	values := make(map[string]interface{}, n)
	for i := 0; i < n; i++ {
		key, err := d.scalar()
		if err != nil {
			return nil, fmt.Errorf("map key: %w", err)
		}
		id, ok := compactKeyID(key)
		if !ok {
			return nil, fmt.Errorf("payload map key of type %T is not a registry id", key)
		}
		info := payloadKeys[id]
		if info.Name == "" {
			// An id this build does not know: a newer engine wrote it. Its value is
			// skipped so the rest of the action still decodes.
			if err := d.skip(); err != nil {
				return nil, fmt.Errorf("payload key id %d: %w", id, err)
			}
			continue
		}
		value, err := d.value(info)
		if err != nil {
			return nil, fmt.Errorf("payload key %s (id %d): %w", info.Name, id, err)
		}
		// A null is an absent key at every level, so it never enters the map; only array
		// elements keep a nil (the positional `legs` placeholders).
		if value == nil {
			continue
		}
		values[info.Name] = value
	}
	return values, nil
}

// checkCount rejects a declared entry count or byte length the bytes left cannot hold:
// `perEntry` is the minimum size of one entry (a map entry needs two bytes, an array
// element and a byte length need one).
func (d *compactDecoder) checkCount(count, perEntry int, what string) error {
	left := d.in.Len()
	if count > left/perEntry {
		return fmt.Errorf("%s declares %d entries but only %d bytes remain", what, count, left)
	}
	return nil
}

// enter counts one nested container and refuses one deeper than the limit.
func (d *compactDecoder) enter(what string) error {
	if d.depth >= compactMaxDepth {
		return fmt.Errorf("%s nests deeper than %d levels", what, compactMaxDepth)
	}
	d.depth++
	return nil
}

// compactKeyID reads a registry id from a decoded msgpack key: any non-negative integer
// up to 255, whatever msgpack width the encoder chose.
func compactKeyID(key interface{}) (uint8, bool) {
	number, ok := msgpackUint(key)
	if !ok || number > 255 {
		return 0, false
	}
	return uint8(number), true
}

// msgpackUint reads a non-negative integer of any msgpack width: the loose interface
// decode yields int64 for every signed code and uint64 for every unsigned one.
func msgpackUint(value interface{}) (uint64, bool) {
	switch value := value.(type) {
	case int64:
		if value < 0 {
			return 0, false
		}
		return uint64(value), true
	case uint64:
		return value, true
	}
	return 0, false
}

// value decodes the value of one registry key. A null is an absent key for every type.
func (d *compactDecoder) value(info payloadKeyInfo) (interface{}, error) {
	if info.Type == payloadTypeOther {
		return d.container()
	}
	value, err := d.scalar()
	if err != nil {
		return nil, fmt.Errorf("decode value: %w", err)
	}
	return compactTypedText(value, info)
}

// compactTypedText renders one decoded value in the text form the routing layer expects.
func compactTypedText(value interface{}, info payloadKeyInfo) (interface{}, error) {
	if value == nil {
		return nil, nil
	}
	switch info.Type {
	case payloadTypeAddress:
		raw, err := compactBin(value, 33, "address")
		if err != nil {
			return nil, err
		}
		// Workchain is a signed int8, the hash 32 bytes; the text is the frozen form the
		// API already reports.
		return strconv.Itoa(int(int8(raw[0]))) + ":" + strings.ToUpper(hex.EncodeToString(raw[1:])), nil
	case payloadTypeBoc:
		raw, err := compactBin(value, 0, "boc")
		if err != nil {
			return nil, err
		}
		return base64.StdEncoding.EncodeToString(raw), nil
	case payloadTypeUint:
		return compactUintText(value)
	case payloadTypeHash, payloadTypeHashHex, payloadTypeHash0x:
		raw, err := compactBin(value, 32, "hash")
		if err != nil {
			return nil, err
		}
		switch info.Type {
		case payloadTypeHash:
			return base64.StdEncoding.EncodeToString(raw), nil
		case payloadTypeHashHex:
			return hex.EncodeToString(raw), nil
		}
		return "0x" + hex.EncodeToString(raw), nil
	case payloadTypeString:
		text, ok := value.(string)
		if !ok {
			return nil, fmt.Errorf("expected a msgpack string, got %T", value)
		}
		return text, nil
	case payloadTypeBool:
		flag, ok := value.(bool)
		if !ok {
			return nil, fmt.Errorf("expected a msgpack bool, got %T", value)
		}
		return flag, nil
	}
	return nil, fmt.Errorf("unsupported payload key type %d", info.Type)
}

// compactBin reads a msgpack bin value and, when `size` is non-zero, requires exactly
// that many bytes.
func compactBin(value interface{}, size int, what string) ([]byte, error) {
	raw, ok := value.([]byte)
	if !ok {
		if size == 0 {
			return nil, fmt.Errorf("expected a bin %s, got %T", what, value)
		}
		return nil, fmt.Errorf("expected a %d byte bin %s, got %T", size, what, value)
	}
	if size != 0 && len(raw) != size {
		return nil, fmt.Errorf("expected a %d byte %s, got %d bytes", size, what, len(raw))
	}
	return raw, nil
}

// compactUintText renders a uint value as its decimal text: a msgpack integer of any
// width, or a big-endian bin for the decimals above uint64 (`order_seqno`,
// `initial_price_x96`).
func compactUintText(value interface{}) (interface{}, error) {
	if raw, ok := value.([]byte); ok {
		return new(big.Int).SetBytes(raw).String(), nil
	}
	number, ok := msgpackUint(value)
	if !ok {
		return nil, fmt.Errorf("expected a msgpack uint or a bin, got %T", value)
	}
	return strconv.FormatUint(number, 10), nil
}

// scalar decodes one non-container value. A str or bin value has its declared length
// checked against the bytes left before its bytes are allocated; the msgpack codes that
// carry no length are bounded by construction (at most nine bytes).
func (d *compactDecoder) scalar() (interface{}, error) {
	code, err := d.dec.PeekCode()
	if err != nil {
		return nil, err
	}
	switch {
	case code == msgpcode.Nil || msgpcode.IsFixedNum(code) || msgpcode.IsFixedString(code) ||
		code == msgpcode.True || code == msgpcode.False ||
		code == msgpcode.Uint8 || code == msgpcode.Uint16 || code == msgpcode.Uint32 || code == msgpcode.Uint64 ||
		code == msgpcode.Int8 || code == msgpcode.Int16 || code == msgpcode.Int32 || code == msgpcode.Int64:
		return d.dec.DecodeInterfaceLoose()
	case isStrCode(code):
		raw, err := d.lengthPrefixed()
		if err != nil {
			return nil, err
		}
		return string(raw), nil
	case isBinCode(code):
		return d.lengthPrefixed()
	}
	return nil, fmt.Errorf("unsupported msgpack code 0x%02x", code)
}

// lengthPrefixed consumes a str or bin header and returns its bytes. The declared length
// is rejected when the bytes left cannot hold it, so nothing larger than the input is ever
// allocated.
func (d *compactDecoder) lengthPrefixed() ([]byte, error) {
	length, err := d.dec.DecodeBytesLen()
	if err != nil {
		return nil, err
	}
	if length > d.in.Len() {
		return nil, fmt.Errorf("value declares %d bytes but only %d bytes remain", length, d.in.Len())
	}
	raw := make([]byte, length)
	if err := d.dec.ReadFull(raw); err != nil {
		return nil, fmt.Errorf("value body: %w", err)
	}
	return raw, nil
}

// container decodes an `other` value: a map becomes a record whose keys are translated and
// whose values are typed by their own key, an array keeps its order with its records
// translated and its null placeholders kept (the positional `legs` slots), and every
// scalar passes through unchanged.
func (d *compactDecoder) container() (interface{}, error) {
	code, err := d.dec.PeekCode()
	if err != nil {
		return nil, err
	}
	switch {
	case isMapCode(code):
		if err := d.enter("payload"); err != nil {
			return nil, err
		}
		values, err := d.record()
		d.depth--
		return values, err
	case isArrayCode(code):
		n, err := d.dec.DecodeArrayLen()
		if err != nil {
			return nil, fmt.Errorf("expected an array: %w", err)
		}
		if n < 0 {
			return nil, nil
		}
		// An element is at least one byte, so the count the bytes left cannot hold is
		// rejected before the slice is reserved.
		if err := d.checkCount(n, 1, "array"); err != nil {
			return nil, err
		}
		if err := d.enter("payload"); err != nil {
			return nil, err
		}
		items := make([]interface{}, n)
		for i := range items {
			item, err := d.container()
			if err != nil {
				d.depth--
				return nil, fmt.Errorf("array element %d: %w", i, err)
			}
			items[i] = item
		}
		d.depth--
		return items, nil
	}
	return d.scalar()
}

// skip consumes one value and keeps nothing: it serves an id this build does not know. It
// obeys the same count, length and depth bounds as the decoder.
func (d *compactDecoder) skip() error {
	code, err := d.dec.PeekCode()
	if err != nil {
		return err
	}
	switch {
	case isMapCode(code):
		n, err := d.dec.DecodeMapLen()
		if err != nil {
			return fmt.Errorf("expected a map: %w", err)
		}
		if n < 0 {
			return nil
		}
		if err := d.checkCount(n, 2, "map"); err != nil {
			return err
		}
		if err := d.enter("payload"); err != nil {
			return err
		}
		for i := 0; i < n; i++ {
			if _, err := d.scalar(); err != nil {
				d.depth--
				return fmt.Errorf("map key: %w", err)
			}
			if err := d.skip(); err != nil {
				d.depth--
				return err
			}
		}
		d.depth--
		return nil
	case isArrayCode(code):
		n, err := d.dec.DecodeArrayLen()
		if err != nil {
			return fmt.Errorf("expected an array: %w", err)
		}
		if n < 0 {
			return nil
		}
		if err := d.checkCount(n, 1, "array"); err != nil {
			return err
		}
		if err := d.enter("payload"); err != nil {
			return err
		}
		for i := 0; i < n; i++ {
			if err := d.skip(); err != nil {
				d.depth--
				return fmt.Errorf("array element %d: %w", i, err)
			}
		}
		d.depth--
		return nil
	}
	_, err = d.scalar()
	return err
}

func isMapCode(code byte) bool {
	return msgpcode.IsFixedMap(code) || code == msgpcode.Map16 || code == msgpcode.Map32
}

func isArrayCode(code byte) bool {
	return msgpcode.IsFixedArray(code) || code == msgpcode.Array16 || code == msgpcode.Array32
}

func isStrCode(code byte) bool {
	return msgpcode.IsFixedString(code) || code == msgpcode.Str8 || code == msgpcode.Str16 || code == msgpcode.Str32
}

func isBinCode(code byte) bool {
	return code == msgpcode.Bin8 || code == msgpcode.Bin16 || code == msgpcode.Bin32
}
