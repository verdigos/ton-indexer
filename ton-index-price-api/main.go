package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"strconv"
	"strings"
	"time"

	"github.com/gofiber/fiber/v2"
	"github.com/gofiber/fiber/v2/middleware/recover"
	"github.com/gofiber/swagger"
	"github.com/toncenter/ton-indexer/ton-index-go/index/crud"
	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
	_ "github.com/toncenter/ton-indexer/ton-index-price-api/docs"
	"github.com/toncenter/ton-indexer/ton-index-price-api/prices"
)

// @title TON Index Price API
// @version 1.0
// @description Jetton values in GRAM and USDT, computed from the on-chain reserves of selected STON.fi and DeDust pools.
// @description A value aggregates pool mid prices, before fees and price impact. It is not a quote, a trade price or a
// @description fiat price, and it is not offered as a valuation, benchmark or reference rate. USDT is a token, not US
// @description dollars. The pools and thresholds can change. Values can be late, wrong or missing, for example when a
// @description pool is thin or manipulated. They are not advice or a recommendation.
// @BasePath /
// @query.collection.format multi
// @securitydefinitions.apikey APIKeyHeader
// @in header
// @name X-Api-Key
// @securitydefinitions.apikey APIKeyQuery
// @in query
// @name api_key

var dexPrices *prices.Service
var dexHistory *prices.ResponseCache
var kvrocksStore *crud.KvrocksStore
var queryTimeout time.Duration

func dexPricesDecimals(ctx context.Context, masters []string) (map[string]prices.Decimals, error) {
	addresses := make([]models.AccountAddress, len(masters))
	for i, master := range masters {
		addresses[i] = models.AccountAddress(master)
	}
	found, err := kvrocksStore.GetJettonMasters(ctx, addresses)
	if err != nil {
		return nil, err
	}
	// Fetched metadata matters only where the on-chain content has no decimals field.
	var unresolved []models.AccountAddress
	for address, master := range found {
		if _, ok := master.JettonContent["decimals"]; !ok {
			unresolved = append(unresolved, address)
		}
	}
	var metadata models.Metadata
	if len(unresolved) > 0 {
		request_settings := models.RequestSettings{NoAddressBook: true}
		if deadline, ok := ctx.Deadline(); ok {
			request_settings.Timeout = time.Until(deadline)
		}
		if metadata, err = crud.QueryMetadataImplKvrocks(unresolved, request_settings, kvrocksStore); err != nil {
			return nil, err
		}
	}
	res := make(map[string]prices.Decimals, len(found))
	for address, master := range found {
		res[string(address)] = prices.ResolveDecimals(master.JettonContent, jettonMasterInfo(metadata[address].TokenInfo))
	}
	return res, nil
}

// jettonMasterInfo returns the newest jetton_masters row.
func jettonMasterInfo(rows []models.TokenInfo) *models.TokenInfo {
	for i := len(rows) - 1; i >= 0; i-- {
		if rows[i].Type != nil && *rows[i].Type == "jetton_masters" {
			return &rows[i]
		}
	}
	return nil
}

// @summary Get Jetton Prices
//
// @description Newest computed values of jettons in GRAM and USDT from selected DEX pools (STON.fi, DeDust).
// @description `value_raw` is raw quote units per raw asset unit. `value` is whole quote per whole asset, present when
// @description the asset's decimals are known (on chain, from fetched metadata, or the default 9). Wrong decimals make
// @description `value` wrong. `pools` is the number of pools used; 1 means no cross-check. `tvl` is the value of both
// @description reserves of the pools used (total value locked) in whole quote units. For GRAM/USDT and USDT/GRAM these
// @description are the anchor pools of the newest minute. It does not show how much can be traded.
// @description `age_sec` is the time from the newest state change of the pools used to the newest indexed block
// @description (`as_of_*`). For GRAM/USDT and USDT/GRAM it counts from the start of the anchor minute to that block.
// @description It has no upper limit. A USDT value is the GRAM value times the GRAM/USDT anchor, a
// @description value from the largest GRAM/USDT pools. `stale` is true when a value uses the anchor and no GRAM/USDT pool was
// @description eligible in that minute, so the last anchor value applies. `stale` false does not mean fresh: read
// @description `age_sec`. USDT in USDT is 1. An asset without a value in a quote has `null` there and a `reason`.
//
// @id api_v3_get_jetton_prices
// @tags jettons
// @Accept json
// @Produce json
// @success 200 {object} prices.PricesResponse
// @failure 400 {object} models.RequestError
// @failure 500 {object} models.RequestError
// @param asset query []string true "Jetton master address in any form, or `gram` for GRAM. Max: 100." collectionFormat(multi)
// @router /api/v3/jetton/prices [get]
// @security		APIKeyHeader
// @security		APIKeyQuery
func GetJettonPrices(c *fiber.Ctx) error {
	values := []string{}
	for _, value := range c.Context().QueryArgs().PeekMulti("asset") {
		values = append(values, string(value))
	}
	assets, err := prices.ParsePricesParams(values)
	if err != nil {
		return err
	}
	request_settings := GetRequestSettings(c)
	ctx, cancel := context.WithTimeout(context.Background(), request_settings.Timeout)
	defer cancel()
	res, err := dexPrices.Prices(ctx, assets)
	if err != nil {
		return err
	}
	return c.JSON(res)
}

