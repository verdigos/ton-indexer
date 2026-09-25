package parse

import (
	"encoding/json"
	"fmt"
	"math"
	"reflect"
	"strings"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
)

// TestPayloadFieldTablesResolve checks the decoder tables against RawAction: every
// mapped key and every legs template must name a real field of its prefix.
func TestPayloadFieldTablesResolve(t *testing.T) {
	rawType := reflect.TypeOf(models.RawAction{})
	for key, name := range actionPayloadFieldNames {
		prefix := key[:strings.LastIndex(key, ".")]
		if strings.HasPrefix(name, ".") {
			// absolute target: the moved row columns reuse an envelope field
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
	for i := 0; i < typ.NumField(); i++ {
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
	for i := 0; i < typ.NumField(); i++ {
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
