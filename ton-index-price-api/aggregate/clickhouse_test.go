package aggregate

import (
	"cmp"
	"context"
	"fmt"
	"math"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/toncenter/ton-indexer/ton-index-price-api/prices"
)

// Prices worked out by hand on synthetic snapshots, run through the real SQL on ClickHouse.
// Units are raw: TON 1e9, USDT 1e6, jettons 1e9. Anchor pools hold 1M and 500k TON at 3 USDT per TON.

const (
	usdtMaster = prices.UsdtMaster
	pton1      = "0:8CDC1D7640AD5EE326527FC1AD0514F468B30DC84B0173F0E155F451B4E11F7C"
	pton2      = "0:671963027F7F85659AB55B821671688601CDCF1EE674FC7FBBB1A776A18D34A3"
	anchor     = 0.003
	day        = int64(86400)
	m0         = int64(1767225600) // 2026-01-01 00:00 UTC
)

func M(k int64) int64 { return m0 + 60*k }

func addr(prefix, name string) string {
	return "0:" + (prefix + strings.Repeat(fmt.Sprintf("%X", name), 64))[:64]
}

func jetton(name string) string { return addr("", name) }

type snapOpt struct {
	removed, locked bool
	dex, writer     string
	mathVersion     int     // 0: 1 for a usable pool
	spot            float64 // 0: r1 / r0
	feeBps          int
}

// snap is one pool_snapshots row with spot = r1 / r0 for a usable pool.
func snap(pool string, ts int64, a0, a1 string, r0, r1 float64, o snapOpt) string {
	spot, depth0, depth1, mv := "NULL", "NULL", "NULL", 0
	if !o.removed && r0 > 0 && r1 > 0 {
		f := 1/math.Sqrt(0.98) - 1
		spot, depth0, depth1, mv = fmt.Sprint(cmp.Or(o.spot, r1/r0)), fmt.Sprint(r0*f), fmt.Sprint(r1*f), 1
	}
	return fmt.Sprintf("('%s', %d, %d, '%s', %v, '%s', '%s', %d, %d, %v, %s, %s, %s, %d, '%s', %d, 10000)",
		addr("F0", pool), 50_000_000+ts-m0+30*day, ts, cmp.Or(o.dex, "stonfi"), o.removed, a0, a1,
		uint64(r0), uint64(r1), o.locked, spot, depth0, depth1, cmp.Or(o.mathVersion, mv), cmp.Or(o.writer, "live"),
		o.feeBps)
}

func anchorPools(ts int64) string {
	return snap("anc1", ts, pton1, usdtMaster, 1e15, 3e12, snapOpt{}) + ", " +
		snap("anc2", ts, usdtMaster, "", 1.5e12, 5e14, snapOpt{dex: "dedust"})
}

type env struct {
	t     *testing.T
	ctx   context.Context
	db    *ClickHouse
	store *prices.ClickHouseStore
}

func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}

func newEnv(t *testing.T) *env {
	dsn := os.Getenv("DEX_PRICES_TEST_DSN")
	if dsn == "" {
		t.Skip("set DEX_PRICES_TEST_DSN to a ClickHouse server, e.g. clickhouse://127.0.0.1:9000")
	}
	ctx := context.Background()
	root, err := prices.Connect(dsn)
	must(t, err)
	name := fmt.Sprintf("dex_prices_test_%d", time.Now().UnixNano())
	t.Cleanup(func() {
		root.Exec(ctx, "DROP DATABASE IF EXISTS "+name)
		root.Close()
	})
	schema, err := os.ReadFile("../../ton-index-worker/ton-dex-prices/sql/schema.sql")
	must(t, err)
	for _, s := range strings.Split(strings.ReplaceAll(string(schema), "dex_prices", name), ";") {
		if strings.TrimSpace(s) != "" {
			must(t, root.Exec(ctx, s))
		}
	}
	dsn = strings.TrimRight(dsn, "/") + "/" + name
	db, err := OpenClickHouse(dsn)
	must(t, err)
	t.Cleanup(func() { db.Close() })
	store, err := prices.OpenClickHouse(dsn)
	must(t, err)
	t.Cleanup(func() { store.Close() })
	e := &env{t, ctx, db, store}
	// The indexer's view over its blocks table; a plain table here.
	e.exec(`CREATE TABLE mc_blocks (mc_seqno Int32, block_utime Int32) ENGINE = MergeTree ORDER BY mc_seqno`)
	e.head(1, M(15))
	return e
}

