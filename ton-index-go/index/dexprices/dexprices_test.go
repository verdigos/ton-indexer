package dexprices

import (
	"context"
	"os"
	"testing"
	"time"
)

func TestPricesQueryMatchesDexTap(t *testing.T) {
	canonical, err := os.ReadFile("../../../dex-tap/sql/prices.sql")
	if err != nil {
		t.Skipf("dex-tap checkout not available: %v", err)
	}
	if string(canonical) != pricesQuery {
		t.Fatal("index/dexprices/prices.sql differs from dex-tap/sql/prices.sql; copy the dex-tap version")
	}
}

func TestArrayParam(t *testing.T) {
	cases := map[string][]string{
		"[]":               nil,
		"['']":             {""},
		"['0:AB','0:CD']":  {"0:AB", "0:CD"},
		`['it\'s','a\\b']`: {"it's", `a\b`},
	}
	for want, values := range cases {
		if got := arrayParam(values); got != want {
			t.Errorf("arrayParam(%q) = %s, want %s", values, got, want)
		}
	}
}

// Runs against a ClickHouse filled by dex-tap, e.g.
// DEX_PRICES_CLICKHOUSE_DSN=clickhouse://localhost:9000/dex_prices go test ./index/dexprices/
func TestStoreAgainstClickHouse(t *testing.T) {
	dsn := os.Getenv("DEX_PRICES_CLICKHOUSE_DSN")
	if dsn == "" {
		t.Skip("DEX_PRICES_CLICKHOUSE_DSN is not set")
	}
	store, err := NewStore(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := context.Background()

	prices, err := store.QueryPrices(ctx, PriceParams{Assets: []string{""}, MaxAge: 100000 * time.Hour, MinLiquidityUsdt: 1000})
	if err != nil {
		t.Fatal(err)
	}
	if len(prices) != 1 || prices[0].Symbol != "TON" || prices[0].PriceTon != 1 {
		t.Fatalf("unexpected TON row: %+v", prices)
	}
	if prices[0].PriceUsdt <= 0 {
		t.Logf("no anchor pools in the database, TON price in USDT is %v", prices[0].PriceUsdt)
	}

	all, err := store.QueryPrices(ctx, PriceParams{MaxAge: 100000 * time.Hour, MinLiquidityUsdt: 1000})
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range all {
		if p.Asset == "" {
			continue
		}
		snapshots, err := store.QueryPoolSnapshots(ctx, "0:0000000000000000000000000000000000000000000000000000000000000000", 0, 4000000000, 10)
		if err != nil {
			t.Fatal(err)
		}
		if len(snapshots) != 0 {
			t.Fatalf("snapshots for a missing pool: %+v", snapshots)
		}
		break
	}
}
