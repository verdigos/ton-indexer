package main

import (
	"strconv"
	"strings"
	"time"

	"github.com/gofiber/fiber/v2"
	"github.com/toncenter/ton-indexer/ton-index-go/index/dexprices"
	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
)

// Experimental DEX price endpoints over the dex-tap ClickHouse tables
// (docs/dex-prices/mvp-plan.md, stage M-1). Registered only with -clickhouse-dex-prices.

var dexPrices *dexprices.Store

type DexPricesResponse struct {
	Prices []dexprices.Price `json:"prices"`
} // @name DexPricesResponse

type DexPoolSnapshotsResponse struct {
	Snapshots []dexprices.PoolSnapshot `json:"snapshots"`
} // @name DexPoolSnapshotsResponse

// dexAsset converts an address in any form, or "ton", to the raw form the dex-tap tables use ("" is TON).
func dexAsset(value string) (string, error) {
	if strings.EqualFold(strings.TrimSpace(value), "ton") {
		return "", nil
	}
	addr, err := models.ParseAccountAddressStruct(value)
	if err != nil {
		return "", err
	}
	if addr.Kind != models.AddressStd {
		return "", models.IndexError{Code: 400, Message: "asset must be a standard address or `ton`"}
	}
	return strings.ToUpper(addr.String()), nil
}

func queryUint(c *fiber.Ctx, name string, fallback uint64) (uint64, error) {
	value := c.Query(name)
	if value == "" {
		return fallback, nil
	}
	parsed, err := strconv.ParseUint(value, 10, 64)
	if err != nil {
		return 0, models.IndexError{Code: 422, Message: "invalid " + name}
	}
	return parsed, nil
}

// @summary Get DEX prices (experimental)
//
// @description Current prices of jettons in TON and USDT, aggregated over DEX pool states by liquidity.
// @description Prototype: reads the dex-tap ClickHouse tables and may change without notice.
//
// @id api_v3_get_experimental_dex_prices
// @tags experimental
// @Accept json
// @Produce json
// @success 200 {object} DexPricesResponse
// @failure 400 {object} models.RequestError
// @param asset query []string false "Jetton master address in any form, or `ton`. Max 1000. Omit for all priced assets." collectionFormat(multi)
// @param min_liquidity_usdt query number false "Minimum pool liquidity in USDT." default(1000)
// @param max_age_days query int32 false "Use only pools changed within this many days." default(7)
// @router /api/v3/experimental/dex/prices [get]
// @security		APIKeyHeader
// @security		APIKeyQuery
func GetDexPrices(c *fiber.Ctx) error {
	var assets []string
	for _, value := range c.Context().QueryArgs().PeekMulti("asset") {
		asset, err := dexAsset(string(value))
		if err != nil {
			return models.IndexError{Code: 422, Message: "invalid asset: " + string(value)}
		}
		assets = append(assets, asset)
	}
	if len(assets) > 1000 {
		return models.IndexError{Code: 422, Message: "too many assets"}
	}
	minLiquidity := 1000.0
	if value := c.Query("min_liquidity_usdt"); value != "" {
		parsed, err := strconv.ParseFloat(value, 64)
		if err != nil || parsed < 0 {
			return models.IndexError{Code: 422, Message: "invalid min_liquidity_usdt"}
		}
		minLiquidity = parsed
	}
	maxAgeDays, err := queryUint(c, "max_age_days", 7)
	if err != nil {
		return err
	}
	prices, err := dexPrices.QueryPrices(c.UserContext(), dexprices.PriceParams{
		Assets:           assets,
		MaxAge:           time.Duration(maxAgeDays) * 24 * time.Hour,
		MinLiquidityUsdt: minLiquidity,
	})
	if err != nil {
		return err
	}
	return c.JSON(DexPricesResponse{Prices: prices})
}

// @summary Get DEX pool snapshots (experimental)
//
// @description Reserves and spot price of one pool after each masterchain block that changed it, newest first.
// @description Prototype: reads the dex-tap ClickHouse tables and may change without notice.
//
// @id api_v3_get_experimental_dex_pool_snapshots
// @tags experimental
// @Accept json
// @Produce json
// @success 200 {object} DexPoolSnapshotsResponse
// @failure 400 {object} models.RequestError
// @param pool query string true "Pool address in any form."
// @param start_utime query int32 false "Snapshots at or after this UTC timestamp." minimum(0)
// @param end_utime query int32 false "Snapshots at or before this UTC timestamp." minimum(0)
// @param limit query int32 false "Maximum number of snapshots." minimum(1) maximum(1000) default(100)
// @router /api/v3/experimental/dex/pool_snapshots [get]
// @security		APIKeyHeader
// @security		APIKeyQuery
func GetDexPoolSnapshots(c *fiber.Ctx) error {
	pool, err := dexAsset(c.Query("pool"))
	if err != nil || pool == "" {
		return models.IndexError{Code: 422, Message: "invalid pool"}
	}
	start, err := queryUint(c, "start_utime", 0)
	if err != nil {
		return err
	}
	end, err := queryUint(c, "end_utime", 4294967295)
	if err != nil {
		return err
	}
	limit, err := queryUint(c, "limit", 100)
	if err != nil {
		return err
	}
	if limit < 1 || limit > 1000 || start > 4294967295 || end > 4294967295 {
		return models.IndexError{Code: 422, Message: "limit must be 1..1000 and times must fit uint32"}
	}
	snapshots, err := dexPrices.QueryPoolSnapshots(c.UserContext(), pool, uint32(start), uint32(end), int(limit))
	if err != nil {
		return err
	}
	return c.JSON(DexPoolSnapshotsResponse{Snapshots: snapshots})
}
