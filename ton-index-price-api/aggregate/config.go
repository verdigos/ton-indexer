package aggregate

const (
	band                = 0.075  // a pool counts only within 7.5% of the reference price
	minPoolLiquidityTon = 100    // and with at least 100 TON on its smaller side
	maxPoolFee          = 0.01   // and with a swap fee of at most 1%
	anchorFloorTon      = 100000 // a TON/USDT pool counts for the anchor with at least 100,000 TON
	anchorTwap          = 5      // the anchor is the average of the last 5 minute medians
)
