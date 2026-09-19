package crud

import (
	"context"
	"os"
	"testing"
)

func TestMakePoolRegistersTypes(t *testing.T) {
	dsn := os.Getenv("TON_INDEX_GO_TEST_PG_DSN")
	if dsn == "" {
		t.Skip("TON_INDEX_GO_TEST_PG_DSN is not set")
	}

	pool, err := makePool(dsn, 1, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()

	conn, err := pool.Acquire(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	conn.Release()
}
