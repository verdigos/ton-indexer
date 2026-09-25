"""Datagrams sent by the worker's DexTap (ton-index-worker/index-engine/src/DexTap.cpp)."""
from __future__ import annotations

import json
from dataclasses import dataclass


@dataclass(frozen=True)
class TapEvent:
    session: int
    seq: int
    mc_seqno: int
    utime: int
    account: str
    last_trans_lt: int
    balance: int
    code_hash: str
    data_hash: str
    code: str
    data: str
    tx_count: int


def parse_event(datagram: bytes) -> TapEvent:
    d = json.loads(datagram)
    if d.get("v") != 1:
        raise ValueError(f"unsupported tap version {d.get('v')}")
    return TapEvent(
        session=int(d["session"]), seq=int(d["seq"]), mc_seqno=int(d["mc_seqno"]), utime=int(d["utime"]),
        account=d["account"].upper(), last_trans_lt=int(d["last_trans_lt"]), balance=int(d["balance"]),
        code_hash=d["code_hash"].upper(), data_hash=d["data_hash"].upper(), code=d["code"], data=d["data"],
        tx_count=int(d["tx_count"]),
    )


def encode_event(event: TapEvent) -> bytes:
    """The worker's wire format; used by the replay tool and tests."""
    return json.dumps({"v": 1, **event.__dict__, "balance": str(event.balance)}, separators=(",", ":")).encode()


class GapCounter:
    """Counts datagrams lost between the worker and the listener from per-session sequence numbers."""

    def __init__(self):
        self.session: int | None = None
        self.last_seq = 0
        self.lost = 0
        self.sessions = 0

    def observe(self, event: TapEvent) -> None:
        if event.session != self.session:
            # Datagrams sent before the listener saw this worker session are not losses.
            self.session, self.last_seq = event.session, event.seq - 1
            self.sessions += 1
        if event.seq > self.last_seq + 1:
            self.lost += event.seq - self.last_seq - 1
        self.last_seq = max(self.last_seq, event.seq)
