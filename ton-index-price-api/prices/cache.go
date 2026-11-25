package prices

import (
	"context"
	"fmt"
	"log"
	"math/rand"
	"sync"
	"time"

	"golang.org/x/sync/singleflight"
)

// ResponseCache keeps encoded answers for a short time. When several requests miss the same key at once, one
// computes the answer and the others wait for it. Each entry lives 80-120% of ttl, so entries do not all expire at
// the same moment.
type ResponseCache struct {
	ttl      time.Duration // 0: no caching
	maxBytes int64
	timeout  time.Duration // limit of one computation; a caller that gives up does not cancel it for the others

	group   singleflight.Group
	mu      sync.RWMutex
	entries map[string]cachedResponse
	bytes   int64
	swept   time.Time
}

type cachedResponse struct {
	body    []byte
	expires time.Time
}

func NewResponseCache(ttl time.Duration, maxBytes int64, timeout time.Duration) *ResponseCache {
	return &ResponseCache{ttl: ttl, maxBytes: maxBytes, timeout: timeout, entries: map[string]cachedResponse{}}
}

// Get returns the cached answer of key or computes it. Errors are not cached. ctx only limits how long this caller waits.
func (c *ResponseCache) Get(ctx context.Context, key string, compute func(context.Context) ([]byte, error)) ([]byte, error) {
	if c.ttl <= 0 {
		return compute(ctx)
	}
	c.mu.RLock()
	entry, ok := c.entries[key]
	c.mu.RUnlock()
	if ok && time.Now().Before(entry.expires) {
		return entry.body, nil
	}
	result := c.group.DoChan(key, func() (any, error) {
		runCtx, cancel := context.WithTimeout(context.Background(), c.timeout)
		defer cancel()
		body, err := protected(runCtx, compute)
		if err != nil {
			return nil, err
		}
		c.put(key, body)
		return body, nil
	})
	select {
	case r := <-result:
		if r.Err != nil {
			return nil, r.Err
		}
		return r.Val.([]byte), nil
	case <-ctx.Done():
		return nil, unavailable(ctx.Err())
	}
}

// protected turns a panic into an error: compute runs on its own goroutine, out of reach of the HTTP recover handler.
func protected(ctx context.Context, compute func(context.Context) ([]byte, error)) (body []byte, err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("panic: %v", r)
		}
	}()
	return compute(ctx)
}

func (c *ResponseCache) put(key string, body []byte) {
	size := int64(len(key) + len(body))
	if size > c.maxBytes {
		return
	}
	now := time.Now()
	c.mu.Lock()
	defer c.mu.Unlock()
	if now.Sub(c.swept) >= c.ttl {
		c.swept = now
		for k, e := range c.entries {
			if now.After(e.expires) {
				c.remove(k, e)
			}
		}
	}
	if old, ok := c.entries[key]; ok {
		c.remove(key, old)
	}
	for k, e := range c.entries { // map order is random, so this drops random entries
		if c.bytes+size <= c.maxBytes {
			break
		}
		c.remove(k, e)
	}
	c.entries[key] = cachedResponse{body: body, expires: now.Add(c.ttl * time.Duration(80+rand.Intn(41)) / 100)}
	c.bytes += size
}

func (c *ResponseCache) remove(key string, e cachedResponse) {
	delete(c.entries, key)
	c.bytes -= int64(len(key) + len(e.body))
}

// latestSnapshot holds the head and the newest rows of all assets. It is read again about every LatestRefresh.
type latestSnapshot struct {
	head   Head
	rows   map[[2]string]*Row // by (asset, quote)
	loaded time.Time
}

// latest returns the snapshot. After LatestRefresh it starts one reload in the background and still returns the old
// snapshot. After LatestMaxAge, or when there is no snapshot, the caller waits for the reload, and gets an error if
// the reload fails.
func (s *Service) latest(ctx context.Context) (*latestSnapshot, error) {
	if snapshot := s.snapshot.Load(); snapshot != nil {
		age := time.Since(snapshot.loaded)
		if age < s.LatestRefresh {
			return snapshot, nil
		}
		if age < s.LatestMaxAge {
			s.reload.DoChan("latest", s.loadLatest)
			return snapshot, nil
		}
	}
	select {
	case r := <-s.reload.DoChan("latest", s.loadLatest):
		if r.Err != nil {
			return nil, unavailable(r.Err)
		}
		return r.Val.(*latestSnapshot), nil
	case <-ctx.Done():
		return nil, unavailable(ctx.Err())
	}
}

func (s *Service) loadLatest() (_ any, err error) {
	// Runs on the singleflight goroutine, out of reach of the HTTP recover handler.
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("panic: %v", r)
			log.Printf("latest prices: %v", err)
		}
	}()
	ctx, cancel := context.WithTimeout(context.Background(), s.LatestTimeout)
	defer cancel()
	// The head is read after the prices, so as_of is never older than a row the prices read returned.
	rows, err := s.store.Latest(ctx)
	if err != nil {
		log.Printf("latest prices: %v", err)
		return nil, err
	}
	head, err := s.store.Head(ctx)
	if err != nil {
		log.Printf("latest prices: %v", err)
		return nil, err
	}
	snapshot := &latestSnapshot{head: head, rows: make(map[[2]string]*Row, len(rows)), loaded: time.Now()}
	for i := range rows {
		snapshot.rows[[2]string{rows[i].Asset, rows[i].Quote}] = &rows[i]
	}
	s.snapshot.Store(snapshot)
	return snapshot, nil
}

const maxCachedDecimals = 200_000 // about 25 MB; when it is full, the cache starts empty again

type cachedDecimals struct {
	value   Decimals
	expires time.Time
}

// cachedDecimalsOf reads decimals through a cache, so most requests do not read kvrocks. Unknown decimals are cached
// too.
func (s *Service) cachedDecimalsOf(ctx context.Context, masters []string) (map[string]Decimals, error) {
	res := make(map[string]Decimals, len(masters))
	var missing []string
	now := time.Now()
	s.decimalsMu.RLock()
	for _, master := range masters {
		if e, ok := s.decimalsCache[master]; ok && now.Before(e.expires) {
			res[master] = e.value
		} else {
			missing = append(missing, master)
		}
	}
	s.decimalsMu.RUnlock()
	if len(missing) == 0 {
		return res, nil
	}
	found, err := s.decimals(ctx, missing)
	if err != nil {
		return nil, err
	}
	expires := now.Add(s.DecimalsTTL)
	s.decimalsMu.Lock()
	defer s.decimalsMu.Unlock()
	if len(s.decimalsCache)+len(missing) > maxCachedDecimals {
		s.decimalsCache = map[string]cachedDecimals{}
	}
	for _, master := range missing {
		res[master] = found[master]
		s.decimalsCache[master] = cachedDecimals{value: found[master], expires: expires}
	}
	return res, nil
}