// @summary Get Jetton Price History
//
// @description Computed value points of a jetton in GRAM or USDT, oldest first. A point is not a trade: it marks a change
// @description of value, pools, tvl or state at a minute where one of the jetton's pools changed; carry it forward.
// @description `timestamp` is the start of that minute in unix seconds; the point is the last state within the minute.
// @description Fields mean the same as in /api/v3/jetton/prices. A USDT point is the GRAM value times the GRAM/USDT anchor
// @description of the same minute. The first point is the one in force at `start_utime`
// @description and may be older. Long ranges are thinned to the newest point of each `step` seconds, about 1440 points at
// @description most; a range of a day or more has steps of 15 minutes or longer. The range is rounded out to whole steps.
// @description Points with `final` false belong to the current minute and can still change.
//
// @id api_v3_get_jetton_price_history
// @tags jettons
// @Accept json
// @Produce json
// @success 200 {object} prices.HistoryResponse
// @failure 400 {object} models.RequestError
// @failure 500 {object} models.RequestError
// @param asset query string true "Jetton master address in any form, or `gram` for GRAM."
// @param quote query string false "Quote asset." Enums(gram, usdt) default(gram)
// @param start_utime query int32 false "Start of the range (default: 24 hours before end_utime)." minimum(0)
// @param end_utime query int32 false "End of the range (default: the newest indexed block; a later time counts as the default)." minimum(0)
// @router /api/v3/jetton/price_history [get]
// @security		APIKeyHeader
// @security		APIKeyQuery
func GetJettonPriceHistory(c *fiber.Ctx) error {
	params, err := prices.ParseHistoryParams(c.Query("asset"), c.Query("quote"), c.Query("start_utime"),
		c.Query("end_utime"))
	if err != nil {
		return err
	}
	request_settings := GetRequestSettings(c)
	ctx, cancel := context.WithTimeout(context.Background(), request_settings.Timeout)
	defer cancel()
	body, err := dexHistory.Get(ctx, params.CacheKey(), func(ctx context.Context) ([]byte, error) {
		res, err := dexPrices.History(ctx, params)
		if err != nil {
			return nil, err
		}
		return json.Marshal(res)
	})
	if err != nil {
		return err
	}
	c.Set(fiber.HeaderContentType, fiber.MIMEApplicationJSON)
	return c.Send(body)
}

func GetRequestSettings(c *fiber.Ctx) models.RequestSettings {
	settings := models.RequestSettings{Timeout: queryTimeout}
	value := c.Get("X-Timeout")
	if value == "" {
		value = c.Query("x_timeout")
	}
	// Floor at 500 ms: a near-zero timeout returns immediately while the ClickHouse query keeps running for the cache.
	if timeout, err := strconv.ParseInt(value, 10, 32); err == nil {
		settings.Timeout = time.Duration(min(max(timeout, 500), 3000)) * time.Millisecond
	}
	return settings
}

func ErrorHandlerFunc(c *fiber.Ctx, err error) error {
	switch e := err.(type) {
	case models.IndexError:
		if e.Code != 404 && e.Code != 409 {
			log.Printf("Code: %d Path: %s IP: %s Error: %s", e.Code, c.Path(), c.IP(), strings.ReplaceAll(err.Error(), "\n", "\\n"))
		}
		return c.Status(e.Code).JSON(e)
	case *fiber.Error:
		return c.Status(e.Code).JSON(fiber.Map{"error": e.Message})
	default:
		log.Printf("Path: %s IP: %s Error: %s", c.Path(), c.IP(), strings.ReplaceAll(err.Error(), "\n", "\\n"))
		return c.Status(500).JSON(fiber.Map{"error": fmt.Sprintf("internal server error: %s", err)})
	}
}

