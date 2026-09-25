"""Send pool fixtures (indexer/tests/pools/*.json) to a dex-tap socket as the worker would.

    python -m dex_tap.replay --socket /tmp/dex-tap.sock ../indexer/tests/pools/*.json

Lets the listener, ClickHouse schema and API be tested end to end without a TON node.
"""
from __future__ import annotations

import argparse
import base64
import json
import socket
import time
from pathlib import Path

from pytoniq_core import Cell

from dex_tap.protocol import TapEvent, encode_event


def fixture_event(fixture: dict, session: int, seq: int) -> TapEvent:
    code = Cell.one_from_boc(base64.b64decode(fixture["code_boc"]))
    data = Cell.one_from_boc(base64.b64decode(fixture["data_boc"]))
    return TapEvent(
        session=session, seq=seq, mc_seqno=fixture["mc_seqno"], utime=int(fixture.get("sync_utime") or time.time()),
        account=fixture["pool"], last_trans_lt=0, balance=int(fixture["balance"]),
        code_hash=code.hash.hex().upper(), data_hash=data.hash.hex().upper(),
        code=fixture["code_boc"], data=fixture["data_boc"], tx_count=1,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Replay pool fixtures into a dex-tap socket")
    parser.add_argument("--socket", required=True)
    parser.add_argument("fixtures", nargs="+", type=Path)
    args = parser.parse_args(argv)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    session = int(time.time() * 1000)
    for seq, path in enumerate(args.fixtures, start=1):
        sock.sendto(encode_event(fixture_event(json.loads(path.read_text()), session, seq)), args.socket)
    print(f"sent {len(args.fixtures)} datagrams")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
