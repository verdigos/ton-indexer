package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/gob"
	"encoding/json"
	"fmt"
	"log"
	"math"
	"os"
	"sort"
	"strconv"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/redis/go-redis/v9"
)

const scanQuery = `select m.mc_seqno, t.hash, t.lt, coalesce(m.created_lt, m.tx_lt), m.msg_hash, m.source, m.destination,
       coalesce(t.aborted, true), coalesce(t.compute_success, false), coalesce(m.bounced, false), m.body_hash
from messages m
join transactions t on t.hash = m.tx_hash and t.lt = m.tx_lt and t.mc_block_seqno = m.mc_seqno
where m.opcode = $1 and (coalesce(m.created_lt, m.tx_lt), m.msg_hash) > ($2, $3) and coalesce(m.created_lt, m.tx_lt) <= $4
  and m.mc_seqno between $5 and $6 and t.mc_block_seqno between $5 and $6 and m.direction = 'in'
order by coalesce(m.created_lt, m.tx_lt), m.msg_hash
limit $7`

const mcBlockLtQuery = `select start_lt, end_lt from blocks where workchain = -1 and seqno = $1 and mc_block_seqno = $1`

const (
	pageSize  = 2000
	pagePause = 100 * time.Millisecond
	// A queued message's created_lt can be far below its transaction's lt.
	ltMarginBelow = 100000 // mc blocks
	ltMarginAbove = 1000   // mc blocks
)

var dnsRoots = map[string]bool{
	"0:B774D95EB20543F186C06B371AB88AD704F7E256130CAF96189368A7D0CB6CCF": true, // .ton
	"0:80D78A35F955A14B679FAA887FF4CD5BFC0F43B4A4EEA2A7E6927F3701B273C2": true, // .t.me
}

type scanCursor struct {
	Lt      int64
	MsgHash string
	Done    bool
}

// Postgres tonhash rejects an empty hash, so start at lt-1 with the all-zero hash.
func startCursor(lt int64) scanCursor {
	return scanCursor{Lt: lt - 1, MsgHash: base64.StdEncoding.EncodeToString(make([]byte, 32))}
}

type bestWrite struct {
	Lt      int64
	McSeqno int32
	Edit    bool
	Writer  Address
	TxHash  string
}

type missingBody struct {
	Item   string `json:"nft_item_address"`
	Lt     int64  `json:"lt"`
	TxHash string `json:"tx_hash"`
}

type scanState struct {
	MinMcSeqno, MaxMcSeqno int32
	LtLo, LtHi             int64
	Cursors                map[string]scanCursor // "<db>/<opcode>"
	Best                   map[Address]bestWrite
	MissingBody            map[Address]missingBody // newest successful change_dns_record without a body in kvrocks
	IsDNS                  map[Address]bool
	Pages                  int64
}

type scanRow struct {
	McSeqno     int32
	TxHash      string
	Lt          int64
	KeyLt       int64
	MsgHash     string
	Source      *string
	Destination *string
	Aborted     bool
	Success     bool
	Bounced     bool
	BodyHash    *string
}

func loadScanState(path string, minMcSeqno, maxMcSeqno int32) (*scanState, error) {
	st := &scanState{MinMcSeqno: minMcSeqno, MaxMcSeqno: maxMcSeqno, Cursors: map[string]scanCursor{},
		Best: map[Address]bestWrite{}, MissingBody: map[Address]missingBody{}, IsDNS: map[Address]bool{}}
	if path == "" {
		return st, nil
	}
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return st, nil
	}
	if err != nil {
		return nil, err
	}
	var saved scanState
	if err := gob.NewDecoder(bytes.NewReader(data)).Decode(&saved); err != nil {
		return nil, fmt.Errorf("checkpoint %s: %w", path, err)
	}
	if saved.MinMcSeqno != minMcSeqno || saved.MaxMcSeqno != maxMcSeqno {
		return nil, fmt.Errorf("checkpoint %s is for mc seqnos %d..%d", path, saved.MinMcSeqno, saved.MaxMcSeqno)
	}
	log.Printf("resuming from %s: %d pages, %d writers", path, saved.Pages, len(saved.Best))
	return &saved, nil
}

func (st *scanState) save(path string) error {
	if path == "" {
		return nil
	}
	var buf bytes.Buffer
	if err := gob.NewEncoder(&buf).Encode(st); err != nil {
		return err
	}
	if err := os.WriteFile(path+".tmp", buf.Bytes(), 0o644); err != nil {
		return err
	}
	return os.Rename(path+".tmp", path)
}

// mcBlockLt returns zeros when no database has the block.
func mcBlockLt(ctx context.Context, conns []*pgx.Conn, names []string, seqno int32) (int64, int64, error) {
	for i, conn := range conns {
		plan, err := explainQuery(ctx, conn, "blocks", mcBlockLtQuery, seqno)
		fmt.Printf("=== EXPLAIN %s mc block lt\n%s\n", names[i], plan)
		if err != nil {
			return 0, 0, fmt.Errorf("%s: %w", names[i], err)
		}
		tx, err := conn.BeginTx(ctx, pgx.TxOptions{AccessMode: pgx.ReadOnly})
		if err != nil {
			return 0, 0, err
		}
		var start, end int64
		err = tx.QueryRow(ctx, mcBlockLtQuery, seqno).Scan(&start, &end)
		tx.Rollback(ctx)
		if err == nil {
			return start, end, nil
		}
		if err != pgx.ErrNoRows {
			return 0, 0, err
		}
	}
	return 0, 0, nil
}

