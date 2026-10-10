// dns-wallet-writers records, for DNS NFT items, who last wrote the item's `wallet` record.
//
// A write is the in-message of a successful transaction on the item (not aborted, compute phase
// successful, message not bounced) with op change_dns_record and key sha256("wallet") (set or
// delete), or op edit_content (.ton only; telemint rejects it). The writer is the in-message
// source; the write with the highest lt wins.
//
// Reads go to production Postgres (hot and cold, read-only sessions) and production kvrocks
// (GET/MGET/ZCARD/ZRANGEBYLEX only, paced). Writes go only to a local Redis on 127.0.0.1/localhost:
// the same keys the worker writes (hash ton-index:v1:dns_wallet_writers:<item> with field
// source_mc_seqno, string <key>:payload), through the worker's set_current script, so the backfill
// and the live worker can run in any order and the higher source_mc_seqno wins.
package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/redis/go-redis/v9"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

const (
	keyPrefix         = "ton-index:v1"
	opChangeDNSRecord = 0x4eb1f0f9
	opEditContent     = 0x1a0b9d51
	kvBatch           = 100
	kvPause           = 200 * time.Millisecond
)

var walletKey = sha256.Sum256([]byte("wallet"))

const writesQuery = `select t.hash, t.lt, t.mc_block_seqno, coalesce(t.aborted, true), coalesce(t.compute_success, false),
       m.source, m.opcode, coalesce(m.bounced, false), m.body_hash
from messages m
join transactions t on t.hash = m.tx_hash and t.lt = m.tx_lt and t.mc_block_seqno = m.mc_seqno
where m.destination = $1 and m.opcode in ($2, $3) and m.direction = 'in'`

// Address is a parsed std address; String gives the raw form used by kvrocks payloads (0:UPPERHEX).
type Address struct {
	Workchain int32
	Hash      [32]byte
}

func ParseAddress(raw string) (Address, error) {
	var a Address
	wc, hexPart, ok := strings.Cut(strings.TrimSpace(raw), ":")
	if !ok {
		return a, fmt.Errorf("not a raw address: %q", raw)
	}
	w, err := strconv.ParseInt(wc, 10, 32)
	if err != nil {
		return a, fmt.Errorf("bad workchain in %q: %w", raw, err)
	}
	b, err := hex.DecodeString(hexPart)
	if err != nil || len(b) != 32 {
		return a, fmt.Errorf("bad hash in %q", raw)
	}
	a.Workchain = int32(w)
	copy(a.Hash[:], b)
	return a, nil
}

func (a Address) String() string {
	return fmt.Sprintf("%d:%X", a.Workchain, a.Hash[:])
}

func parseOptAddress(raw *string) *Address {
	if raw == nil || *raw == "" {
		return nil
	}
	a, err := ParseAddress(*raw)
	if err != nil {
		return nil
	}
	return &a
}

// prodKV is the production kvrocks reader: one connection, read commands only, paced batches.
type prodKV struct {
	c *redis.Client
}

func newProdKV() (*prodKV, error) {
	host, port := os.Getenv("KV_ROCKS_HOST"), os.Getenv("KV_ROCKS_PORT")
	if host == "" || port == "" {
		return nil, fmt.Errorf("KV_ROCKS_HOST and KV_ROCKS_PORT must be set")
	}
	c := redis.NewClient(&redis.Options{
		Addr:         net.JoinHostPort(host, port),
		Password:     os.Getenv("KV_ROCKS_PASSWORD"),
		PoolSize:     1,
		MinIdleConns: 0,
		ReadTimeout:  30 * time.Second,
	})
	return &prodKV{c: c}, nil
}

func (k *prodKV) zcard(ctx context.Context, key string) (int64, error) {
	time.Sleep(kvPause)
	return k.c.ZCard(ctx, key).Result()
}

