package emulated

import (
	"os"
	"testing"

	"github.com/vmihailenco/msgpack/v5"
)

// TestActionsWireFixture checks the action writer and decoder wire contract.
//
// Generate testdata/actions_wire.msgpack with:
//
//	ton-mch-dev-engine --actions-msgpack-out ton-index-go/index/emulated/testdata/actions_wire.msgpack
//
// The engine's writer self-test fixture holds one jetton_transfer action whose details
// travel in the `payload` bytes; the decoder is the shared payload decoder. The other
// stored types are covered by TestPayloadContract, which runs every type over the real
// reader with engine-produced payload bytes.
func TestActionsWireFixture(t *testing.T) {
	blob, err := os.ReadFile("testdata/actions_wire.msgpack")
	if err != nil {
		t.Fatalf("read fixture: %v", err)
	}

	var actions []Action
	if err := msgpack.Unmarshal(blob, &actions); err != nil {
		t.Fatalf("Unmarshal: %v", err)
	}
	if len(actions) == 0 {
		t.Fatal("fixture is empty")
	}
	a := actions[0]

	str := func(name string, got *string, want string) {
		t.Helper()
		if got == nil {
			t.Errorf("%s is nil, want %q", name, want)
		} else if *got != want {
			t.Errorf("%s = %q, want %q", name, *got, want)
		}
	}

	// --- fixed keys ---------------------------------------------------------
	if a.Type != "jetton_transfer" {
		t.Errorf("type = %q, want jetton_transfer", a.Type)
	}
	if !a.Success {
		t.Error("success = false, want true")
	}
	if a.StartLt == nil || *a.StartLt != 58291746000001 {
		t.Errorf("start_lt = %v, want 58291746000001", a.StartLt)
	}
	if a.EndUtime == nil || *a.EndUtime != 1753900002 {
		t.Errorf("end_utime = %v, want 1753900002", a.EndUtime)
	}
	// min over the view's node finalities: {confirmed, finalized} -> confirmed.
	if a.Finality != 1 {
		t.Errorf("finality = %d, want 1 (confirmed)", a.Finality)
	}
	if len(a.TxHashes) != 2 || a.TxHashes[0] != "txA" {
		t.Errorf("tx_hashes = %v", a.TxHashes)
	}
	if len(a.Accounts) != 2 {
		t.Errorf("accounts = %v", a.Accounts)
	}
	if len(a.AncestorType) != 1 || a.AncestorType[0] != "jvault_stake" {
		t.Errorf("ancestor_type = %v", a.AncestorType)
	}

	// --- the natural-types rule --------------------------------------------
	// opcode is *uint32: a natural msgpack integer.
	if a.Opcode == nil || *a.Opcode != 0x0f8a7ea5 {
		t.Errorf("opcode = %v, want 0x0f8a7ea5", a.Opcode)
	}
	// amount is *string and 256-bit: only a decimal string survives.
	str("amount", a.Amount, "340282366920938463463374607431768211456")

	// --- payload details ----------------------------------------------------
	// Details live in the payload bytes, decoded by the shared decoder.
	raw, err := a.ToRawAction()
	if err != nil {
		t.Fatalf("ToRawAction: %v", err)
	}
	// The wire ancestry list is copied onto the RawAction the API embeds as raw_action;
	// the three baseline rows that embed one carry no ancestor_type, so nothing else
	// covers this copy.
	if len(raw.AncestorType) != 1 || raw.AncestorType[0] != "jvault_stake" {
		t.Errorf("raw ancestor_type = %v", raw.AncestorType)
	}
	str("jetton_transfer.query_id", raw.JettonTransferQueryId, "18446744073709551615")
	str("jetton_transfer.forward_amount", raw.JettonTransferForwardAmount, "400000000")
	if raw.JettonTransferComment == nil || *raw.JettonTransferComment != "hello" {
		t.Errorf("jetton_transfer.comment = %v", raw.JettonTransferComment)
	}
	if raw.JettonTransferIsEncryptedComment == nil || *raw.JettonTransferIsEncryptedComment {
		t.Errorf("jetton_transfer.comment_encrypted = %v", raw.JettonTransferIsEncryptedComment)
	}

	// The writer omits an unfilled row column, so unset envelope fields stay nil.
	if a.TraceId != nil || a.TraceExternalHash != "" {
		t.Errorf("trace fields = %v / %q", a.TraceId, a.TraceExternalHash)
	}
}
