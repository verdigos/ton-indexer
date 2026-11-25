package prices

import (
	"context"
	"fmt"
	"math"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"golang.org/x/sync/singleflight"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
)

const (
	MaxAssets       = 100
	DefaultRange    = 24 * 60 * 60 // history range in seconds when start_utime is not given
	DefaultDecimals = 9            // TEP-64 default when the metadata has no decimals

	tonDecimals  = 9
	usdtDecimals = 6
)

func quoteDecimals(quote string) uint8 {
	if quote == quoteUsdt {
		return usdtDecimals
	}
	return tonDecimals
}

// historySteps maps each range ceiling (exclusive, seconds) to its step in seconds, shortest first, keeping
// responses under 1440 points. Ranges of a day or more start at 15-minute steps so rolling "last 24h" queries
// round to the same cache key for 15 minutes instead of 5.
var historySteps = []struct{ step, below uint32 }{
	{60, 6 * 60 * 60}, {5 * 60, 24 * 60 * 60}, {15 * 60, 15 * 24 * 60 * 60}, {60 * 60, 60 * 24 * 60 * 60},
	{4 * 60 * 60, 240 * 24 * 60 * 60}, {24 * 60 * 60, 1440 * 24 * 60 * 60}, {7 * 24 * 60 * 60, math.MaxUint32},
}

type Store interface {
	// Head returns the head; ErrNoHead when there is none yet.
	Head(ctx context.Context) (Head, error)
	// Latest returns the newest TON and USDT row of all assets.
	Latest(ctx context.Context) ([]Row, error)
	// History returns one asset's points in one quote, oldest first. The first point is the one in force at q.Start.
	History(ctx context.Context, q HistoryQuery) ([]Row, error)
}

type Decimals struct {
	Value uint8
	Known bool
}

// DecimalsFunc reads the decimals of jetton masters (raw addresses). A master missing from the result is unknown.
type DecimalsFunc func(ctx context.Context, masters []string) (map[string]Decimals, error)

type Service struct {
	LatestRefresh time.Duration // age at which the snapshot of newest prices starts a reload in the background
	LatestMaxAge  time.Duration // age from which a request waits for the reload
	LatestTimeout time.Duration // limit of one reload
	DecimalsTTL   time.Duration

	store    Store
	decimals DecimalsFunc

	snapshot      atomic.Pointer[latestSnapshot]
	reload        singleflight.Group
	decimalsMu    sync.RWMutex
	decimalsCache map[string]cachedDecimals
}

func NewService(store Store, decimals DecimalsFunc) *Service {
	return &Service{LatestRefresh: 2 * time.Second, LatestMaxAge: 30 * time.Second, LatestTimeout: 3 * time.Second,
		DecimalsTTL: 5 * time.Minute, store: store, decimals: decimals, decimalsCache: map[string]cachedDecimals{}}
}

type QuotePrice struct {
	Price    *string `json:"value"`
	PriceRaw float64 `json:"value_raw"`
	Pools    uint16  `json:"pools"`
	Tvl      *string `json:"tvl"`
	AgeSec   uint32  `json:"age_sec"`
	Stale    bool    `json:"stale"` // true: uses the last GRAM/USDT anchor value; false does not mean fresh
} // @name DexQuotePrice

type AssetPrice struct {
	Asset    string      `json:"asset"`
	Decimals *uint8      `json:"decimals"`
	Gram     *QuotePrice `json:"gram"`
	Usdt     *QuotePrice `json:"usdt"`
	Reason   *string     `json:"reason" enums:"no_pools,no_eligible_pools,no_anchor"`
} // @name DexAssetPrice

type PricesResponse struct {
	Prices    []AssetPrice `json:"prices"`
	AsOfSeqno uint32       `json:"as_of_seqno"`
	AsOfUtime uint32       `json:"as_of_utime"`
} // @name DexPricesResponse

type Bar struct {
	Timestamp uint32   `json:"timestamp"` // unix seconds, start of the minute; the point is the last state in that minute
	Price     *string  `json:"value"`
	PriceRaw  *float64 `json:"value_raw"`
	Pools     uint16   `json:"pools"`
	Tvl       *string  `json:"tvl"`
	Stale     bool     `json:"stale"`                                      // true: uses the last GRAM/USDT anchor value
	Reason    *string  `json:"reason" enums:"no_eligible_pools,no_anchor"` // set when value_raw is null
	Final     bool     `json:"final"`                                      // false: the current minute, can still change
} // @name DexPricePoint

