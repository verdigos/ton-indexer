package aggregate

import (
	"embed"
	"fmt"
)

//go:embed sql/*.sql
var sqlFS embed.FS

const (
	stmtProgress     = "progress.sql"
	stmtHead         = "head.sql"
	stmtPoolRecords  = "pool_records.sql"
	stmtAnchor       = "anchor.sql"
	stmtPrices       = "prices.sql"
	stmtRefillLatest = "refill_latest.sql"

	stmtTruncate = "truncate"
	stmtDelete   = "delete"
	stmtSaveDone = "save done"
	stmtSwap     = "swap"
)

var shortStatements = map[string]string{
	stmtTruncate: `TRUNCATE TABLE {table:Identifier}`,
	stmtDelete:   `DELETE FROM {table:Identifier} WHERE minute >= {from_minute:DateTime}`,
	stmtSaveDone: `INSERT INTO aggregate_state (job, done_minute, seq)
		SELECT 'price_1m', {done_minute:DateTime}, (SELECT max(seq) FROM aggregate_state) + 1`,
	stmtSwap: `EXCHANGE TABLES {a:Identifier} AND {b:Identifier}`,
}

func statement(name string) string {
	if s, ok := shortStatements[name]; ok {
		return s
	}
	b, err := sqlFS.ReadFile("sql/" + name)
	if err != nil {
		panic(fmt.Sprintf("dex prices aggregation: embedded statement %s: %v", name, err))
	}
	return string(b)
}
