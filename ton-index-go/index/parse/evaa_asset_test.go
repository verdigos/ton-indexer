package parse_test

import (
	"strings"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"
)

// TestParseEvaaAssetId pins the exact asset-id lookup: the engine writes a full 64-digit
// lowercase `0x` text and the tables carry that same form, so only an exact hit resolves.
// The TON id is a known asset without a master address.
func TestParseEvaaAssetId(t *testing.T) {
	const mainnetId = "0xb387968236197958ca4ac55e9b5be38e688c7631af84c86756431f49a878ef33"
	master, ok := parse.EVAA_ASSET_ID_MAP_MAINNET[mainnetId]
	if !ok {
		t.Fatalf("the fixture id %s left the mainnet table", mainnetId)
	}
	for _, tc := range []struct {
		name    string
		in      string
		known   bool
		address string // empty when the known asset has no master address
	}{
		{name: "ton asset", in: parse.EVAA_TON_ASSET_ID, known: true},
		{name: "mainnet id", in: mainnetId, known: true, address: master},
		{name: "upper case is another text", in: strings.ToUpper(mainnetId)},
		{name: "unpadded form", in: "0x0" + strings.TrimPrefix(mainnetId, "0x")},
		{name: "bare hex", in: strings.TrimPrefix(mainnetId, "0x")},
		{name: "empty", in: ""},
		{name: "garbage", in: "garbage"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, known := parse.ParseEvaaAssetId(tc.in)
			if known != tc.known {
				t.Fatalf("ParseEvaaAssetId(%q) known = %v, want %v", tc.in, known, tc.known)
			}
			if !known {
				return
			}
			if tc.address == "" {
				if got != nil {
					t.Fatalf("ParseEvaaAssetId(%q) = %v, want a known asset with no address", tc.in, got)
				}
				return
			}
			if got == nil || string(*got) != tc.address {
				t.Fatalf("ParseEvaaAssetId(%q) = %v, want %s", tc.in, got, tc.address)
			}
		})
	}
}

// TestParseEvaaAssetIdTables pins that every entry of the selected network's table
// resolves to its own master address.
func TestParseEvaaAssetIdTables(t *testing.T) {
	saved := parse.IsTestnet
	t.Cleanup(func() { parse.IsTestnet = saved })
	for _, tc := range []struct {
		name    string
		table   map[string]string
		testnet bool
	}{
		{name: "mainnet", table: parse.EVAA_ASSET_ID_MAP_MAINNET, testnet: false},
		{name: "testnet", table: parse.EVAA_ASSET_ID_MAP_TESTNET, testnet: true},
	} {
		parse.IsTestnet = tc.testnet
		if len(tc.table) == 0 {
			t.Fatalf("the %s table is empty", tc.name)
		}
		for id, address := range tc.table {
			got, known := parse.ParseEvaaAssetId(id)
			if !known || got == nil || string(*got) != address {
				t.Errorf("ParseEvaaAssetId(%q) = %v, %v; want %s", id, got, known, address)
			}
		}
	}
}

// TestParseEvaaAssetIdTestnetSwitch pins the network dispatch: a mainnet-only id stops
// resolving on testnet, where the testnet table answers instead.
func TestParseEvaaAssetIdTestnetSwitch(t *testing.T) {
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
	if _, known := parse.ParseEvaaAssetId(mainnetOnly); !known {
		t.Fatalf("the mainnet-only id did not resolve on mainnet")
	}

	parse.IsTestnet = true
	if got, known := parse.ParseEvaaAssetId(shared); !known || got == nil || string(*got) != testnetMaster {
		t.Fatalf("ParseEvaaAssetId(%s) on testnet = %v, %v; want %s", shared, got, known, testnetMaster)
	}
	if got, known := parse.ParseEvaaAssetId(mainnetOnly); known {
		t.Errorf("mainnet-only id resolved on testnet: %v", got)
	}
	if _, known := parse.ParseEvaaAssetId(parse.EVAA_TON_ASSET_ID); !known {
		t.Errorf("the TON asset id did not resolve on testnet")
	}
}
