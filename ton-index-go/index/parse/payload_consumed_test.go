package parse

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"reflect"
	"sort"
	"strings"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
)

// consumedPayloadKeyAllowlist lists `<stored_type>.<key>` entries the reader may ignore,
// each with its one-line reason. Every other payload key of the frozen baseline must
// change either the API JSON or the collected address list when it is dropped.
var consumedPayloadKeyAllowlist = map[string]string{
	"dedust_v2_claim_fees.is_refund":                   "owner decision: keep, the API field is never assigned",
	"dedust_v2_claim_reward.is_refund":                 "owner decision: keep, the API field is never assigned",
	"dex_withdraw_liquidity.is_refund":                 "owner decision: keep, the API field is never assigned",
	"evaa_withdraw.is_ton":                             "no API field: ActionDetailsEvaaWithdraw has no is_ton",
	"dedust_v2_claim_fees.legs[0].dex_wallet":          "aligned legs layout of the five DEX types",
	"dedust_v2_claim_fees.legs[1].dex_wallet":          "aligned legs layout of the five DEX types",
	"dedust_v2_claim_reward.legs[0].dex_wallet":        "aligned legs layout of the five DEX types",
	"dex_withdraw_liquidity.legs[0].dex_jetton_wallet": "aligned legs layout of the five DEX types",
	"dex_withdraw_liquidity.legs[1].dex_jetton_wallet": "aligned legs layout of the five DEX types",
}

type consumedFixtureRow struct {
	FixtureID string `json:"fixture_id"`
	ActionID  string `json:"action_id"`
	Type      string `json:"type"`
	Payload   string `json:"payload_hex"`
}