func (k *prodKV) zrangeAllByLex(ctx context.Context, key string) ([]string, error) {
	n, err := k.zcard(ctx, key)
	if err != nil {
		return nil, err
	}
	res := make([]string, 0, n)
	for off := int64(0); off < n; off += kvBatch {
		time.Sleep(kvPause)
		page, err := k.c.ZRangeByLex(ctx, key, &redis.ZRangeBy{Min: "-", Max: "+", Offset: off, Count: kvBatch}).Result()
		if err != nil {
			return nil, err
		}
		res = append(res, page...)
		if len(page) < kvBatch {
			break
		}
	}
	return res, nil
}

// mgetPayloads returns id -> payload for the ids found under <table>:<id>:payload.
func (k *prodKV) mgetPayloads(ctx context.Context, table string, ids []string) (map[string]string, error) {
	res := map[string]string{}
	for start := 0; start < len(ids); start += kvBatch {
		end := min(start+kvBatch, len(ids))
		keys := make([]string, 0, end-start)
		for _, id := range ids[start:end] {
			keys = append(keys, payloadKey(table, id))
		}
		time.Sleep(kvPause)
		values, err := k.c.MGet(ctx, keys...).Result()
		if err != nil {
			return nil, err
		}
		for i, v := range values {
			if s, ok := v.(string); ok {
				res[ids[start+i]] = s
			}
		}
	}
	return res, nil
}

// localRedis is the only write target; it must be on the loopback host.
type localRedis struct {
	c *redis.Client
}

func newLocalRedis(addr string, db int) (*localRedis, error) {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		return nil, err
	}
	if host != "127.0.0.1" && host != "localhost" {
		return nil, fmt.Errorf("refusing to write to %q: only 127.0.0.1 or localhost", host)
	}
	c := redis.NewClient(&redis.Options{Addr: addr, DB: db})
	if err := c.Ping(context.Background()).Err(); err != nil {
		return nil, err
	}
	return &localRedis{c: c}, nil
}

// Same text and KEYS/ARGV layout as the worker's kvrocks_set_current_script (KvrocksState.cpp):
// KEYS = {<table>:<id>, <table>:<id>:payload}, ARGV = {source_mc_seqno, payload}. A write with a
// lower source_mc_seqno than the stored one is ignored; an equal or higher one replaces it.
var setCurrentScript = redis.NewScript(`
local current = redis.call('HGET', KEYS[1], 'source_mc_seqno')
if (not current) or tonumber(current) <= tonumber(ARGV[1]) then
  redis.call('HSET', KEYS[1], 'source_mc_seqno', ARGV[1])
  redis.call('SET', KEYS[2], ARGV[2])
  return 1
end
return 0
`)

func rowKey(table, id string) string {
	return keyPrefix + ":" + table + ":" + id
}

func payloadKey(table, id string) string {
	return rowKey(table, id) + ":payload"
}

func indexKey(table, name string) string {
	return keyPrefix + ":idx:" + table + ":" + name
}

// itemFromLexMember takes the trailing raw address of a `<len>:<domain>:<item id>` member.
func itemFromLexMember(member string) (string, bool) {
	if len(member) < 66 || member[len(member)-65] != ':' {
		return "", false
	}
	i := strings.LastIndexByte(member[:len(member)-65], ':')
	return member[i+1:], true
}

type dnsEntry struct {
	NftItemAddress string  `json:"nft_item_address"`
	NftItemOwner   *string `json:"nft_item_owner"`
	Domain         string  `json:"domain"`
	DNSWallet      *string `json:"dns_wallet"`
	Destroyed      bool    `json:"destroyed"`
}

type writeRow struct {
	TxHash         string
	Lt             int64
	McSeqno        int32
	Aborted        bool
	ComputeSuccess bool
	Source         *string
	Opcode         int32
	Bounced        bool
	BodyHash       *string
	DB             string
}

