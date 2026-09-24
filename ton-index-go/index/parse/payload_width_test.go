package parse

import (
	"encoding/json"
	"fmt"
	"math"
	"math/big"
	"os"
	"reflect"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
)

func TestPayloadNumericWidthsMatchABI(t *testing.T) {
	checks := []struct {
		field, file, structure, abiField string
	}{
		{"MultisigCreateOrderCreatorIndex", "multisig.tolk", "MultisigNewOrder", "singer_index"},
		{"MultisigCreateOrderExpirationDate", "multisig.tolk", "MultisigNewOrder", "expiration_date"},
		{"MultisigApproveSignerIndex", "multisig.tolk", "MultisigApprove", "signer_index"},
		{"MultisigApproveExitCode", "multisig.tolk", "MultisigApproveRejected", "exit_code"},
		{"MultisigExecuteExpirationDate", "multisig.tolk", "MultisigExecute", "expiration_date"},
		{"MultisigExecuteApprovalsNum", "multisig.tolk", "MultisigExecute", "approvals_num"},
		{"JvaultStakePeriod", "jvault_payload.tolk", "JVaultStakePeriodPayload", "stake_period"},
		{"LayerzeroSendSendRequestId", "layerzero.tolk", "LzMdLzSend", "send_request_id"},
		{"LayerzeroSendNativeFee", "layerzero.tolk", "LzMdLzSendRef", "native_fee"},
		{"LayerzeroSendZroFee", "layerzero.tolk", "LzMdLzSendRef", "zro_fee"},
		{"LayerzeroPacketSrcEid", "layerzero.tolk", "LzPath", "src_eid"},
		{"LayerzeroPacketDstEid", "layerzero.tolk", "LzPath", "dst_eid"},
		{"LayerzeroPacketNonce", "layerzero.tolk", "LzPacket", "nonce"},
		{"LayerzeroDvnVerifyNonce", "layerzero.tolk", "UlnVerificationStatus", "nonce"},
		{"CocoonWorkerPayoutWorkerState", "cocoon.tolk", "CocoonWorkerProxyRequest", "state"},
		{"CocoonUnregisterProxySeqno", "cocoon.tolk", "CocoonUnregisterProxy", "seqno"},
	}

	checked := make(map[string]bool, len(checks))
	checked["ChangeDNSRecordFlags"] = true
	for _, check := range checks {
		checked[check.field] = true
		t.Run(check.field, func(t *testing.T) {
			abi := readABI(t, check.file)
			width, signed := abiFieldWidth(t, abi, check.structure, check.abiField)
			field, ok := reflect.TypeOf(models.RawAction{}).FieldByName(check.field)
			if !ok {
				t.Fatalf("RawAction.%s is missing", check.field)
			}
			goType := field.Type
			if goType.Kind() != reflect.Ptr {
				t.Fatalf("RawAction.%s has type %s, want pointer", check.field, goType)
			}
			goType = goType.Elem()
			if goType == reflect.TypeOf(big.Int{}) {
				if signed {
					t.Fatalf("RawAction.%s uses big.Int for signed ABI field", check.field)
				}
				return
			}
			goSigned := goType.Kind() >= reflect.Int && goType.Kind() <= reflect.Int64
			if !goSigned && goType.Kind() != reflect.Uint && (goType.Kind() < reflect.Uint8 || goType.Kind() > reflect.Uint64) {
				t.Fatalf("RawAction.%s has non-integer type %s", check.field, goType)
			}
			narrow := goType.Bits() < width || signed && !goSigned || !signed && goSigned && goType.Bits() <= width
			if narrow {
				t.Fatalf("RawAction.%s type %s is narrower than ABI %s%d", check.field, goType, map[bool]string{true: "int", false: "uint"}[signed], width)
			}
		})
	}
	for i := 0; i < reflect.TypeOf(models.RawAction{}).NumField(); i++ {
		field := reflect.TypeOf(models.RawAction{}).Field(i)
		if field.Type.Kind() != reflect.Ptr {
			continue
		}
		kind := field.Type.Elem().Kind()
		if kind >= reflect.Int && kind <= reflect.Int64 || kind == reflect.Uint || kind >= reflect.Uint8 && kind <= reflect.Uint64 {
			if !checked[field.Name] && field.Name != "Opcode" {
				t.Errorf("numeric RawAction.%s has no ABI width check", field.Name)
			}
		}
	}

	// ParseDns reads these two DNS flags with fetch_ulong(8), then emits the
	// decimal value into ChangeDNSRecordFlags.
	dns := readRepoFile(t, "ton-index-worker/ton-mch-engine/src/parse/ParseDns.cpp")
	flagReads := regexp.MustCompile(`vf\.emplace_back\("flags", Value::make_int64\(static_cast<std::int64_t>\(vs\.fetch_ulong\(([0-9]+)\)\)\)\);`).FindAllStringSubmatch(dns, -1)
	if len(flagReads) != 2 || flagReads[0][1] != "8" || flagReads[1][1] != "8" {
		t.Fatalf("DNS flag width declarations = %v, want two 8-bit reads", flagReads)
	}
	field, _ := reflect.TypeOf(models.RawAction{}).FieldByName("ChangeDNSRecordFlags")
	if field.Type.Elem().Bits() <= 8 {
		t.Fatalf("RawAction.ChangeDNSRecordFlags type %s is narrower than DNS uint8", field.Type)
	}
}

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

func abiFieldWidth(t *testing.T, source, structure, field string) (int, bool) {
	t.Helper()
	decl := regexp.MustCompile(`(?s)struct(?:\s+\([^)]*\))?\s+` + regexp.QuoteMeta(structure) + `\s*\{([^}]*)\}`).FindStringSubmatch(source)
	if len(decl) != 2 {
		t.Fatalf("ABI struct %s not found", structure)
	}
	line := regexp.MustCompile(`(?m)^\s*` + regexp.QuoteMeta(field) + `:\s*(u?int)([0-9]+)\b`).FindStringSubmatch(decl[1])
	if len(line) != 3 {
		t.Fatalf("ABI field %s.%s not found", structure, field)
	}
	width, err := strconv.Atoi(line[2])
	if err != nil {
		t.Fatal(err)
	}
	return width, line[1] == "int"
}

func readABI(t *testing.T, name string) string {
	t.Helper()
	return readRepoFile(t, "ton-index-worker/ton-abi/abi/"+name)
}

func readRepoFile(t *testing.T, name string) string {
	t.Helper()
	data, err := os.ReadFile("../../../" + name)
	if err != nil {
		t.Fatalf("read %s: %v", name, err)
	}
	return string(data)
}
