package parse_test

import (
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"
)

const (
	rowSourceSecondary = models.AccountAddress("0:1111111111111111111111111111111111111111111111111111111111111111")
	rowDestSecondary   = models.AccountAddress("0:2222222222222222222222222222222222222222222222222222222222222222")
	rowAsset           = models.AccountAddress("0:3333333333333333333333333333333333333333333333333333333333333333")
	payloadMarketplace = models.AccountAddress("0:4444444444444444444444444444444444444444444444444444444444444444")
)

func addrPtr(a models.AccountAddress) *models.AccountAddress { return &a }

// TestApiRepairReadsRowColumn pins the five repaired details fields: the payload has
// no key for them, so each reads the envelope column the engine wrote on the same row.
// Every case also fills the column that must not be read, with a different address.
func TestApiRepairReadsRowColumn(t *testing.T) {
	for _, tc := range []struct {
		name string
		raw  models.RawAction
		pick func(*models.Action) (*models.AccountAddress, bool)
		want *models.AccountAddress
	}{
		{
			name: "evaa_supply.sender_jetton_wallet from source_secondary",
			raw: models.RawAction{
				Type:                 "evaa_supply",
				SourceSecondary:      addrPtr(rowSourceSecondary),
				DestinationSecondary: addrPtr(rowDestSecondary),
			},
			pick: func(action *models.Action) (*models.AccountAddress, bool) {
				details, ok := action.Details.(models.ActionDetailsEvaaSupply)
				return details.SenderJettonWallet, ok
			},
			want: addrPtr(rowSourceSecondary),
		},
		{
			name: "evaa_supply.sender_jetton_wallet stays null without source_secondary",
			raw: models.RawAction{
				Type:                 "evaa_supply",
				DestinationSecondary: addrPtr(rowDestSecondary),
			},
			pick: func(action *models.Action) (*models.AccountAddress, bool) {
				details, ok := action.Details.(models.ActionDetailsEvaaSupply)
				return details.SenderJettonWallet, ok
			},
			want: nil,
		},
		{
			name: "coffee_create_pool.pool_creator_contract from destination_secondary",
			raw: models.RawAction{
				Type:                 "coffee_create_pool",
				SourceSecondary:      addrPtr(rowSourceSecondary),
				DestinationSecondary: addrPtr(rowDestSecondary),
			},
			pick: func(action *models.Action) (*models.AccountAddress, bool) {
				details, ok := action.Details.(models.ActionDetailsCoffeeCreatePool)
				return details.PoolCreatorContract, ok
			},
			want: addrPtr(rowDestSecondary),
		},
		{
			name: "coffee_create_pool.pool_creator_contract stays null without destination_secondary",
			raw: models.RawAction{
				Type:            "coffee_create_pool",
				SourceSecondary: addrPtr(rowSourceSecondary),
			},
			pick: func(action *models.Action) (*models.AccountAddress, bool) {
				details, ok := action.Details.(models.ActionDetailsCoffeeCreatePool)
				return details.PoolCreatorContract, ok
			},
			want: nil,
		},
		{
			name: "coffee_create_pool_creator.provided_asset from the row asset",
			raw: models.RawAction{
				Type:                 "coffee_create_pool_creator",
				DestinationSecondary: addrPtr(rowDestSecondary),
				Asset:                addrPtr(rowAsset),
			},
			pick: func(action *models.Action) (*models.AccountAddress, bool) {
				details, ok := action.Details.(models.ActionDetailsCoffeeCreatePoolCreator)
				return details.ProvidedAsset, ok
			},
			want: addrPtr(rowAsset),
		},
		{
			name: "coffee_create_pool_creator.provided_asset stays null without a row asset",
			raw: models.RawAction{
				Type:                 "coffee_create_pool_creator",
				DestinationSecondary: addrPtr(rowDestSecondary),
			},
			pick: func(action *models.Action) (*models.AccountAddress, bool) {
				details, ok := action.Details.(models.ActionDetailsCoffeeCreatePoolCreator)
				return details.ProvidedAsset, ok
			},
			want: nil,
		},
		{
			name: "nft_put_on_sale.marketplace_address from the payload address",
			raw: models.RawAction{
				Type:                          "nft_put_on_sale",
				DestinationSecondary:          addrPtr(rowDestSecondary),
				NFTTransferMarketplaceAddress: addrPtr(payloadMarketplace),
			},
			pick: func(action *models.Action) (*models.AccountAddress, bool) {
				details, ok := action.Details.(*models.ActionDetailsNftPutOnSale)
				return details.MarketplaceAddress, ok
			},
			want: addrPtr(payloadMarketplace),
		},
		{
			name: "nft_put_on_sale.marketplace_address stays null without the payload key",
			raw: models.RawAction{
				Type:                 "nft_put_on_sale",
				DestinationSecondary: addrPtr(rowDestSecondary),
			},
			pick: func(action *models.Action) (*models.AccountAddress, bool) {
				details, ok := action.Details.(*models.ActionDetailsNftPutOnSale)
				return details.MarketplaceAddress, ok
			},
			want: nil,
		},
		{
			name: "nft_put_on_auction.marketplace_address from the payload address",
			raw: models.RawAction{
				Type:                          "nft_put_on_auction",
				DestinationSecondary:          addrPtr(rowDestSecondary),
				NFTTransferMarketplaceAddress: addrPtr(payloadMarketplace),
			},
			pick: func(action *models.Action) (*models.AccountAddress, bool) {
				details, ok := action.Details.(*models.ActionDetailsNftPutOnAuction)
				return details.MarketplaceAddress, ok
			},
			want: addrPtr(payloadMarketplace),
		},
		{
			name: "nft_put_on_auction.marketplace_address stays null without the payload key",
			raw: models.RawAction{
				Type:                 "nft_put_on_auction",
				DestinationSecondary: addrPtr(rowDestSecondary),
			},
			pick: func(action *models.Action) (*models.AccountAddress, bool) {
				details, ok := action.Details.(*models.ActionDetailsNftPutOnAuction)
				return details.MarketplaceAddress, ok
			},
			want: nil,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			action, err := parse.ParseRawAction(&tc.raw)
			if err != nil {
				t.Fatalf("ParseRawAction: %v", err)
			}
			got, ok := tc.pick(action)
			if !ok {
				t.Fatalf("details = %T", action.Details)
			}
			if !addrEqual(got, tc.want) {
				t.Errorf("got %s, want %s", addrString(got), addrString(tc.want))
			}
		})
	}
}

