package parse

import (
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
)

// TestSignedValuesDecodeIntoTheirFields pins that every signed-value key routes its text
// into its own field: one row per key.
func TestSignedValuesDecodeIntoTheirFields(t *testing.T) {
	const want = "-1"
	for _, tc := range []struct {
		key        string
		actionType string
		field      func(*models.RawAction) *string
	}{
		{"tick_spacing", "tonco_deploy_pool", func(raw *models.RawAction) *string { return raw.ToncoDeployPoolTickSpacing }},
		{"nft_index", "nft_transfer", func(raw *models.RawAction) *string { return raw.NFTTransferNFTItemIndex }},
		{"full_price", "nft_put_on_sale", func(raw *models.RawAction) *string { return raw.NFTListingFullPrice }},
		{"marketplace_fee", "nft_put_on_sale", func(raw *models.RawAction) *string { return raw.NFTListingMarketplaceFee }},
		{"max_bid", "nft_put_on_auction", func(raw *models.RawAction) *string { return raw.NFTListingMaxBid }},
		{"min_bid", "nft_put_on_auction", func(raw *models.RawAction) *string { return raw.NFTListingMinBid }},
		{"mp_fee_base", "nft_put_on_auction", func(raw *models.RawAction) *string { return raw.NFTListingMarketplaceFeeBase }},
		{"mp_fee_factor", "nft_put_on_auction", func(raw *models.RawAction) *string { return raw.NFTListingMarketplaceFeeFactor }},
		{"price", "nft_transfer", func(raw *models.RawAction) *string { return raw.NFTTransferPrice }},
		{"royalty_amount", "nft_transfer", func(raw *models.RawAction) *string { return raw.NFTTransferRoyaltyAmount }},
		{"royalty_fee_base", "nft_put_on_auction", func(raw *models.RawAction) *string { return raw.NFTListingRoyaltyFeeBase }},
	} {
		t.Run(tc.key, func(t *testing.T) {
			payload, err := EncodeCompactForTest(map[string]interface{}{tc.key: want})
			if err != nil {
				t.Fatalf("EncodeCompactForTest: %v", err)
			}
			raw := &models.RawAction{Type: tc.actionType}
			if err := DecodeActionPayload(payload, raw); err != nil {
				t.Fatalf("DecodeActionPayload: %v", err)
			}
			got := tc.field(raw)
			if got == nil {
				t.Fatalf("%s is nil", tc.key)
			}
			if *got != want {
				t.Fatalf("%s = %q, want %q", tc.key, *got, want)
			}
		})
	}
}