func main() {
	var config crud.KvrocksConfig
	var sentinels string
	var refreshMS int64
	dsn := flag.String("clickhouse", "", "ClickHouse DSN of the prices database (required)")
	bind := flag.String("bind", ":8000", "Bind address")
	timeoutMS := flag.Int("query-timeout", 3000, "Query timeout in milliseconds")
	prefork := flag.Bool("prefork", false, "Prefork workers")
	historyTTL := flag.Duration("history-cache-ttl", 30*time.Second, "Lifetime of cached price history answers; 0 turns the cache off")
	historyMB := flag.Int64("history-cache-mb", 1024, "Size limit of cached price history answers in MiB")
	latestRefresh := flag.Duration("latest-refresh", 2*time.Second, "Age at which the newest prices held in memory are read again")
	flag.StringVar(&config.Addr, "kvrocks", "", "Kvrocks address or Redis URL for point-state reads")
	flag.StringVar(&sentinels, "kvrocks-sentinels", "", "Comma-separated Kvrocks Sentinel addresses")
	flag.StringVar(&config.SentinelMasterName, "kvrocks-sentinel-master", "", "Kvrocks Sentinel master name")
	flag.StringVar(&config.Username, "kvrocks-user", "", "Kvrocks username")
	flag.StringVar(&config.Password, "kvrocks-password", "", "Kvrocks password")
	flag.IntVar(&config.DB, "kvrocks-db", 0, "Kvrocks database number")
	flag.BoolVar(&config.ReplicaReads, "kvrocks-replica-reads", false, "Read from Kvrocks replicas discovered via Sentinel")
	flag.Int64Var(&config.StalenessBlocks, "kvrocks-staleness-blocks", 5, "Max watermark lag in masterchain blocks for a Kvrocks replica to serve reads")
	flag.Int64Var(&refreshMS, "kvrocks-replica-refresh-ms", 300, "Kvrocks replica discovery and freshness poll interval in milliseconds")
	flag.Parse()
	if *dsn == "" || (config.Addr == "" && sentinels == "") {
		log.Fatal("-clickhouse and one of -kvrocks/-kvrocks-sentinels are required")
	}
	config.SentinelAddrs = crud.ParseKvrocksSentinelAddrs(sentinels)
	config.ReplicaRefresh = time.Duration(refreshMS) * time.Millisecond
	queryTimeout = time.Duration(*timeoutMS) * time.Millisecond
	var err error
	kvrocksStore, err = crud.NewKvrocksStore(config)
	if err != nil {
		log.Fatal(err)
	}
	defer kvrocksStore.Close()
	store, err := prices.OpenClickHouse(*dsn)
	if err != nil {
		log.Fatal(err)
	}
	defer store.Close()
	dexPrices = prices.NewService(store, dexPricesDecimals)
	dexPrices.LatestRefresh, dexPrices.LatestTimeout = *latestRefresh, queryTimeout
	dexHistory = prices.NewResponseCache(*historyTTL, *historyMB<<20, queryTimeout)
	// 100 raw addresses make a URL of about 7.3 KB; the default 4 KB buffer rejects it. Each connection holds this buffer.
	app := fiber.New(fiber.Config{AppName: "TON Index Price API", Prefork: *prefork, ErrorHandler: ErrorHandlerFunc,
		ReadBufferSize: 16384})
	app.Use(recover.New())
	app.Use("/api/v3/", func(c *fiber.Ctx) error {
		start := time.Now()
		err := c.Next()
		c.Append("Server-timing", fmt.Sprintf("app;dur=%v", time.Since(start).String()))
		return err
	})
	app.Get("/api/v3/jetton/prices", GetJettonPrices)
	app.Get("/api/v3/jetton/price_history", GetJettonPriceHistory)
	app.Get("/healthcheck", func(c *fiber.Ctx) error { return c.SendString("OK") })
	app.Get("/api/prices/*", swagger.New(swagger.Config{Title: "TON Index Price API", Layout: "BaseLayout", DeepLinking: true, TryItOutEnabled: true}))
	log.Fatal(app.Listen(*bind))
}
