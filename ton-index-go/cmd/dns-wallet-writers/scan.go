package main

// Scan mode: walk every in-message with op change_dns_record or edit_content in hot and cold
// Postgres, keep the max-lt successful wallet write per .ton/.t.me item, then write all writers
// through the worker's set_current script.
//
// Paging is keyset on the (opcode, coalesce(created_lt, tx_lt), msg_hash) index, one opcode at a
// time. mc seqno chunks would not work on cold Postgres: it has a single 6e9-row partition and no
// mc_seqno index, so every chunk would rescan the opcode's whole index range. The mc seqno bounds
// are applied as filters (they prune the hot partitions) and turned into a generous lt range.

import (
	"bufio"
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

// Shard blocks reference a recent masterchain block and have a higher lt, and messages arrive
// within a few blocks; the lt range is widened by this many mc blocks on both sides.
const ltMarginBlocks = 1000

var dnsRoots = []string{
	"0:B774D95EB20543F186C06B371AB88AD704F7E256130CAF96189368A7D0CB6CCF", // .ton
	"0:80D78A35F955A14B679FAA887FF4CD5BFC0F43B4A4EEA2A7E6927F3701B273C2", // .t.me
}

type scanOptions struct {
	minMcSeqno, maxMcSeqno int32
	checkpoint             string
	pageSize               int
	pause                  time.Duration
	explainOnly            bool
	out                    string
}

// startCursor sorts before every message with key lt >= lt. msg_hash cannot be empty (tonhash rejects
// it), so the start uses lt-1 and the all-zero hash.
func startCursor(lt int64) scanCursor {
	return scanCursor{Lt: lt - 1, MsgHash: base64.StdEncoding.EncodeToString(make([]byte, 32))}
}

type scanCursor struct {
	Lt      int64
	MsgHash string
	Done    bool
}

type bestWrite struct {
	Lt      int64
	McSeqno int32
	Edit    bool
	Writer  Address
	TxHash  [32]byte
}

// scanState is everything a resumed run needs; it is saved with encoding/gob.
type scanState struct {
	MinMcSeqno, MaxMcSeqno int32
	LtLo, LtHi             int64
	Cursors                map[string]scanCursor // "<db>/<opcode>"
	Best                   map[Address]bestWrite
	MissingBody            map[Address]int64 // highest lt of a successful change_dns_record whose body was not found
	IsDNS                  map[Address]bool
	Rows, DNSRows, Pages   int64
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

func loadScanState(path string, opts scanOptions) (*scanState, error) {
	st := &scanState{MinMcSeqno: opts.minMcSeqno, MaxMcSeqno: opts.maxMcSeqno, Cursors: map[string]scanCursor{},
		Best: map[Address]bestWrite{}, MissingBody: map[Address]int64{}, IsDNS: map[Address]bool{}}
	if path == "" {
		return st, nil
	}
	f, err := os.Open(path)
	if os.IsNotExist(err) {
		return st, nil
	}
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var saved scanState
	if err := gob.NewDecoder(bufio.NewReader(f)).Decode(&saved); err != nil {
		return nil, fmt.Errorf("checkpoint %s: %w", path, err)
	}
	if saved.MinMcSeqno != opts.minMcSeqno || saved.MaxMcSeqno != opts.maxMcSeqno {
		return nil, fmt.Errorf("checkpoint %s is for mc seqnos %d..%d", path, saved.MinMcSeqno, saved.MaxMcSeqno)
	}
	log.Printf("resuming from %s: %d pages, %d rows, %d writers", path, saved.Pages, saved.Rows, len(saved.Best))
	return &saved, nil
}

func (st *scanState) save(path string) error {
	if path == "" {
		return nil
	}
	f, err := os.Create(path + ".tmp")
	if err != nil {
		return err
	}
	w := bufio.NewWriter(f)
	if err := gob.NewEncoder(w).Encode(st); err != nil {
		f.Close()
		return err
	}
	if err := w.Flush(); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return os.Rename(path+".tmp", path)
}

// mcBlockLt returns start_lt and end_lt of a masterchain block from the first database that has it.
func mcBlockLt(ctx context.Context, conns []*pgx.Conn, names []string, seqno int32) (int64, int64, bool, error) {
	for i, conn := range conns {
		plan, err := explainQuery(ctx, conn, "Index", mcBlockLtQuery, seqno)
		fmt.Printf("=== EXPLAIN %s mc block lt\n%s\n", names[i], plan)
		if err != nil {
			return 0, 0, false, fmt.Errorf("%s: %w; not running queries", names[i], err)
		}
		tx, err := conn.BeginTx(ctx, pgx.TxOptions{AccessMode: pgx.ReadOnly})
		if err != nil {
			return 0, 0, false, err
		}
		var start, end int64
		err = tx.QueryRow(ctx, mcBlockLtQuery, seqno).Scan(&start, &end)
		tx.Rollback(ctx)
		if err == nil {
			return start, end, true, nil
		}
		if err != pgx.ErrNoRows {
			return 0, 0, false, err
		}
	}
	return 0, 0, false, nil
}

func runScan(ctx context.Context, kv *prodKV, local *localRedis, opts scanOptions) error {
	if opts.maxMcSeqno <= 0 {
		opts.maxMcSeqno = math.MaxInt32
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

	st, err := loadScanState(opts.checkpoint, opts)
	if err != nil {
		return err
	}
	if st.LtHi == 0 {
		st.LtLo, st.LtHi = 0, math.MaxInt64
		if opts.minMcSeqno > ltMarginBlocks {
			start, _, ok, err := mcBlockLt(ctx, conns, dbNames, opts.minMcSeqno-ltMarginBlocks)
			if err != nil {
				return err
			}
			if ok {
				st.LtLo = start
			}
		}
		if opts.maxMcSeqno < math.MaxInt32-ltMarginBlocks {
			_, end, ok, err := mcBlockLt(ctx, conns, dbNames, opts.maxMcSeqno+ltMarginBlocks)
			if err != nil {
				return err
			}
			if ok {
				st.LtHi = end
			}
		}
	}
	log.Printf("scan mc seqnos %d..%d, lt %d..%d", st.MinMcSeqno, opts.maxMcSeqno, st.LtLo, st.LtHi)

	roots := map[Address]bool{}
	for _, r := range dnsRoots {
		a, _ := ParseAddress(r)
		roots[a] = true
	}

	lastSave := time.Now()
	for i, name := range dbNames {
		conn := conns[i]
		args := func(op int32, cur scanCursor) []any {
			return []any{op, cur.Lt, cur.MsgHash, st.LtHi, opts.minMcSeqno, opts.maxMcSeqno, opts.pageSize}
		}
		plan, err := explainQuery(ctx, conn, "opcode_coalesce", scanQuery, args(opChangeDNSRecord, startCursor(st.LtLo))...)
		fmt.Printf("=== EXPLAIN %s scan page\n%s\n", name, plan)
		if err != nil {
			return fmt.Errorf("%s: %w; not running queries", name, err)
		}
		if opts.explainOnly {
			continue
		}
		for _, op := range []int32{opChangeDNSRecord, opEditContent} {
			key := name + "/" + strconv.Itoa(int(op))
			cur, ok := st.Cursors[key]
			if !ok {
				cur = startCursor(st.LtLo)
			}
			for !cur.Done {
				rows, err := queryScanPage(ctx, conn, args(op, cur))
				if err != nil {
					return fmt.Errorf("%s page after lt %d: %w", key, cur.Lt, err)
				}
				if err := st.process(ctx, kv, roots, op, rows); err != nil {
					return err
				}
				if len(rows) > 0 {
					last := rows[len(rows)-1]
					cur.Lt, cur.MsgHash = last.KeyLt, last.MsgHash
				}
				cur.Done = len(rows) < opts.pageSize
				st.Cursors[key] = cur
				st.Pages++
				if st.Pages%50 == 0 || cur.Done {
					log.Printf("%s: lt %d, pages %d, rows %d, dns rows %d, items %d, writers %d", key, cur.Lt, st.Pages, st.Rows, st.DNSRows, len(st.IsDNS), len(st.Best))
				}
				if cur.Done || time.Since(lastSave) > time.Minute {
					if err := st.save(opts.checkpoint); err != nil {
						return err
					}
					lastSave = time.Now()
				}
				time.Sleep(opts.pause)
			}
		}
	}
	if opts.explainOnly {
		return nil
	}
	return st.writeAll(ctx, local, opts.out)
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
	defer rows.Close()
	var res []scanRow
	for rows.Next() {
		var r scanRow
		if err := rows.Scan(&r.McSeqno, &r.TxHash, &r.Lt, &r.KeyLt, &r.MsgHash, &r.Source, &r.Destination,
			&r.Aborted, &r.Success, &r.Bounced, &r.BodyHash); err != nil {
			return nil, err
		}
		res = append(res, r)
	}
	return res, rows.Err()
}

// process classifies new destinations, decodes change_dns_record bodies and updates Best.
func (st *scanState) process(ctx context.Context, kv *prodKV, roots map[Address]bool, op int32, rows []scanRow) error {
	type cand struct {
		row  scanRow
		item Address
	}
	var cands []cand
	var unknown []string
	seenUnknown := map[Address]bool{}
	for _, r := range rows {
		st.Rows++
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
	if len(unknown) > 0 {
		payloads, err := kv.mgetPayloads(ctx, "nft_items", unknown)
		if err != nil {
			return fmt.Errorf("mget nft_items: %w", err)
		}
		for _, id := range unknown {
			item, _ := ParseAddress(id)
			var row struct {
				CollectionAddress *string `json:"collection_address"`
			}
			if p, ok := payloads[id]; ok {
				if err := json.Unmarshal([]byte(p), &row); err != nil {
					return fmt.Errorf("decode nft_items %s: %w", id, err)
				}
			}
			coll := parseOptAddress(row.CollectionAddress)
			st.IsDNS[item] = coll != nil && roots[*coll]
		}
	}

	var dns []cand
	var bodyHashes []string
	for _, c := range cands {
		if !st.IsDNS[c.item] {
			continue
		}
		st.DNSRows++
		if b, ok := st.Best[c.item]; ok && b.Lt >= c.row.Lt {
			continue
		}
		dns = append(dns, c)
		if op == opChangeDNSRecord && c.row.BodyHash != nil {
			bodyHashes = append(bodyHashes, *c.row.BodyHash)
		}
	}
	var bodies map[string]string
	if len(bodyHashes) > 0 {
		var err error
		if bodies, err = kv.mgetPayloads(ctx, "message_contents", bodyHashes); err != nil {
			return fmt.Errorf("mget message_contents: %w", err)
		}
	}
	for _, c := range dns {
		if op == opChangeDNSRecord {
			p, ok := "", false
			if c.row.BodyHash != nil {
				p, ok = bodies[*c.row.BodyHash]
			}
			if !ok {
				st.MissingBody[c.item] = max(st.MissingBody[c.item], c.row.Lt)
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
		hash, err := base64.StdEncoding.DecodeString(c.row.TxHash)
		if err != nil || len(hash) != 32 {
			return fmt.Errorf("tx hash %q: bad base64", c.row.TxHash)
		}
		if b, ok := st.Best[c.item]; ok && b.Lt >= c.row.Lt {
			continue
		}
		bw := bestWrite{Lt: c.row.Lt, McSeqno: c.row.McSeqno, Edit: op == opEditContent, Writer: writer}
		copy(bw.TxHash[:], hash)
		st.Best[c.item] = bw
	}
	return nil
}

func (b bestWrite) payload(item Address) writerPayload {
	op := "change_dns_record"
	if b.Edit {
		op = "edit_content"
	}
	return writerPayload{SchemaVersion: 1, SourceMcSeqno: b.McSeqno, NftItemAddress: item.String(), Writer: b.Writer.String(),
		WriteLt: strconv.FormatInt(b.Lt, 10), WriteTxHash: base64.StdEncoding.EncodeToString(b.TxHash[:]), WriteOp: op}
}

// writeAll writes every writer through set_current in pipelines of 100 (skipped when local is nil)
// and, with out set, one JSON line per writer.
func (st *scanState) writeAll(ctx context.Context, local *localRedis, out string) error {
	items := make([]Address, 0, len(st.Best))
	skipped := 0
	for item, b := range st.Best {
		if st.MissingBody[item] > b.Lt {
			skipped++ // a later change_dns_record has no body: it may be a later wallet write
			continue
		}
		items = append(items, item)
	}
	sort.Slice(items, func(i, j int) bool { return items[i].String() < items[j].String() })

	var outFile *bufio.Writer
	if out != "" {
		f, err := os.Create(out)
		if err != nil {
			return err
		}
		defer f.Close()
		outFile = bufio.NewWriter(f)
		defer outFile.Flush()
	}
	if local != nil {
		if err := setCurrentScript.Load(ctx, local.c).Err(); err != nil {
			return err
		}
	}
	applied := 0
	for start := 0; start < len(items); start += kvBatch {
		end := min(start+kvBatch, len(items))
		var pipe redis.Pipeliner
		var cmds []*redis.Cmd
		if local != nil {
			pipe = local.c.Pipeline()
		}
		for _, item := range items[start:end] {
			w := st.Best[item].payload(item)
			data, err := json.Marshal(w)
			if err != nil {
				return err
			}
			if outFile != nil {
				fmt.Fprintf(outFile, "%s\n", data)
			}
			if pipe != nil {
				keys := []string{rowKey("dns_wallet_writers", w.NftItemAddress), payloadKey("dns_wallet_writers", w.NftItemAddress)}
				cmds = append(cmds, setCurrentScript.EvalSha(ctx, pipe, keys, w.SourceMcSeqno, string(data)))
			}
		}
		if pipe != nil {
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
	log.Printf("writers: %d (applied %d, skipped for a missing body %d, written to target: %v); rows %d, dns rows %d, dns items %d of %d destinations",
		len(items), applied, skipped, local != nil, st.Rows, st.DNSRows, countTrue(st.IsDNS), len(st.IsDNS))
	return nil
}

func countTrue(m map[Address]bool) int {
	n := 0
	for _, v := range m {
		if v {
			n++
		}
	}
	return n
}