type writerPayload struct {
	SchemaVersion  int    `json:"schema_version"`
	SourceMcSeqno  int32  `json:"source_mc_seqno"`
	NftItemAddress string `json:"nft_item_address"`
	Writer         string `json:"writer"`
	WriteLt        string `json:"write_lt"`
	WriteTxHash    string `json:"write_tx_hash"`
	WriteOp        string `json:"write_op"`
}

type itemResult struct {
	Item          string         `json:"item"`
	Domain        string         `json:"domain"`
	Owner         *string        `json:"owner"`
	DNSWallet     *string        `json:"dns_wallet"`
	Candidates    int            `json:"candidates"`
	Successful    int            `json:"successful_writes"`
	Write         *writerPayload `json:"write"`
	CurrentRule   bool           `json:"current_rule_shows"`
	NewRule       bool           `json:"new_rule_shows"`
	FromSubject   bool           `json:"from_subject_index"`
	Stored        string         `json:"stored,omitempty"`
	MissingBodies int            `json:"missing_bodies,omitempty"`
}

func openPG(ctx context.Context, envName string) (*pgx.Conn, error) {
	dsn := os.Getenv(envName)
	if dsn == "" {
		return nil, fmt.Errorf("%s is not set", envName)
	}
	cfg, err := pgx.ParseConfig(dsn)
	if err != nil {
		return nil, fmt.Errorf("parse %s: %w", envName, err)
	}
	cfg.RuntimeParams["default_transaction_read_only"] = "on"
	cfg.RuntimeParams["statement_timeout"] = "30s"
	cfg.RuntimeParams["jit"] = "off"
	// Unnamed statements: every query is planned with its parameter values, like EXPLAIN.
	cfg.DefaultQueryExecMode = pgx.QueryExecModeExec
	conn, err := pgx.ConnectConfig(ctx, cfg)
	if err != nil {
		return nil, fmt.Errorf("connect %s: %w", envName, err)
	}
	return conn, nil
}

// explain returns the plan of writesQuery for one item and fails on a sequential scan.
func explain(ctx context.Context, conn *pgx.Conn, item string) (string, error) {
	tx, err := conn.BeginTx(ctx, pgx.TxOptions{AccessMode: pgx.ReadOnly})
	if err != nil {
		return "", err
	}
	defer tx.Rollback(ctx)
	rows, err := tx.Query(ctx, "explain "+writesQuery, item, int32(opChangeDNSRecord), int32(opEditContent))
	if err != nil {
		return "", err
	}
	var lines []string
	for rows.Next() {
		var line string
		if err := rows.Scan(&line); err != nil {
			rows.Close()
			return "", err
		}
		lines = append(lines, line)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return "", err
	}
	plan := strings.Join(lines, "\n")
	// The only sequential scans allowed are on default partitions that are empty.
	var sizes []string
	for _, line := range lines {
		_, rest, ok := strings.Cut(line, "Seq Scan on ")
		if !ok {
			continue
		}
		rel, _, _ := strings.Cut(rest, " ")
		if !strings.HasSuffix(rel, "_default") {
			return plan, fmt.Errorf("plan has a sequential scan on %s", rel)
		}
		var pages int64
		var tuples float64
		if err := tx.QueryRow(ctx, "select relpages::bigint, reltuples::float8 from pg_class where relname = $1", rel).Scan(&pages, &tuples); err != nil {
			return plan, fmt.Errorf("pg_class %s: %w", rel, err)
		}
		if pages > 0 || tuples > 0 {
			return plan, fmt.Errorf("plan has a sequential scan on non-empty %s: relpages %d, reltuples %.0f", rel, pages, tuples)
		}
		sizes = append(sizes, fmt.Sprintf("seq scan on %s: relpages=%d reltuples=%.0f", rel, pages, tuples))
	}
	if len(sizes) > 0 {
		plan += "\n-- " + strings.Join(sizes, "; ")
	}
	if !strings.Contains(plan, "destination_opcode") {
		return plan, fmt.Errorf("plan does not use the (destination, opcode) index")
	}
	return plan, nil
}