// TestPayloadConsumedKeys walks every payload key of the frozen baseline (top level,
// nested leaves and each `legs[i].<member>`, one key per leg slot) and drops it from a
// decoded RawAction copy: a key is consumed when the API JSON or the collected
// addresses change without it, or when the collector reads the field it resolved to.
// Keys that resolve to no RawAction field are unconsumed by construction. A key whose
// value is an empty list or map is invisible to this test, so it is counted separately
// instead of failing. Keys whose value is a record list (such as `claimed_rewards`) are
// checked as one key, the way the decoder assigns them.
func TestPayloadConsumedKeys(t *testing.T) {
	f, err := os.Open("testdata/payload_contract.jsonl.gz")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	z, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	defer z.Close()

	scan := newConsumedScan()
	rows := 0
	scanner := bufio.NewScanner(z)
	scanner.Buffer(nil, 16<<20)
	for scanner.Scan() {
		var row consumedFixtureRow
		if err := json.Unmarshal(scanner.Bytes(), &row); err != nil {
			t.Fatalf("row %d: %v", rows+1, err)
		}
		rows++
		t.Run(row.FixtureID+"/"+row.ActionID+"/"+row.Type, func(t *testing.T) {
			scan.checkRow(t, row)
		})
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if rows == 0 || scan.keys == 0 {
		t.Fatal("empty baseline")
	}

	var failed, invisible []string
	for label, count := range scan.rowsSeen {
		if consumedPayloadKeyAllowlist[label] != "" {
			continue // allowed to be unconsumed; the allowlist is validated below
		}
		switch {
		case scan.observed[label] > 0:
		case scan.inconclusive[label] > 0:
			invisible = append(invisible, fmt.Sprintf("%s (empty value in %d of %d rows)", label, scan.inconclusive[label], count))
		default:
			failed = append(failed, fmt.Sprintf("%s (%d rows)", label, count))
		}
	}
	sort.Strings(failed)
	sort.Strings(invisible)
	if len(failed) > 0 {
		t.Errorf("%d payload keys are never consumed by the reader (fix the reader, or add an allowlist entry with a reason):\n  %s",
			len(failed), strings.Join(failed, "\n  "))
	}
	for key, reason := range consumedPayloadKeyAllowlist {
		if scan.rowsSeen[key] == 0 {
			t.Errorf("allowlist entry %s matches no payload key", key)
		}
		if scan.observed[key] > 0 {
			t.Errorf("allowed key %s is consumed: %s", key, reason)
		}
	}
	if len(invisible) > 0 {
		t.Logf("keys invisible to the drop test (empty value):\n  %s", strings.Join(invisible, "\n  "))
	}
	t.Logf("rows %d, keys %d, invisible keys %d, unconsumed keys %d", rows, scan.keys, len(invisible), len(failed))
}

// consumedScan holds the drop test verdicts: rows carrying a key, rows where dropping the
// key changed an observable, and rows where the value is an empty container.
type consumedScan struct {
	rowsSeen     map[string]int
	observed     map[string]int
	inconclusive map[string]int
	keys         int
}

func newConsumedScan() *consumedScan {
	return &consumedScan{
		rowsSeen:     map[string]int{},
		observed:     map[string]int{},
		inconclusive: map[string]int{},
	}
}

// checkRow drops every payload key of one row and records what each drop changed.
func (s *consumedScan) checkRow(t *testing.T, row consumedFixtureRow) {
	t.Helper()
	payload, err := hex.DecodeString(row.Payload)
	if err != nil {
		t.Fatalf("decode payload hex: %v", err)
	}
	if len(payload) == 0 || payload[0] != actionPayloadCodecMsgpack {
		t.Fatalf("payload codec %v, want 1", payload)
	}
	base := &models.RawAction{Type: row.Type}
	if err := DecodeActionPayload(payload, base); err != nil {
		t.Fatalf("DecodeActionPayload: %v", err)
	}
	original, err := observablesOf(base)
	if err != nil {
		t.Fatalf("original row: %v", err)
	}
	paths, data, err := payloadKeyPaths(payload)
	if err != nil {
		t.Fatalf("payload key paths: %v", err)
	}
	for _, path := range paths {
		s.keys++
		label := row.Type + "." + path.label()
		s.rowsSeen[label]++
		if path.empty {
			s.inconclusive[label]++
			continue
		}
		collected := false
		var modifiedRaw *models.RawAction
		if path.placeholder {
			// No field carries a placeholder: dropping it means the list loses the
			// element, which shifts every leg behind it.
			modifiedRaw, err = payloadWithoutLegElement(row.Type, data, path.index)
			if err != nil {
				t.Fatalf("without %s: %v", path.label(), err)
			}
		} else {
			variant := *base
			if base.Extra != nil {
				variant.Extra = map[string]interface{}{}
				for name, value := range base.Extra {
					variant.Extra[name] = value
				}
			}
			collected = dropPayloadKey(&variant, row.Type, path)
			modifiedRaw = &variant
		}
		modified, err := observablesOf(modifiedRaw)
		if err != nil {
			t.Fatalf("without %s: %v", path.label(), err)
		}
		// Consumed when the API JSON or the address set changes, or when the collector
		// reads the field itself: an address another collected field already contributed
		// leaves the set unchanged.
		if collected ||
			!bytes.Equal(original.api, modified.api) ||
			!sameAddresses(original.addresses, modified.addresses) {
			s.observed[label]++
		}
	}
}

// TestPayloadConsumedKeysPlaceholderRow checks the placeholder mechanic on a synthetic
// row: the frozen corpus has no empty leg, and the C++ encoder emits one for an empty leg
// before a filled one. The placeholder must count as consumed - removing it moves the
// filled leg down a slot - so the key does not fail.
func TestPayloadConsumedKeysPlaceholderRow(t *testing.T) {
	record := map[string]interface{}{
		"asset":  "0:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
		"amount": "456",
	}
	payload, err := EncodeCompactForTest(map[string]interface{}{
		"legs": []interface{}{nil, record},
	})
	if err != nil {
		t.Fatal(err)
	}
	row := consumedFixtureRow{
		FixtureID: "synthetic",
		ActionID:  "legs-sol",
		Type:      "dex_deposit_liquidity",
		Payload:   hex.EncodeToString(payload),
	}
	scan := newConsumedScan()
	scan.checkRow(t, row)
	for _, label := range []string{"dex_deposit_liquidity.legs[0]", "dex_deposit_liquidity.legs[1].amount", "dex_deposit_liquidity.legs[1].asset"} {
		if scan.rowsSeen[label] != 1 {
			t.Errorf("%s seen %d times, want 1", label, scan.rowsSeen[label])
			continue
		}
		if scan.observed[label] != 1 {
			t.Errorf("%s observed as consumed %d times, want 1 (inconclusive=%v)",
				label, scan.observed[label], scan.inconclusive[label] == 1)
		}
	}
	if scan.inconclusive["dex_deposit_liquidity.legs[0]"] != 0 {
		t.Errorf("the placeholder was treated as an empty container")
	}
}

type actionObservables struct {
	api       []byte
	addresses map[models.AccountAddress]bool
}

func observablesOf(raw *models.RawAction) (actionObservables, error) {
	action, err := ParseRawAction(raw)
	if err != nil {
		return actionObservables{}, err
	}
	encoded, err := json.Marshal(action)
	if err != nil {
		return actionObservables{}, err
	}
	addresses := map[models.AccountAddress]bool{}
	CollectAddressesFromAction(&addresses, raw)
	return actionObservables{api: encoded, addresses: addresses}, nil
}

func sameAddresses(a, b map[models.AccountAddress]bool) bool {
	if len(a) != len(b) {
		return false
	}
	for address := range a {
		if !b[address] {
			return false
		}
	}
	return true
}

// keyPath is one payload key of a row: its path parts, whether it is a `legs[i]` entry,
// whether that entry is a nil placeholder (no field to zero - the whole list has to lose
// the element), its leg index, and whether its value is an empty container (invisible to
// any drop test).
type keyPath struct {
	parts       []string
	leg         bool
	placeholder bool
	index       int
	empty       bool
}

func (p keyPath) label() string {
	out := ""
	for index, part := range p.parts {
		if index > 0 {
			out += "."
		}
		out += part
	}
	return out
}

// payloadKeyPaths lists the payload keys to check (leaves of nested maps as dotted paths,
// each `legs[i].<member>`, and any other value as its own key) and returns the decoded
// payload map, which the placeholder drop needs to re-encode the row.
func payloadKeyPaths(payload []byte) ([]keyPath, map[string]interface{}, error) {
	if len(payload) == 0 || payload[0] != actionPayloadCodecMsgpack {
		return nil, nil, fmt.Errorf("unsupported payload codec")
	}
	data, err := decodeCompactPayload(payload[1:])
	if err != nil {
		return nil, nil, fmt.Errorf("decode payload: %w", err)
	}
	var paths []keyPath
	for _, name := range sortedKeys(data) {
		value := data[name]
		if name == "legs" {
			items, ok := value.([]interface{})
			if !ok {
				return nil, nil, fmt.Errorf("legs is %T, want an array", value)
			}
			for index, item := range items {
				record, ok := item.(map[string]interface{})
				if !ok {
					paths = append(paths, keyPath{
						parts:       []string{fmt.Sprintf("legs[%d]", index)},
						leg:         true,
						placeholder: item == nil,
						index:       index,
					})
					continue
				}
				for _, member := range sortedKeys(record) {
					paths = append(paths, keyPath{
						parts: []string{fmt.Sprintf("legs[%d]", index), member},
						leg:   true,
						empty: emptyContainer(record[member]),
					})
				}
			}
			continue
		}
		if nested, ok := value.(map[string]interface{}); ok {
			for _, child := range sortedKeys(nested) {
				if inner, ok := nested[child].(map[string]interface{}); ok {
					for _, grand := range sortedKeys(inner) {
						paths = append(paths, keyPath{parts: []string{name, child, grand}, empty: emptyContainer(inner[grand])})
					}
					continue
				}
				paths = append(paths, keyPath{parts: []string{name, child}, empty: emptyContainer(nested[child])})
			}
			continue
		}
		paths = append(paths, keyPath{parts: []string{name}, empty: emptyContainer(value)})
	}
	return paths, data, nil
}

func emptyContainer(value interface{}) bool {
	switch typed := value.(type) {
	case []interface{}:
		return len(typed) == 0
	case map[string]interface{}:
		return len(typed) == 0
	default:
		return false
	}
}

func sortedKeys(values map[string]interface{}) []string {
	names := make([]string, 0, len(values))
	for name := range values {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

// sentinelAddress is a valid address the collector probe uses to detect that a field is
// one of the collector's reads.
var sentinelAddress = models.AccountAddress("0:FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF")

// payloadWithoutLegElement re-encodes the row's payload with one `legs` element removed,
// which is how a nil placeholder's absence looks: the legs behind it move down a slot.
func payloadWithoutLegElement(typeName string, data map[string]interface{}, index int) (*models.RawAction, error) {
	clone := make(map[string]interface{}, len(data))
	for key, value := range data {
		clone[key] = value
	}
	items, ok := clone["legs"].([]interface{})
	if !ok {
		return nil, fmt.Errorf("legs is %T, want an array", clone["legs"])
	}
	if index < 0 || index >= len(items) {
		return nil, fmt.Errorf("legs index %d is outside the %d element list", index, len(items))
	}
	kept := make([]interface{}, 0, len(items)-1)
	kept = append(kept, items[:index]...)
	kept = append(kept, items[index+1:]...)
	clone["legs"] = kept
	body, err := EncodeCompactForTest(clone)
	if err != nil {
		return nil, err
	}
	raw := &models.RawAction{Type: typeName}
	if err := DecodeActionPayload(body, raw); err != nil {
		return nil, err
	}
	return raw, nil
}

// dropPayloadKey removes one decoded payload key from `raw` the way the key's absence
// would: extra keys leave RawAction.Extra, every other key zeroes the field the decoder
// resolved for it, and a key with no field changes nothing. It reports whether the
// collector reads that field.
func dropPayloadKey(raw *models.RawAction, typeName string, path keyPath) bool {
	prefixes := actionDataPrefixes[typeName]
	target := reflect.ValueOf(raw).Elem()
	var field reflect.Value
	switch {
	case path.leg:
		index, member, ok := legMember(path)
		if !ok {
			return false
		}
		field = resolvePayloadLegField(target, prefixes, index, member)
	case len(path.parts) == 1 && actionPayloadExtraKeys[path.parts[0]]:
		delete(raw.Extra, path.parts[0])
		return false
	default:
		field = resolvePayloadField(target, prefixes, path.label())
	}
	if !field.IsValid() {
		return false
	}
	collected := collectorReadsField(typeName, fieldNameOf(raw, field))
	field.SetZero()
	return collected
}

// fieldNameOf names the RawAction field a resolved reflect.Value points at.
func fieldNameOf(raw *models.RawAction, field reflect.Value) string {
	target := reflect.ValueOf(raw).Elem()
	for index := range target.NumField() {
		if target.Field(index).Addr().Pointer() == field.Addr().Pointer() {
			return target.Type().Field(index).Name
		}
	}
	return ""
}

// collectorReadsField probes the collector with a zero action of the row's type carrying
// only this field. The collector is a flat list of per-field reads, so a collected
// sentinel means the field consumes the key through the address list.
func collectorReadsField(typeName, fieldName string) bool {
	if fieldName == "" {
		return false
	}
	probe := &models.RawAction{Type: typeName}
	probeField := reflect.ValueOf(probe).Elem().FieldByName(fieldName)
	if !probeField.IsValid() {
		return false
	}
	switch probeField.Type() {
	case reflect.TypeOf((*models.AccountAddress)(nil)):
		value := sentinelAddress
		probeField.Set(reflect.ValueOf(&value))
	case reflect.TypeOf([]models.AccountAddress(nil)):
		probeField.Set(reflect.ValueOf([]models.AccountAddress{sentinelAddress}))
	default:
		return false
	}
	addresses := map[models.AccountAddress]bool{}
	CollectAddressesFromAction(&addresses, probe)
	return addresses[sentinelAddress]
}

func legMember(path keyPath) (int, string, bool) {
	if len(path.parts) != 2 {
		return 0, "", false
	}
	var index int
	if _, err := fmt.Sscanf(path.parts[0], "legs[%d]", &index); err != nil {
		return 0, "", false
	}
	return index, path.parts[1], true
}
