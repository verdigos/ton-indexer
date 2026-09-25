// Package dexprices reads the DEX prices prototype tables from ClickHouse
// (dex-tap/sql/schema.sql, docs/dex-prices/mvp-plan.md, stage M-1).
package dexprices

import (
	"context"
	_ "embed"
	"fmt"
	"strings"
	"time"

	"github.com/ClickHouse/clickhouse-go/v2"
	"github.com/ClickHouse/clickhouse-go/v2/lib/driver"
)

// pricesQuery is a copy of dex-tap/sql/prices.sql; TestPricesQueryMatchesDexTap keeps them equal.
//
//go:embed prices.sql
var pricesQuery string

type Price struct {
	Asset        string  `json:"asset"`
	Symbol       string  `json:"symbol"`
	Decimals     uint8   `json:"decimals"`
	PriceTon     float64 `json:"price_ton"`
	PriceUsdt    float64 `json:"price_usdt"`
	PriceTonRaw  float64 `json:"price_ton_raw"`
	PriceUsdtRaw float64 `json:"price_usdt_raw"`
	LiquidityTon float64 `json:"liquidity_ton"`
	Pools        uint64  `json:"pools"`
	Hops         uint8   `json:"hops"`
} // @name DexPrice

type PoolSnapshot struct {
	Pool      string  `json:"pool"`
	McSeqno   uint32  `json:"mc_seqno"`
	Utime     uint32  `json:"utime"`
	LastTxLt  uint64  `json:"last_tx_lt,string"`
	Dex       string  `json:"dex"`
	PoolType  string  `json:"pool_type"`
	Asset0    string  `json:"asset0"`
	Asset1    string  `json:"asset1"`
	Reserve0  string  `json:"reserve0"`
	Reserve1  string  `json:"reserve1"`
	SpotPrice float64 `json:"spot_price"`
	TxCount   uint16  `json:"tx_count"`
} // @name DexPoolSnapshot

type PriceParams struct {
	Assets           []string
	MaxAge           time.Duration
	MinLiquidityUsdt float64
}

type Store struct {
	conn driver.Conn
}

// NewStore opens a ClickHouse connection, e.g. "clickhouse://user:pass@localhost:9000/dex_prices".
func NewStore(dsn string) (*Store, error) {
	options, err := clickhouse.ParseDSN(dsn)
	if err != nil {
		return nil, fmt.Errorf("parse ClickHouse DSN: %w", err)
	}
	conn, err := clickhouse.Open(options)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := conn.Ping(ctx); err != nil {
		return nil, fmt.Errorf("ClickHouse ping: %w", err)
	}
	return &Store{conn: conn}, nil
}

func (s *Store) Close() error {
	return s.conn.Close()
}

// arrayParam renders a ClickHouse Array(String) query parameter; assets are raw addresses or "".
func arrayParam(values []string) string {
	quoted := make([]string, len(values))
	for i, v := range values {
		quoted[i] = "'" + strings.ReplaceAll(strings.ReplaceAll(v, `\`, `\\`), "'", `\'`) + "'"
	}
	return "[" + strings.Join(quoted, ",") + "]"
}

func (s *Store) QueryPrices(ctx context.Context, params PriceParams) ([]Price, error) {
	ctx = clickhouse.Context(ctx, clickhouse.WithParameters(clickhouse.Parameters{
		"max_age_sec":        fmt.Sprintf("%d", int64(params.MaxAge/time.Second)),
		"min_liquidity_usdt": fmt.Sprintf("%g", params.MinLiquidityUsdt),
		"assets":             arrayParam(params.Assets),
	}))
	rows, err := s.conn.Query(ctx, pricesQuery)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	prices := []Price{}
	for rows.Next() {
		var p Price
		if err := rows.Scan(&p.Asset, &p.Symbol, &p.Decimals, &p.PriceTon, &p.PriceUsdt, &p.PriceTonRaw,
			&p.PriceUsdtRaw, &p.LiquidityTon, &p.Pools, &p.Hops); err != nil {
			return nil, err
		}
		prices = append(prices, p)
	}
	return prices, rows.Err()
}

func (s *Store) QueryPoolSnapshots(ctx context.Context, pool string, startUtime, endUtime uint32, limit int) ([]PoolSnapshot, error) {
	rows, err := s.conn.Query(ctx, `
		SELECT pool, mc_seqno, toUInt32(block_utime), last_tx_lt, toString(dex), toString(pool_type), asset0, asset1,
		       toString(reserve0), toString(reserve1), spot_price, tx_count
		FROM dex_prices.dex_pool_snapshots FINAL
		WHERE pool = ? AND block_utime >= toDateTime(?) AND block_utime <= toDateTime(?)
		ORDER BY mc_seqno DESC
		LIMIT ?`, pool, startUtime, endUtime, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	snapshots := []PoolSnapshot{}
	for rows.Next() {
		var p PoolSnapshot
		if err := rows.Scan(&p.Pool, &p.McSeqno, &p.Utime, &p.LastTxLt, &p.Dex, &p.PoolType, &p.Asset0, &p.Asset1,
			&p.Reserve0, &p.Reserve1, &p.SpotPrice, &p.TxCount); err != nil {
			return nil, err
		}
		snapshots = append(snapshots, p)
	}
	return snapshots, rows.Err()
}
