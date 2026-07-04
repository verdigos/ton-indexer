package engine

import (
	"fmt"

	"mchpoc/tree"
)

// Protocol opcodes, taken verbatim from the Python message classes.
const (
	opJettonTransfer         uint32 = 0x0f8a7ea5 // JettonTransfer
	opJettonInternalTransfer uint32 = 0x178d4519 // JettonInternalTransfer
	opJettonNotify           uint32 = 0x7362d09c // JettonNotify

	opDedustSwap             uint32 = 0xea06185d // DedustSwap (user -> vault, TON)
	opDedustSwapExternal     uint32 = 0x61ee542d // DedustSwapExternal (vault/pool)
	opDedustSwapPeer         uint32 = 0x72aca8aa // DedustSwapPeer (pool -> pool)
	opDedustSwapNotification uint32 = 0x9c610de3 // DedustSwapNotification (log)
	opDedustPayoutFromPool   uint32 = 0xad4eb6f5 // DedustPayoutFromPool
	opDedustPayout           uint32 = 0x474f86cf // DedustPayout
)

// Match records one fired matcher for the end-of-run report.
type Match struct {
	ProducedBtype string
	AnchorDesc    string   // opcode or btype of the anchor block
	AnchorTx      string   // tx hash of the anchor
	Consumed      []string // per-consumed-block description (btype / opcode)
}

// MatchLog accumulates fired matches across a run.
type MatchLog struct {
	Matches []Match
}

func describeBlock(b *tree.Block) string {
	if b.Btype == "call_contract" {
		return fmt.Sprintf("call_contract(%#08x)", b.Opcode)
	}
	return b.Btype
}

func (log *MatchLog) record(produced string, anchor *tree.Block, consumed []*tree.Block) {
	seen := map[*tree.Block]bool{}
	var desc []string
	for _, b := range consumed {
		if b == nil || seen[b] {
			continue
		}
		seen[b] = true
		desc = append(desc, describeBlock(b))
	}
	log.Matches = append(log.Matches, Match{
		ProducedBtype: produced,
		AnchorDesc:    describeBlock(anchor),
		AnchorTx:      anchor.AnchorTxHash(),
		Consumed:      desc,
	})
}

// stubBuild is the shared stub builder: it performs the structural merge that
// every real build_block ends with, but skips the interface-repository lookups
// and BOC parsing that fill the protocol-specific `data` dict. The produced
// block carries only its btype, which is all downstream matchers key on.
func stubBuild(produced string, log *MatchLog) BuildFunc {
	return func(anchor *tree.Block, others []*tree.Block) []*tree.Block {
		consumed := append([]*tree.Block{anchor}, others...)
		log.record(produced, anchor, consumed)
		nb := tree.NewBlock(produced)
		if !nb.MergeBlocks(consumed) {
			return nil // earliest-common-block not found -> reject, like Python
		}
		return []*tree.Block{nb}
	}
}

// StubBuild exposes the shared stub builder to the IR loader so IR-driven
// matchers record and merge byte-identically to the hardcoded graphs.
func StubBuild(produced string, log *MatchLog) BuildFunc { return stubBuild(produced, log) }

// JettonTransferMatcher hardcodes JettonTransferBlockMatcher's constructor graph:
//
//	op JettonTransfer @anchor
//	  -> op JettonInternalTransfer
//	       -> maybe op JettonNotify
//
// build is stubbed (no jetton-wallet lookup); it produces a `jetton_transfer`
// block and merges the consumed subtree.
func JettonTransferMatcher(log *MatchLog) Matcher {
	return Generic(
		func(b *tree.Block) bool {
			return b.Btype == "call_contract" && b.Opcode == opJettonTransfer
		},
		Opts{
			Child: Contract(opJettonInternalTransfer, Opts{
				Child: Contract(opJettonNotify, Opts{Optional: true}),
			}),
			Build: stubBuild("jetton_transfer", log),
		},
	)
}

// DedustSwapMatcher hardcodes DedustSwapBlockMatcher's constructor graph:
//
//	anchor:   op DedustSwapExternal
//	parent:   (btype jetton_transfer | op DedustSwap)
//	children: [ (payout-chain | recursive-peer-chain), maybe op DedustSwapNotification ]
//
// The recursive peer chain (RecursiveMatcher) walks pool->pool DedustSwapPeer /
// DedustSwapExternal hops until it reaches the terminal DedustPayoutFromPool.
// build is stubbed; it produces a `jetton_swap` block. Requires the jetton pass
// to have run first so the `jetton_transfer` parent/child arms resolve.
func DedustSwapMatcher(log *MatchLog) Matcher {
	swap := Or(
		Contract(opDedustSwapPeer, Opts{
			Children: []Matcher{Contract(opDedustSwapNotification, Opts{Optional: true})},
		}),
		Contract(opDedustSwapExternal, Opts{
			Children: []Matcher{Contract(opDedustSwapNotification, Opts{Optional: true})},
		}),
	)

	peerChain := Recursive(
		swap,
		Contract(opDedustPayoutFromPool, Opts{
			Child: Or(
				Contract(opDedustPayout, Opts{}),
				BlockType("jetton_transfer", Opts{}),
			),
		}),
		false,
	)

	payout := ChildSequence(
		Contract(opDedustPayoutFromPool, Opts{}),
		Or(
			BlockType("jetton_transfer", Opts{}),
			Contract(opDedustPayout, Opts{}),
		),
	)

	return Generic(
		func(b *tree.Block) bool {
			return b.Btype == "call_contract" && b.Opcode == opDedustSwapExternal
		},
		Opts{
			Parent: Or(
				BlockType("jetton_transfer", Opts{}),
				Contract(opDedustSwap, Opts{}),
			),
			Children: []Matcher{
				Or(payout, peerChain),
				Contract(opDedustSwapNotification, Opts{Optional: true}),
			},
			Build: stubBuild("jetton_swap", log),
		},
	)
}
