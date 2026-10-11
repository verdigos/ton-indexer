// dns-wallet-writers backfills dns_wallet_writers: the sender of the last successful wallet-record
// write of each .ton and .t.me item.
package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
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

type sourceKV struct {
	c *redis.Client
}

func newSourceKV() (*sourceKV, error) {
	host, port := os.Getenv("KV_ROCKS_HOST"), os.Getenv("KV_ROCKS_PORT")
	if host == "" || port == "" {
		return nil, fmt.Errorf("KV_ROCKS_HOST and KV_ROCKS_PORT must be set")
	}
	c := redis.NewClient(&redis.Options{
		Addr:        net.JoinHostPort(host, port),
		Password:    os.Getenv("KV_ROCKS_PASSWORD"),
		PoolSize:    1,
		ReadTimeout: 30 * time.Second,
	})
	return &sourceKV{c: c}, nil
}

func (k *sourceKV) mgetPayloads(ctx context.Context, table string, ids []string) (map[string]string, error) {
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

func newTarget(addr string, allowRemote string, passwordEnv string) (*redis.Client, error) {
	if addr == "" {
		return nil, fmt.Errorf("-target is required unless -dry-run")
	}
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		return nil, err
	}
	if host != "127.0.0.1" && host != "localhost" && addr != allowRemote {
		return nil, fmt.Errorf("remote target %q requires -allow-remote-write-target=%s", addr, addr)
	}
	password := ""
	if passwordEnv != "" {
		password = os.Getenv(passwordEnv)
	}
	c := redis.NewClient(&redis.Options{Addr: addr, Password: password})
	if err := c.Ping(context.Background()).Err(); err != nil {
		return nil, err
	}
	return c, nil
}

// Same seqno guard as kvrocks_set_current_script in KvrocksState.cpp.
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

type writerPayload struct {
	SchemaVersion  int    `json:"schema_version"`
	SourceMcSeqno  int32  `json:"source_mc_seqno"`
	NftItemAddress string `json:"nft_item_address"`
	Writer         string `json:"writer"`
	WriteLt        string `json:"write_lt"`
	WriteTxHash    string `json:"write_tx_hash"`
	WriteOp        string `json:"write_op"`
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
	// Plan every query with its parameter values, as EXPLAIN does.
	cfg.DefaultQueryExecMode = pgx.QueryExecModeExec
	conn, err := pgx.ConnectConfig(ctx, cfg)
	if err != nil {
		return nil, fmt.Errorf("connect %s: %w", envName, err)
	}
	return conn, nil
}

func explainQuery(ctx context.Context, conn *pgx.Conn, rel string, sql string, args ...any) (string, error) {
	tx, err := conn.BeginTx(ctx, pgx.TxOptions{AccessMode: pgx.ReadOnly})
	if err != nil {
		return "", err
	}
	defer tx.Rollback(ctx)
	rows, err := tx.Query(ctx, "explain "+sql, args...)
	if err != nil {
		return "", err
	}
	lines, err := pgx.CollectRows(rows, pgx.RowTo[string])
	if err != nil {
		return "", err
	}
	plan := strings.Join(lines, "\n")
	indexed := false
	for _, line := range lines {
		if strings.Contains(line, "Index") && strings.Contains(line, " on "+rel) {
			indexed = true
		}
		_, rest, ok := strings.Cut(line, "Seq Scan on ")
		if !ok {
			continue
		}
		seqRel, _, _ := strings.Cut(rest, " ")
		var pages int64
		var tuples float64
		if err := tx.QueryRow(ctx, "select relpages::bigint, reltuples::float8 from pg_class where relname = $1", seqRel).Scan(&pages, &tuples); err != nil {
			return plan, fmt.Errorf("pg_class %s: %w", seqRel, err)
		}
		if pages > 0 || tuples > 0 {
			return plan, fmt.Errorf("plan has a sequential scan on non-empty %s: relpages %d, reltuples %.0f", seqRel, pages, tuples)
		}
	}
	if !indexed {
		return plan, fmt.Errorf("plan has no index scan on %s", rel)
	}
	return plan, nil
}

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

func main() {
	target := flag.String("target", "", "kvrocks host:port to write to; required unless -dry-run")
	allowRemote := flag.String("allow-remote-write-target", "", "remote host:port allowed as -target; must equal -target")
	passwordEnv := flag.String("write-password-env", "", "environment variable with the target password")
	minMcSeqno := flag.Int("min-mc-seqno", 0, "minimum transaction mc seqno")
	maxMcSeqno := flag.Int("max-mc-seqno", 0, "maximum transaction mc seqno, 0 = no limit")
	checkpoint := flag.String("checkpoint", "", "checkpoint file, resumed if it exists")
	dryRun := flag.Bool("dry-run", false, "do not write to -target; -out is still written")
	out := flag.String("out", "", "writer payloads as JSON lines; items with a missing body go to <out>.missing")
	flag.Usage = func() {
		fmt.Fprint(flag.CommandLine.Output(), `Usage: dns-wallet-writers [flags]

Backfills dns_wallet_writers from HOT_DB and COLD_DB into -target. Postgres and the source kvrocks
are only read. The higher source_mc_seqno wins, so it can run alongside the worker.
Environment: HOT_DB, COLD_DB, KV_ROCKS_HOST, KV_ROCKS_PORT, KV_ROCKS_PASSWORD.

`)
		flag.PrintDefaults()
	}
	flag.Parse()
	ctx := context.Background()

	var dst *redis.Client
	if !*dryRun {
		c, err := newTarget(*target, *allowRemote, *passwordEnv)
		if err != nil {
			log.Fatal(err)
		}
		defer c.Close()
		dst = c
	}

	kv, err := newSourceKV()
	if err != nil {
		log.Fatal(err)
	}
	defer kv.c.Close()

	if err := runScan(ctx, kv, dst, int32(*minMcSeqno), int32(*maxMcSeqno), *checkpoint, *out); err != nil {
		log.Fatal(err)
	}
}
