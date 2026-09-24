package emulated

import (
	"bytes"
	"compress/gzip"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"strings"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"
	"github.com/vmihailenco/msgpack/v5"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

type AccountStatus int

const (
	AccountStatusUninit AccountStatus = iota
	AccountStatusFrozen
	AccountStatusActive
	AccountStatusNonexist
)

func (d AccountStatus) Str() (string, error) {
	switch d {
	case AccountStatusUninit:
		return "uninit", nil
	case AccountStatusFrozen:
		return "frozen", nil
	case AccountStatusActive:
		return "active", nil
	case AccountStatusNonexist:
		return "nonexist", nil
	default:
		return "", fmt.Errorf("unknown account status: %d", d)
	}
}

type AccStatusChange int

const (
	AccStatusUnchanged AccStatusChange = iota
	AccStatusFrozen
	AccStatusDeleted
)

func (d AccStatusChange) Str() (string, error) {
	switch d {
	case AccStatusUnchanged:
		return "unchanged", nil
	case AccStatusFrozen:
		return "frozen", nil
	case AccStatusDeleted:
		return "deleted", nil
	default:
		return "", fmt.Errorf("unknown account status change: %d", d)
	}
}

type ComputeSkipReason int

const (
	ComputeSkipNoState ComputeSkipReason = iota
	ComputeSkipBadState
	ComputeSkipNoGas
	ComputeSkipSuspended
)

func (d ComputeSkipReason) Str() (string, error) {
	switch d {
	case ComputeSkipNoState:
		return "no_state", nil
	case ComputeSkipBadState:
		return "bad_state", nil
	case ComputeSkipNoGas:
		return "no_gas", nil
	case ComputeSkipSuspended:
		return "suspended", nil
	default:
		return "", fmt.Errorf("unknown compute skip reason: %d", d)
	}
}

type trStoragePhase struct {
	StorageFeesCollected uint64          `msgpack:"storage_fees_collected" json:"storage_fees_collected"`
	StorageFeesDue       *uint64         `msgpack:"storage_fees_due" json:"storage_fees_due"`
	StatusChange         AccStatusChange `msgpack:"status_change" json:"status_change"`
}

type trCreditPhase struct {
	DueFeesCollected *uint64 `msgpack:"due_fees_collected" json:"due_fees_collected"`
	Credit           uint64  `msgpack:"credit" json:"credit"`
}

type trComputePhaseSkipped struct {
	Reason ComputeSkipReason `msgpack:"reason" json:"reason"`
}

type trComputePhaseVm struct {
	Success          bool    `msgpack:"success" json:"success"`
	MsgStateUsed     bool    `msgpack:"msg_state_used" json:"msg_state_used"`
	AccountActivated bool    `msgpack:"account_activated" json:"account_activated"`
	GasFees          uint64  `msgpack:"gas_fees" json:"gas_fees"`
	GasUsed          uint64  `msgpack:"gas_used" json:"gas_used"`
	GasLimit         uint64  `msgpack:"gas_limit" json:"gas_limit"`
	GasCredit        *uint64 `msgpack:"gas_credit" json:"gas_credit"`
	Mode             int8    `msgpack:"mode" json:"mode"`
	ExitCode         int32   `msgpack:"exit_code" json:"exit_code"`
	ExitArg          *int32  `msgpack:"exit_arg" json:"exit_arg"`
	VmSteps          uint32  `msgpack:"vm_steps" json:"vm_steps"`
	VmInitStateHash  hash    `msgpack:"vm_init_state_hash" json:"vm_init_state_hash"`
	VmFinalStateHash hash    `msgpack:"vm_final_state_hash" json:"vm_final_state_hash"`
}

type storageUsedShort struct {
	Cells uint64 `msgpack:"cells" json:"cells"`
	Bits  uint64 `msgpack:"bits" json:"bits"`
}

type trActionPhase struct {
	Success         bool             `msgpack:"success" json:"success"`
	Valid           bool             `msgpack:"valid" json:"valid"`
	NoFunds         bool             `msgpack:"no_funds" json:"no_funds"`
	StatusChange    AccStatusChange  `msgpack:"status_change" json:"status_change"`
	TotalFwdFees    *uint64          `msgpack:"total_fwd_fees" json:"total_fwd_fees"`
	TotalActionFees *uint64          `msgpack:"total_action_fees" json:"total_action_fees"`
	ResultCode      *int32           `msgpack:"result_code" json:"result_code"`
	ResultArg       *int32           `msgpack:"result_arg" json:"result_arg"`
	TotActions      *uint16          `msgpack:"tot_actions" json:"tot_actions"`
	SpecActions     *uint16          `msgpack:"spec_actions" json:"spec_actions"`
	SkippedActions  *uint16          `msgpack:"skipped_actions" json:"skipped_actions"`
	MsgsCreated     *uint16          `msgpack:"msgs_created" json:"msgs_created"`
	ActionListHash  hash             `msgpack:"action_list_hash" json:"action_list_hash"`
	TotMsgSize      storageUsedShort `msgpack:"tot_msg_size" json:"tot_msg_size"`
}

type trBouncePhaseNegfunds struct {
	Dummy bool `msgpack:"dummy" json:"dummy"`
}

type trBouncePhaseNofunds struct {
	MsgSize    storageUsedShort `msgpack:"msg_size" json:"msg_size"`
	ReqFwdFees uint64           `msgpack:"req_fwd_fees" json:"req_fwd_fees"`
}

type trBouncePhaseOk struct {
	MsgSize storageUsedShort `msgpack:"msg_size" json:"msg_size"`
	MsgFees uint64           `msgpack:"msg_fees" json:"msg_fees"`
	FwdFees uint64           `msgpack:"fwd_fees" json:"fwd_fees"`
}

type trMessage struct {
	Hash         hash    `msgpack:"hash" json:"hash"`
	Source       *string `msgpack:"source" json:"source"`
	Destination  *string `msgpack:"destination" json:"destination"`
	Value        *uint64 `msgpack:"value" json:"value"`
	FwdFee       *uint64 `msgpack:"fwd_fee" json:"fwd_fee"`
	IhrFee       *uint64 `msgpack:"ihr_fee" json:"ihr_fee"`
	ExtraFlags   *string `msgpack:"extra_flags" json:"extra_flags"`
	CreatedLt    *uint64 `msgpack:"created_lt" json:"created_lt"`
	CreatedAt    *uint32 `msgpack:"created_at" json:"created_at"`
	Opcode       *int32  `msgpack:"opcode" json:"opcode"`
	IhrDisabled  *bool   `msgpack:"ihr_disabled" json:"ihr_disabled"`
	Bounce       *bool   `msgpack:"bounce" json:"bounce"`
	Bounced      *bool   `msgpack:"bounced" json:"bounced"`
	ImportFee    *uint64 `msgpack:"import_fee" json:"import_fee"`
	BodyBoc      string  `msgpack:"body_boc" json:"body_boc"`
	InitStateBoc *string `msgpack:"init_state_boc" json:"init_state_boc"`
	HashNorm     *hash   `msgpack:"hash_norm" json:"hash_norm"`
}

type transactionDescr struct {
	CreditFirst bool            `msgpack:"credit_first" json:"credit_first"`
	StoragePh   *trStoragePhase `msgpack:"storage_ph" json:"storage_ph"`
	CreditPh    *trCreditPhase  `msgpack:"credit_ph" json:"credit_ph"`
	ComputePh   computePhaseVar `msgpack:"compute_ph" json:"compute_ph"`
	Action      *trActionPhase  `msgpack:"action" json:"action"`
	Aborted     bool            `msgpack:"aborted" json:"aborted"`
	Bounce      *BouncePhaseVar `msgpack:"bounce" json:"bounce"`
	Destroyed   bool            `msgpack:"destroyed" json:"destroyed"`
}

type transaction struct {
	Hash                   hash             `msgpack:"hash" json:"-"`
	Account                string           `msgpack:"account" json:"account"`
	Lt                     uint64           `msgpack:"lt" json:"lt,string"`
	PrevTransHash          hash             `msgpack:"prev_trans_hash" json:"prev_trans_hash"`
	PrevTransLt            uint64           `msgpack:"prev_trans_lt" json:"prev_trans_lt,string"`
	Now                    uint32           `msgpack:"now" json:"now"`
	OrigStatus             AccountStatus    `msgpack:"orig_status" json:"orig_status"`
	EndStatus              AccountStatus    `msgpack:"end_status" json:"end_status"`
	InMsg                  *trMessage       `msgpack:"in_msg" json:"in_msg"`
	OutMsgs                []trMessage      `msgpack:"out_msgs" json:"out_msgs"`
	TotalFees              uint64           `msgpack:"total_fees" json:"total_fees"`
	AccountStateHashBefore hash             `msgpack:"account_state_hash_before" json:"account_state_hash_before"`
	AccountStateHashAfter  hash             `msgpack:"account_state_hash_after" json:"account_state_hash_after"`
	Description            transactionDescr `msgpack:"description" json:"description"`
}

type Action struct {
	ActionId              string               `msgpack:"action_id"`
	Type                  string               `msgpack:"type"`
	TraceId               *string              `msgpack:"trace_id"`
	TraceExternalHash     string               `msgpack:"trace_external_hash"`
	TraceExternalHashNorm *string              `msgpack:"trace_external_hash_norm"`
	TxHashes              []string             `msgpack:"tx_hashes"`
	Value                 *string              `msgpack:"value"`
	Amount                *string              `msgpack:"amount"`
	StartLt               *uint64              `msgpack:"start_lt"`
	EndLt                 *uint64              `msgpack:"end_lt"`
	StartUtime            *uint32              `msgpack:"start_utime"`
	EndUtime              *uint32              `msgpack:"end_utime"`
	TraceEndLt            *uint64              `msgpack:"trace_end_lt"`
	TraceEndUtime         *uint32              `msgpack:"trace_end_utime"`
	TraceStartLt          *uint64              `msgpack:"trace_start_lt"`
	TraceMcSeqnoEnd       *uint32              `msgpack:"trace_mc_seqno_end"`
	Source                *string              `msgpack:"source"`
	SourceSecondary       *string              `msgpack:"source_secondary"`
	Destination           *string              `msgpack:"destination"`
	DestinationSecondary  *string              `msgpack:"destination_secondary"`
	Asset                 *string              `msgpack:"asset"`
	AssetSecondary        *string              `msgpack:"asset_secondary"`
	Asset2                *string              `msgpack:"asset2"`
	Asset2Secondary       *string              `msgpack:"asset2_secondary"`
	Opcode                *uint32              `msgpack:"opcode"`
	Success               bool                 `msgpack:"success"`
	Finality              models.FinalityState `msgpack:"finality"`
	AncestorType          []string             `msgpack:"ancestor_type"`
	ParentActionId        *string              `msgpack:"parent_action_id"`
	Accounts              []string             `msgpack:"accounts"`
	Payload               []byte               `msgpack:"payload"`
}

type blockId struct {
	Workchain int32  `msgpack:"workchain"`
	Shard     uint64 `msgpack:"shard"`
	Seqno     uint32 `msgpack:"seqno"`
}
type Trace struct {
	TraceId      *string
	ExternalHash string
	Nodes        []TraceNode
	Classified   bool
	Actions      []Action
}

type TraceNode struct {
	Transaction   transaction          `msgpack:"transaction"`
	Emulated      bool                 `msgpack:"emulated"`
	BlockId       blockId              `msgpack:"block_id"`
	McBlockSeqno  uint32               `msgpack:"mc_block_seqno"`
	FinalityState models.FinalityState `msgpack:"finality"`
	TraceId       *string
	Key           string
}

type computePhaseVar struct {
	Type uint8
	Data interface{} // Can be trComputePhaseSkipped or trComputePhaseVm
}

var _ msgpack.CustomDecoder = (*computePhaseVar)(nil)

func (bpv *computePhaseVar) DecodeMsgpack(dec *msgpack.Decoder) error {
	length, err := dec.DecodeArrayLen()
	if err != nil {
		return err
	}
	if length != 2 {
		return fmt.Errorf("invalid variant array length: %d", length)
	}

	index, err := dec.DecodeUint8()
	if err != nil {
		return err
	}

	switch index {
	case 0:
		var a trComputePhaseSkipped
		err = dec.Decode(&a)
		bpv.Data = a
	case 1:
		var b trComputePhaseVm
		err = dec.Decode(&b)
		bpv.Data = b
	default:
		return fmt.Errorf("unknown variant index: %d", index)
	}

	bpv.Type = index
	return err
}

type BouncePhaseVar struct {
	Type uint8
	Data interface{} // Can be trBouncePhaseNegfunds, trBouncePhaseNofunds or trBouncePhaseOk
}

var _ msgpack.CustomDecoder = (*BouncePhaseVar)(nil)

func (s *BouncePhaseVar) DecodeMsgpack(dec *msgpack.Decoder) error {
	length, err := dec.DecodeArrayLen()
	if err != nil {
		return err
	}
	if length != 2 {
		return fmt.Errorf("invalid variant array length: %d", length)
	}

	index, err := dec.DecodeUint8()
	if err != nil {
		return err
	}

	switch index {
	case 0:
		var a trBouncePhaseNegfunds
		err = dec.Decode(&a)
		s.Data = a
	case 1:
		var b trBouncePhaseNofunds
		err = dec.Decode(&b)
		s.Data = b
	case 2:
		var c trBouncePhaseOk
		err = dec.Decode(&c)
		s.Data = c
	default:
		return fmt.Errorf("unknown variant index: %d", index)
	}

	s.Type = index
	return err
}

type AccountState struct {
	Hash          hash    `msgpack:"hash" json:"-"`
	Balance       uint64  `msgpack:"balance" json:"balance"`
	AccountStatus string  `msgpack:"account_status" json:"account_status"`
	FrozenHash    *hash   `msgpack:"frozen_hash" json:"frozen_hash"`
	CodeHash      *hash   `msgpack:"code_hash" json:"code_hash"`
	DataBoc       *string `msgpack:"data_boc" json:"data_boc"`
	DataHash      *hash   `msgpack:"data_hash" json:"data_hash"`
	LastTransHash *hash   `msgpack:"last_trans_hash" json:"last_trans_hash"`
	LastTransLt   *uint64 `msgpack:"last_trans_lt" json:"last_trans_lt"`
	Timestamp     *uint32 `msgpack:"timestamp" json:"timestamp"`
}

type hash [32]byte

func (h hash) MarshalText() (data []byte, err error) {
	return []byte(base64.StdEncoding.EncodeToString(h[:])), nil
}

// MarshalJSON implements json.Marshaler interface
func (h hash) MarshalJSON() ([]byte, error) {
	return json.Marshal(base64.StdEncoding.EncodeToString(h[:]))
}

// EncodeMsgpack implements msgpack.CustomEncoder interface
func (h hash) EncodeMsgpack(enc *msgpack.Encoder) error {
	return enc.EncodeBytes(h[:])
}

// DecodeMsgpack implements msgpack.CustomDecoder interface
func (h *hash) DecodeMsgpack(dec *msgpack.Decoder) error {
	bytes, err := dec.DecodeBytes()
	if err != nil {
		return err
	}

	if len(bytes) != 32 {
		return fmt.Errorf("invalid hash length: expected 32 bytes, got %d", len(bytes))
	}

	copy(h[:], bytes)
	return nil
}

func decompressGzip(data []byte) ([]byte, error) {
	if len(data) < 2 {
		return data, nil // too short for gzip
	}
	// check if gzip, else return data unchanged
	if data[0] == 0x1f && data[1] == 0x8b {
		reader, err := gzip.NewReader(bytes.NewReader(data))
		if err != nil {
			return nil, err
		}
		defer reader.Close()
		return io.ReadAll(reader)
	} else {
		return data, nil
	}
}

func ConvertHSet(traceHash map[string]string, traceKey string) (Trace, error) {

	queue := make([]string, 0)

	rootNodeId, exists := traceHash["root_node"]
	if !exists {
		return Trace{}, fmt.Errorf("root_node not found in trace %s", traceKey)
	}
	queue = append(queue, rootNodeId)
	txs := make([]TraceNode, 0)
	actions := make([]Action, 0)
	var endLt uint64 = 0
	var endUtime uint32 = 0
	var mcSeqnoEnd uint32 = 0
	var traceId *string
	for len(queue) > 0 {
		key := queue[0]
		queue = queue[1:]
		var node TraceNode
		nodeData, exists := traceHash[key]
		if !exists {
			return Trace{}, fmt.Errorf("key %s not found in trace", key)
		}
		nodeBytes := []byte(nodeData)
		err := msgpack.Unmarshal(nodeBytes, &node)
		node.Key = key
		if key == rootNodeId && !node.Emulated {
			id := base64.StdEncoding.EncodeToString(node.Transaction.Hash[:])
			traceId = &id
		}

		node.TraceId = traceId
		txs = append(txs, node)

		if err != nil {
			return Trace{}, fmt.Errorf("failed to unmarshal node: %w", err)
		}
		for _, outMsg := range node.Transaction.OutMsgs {
			// bytes to hex string
			nextKey := base64.StdEncoding.EncodeToString(outMsg.Hash[:])
			if _, exists := traceHash[nextKey]; exists {
				queue = append(queue, nextKey)
			}
		}
		if endLt < node.Transaction.Lt {
			endLt = node.Transaction.Lt
		}
		if endUtime < node.Transaction.Now {
			endUtime = node.Transaction.Now
		}
		if mcSeqnoEnd < node.McBlockSeqno {
			mcSeqnoEnd = node.McBlockSeqno
		}
	}
	if actionsBytes, exists := traceHash["actions"]; exists {
		decompressed, err := decompressGzip([]byte(actionsBytes))
		if err != nil {
			return Trace{}, fmt.Errorf("failed to decompress actions: %w", err)
		}
		err = msgpack.Unmarshal(decompressed, &actions)
		if err != nil {
			return Trace{}, fmt.Errorf("failed to unmarshal actions: %w", err)
		}
		for i := range actions {
			actions[i].TraceEndUtime = &endUtime
			actions[i].TraceEndLt = &endLt
			actions[i].TraceMcSeqnoEnd = &mcSeqnoEnd
			actions[i].TraceExternalHash = rootNodeId
			actions[i].TraceId = traceId
		}
	}
	_, has_actions := traceHash["actions"]
	return Trace{
		TraceId:      traceId,
		ExternalHash: rootNodeId,
		Nodes:        txs,
		Classified:   has_actions,
		Actions:      actions,
	}, nil
}

func (h hash) Base64Ptr() *string {
	str := base64.StdEncoding.EncodeToString(h[:])
	return &str
}

func ptrInt64FromUint64(v *uint64) *int64 {
	if v == nil {
		return nil
	}
	i := int64(*v)
	return &i
}

func ptrInt32FromUint16(v *uint16) *int32 {
	if v == nil {
		return nil
	}
	i := int32(*v)
	return &i
}

// normAddr uppercases raw address hex for address-book lookups. Non-raw input is
// returned unchanged, and uppercase input does not allocate.
func normAddr(s string) string {
	i := strings.IndexByte(s, ':')
	if i < 0 {
		return s
	}
	for j := i + 1; j < len(s); j++ {
		switch c := s[j]; {
		case c >= 'a' && c <= 'f':
			return s[:i+1] + strings.ToUpper(s[i+1:])
		case (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'):
		default:
			return s
		}
	}
	return s
}

func ptrAccountAddress(s *string) *models.AccountAddress {
	if s == nil {
		return nil
	}
	a := models.AccountAddress(normAddr(*s))
	return &a
}

func ptrHashType(s *string) *models.HashType {
	return (*models.HashType)(s)
}

func ptrOpcodeFromUint32(v *uint32) *models.OpcodeType {
	if v == nil {
		return nil
	}
	o := models.OpcodeType(int64(*v))
	return &o
}

func ptrOpcodeFromInt32(v *int32) *models.OpcodeType {
	if v == nil {
		return nil
	}
	o := models.OpcodeType(int64(*v))
	return &o
}

func int64FromPtrUint64(v *uint64) int64 {
	if v == nil {
		return 0
	}
	return int64(*v)
}

func int64FromPtrUint32(v *uint32) int64 {
	if v == nil {
		return 0
	}
	return int64(*v)
}

func int32FromPtrUint32(v *uint32) int32 {
	if v == nil {
		return 0
	}
	return int32(*v)
}
func calcHash(boc string) (string, error) {
	decodedBody, err := base64.StdEncoding.DecodeString(boc)
	if err != nil {
		return "", err
	}
	msg_cell, err := cell.FromBOC(decodedBody)
	if err != nil {
		return "", err
	}
	hash := msg_cell.Hash()
	// bytes to
	hash_b64 := base64.StdEncoding.EncodeToString(hash[:])
	return hash_b64, nil
}

func (a *Action) ToRawAction() (*models.RawAction, error) {
	var traceExternalHashNorm *models.HashType
	if a.TraceExternalHashNorm != nil && *a.TraceExternalHashNorm != a.TraceExternalHash {
		traceExternalHashNorm = (*models.HashType)(a.TraceExternalHashNorm)
	}

	// Convert TxHashes []string -> []models.HashType
	txHashes := make([]models.HashType, len(a.TxHashes))
	for i, h := range a.TxHashes {
		txHashes[i] = models.HashType(h)
	}
	success := a.Success

	traceExternalHash := models.HashType(a.TraceExternalHash)

	accounts := make([]models.AccountAddress, 0, len(a.Accounts))
	for _, acc := range a.Accounts {
		accounts = append(accounts, models.AccountAddress(normAddr(acc)))
	}

	raw := &models.RawAction{
		TraceId:               ptrHashType(a.TraceId),
		ActionId:              models.HashType(a.ActionId),
		StartLt:               int64FromPtrUint64(a.StartLt),
		EndLt:                 int64FromPtrUint64(a.EndLt),
		StartUtime:            int64FromPtrUint32(a.StartUtime),
		EndUtime:              int64FromPtrUint32(a.EndUtime),
		TraceEndLt:            int64FromPtrUint64(a.TraceEndLt),
		TraceEndUtime:         int64FromPtrUint32(a.TraceEndUtime),
		TraceMcSeqnoEnd:       int32FromPtrUint32(a.TraceMcSeqnoEnd),
		Source:                ptrAccountAddress(a.Source),
		SourceSecondary:       ptrAccountAddress(a.SourceSecondary),
		Destination:           ptrAccountAddress(a.Destination),
		DestinationSecondary:  ptrAccountAddress(a.DestinationSecondary),
		Asset:                 ptrAccountAddress(a.Asset),
		AssetSecondary:        ptrAccountAddress(a.AssetSecondary),
		Asset2:                ptrAccountAddress(a.Asset2),
		Asset2Secondary:       ptrAccountAddress(a.Asset2Secondary),
		Opcode:                ptrOpcodeFromUint32(a.Opcode),
		TxHashes:              txHashes,
		Type:                  a.Type,
		Value:                 a.Value,
		Amount:                a.Amount,
		Success:               &success,
		Finality:              a.Finality,
		TraceExternalHash:     &traceExternalHash,
		TraceExternalHashNorm: traceExternalHashNorm,
		ExtraCurrencies:       map[string]string{},
		AncestorType:          a.AncestorType,
		Accounts:              accounts,
	}

	// The emulator row carries the same payload bytes as the DB `payload` column; the
	// DB path and this one share DecodeActionPayload. A row without a payload column
	// has no details.
	if len(a.Payload) != 0 {
		if err := parse.DecodeActionPayload(a.Payload, raw); err != nil {
			return nil, err
		}
	}
	return raw, nil
}

func (n *TraceNode) ToTransaction() (*models.Transaction, error) {
	origStatus, err := n.Transaction.OrigStatus.Str()
	if err != nil {
		return nil, err
	}
	endStatus, err := n.Transaction.EndStatus.Str()
	if err != nil {
		return nil, err
	}

	ordVal := "ord"
	isTock := false

	t := &models.Transaction{
		Account:                  models.AccountAddress(normAddr(n.Transaction.Account)),
		Hash:                     models.HashType(*n.Transaction.Hash.Base64Ptr()),
		Lt:                       int64(n.Transaction.Lt),
		Now:                      int32(n.Transaction.Now),
		Workchain:                n.BlockId.Workchain,
		Shard:                    models.ShardId(int64(n.BlockId.Shard)),
		Seqno:                    int32(n.BlockId.Seqno),
		McSeqno:                  int32(n.McBlockSeqno),
		TraceId:                  ptrHashType(n.TraceId),
		PrevTransHash:            models.HashType(*n.Transaction.PrevTransHash.Base64Ptr()),
		PrevTransLt:              int64(n.Transaction.PrevTransLt),
		OrigStatus:               origStatus,
		EndStatus:                endStatus,
		TotalFees:                int64(n.Transaction.TotalFees),
		TotalFeesExtraCurrencies: map[string]string{},
		AccountStateHashBefore:   models.HashType(*n.Transaction.AccountStateHashBefore.Base64Ptr()),
		AccountStateHashAfter:    models.HashType(*n.Transaction.AccountStateHashAfter.Base64Ptr()),
		Descr: models.TransactionDescr{
			Type:        ordVal,
			Aborted:     &n.Transaction.Description.Aborted,
			Destroyed:   &n.Transaction.Description.Destroyed,
			CreditFirst: &n.Transaction.Description.CreditFirst,
			IsTock:      &isTock,
			Installed:   &isTock,
		},
		OutMsgs:  []*models.Message{},
		Emulated: n.Emulated,
		Finality: n.FinalityState,
	}

	t.BlockRef = models.BlockId{
		Workchain: n.BlockId.Workchain,
		Shard:     models.ShardId(int64(n.BlockId.Shard)),
		Seqno:     int32(n.BlockId.Seqno),
	}
	t.AccountStateBefore = &models.AccountState{Hash: t.AccountStateHashBefore}
	t.AccountStateAfter = &models.AccountState{Hash: t.AccountStateHashAfter}

	var st models.StoragePhase
	if n.Transaction.Description.StoragePh != nil {
		statusChange, err := n.Transaction.Description.StoragePh.StatusChange.Str()
		if err != nil {
			return nil, err
		}
		collected := int64(n.Transaction.Description.StoragePh.StorageFeesCollected)
		st.StorageFeesCollected = &collected
		st.StorageFeesDue = ptrInt64FromUint64(n.Transaction.Description.StoragePh.StorageFeesDue)
		st.StatusChange = &statusChange
		t.Descr.StoragePh = &st
	}

	var cr models.CreditPhase
	if n.Transaction.Description.CreditPh != nil {
		cr.DueFeesCollected = ptrInt64FromUint64(n.Transaction.Description.CreditPh.DueFeesCollected)
		credit := int64(n.Transaction.Description.CreditPh.Credit)
		cr.Credit = &credit
		cr.CreditExtraCurrencies = map[string]string{}
		t.Descr.CreditPh = &cr
	}

	var co models.ComputePhase
	if n.Transaction.Description.ComputePh.Type == 0 {
		isSkipped := true
		co.IsSkipped = &isSkipped
		reason, err := n.Transaction.Description.ComputePh.Data.(trComputePhaseSkipped).Reason.Str()
		if err != nil {
			return nil, err
		}
		co.Reason = &reason
		t.Descr.ComputePh = &co
	} else {
		isSkipped := false
		co.IsSkipped = &isSkipped
		vm := n.Transaction.Description.ComputePh.Data.(trComputePhaseVm)
		co.Success = &vm.Success
		co.MsgStateUsed = &vm.MsgStateUsed
		co.AccountActivated = &vm.AccountActivated
		gasFees := int64(vm.GasFees)
		co.GasFees = &gasFees
		gasUsed := int64(vm.GasUsed)
		co.GasUsed = &gasUsed
		gasLimit := int64(vm.GasLimit)
		co.GasLimit = &gasLimit
		co.GasCredit = ptrInt64FromUint64(vm.GasCredit)
		mode := int32(vm.Mode)
		co.Mode = &mode
		exitCode := int32(vm.ExitCode)
		co.ExitCode = &exitCode
		co.ExitArg = vm.ExitArg
		vmSteps := uint32(vm.VmSteps)
		co.VmSteps = &vmSteps
		co.VmInitStateHash = ptrHashType(vm.VmInitStateHash.Base64Ptr())
		co.VmFinalStateHash = ptrHashType(vm.VmFinalStateHash.Base64Ptr())
		t.Descr.ComputePh = &co
	}

	if n.Transaction.Description.Action != nil {
		var ap models.ActionPhase
		statusChange, err := n.Transaction.Description.Action.StatusChange.Str()
		if err != nil {
			return nil, err
		}
		ap.Success = &n.Transaction.Description.Action.Success
		ap.Valid = &n.Transaction.Description.Action.Valid
		ap.NoFunds = &n.Transaction.Description.Action.NoFunds
		ap.StatusChange = &statusChange
		ap.TotalFwdFees = ptrInt64FromUint64(n.Transaction.Description.Action.TotalFwdFees)
		ap.TotalActionFees = ptrInt64FromUint64(n.Transaction.Description.Action.TotalActionFees)
		ap.ResultCode = n.Transaction.Description.Action.ResultCode
		ap.ResultArg = n.Transaction.Description.Action.ResultArg
		ap.TotActions = ptrInt32FromUint16(n.Transaction.Description.Action.TotActions)
		ap.SpecActions = ptrInt32FromUint16(n.Transaction.Description.Action.SpecActions)
		ap.SkippedActions = ptrInt32FromUint16(n.Transaction.Description.Action.SkippedActions)
		ap.MsgsCreated = ptrInt32FromUint16(n.Transaction.Description.Action.MsgsCreated)
		ap.ActionListHash = ptrHashType(n.Transaction.Description.Action.ActionListHash.Base64Ptr())
		cells := int64(n.Transaction.Description.Action.TotMsgSize.Cells)
		bits := int64(n.Transaction.Description.Action.TotMsgSize.Bits)
		ap.TotMsgSize = &models.MsgSize{Cells: &cells, Bits: &bits}
		t.Descr.Action = &ap
	}

	if n.Transaction.Description.Bounce != nil {
		var bp models.BouncePhase
		if n.Transaction.Description.Bounce.Type == 0 {
			btype := "negfunds"
			bp.Type = &btype
		} else if n.Transaction.Description.Bounce.Type == 1 {
			btype := "nofunds"
			bp.Type = &btype
			phase := n.Transaction.Description.Bounce.Data.(trBouncePhaseNofunds)
			cells := int64(phase.MsgSize.Cells)
			bits := int64(phase.MsgSize.Bits)
			bp.MsgSize = &models.MsgSize{Cells: &cells, Bits: &bits}
			reqFwdFees := int64(phase.ReqFwdFees)
			bp.ReqFwdFees = &reqFwdFees
		} else {
			btype := "ok"
			bp.Type = &btype
			phase := n.Transaction.Description.Bounce.Data.(trBouncePhaseOk)
			cells := int64(phase.MsgSize.Cells)
			bits := int64(phase.MsgSize.Bits)
			bp.MsgSize = &models.MsgSize{Cells: &cells, Bits: &bits}
			msgFees := int64(phase.MsgFees)
			bp.MsgFees = &msgFees
			fwdFees := int64(phase.FwdFees)
			bp.FwdFees = &fwdFees
		}
		t.Descr.Bounce = &bp
	}

	return t, nil
}

func (m *trMessage) toMessage(traceId string, direction string, txLt uint64, txHash string) (*models.Message, error) {
	bodyHash, err := calcHash(m.BodyBoc)
	if err != nil {
		return nil, err
	}

	var initStateHash *models.HashType
	var initStateBoc *string
	if m.InitStateBoc != nil {
		h, err := calcHash(*m.InitStateBoc)
		if err != nil {
			return nil, err
		}
		ht := models.HashType(h)
		initStateHash = &ht
		initStateBoc = m.InitStateBoc
	}

	var hashNorm *models.HashType
	if m.HashNorm != nil {
		n := base64.StdEncoding.EncodeToString((*m.HashNorm)[:])
		ht := models.HashType(n)
		hashNorm = &ht
	}

	bodyHashType := models.HashType(bodyHash)
	traceIdHash := models.HashType(traceId)
	msg := &models.Message{
		TxHash:               models.HashType(txHash),
		TxLt:                 int64(txLt),
		MsgHash:              models.HashType(base64.StdEncoding.EncodeToString(m.Hash[:])),
		Direction:            direction,
		TraceId:              &traceIdHash,
		Source:               ptrAccountAddress(m.Source),
		Destination:          ptrAccountAddress(m.Destination),
		Value:                ptrInt64FromUint64(m.Value),
		ValueExtraCurrencies: map[string]string{},
		FwdFee:               m.FwdFee,
		IhrFee:               m.IhrFee,
		ExtraFlags:           m.ExtraFlags,
		CreatedLt:            m.CreatedLt,
		CreatedAt:            m.CreatedAt,
		Opcode:               ptrOpcodeFromInt32(m.Opcode),
		IhrDisabled:          m.IhrDisabled,
		Bounce:               m.Bounce,
		Bounced:              m.Bounced,
		ImportFee:            m.ImportFee,
		BodyHash:             &bodyHashType,
		MessageContent:       &models.MessageContent{Hash: &bodyHashType, Body: new(models.BytesType(m.BodyBoc))},
		InitStateHash:        initStateHash,
		MsgHashNorm:          hashNorm,
	}
	if initStateBoc != nil {
		msg.InitState = &models.MessageContent{Hash: initStateHash, Body: new(models.BytesType(*initStateBoc))}
	}

	return msg, nil
}

func (n *TraceNode) ToMessages() ([]*models.Message, error) {
	msgs := make([]*models.Message, 0, len(n.Transaction.OutMsgs)+1)
	txHash := base64.StdEncoding.EncodeToString(n.Transaction.Hash[:])

	for _, outMsg := range n.Transaction.OutMsgs {
		msg, err := outMsg.toMessage(n.Key, "out", n.Transaction.Lt, txHash)
		if err != nil {
			return nil, err
		}
		msgs = append(msgs, msg)
	}

	if n.Transaction.InMsg != nil {
		inMsg, err := n.Transaction.InMsg.toMessage(n.Key, "in", n.Transaction.Lt, txHash)
		if err != nil {
			return nil, err
		}
		msgs = append(msgs, inMsg)
	}

	return msgs, nil
}