// head adds an indexed masterchain block and moves the live head as the aggregator does.
func (e *env) head(seqno, utime int64) {
	e.t.Helper()
	e.exec(fmt.Sprintf(`INSERT INTO mc_blocks VALUES (%d, %d)`, seqno, utime))
	must(e.t, e.db.Exec(e.ctx, statement(stmtHead), nil))
}

func (e *env) exec(query string) {
	e.t.Helper()
	must(e.t, e.db.conn.Exec(e.ctx, query))
}

func (e *env) load(rows ...string) {
	e.exec(`INSERT INTO pool_snapshots (pool, mc_seqno, block_utime, dex, removed, asset0, asset1, reserve0, reserve1,
		is_locked, spot, depth0, depth1, math_version, writer, lp_fee, fee_denominator) VALUES ` + strings.Join(rows, ", "))
}

func (e *env) forward(maxRange int) *Job {
	return e.drain(NewJob(e.db, Options{Start: M(0), Until: M(9), MaxRange: maxRange}))
}

func (e *env) drain(j *Job) *Job {
	e.t.Helper()
	for {
		worked, err := j.Step(e.ctx)
		must(e.t, err)
		if !worked {
			return j
		}
	}
}

func (e *env) query(q prices.HistoryQuery, count int) []prices.Row {
	e.t.Helper()
	rows, err := e.store.History(e.ctx, q)
	if err != nil || len(rows) != count {
		e.t.Fatalf("history %+v: got %d rows %+v, error %v, want %d", q, len(rows), rows, err, count)
	}
	return rows
}

func (e *env) history(asset, quote string, count int) []prices.Row {
	e.t.Helper()
	return e.query(prices.HistoryQuery{Asset: asset, Quote: quote, Start: uint32(M(0)), End: uint32(M(9)), Step: 60}, count)
}

type pair struct{ asset, quote string }

func (e *env) latest() map[pair]prices.Row {
	e.t.Helper()
	rows, err := e.store.Latest(e.ctx)
	must(e.t, err)
	res := map[pair]prices.Row{}
	for _, r := range rows {
		res[pair{r.Asset, r.Quote}] = r
	}
	return res
}

func floatOrNil(v *float64) any {
	if v == nil {
		return nil
	}
	return *v
}

func near(v *float64, want float64) bool {
	return v != nil && math.Abs(*v-want) <= 1e-9*math.Abs(want)
}

// bar checks one row; price < 0 means a NULL price with the given reason.
func bar(t *testing.T, label string, r prices.Row, minute int64, price float64, pools uint16, stale bool, reason string) {
	t.Helper()
	priceOK := price < 0 && r.PriceRaw == nil || price >= 0 && near(r.PriceRaw, price)
	if !priceOK || r.Minute != uint32(minute) || r.Pools != pools || r.Stale != stale || r.Reason != reason {
		t.Errorf("%s: got minute %d price %v pools %d stale %v reason %q, want M%d %g %d %v %q", label, r.Minute,
			floatOrNil(r.PriceRaw), r.Pools, r.Stale, r.Reason, (minute-m0)/60, price, pools, stale, reason)
	}
}

