package parse_test

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"testing"
)

// TestFrozenPayloadBytes pins the fixture SHA-256 and each row's API JSON bytes.
// Each row includes its wire envelope because payload bytes alone cannot reproduce those
// fields; registry id or type changes therefore fail against the pinned output.
const (
	frozenPayloadPath   = "testdata/payload_frozen_v1.jsonl.gz"
	frozenPayloadSHA256 = "35caf32938175163f97b23b566e6217cb94b7b36ef9739132a424eebc53db868"
	frozenPayloadRows   = 1218
)

func TestFrozenPayloadBytes(t *testing.T) {
	blob, err := os.ReadFile(frozenPayloadPath)
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(blob)
	if got := hex.EncodeToString(digest[:]); got != frozenPayloadSHA256 {
		t.Fatalf("frozen set sha256 = %s, want %s (edit only with the owner's decision)", got, frozenPayloadSHA256)
	}
	checkAPIBaseline(t, frozenPayloadPath, frozenPayloadRows)
}
