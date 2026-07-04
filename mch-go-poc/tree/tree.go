// Package tree ports the block-tree construction and merge machinery from the
// Python indexer (blocks/utils/tree_utils.py, blocks/core.py, blocks/basic_blocks.py,
// event_processing.init_block). It is the structural substrate the matcher engine
// operates on.
package tree

import (
	"math"
	"sort"

	"mchpoc/trace"
)

const (
	// TonTransferMessage.encrypted_opcode — an encrypted-comment plain transfer.
	encryptedTonOpcode uint32 = 0x2167da4b
)

// EventNode mirrors tree_utils.EventNode: a node in the message tree. For a
// normal tx, Msg is the incoming message; for a notification/log leaf, Msg is
// the destination-less outgoing message.
type EventNode struct {
	Msg        *trace.Message
	Tx         *trace.Transaction
	IsTickTock bool
	Parent     *EventNode
	Children   []*EventNode
}

func (n *EventNode) addChild(c *EventNode) {
	n.Children = append(n.Children, c)
	c.Parent = n
}

// Opcode mirrors EventNode.get_opcode.
func (n *EventNode) Opcode() (uint32, bool) {
	if n.Msg == nil {
		return 0, false
	}
	return n.Msg.Opcode32()
}

// Lt mirrors EventNode.get_lt.
func (n *EventNode) Lt() int64 {
	if n.IsTickTock && n.Tx != nil {
		return n.Tx.Lt
	}
	if n.Msg != nil && n.Msg.CreatedLt != nil {
		return *n.Msg.CreatedLt
	}
	if n.Msg != nil {
		return n.Msg.TxLt
	}
	return 0
}

// TxHash returns the owning transaction hash for reporting.
func (n *EventNode) TxHash() string {
	if n.Tx != nil {
		return n.Tx.Hash
	}
	if n.Msg != nil {
		return n.Msg.TxHash
	}
	return ""
}

// ToTree ports tree_utils.to_tree: builds the EventNode tree from the flat
// transaction list using the msg_hash linkage.
func ToTree(txs []*trace.Transaction) *EventNode {
	sorted := append([]*trace.Transaction{}, txs...)
	sort.SliceStable(sorted, func(i, j int) bool { return sorted[i].Lt > sorted[j].Lt }) // desc

	msgTx := map[string]string{}       // in-message hash -> owning tx hash
	nodes := map[string]*EventNode{}   // tx hash -> node

	createNode := func(tx *trace.Transaction) *EventNode {
		var inMsg *trace.Message
		for _, m := range tx.Messages {
			if m.Direction == "in" {
				inMsg = m
				break
			}
		}
		if inMsg != nil {
			msgTx[inMsg.MsgHash] = tx.Hash
		}
		return &EventNode{Msg: inMsg, Tx: tx, IsTickTock: tx.Descr == "tick_tock"}
	}

	for _, tx := range sorted {
		if _, ok := nodes[tx.Hash]; ok {
			continue
		}
		n := createNode(tx)
		nodes[tx.Hash] = n
		for _, m := range tx.Messages {
			if m.Direction != "out" {
				continue
			}
			if m.Destination == nil {
				n.addChild(&EventNode{Msg: m, Tx: tx}) // notification / log leaf
			} else if childHash, ok := msgTx[m.MsgHash]; ok {
				n.addChild(nodes[childHash])
			}
			// If the out-message has a destination but no matching child tx was
			// seen, the Python code asserts; we skip for POC robustness.
		}
	}

	root := nodes[sorted[len(sorted)-1].Hash]
	for root.Parent != nil {
		root = root.Parent
	}
	return root
}

// Block mirrors blocks/core.py Block plus the CallContractBlock opcode field.
type Block struct {
	Btype     string
	Opcode    uint32
	HasOpcode bool

	EventNodes     []*EventNode
	NextBlocks     []*Block
	ChildrenBlocks []*Block
	PreviousBlock  *Block
	Parent         *Block

	Failed  bool
	Broken  bool
	Transient bool

	MinLt                    int64
	MinLtWithoutInitiatingTx int64
}

func newLeafBlock(btype string, node *EventNode) *Block {
	b := &Block{Btype: btype, EventNodes: []*EventNode{node}}
	b.MinLt = node.Lt()
	if node.Tx != nil {
		b.MinLtWithoutInitiatingTx = node.Tx.Lt
	} else {
		b.MinLtWithoutInitiatingTx = node.Lt()
	}
	return b
}

// NewBlock creates an empty synthesized block (no nodes yet) that a builder
// fills via MergeBlocks — matching the Python `SomeBlock('btype', [], data)`
// pattern followed by merge_blocks.
func NewBlock(btype string) *Block {
	return &Block{Btype: btype}
}

// InitBlock ports event_processing.init_block: classify each node into a leaf
// block and connect the subtree.
func InitBlock(node *EventNode) *Block {
	op, hasOp := node.Opcode()
	isTon := !hasOp || op == 0 || op == encryptedTonOpcode

	var b *Block
	switch {
	case node.IsTickTock:
		b = newLeafBlock("tick_tock", node)
	case isTon && node.Msg != nil && node.Msg.Destination != nil && node.Msg.Source != nil:
		b = newLeafBlock("ton_transfer", node)
	default:
		b = newLeafBlock("call_contract", node)
		b.Opcode = op
		b.HasOpcode = hasOp
		if node.Tx != nil {
			b.Failed = node.Tx.Aborted
		}
	}
	for _, c := range node.Children {
		b.Connect(InitBlock(c))
	}
	return b
}