func TestPricesOnClickHouse(t *testing.T) {
	t.Run("the live head stops at a missing block", func(t *testing.T) {
		e := newEnv(t)
		add := func(seqno, utime int64, want uint32) {
			t.Helper()
			e.head(seqno, utime)
			h, err := e.store.Head(e.ctx)
			if err != nil || h.Seqno != want {
				t.Fatalf("head %+v %v, want seqno %d", h, err, want)
			}
		}
		add(3, M(16), 1) // block 2 is still in flight
		add(2, M(15)+30, 3)
		e.head(3, M(16)) // a retried batch writes block 3 again
		add(5, M(17), 3)
		add(4, M(16)+30, 5)
		// Block 6 is in flight more than 100,000 blocks below the newest one.
		e.exec(fmt.Sprintf(`INSERT INTO mc_blocks SELECT number, %d FROM numbers(7, 100010)`, M(18)))
		add(100017, M(18), 5)
		add(6, M(17), 100005) // one search covers 100,000 blocks after the saved head
		add(100017, M(18), 100017)
	})

	t.Run("outlier and removed pools", func(t *testing.T) {
		e := newEnv(t)
		x, y := jetton("X1"), jetton("X11")
		e.load(anchorPools(M(0)+1),
			snap("p1a", M(0)+10, x, pton1, 5e15, 5e13, snapOpt{}),               // reference: 50k TON at 0.01
			snap("p1b", M(0)+10, "", x, 2.04e13, 2e15, snapOpt{dex: "dedust"}),  // 0.0102
			snap("p1c", M(0)+10, x, usdtMaster, 1e15, 2.97e10, snapOpt{}),       // 0.0099 via USDT
			snap("p1d", M(0)+10, x, pton2, 1315789474, 5e11, snapOpt{}),         // attacker: 38,000x
			snap("p1e", M(0)+10, x, pton1, 1.9e18, 5e11, snapOpt{}),             // inverse outlier
			snap("p1f", M(0)+10, x, pton2, 5e12, 5e10, snapOpt{}),               // below the 100 TON floor
			snap("p11a", M(0)+10, y, pton1, 1e15, 1e13, snapOpt{}),              // 0.01, 10k TON
			snap("p11b", M(0)+10, "", y, 5.05e12, 5e14, snapOpt{dex: "dedust"}), // 0.0101, 5.05k TON
			snap("p11a", M(2)+5, y, pton1, 0, 0, snapOpt{removed: true}),
			snap("p11b", M(3)+5, "", y, 0, 0, snapOpt{removed: true, dex: "dedust"}),
		)
		e.forward(10)
		latest := e.latest()
		bar(t, "latest TON", latest[pair{x, "TON"}], M(0), 0.01, 3, false, "")
		bar(t, "latest USDT", latest[pair{x, "USDT"}], M(0), 0.01*anchor, 3, false, "")
		// The kept pools hold 50k + 20.4k TON + 29.7k USDT (9.9k TON) and 5M + 2M + 1M X at 0.01: 160.3k TON.
		if l := latest[pair{x, "TON"}].TvlQuote; !near(l, 1.603e14) {
			t.Errorf("tvl: got %v raw TON, want 1.603e14", floatOrNil(l))
		}
		// Both sides of the two anchor pools sum to 3M TON, or 9M USDT at 3 USDT per TON.
		for _, c := range []struct {
			key        pair
			price, tvl float64
		}{
			{pair{"TON", "USDT"}, anchor, 9e12},
			{pair{usdtMaster, "TON"}, 1 / anchor, 3e15},
		} {
			row := latest[c.key]
			bar(t, c.key.quote, row, M(9), c.price, 2, false, "")
			points := e.history(c.key.asset, c.key.quote, 10)
			if !near(row.TvlQuote, c.tvl) || !near(points[len(points)-1].TvlQuote, c.tvl) {
				t.Errorf("anchor %s tvl: latest %v, history %v, want %g", c.key.quote,
					floatOrNil(row.TvlQuote), floatOrNil(points[len(points)-1].TvlQuote), c.tvl)
			}
		}

		ton := e.history(y, "TON", 3)
		bar(t, "two pools", ton[0], M(0), (1e13*0.01+5e12*0.0101)/1.5e13, 2, false, "")
		bar(t, "one left", ton[1], M(2), 0.0101, 1, false, "")
		bar(t, "none left", ton[2], M(3), -1, 0, false, "no_eligible_pools")
		bar(t, "latest TON", latest[pair{y, "TON"}], M(3), -1, 0, false, "no_eligible_pools")
		bar(t, "latest USDT", latest[pair{y, "USDT"}], M(3), -1, 0, false, "no_eligible_pools")
	})

	t.Run("reference and fee cap", func(t *testing.T) {
		e := newEnv(t)
		x, y, z := jetton("X17"), jetton("X18"), jetton("X19")
		e.load(anchorPools(M(0)+1),
			// Four pools at 0.01, and the largest one swapped up 8.2%: the median of all five keeps the four.
			snap("p17a", M(0)+10, x, pton1, 9.61353586e14, 1.0402e13, snapOpt{}),
			snap("p17b", M(0)+10, x, pton1, 1e15, 1e13, snapOpt{}),
			snap("p17c", M(0)+10, x, pton1, 1e15, 1e13, snapOpt{}),
			snap("p17d", M(0)+10, x, pton1, 1e15, 1e13, snapOpt{}),
			snap("p17e", M(0)+10, x, pton1, 1e15, 1e13, snapOpt{}),
			// The most TON, but a 25% weighted pool whose 45 Y are worth 90 TON at its own price of 2: below the floor.
			snap("p18w", M(0)+10, y, pton1, 45e9, 270e9, snapOpt{spot: 2}),
			snap("p18a", M(0)+10, y, pton1, 200e9, 200e9, snapOpt{}),
			// A 3% fee pool 5% above a 1% fee pool does not count.
			snap("p19a", M(0)+10, z, pton1, 1e15, 1e13, snapOpt{feeBps: 100}),
			snap("p19b", M(0)+10, z, pton1, 5e15, 5.25e13, snapOpt{feeBps: 300}),
		)
		e.forward(10)
		bar(t, "moved largest pool", e.history(x, "TON", 1)[0], M(0), 0.01, 4, false, "")
		bar(t, "weighted pool below the floor", e.history(y, "TON", 1)[0], M(0), 1, 1, false, "")
		bar(t, "fee cap", e.history(z, "TON", 1)[0], M(0), 0.01, 1, false, "")
	})

	t.Run("stale anchor", func(t *testing.T) {
		e := newEnv(t)
		x := jetton("X5")
		e.load(anchorPools(M(0)+1),
			snap("anc1", M(1)+5, pton1, usdtMaster, 1e15, 3.1e12, snapOpt{}),  // anchor moves to 0.0031
			snap("anc1", M(3)+5, pton1, usdtMaster, 5e13, 1.55e11, snapOpt{}), // below the 100k TON floor
			snap("anc2", M(3)+6, usdtMaster, "", 0, 0, snapOpt{removed: true, dex: "dedust"}),
			snap("p5a", M(0)+10, x, pton1, 1e15, 1e13, snapOpt{}), // 0.01 TON
			// At M2 only a pool below the floor changes: the row differs from M0 by its anchor alone.
			snap("p5b", M(2)+10, x, pton2, 5e12, 5e10, snapOpt{}),
			snap("p5a", M(3)+10, x, pton1, 1e15, 1e13, snapOpt{}), // trades back and forth: the price stays
		)
		e.forward(10)
		a := (0.003 + 0.0031 + 0.0031) / 3 // TWAP when the anchor goes stale at M3
		usdt := e.history(x, "USDT", 3)
		bar(t, "USDT at M0", usdt[0], M(0), 0.01*anchor, 1, false, "")
		bar(t, "USDT at M2", usdt[1], M(2), 0.01*a, 1, false, "")
		bar(t, "USDT at M3", usdt[2], M(3), 0.01*a, 1, true, "")
		staleAnchor := e.latest()[pair{"TON", "USDT"}]
		bar(t, "TON in USDT", staleAnchor, M(9), a, 0, true, "")
		if staleAnchor.TvlQuote != nil {
			t.Errorf("stale anchor tvl: %v, want NULL", *staleAnchor.TvlQuote)
		}
	})

	for _, size := range []int{1, 10} {
		t.Run(fmt.Sprintf("no rows before the first price/range %d", size), func(t *testing.T) {
			e := newEnv(t)
			x := jetton("X15")
			// Only a USDT pool and no anchor: no_anchor at M0, then no_eligible_pools at M1 when the pool goes.
			e.load(
				snap("p15u", M(0)+10, x, usdtMaster, 1e15, 1e10, snapOpt{}),
				snap("p15u", M(1)+10, x, usdtMaster, 0, 0, snapOpt{removed: true}),
			)
			e.forward(size)
			e.history(x, "TON", 0)
		})
		t.Run(fmt.Sprintf("the first anchor prices every asset/range %d", size), func(t *testing.T) {
			e := newEnv(t)
			x, y := jetton("X16"), jetton("Y16")
			e.load(
				snap("p16x", M(0)+10, x, pton1, 1e15, 1e13, snapOpt{}), // no USDT on chain yet
				snap("p16x", M(2)+10, x, pton1, 1e15, 1.2e13, snapOpt{}),
				snap("p16y", M(4)+10, y, usdtMaster, 1e15, 5e9, snapOpt{}), // Y has only a USDT pool
				anchorPools(M(6)+1),
				snap("p16x", M(7)+10, x, pton1, 1e15, 1.3e13, snapOpt{}),
			)
			e.forward(size)
			usdt := e.history(x, "USDT", 4)
			bar(t, "X before the anchor", usdt[1], M(2), -1, 1, false, "no_anchor")
			bar(t, "X when the anchor starts", usdt[2], M(6), 0.012*anchor, 1, false, "")
			bar(t, "X after", usdt[3], M(7), 0.013*anchor, 1, false, "")
			bar(t, "Y when the anchor starts", e.history(y, "TON", 1)[0], M(6), 5e-6/anchor, 1, false, "")
		})
	}

	t.Run("late data and a rewind", func(t *testing.T) {
		e := newEnv(t)
		x, y := jetton("X10"), jetton("X10Y")
		e.load(anchorPools(M(0)+1),
			snap("p10a", M(0)+10, x, pton1, 1e15, 1e13, snapOpt{}),
			snap("p10a", M(6)+10, x, pton1, 1e15, 1.1e13, snapOpt{}),
			snap("p10y", M(0)+10, y, pton1, 1e15, 1e13, snapOpt{}),           // Y at 0.01 TON
			snap("p10z", M(6)+10, y, pton2, 1e15, 1e13, snapOpt{}),           // a second Y pool from M6
			snap("anc1", M(5)+5, pton1, usdtMaster, 1e15, 3.3e12, snapOpt{}), // the anchor moves from M5
		)
		j := e.forward(4)
		if r := e.latest()[pair{y, "TON"}]; r.Minute != uint32(M(6)) || r.Pools != 2 {
			t.Fatalf("before the late data Y's newest row is M6 with 2 pools: %+v", r)
		}
		e.load(
			snap("p10a", M(3)+20, x, pton1, 1e15, 1.2e13, snapOpt{writer: "backfill"}), // X10 moves to 0.012
			// Newer math versions of the same blocks: the anchor did not move, Y's second pool is locked.
			snap("anc1", M(5)+5, pton1, usdtMaster, 1e15, 3e12, snapOpt{writer: "backfill", mathVersion: 2}),
			snap("p10z", M(6)+10, y, pton2, 1e15, 1e13, snapOpt{locked: true, writer: "backfill", mathVersion: 2}),
		)
		// The rewind deletes Y's M6 row, and the same row as M0 is not written again; with price_latest merged, Y must
		// come back from price_1m (the M0 row).
		e.exec("OPTIMIZE TABLE price_latest FINAL")
		must(t, j.Rewind(e.ctx, M(5)))
		must(t, j.Rewind(e.ctx, M(5))) // retry before the first step
		e.forward(4)
		bar(t, "Y latest after the first rewind", e.latest()[pair{y, "TON"}], M(0), 0.01, 1, false, "")
		must(t, j.Rewind(e.ctx, M(3)))
		e.forward(4)
		ton := e.history(x, "TON", 3)
		bar(t, "M3", ton[1], M(3), 0.012, 1, false, "")
		bar(t, "M6", ton[2], M(6), 0.011, 1, false, "")
	})
	t.Run("rewind with an older latest row", func(t *testing.T) {
		e := newEnv(t)
		// Keep the two ranges in separate parts of price_latest.
		e.exec("ALTER TABLE price_latest MODIFY SETTING max_bytes_to_merge_at_max_space_in_pool = 1, max_bytes_to_merge_at_min_space_in_pool = 1")
		y := jetton("Y")
		e.load(anchorPools(M(0)+1),
			snap("a", M(0)+10, y, pton1, 1e15, 1e13, snapOpt{}),
			snap("b", M(5)+10, y, pton2, 1e15, 1.2e13, snapOpt{}),
			snap("b", M(8)+10, y, pton2, 0, 0, snapOpt{removed: true}),
		)
		j := e.forward(5)
		must(t, j.Rewind(e.ctx, M(6)))
		j = NewJob(e.db, Options{Until: M(9), MaxRange: 1})
		if worked, err := j.Step(e.ctx); err != nil || !worked {
			t.Fatalf("rewind M6: %v %v", worked, err)
		}
		bar(t, "restored latest", e.latest()[pair{y, "TON"}], M(5), 0.012, 1, false, "")
		e.drain(j)
		bar(t, "latest after removal", e.latest()[pair{y, "TON"}], M(8), 0.01, 1, false, "")
	})

	t.Run("history ranges and steps", func(t *testing.T) {
		e := newEnv(t)
		D := func(d, k int64) int64 { return m0 + d*day + 60*k }
		x := jetton("X14")
		e.load(anchorPools(M(0)+1),
			snap("p14x", M(0)+10, x, pton1, 1e15, 1e13, snapOpt{}),           // 0.01 from day 0
			snap("p14x", D(1, 600)+10, x, pton1, 1e15, 2e13, snapOpt{}),      // 0.02 on day 1
			snap("p14x", D(1, 700)+10, x, pton1, 1e15, 3e13, snapOpt{}),      // 0.03 later that day
			snap("p14x", D(2, 1)+10, x, pton1, 0, 0, snapOpt{removed: true}), // no price from day 2
		)
		e.head(2, D(4, 0))
		e.drain(NewJob(e.db, Options{Start: M(0), Until: D(3, 9), MaxRange: 1440}))
		history := func(quote string, start, end int64, step uint32, count int) []prices.Row {
			t.Helper()
			return e.query(prices.HistoryQuery{Asset: x, Quote: quote, Start: uint32(start), End: uint32(end), Step: step}, count)
		}
		b := history("USDT", D(1, 0), D(3, 9), 60, 4)
		bar(t, "in force at the start", b[0], M(0), 0.01*anchor, 1, false, "")
		bar(t, "day 1", b[2], D(1, 700), 0.03*anchor, 1, false, "")
		bar(t, "without a price", b[3], D(2, 1), -1, 0, false, "no_eligible_pools")
		daily := history("TON", M(0), D(3, 9), 86400, 3)
		bar(t, "newest of day 1", daily[1], D(1, 700), 0.03, 1, false, "")
		quiet := history("TON", D(1, 601), D(1, 650), 60, 1)
		bar(t, "no changes in range", quiet[0], D(1, 600), 0.02, 1, false, "")
		history("TON", 0, M(0)-60, 60, 0)
	})

	t.Run("the current minute", func(t *testing.T) {
		e := newEnv(t) // live head at M15
		x := jetton("X12")
		e.load(anchorPools(M(0)+1),
			snap("p12a", M(0)+10, x, pton1, 1e15, 1e13, snapOpt{}),
			snap("p12a", M(15)+20, x, pton1, 1e15, 1.1e13, snapOpt{}), // the current minute
			snap("p12a", M(15)+40, x, pton1, 1e15, 1.2e13, snapOpt{}), // after the head: not used yet
		)
		e.head(2, M(15)+30)
		j := e.drain(NewJob(e.db, Options{Start: M(0)}))
		must(t, j.UpdateCurrentMinute(e.ctx))
		// History receives the asset's newest TON row even when the query asks for USDT.
		open := func() prices.Row { return e.latest()[pair{x, "TON"}] }
		bar(t, "latest from the current minute", open(), M(15), 0.011, 1, false, "")
		bars := func(quote string, step uint32, count int) []prices.Row {
			t.Helper()
			return e.query(prices.HistoryQuery{Asset: x, Quote: quote, Start: uint32(M(0)), End: uint32(M(15)),
				Step: step, Open: new(open())}, count)
		}
		checkM15 := func(row prices.Row, final bool, price float64) {
			t.Helper()
			if row.Final != final || row.Minute != uint32(M(15)) || !near(row.PriceRaw, price) {
				t.Errorf("got M%d final %v price %v, want M15 final %v price %g",
					(int64(row.Minute)-m0)/60, row.Final, floatOrNil(row.PriceRaw), final, price)
			}
		}
		checkOpen := func(quote string, step uint32, count int, price float64) {
			t.Helper()
			b := bars(quote, step, count)
			if count == 2 && !b[0].Final {
				t.Errorf("%s step %d: first bar should be final: %+v", quote, step, b[0])
			}
			checkM15(b[count-1], false, price)
		}
		checkOpen("TON", 60, 2, 0.011)
		checkOpen("TON", 3600, 1, 0.011)
		checkOpen("USDT", 60, 2, 0.011*anchor)
		e.head(3, M(16)+5)
		steps := rangeSteps(Range{From: M(15), To: M(15)}, doneOutputs, M(16)+5)
		must(t, j.exec(e.ctx, "final anchor", steps[:3]))
		checkM15(open(), false, 0.011)
		must(t, j.exec(e.ctx, "final prices", steps[3:]))
		checkM15(open(), true, 0.012)
		checkM15(bars("TON", 60, 2)[1], true, 0.012)
	})
}
