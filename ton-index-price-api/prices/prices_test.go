package prices

import (
	"context"
	"encoding/json"
	"math"
	"reflect"
	"strings"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
)

const jettonA = "0:2F956143C461769579BAEF2E32CC2D7BC18283F40D20BB03E432CD603AC33FFC" // NOT

type fakeStore struct {
	head     Head
	rows     []Row
	onLatest func() error
}

func (f *fakeStore) Head(context.Context) (Head, error)                   { return f.head, nil }
func (f *fakeStore) History(context.Context, HistoryQuery) ([]Row, error) { return f.rows, nil }
func (f *fakeStore) Latest(context.Context) ([]Row, error) {
	if f.onLatest != nil {
		return f.rows, f.onLatest()
	}
	return f.rows, nil
}

func checkEqual[T any](t *testing.T, got, want T) {
	t.Helper()
	if !reflect.DeepEqual(got, want) {
		actual, _ := json.Marshal(got)
		expected, _ := json.Marshal(want)
		t.Errorf("got %s, want %s", actual, expected)
	}
}

func TestResolveDecimals(t *testing.T) {
	type metadata = map[string]any
	fetched := func(valid bool, extra metadata) *models.TokenInfo {
		return &models.TokenInfo{Valid: new(valid), Extra: extra}
	}
	link, six := metadata{"uri": "u"}, metadata{"decimals": "6"}
	for _, c := range []struct {
		name    string
		content metadata
		info    *models.TokenInfo
		want    Decimals
	}{
		{"onchain wins", metadata{"decimals": "8", "uri": "u"}, fetched(true, six), Decimals{8, true}},
		{"onchain invalid", metadata{"decimals": "abc"}, fetched(true, six), Decimals{}},
		{"offchain number", link, fetched(true, metadata{"decimals": 18.0}), Decimals{18, true}},
		{"offchain fractional", link, fetched(true, metadata{"decimals": 6.5}), Decimals{}},
		{"fetched without decimals", link, fetched(true, metadata{"x": 1}), Decimals{9, true}},
		{"onchain without decimals", metadata{"name": "X"}, nil, Decimals{9, true}},
		{"fetch failed", link, fetched(false, six), Decimals{}},
		{"no metadata at all", nil, nil, Decimals{}},
	} {
		if got := ResolveDecimals(c.content, c.info); got != c.want {
			t.Errorf("%s: got %+v, want %+v", c.name, got, c.want)
		}
	}
}

func TestScaleDecimal(t *testing.T) {
	checkEqual(t, scaleDecimal(new(1.23e-8), 0), new("0.0000000123"))
	checkEqual(t, scaleDecimal(new(1e300), 20), nil)
}

func TestPrices(t *testing.T) {
	store := &fakeStore{head: Head{51234567, 1791234567}, rows: []Row{
		{Asset: jettonA, Quote: "TON", Minute: 1791234480, PriceRaw: new(0.0123), Pools: 3,
			TvlQuote: new(51234.5e9), NewestUtime: 1791234525},
		{Asset: jettonA, Quote: "USDT", Minute: 1791234480, PriceRaw: new(0.0000369), Pools: 3,
			TvlQuote: new(math.NaN()), NewestUtime: 1791234525, Stale: true},
		{Asset: "TON", Quote: "USDT", Minute: 1791234480, PriceRaw: new(0.003), Pools: 2, NewestUtime: 1791234480},
		{Asset: UsdtMaster, Quote: "TON", Minute: 1791234480, PriceRaw: new(333.3333), Pools: 2, NewestUtime: 1791234480},
	}}
	var asked []string
	svc := NewService(store, func(_ context.Context, masters []string) (map[string]Decimals, error) {
		asked = append(asked, masters...)
		return map[string]Decimals{jettonA: {9, true}, UsdtMaster: {6, true}}, nil
	})
	other := "0:" + strings.Repeat("A", 64)
	res, err := svc.Prices(t.Context(), []string{jettonA, "TON", UsdtMaster, other})
	if err != nil {
		t.Fatal(err)
	}
	checkEqual(t, asked, []string{jettonA, UsdtMaster})
	one := &QuotePrice{Price: new("1"), PriceRaw: 1}
	checkEqual(t, res, &PricesResponse{AsOfSeqno: 51234567, AsOfUtime: 1791234567, Prices: []AssetPrice{
		{Asset: jettonA, Decimals: new(uint8(9)),
			Gram: &QuotePrice{Price: new("0.0123"), PriceRaw: 0.0123, Pools: 3, Tvl: new("51234.5"), AgeSec: 42},
			Usdt: &QuotePrice{Price: new("0.0369"), PriceRaw: 0.0000369, Pools: 3, AgeSec: 42, Stale: true}},
		{Asset: "gram", Decimals: new(uint8(9)), Gram: one,
			Usdt: &QuotePrice{Price: new("3"), PriceRaw: 0.003, Pools: 2, AgeSec: 87}},
		{Asset: UsdtMaster, Decimals: new(uint8(6)), Usdt: one,
			Gram: &QuotePrice{Price: new("0.3333333"), PriceRaw: 333.3333, Pools: 2, AgeSec: 87}},
		{Asset: other, Reason: new("no_pools")},
	}})
}

func TestEmptyHistoryAndRoundedStep(t *testing.T) {
	const start = uint32(1_800_000_000)
	const head = start + 6*3600 + 29
	svc := NewService(&fakeStore{head: Head{Utime: head}}, func(context.Context, []string) (map[string]Decimals, error) {
		t.Error("decimals requested for empty history")
		return nil, context.DeadlineExceeded
	})
	p := HistoryParams{Asset: jettonA, Quote: "TON"}
	res, err := svc.History(t.Context(), p)
	if err != nil || res.Bars == nil || len(res.Bars) != 0 || res.Decimals != nil {
		t.Fatalf("empty history: %+v %v", res, err)
	}
	short, long := p, p
	short.Start, long.Start = new(start+59), new(start) // 5h59m30s and 6h00m29s
	short.align(head)
	long.align(head)
	checkEqual(t, short.Start, new(start))
	checkEqual(t, long.Start, new(start))
	if short.CacheKey() == long.CacheKey() {
		t.Error("ranges with different steps share a cache key")
	}
	if res, err := svc.History(t.Context(), short); err != nil || res.Step != 60 {
		t.Errorf("rounded history: %+v %v, want a 60-second step", res, err)
	}
}

func TestAlign(t *testing.T) {
	const now = uint32(1_800_000_000)
	u := func(v uint32) *uint32 { return &v }
	for _, c := range []struct{ start, end, wantStart, wantEnd *uint32 }{
		{u(1000), u(4600), u(960), u(4619)},              // an hour: 1-minute steps
		{nil, u(now - 7), nil, u((now-7)/900*900 + 899)}, // the default day: 15-minute steps
		{nil, u(now + 7), nil, nil},                      // a future end counts as now
	} {
		p := HistoryParams{Start: c.start, End: c.end}
		p.align(now)
		checkEqual(t, p.Start, c.wantStart)
		checkEqual(t, p.End, c.wantEnd)
	}
}
