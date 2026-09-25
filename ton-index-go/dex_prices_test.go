package main

import (
	"encoding/json"
	"io"
	"net/http/httptest"
	"os"
	"testing"

	"github.com/gofiber/fiber/v2"
	"github.com/toncenter/ton-indexer/ton-index-go/index/dexprices"
)

const usdtRaw = "0:B113A994B5024A16719F69139328EB759596C38A25F59028B146FECDC3621DFE"

func TestDexAsset(t *testing.T) {
	cases := map[string]string{
		"ton": "",
		"TON": "",
		"EQCxE6mUtQJKFnGfaROTKOt1lZbDiiX1kCixRv7Nw2Id_sDs":                   usdtRaw,
		"0:b113a994b5024a16719f69139328eb759596c38a25f59028b146fecdc3621dfe": usdtRaw,
	}
	for input, want := range cases {
		got, err := dexAsset(input)
		if err != nil || got != want {
			t.Errorf("dexAsset(%q) = %q, %v; want %q", input, got, err, want)
		}
	}
	if _, err := dexAsset("not-an-address"); err == nil {
		t.Error("dexAsset accepted garbage")
	}
}

// Runs the handlers against a ClickHouse filled by dex-tap, e.g.
// DEX_PRICES_CLICKHOUSE_DSN=clickhouse://localhost:9000/dex_prices go test -run TestDexEndpoints .
func TestDexEndpoints(t *testing.T) {
	dsn := os.Getenv("DEX_PRICES_CLICKHOUSE_DSN")
	if dsn == "" {
		t.Skip("DEX_PRICES_CLICKHOUSE_DSN is not set")
	}
	store, err := dexprices.NewStore(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	dexPrices = store

	app := fiber.New()
	app.Get("/prices", GetDexPrices)
	app.Get("/pool_snapshots", GetDexPoolSnapshots)

	get := func(url string, out any) int {
		resp, err := app.Test(httptest.NewRequest("GET", url, nil), -1)
		if err != nil {
			t.Fatal(err)
		}
		body, _ := io.ReadAll(resp.Body)
		if resp.StatusCode == 200 {
			if err := json.Unmarshal(body, out); err != nil {
				t.Fatalf("%s: %v in %s", url, err, body)
			}
		}
		return resp.StatusCode
	}

	var prices DexPricesResponse
	if code := get("/prices?asset=ton&asset=EQCxE6mUtQJKFnGfaROTKOt1lZbDiiX1kCixRv7Nw2Id_sDs&max_age_days=100000", &prices); code != 200 {
		t.Fatalf("prices: status %d", code)
	}
	if len(prices.Prices) != 2 {
		t.Fatalf("expected TON and USDT rows, got %+v", prices.Prices)
	}
	for _, p := range prices.Prices {
		t.Logf("%s %s: %.6f TON, %.6f USDT", p.Symbol, p.Asset, p.PriceTon, p.PriceUsdt)
	}

	var snapshots DexPoolSnapshotsResponse
	anchor := "EQD8TJ8xEWB1SpnRE4d89YO3jl0W0EiBnNS4IBaHaUmdfizE" // STON.fi v1 USDT/TON
	if code := get("/pool_snapshots?pool="+anchor+"&limit=5", &snapshots); code != 200 {
		t.Fatalf("pool_snapshots: status %d", code)
	}
	t.Logf("%d snapshots of the STON.fi v1 anchor", len(snapshots.Snapshots))

	if code := get("/prices?asset=garbage", &prices); code == 200 {
		t.Fatal("garbage asset accepted")
	}
}
