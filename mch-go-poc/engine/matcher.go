// Package engine ports the matcher primitives from blocks/basic_matchers.py:
// BlockMatcher.try_build and its child/parent/children/auto-append machinery,
// plus ContractMatcher, BlockTypeMatcher, OrMatcher, ExclusiveOrMatcher,
// RecursiveMatcher and child_sequence_matcher.
package engine

import "mchpoc/tree"

const (
	excessOpcode uint32 = 0xd53276db // ExcessMessage.opcode
	bounceOpcode uint32 = 0xffffffff
)

// Matcher is the common interface. TryBuild returns the list of blocks the match
// produced (the newly built block plus any auto-appended excess/bounce), or nil
// to reject. This mirrors Python's `try_build -> list | None`.
type Matcher interface {
	TestSelf(b *tree.Block) bool
	TryBuild(b *tree.Block) []*tree.Block
	IsOptional() bool
}

// BuildFunc constructs the result blocks after a successful structural match.
// anchor is the block that passed test_self; others is everything the child /
// parent / children matchers consumed. Returning nil rejects the match (as when
// a Python build_block raises or returns []).
type BuildFunc func(anchor *tree.Block, others []*tree.Block) []*tree.Block

// BaseMatcher ports blocks/basic_matchers.py BlockMatcher. ContractMatcher and
// BlockTypeMatcher are just BaseMatchers with a specific testSelf.
type BaseMatcher struct {
	testSelf         func(*tree.Block) bool
	ChildMatcher     Matcher
	ParentMatcher    Matcher
	ChildrenMatchers []Matcher
	optional         bool
	includeExcess    bool
	includeBounces   bool
	preBuildAppend   bool
	build            BuildFunc

	// childProcessor overrides process_child_matcher (used by RecursiveMatcher).
	childProcessor func(bm *BaseMatcher, block *tree.Block, blocks *[]*tree.Block) bool
}

func (bm *BaseMatcher) TestSelf(b *tree.Block) bool {
	if bm.testSelf != nil {
		return bm.testSelf(b)
	}
	return true
}

func (bm *BaseMatcher) IsOptional() bool { return bm.optional }

// TryBuild ports BlockMatcher.try_build.
func (bm *BaseMatcher) TryBuild(block *tree.Block) []*tree.Block {
	if !bm.TestSelf(block) {
		return nil
	}
	var blocks []*tree.Block
	childMatched := bm.processChild(block, &blocks)
	parentMatched := bm.processParent(block, &blocks)
	if !childMatched || !parentMatched {
		return nil
	}

	var autoOps []uint32
	if bm.includeExcess {
		autoOps = append(autoOps, excessOpcode)
	}
	if bm.includeBounces {
		autoOps = append(autoOps, bounceOpcode)
	}

	if len(autoOps) > 0 && bm.preBuildAppend {
		blocks = appendAutoOps(block, blocks, autoOps)
	}
	r := bm.doBuild(block, blocks)
	if r == nil {
		return nil
	}
	if len(autoOps) > 0 && !bm.preBuildAppend {
		r = appendAutoOps(block, r, autoOps)
	}
	return r
}

func appendAutoOps(block *tree.Block, dst []*tree.Block, autoOps []uint32) []*tree.Block {
	for _, nb := range block.NextBlocks {
		if nb.Btype == "call_contract" {
			for _, op := range autoOps {
				if nb.Opcode == op {
					dst = append(dst, nb)
					break
				}
			}
		}
	}
	return dst
}

func (bm *BaseMatcher) doBuild(block *tree.Block, others []*tree.Block) []*tree.Block {
	if bm.build != nil {
		return bm.build(block, others)
	}
	return append([]*tree.Block{block}, others...)
}

