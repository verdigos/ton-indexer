// Package prices reads jetton prices and history from ClickHouse.
package prices

import (
	"cmp"
	"context"
	"database/sql"
	_ "embed"
	"errors"
	"fmt"
	"slices"
	"strconv"

	"github.com/ClickHouse/clickhouse-go/v2"
	"github.com/ClickHouse/clickhouse-go/v2/lib/driver"
)

const (
	tonAsset  = "TON" // Toncoin as an asset key in the price tables
	quoteTon  = "TON"
	quoteUsdt = "USDT"

	UsdtMaster = "0:B113A994B5024A16719F69139328EB759596C38A25F59028B146FECDC3621DFE"
)

// TonAliases are the assets that pools use for TON: DeDust's native TON (an empty string) and the STON.fi pTON masters.
var TonAliases = []string{"",
	"0:8CDC1D7640AD5EE326527FC1AD0514F468B30DC84B0173F0E155F451B4E11F7C",
	"0:671963027F7F85659AB55B821671688601CDCF1EE674FC7FBBB1A776A18D34A3",
}

// Head is the newest block the prices use (aggregate/sql/head.sql).
type Head struct {
	Seqno uint32
	Utime uint32
}

var ErrNoHead = fmt.Errorf("no live ingest head")

// Row is one price row from ClickHouse, or a USDT row computed from it. nil means NULL.
type Row struct {
	Asset       string
	Quote       string
	Minute      uint32
	PriceRaw    *float64
	Pools       uint16
	TvlQuote    *float64
	NewestUtime uint32
	Stale       bool
	Reason      string
	Final       bool     // false: a row of the current minute, which can still change
	AnchorRaw   *float64 // TON rows: the TON/USDT price of the minute; the USDT price is the TON price times it
	AnchorStale bool
}

type HistoryQuery struct {
	Asset string
	Quote string
	Start uint32
	End   uint32
	Step  uint32 // seconds: the newest row of each step
	Open  *Row   // the asset's newest TON row; the store adds it when it is a row of the current minute
}

type ClickHouseStore struct {
	conn driver.Conn
}

// Opening the store defers network access until the first query.
func OpenClickHouse(dsn string) (*ClickHouseStore, error) {
	conn, err := Connect(dsn)
	if err != nil {
		return nil, err
	}
	return &ClickHouseStore{conn: conn}, nil
}

// Connect opens the ClickHouse pool that the API and the aggregator use. It connects on the first query.
func Connect(dsn string) (driver.Conn, error) {
	options, err := clickhouse.ParseDSN(dsn)
	if err != nil {
		return nil, fmt.Errorf("parse ClickHouse DSN: %w", err)
	}
	return clickhouse.Open(options)
}

func WithParams(ctx context.Context, params map[string]string) context.Context {
	return clickhouse.Context(ctx, clickhouse.WithParameters(clickhouse.Parameters(params)))
}

func (s *ClickHouseStore) Close() error {
	return s.conn.Close()
}

func (s *ClickHouseStore) Head(ctx context.Context) (Head, error) {
	var head Head
	err := s.conn.QueryRow(ctx, `SELECT mc_seqno, toUInt32(block_utime) FROM ingest_head FINAL WHERE writer = 'live'`).
		Scan(&head.Seqno, &head.Utime)
	if errors.Is(err, sql.ErrNoRows) {
		return head, ErrNoHead
	}
	return head, err
}

//go:embed sql/history.sql
var historySQL string

//go:embed sql/latest.sql
var latestSQL string

//go:embed sql/anchor_history.sql
var anchorHistorySQL string

func scanRows(rows driver.Rows) ([]Row, error) {
	defer rows.Close()
	res := []Row{}
	for rows.Next() {
		var r Row
		if err := rows.Scan(&r.Asset, &r.Quote, &r.Minute, &r.PriceRaw, &r.Pools, &r.TvlQuote, &r.NewestUtime,
			&r.Stale, &r.Reason, &r.Final, &r.AnchorRaw, &r.AnchorStale); err != nil {
			return nil, err
		}
		res = append(res, r)
	}
	return res, rows.Err()
}

