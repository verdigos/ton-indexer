package ir

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"mchpoc/engine"
	"mchpoc/tree"
)

// PredRegistry maps a predicate name to its host implementation. Unknown names
// resolve to an always-true stub (with one stderr warning), so an artifact whose
// predicates aren't ported stays runnable — per schema §Predicates.
type PredRegistry map[string]func(*tree.Block) bool

// DefaultPredicates is the POC registry. No predicates are ported yet, so it is
// empty and every pred/where name hits the always-true stub.
func DefaultPredicates() PredRegistry { return PredRegistry{} }

// Load parses an mch-ir.json artifact. It does not wire matchers — call Build.
func Load(path string) (*IR, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var doc IR
	if err := json.Unmarshal(data, &doc); err != nil {
		return nil, fmt.Errorf("parse %s: %w", path, err)
	}
	return &doc, nil
}

// nodeMatcher is the stable per-node shell that makes two-pass wiring possible:
// pass 1 allocates one per node (a fixed pointer that any edge — including a
// cyclic back-edge — can reference), pass 2 fills inner. It also composes the
// node's `where` predicate onto both TestSelf and TryBuild, which is where the
// schema's "where composed onto test_self" is realised uniformly across kinds
// (try_build re-checks test_self, so gating both is equivalent).
type nodeMatcher struct {
	inner engine.Matcher
	where func(*tree.Block) bool
}

func (n *nodeMatcher) TestSelf(b *tree.Block) bool {
	if n.where != nil && !n.where(b) {
		return false
	}
	return n.inner.TestSelf(b)
}

func (n *nodeMatcher) TryBuild(b *tree.Block) []*tree.Block {
	if n.where != nil && !n.where(b) {
		return nil
	}
	return n.inner.TryBuild(b)
}

func (n *nodeMatcher) IsOptional() bool { return n.inner.IsOptional() }

// Build wires the whole node pool once (two-pass) and returns the engine
// matchers for the named matchers, in the given order (= pass order). preds may
// be nil.
func (doc *IR) Build(names []string, log *engine.MatchLog, preds PredRegistry) ([]engine.Matcher, error) {
	if preds == nil {
		preds = PredRegistry{}
	}
	warned := map[string]bool{}
	lookupPred := func(name string) func(*tree.Block) bool {
		if f, ok := preds[name]; ok {
			return f
		}
		if !warned[name] {
			warned[name] = true
			fmt.Fprintf(os.Stderr, "ir: predicate %q not registered; using always-true stub\n", name)
		}
		return func(*tree.Block) bool { return true }
	}

	// Pass 1: allocate a shell per node.
	wrappers := make([]*nodeMatcher, len(doc.Nodes))
	for i := range wrappers {
		wrappers[i] = &nodeMatcher{}
	}

	// Root node id -> owning matcher record (anchor test + build live there).
	rootOf := map[int]*Matcher{}
	for i := range doc.Matchers {
		m := &doc.Matchers[i]
		if m.Root < 0 || m.Root >= len(doc.Nodes) {
			return nil, fmt.Errorf("matcher %q: root %d out of range", m.Name, m.Root)
		}
		rootOf[m.Root] = m
	}

	// Pass 2: build each node's inner matcher, referencing the shells.
	for i := range doc.Nodes {
		n := &doc.Nodes[i]
		if n.Where != "" {
			wrappers[i].where = lookupPred(n.Where)
		}
		inner, err := doc.buildInner(i, n, wrappers, rootOf[i], log, lookupPred)
		if err != nil {
			return nil, err
		}
		wrappers[i].inner = inner
	}

	// Duplicate matcher names are legal (a name can appear across spec files with
	// identical patterns — schema §Emitter-note 2). A selected name expands to
	// EVERY occurrence, in registration order; re-runs are idempotent because the
	// first pass's absorbed blocks don't re-match. nameToRoots preserves that
	// order.
	order := doc.RegistrationOrder
	if len(order) != len(doc.Matchers) {
		order = make([]int, len(doc.Matchers))
		for i := range order {
			order[i] = i
		}
	}
	nameToRoots := map[string][]int{}
	for _, mi := range order {
		if mi < 0 || mi >= len(doc.Matchers) {
			return nil, fmt.Errorf("registration_order entry %d out of range", mi)
		}
		m := &doc.Matchers[mi]
		nameToRoots[m.Name] = append(nameToRoots[m.Name], m.Root)
	}
	out := make([]engine.Matcher, 0, len(names))
	for _, name := range names {
		roots, ok := nameToRoots[name]
		if !ok {
			return nil, fmt.Errorf("matcher %q not found in artifact", name)
		}
		for _, r := range roots {
			out = append(out, wrappers[r])
		}
	}
	return out, nil
}