// processChild ports BlockMatcher.process_child_matcher.
func (bm *BaseMatcher) processChild(block *tree.Block, blocks *[]*tree.Block) bool {
	if bm.childProcessor != nil {
		return bm.childProcessor(bm, block, blocks)
	}
	childMatched := true
	if bm.ChildMatcher != nil {
		for _, child := range block.NextBlocks {
			if r := bm.ChildMatcher.TryBuild(child); r != nil {
				*blocks = append(*blocks, r...)
			}
		}
		childMatched = bm.ChildMatcher.IsOptional() || len(*blocks) > 0
	}
	if bm.ChildrenMatchers != nil {
		if r := bm.processChildren(block); r != nil {
			*blocks = append(*blocks, r...)
		} else {
			childMatched = false
		}
	}
	return childMatched
}

// processChildren ports BlockMatcher.process_children_matchers: ordered,
// first-match-wins consumption of next_blocks, skipping optional matchers that
// find no block. Returns nil only when a required matcher goes unmatched.
func (bm *BaseMatcher) processChildren(block *tree.Block) []*tree.Block {
	nextBlocks := append([]*tree.Block{}, block.NextBlocks...)
	remaining := append([]Matcher{}, bm.ChildrenMatchers...)
	var result []*tree.Block
	for len(remaining) > 0 {
		matcher := remaining[0]
		matched := false
		for i, nb := range nextBlocks {
			if res := matcher.TryBuild(nb); res != nil {
				result = append(result, res...)
				remaining = remaining[1:]
				nextBlocks = append(nextBlocks[:i:i], nextBlocks[i+1:]...)
				matched = true
				break
			}
		}
		if !matched {
			if matcher.IsOptional() {
				remaining = remaining[1:]
			} else {
				return nil
			}
		}
	}
	return result
}

// processParent ports BlockMatcher.process_parent_matcher.
func (bm *BaseMatcher) processParent(block *tree.Block, blocks *[]*tree.Block) bool {
	if bm.ParentMatcher == nil {
		return true
	}
	if block.PreviousBlock == nil {
		return bm.ParentMatcher.IsOptional()
	}
	if r := bm.ParentMatcher.TryBuild(block.PreviousBlock); r != nil {
		*blocks = append(*blocks, r...)
		return true
	}
	return bm.ParentMatcher.IsOptional()
}

// Opts collects the optional constructor parameters shared by the matcher
// factories, keeping call sites readable at the graph definition points.
type Opts struct {
	Child          Matcher
	Parent         Matcher
	Optional       bool
	Children       []Matcher
	IncludeExcess  *bool // defaults to true (matches Python)
	Build          BuildFunc
}

func excessDefault(o Opts) bool {
	if o.IncludeExcess == nil {
		return true
	}
	return *o.IncludeExcess
}

// Contract ports ContractMatcher.
func Contract(opcode uint32, o Opts) *BaseMatcher {
	return &BaseMatcher{
		testSelf: func(b *tree.Block) bool {
			return b.Btype == "call_contract" && b.Opcode == opcode
		},
		ChildMatcher:     o.Child,
		ParentMatcher:    o.Parent,
		ChildrenMatchers: o.Children,
		optional:         o.Optional,
		includeExcess:    excessDefault(o),
		includeBounces:   true,
		build:            o.Build,
	}
}

// BlockType ports BlockTypeMatcher.
func BlockType(btype string, o Opts) *BaseMatcher {
	return &BaseMatcher{
		testSelf: func(b *tree.Block) bool {
			return b.Btype == btype
		},
		ChildMatcher:   o.Child,
		ParentMatcher:  o.Parent,
		ChildrenMatchers: o.Children,
		optional:       o.Optional,
		includeExcess:  excessDefault(o),
		includeBounces: true,
		build:          o.Build,
	}
}

