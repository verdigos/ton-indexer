// Command mchpoc is a minimal proof-of-concept of the TON trace matcher engine.
// It loads an .lz4 trace fixture, builds the block tree, runs a hardcoded
// jetton_transfer matcher pass followed by a dedust_swap matcher pass, and
// prints a report of what fired and the resulting top-level block tree.
package main

import (
	"flag"
	"fmt"
	"os"
	"sort"

	"mchpoc/engine"
	"mchpoc/ir"
	"mchpoc/trace"
	"mchpoc/tree"
)

func main() {
	tracePath := flag.String("trace", "", "path to an .lz4 trace fixture")
	irPath := flag.String("ir", "", "path to an mch-ir.json artifact; enables IR-driven mode")
	matchersCSV := flag.String("matchers", "", "comma list of matcher names to run in order (IR mode; default jetton_transfer,dedust_swap)")
	flag.Parse()
	if *tracePath == "" {
		fmt.Fprintln(os.Stderr, "usage: mchpoc -trace <path-to-fixture.lz4>")
		os.Exit(2)
	}

	tr, err := trace.Load(*tracePath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "load: %v\n", err)
		os.Exit(1)
	}

	// Prelude: EventNode tree -> Block tree, exactly like event_processing.
	node := tree.ToTree(tr.Transactions)
	root := tree.NewBlock("root")
	root.Connect(tree.InitBlock(node))

	fmt.Printf("Trace: %s\n", tr.TraceID)
	fmt.Printf("Transactions: %d\n", len(tr.Transactions))
	fmt.Printf("Initial block tree (%d blocks):\n", countBlocks(root))
	printOpcodeHistogram(root)

	log := &engine.MatchLog{}

	if *irPath != "" {
		// IR-driven mode: load matchers from the artifact, run one pass each in
		// the order given by -matchers (default jetton_transfer,dedust_swap).
		doc, err := ir.Load(*irPath)
		if err != nil {
			fmt.Fprintf(os.Stderr, "ir load: %v\n", err)
			os.Exit(1)
		}
		names := ir.ParseMatchers(*matchersCSV, []string{"jetton_transfer", "dedust_swap"})
		matchers, err := doc.Build(names, log, ir.DefaultPredicates())
		if err != nil {
			fmt.Fprintf(os.Stderr, "ir build: %v\n", err)
			os.Exit(1)
		}
		for _, m := range matchers {
			runPass(root, m)
		}
	} else {
		// Hardcoded mode (unchanged): jetton_transfer then dedust_swap.
		runPass(root, engine.JettonTransferMatcher(log))
		runPass(root, engine.DedustSwapMatcher(log))
	}

	fmt.Printf("\n=== Fired matchers (%d) ===\n", len(log.Matches))
	for i, m := range log.Matches {
		fmt.Printf("[%d] produced %-15s anchor=%s tx=%s\n", i+1, m.ProducedBtype, m.AnchorDesc, short(m.AnchorTx))
		fmt.Printf("      consumed %d blocks: %v\n", len(m.Consumed), m.Consumed)
	}
	if len(log.Matches) == 0 {
		fmt.Println("(no matchers fired)")
	}

	fmt.Println("\n=== Resulting top-level block tree ===")
	printTree(root, 0)
}

// runPass mirrors event_processing.process_event_async's inner loop: walk the
// tree and try the matcher on every not-yet-absorbed block.
func runPass(root *tree.Block, m engine.Matcher) {
	root.BFS(func(b *tree.Block) {
		if b.Btype == "root" || b.Parent != nil {
			return
		}
		m.TryBuild(b)
	})
}

func countBlocks(root *tree.Block) int {
	n := 0
	root.BFS(func(b *tree.Block) {
		if b.Btype != "root" {
			n++
		}
	})
	return n
}

func printOpcodeHistogram(root *tree.Block) {
	counts := map[string]int{}
	root.BFS(func(b *tree.Block) {
		if b.Btype == "root" {
			return
		}
		if b.Btype == "call_contract" {
			counts[fmt.Sprintf("call_contract(%#08x)", b.Opcode)]++
		} else {
			counts[b.Btype]++
		}
	})
	keys := make([]string, 0, len(counts))
	for k := range counts {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		fmt.Printf("    %3d  %s\n", counts[k], k)
	}
}

// printTree walks only the live (parent==nil) top-level chain from root.
func printTree(root *tree.Block, depth int) {
	seen := map[*tree.Block]bool{}
	var walk func(b *tree.Block, depth int)
	walk = func(b *tree.Block, depth int) {
		if seen[b] {
			return
		}
		seen[b] = true
		if b.Btype != "root" {
			indent := ""
			for i := 0; i < depth; i++ {
				indent += "  "
			}
			desc := b.Btype
			if b.Btype == "call_contract" {
				desc = fmt.Sprintf("call_contract %#08x", b.Opcode)
			}
			fmt.Printf("%s- %s (nodes=%d, children=%d)\n", indent, desc, len(b.EventNodes), len(b.ChildrenBlocks))
		}
		next := append([]*tree.Block{}, b.NextBlocks...)
		sort.SliceStable(next, func(i, j int) bool { return next[i].MinLt < next[j].MinLt })
		for _, n := range next {
			if n.Parent == nil {
				childDepth := depth
				if b.Btype != "root" {
					childDepth = depth + 1
				}
				walk(n, childDepth)
			}
		}
	}
	walk(root, depth)
}

func short(h string) string {
	if len(h) > 12 {
		return h[:12] + "..."
	}
	return h
}