func (doc *IR) buildInner(
	i int, n *Node, w []*nodeMatcher, root *Matcher,
	log *engine.MatchLog, lookupPred func(string) func(*tree.Block) bool,
) (engine.Matcher, error) {
	if err := doc.checkEdges(i, n, w); err != nil {
		return nil, err
	}

	opts := engine.Opts{Optional: n.Optional}
	if n.Child != nil {
		opts.Child = w[*n.Child]
	}
	if n.Parent != nil {
		opts.Parent = w[*n.Parent]
	}
	for _, c := range n.Children {
		opts.Children = append(opts.Children, w[c])
	}

	// The anchor (root) node: test_self comes from the matcher's anchor record,
	// build/include_excess from the matcher record. Its own kind/opcode is
	// subsumed by the anchor test (so multi-value opcode_set groups work).
	if root != nil {
		test, err := anchorTest(root.Anchor)
		if err != nil {
			return nil, fmt.Errorf("matcher %q: %w", root.Name, err)
		}
		opts.Build = engine.StubBuild(root.primaryBtype(), log)
		opts.IncludeExcess = root.IncludeExcess // nil -> engine default true
		return engine.Generic(test, opts), nil
	}

	switch n.Kind {
	case "contract":
		if n.Opcode == nil {
			return nil, fmt.Errorf("node %d: contract without opcode", i)
		}
		op := normOpcode(*n.Opcode)
		return engine.Generic(func(b *tree.Block) bool {
			return b.Btype == "call_contract" && b.Opcode == op
		}, opts), nil

	case "block_type":
		bt := n.Btype
		return engine.Generic(func(b *tree.Block) bool { return b.Btype == bt }, opts), nil

	case "pred":
		return engine.Generic(lookupPred(n.Pred), opts), nil

	case "any":
		return engine.Generic(func(*tree.Block) bool { return true }, opts), nil

	case "or":
		branches := make([]engine.Matcher, 0, len(n.Branches))
		for _, bi := range n.Branches {
			branches = append(branches, w[bi])
		}
		if n.Exclusive {
			if n.Optional {
				fmt.Fprintf(os.Stderr, "ir: node %d: exclusive+optional or is not representable in the engine; treating as non-optional\n", i)
			}
			return engine.Xor(branches...), nil
		}
		if n.Optional {
			return engine.OrOptional(branches...), nil
		}
		return engine.Or(branches...), nil

	case "recursive":
		// step is required; exit is optional (schema omits absent keys, and the
		// engine's frontier walk guards `if exit != nil` — a step-only recursion
		// repeats until step stops matching / the frontier is exhausted).
		if n.Step == nil {
			return nil, fmt.Errorf("node %d: recursive requires step", i)
		}
		var exit engine.Matcher
		if n.Exit != nil {
			exit = w[*n.Exit]
		}
		return engine.Recursive(w[*n.Step], exit, n.Optional), nil

	default:
		return nil, fmt.Errorf("node %d: unknown kind %q", i, n.Kind)
	}
}

// checkEdges validates every referenced node id is in range before we index the
// shell slice with it.
func (doc *IR) checkEdges(i int, n *Node, w []*nodeMatcher) error {
	check := func(id int, what string) error {
		if id < 0 || id >= len(w) {
			return fmt.Errorf("node %d: %s id %d out of range", i, what, id)
		}
		return nil
	}
	if n.Child != nil {
		if err := check(*n.Child, "child"); err != nil {
			return err
		}
	}
	if n.Parent != nil {
		if err := check(*n.Parent, "parent"); err != nil {
			return err
		}
	}
	for _, c := range n.Children {
		if err := check(c, "children"); err != nil {
			return err
		}
	}
	for _, b := range n.Branches {
		if err := check(b, "branch"); err != nil {
			return err
		}
	}
	if n.Step != nil {
		if err := check(*n.Step, "step"); err != nil {
			return err
		}
	}
	if n.Exit != nil {
		if err := check(*n.Exit, "exit"); err != nil {
			return err
		}
	}
	return nil
}

// anchorTest builds the root node's test_self from the anchor record.
func anchorTest(a Anchor) (func(*tree.Block) bool, error) {
	switch a.Kind {
	case "opcode_set":
		set := map[uint32]bool{}
		for _, raw := range a.Values {
			var v int64
			if err := json.Unmarshal(raw, &v); err != nil {
				return nil, fmt.Errorf("anchor opcode_set value %s: %w", raw, err)
			}
			set[normOpcode(v)] = true
		}
		return func(b *tree.Block) bool {
			return b.Btype == "call_contract" && set[b.Opcode]
		}, nil
	case "btype":
		set := map[string]bool{}
		for _, raw := range a.Values {
			var s string
			if err := json.Unmarshal(raw, &s); err != nil {
				return nil, fmt.Errorf("anchor btype value %s: %w", raw, err)
			}
			set[s] = true
		}
		return func(b *tree.Block) bool { return set[b.Btype] }, nil
	default:
		return nil, fmt.Errorf("unknown anchor kind %q", a.Kind)
	}
}

// normOpcode reduces a signed-or-unsigned opcode to its unsigned 32-bit value,
// matching how trace/tree store message opcodes (Message.Opcode32).
func normOpcode(v int64) uint32 { return uint32(uint64(v) & 0xFFFFFFFF) }

// ParseMatchers splits a comma list into trimmed non-empty names. Empty input
// yields the given default.
func ParseMatchers(csv string, def []string) []string {
	if strings.TrimSpace(csv) == "" {
		return def
	}
	var out []string
	for _, p := range strings.Split(csv, ",") {
		if p = strings.TrimSpace(p); p != "" {
			out = append(out, p)
		}
	}
	if len(out) == 0 {
		return def
	}
	return out
}