// Generic ports GenericMatcher / a BlockMatcher with a bespoke test_self and
// build (used for the two anchor matchers that are not ContractMatchers).
func Generic(testSelf func(*tree.Block) bool, o Opts) *BaseMatcher {
	return &BaseMatcher{
		testSelf:         testSelf,
		ChildMatcher:     o.Child,
		ParentMatcher:    o.Parent,
		ChildrenMatchers: o.Children,
		optional:         o.Optional,
		includeExcess:    excessDefault(o),
		includeBounces:   true,
		build:            o.Build,
	}
}

// OrMatcher ports OrMatcher: first sub-matcher whose try_build succeeds wins.
type OrMatcher struct {
	Matchers []Matcher
	optional bool
}

func Or(matchers ...Matcher) *OrMatcher { return &OrMatcher{Matchers: matchers} }

func OrOptional(matchers ...Matcher) *OrMatcher {
	return &OrMatcher{Matchers: matchers, optional: true}
}

func (o *OrMatcher) TestSelf(b *tree.Block) bool {
	for _, m := range o.Matchers {
		if m.TestSelf(b) {
			return true
		}
	}
	return false
}

func (o *OrMatcher) TryBuild(b *tree.Block) []*tree.Block {
	for _, m := range o.Matchers {
		if r := m.TryBuild(b); r != nil {
			return r
		}
	}
	return nil
}

func (o *OrMatcher) IsOptional() bool { return o.optional }

// ExclusiveOrMatcher ports ExclusiveOrMatcher: overrides test_self ONLY (exactly
// one sub-matcher may match); try_build is inherited from OrMatcher.
type ExclusiveOrMatcher struct{ OrMatcher }

func Xor(matchers ...Matcher) *ExclusiveOrMatcher {
	return &ExclusiveOrMatcher{OrMatcher{Matchers: matchers}}
}

func (x *ExclusiveOrMatcher) TestSelf(b *tree.Block) bool {
	count := 0
	for _, m := range x.Matchers {
		if m.TestSelf(b) {
			count++
		}
	}
	return count == 1
}

// Recursive ports RecursiveMatcher: a frontier walk that repeatedly applies
// repeating over the block's next-block frontier until exit matches.
func Recursive(repeating, exit Matcher, optional bool) *BaseMatcher {
	bm := &BaseMatcher{
		optional:       optional,
		includeExcess:  true,
		includeBounces: true,
		testSelf:       func(b *tree.Block) bool { return repeating.TestSelf(b) },
	}
	bm.childProcessor = func(_ *BaseMatcher, block *tree.Block, blocks *[]*tree.Block) bool {
		current := []*tree.Block{block}
		var toAdd []*tree.Block
		finished := false
		for {
			check := current
			current = nil
			for _, c := range check {
				if exit != nil {
					if r := exit.TryBuild(c); r != nil {
						toAdd = append(toAdd, r...)
						finished = true
					}
				}
				if finished {
					break
				}
				if r := repeating.TryBuild(c); r != nil {
					toAdd = append(toAdd, r...)
					for _, b := range r {
						current = append(current, b.NextBlocks...)
					}
					break
				}
			}
			if finished {
				break
			}
			if len(current) == 0 {
				break
			}
		}
		if len(toAdd) > 0 {
			*blocks = append(*blocks, dedupBlocks(toAdd)...)
			return true
		}
		return false
	}
	return bm
}

func dedupBlocks(in []*tree.Block) []*tree.Block {
	seen := map[*tree.Block]bool{}
	var out []*tree.Block
	for _, b := range in {
		if !seen[b] {
			seen[b] = true
			out = append(out, b)
		}
	}
	return out
}

// ChildSequence ports child_sequence_matcher: chains matchers via ChildMatcher.
func ChildSequence(matchers ...Matcher) Matcher {
	if len(matchers) == 0 {
		return nil
	}
	if len(matchers) == 1 {
		return matchers[0]
	}
	for i := 0; i < len(matchers)-1; i++ {
		if bm, ok := matchers[i].(*BaseMatcher); ok {
			bm.ChildMatcher = matchers[i+1]
		}
	}
	return matchers[0]
}
