// Package ir loads the mch-ir.json IR POC artifact at schema v0.1-poc and builds native mchpoc/engine
// matchers from it; no .mch parsing happens here because the Python emitter already lowered the matcher graph.
package ir

import "encoding/json"

// IR is the whole artifact. Nodes is a single global pool; each Matcher points
// into it via Root. Indices are 0-based into Nodes.
type IR struct {
	Version           string    `json:"mch_ir_version"`
	Frontend          Frontend  `json:"frontend"`
	Matchers          []Matcher `json:"matchers"`
	Nodes             []Node    `json:"nodes"`
	RegistrationOrder []int     `json:"registration_order"`
}

// Frontend is informational provenance.
type Frontend struct {
	Generator   string   `json:"generator"`
	SourceFiles []string `json:"source_files"`
}

// Matcher is one registered matcher. Root is the node id of its anchor node;
// the anchor's test_self comes from Anchor (not the root node's own kind test),
// so multi-value opcode sets (T10 anchor groups) work.
type Matcher struct {
	Name           string    `json:"name"`
	Anchor         Anchor    `json:"anchor"`
	Root           int       `json:"root"`
	Produces       []string  `json:"produces"`
	Captures       []Capture `json:"captures"`
	IncludeExcess  *bool     `json:"include_excess"`  // default true when absent
	IncludeBounces *bool     `json:"include_bounces"` // default true when absent
	Builder        string    `json:"builder"`         // host registry name; POC ignores
}

// Anchor holds the anchor test. Values are ints for kind "opcode_set" and
// strings for kind "btype"; kept as RawMessage so the loader decodes per kind
// without going through float64.
type Anchor struct {
	Kind   string            `json:"kind"` // "opcode_set" | "btype"
	Values []json.RawMessage `json:"values"`
}

// Capture is informational in the POC (the Go engine ignores it).
type Capture struct {
	Name string `json:"name"`
	Card string `json:"card"` // "one" | "opt" | "many"
}

// Node is one flat tagged pattern node. Common optional edge/flag fields sit
// alongside the kind-specific ones; only the fields relevant to Kind are read.
// Child/Parent/Step/Exit are *int so node id 0 is distinguishable from absent.
type Node struct {
	Kind     string `json:"kind"`
	Optional bool   `json:"optional"`
	Capture  string `json:"capture"` // informational
	Where    string `json:"where"`   // predicate name, composed onto test_self
	Child    *int   `json:"child"`
	Parent   *int   `json:"parent"`
	Children []int  `json:"children"`

	// kind-specific
	Opcode    *int64 `json:"opcode"`    // contract; signed or unsigned accepted
	Btype     string `json:"btype"`     // block_type
	Pred      string `json:"pred"`      // pred
	Branches  []int  `json:"branches"`  // or
	Exclusive bool   `json:"exclusive"` // or
	Step      *int   `json:"step"`      // recursive
	Exit      *int   `json:"exit"`      // recursive
}

func (m *Matcher) primaryBtype() string {
	if len(m.Produces) > 0 {
		return m.Produces[0]
	}
	return ""
}
