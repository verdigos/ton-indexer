"""dex-tap listener: receives pool states from the worker and writes snapshots.

    python -m dex_tap --socket /run/dex-tap.sock --clickhouse http://localhost:8123
    python -m dex_tap --socket /run/dex-tap.sock --ndjson ./out

Start the listener before or after the worker; datagrams sent while it is down are lost and
counted from sequence numbers once it runs.
"""
from __future__ import annotations

import argparse
import logging
import os
import socket
import time
from pathlib import Path

from dex_tap.assets import AssetResolver
from dex_tap.protocol import parse_event
from dex_tap.service import Processor
from dex_tap.sink import ClickHouseSink, NdjsonSink
from dex_tap.toncenter import Toncenter

log = logging.getLogger("dex_tap")


def bind(path: str) -> socket.socket:
    if os.path.exists(path):
        os.unlink(path)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    sock.bind(path)
    os.chmod(path, 0o660)
    sock.settimeout(1.0)
    return sock


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Receive DEX pool states from the worker's --dex-tap-socket")
    parser.add_argument("--socket", required=True, help="unix datagram socket path the worker sends to")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--clickhouse", help="ClickHouse HTTP URL, e.g. http://user:pass@localhost:8123")
    target.add_argument("--ndjson", type=Path, help="directory for NDJSON output instead of ClickHouse")
    parser.add_argument("--database", default="dex_prices")
    parser.add_argument("--cache-dir", type=Path, default=Path("~/.cache/dex-tap").expanduser(),
                        help="cache of libraries, wallet masters and jetton decimals")
    parser.add_argument("--stats-every", type=float, default=10.0, help="seconds between stats lines")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    sink = (ClickHouseSink(args.clickhouse, database=args.database) if args.clickhouse
            else NdjsonSink(args.ndjson))
    processor = Processor(AssetResolver(Toncenter(), args.cache_dir), sink)
    sock = bind(args.socket)
    log.info("listening on %s", args.socket)
    next_report = time.monotonic() + args.stats_every
    try:
        while True:
            try:
                datagram = sock.recv(1 << 20)
                try:
                    processor.handle(parse_event(datagram), len(datagram))
                except ValueError as exc:
                    processor.stats.errors["bad_datagram"] += 1
                    log.debug("bad datagram: %s", exc)
            except socket.timeout:
                sink.flush()
            if time.monotonic() >= next_report:
                sink.flush()
                log.info(processor.stats.report(sink))
                next_report = time.monotonic() + args.stats_every
    except KeyboardInterrupt:
        pass
    finally:
        sink.flush()
        log.info(processor.stats.report(sink))
        sock.close()
        os.unlink(args.socket)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