func (s *ClickHouseStore) Latest(ctx context.Context) ([]Row, error) {
	rows, err := s.conn.Query(ctx, latestSQL)
	if err != nil {
		return nil, err
	}
	raw, err := scanRows(rows)
	if err != nil {
		return nil, err
	}
	return deriveLatest(raw), nil
}

// deriveLatest adds the USDT row of each TON row. The anchor row (TON in USDT) also gives USDT in TON, as its inverse.
func deriveLatest(raw []Row) []Row {
	res := []Row{}
	for _, r := range raw {
		switch {
		case r.Quote == quoteUsdt && r.PriceRaw != nil && *r.PriceRaw > 0: // the anchor, its tvl in raw TON
			inverse := 1 / *r.PriceRaw
			u := r
			u.Asset, u.Quote, u.PriceRaw = UsdtMaster, quoteTon, &inverse
			if r.TvlQuote != nil { // raw TON to raw USDT
				l := *r.TvlQuote * *r.PriceRaw
				r.TvlQuote = &l
			}
			res = append(res, r, u)
		case r.Quote == quoteTon && r.Asset != tonAsset && r.Asset != UsdtMaster:
			res = append(res, r, usdtRow(r))
		}
	}
	return res
}

// usdtRow is a TON row in USDT: its price and tvl times the anchor of its minute. A TON row without a price
// keeps its reason; an anchor without a price gives no_anchor.
func usdtRow(ton Row) Row {
	u := Row{Asset: ton.Asset, Quote: quoteUsdt, Minute: ton.Minute, Pools: ton.Pools, NewestUtime: ton.NewestUtime,
		Reason: ton.Reason, Final: ton.Final}
	switch {
	case ton.PriceRaw == nil:
	case ton.AnchorRaw == nil || *ton.AnchorRaw <= 0:
		u.Reason = "no_anchor"
	default:
		price := *ton.PriceRaw * *ton.AnchorRaw
		u.PriceRaw, u.Reason, u.Stale = &price, "", ton.AnchorStale
		if ton.TvlQuote != nil {
			tvl := *ton.TvlQuote * *ton.AnchorRaw
			u.TvlQuote = &tvl
		}
	}
	return u
}

func (s *ClickHouseStore) History(ctx context.Context, q HistoryQuery) ([]Row, error) {
	params := map[string]string{"asset": q.Asset, "from_minute": strconv.FormatUint(uint64(q.Start), 10),
		"to_minute": strconv.FormatUint(uint64(q.End), 10), "step": strconv.FormatUint(uint64(q.Step), 10)}
	query := historySQL
	switch {
	case q.Asset == tonAsset && q.Quote == quoteUsdt, q.Asset == UsdtMaster && q.Quote == quoteTon: // the anchor alone
		params["quote"], params["inverse"] = q.Quote, strconv.FormatBool(q.Asset == UsdtMaster)
		query = anchorHistorySQL
	case q.Asset == UsdtMaster, q.Asset == tonAsset: // no points in its own quote
		return []Row{}, nil
	}
	rows, err := s.conn.Query(WithParams(ctx, params), query)
	if err != nil {
		return nil, err
	}
	res, err := scanRows(rows)
	if err != nil {
		return nil, err
	}
	if query == historySQL {
		res = withRow(res, q.Open, q.Start, q.End, q.Step)
		if q.Quote == quoteUsdt {
			for i, r := range res {
				res[i] = usdtRow(r)
			}
		}
	}
	return res, nil
}

// withRow sorts history rows by minute and appends the current-minute row when it is in range and newer than stored
// rows, replacing any stored point in the same step.
func withRow(rows []Row, open *Row, start, end, step uint32) []Row {
	slices.SortFunc(rows, func(a, b Row) int { return cmp.Compare(a.Minute, b.Minute) })
	if open != nil && !open.Final && open.Minute >= start && open.Minute <= end &&
		(len(rows) == 0 || rows[len(rows)-1].Minute < open.Minute) {
		if n := len(rows); n > 0 && rows[n-1].Minute/step == open.Minute/step {
			rows[n-1] = *open
		} else {
			rows = append(rows, *open)
		}
	}
	return rows
}
