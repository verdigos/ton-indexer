package parse_test

import (
	"strings"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"
)

// assertAssetsEqual checks the resolved master address of one asset id text.
func assertAssetsEqual(t *testing.T, form, want string) {
	t.Helper()
	got, ok := parse.ParseEvaaAssetId(form)
	if !ok {
		t.Fatalf("ParseEvaaAssetId(%q) = not found, want %s", form, want)
	}
	if got == nil {
		t.Fatalf("ParseEvaaAssetId(%q) reported a known asset with no address (the TON asset)", form)
	}
	if string(*got) != want {
		t.Errorf("ParseEvaaAssetId(%q) = %s, want %s", form, *got, want)
	}
}

// TestParseEvaaAssetIdWidths pins the asset-id lookup across both text widths: the tables
// are written without leading zeros while the producer pads the id to a fixed 64 hex digits,
// so a padded text must resolve to the same master address - and everything that resolves
// today keeps resolving unchanged.
func TestParseEvaaAssetIdWidths(t *testing.T) {
	// No real id has a leading-zero nibble: this scan of the whole frozen baseline found 22
	// `asset_id` values, all exactly 64 hex digits, and no table key starts with a zero. The
	// padded forms below are therefore built by hand.
	const mainnetId = "0xb387968236197958ca4ac55e9b5be38e688c7631af84c86756431f49a878ef33"
	master, known := parse.EVAA_ASSET_ID_MAP_MAINNET[mainnetId]
	if !known {
		t.Fatalf("the fixture id %s left the mainnet table", mainnetId)
	}
	digits := strings.TrimPrefix(mainnetId, "0x")

	// The TON asset: as stored, padded by one and by several zeros, and in upper case.
	if _, ok := parse.ParseEvaaAssetId(parse.EVAA_TON_ASSET_ID); !ok {
		t.Errorf("the TON asset id as stored did not resolve")
	}
	for _, form := range []string{
		"0x0" + strings.TrimPrefix(parse.EVAA_TON_ASSET_ID, "0x"),
		"0x000" + strings.TrimPrefix(parse.EVAA_TON_ASSET_ID, "0x"),
		strings.ToUpper(parse.EVAA_TON_ASSET_ID),
	} {
		if _, ok := parse.ParseEvaaAssetId(form); !ok {
			t.Errorf("the TON asset id as %q did not resolve", form)
		}
	}
	// Without the `0x` prefix it is a different wire form and resolves nowhere.
	if _, ok := parse.ParseEvaaAssetId(strings.TrimPrefix(parse.EVAA_TON_ASSET_ID, "0x")); ok {
		t.Errorf("a bare hex TON asset id resolved")
	}

	// A known mainnet id: as stored, one and several leading zeros, upper case.
	assertAssetsEqual(t, mainnetId, master)
	for _, form := range []string{"0x0" + digits, "0x000000" + digits, strings.ToUpper(mainnetId)} {
		assertAssetsEqual(t, form, master)
	}

	// A garbage string and an id that resolves neither way in any width.
	for _, form := range []string{"", "garbage", "0x", "0x0", "0xzz", "0xdeadbeef", "0x0deadbeef", strings.Repeat("0", 64), "0x" + strings.Repeat("f", 64)} {
		if got, ok := parse.ParseEvaaAssetId(form); ok {
			t.Errorf("ParseEvaaAssetId(%q) = %v, want not found", form, got)
		}
	}

	// Every entry of the table of the selected network resolves in both widths, unchanged.
	for _, tc := range []struct {
		name    string
		table   map[string]string
		testnet bool
	}{
		{name: "mainnet", table: parse.EVAA_ASSET_ID_MAP_MAINNET, testnet: false},
		{name: "testnet", table: parse.EVAA_ASSET_ID_MAP_TESTNET, testnet: true},
	} {
		saved := parse.IsTestnet
		parse.IsTestnet = tc.testnet
		if len(tc.table) == 0 {
			t.Fatalf("the %s table is empty", tc.name)
		}
		for id, address := range tc.table {
			assertAssetsEqual(t, id, address)
			assertAssetsEqual(t, "0x0"+strings.TrimPrefix(id, "0x"), address)
		}
		parse.IsTestnet = saved
	}
}

// TestParseEvaaAssetIdSyntheticPaddedKey covers the case no real key shows: a producer-padded
// id whose table key is written short (leading zero dropped). The synthetic entry is added to
// the exported mainnet table for the duration of the test.
func TestParseEvaaAssetIdSyntheticPaddedKey(t *testing.T) {
	short := "0x" + strings.Repeat("ab", 31) + "c" // 63 hex digits, no leading zero
	padded := "0x0" + strings.TrimPrefix(short, "0x")
	const master = "0:E8CB571401B1AADB9DEFC1FA0F72A4A5A0E0D5016903067891D763C91541E72B"

	if _, ok := parse.ParseEvaaAssetId(padded); ok {
		t.Fatalf("the synthetic id %s resolved before it was added to the table", padded)
	}
	parse.EVAA_ASSET_ID_MAP_MAINNET[short] = master
	t.Cleanup(func() { delete(parse.EVAA_ASSET_ID_MAP_MAINNET, short) })

	assertAssetsEqual(t, short, master)
	assertAssetsEqual(t, padded, master)
	assertAssetsEqual(t, "0x00"+strings.TrimPrefix(short, "0x"), master)
	assertAssetsEqual(t, strings.ToUpper(padded), master)
}

// TestParseEvaaAssetIdTestnetTable pins the network switch: the testnet table answers while
// IsTestnet is set, and a mainnet-only id stops resolving there.
func TestParseEvaaAssetIdTestnetTable(t *testing.T) {
	saved := parse.IsTestnet
	t.Cleanup(func() { parse.IsTestnet = saved })

	const shared = "0xb387968236197958ca4ac55e9b5be38e688c7631af84c86756431f49a878ef33"
	const mainnetOnly = "0x3313e2f57ba870af34480350c789b0987d15b43a53172bfce294de21e7d724e7"
	testnetMaster, ok := parse.EVAA_ASSET_ID_MAP_TESTNET[shared]
	if !ok {
		t.Fatalf("the fixture id %s left the testnet table", shared)
	}
	if _, ok := parse.EVAA_ASSET_ID_MAP_MAINNET[mainnetOnly]; !ok {
		t.Fatalf("the fixture id %s left the mainnet table", mainnetOnly)
	}

	assertAssetsEqual(t, shared, parse.EVAA_ASSET_ID_MAP_MAINNET[shared])
	parse.IsTestnet = true
	assertAssetsEqual(t, shared, testnetMaster)
	assertAssetsEqual(t, "0x0"+strings.TrimPrefix(shared, "0x"), testnetMaster)
	if got, ok := parse.ParseEvaaAssetId(mainnetOnly); ok {
		t.Errorf("mainnet-only id resolved on testnet: %v", got)
	}
	if _, ok := parse.ParseEvaaAssetId(parse.EVAA_TON_ASSET_ID); !ok {
		t.Errorf("the TON asset id did not resolve on testnet")
	}
}
