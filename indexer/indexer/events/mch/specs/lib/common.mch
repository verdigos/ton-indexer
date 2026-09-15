# Whether the NFT's content URI contains the Fragment NFT host.
# Missing content or URI answers false.
export fn is_fragment_item(nft) = contains(get(get(nft, "content"), "uri") ?? "", "https://nft.fragment.com")
