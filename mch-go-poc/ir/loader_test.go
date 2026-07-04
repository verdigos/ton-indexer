package ir

import (
	"testing"

	"mchpoc/engine"
)

// TestBuildSample loads the hand-crafted artifact and wires every matcher,
// including recursive_probe whose node 4<->5 form a genuine cycle. If two-pass
// wiring were wrong (eager single-pass), the cycle would deadlock or nil-deref.
func TestBuildSample(t *testing.T) {
	doc, err := Load("testdata/sample-ir.json")
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	log := &engine.MatchLog{}
	ms, err := doc.Build([]string{"jetton_transfer", "recursive_probe"}, log, DefaultPredicates())
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	if len(ms) != 2 {
		t.Fatalf("want 2 matchers, got %d", len(ms))
	}
	for i, m := range ms {
		if m == nil {
			t.Fatalf("matcher %d is nil", i)
		}
	}
}

func TestBuildUnknownMatcher(t *testing.T) {
	doc, err := Load("testdata/sample-ir.json")
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	if _, err := doc.Build([]string{"nope"}, &engine.MatchLog{}, nil); err == nil {
		t.Fatal("expected error for unknown matcher name")
	}
}
