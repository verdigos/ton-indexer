package parse

import (
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
)

var signedInt257Values = []string{
	"-1",
	"0",
	"18446744073709551616",
	"-115792089237316195423570985008687907853269984665640564039457584007913129639936",
	"115792089237316195423570985008687907853269984665640564039457584007913129639935",
}

func TestToncoDeployPoolTickSpacingStringPayload(t *testing.T) {
	for _, want := range []string{"-1", "0", "8388607"} {
		t.Run(want, func(t *testing.T) {
			payload, err := EncodeCompactForTest(map[string]interface{}{
				"tick_spacing": want,
			})
			if err != nil {
				t.Fatalf("EncodeCompactForTest: %v", err)
			}
			raw := &models.RawAction{Type: "tonco_deploy_pool"}
			if err := DecodeActionPayload(payload, raw); err != nil {
				t.Fatalf("DecodeActionPayload: %v", err)
			}
			if raw.ToncoDeployPoolTickSpacing == nil {
				t.Fatal("TickSpacing is nil")
			}
			if got := *raw.ToncoDeployPoolTickSpacing; got != want {
				t.Fatalf("TickSpacing = %q, want %q", got, want)
			}
		})
	}
}

func TestNFTIndexSignedFullRange(t *testing.T) {
	for _, want := range signedInt257Values {
		t.Run(want, func(t *testing.T) {
			payload, err := EncodeCompactForTest(map[string]interface{}{
				"nft_index": want,
			})
			if err != nil {
				t.Fatalf("EncodeCompactForTest: %v", err)
			}
			raw := &models.RawAction{Type: "nft_transfer"}
			if err := DecodeActionPayload(payload, raw); err != nil {
				t.Fatalf("DecodeActionPayload: %v", err)
			}
			if raw.NFTTransferNFTItemIndex == nil {
				t.Fatal("NFTItemIndex is nil")
			}
			if got := *raw.NFTTransferNFTItemIndex; got != want {
				t.Fatalf("NFTItemIndex = %q, want %q", got, want)
			}
		})
	}
}

func TestNFTValuesSignedFullRange(t *testing.T) {
	for _, tc := range []struct {
		key        string
		actionType string
		field      func(*models.RawAction) *string
	}{
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
			for _, want := range signedInt257Values {
				t.Run(want, func(t *testing.T) {
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
						t.Fatalf("%s field is nil", tc.key)
					}
					if *got != want {
						t.Fatalf("%s = %q, want %q", tc.key, *got, want)
					}
				})
			}
		})
	}
}
