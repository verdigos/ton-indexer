package prices

import (
	"context"
	"errors"
	"sync/atomic"
	"testing"
	"testing/synctest"
	"time"
)

func TestResponseCache(t *testing.T) {
	synctest.Test(t, func(t *testing.T) {
		c := NewResponseCache(50*time.Millisecond, 1<<20, time.Second)
		var calls atomic.Int32
		release := make(chan struct{})
		get := func(key string) {
			b, err := c.Get(t.Context(), key, func(context.Context) ([]byte, error) {
				calls.Add(1)
				if key == "concurrent" {
					<-release
				}
				return []byte("a"), nil
			})
			if err != nil || string(b) != "a" {
				t.Errorf("Get: %q, %v", b, err)
			}
		}
		for range 3 {
			get("serial")
		}
		checkEqual(t, calls.Load(), 1)
		time.Sleep(100 * time.Millisecond) // the longest lifetime is 120% of ttl
		get("serial")
		checkEqual(t, calls.Load(), 2)

		for range 20 {
			go get("concurrent")
		}
		synctest.Wait() // all 20 misses are waiting for the same computation
		close(release)
		synctest.Wait()
		checkEqual(t, calls.Load(), 3)

		if _, err := c.Get(t.Context(), "panic", func(context.Context) ([]byte, error) { panic("boom") }); err == nil {
			t.Fatal("panic returned no error")
		}
		finish := make(chan struct{})
		done := make(chan error, 1)
		short, cancel := context.WithTimeout(t.Context(), 10*time.Millisecond)
		defer cancel()
		_, err := c.Get(short, "timeout", func(run context.Context) ([]byte, error) {
			<-finish
			done <- run.Err()
			return []byte("a"), nil
		})
		close(finish)
		if err == nil {
			t.Error("the caller did not time out")
		}
		if err := <-done; err != nil {
			t.Errorf("computation context: %v", err)
		}
	})
}

func TestResponseCacheSize(t *testing.T) {
	c := NewResponseCache(time.Minute, 100, time.Second)
	for _, key := range "abcdefghijkl" {
		c.Get(t.Context(), string(key), func(context.Context) ([]byte, error) {
			return []byte("0123456789012345678"), nil
		})
	}
	if c.bytes > 100 || len(c.entries) != 5 {
		t.Errorf("%d bytes in %d entries", c.bytes, len(c.entries))
	}
}

func TestLatestSnapshot(t *testing.T) {
	synctest.Test(t, func(t *testing.T) {
		rows := func(minute uint32, price float64) []Row {
			return []Row{{Asset: jettonA, Quote: "TON", Minute: minute, PriceRaw: &price, NewestUtime: minute}}
		}
		store := &fakeStore{head: Head{1, 60}, rows: rows(120, 0.5)}
		// The head advances during the first row read. The as-of stamp must include it.
		store.onLatest = func() error { store.head = Head{2, 120}; return nil }
		svc := NewService(store, nil)
		svc.LatestRefresh, svc.LatestMaxAge = 0, 30*time.Millisecond
		assets := []string{jettonA}
		price := func(want float64) Head {
			t.Helper()
			res, err := svc.Prices(t.Context(), assets)
			if err != nil {
				t.Fatal(err)
			}
			checkEqual(t, res.Prices[0].Gram.PriceRaw, want)
			return Head{res.AsOfSeqno, res.AsOfUtime}
		}
		checkEqual(t, price(0.5), Head{2, 120})
		store.onLatest = func() error { return errors.New("down") }
		price(0.5) // stale while the reload fails
		synctest.Wait()
		time.Sleep(40 * time.Millisecond)
		if _, err := svc.Prices(t.Context(), assets); err == nil {
			t.Fatal("a snapshot past the maximum age answered")
		}
		store.onLatest = nil
		price(0.5)

		var reloads atomic.Int32
		release := make(chan struct{})
		store.onLatest = func() error {
			reloads.Add(1)
			<-release
			store.head = Head{3, 180}
			return nil
		}
		store.rows = rows(180, 0.7)
		for range 5 {
			price(0.5)
		}
		synctest.Wait()
		checkEqual(t, reloads.Load(), 1)
		close(release)
		synctest.Wait()
		checkEqual(t, price(0.7), Head{3, 180})
	})
}

func TestDecimalsCache(t *testing.T) {
	const other, third = "0:AB", "0:CD"
	var calls int
	var failure error
	svc := NewService(&fakeStore{}, func(context.Context, []string) (map[string]Decimals, error) {
		calls++
		if failure != nil {
			return nil, failure
		}
		return map[string]Decimals{jettonA: {9, true}}, nil // other and third are unknown
	})
	read := func(assets ...string) (map[string]Decimals, error) { return svc.assetDecimals(t.Context(), assets) }
	res, err := read(jettonA, other, tonAsset)
	if err != nil || res[jettonA].Value != 9 || res[other].Known || !res[tonAsset].Known {
		t.Fatalf("%v %v", res, err)
	}
	if res, err = read(other, jettonA); err != nil || res[jettonA].Value != 9 || calls != 1 {
		t.Errorf("second read: %v %v, %d lookups", res, err, calls)
	}

	svc.DecimalsTTL = 0 // new entries expire at once
	failure = errors.New("down")
	if _, err := read(third); err == nil {
		t.Error("a failed lookup gave no error")
	}
	failure = nil
	for _, wantCalls := range []int{3, 4} { // retry the failure, then read the expired entry again
		if res, err := read(third); err != nil || res[third].Known || calls != wantCalls {
			t.Errorf("after recovery: %v %v, %d lookups, want %d", res, err, calls, wantCalls)
		}
	}
}
