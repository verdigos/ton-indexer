package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/toncenter/ton-indexer/ton-index-price-api/aggregate"
)

func parseMinute(name, v string) (int64, error) {
	if v == "" {
		return 0, nil
	}
	n, err := strconv.ParseInt(v, 10, 64)
	if err != nil || n < 0 {
		return 0, fmt.Errorf("-%s: want non-negative unix seconds, got %q", name, v)
	}
	return n - n%60, nil
}

func main() {
	dsn := flag.String("dsn", "", "ClickHouse DSN of the prices database, e.g. clickhouse://localhost:9000/dex_prices (required)")
	maxRange := flag.Int("max-range", 1440, "minutes per statement run")
	interval := flag.Duration("interval", 5*time.Second, "pause when there is nothing to do or after an error")
	once := flag.Bool("once", false, "compute the ready minutes and the current minute, then exit; exit code 1 on an error")
	startFlag := flag.String("start", "", "first minute when the job never ran (unix seconds; default: the oldest snapshot's minute)")
	untilFlag := flag.String("until", "", "last minute to compute (unix seconds; default: no limit)")
	rewindFlag := flag.String("rewind-from", "", "recompute from this minute (unix seconds) up to the head, then continue")
	flag.Parse()

	logger := log.New(os.Stderr, "dex-prices-aggregator: ", log.LstdFlags|log.LUTC)
	if *dsn == "" {
		flag.Usage()
		os.Exit(2)
	}
	start, err := parseMinute("start", *startFlag)
	if err != nil {
		logger.Fatal(err)
	}
	until, err := parseMinute("until", *untilFlag)
	if err != nil {
		logger.Fatal(err)
	}
	rewind, err := parseMinute("rewind-from", *rewindFlag)
	if err != nil {
		logger.Fatal(err)
	}
	if *maxRange < 1 {
		logger.Fatal("-max-range must be at least 1")
	}
	db, err := aggregate.OpenClickHouse(*dsn)
	if err != nil {
		logger.Fatal(err)
	}
	defer db.Close()

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	job := aggregate.NewJob(db, aggregate.Options{Start: start, Until: until, MaxRange: *maxRange, Log: logger.Printf})
	if *rewindFlag != "" {
		if err := job.Rewind(ctx, rewind); err != nil {
			logger.Fatal(err)
		}
	}
	for {
		worked, err := job.Step(ctx)
		if ctx.Err() != nil {
			return
		}
		if err == nil && worked {
			continue
		}
		if err == nil {
			err = job.UpdateCurrentMinute(ctx)
		}
		if err != nil {
			if *once {
				logger.Fatal(err)
			}
			logger.Printf("error (retrying in %v): %v", *interval, err)
		} else if *once {
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(*interval):
		}
	}
}
