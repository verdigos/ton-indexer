package parse_test

import (
	"strings"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"
)

// TestParseEvaaAssetId requires exact network-specific table keys and treats the TON id
// as known on both networks without a master address.
func TestParseEvaaAssetId(t *testing.T) {
	const (
		mainnetId     = "0xb387968236197958ca4ac55e9b5be38e688c7631af84c86756431f49a878ef33"
		mainnetOnlyId = "0x3313e2f57ba870af34480350c789b0987d15b43a53172bfce294de21e7d724e7"
	)
	mainnetMaster, ok := parse.EVAA_ASSET_ID_MAP_MAINNET[mainnetId]
	if !ok {
		t.Fatalf("the fixture id %s left the mainnet table", mainnetId)
	}
	testnetMaster, ok := parse.EVAA_ASSET_ID_MAP_TESTNET[mainnetId]
	if !ok {
		t.Fatalf("the fixture id %s left the testnet table", mainnetId)
	}
	mainnetOnlyMaster, ok := parse.EVAA_ASSET_ID_MAP_MAINNET[mainnetOnlyId]
	if !ok {
		t.Fatalf("the fixture id %s left the mainnet table", mainnetOnlyId)
	}
	for _, tc := range []struct {
		name    string
		in      string
		testnet bool
		known   bool
		address string // empty when the known asset has no master address
	}{
		{name: "ton asset", in: parse.EVAA_TON_ASSET_ID, known: true},
		{name: "ton asset on testnet", in: parse.EVAA_TON_ASSET_ID, testnet: true, known: true},
		{name: "mainnet id", in: mainnetId, known: true, address: mainnetMaster},
		{name: "mainnet id on testnet", in: mainnetId, testnet: true, known: true, address: testnetMaster},
		{name: "mainnet-only id", in: mainnetOnlyId, known: true, address: mainnetOnlyMaster},
		{name: "mainnet-only id on testnet", in: mainnetOnlyId, testnet: true},
		{name: "upper case is another text", in: strings.ToUpper(mainnetId)},
		{name: "unpadded form", in: "0x0" + strings.TrimPrefix(mainnetId, "0x")},
		{name: "bare hex", in: strings.TrimPrefix(mainnetId, "0x")},
		{name: "empty", in: ""},
		{name: "garbage", in: "garbage"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			saved := parse.IsTestnet
			t.Cleanup(func() { parse.IsTestnet = saved })
			parse.IsTestnet = tc.testnet
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