func queryWrites(ctx context.Context, conn *pgx.Conn, dbName string, item string) ([]writeRow, error) {
	tx, err := conn.BeginTx(ctx, pgx.TxOptions{AccessMode: pgx.ReadOnly})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)
	rows, err := tx.Query(ctx, writesQuery, item, int32(opChangeDNSRecord), int32(opEditContent))
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var res []writeRow
	for rows.Next() {
		r := writeRow{DB: dbName}
		if err := rows.Scan(&r.TxHash, &r.Lt, &r.McSeqno, &r.Aborted, &r.ComputeSuccess, &r.Source, &r.Opcode, &r.Bounced, &r.BodyHash); err != nil {
			return nil, err
		}
		res = append(res, r)
	}
	return res, rows.Err()
}

// changesWalletKey reports whether a change_dns_record body targets key sha256("wallet").
func changesWalletKey(bodyB64 string) (bool, error) {
	boc, err := base64.StdEncoding.DecodeString(bodyB64)
	if err != nil {
		return false, err
	}
	c, err := cell.FromBOC(boc)
	if err != nil {
		return false, err
	}
	s := c.BeginParse()
	op, err := s.LoadUInt(32)
	if err != nil || op != opChangeDNSRecord {
		return false, err
	}
	if _, err := s.LoadUInt(64); err != nil {
		return false, err
	}
	key, err := s.LoadSlice(256)
	if err != nil {
		return false, err
	}
	return bytes.Equal(key, walletKey[:]), nil
}

func readLines(path string) ([]string, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var res []string
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if line := strings.TrimSpace(sc.Text()); line != "" && !strings.HasPrefix(line, "#") {
			res = append(res, line)
		}
	}
	return res, sc.Err()
}

func sameAddr(a, b *Address) bool {
	return a != nil && b != nil && *a == *b
}