type HistoryResponse struct {
	Asset    string `json:"asset"`
	Quote    string `json:"quote" enums:"gram,usdt"`
	Decimals *uint8 `json:"decimals"`
	Step     uint32 `json:"step"` // seconds; each point is the newest change within its step
	Bars     []Bar  `json:"points"`
} // @name DexPriceHistoryResponse

type HistoryParams struct {
	Asset string
	Quote string // 'TON' | 'USDT'
	Start *uint32
	End   *uint32
	step  uint32 // step chosen before rounding the bounds
}

func (p HistoryParams) CacheKey() string {
	bound := func(v *uint32) string {
		if v == nil {
			return ""
		}
		return strconv.FormatUint(uint64(*v), 10)
	}
	return strings.Join([]string{p.Asset, p.Quote, bound(p.Start), bound(p.End), bound(&p.step)}, "|")
}

func invalid(format string, args ...any) error {
	return models.IndexError{Code: 400, Message: fmt.Sprintf(format, args...)}
}

func unavailable(err error) error {
	return models.IndexError{Code: 500, Message: "DEX prices unavailable: " + err.Error()}
}

func ParseAsset(value string) (string, error) {
	value = strings.TrimSpace(value)
	if strings.EqualFold(value, "gram") || strings.EqualFold(value, "ton") {
		return tonAsset, nil
	}
	addr, err := models.ParseAccountAddressStruct(value)
	if err != nil || addr.Kind != models.AddressStd {
		return "", invalid("invalid asset: %q (a jetton master address or `gram`)", value)
	}
	return addr.String(), nil
}

// The API calls Toncoin GRAM, its current name. The tables keep 'TON'. Requests also accept the old name.
const gramOut = "gram"

func assetOut(asset string) string {
	if asset == tonAsset {
		return gramOut
	}
	return asset
}

func quoteOut(quote string) string {
	if quote == quoteTon {
		return gramOut
	}
	return strings.ToLower(quote)
}

// ParsePricesParams parses the assets. It drops repeated assets and keeps the request order.
func ParsePricesParams(values []string) ([]string, error) {
	if len(values) == 0 {
		return nil, invalid("at least one asset is required")
	}
	assets := make([]string, 0, len(values))
	seen := map[string]bool{}
	for _, value := range values {
		asset, err := ParseAsset(value)
		if err != nil {
			return nil, err
		}
		if !seen[asset] {
			seen[asset] = true
			assets = append(assets, asset)
		}
	}
	if len(assets) > MaxAssets {
		return nil, invalid("too many assets: %d > %d", len(assets), MaxAssets)
	}
	return assets, nil
}

func parseUtime(name, value string) (*uint32, error) {
	if value == "" {
		return nil, nil
	}
	parsed, err := strconv.ParseUint(value, 10, 32)
	if err != nil {
		return nil, invalid("invalid %s: %q", name, value)
	}
	v := uint32(parsed)
	return &v, nil
}

func ParseHistoryParams(asset, quote, start, end string) (HistoryParams, error) {
	var p HistoryParams
	var err error
	if strings.TrimSpace(asset) == "" {
		return p, invalid("asset is required")
	}
	if p.Asset, err = ParseAsset(asset); err != nil {
		return p, err
	}
	switch strings.ToLower(strings.TrimSpace(quote)) {
	case "", "gram", "ton":
		p.Quote = quoteTon
	case "usdt":
		p.Quote = quoteUsdt
	default:
		return p, invalid("invalid quote: %q (gram or usdt)", quote)
	}
	if p.Start, err = parseUtime("start_utime", start); err != nil {
		return p, err
	}
	if p.End, err = parseUtime("end_utime", end); err != nil {
		return p, err
	}
	if p.Start != nil && p.End != nil && *p.Start > *p.End {
		return p, invalid("start_utime is after end_utime")
	}
	p.align(uint32(time.Now().Unix()))
	return p, nil
}

