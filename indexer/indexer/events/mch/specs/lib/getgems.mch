# Unknown code hashes are not GetGems contracts.
export fn getgems_sale_version(h) = (
    1 if h == "2pufziLofEllctIDZSWVebzO+RpyA1fMvowFLvyb4I8=" else
    2 if h in [
        "gnj0xSM95vvtyWmvUZNEp6m//FRIVtuphqlcC8+Fcck=",
        "MgUN+sRPZIZrzIbyzZ4TBf6dyts5WcACI3z7CQLUQyM=",
        "3rU7bFdlwebNI4v0e8XoO6WWvcwEsLhM1Qqx5HSgjzE=",
        "JCIfpXHlQuBVx3vt/b9SfHr0YM/cfzRMRQeHtM+h600=",
    ] else
    4 if h == "a5WmQYucnSNZBF0edVm41UmuDlBvJMqrWPowyPsf64Y=" else
    null
)

export fn getgems_auction_version(h) = (
    1 if h == "/ACindAgW83MDT/7nKOMw8jBWexg2KpUMkCpLxBZLUA=" else
    2 if h in [
        "ZmiHL6eXBUQ//UdSPo6eqfdquZ+aC1nSfej4GhwnudQ=",
        "G9nFo5v/t6DzQViLXdkrgTqEK/Ze8UEJOCIAzq+Pct8=",
    ] else
    3 if h == "u29ireD+stefqzuK6/CTCvmFU99gCTsgJ/Covxab/Ow=" else
    4 if h == "zlp4U06qps7tja/UhtB262CpsNbb+1Nnb2YmScBomVY=" else
    null
)

fn known_getgems_sale(sale) = sale if getgems_sale_version(sale.code_hash) != null else null
fn known_getgems_auction(auction) = auction if getgems_auction_version(auction.code_hash) != null else null

export fn getgems_sale(addr) = known_getgems_sale(lookup nft_sale(addr))
export fn getgems_auction(addr) = known_getgems_auction(lookup nft_auction(addr))