func main() {
	subjectFlag := flag.String("subject", "", "raw address whose owner_wallet and wallet dns index members form the item set")
	extraFlag := flag.String("extra-items", "", "file with extra item ids (raw addresses), one per line")
	localFlag := flag.String("local", "127.0.0.1:16379", "local Redis to write to (127.0.0.1 or localhost only)")
	localDB := flag.Int("local-db", 0, "local Redis db number")
	maxMcSeqno := flag.Int("max-mc-seqno", 0, "ignore writes with a higher mc seqno (0: no limit), to compare with an indexer run that ends there")
	explainOnly := flag.Bool("explain-only", false, "print EXPLAIN of the writes query for the first item on each database and exit")
	copySubject := flag.Bool("copy-subject", false, "copy the subject's dns_entries rows, index members and address-book state to local Redis")
	outFlag := flag.String("out", "", "write per-item results as JSON to this file")
	flag.Parse()

	ctx := context.Background()
	log.SetFlags(log.LstdFlags)

	var subject *Address
	if *subjectFlag != "" {
		a, err := ParseAddress(*subjectFlag)
		if err != nil {
			log.Fatal(err)
		}
		subject = &a
	}

	// The write target is checked before any production read.
	var local *localRedis
	if !*explainOnly {
		l, err := newLocalRedis(*localFlag, *localDB)
		if err != nil {
			log.Fatal(err)
		}
		local = l
		defer local.c.Close()
	}

	kv, err := newProdKV()
	if err != nil {
		log.Fatal(err)
	}
	defer kv.c.Close()

	// Item set: subject index members first, then extra items.
	var items []string
	seen := map[string]bool{}
	fromSubject := map[string]bool{}
	indexMembers := map[string][]string{}
	addItem := func(raw string, subj bool) {
		a, err := ParseAddress(raw)
		if err != nil {
			log.Fatalf("item %q: %v", raw, err)
		}
		id := a.String()
		if subj {
			fromSubject[id] = true
		}
		if !seen[id] {
			seen[id] = true
			items = append(items, id)
		}
	}
	if subject != nil {
		for _, name := range []string{"owner_wallet:" + subject.String() + ":by_domain", "wallet:" + subject.String() + ":by_domain"} {
			members, err := kv.zrangeAllByLex(ctx, indexKey("dns_entries", name))
			if err != nil {
				log.Fatalf("zrangebylex %s: %v", name, err)
			}
			indexMembers[name] = members
			log.Printf("index %s: %d members", name, len(members))
			for _, m := range members {
				id, ok := itemFromLexMember(m)
				if !ok {
					log.Fatalf("unexpected index member %q", m)
				}
				addItem(id, true)
			}
		}
	}
	if *extraFlag != "" {
		lines, err := readLines(*extraFlag)
		if err != nil {
			log.Fatal(err)
		}
		for _, l := range lines {
			addItem(l, false)
		}
	}
	if len(items) == 0 {
		log.Fatal("empty item set")
	}
	log.Printf("item set: %d items (%d from subject indexes)", len(items), len(fromSubject))

	entryPayloads, err := kv.mgetPayloads(ctx, "dns_entries", items)
	if err != nil {
		log.Fatalf("mget dns_entries: %v", err)
	}
	entries := map[string]dnsEntry{}
	for id, p := range entryPayloads {
		var e dnsEntry
		if err := json.Unmarshal([]byte(p), &e); err != nil {
			log.Fatalf("decode dns_entries %s: %v", id, err)
		}
		entries[id] = e
	}
	log.Printf("dns_entries payloads: %d of %d", len(entryPayloads), len(items))

	dbNames := []string{"HOT_DB", "COLD_DB"}
	conns := map[string]*pgx.Conn{}
	for _, name := range dbNames {
		conn, err := openPG(ctx, name)
		if err != nil {
			log.Fatal(err)
		}
		defer conn.Close(ctx)
		conns[name] = conn
	}

	// Plan check on the first item before any real query.
	for _, name := range dbNames {
		plan, err := explain(ctx, conns[name], items[0])
		fmt.Printf("=== EXPLAIN %s (item %s)\n%s\n", name, items[0], plan)
		if err != nil {
			log.Fatalf("%s: %v; not running queries", name, err)
		}
	}
	if *explainOnly {
		return
	}

	if *copySubject {
		if subject == nil {
			log.Fatal("-copy-subject needs -subject")
		}
		if err := copySubjectData(ctx, kv, local, subject, indexMembers, entryPayloads, fromSubject); err != nil {
			log.Fatalf("copy subject: %v", err)
		}
	}

	// Writes per item from both databases, deduped by tx hash.
	writes := map[string][]writeRow{}
	rowCounts := map[string]int{}
	for _, item := range items {
		byHash := map[string]writeRow{}
		for _, name := range dbNames {
			rows, err := queryWrites(ctx, conns[name], name, item)
			if err != nil {
				log.Fatalf("%s writes for %s: %v", name, item, err)
			}
			rowCounts[name] += len(rows)
			for _, r := range rows {
				if *maxMcSeqno > 0 && r.McSeqno > int32(*maxMcSeqno) {
					continue
				}
				if _, ok := byHash[r.TxHash]; !ok {
					byHash[r.TxHash] = r
				}
			}
		}
		for _, r := range byHash {
			writes[item] = append(writes[item], r)
		}
		sort.Slice(writes[item], func(i, j int) bool { return writes[item][i].Lt < writes[item][j].Lt })
	}
	log.Printf("pg rows: HOT_DB %d, COLD_DB %d", rowCounts["HOT_DB"], rowCounts["COLD_DB"])

	// Bodies of successful change_dns_record messages.
	var bodyHashes []string
	seenBodies := map[string]bool{}
	for _, rows := range writes {
		for _, r := range rows {
			if r.Opcode == opChangeDNSRecord && r.BodyHash != nil && successful(r) && !seenBodies[*r.BodyHash] {
				seenBodies[*r.BodyHash] = true
				bodyHashes = append(bodyHashes, *r.BodyHash)
			}
		}
	}
	bodyPayloads, err := kv.mgetPayloads(ctx, "message_contents", bodyHashes)
	if err != nil {
		log.Fatalf("mget message_contents: %v", err)
	}
	log.Printf("message bodies: %d of %d", len(bodyPayloads), len(bodyHashes))

	results := make([]itemResult, 0, len(items))
	for _, item := range items {
		e, hasEntry := entries[item]
		res := itemResult{Item: item, Domain: e.Domain, Owner: e.NftItemOwner, DNSWallet: e.DNSWallet,
			Candidates: len(writes[item]), FromSubject: fromSubject[item]}
		if !hasEntry {
			res.Domain = "(no dns_entries row)"
		}
		var best *writeRow
		for i := range writes[item] {
			r := writes[item][i]
			if !successful(r) || r.Source == nil {
				continue
			}
			if r.Opcode == opChangeDNSRecord {
				if r.BodyHash == nil {
					continue
				}
				p, ok := bodyPayloads[*r.BodyHash]
				if !ok {
					res.MissingBodies++
					continue
				}
				var mc struct {
					Body string `json:"body"`
				}
				if err := json.Unmarshal([]byte(p), &mc); err != nil {
					log.Fatalf("decode message_contents %s: %v", *r.BodyHash, err)
				}
				isWallet, err := changesWalletKey(mc.Body)
				if err != nil {
					log.Printf("item %s tx %s: bad body: %v", item, r.TxHash, err)
					continue
				}
				if !isWallet {
					continue
				}
			}
			res.Successful++
			if best == nil || r.Lt > best.Lt {
				best = &writes[item][i]
			}
		}
		if best != nil {
			writer, err := ParseAddress(*best.Source)
			if err != nil {
				log.Fatalf("item %s writer: %v", item, err)
			}
			op := "change_dns_record"
			if best.Opcode == opEditContent {
				op = "edit_content"
			}
			res.Write = &writerPayload{SchemaVersion: 1, SourceMcSeqno: best.McSeqno, NftItemAddress: item,
				Writer: writer.String(), WriteLt: strconv.FormatInt(best.Lt, 10), WriteTxHash: best.TxHash, WriteOp: op}
		}
		owner, wallet := parseOptAddress(e.NftItemOwner), parseOptAddress(e.DNSWallet)
		res.CurrentRule = hasEntry && !e.Destroyed && sameAddr(owner, wallet)
		if res.CurrentRule && res.Write != nil {
			w, _ := ParseAddress(res.Write.Writer)
			res.NewRule = sameAddr(owner, &w)
		}
		// A missing body could hide a later wallet write, so such an item gets no writer key.
		if res.MissingBodies > 0 {
			res.Stored = "skipped: missing bodies"
		} else if res.Write != nil {
			stored, err := storeWriter(ctx, local, res.Write)
			if err != nil {
				log.Fatalf("store writer %s: %v", item, err)
			}
			res.Stored = stored
		}
		results = append(results, res)
	}

	printTable(results)
	if subject != nil {
		printSubject(subject, indexMembers, results)
	}
	if *outFlag != "" {
		data, _ := json.MarshalIndent(results, "", " ")
		if err := os.WriteFile(*outFlag, data, 0o644); err != nil {
			log.Fatal(err)
		}
	}
}

