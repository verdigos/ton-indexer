package parse

import (
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
)

func TestToncoDeployPoolTickSpacingStringPayload(t *testing.T) {
	for _, want := range []string{"-1", "0", "8388607"} {
		t.Run(want, func(t *testing.T) {
			payload, err := EncodeCompactForTest(map[string]interface{}{
				"tick_spacing": want,
			})
			if err != nil {
				t.Fatalf("EncodeCompactForTest: %v", err)
			}
			raw := &models.RawAction{Type: "tonco_deploy_pool"}
			if err := DecodeActionPayload(payload, raw); err != nil {
				t.Fatalf("DecodeActionPayload: %v", err)
			}
			if raw.ToncoDeployPoolTickSpacing == nil {
				t.Fatal("TickSpacing is nil")
			}
			if got := *raw.ToncoDeployPoolTickSpacing; got != want {
				t.Fatalf("TickSpacing = %q, want %q", got, want)
			}
		})
	}
}
