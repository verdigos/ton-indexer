package emulated

import (
	"math/big"
	"os"
	"strings"
	"testing"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
	"github.com/vmihailenco/msgpack/v5"
)

// TestActionsWireFixture checks the action writer and decoder wire contract.
//
// Generate testdata/actions_wire.msgpack with:
//
//	ton-mch-dev-engine --actions-msgpack-out ton-index-go/index/emulated/testdata/actions_wire.msgpack
//
// The fixture covers every writer wire type. Each action is a real stored type whose
// details travel in the `payload` bytes; the decoder is the shared payload decoder.
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
	// Child-action recursion: the row is a leg some non-v1_ops action absorbed.
	str("parent_action_id", a.ParentActionId, "QIoX0flkEEHBMhk2Z6QlnxrXlWXACZBCee6T8YKYSKE=")
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

	byType := map[string]*Action{}
	for i := range actions {
		byType[actions[i].Type] = &actions[i]
	}

	rawOf := func(kind string) *models.RawAction {
		t.Helper()
		action, ok := byType[kind]
		if !ok {
			t.Fatalf("fixture has no %s action", kind)
		}
		converted, err := action.ToRawAction()
		if err != nil {
			t.Fatalf("%s: ToRawAction: %v", kind, err)
		}
		return converted
	}

	// A bounded *int64 beside the status STRING the parser maps from the code.
	lzVerify := rawOf("layerzero_dvn_verify")
	if lzVerify.LayerzeroDvnVerifyNonce == nil || *lzVerify.LayerzeroDvnVerifyNonce != 125 {
		t.Errorf("dvn nonce = %v, want 125", lzVerify.LayerzeroDvnVerifyNonce)
	}
	str("layerzero_dvn_verify.status", lzVerify.LayerzeroDvnVerifyStatus, "succeeded")

	// uint64 keys use decimal strings: send_request_id sits above 2^63 and the
	// fees are uint128-domain, so the writer cannot emit them as integers.
	lzSend := rawOf("layerzero_send")
	if lzSend.LayerzeroSendSendRequestId == nil ||
		*lzSend.LayerzeroSendSendRequestId != 18446744073709551614 {
		t.Errorf("send_request_id = %v, want 18446744073709551614", lzSend.LayerzeroSendSendRequestId)
	}
	if lzSend.LayerzeroSendNativeFee == nil || lzSend.LayerzeroSendNativeFee.Cmp(big.NewInt(14835102020)) != 0 {
		t.Errorf("native_fee = %v, want 14835102020", lzSend.LayerzeroSendNativeFee)
	}
	if lzSend.LayerzeroSendZroFee == nil || lzSend.LayerzeroSendZroFee.Sign() != 0 {
		t.Errorf("zro_fee = %v, want 0", lzSend.LayerzeroSendZroFee)
	}
	str("layerzero_send.msglib", lzSend.LayerzeroSendMsglib, "0x63bdfbf347f883dd")
	// Packet keys and send keys share one flat map.
	packet := rawOf("layerzero_receive")
	if packet.LayerzeroPacketSrcEid == nil || *packet.LayerzeroPacketSrcEid != 30101 {
		t.Errorf("src_eid = %v, want 30101", packet.LayerzeroPacketSrcEid)
	}
	if packet.LayerzeroPacketNonce == nil || *packet.LayerzeroPacketNonce != 1234567890123 {
		t.Errorf("packet nonce = %v, want 1234567890123", packet.LayerzeroPacketNonce)
	}

	// The layerzero_commit_packet row moves the former asset columns into the payload;
	// the decoder lands them back on Asset/AssetSecondary for the frozen API read.
	commit := rawOf("layerzero_commit_packet")
	if commit.Asset == nil || string(*commit.Asset) != "0:"+strings.Repeat("C", 64) {
		t.Errorf("channel = %v", commit.Asset)
	}
	if commit.AssetSecondary == nil || string(*commit.AssetSecondary) != "0:"+strings.Repeat("D", 64) {
		t.Errorf("msglib_connection = %v", commit.AssetSecondary)
	}

	// All ten tonco_deploy_pool fields are *string, but only the Q64.96 price exceeds
	// int64 and therefore has to travel as a decimal string; the u16 fees and the int24
	// tick spacing stay natural integers and reach the same *string field.
	tonco := rawOf("tonco_deploy_pool")
	str("tonco_deploy_pool.initial_price_x96", tonco.ToncoDeployPoolInitialPriceX96, "2159938633973433351177391787024")
	str("tonco_deploy_pool.protocol_fee", tonco.ToncoDeployPoolProtocolFee, "33268")
	str("tonco_deploy_pool.tick_spacing", tonco.ToncoDeployPoolTickSpacing, "60")
	if tonco.ToncoDeployPoolPoolActive == nil || !*tonco.ToncoDeployPoolPoolActive {
		t.Errorf("pool_active = %v, want true", tonco.ToncoDeployPoolPoolActive)
	}
	// Legs reach the indexed fields.
	if tonco.ToncoDeployPoolJetton0Minter == nil || tonco.ToncoDeployPoolJetton1Minter != nil {
		t.Errorf("jetton minters = %v / %v", tonco.ToncoDeployPoolJetton0Minter, tonco.ToncoDeployPoolJetton1Minter)
	}
	if tonco.ToncoDeployPoolJetton1RouterWallet == nil {
		t.Error("ToRawAction dropped jetton1 router wallet")
	}

	// These Cocoon composites cover every family wire shape.
	payout := rawOf("cocoon_worker_payout")
	str("cocoon_worker_payout.payout_type", payout.CocoonWorkerPayoutPayoutType, "last")
	str("cocoon_worker_payout.new_tokens", payout.CocoonWorkerPayoutNewTokens, "9229614747703451079")
	str("cocoon_worker_payout.worker_tokens", payout.CocoonWorkerPayoutWorkerTokens, "73063940556")
	if payout.CocoonWorkerPayoutWorkerState == nil || *payout.CocoonWorkerPayoutWorkerState != 2 {
		t.Errorf("worker_state = %v, want 2", payout.CocoonWorkerPayoutWorkerState)
	}
	str("cocoon_client_register.nonce", rawOf("cocoon_client_register").CocoonClientRegisterNonce, "11924145372215500834")
	if refund := rawOf("cocoon_client_request_refund"); refund.CocoonClientRequestRefundViaWallet == nil ||
		!*refund.CocoonClientRequestRefundViaWallet {
		t.Errorf("via_wallet = %v, want true", refund.CocoonClientRequestRefundViaWallet)
	}
	if seqno := rawOf("cocoon_unregister_proxy").CocoonUnregisterProxySeqno; seqno == nil || *seqno != 1 {
		t.Errorf("seqno = %v, want 1", seqno)
	}

	if flags := rawOf("change_dns").ChangeDNSRecordFlags; flags == nil || *flags != 7 {
		t.Errorf("change_dns.flags = %v, want 7", flags)
	}

	// --- []string, whose elements bypass the flexible decoders --------------
	claim := rawOf("jvault_claim")
	if len(claim.JvaultClaimClaimedJettons) != 2 {
		t.Fatalf("claimed_rewards = %+v", claim.JvaultClaimClaimedJettons)
	}
	if claim.JvaultClaimClaimedJettons[1].Amount == nil ||
		*claim.JvaultClaimClaimedJettons[1].Amount != "340282366920938463463374607431768211456" {
		t.Errorf("claimed_rewards[1].amount = %v", claim.JvaultClaimClaimedJettons[1].Amount)
	}
	whitelist := rawOf("vesting_add_whitelist")
	wantAccounts := []string{"0:" + strings.Repeat("A", 64), "0:" + strings.Repeat("B", 64)}
	if len(whitelist.VestingAddWhitelistAccountsAdded) != len(wantAccounts) {
		t.Fatalf("accounts_added = %v, want %v", whitelist.VestingAddWhitelistAccountsAdded, wantAccounts)
	}
	for i, want := range wantAccounts {
		if string(whitelist.VestingAddWhitelistAccountsAdded[i]) != want {
			t.Errorf("accounts_added[%d] = %q, want %q", i, whitelist.VestingAddWhitelistAccountsAdded[i], want)
		}
	}

	// The unstake row moved its opcode column into data.exit_code, which lands on the
	// Opcode field the API's ExitCode reads.
	unstake := rawOf("jvault_unstake")
	if unstake.Opcode == nil || int64(*unstake.Opcode) != 260734629 {
		t.Errorf("exit_code = %v, want 260734629", unstake.Opcode)
	}

	// --- multisig *int64 / *uint32 from decimal strings ----------------------
	create := rawOf("multisig_create_order")
	if create.MultisigCreateOrderCreatorIndex == nil || *create.MultisigCreateOrderCreatorIndex != 2 {
		t.Errorf("signer_index = %v, want 2", create.MultisigCreateOrderCreatorIndex)
	}
	if create.MultisigCreateOrderExpirationDate == nil || *create.MultisigCreateOrderExpirationDate != 1785595662 {
		t.Errorf("expiration_date = %v", create.MultisigCreateOrderExpirationDate)
	}
	approve := rawOf("multisig_approve")
	if approve.MultisigApproveSignerIndex == nil || *approve.MultisigApproveSignerIndex != -1 {
		t.Errorf("signer_index = %v, want -1", approve.MultisigApproveSignerIndex)
	}
	str("multisig_execute.order_seqno", rawOf("multisig_execute").MultisigExecuteOrderSeqno, "899")
}
