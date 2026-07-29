package emulated

import (
	"fmt"
	"math"
	"reflect"
	"strconv"

	"github.com/vmihailenco/msgpack/v5"
	"github.com/vmihailenco/msgpack/v5/msgpcode"
)

// Wire tolerance for the emulated-trace payloads in redis.
//
// Python's writer stringifies every integer outside a small top-level exclusion
// set (convert_numerics_to_strings), so ints nested in composites arrive as
// msgpack strings while the models here declare them numeric - and the reverse
// holds for a writer that emits natural types into the 169 *string fields.
// Either mismatch is a hard Unmarshal error, which drops the whole trace from
// the pending endpoints.
//
// Registering flexible decoders for the ten integer kinds and for string makes
// every field of every model accept both encodings, so no writer has to
// reproduce the other's stringify rules. Values that already decode keep
// decoding to exactly the same result; only previously-failing inputs change.
//
// msgpack.Register is keyed on reflect.Type and consulted before the stock
// decoders (both directly and through *T). Registration must happen in init():
// per-type and per-struct decoders are memoized on first use, and the package
// decodes nothing before main.
func init() {
	for _, v := range []interface{}{int(0), int8(0), int16(0), int32(0), int64(0)} {
		msgpack.Register(v, nil, decodeFlexInt)
	}
	for _, v := range []interface{}{uint(0), uint8(0), uint16(0), uint32(0), uint64(0)} {
		msgpack.Register(v, nil, decodeFlexUint)
	}
	msgpack.Register("", nil, decodeFlexString)
}

func decodeFlexInt(d *msgpack.Decoder, v reflect.Value) error {
	c, err := d.PeekCode()
	if err != nil {
		return err
	}
	if msgpcode.IsString(c) || msgpcode.IsBin(c) {
		s, err := d.DecodeString()
		if err != nil {
			return err
		}
		n, err := parseFlexInt(s)
		if err != nil {
			return err
		}
		v.SetInt(n)
		return nil
	}
	n, err := d.DecodeInt64()
	if err != nil {
		return err
	}
	v.SetInt(n)
	return nil
}

func decodeFlexUint(d *msgpack.Decoder, v reflect.Value) error {
	c, err := d.PeekCode()
	if err != nil {
		return err
	}
	if msgpcode.IsString(c) || msgpcode.IsBin(c) {
		s, err := d.DecodeString()
		if err != nil {
			return err
		}
		n, err := parseFlexUint(s)
		if err != nil {
			return err
		}
		v.SetUint(n)
		return nil
	}
	n, err := d.DecodeUint64()
	if err != nil {
		return err
	}
	v.SetUint(n)
	return nil
}

func decodeFlexString(d *msgpack.Decoder, v reflect.Value) error {
	c, err := d.PeekCode()
	if err != nil {
		return err
	}
	switch {
	case msgpcode.IsFixedNum(c), c == msgpcode.Int8, c == msgpcode.Int16, c == msgpcode.Int32, c == msgpcode.Int64:
		n, err := d.DecodeInt64()
		if err != nil {
			return err
		}
		v.SetString(strconv.FormatInt(n, 10))
	case c == msgpcode.Uint8, c == msgpcode.Uint16, c == msgpcode.Uint32, c == msgpcode.Uint64:
		n, err := d.DecodeUint64()
		if err != nil {
			return err
		}
		v.SetString(strconv.FormatUint(n, 10))
	case c == msgpcode.Float, c == msgpcode.Double:
		f, err := d.DecodeFloat64()
		if err != nil {
			return err
		}
		v.SetString(strconv.FormatFloat(f, 'g', -1, 64))
	default:
		s, err := d.DecodeString()
		if err != nil {
			return err
		}
		v.SetString(s)
	}
	return nil
}

// parseFlexInt accepts a decimal integer, and as a fallback the forms Python's
// str(float) produces for an integer-valued float ("1.0", "1e+16"). A value
// with a fractional part is rejected rather than rounded - in a field declared
// integer it means the writer sent something else.
func parseFlexInt(s string) (int64, error) {
	if n, err := strconv.ParseInt(s, 10, 64); err == nil {
		return n, nil
	}
	f, err := strconv.ParseFloat(s, 64)
	// The upper bound is >= and not >: float64 cannot represent MaxInt64, so
	// the constant rounds up to 2^63 - and "9223372036854775807.0" parses to
	// that same 2^63, which int64(f) cannot hold. Rejecting the rounded
	// boundary value is deliberate. MinInt64 is exact, so its bound is strict.
	if err != nil || math.Trunc(f) != f || f < math.MinInt64 || f >= math.MaxInt64 {
		return 0, fmt.Errorf("msgpack: %q is not an int64", s)
	}
	return int64(f), nil
}

func parseFlexUint(s string) (uint64, error) {
	if n, err := strconv.ParseUint(s, 10, 64); err == nil {
		return n, nil
	}
	f, err := strconv.ParseFloat(s, 64)
	// >= for the same reason as parseFlexInt: float64(MaxUint64) rounds up to
	// 2^64, and "18446744073709551615.0" parses to it.
	if err != nil || math.Trunc(f) != f || f < 0 || f >= math.MaxUint64 {
		return 0, fmt.Errorf("msgpack: %q is not a uint64", s)
	}
	return uint64(f), nil
}