// align rounds the range out to whole steps, so requests for the same chart share one cached answer. A missing end
// is now.
func (p *HistoryParams) align(now uint32) {
	// A bound after now counts as now, so future values share the open-end answer instead of each costing a query.
	if p.End != nil && *p.End >= now {
		p.End = nil
	}
	if p.Start != nil && *p.Start > now {
		p.Start = &now
	}
	end := now
	if p.End != nil {
		end = *p.End
	}
	start := end - min(end, DefaultRange)
	if p.Start != nil {
		start = min(*p.Start, end)
	}
	step := historyStep(end - start)
	p.step = step
	if p.Start != nil {
		v := *p.Start / step * step
		p.Start = &v
	}
	if p.End != nil && *p.End/step < math.MaxUint32/step {
		v := (*p.End/step+1)*step - 1
		p.End = &v
	}
}

// parseDecimals reads the decimals field. On chain it is a string; in fetched metadata a string or a number.
func parseDecimals(values map[string]interface{}) (d uint8, present bool, ok bool) {
	raw, present := values["decimals"]
	if !present {
		return 0, false, false
	}
	switch v := raw.(type) {
	case string:
		n, err := strconv.ParseUint(strings.TrimSpace(v), 10, 8)
		return uint8(n), true, err == nil
	case float64:
		if v >= 0 && v <= 255 && v == math.Trunc(v) {
			return uint8(v), true, true
		}
	}
	return 0, true, false
}

// ResolveDecimals takes the decimals from the on-chain metadata, else from the fetched metadata. Without a decimals
// field it uses the default 9, but only for complete metadata: fetched, or on chain without a link to more.
func ResolveDecimals(content map[string]interface{}, info *models.TokenInfo) Decimals {
	if d, present, ok := parseDecimals(content); present {
		if ok {
			return Decimals{Value: d, Known: true}
		}
		return Decimals{}
	}
	fetched := info != nil && info.Valid != nil && *info.Valid
	if fetched {
		if d, present, ok := parseDecimals(info.Extra); present {
			if ok {
				return Decimals{Value: d, Known: true}
			}
			return Decimals{}
		}
	}
	_, hasUri := content["uri"]
	_, hasUrl := content["url"]
	if fetched || (content != nil && !hasUri && !hasUrl) {
		return Decimals{Value: DefaultDecimals, Known: true}
	}
	return Decimals{}
}

func finite(v *float64) *float64 {
	if v == nil || math.IsNaN(*v) || math.IsInf(*v, 0) {
		return nil
	}
	return v
}

// scaleDecimal moves the decimal point of v by exp places. It works on the shortest text form of v, so the result
// shows no float noise.
func scaleDecimal(v *float64, exp int) *string {
	if finite(v) == nil {
		return nil
	}
	s := strconv.FormatFloat(*v, 'e', -1, 64)
	i := strings.IndexByte(s, 'e')
	e, _ := strconv.Atoi(s[i+1:])
	scaled, err := strconv.ParseFloat(s[:i]+"e"+strconv.Itoa(e+exp), 64)
	if err != nil {
		return nil
	}
	out := strconv.FormatFloat(scaled, 'f', -1, 64)
	return &out
}

func (s *Service) assetDecimals(ctx context.Context, assets []string) (map[string]Decimals, error) {
	masters := make([]string, 0, len(assets))
	for _, asset := range assets {
		if asset != tonAsset {
			masters = append(masters, asset)
		}
	}
	res := map[string]Decimals{}
	if len(masters) > 0 && s.decimals != nil {
		var err error
		if res, err = s.cachedDecimalsOf(ctx, masters); err != nil {
			return nil, unavailable(err)
		}
	}
	res[tonAsset] = Decimals{Value: tonDecimals, Known: true}
	return res, nil
}

// priceExp is the power of ten from a raw price to a price in whole units.
func priceExp(d Decimals, quote string) (int, bool) {
	return int(d.Value) - int(quoteDecimals(quote)), d.Known
}

func tvl(v *float64, quote string) *string {
	return scaleDecimal(v, -int(quoteDecimals(quote)))
}

func isQuoteItself(asset, quote string) bool {
	return (quote == quoteTon && asset == tonAsset) || (quote == quoteUsdt && asset == UsdtMaster)
}

func quotePrice(asset, quote string, row *Row, d Decimals, head Head) (*QuotePrice, string) {
	if isQuoteItself(asset, quote) {
		one := "1"
		return &QuotePrice{Price: &one, PriceRaw: 1}, ""
	}
	if row == nil {
		return nil, "no_pools"
	}
	price := finite(row.PriceRaw)
	if price == nil {
		if row.Reason == "" {
			return nil, "no_eligible_pools"
		}
		return nil, row.Reason
	}
	qp := &QuotePrice{
		PriceRaw: *price,
		Pools:    row.Pools,
		Tvl:      tvl(row.TvlQuote, quote),
		Stale:    row.Stale,
	}
	if head.Utime > row.NewestUtime {
		qp.AgeSec = head.Utime - row.NewestUtime
	}
	if exp, ok := priceExp(d, quote); ok {
		qp.Price = scaleDecimal(price, exp)
	}
	return qp, ""
}