func runScan(ctx context.Context, kv *sourceKV, dst *redis.Client, minMcSeqno, maxMcSeqno int32, checkpoint, out string) error {
	if maxMcSeqno <= 0 {
		maxMcSeqno = math.MaxInt32
	}
	dbNames := []string{"COLD_DB", "HOT_DB"}
	conns := make([]*pgx.Conn, 0, len(dbNames))
	for _, name := range dbNames {
		conn, err := openPG(ctx, name)
		if err != nil {
			return err
		}
		defer conn.Close(ctx)
		conns = append(conns, conn)
	}

	st, err := loadScanState(checkpoint, minMcSeqno, maxMcSeqno)
	if err != nil {
		return err
	}
	if st.LtHi == 0 {
		st.LtLo, st.LtHi = 0, math.MaxInt64
		if minMcSeqno > ltMarginBelow {
			if st.LtLo, _, err = mcBlockLt(ctx, conns, dbNames, minMcSeqno-ltMarginBelow); err != nil {
				return err
			}
		}
		if maxMcSeqno < math.MaxInt32-ltMarginAbove {
			_, end, err := mcBlockLt(ctx, conns, dbNames, maxMcSeqno+ltMarginAbove)
			if err != nil {
				return err
			}
			if end > 0 {
				st.LtHi = end
			}
		}
	}
	log.Printf("scan mc seqnos %d..%d, lt %d..%d", minMcSeqno, maxMcSeqno, st.LtLo, st.LtHi)

	lastSave := time.Now()
	for i, name := range dbNames {
		conn := conns[i]
		args := func(op int32, cur scanCursor) []any {
			return []any{op, cur.Lt, cur.MsgHash, st.LtHi, minMcSeqno, maxMcSeqno, pageSize}
		}
		for _, op := range []int32{opChangeDNSRecord, opEditContent} {
			key := name + "/" + strconv.Itoa(int(op))
			cur, ok := st.Cursors[key]
			if !ok {
				cur = startCursor(st.LtLo)
			}
			firstPage := true
			for !cur.Done {
				pageArgs := args(op, cur)
				plan, err := explainQuery(ctx, conn, "messages", scanQuery, pageArgs...)
				if firstPage || err != nil {
					fmt.Printf("=== EXPLAIN %s scan page\n%s\n", key, plan)
				}
				if err != nil {
					return fmt.Errorf("%s page after lt %d: %w", key, cur.Lt, err)
				}
				firstPage = false
				rows, err := queryScanPage(ctx, conn, pageArgs)
				if err != nil {
					return fmt.Errorf("%s page after lt %d: %w", key, cur.Lt, err)
				}
				if err := st.process(ctx, kv, op, rows); err != nil {
					return err
				}
				if len(rows) > 0 {
					last := rows[len(rows)-1]
					cur.Lt, cur.MsgHash = last.KeyLt, last.MsgHash
				}
				cur.Done = len(rows) < pageSize
				st.Cursors[key] = cur
				st.Pages++
				if st.Pages%50 == 0 || cur.Done {
					log.Printf("%s: lt %d, pages %d, writers %d", key, cur.Lt, st.Pages, len(st.Best))
				}
				if cur.Done || time.Since(lastSave) > time.Minute {
					if err := st.save(checkpoint); err != nil {
						return err
					}
					lastSave = time.Now()
				}
				time.Sleep(pagePause)
			}
		}
	}
	return st.writeAll(ctx, dst, out)
}

func queryScanPage(ctx context.Context, conn *pgx.Conn, args []any) ([]scanRow, error) {
	tx, err := conn.BeginTx(ctx, pgx.TxOptions{AccessMode: pgx.ReadOnly})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)
	rows, err := tx.Query(ctx, scanQuery, args...)
	if err != nil {
		return nil, err
	}
	return pgx.CollectRows(rows, pgx.RowToStructByPos[scanRow])
}

