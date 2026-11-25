package aggregate

import (
	"context"
	"fmt"
	"time"

	"github.com/ClickHouse/clickhouse-go/v2/lib/driver"

	"github.com/toncenter/ton-indexer/ton-index-price-api/prices"
)

type ClickHouse struct {
	conn driver.Conn
}

func OpenClickHouse(dsn string) (*ClickHouse, error) {
	conn, err := prices.Connect(dsn)
	if err != nil {
		return nil, err
	}
	return &ClickHouse{conn: conn}, nil
}

func (c *ClickHouse) Close() error { return c.conn.Close() }

func (c *ClickHouse) Exec(ctx context.Context, query string, params map[string]string) error {
	return c.conn.Exec(prices.WithParams(ctx, params), query)
}

func unixPtr(t *time.Time) *int64 {
	if t == nil {
		return nil
	}
	u := t.Unix()
	return &u
}

func (c *ClickHouse) Progress(ctx context.Context) (Progress, error) {
	var ready, done, head *time.Time
	if err := c.conn.QueryRow(ctx, statement(stmtProgress)).Scan(&ready, &done, &head); err != nil {
		return Progress{}, fmt.Errorf("progress: %w", err)
	}
	return Progress{Head: unixPtr(head), Done: unixPtr(done), Ready: unixPtr(ready)}, nil
}

func (c *ClickHouse) AnchorRows(ctx context.Context, from int64) (bool, error) {
	var n uint64
	err := c.conn.QueryRow(ctx, `SELECT count() FROM (SELECT 1 FROM anchor_1m WHERE minute >= toDateTime(?) LIMIT 1)`, from).Scan(&n)
	return n > 0, err
}

func (c *ClickHouse) FirstSnapshotMinute(ctx context.Context) (int64, bool, error) {
	var n uint64
	var first uint32
	if err := c.conn.QueryRow(ctx, `SELECT count(), toUInt32(toStartOfMinute(min(block_utime))) FROM pool_snapshots`).
		Scan(&n, &first); err != nil {
		return 0, false, err
	}
	return int64(first), n > 0, nil
}