func decimalsOut(d Decimals) *uint8 {
	if !d.Known {
		return nil
	}
	return &d.Value
}

func (s *Service) Prices(ctx context.Context, assets []string) (*PricesResponse, error) {
	snapshot, err := s.latest(ctx)
	if err != nil {
		return nil, err
	}
	head, latest := snapshot.head, snapshot.rows
	// Decimals only for assets with a price row: unknown addresses cost no kvrocks reads and do not fill the cache.
	priced := make([]string, 0, len(assets))
	for _, asset := range assets {
		if latest[[2]string{asset, quoteTon}] != nil {
			priced = append(priced, asset)
		}
	}
	decimals, err := s.assetDecimals(ctx, priced)
	if err != nil {
		return nil, err
	}
	res := &PricesResponse{Prices: make([]AssetPrice, 0, len(assets)), AsOfSeqno: head.Seqno, AsOfUtime: head.Utime}
	for _, asset := range assets {
		d := decimals[asset]
		ap := AssetPrice{Asset: assetOut(asset)}
		ap.Decimals = decimalsOut(d)
		var tonReason, usdtReason string
		ap.Gram, tonReason = quotePrice(asset, quoteTon, latest[[2]string{asset, quoteTon}], d, head)
		ap.Usdt, usdtReason = quotePrice(asset, quoteUsdt, latest[[2]string{asset, quoteUsdt}], d, head)
		if ap.Gram == nil {
			ap.Reason = &tonReason
		} else if ap.Usdt == nil {
			ap.Reason = &usdtReason
		}
		res.Prices = append(res.Prices, ap)
	}
	return res, nil
}

func historyStep(seconds uint32) uint32 {
	for _, s := range historySteps {
		if seconds < s.below {
			return s.step
		}
	}
	return historySteps[len(historySteps)-1].step
}

// History returns one asset's points. A missing end defaults to the live head; a missing start is DefaultRange before the end.
func (s *Service) History(ctx context.Context, p HistoryParams) (*HistoryResponse, error) {
	snapshot, err := s.latest(ctx)
	if err != nil {
		return nil, err
	}
	q := HistoryQuery{Asset: p.Asset, Quote: p.Quote, End: snapshot.head.Utime, Open: snapshot.rows[[2]string{p.Asset, quoteTon}]}
	if p.End != nil {
		q.End = *p.End
	}
	if p.Start != nil {
		q.Start = *p.Start
	} else if q.End > DefaultRange {
		q.Start = q.End - DefaultRange
	}
	if q.Start > q.End {
		return nil, invalid("start_utime is after the newest indexed block")
	}
	q.Step = p.step
	if q.Step == 0 {
		q.Step = historyStep(q.End - q.Start)
	}
	rows, err := s.store.History(ctx, q)
	if err != nil {
		return nil, unavailable(err)
	}
	var assets []string
	if len(rows) > 0 {
		assets = []string{p.Asset}
	}
	decimals, err := s.assetDecimals(ctx, assets)
	if err != nil {
		return nil, err
	}
	d := decimals[p.Asset]
	exp, known := priceExp(d, p.Quote)
	res := &HistoryResponse{Asset: assetOut(p.Asset), Quote: quoteOut(p.Quote), Step: q.Step,
		Bars: make([]Bar, 0, len(rows))}
	res.Decimals = decimalsOut(d)
	for _, row := range rows {
		bar := Bar{
			Timestamp: row.Minute,
			PriceRaw:  finite(row.PriceRaw),
			Pools:     row.Pools,
			Tvl:       tvl(row.TvlQuote, p.Quote),
			Stale:     row.Stale,
			Final:     row.Final,
		}
		if bar.PriceRaw == nil {
			reason := row.Reason
			if reason == "" {
				reason = "no_eligible_pools"
			}
			bar.Reason = &reason
		} else if known {
			bar.Price = scaleDecimal(bar.PriceRaw, exp)
		}
		res.Bars = append(res.Bars, bar)
	}
	return res, nil
}