// Connect ports Block.connect.
func (b *Block) Connect(o *Block) {
	b.NextBlocks = append(b.NextBlocks, o)
	o.PreviousBlock = b
}

// direction ports core._get_direction_for_block.
func (b *Block) direction() int {
	if len(b.EventNodes) == 1 {
		if b.EventNodes[0].Msg != nil && b.EventNodes[0].Msg.Direction == "out" {
			return 1
		}
		return 0
	}
	return 2
}

func (b *Block) topmostParent() *Block {
	if b.Parent == nil {
		return b
	}
	return b.Parent.topmostParent()
}

// compactConnections ports Block.compact_connections: rewire next_blocks to
// their topmost parent, dropping own children and self.
func (b *Block) compactConnections() {
	childSet := map[*Block]bool{}
	for _, c := range b.ChildrenBlocks {
		childSet[c] = true
	}
	seen := map[*Block]bool{}
	var out []*Block
	for _, n := range b.NextBlocks {
		if childSet[n] || n == b {
			continue
		}
		tp := n.topmostParent()
		if !seen[tp] {
			seen[tp] = true
			out = append(out, tp)
		}
	}
	b.NextBlocks = out
}

// findFrontier ports the specific find_next call used inside merge_blocks:
// walk next_blocks, descending through blocks IN the merge set, yielding the
// first block OFF the set on each path (yield_on_unmatch + stop_on_filter_unmatch).
func (b *Block) findFrontier(inSet map[*Block]bool) []*Block {
	type qi struct{ node *Block }
	var queue []qi
	for _, c := range b.NextBlocks {
		queue = append(queue, qi{c})
	}
	seen := map[*Block]bool{}
	var out []*Block
	for len(queue) > 0 {
		it := queue[0]
		queue = queue[1:]
		if inSet[it.node] {
			for _, c := range it.node.NextBlocks {
				queue = append(queue, qi{c})
			}
		} else if !seen[it.node] {
			seen[it.node] = true
			out = append(out, it.node)
		}
	}
	return out
}

func (b *Block) calculateMinMaxLt() {
	minLt := int64(math.MaxInt64)
	for _, n := range b.EventNodes {
		if lt := n.Lt(); lt < minLt {
			minLt = lt
		}
	}
	if len(b.EventNodes) == 0 {
		minLt = 0
	}
	b.MinLt = minLt
	b.MinLtWithoutInitiatingTx = minLt
}

// ensureEarliestCommonBlock ports core._ensure_earliest_common_block.
func ensureEarliestCommonBlock(blocks []*Block) *Block {
	sorted := append([]*Block{}, blocks...)
	sort.SliceStable(sorted, func(i, j int) bool {
		a, c := sorted[i], sorted[j]
		if a.MinLtWithoutInitiatingTx != c.MinLtWithoutInitiatingTx {
			return a.MinLtWithoutInitiatingTx < c.MinLtWithoutInitiatingTx
		}
		return a.direction() < c.direction()
	})
	if len(sorted) == 0 {
		return nil
	}
	earliest := sorted[0]
	connected := map[*Block]bool{earliest: true}
	for _, block := range sorted {
		if connected[block] {
			continue
		}
		if block.PreviousBlock != nil {
			if connected[block.PreviousBlock] {
				connected[block] = true
				continue
			}
			return nil
		}
	}
	return earliest
}

// MergeBlocks ports Block.merge_blocks: absorb `blocks` as children of this
// block, splice this block into the tree where the earliest merged block sat,
// and set next_blocks to the frontier just outside the merged region.
func (nb *Block) MergeBlocks(blocks []*Block) bool {
	set := map[*Block]bool{}
	var toMerge []*Block
	for _, b := range blocks {
		if b == nil || b.Transient || set[b] {
			continue
		}
		set[b] = true
		toMerge = append(toMerge, b)
	}
	earliest := ensureEarliestCommonBlock(toMerge)
	if earliest == nil {
		return false
	}
	for _, b := range toMerge {
		b.Parent = nb
		nb.EventNodes = append(nb.EventNodes, b.EventNodes...)
		nb.ChildrenBlocks = append(nb.ChildrenBlocks, b)
	}
	for _, fb := range earliest.findFrontier(set) {
		nb.NextBlocks = append(nb.NextBlocks, fb)
		fb.PreviousBlock = nb
	}
	nb.PreviousBlock = earliest.PreviousBlock
	if earliest.PreviousBlock != nil {
		earliest.PreviousBlock.compactConnections()
	}
	nb.calculateMinMaxLt()
	return true
}

// BFS ports Block.bfs_iter (include_self=true) as an eager pre-order walk over
// next_blocks. The caller mutates the tree during iteration (merges), exactly
// like the Python generator: each node's next_blocks are read *after* the
// visitor runs on it.
func (b *Block) BFS(visit func(*Block)) {
	queue := []*Block{b}
	for len(queue) > 0 {
		cur := queue[0]
		queue = queue[1:]
		visit(cur)
		queue = append(queue, cur.NextBlocks...)
	}
}

// AnchorTxHash returns the tx hash of this block's first event node (reporting).
func (b *Block) AnchorTxHash() string {
	if len(b.EventNodes) > 0 {
		return b.EventNodes[0].TxHash()
	}
	return ""
}