// TestNftAuctionMarketplaceNameFromPayloadAddress pins the auction name lookup to the
// payload address the API reports, rather than the row column the engine leaves unset.
func TestNftAuctionMarketplaceNameFromPayloadAddress(t *testing.T) {
	saved := parse.MarketplaceCache
	parse.MarketplaceCache = map[string]string{string(payloadMarketplace): "getgems"}
	t.Cleanup(func() { parse.MarketplaceCache = saved })

	raw := &models.RawAction{
		Type:                          "nft_put_on_auction",
		DestinationSecondary:          addrPtr(rowDestSecondary),
		NFTTransferMarketplaceAddress: addrPtr(payloadMarketplace),
	}
	action, err := parse.ParseRawAction(raw)
	if err != nil {
		t.Fatalf("ParseRawAction: %v", err)
	}
	details, ok := action.Details.(*models.ActionDetailsNftPutOnAuction)
	if !ok {
		t.Fatalf("details = %T", action.Details)
	}
	if details.Marketplace == nil || *details.Marketplace != "getgems" {
		t.Errorf("marketplace = %s, want getgems from the payload address", addrString(details.MarketplaceAddress))
	}
	if !addrEqual(details.MarketplaceAddress, addrPtr(payloadMarketplace)) {
		t.Errorf("marketplace_address = %s, want %s", addrString(details.MarketplaceAddress), payloadMarketplace)
	}
}

func addrEqual(got, want *models.AccountAddress) bool {
	if got == nil || want == nil {
		return got == want
	}
	return *got == *want
}

func addrString(a *models.AccountAddress) string {
	if a == nil {
		return "null"
	}
	return string(*a)
}
