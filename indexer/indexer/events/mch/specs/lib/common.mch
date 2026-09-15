# Whether the NFT's content URI contains the Fragment NFT host.
# Missing content or URI answers false.
export fn is_fragment_item(nft) = contains(get(get(nft, "content"), "uri") ?? "", "https://nft.fragment.com")

# The smaller of two values.
export fn min(a, b) = a if a < b else b

# Whether the transaction aborted, defaulting to false when missing.
export fn tx_failed(x) = x.msg.transaction.aborted ?? false
