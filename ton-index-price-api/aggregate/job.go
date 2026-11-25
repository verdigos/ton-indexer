package aggregate

import (
	"context"
	"fmt"
	"strconv"
	"strings"

	"github.com/toncenter/ton-indexer/ton-index-price-api/prices"
)

type Options struct {
	Start    int64                         // first minute when the job never ran (0: the minute of the oldest snapshot)
	Until    int64                         // last minute to compute (0: no limit; non-zero skips the current minute)
	MaxRange int                           // most minutes in one run of the statements
	Log      func(format string, v ...any) // one line per computed range; nil: no log
}

type Job struct {
	db          *ClickHouse
	opt         Options
	currentHead int64 // head time when the current minute was last computed
}

func NewJob(db *ClickHouse, opt Options) *Job {
	if opt.MaxRange < 1 {
		opt.MaxRange = 1440
	}
	return &Job{db: db, opt: opt}
}

// Step moves the head, then computes the next range of ready minutes. It returns true when it did some work, so the
// caller can go on without a pause.
func (j *Job) Step(ctx context.Context) (bool, error) {
	if err := j.db.Exec(ctx, statement(stmtHead), nil); err != nil {
		return false, fmt.Errorf("head: %w", err)
	}
	p, err := j.db.Progress(ctx)
	if err != nil {
		return false, err
	}
	start := j.opt.Start
	if p.Done == nil && start == 0 && p.Ready != nil {
		first, ok, err := j.db.FirstSnapshotMinute(ctx)
		if err != nil {
			return false, fmt.Errorf("first snapshot: %w", err)
		}
		if !ok {
			return false, nil
		}
		start = first
	}
	r, ok := NextRange(p, start, j.opt.Until, j.opt.MaxRange)
	if !ok {
		return false, nil
	}
	if err := j.runRange(ctx, r, *p.Head); err != nil {
		return true, err
	}
	if err := j.db.Exec(ctx, statement(stmtSaveDone), map[string]string{"done_minute": unix(r.To)}); err != nil {
		return true, fmt.Errorf("save done minute: %w", err)
	}
	return true, nil
}

// Rewind sets the done minute back to just before from. The next steps delete the results from that minute on and
// compute them again.
func (j *Job) Rewind(ctx context.Context, from int64) error {
	p, err := j.db.Progress(ctx)
	if err != nil {
		return err
	}
	if p.Done == nil || from-60 > *p.Done {
		return fmt.Errorf("rewind to %s: the job has not reached that minute", fmtMinute(from))
	}
	return j.exec(ctx, "rewind", []step{
		{stmtSaveDone, map[string]string{"done_minute": unix(from - 60)}},
		{stmtTruncate, map[string]string{"table": "anchor_now"}},
		{stmtTruncate, map[string]string{"table": "price_now"}},
	})
}

// UpdateCurrentMinute computes the minute that is not over yet, from the blocks up to the head. It runs only when the
// job has caught up and the head has moved. It writes into the *_next tables, then swaps them with the *_now tables.
func (j *Job) UpdateCurrentMinute(ctx context.Context) error {
	p, err := j.db.Progress(ctx)
	if err != nil {
		return err
	}
	if j.opt.Until != 0 || p.Done == nil || p.Ready == nil || p.Head == nil || *p.Done < *p.Ready ||
		*p.Head == j.currentHead {
		return nil
	}
	r := Range{From: *p.Done + 60, To: floorMinute(*p.Head)}
	steps := []step{{stmtTruncate, map[string]string{"table": "anchor_next"}}, {stmtTruncate, map[string]string{"table": "price_next"}}}
	if r.From <= r.To {
		steps = append(steps, rangeSteps(r, currentOutputs, *p.Head)...)
	}
	steps = append(steps, step{stmtSwap, map[string]string{"a": "anchor_now", "b": "anchor_next"}},
		step{stmtSwap, map[string]string{"a": "price_now", "b": "price_next"}})
	if err := j.exec(ctx, "current minute "+r.String(), steps); err != nil {
		return err
	}
	j.currentHead = *p.Head
	return nil
}

type step struct {
	name   string
	params map[string]string
}

func unix(t int64) string    { return strconv.FormatInt(t, 10) }
func float(v float64) string { return strconv.FormatFloat(v, 'f', -1, 64) }

func (j *Job) exec(ctx context.Context, label string, steps []step) error {
	for _, s := range steps {
		if err := j.db.Exec(ctx, statement(s.name), s.params); err != nil {
			return fmt.Errorf("%s: %s: %w", label, s.name, err)
		}
	}
	return nil
}

// runRange computes one range of minutes. If anchor rows already exist from its first minute, a rewind or a failed
// run left them: it deletes the old results first.
func (j *Job) runRange(ctx context.Context, r Range, head int64) error {
	again, err := j.db.AnchorRows(ctx, r.From)
	if err != nil {
		return fmt.Errorf("check range %s: %w", r, err)
	}
	var steps []step
	if again {
		del := func(table string) step {
			return step{stmtDelete, map[string]string{"table": table, "from_minute": unix(r.From)}}
		}
		// The anchor rows go last: while they are there, the next run knows the delete is not finished.
		steps = append(steps, del("price_latest"), step{stmtRefillLatest, map[string]string{"from_minute": unix(r.From)}},
			del("price_1m"), del("anchor_1m"))
	}
	if err := j.exec(ctx, "range "+r.String(), append(steps, rangeSteps(r, doneOutputs, head)...)); err != nil {
		return err
	}
	if j.opt.Log != nil {
		kind := "aggregated"
		if again {
			kind = "rewrote"
		}
		j.opt.Log("%s %s", kind, r)
	}
	return nil
}

var tonAliases = "['" + strings.Join(prices.TonAliases, "','") + "']"

// The tables a range writes: the stored results, or the *_next tables for the current minute.
type outputs struct{ anchor, price, current string }

var (
	doneOutputs    = outputs{"anchor_1m", "price_1m", "0"}
	currentOutputs = outputs{"anchor_next", "price_next", "1"}
)

// rangeSteps builds the SQL steps for one range. pool_records starts anchorTwap - 1 minutes early so the anchor
// TWAP has its preceding window.
func rangeSteps(r Range, out outputs, head int64) []step {
	return []step{
		{stmtTruncate, map[string]string{"table": "pool_records"}},
		{stmtPoolRecords, map[string]string{"records_from": unix(r.From - int64(anchorTwap-1)*60), "records_to": unix(r.To),
			"head_utime": unix(head), "ton_aliases": tonAliases, "usdt": prices.UsdtMaster,
			"max_pool_fee": float(maxPoolFee)}},
		{stmtAnchor, map[string]string{"from_minute": unix(r.From), "to_minute": unix(r.To), "anchor_out": out.anchor,
			"anchor_floor_ton": float(anchorFloorTon), "anchor_twap": strconv.FormatUint(uint64(anchorTwap), 10)}},
		{stmtPrices, map[string]string{"from_minute": unix(r.From), "to_minute": unix(r.To), "price_out": out.price,
			"current": out.current, "band": float(band), "min_pool_liquidity_ton": float(minPoolLiquidityTon)}},
	}
}