func (st *scanState) process(ctx context.Context, kv *sourceKV, op int32, rows []scanRow) error {
	type cand struct {
		row  scanRow
		item Address
	}
	var cands []cand
	var unknown []string
	seenUnknown := map[Address]bool{}
	for _, r := range rows {
		if r.Aborted || !r.Success || r.Bounced || r.Source == nil || r.Destination == nil {
			continue
		}
		item, err := ParseAddress(*r.Destination)
		if err != nil {
			continue
		}
		if _, known := st.IsDNS[item]; !known && !seenUnknown[item] {
			seenUnknown[item] = true
			unknown = append(unknown, item.String())
		}
		cands = append(cands, cand{r, item})
	}
	items, err := kv.mgetPayloads(ctx, "nft_items", unknown)
	if err != nil {
		return fmt.Errorf("mget nft_items: %w", err)
	}
	for _, id := range unknown {
		item, _ := ParseAddress(id)
		var row struct {
			CollectionAddress string `json:"collection_address"`
		}
		if p, ok := items[id]; ok {
			if err := json.Unmarshal([]byte(p), &row); err != nil {
				return fmt.Errorf("decode nft_items %s: %w", id, err)
			}
		}
		coll, err := ParseAddress(row.CollectionAddress)
		st.IsDNS[item] = err == nil && dnsRoots[coll.String()]
	}

	var dns []cand
	var bodyHashes []string
	for _, c := range cands {
		if !st.IsDNS[c.item] {
			continue
		}
		dns = append(dns, c)
		if op == opChangeDNSRecord && c.row.BodyHash != nil {
			bodyHashes = append(bodyHashes, *c.row.BodyHash)
		}
	}
	bodies, err := kv.mgetPayloads(ctx, "message_contents", bodyHashes)
	if err != nil {
		return fmt.Errorf("mget message_contents: %w", err)
	}
	for _, c := range dns {
		if b, ok := st.Best[c.item]; ok && b.Lt >= c.row.Lt {
			continue
		}
		if op == opChangeDNSRecord {
			p, ok := "", false
			if c.row.BodyHash != nil {
				p, ok = bodies[*c.row.BodyHash]
			}
			if !ok {
				if m := st.MissingBody[c.item]; m.Lt < c.row.Lt {
					st.MissingBody[c.item] = missingBody{Item: c.item.String(), Lt: c.row.Lt, TxHash: c.row.TxHash}
				}
				continue
			}
			var mc struct {
				Body string `json:"body"`
			}
			if err := json.Unmarshal([]byte(p), &mc); err != nil {
				return fmt.Errorf("decode message_contents %s: %w", *c.row.BodyHash, err)
			}
			isWallet, err := changesWalletKey(mc.Body)
			if err != nil || !isWallet {
				continue
			}
		}
		writer, err := ParseAddress(*c.row.Source)
		if err != nil {
			continue
		}
		st.Best[c.item] = bestWrite{Lt: c.row.Lt, McSeqno: c.row.McSeqno, Edit: op == opEditContent, Writer: writer, TxHash: c.row.TxHash}
	}
	return nil
}

func (b bestWrite) payload(item Address) writerPayload {
	op := "change_dns_record"
	if b.Edit {
		op = "edit_content"
	}
	return writerPayload{SchemaVersion: 1, SourceMcSeqno: b.McSeqno, NftItemAddress: item.String(), Writer: b.Writer.String(),
		WriteLt: strconv.FormatInt(b.Lt, 10), WriteTxHash: b.TxHash, WriteOp: op}
}

func writeJSONLines[T any](path string, rows []T) error {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	for _, r := range rows {
		if err := enc.Encode(r); err != nil {
			return err
		}
	}
	return os.WriteFile(path, buf.Bytes(), 0o644)
}

// An item whose newest change_dns_record has no body gets no key: that write may be a later wallet write.
func (st *scanState) writeAll(ctx context.Context, dst *redis.Client, out string) error {
	var payloads []writerPayload
	var missing []missingBody
	for item, m := range st.MissingBody {
		if b, ok := st.Best[item]; !ok || m.Lt > b.Lt {
			missing = append(missing, m)
		}
	}
	for item, b := range st.Best {
		if m, ok := st.MissingBody[item]; !ok || m.Lt < b.Lt {
			payloads = append(payloads, b.payload(item))
		}
	}
	sort.Slice(payloads, func(i, j int) bool { return payloads[i].NftItemAddress < payloads[j].NftItemAddress })
	sort.Slice(missing, func(i, j int) bool { return missing[i].Item < missing[j].Item })
	if out != "" {
		if err := writeJSONLines(out, payloads); err != nil {
			return err
		}
		if err := writeJSONLines(out+".missing", missing); err != nil {
			return err
		}
	}

	applied := 0
	if dst != nil {
		if err := setCurrentScript.Load(ctx, dst).Err(); err != nil {
			return err
		}
		for start := 0; start < len(payloads); start += kvBatch {
			pipe := dst.Pipeline()
			var cmds []*redis.Cmd
			for _, w := range payloads[start:min(start+kvBatch, len(payloads))] {
				data, err := json.Marshal(w)
				if err != nil {
					return err
				}
				keys := []string{rowKey("dns_wallet_writers", w.NftItemAddress), payloadKey("dns_wallet_writers", w.NftItemAddress)}
				cmds = append(cmds, setCurrentScript.EvalSha(ctx, pipe, keys, w.SourceMcSeqno, string(data)))
			}
			if _, err := pipe.Exec(ctx); err != nil {
				return err
			}
			for _, c := range cmds {
				if n, _ := c.Int(); n == 1 {
					applied++
				}
			}
			time.Sleep(kvPause)
		}
	}
	log.Printf("writers %d, applied %d, target written %v, missing body %d",
		len(payloads), applied, dst != nil, len(missing))
	return nil
}