func successful(r writeRow) bool {
	return !r.Aborted && r.ComputeSuccess && !r.Bounced
}

func storeWriter(ctx context.Context, local *localRedis, w *writerPayload) (string, error) {
	data, err := json.Marshal(w)
	if err != nil {
		return "", err
	}
	keys := []string{rowKey("dns_wallet_writers", w.NftItemAddress), payloadKey("dns_wallet_writers", w.NftItemAddress)}
	n, err := setCurrentScript.Run(ctx, local.c, keys, w.SourceMcSeqno, string(data)).Int()
	if err != nil {
		return "", err
	}
	if n == 1 {
		return "written", nil
	}
	return "kept", nil
}

// copySubjectData copies what a local address-book query for the subject reads: the subject's
// dns_entries payloads, both index keys with the same members (score 0), the subject's
// latest_account_states payload and the contract_methods payload of its code hash.
func copySubjectData(ctx context.Context, kv *prodKV, local *localRedis, subject *Address,
	indexMembers map[string][]string, entryPayloads map[string]string, fromSubject map[string]bool) error {
	pipe := local.c.TxPipeline()
	nEntries := 0
	for id := range fromSubject {
		if p, ok := entryPayloads[id]; ok {
			pipe.Set(ctx, payloadKey("dns_entries", id), p, 0)
			nEntries++
		}
	}
	nMembers := 0
	for name, members := range indexMembers {
		for _, m := range members {
			pipe.ZAdd(ctx, indexKey("dns_entries", name), redis.Z{Score: 0, Member: m})
			nMembers++
		}
	}
	states, err := kv.mgetPayloads(ctx, "latest_account_states", []string{subject.String()})
	if err != nil {
		return err
	}
	nMethods := 0
	if state, ok := states[subject.String()]; ok {
		pipe.Set(ctx, payloadKey("latest_account_states", subject.String()), state, 0)
		var row struct {
			CodeHash *string `json:"code_hash"`
		}
		if err := json.Unmarshal([]byte(state), &row); err != nil {
			return err
		}
		if row.CodeHash != nil {
			methods, err := kv.mgetPayloads(ctx, "contract_methods", []string{*row.CodeHash})
			if err != nil {
				return err
			}
			if m, ok := methods[*row.CodeHash]; ok {
				pipe.Set(ctx, payloadKey("contract_methods", *row.CodeHash), m, 0)
				nMethods++
			}
		}
	}
	if _, err := pipe.Exec(ctx); err != nil {
		return err
	}
	log.Printf("copied: %d dns_entries, %d index members, %d latest_account_states, %d contract_methods",
		nEntries, nMembers, len(states), nMethods)
	return nil
}

