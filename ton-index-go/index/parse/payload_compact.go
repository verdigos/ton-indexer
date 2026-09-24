package parse

import (
	"bytes"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"io"
	"math/big"
	"strconv"
	"strings"

	"github.com/vmihailenco/msgpack/v5"
	"github.com/vmihailenco/msgpack/v5/msgpcode"
)

// The compact payload (codec byte 1) is ONE msgpack map whose keys are registry ids and
// whose values are typed by those ids, at every nesting level. decodeCompactPayload turns
// such a body (everything after the codec byte) into the named map the routing layer and
// setPayloadValue consume, in EXACTLY the text forms the previous name-keyed payload
// carried: decimal strings for integers, `W:HEX` for addresses, standard padded base64
// for BOCs and base64 hashes, 64 lowercase hex digits (with or without the `0x` prefix)
// for the hash forms, msgpack str and bool as is.
func decodeCompactPayload(body []byte) (map[string]interface{}, error) {
	dec := msgpack.NewDecoder(bytes.NewReader(body))
	values, err := decodeCompactRecord(dec)
	if err != nil {
		return nil, err
	}
	if _, err := dec.DecodeInterface(); err != io.EOF {
		return nil, fmt.Errorf("trailing data")
	}
	return values, nil
}

// decodeCompactRecord decodes one id-keyed map: the payload body or a nested record
// (`legs[]` element, claim reward, swap record, `peer_swaps[]`, `vault_excesses[]`, any
// nested map anywhere). A key must be a registry id in any msgpack integer width; a
// later entry of a duplicated id wins, the way Go map assignment resolves it.
func decodeCompactRecord(dec *msgpack.Decoder) (map[string]interface{}, error) {
	n, err := dec.DecodeMapLen()
	if err != nil {
		return nil, fmt.Errorf("expected a map: %w", err)
	}
	if n < 0 {
		return nil, fmt.Errorf("expected a map, got nil")
	}
	values := make(map[string]interface{}, n)
	for i := 0; i < n; i++ {
		key, err := dec.DecodeInterface()
		if err != nil {
			return nil, fmt.Errorf("map key: %w", err)
		}
		id, info, err := compactKeyInfo(key)
		if err != nil {
			return nil, err
		}
		value, err := decodeCompactValue(dec, info)
		if err != nil {
			return nil, fmt.Errorf("payload key %s (id %d): %w", info.Name, id, err)
		}
		values[info.Name] = value
	}
	return values, nil
}

// compactKeyInfo resolves a decoded msgpack map key to its registry id and entry. A
// retired entry still resolves: its name routes to no field and is dropped downstream,
// the way the retired name behaved before.
func compactKeyInfo(key interface{}) (uint8, payloadKeyInfo, error) {
	id, ok := compactKeyID(key)
	if !ok {
		return 0, payloadKeyInfo{}, fmt.Errorf("payload map key %s is not a registry id; the old name-keyed payload format is not supported", compactKeyText(key))
	}
	info := payloadKeys[id]
	if info.Name == "" {
		return id, info, fmt.Errorf("payload key id %d is not in the registry", id)
	}
	return id, info, nil
}

// compactKeyID reads a registry id from a decoded msgpack key: any non-negative integer
// up to 255, whatever msgpack width the encoder chose (positive fixint, uint8, uint16
// and larger are all accepted).
func compactKeyID(key interface{}) (uint8, bool) {
	switch key := key.(type) {
	case int8:
		return uint8(key), key >= 0
	case int16:
		return uint8(key), key >= 0 && key <= 255
	case int32:
		return uint8(key), key >= 0 && key <= 255
	case int64:
		return uint8(key), key >= 0 && key <= 255
	case uint8:
		return key, true
	case uint16:
		return uint8(key), key <= 255
	case uint32:
		return uint8(key), key <= 255
	case uint64:
		return uint8(key), key <= 255
	}
	return 0, false
}

