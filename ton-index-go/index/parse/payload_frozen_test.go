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
	frozenPayloadSHA256 = "1aaf019053df85f4a04fae675bb62ba67e3ef9597b8ce5533dd7798f12db780a"
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
