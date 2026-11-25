package aggregate

import (
	"fmt"
	"time"
)

// Progress tells how far the job is. Times are unix seconds; nil means not known yet.
type Progress struct {
	Head  *int64 // time of the newest block the job may use
	Done  *int64 // last minute already computed (nil: the job never ran)
	Ready *int64 // last minute that can be computed now
}

// Range is a range of minutes, both ends included, as unix seconds.
type Range struct {
	From, To int64
}

func (r Range) Minutes() int64 { return (r.To-r.From)/60 + 1 }

func (r Range) String() string {
	return fmt.Sprintf("%s..%s (%d min)", fmtMinute(r.From), fmtMinute(r.To), r.Minutes())
}

func fmtMinute(t int64) string {
	return time.Unix(t, 0).UTC().Format("2006-01-02 15:04")
}

func floorMinute(t int64) int64 { return t - t%60 }

// NextRange returns the minutes to compute next: from the minute after Done (or from start), up to Ready and until,
// and at most maxRange minutes.
func NextRange(p Progress, start, until int64, maxRange int) (r Range, ok bool) {
	if p.Ready == nil || maxRange < 1 {
		return Range{}, false
	}
	from := floorMinute(start)
	if p.Done != nil {
		from = *p.Done + 60
	}
	to := *p.Ready
	if until != 0 && floorMinute(until) < to {
		to = floorMinute(until)
	}
	if limit := from + int64(maxRange-1)*60; limit < to {
		to = limit
	}
	if from > to {
		return Range{}, false
	}
	return Range{From: from, To: to}, true
}