// decodeCompactValue decodes the value of one registry key. A null is an absent key for
// every type, exactly as the previous reader skipped a null.
func decodeCompactValue(dec *msgpack.Decoder, info payloadKeyInfo) (interface{}, error) {
	if info.Type == payloadTypeOther {
		return decodeCompactContainer(dec)
	}
	value, err := dec.DecodeInterface()
	if err != nil {
		return nil, fmt.Errorf("decode value: %w", err)
	}
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
	case payloadTypeHash:
		raw, err := compactBin(value, 32, "hash")
		if err != nil {
			return nil, err
		}
		return base64.StdEncoding.EncodeToString(raw), nil
	case payloadTypeHashHex:
		raw, err := compactBin(value, 32, "hash")
		if err != nil {
			return nil, err
		}
		return hex.EncodeToString(raw), nil
	case payloadTypeHash0x:
		raw, err := compactBin(value, 32, "hash")
		if err != nil {
			return nil, err
		}
		return "0x" + hex.EncodeToString(raw), nil
	case payloadTypeString:
		text, ok := value.(string)
		if !ok {
			return nil, fmt.Errorf("expected a msgpack string, got %s", compactTypeName(value))
		}
		return text, nil
	case payloadTypeBool:
		flag, ok := value.(bool)
		if !ok {
			return nil, fmt.Errorf("expected a msgpack bool, got %s", compactTypeName(value))
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
		return nil, fmt.Errorf("expected a %d byte bin %s, got %s", size, what, compactTypeName(value))
	}
	if size != 0 && len(raw) != size {
		return nil, fmt.Errorf("expected a %d byte %s, got %d bytes", size, what, len(raw))
	}
	return raw, nil
}

// compactUintText renders a uint value as its decimal text: a msgpack integer of any
// width (a positive fixint decodes to int8, so a non-negative int is a valid uint too),
// or a big-endian bin for the decimals above uint64 (`nft_index`, `order_seqno`,
// `initial_price_x96`).
func compactUintText(value interface{}) (interface{}, error) {
	if raw, ok := value.([]byte); ok {
		return new(big.Int).SetBytes(raw).String(), nil
	}
	var number uint64
	switch value := value.(type) {
	case int8:
		if value < 0 {
			return nil, fmt.Errorf("negative msgpack integer %d for a uint key", value)
		}
		number = uint64(value)
	case int16:
		if value < 0 {
			return nil, fmt.Errorf("negative msgpack integer %d for a uint key", value)
		}
		number = uint64(value)
	case int32:
		if value < 0 {
			return nil, fmt.Errorf("negative msgpack integer %d for a uint key", value)
		}
		number = uint64(value)
	case int64:
		if value < 0 {
			return nil, fmt.Errorf("negative msgpack integer %d for a uint key", value)
		}
		number = uint64(value)
	case uint8:
		number = uint64(value)
	case uint16:
		number = uint64(value)
	case uint32:
		number = uint64(value)
	case uint64:
		number = value
	default:
		return nil, fmt.Errorf("expected a msgpack uint or a bin, got %s", compactTypeName(value))
	}
	return strconv.FormatUint(number, 10), nil
}

// decodeCompactContainer decodes an `other` value: a map becomes a record whose keys are
// translated and whose values are typed by their own key, an array keeps its order with
// its records translated and its null placeholders kept (the positional `legs` slots),
// and every scalar passes through unchanged.
func decodeCompactContainer(dec *msgpack.Decoder) (interface{}, error) {
	code, err := dec.PeekCode()
	if err != nil {
		return nil, err
	}
	switch {
	case msgpcode.IsFixedMap(code) || code == msgpcode.Map16 || code == msgpcode.Map32:
		return decodeCompactRecord(dec)
	case msgpcode.IsFixedArray(code) || code == msgpcode.Array16 || code == msgpcode.Array32:
		n, err := dec.DecodeArrayLen()
		if err != nil {
			return nil, fmt.Errorf("expected an array: %w", err)
		}
		if n < 0 {
			return nil, nil
		}
		// The declared length is untrusted; the read below fails long before a bogus
		// length can be honoured, so only a bounded slice is reserved up front.
		items := make([]interface{}, 0, min(n, 1024))
		for i := 0; i < n; i++ {
			item, err := decodeCompactContainer(dec)
			if err != nil {
				return nil, fmt.Errorf("array element %d: %w", i, err)
			}
			items = append(items, item)
		}
		return items, nil
	}
	return dec.DecodeInterface()
}

func compactTypeName(value interface{}) string {
	if value == nil {
		return "nil"
	}
	return fmt.Sprintf("%T", value)
}

// compactKeyText names a rejected key: a string key of the old payload gets its quotes,
// an out of range integer its number.
func compactKeyText(key interface{}) string {
	if text, ok := key.(string); ok {
		return strconv.Quote(text)
	}
	return fmt.Sprintf("%v", key)
}
