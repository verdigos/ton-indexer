// Package trace loads an .lz4 trace fixture (LZ4-frame-compressed msgpack) into
// the minimal record set the matcher engine needs: transactions, their in/out
// messages, opcodes and the parent linkage encoded in message hashes.
//
// The fixture format is the one produced by the Python indexer's
// tests/utils/trace_deserializer.py. Only the fields used for *structural*
// matching are decoded; msgpack silently ignores the rest.
package trace

import (
	"fmt"
	"os"

	"github.com/pierrec/lz4/v4"
	"github.com/vmihailenco/msgpack/v5"
)

// MsgContent is the parsed body cell of a message. body is a base64 BOC string;
// the POC never parses it (opcodes come from Message.Opcode), so it is kept only
// for completeness / debugging.
type MsgContent struct {
	Hash string `msgpack:"hash"`
	Body string `msgpack:"body"`
}

// Message mirrors the indexer Message model, trimmed to the fields the engine
// reads. Nullable columns are pointers so that a msgpack nil round-trips as nil.
type Message struct {
	MsgHash     string      `msgpack:"msg_hash"`
	TxHash      string      `msgpack:"tx_hash"`
	TxLt        int64       `msgpack:"tx_lt"`
	Direction   string      `msgpack:"direction"` // "in" | "out"
	Source      *string     `msgpack:"source"`
	Destination *string     `msgpack:"destination"`
	Opcode      *int64      `msgpack:"opcode"` // stored SIGNED (int32); mask &0xFFFFFFFF
	CreatedLt   *int64      `msgpack:"created_lt"`
	CreatedAt   *int64      `msgpack:"created_at"`
	Bounced     bool        `msgpack:"bounced"`
	Content     *MsgContent `msgpack:"message_content"`

	// Tx is wired up after decode so a message can reach its owning transaction
	// the way the Python Message.transaction relationship does.
	Tx *Transaction `msgpack:"-"`
}

// Transaction mirrors the indexer Transaction model, trimmed to what the engine
// reads.
type Transaction struct {
	Hash       string     `msgpack:"hash"`
	Lt         int64      `msgpack:"lt"`
	Now        int64      `msgpack:"now"`
	Account    string     `msgpack:"account"`
	Descr      string     `msgpack:"descr"` // "ord" | "tick_tock" | ...
	OrigStatus string     `msgpack:"orig_status"`
	EndStatus  string     `msgpack:"end_status"`
	Aborted    bool       `msgpack:"aborted"`
	Messages   []*Message `msgpack:"messages"`
}

// Trace is the loaded fixture.
type Trace struct {
	TraceID      string
	Transactions []*Transaction
}

type rawTrace struct {
	Trace struct {
		TraceID string `msgpack:"trace_id"`
	} `msgpack:"trace"`
	Transactions []*Transaction `msgpack:"transactions"`
}

// Load reads and decompresses an .lz4 fixture and wires the transaction
// back-references onto every message.
func Load(path string) (*Trace, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	var out rawTrace
	dec := msgpack.NewDecoder(lz4.NewReader(f))
	if err := dec.Decode(&out); err != nil {
		return nil, fmt.Errorf("decode msgpack: %w", err)
	}

	for _, tx := range out.Transactions {
		for _, m := range tx.Messages {
			m.Tx = tx
		}
	}
	return &Trace{TraceID: out.Trace.TraceID, Transactions: out.Transactions}, nil
}

// Opcode returns the message opcode masked to 32 bits (matching the Python
// EventNode.get_opcode), plus whether an opcode was present at all.
func (m *Message) Opcode32() (uint32, bool) {
	if m.Opcode == nil {
		return 0, false
	}
	return uint32(*m.Opcode & 0xFFFFFFFF), true
}