func short(s *string) string {
	if s == nil {
		return "-"
	}
	if len(*s) > 12 {
		return (*s)[:8] + ".." + (*s)[len(*s)-4:]
	}
	return *s
}

func printTable(results []itemResult) {
	fmt.Printf("%-28s %-14s %-14s %-14s %-18s %-16s %s/%s\n", "domain", "owner", "dns_wallet", "writer", "op", "write_lt", "cur", "new")
	for _, r := range results {
		writer, op, lt := "-", "-", "-"
		if r.Write != nil {
			writer, op, lt = short(&r.Write.Writer), r.Write.WriteOp, r.Write.WriteLt
		}
		fmt.Printf("%-28s %-14s %-14s %-14s %-18s %-16s %v/%v\n", r.Domain, short(r.Owner), short(r.DNSWallet), writer, op, lt, r.CurrentRule, r.NewRule)
	}
}

// printSubject compares the current primary domain (first owner_wallet member, i.e. the shortest)
// with the shortest owner_wallet domain whose wallet-record writer is the subject.
func printSubject(subject *Address, indexMembers map[string][]string, results []itemResult) {
	byItem := map[string]itemResult{}
	for _, r := range results {
		byItem[r.Item] = r
	}
	members := indexMembers["owner_wallet:"+subject.String()+":by_domain"]
	current, next := "(none)", "(none)"
	if len(members) > 0 {
		current = members[0]
	}
	for _, m := range members {
		id, _ := itemFromLexMember(m)
		if byItem[id].NewRule {
			next = m
			break
		}
	}
	fmt.Printf("subject %s: current rule %s; new rule %s\n", subject, current, next)
}
