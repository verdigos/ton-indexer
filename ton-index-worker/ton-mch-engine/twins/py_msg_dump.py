# Python twin of ton-mch-engine's `--parse-dump` (src/MsgParse.cpp), for the
# message-parsing A/B. Uses the REAL Python message classes from
# indexer.events.blocks.messages.jettons over every fixture message whose
# opcode is in the jetton family, and emits the exact same canonical dump:
#   === <fixture-basename>
#   <msg_hash> <MsgType> {k=v,...}     # vars(obj), keys sorted
#   <msg_hash> <MsgType> PARSE_FAIL
# Value rendering: null | true/false | decimal int | wc:HEX account |
# str:<s> | b64:<base64 bytes> | cell:<HASH hex upper> | {k=v,...} sorted dict |
# extern:<len>:<value>. Slice values render as their to_cell() hash.
import base64
import os
import sys

import lz4.frame
import msgpack

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "indexer"))

import indexer.events.context  # noqa: E402,F401  (break circular import)
from pytoniq_core import Address, Cell, Slice  # noqa: E402
from pytoniq_core.boc.address import ExternalAddress  # noqa: E402

from indexer.events.blocks.messages.jettons import (  # noqa: E402
    JettonBurn, JettonInternalTransfer, JettonMint, JettonNotify,
    JettonTransfer, MinterJettonMint,
)
from indexer.events.blocks.messages.dns import ChangeDnsRecordMessage  # noqa: E402
from indexer.events.blocks.messages.vesting import VestingSendMessage  # noqa: E402
from indexer.events.blocks.messages.swaps import (  # noqa: E402
    DedustPayoutFromPool, DedustSwapNotification, PTonTransfer,
)
from indexer.events.blocks.messages.liquidity import (  # noqa: E402
    StonfiV2ProvideLiquidity,
)
from indexer.events.blocks.messages.coffee import CoffeeCreateVault  # noqa: E402
from indexer.events.blocks.messages.evaa import (  # noqa: E402
    EvaaWithdrawCollateralized, EvaaWithdrawFailExcess, EvaaWithdrawMaster,
)
from indexer.events.blocks.messages.layerzero import (  # noqa: E402
    ChannelCommitPacket, LayerZeroMDLzSend, LayerZeroOappExecuteCallback,
    LayerZeroPacket, LayerZeroPath, LayerzeroChannelSendCallback,
    UlnConnectionVerifyCallbackParser,
)
from indexer.events.blocks.utils import AccountId, Asset  # noqa: E402

CLASSES = {
    0x0F8A7EA5: ("JettonTransfer", JettonTransfer),
    0x178D4519: ("JettonInternalTransfer", JettonInternalTransfer),
    0x595F07BC: ("JettonBurn", JettonBurn),
    0x7362D09C: ("JettonNotify", JettonNotify),
    0x642B7D07: ("JettonMint", JettonMint),
    0x00000015: ("MinterJettonMint", MinterJettonMint),
    0x4EB1F0F9: ("ChangeDnsRecordMessage", ChangeDnsRecordMessage),
    0xA7733ACD: ("VestingSendMessage", VestingSendMessage),
    0x01F3835D: ("PTonTransfer", PTonTransfer),
    0x37C096DF: ("StonfiV2ProvideLiquidity", StonfiV2ProvideLiquidity),
    0xAD4EB6F5: ("DedustPayoutFromPool", DedustPayoutFromPool),
    0x9C610DE3: ("DedustSwapNotification", DedustSwapNotification),
    0xC0FFEE06: ("CoffeeCreateVault", CoffeeCreateVault),
    0x00000002: ("EvaaWithdrawMaster", EvaaWithdrawMaster),
    0x00000211: ("EvaaWithdrawCollateralized", EvaaWithdrawCollateralized),
    0x000021E6: ("EvaaWithdrawFailExcess", EvaaWithdrawFailExcess),
    0x000021E7: ("EvaaWithdrawFailExcess", EvaaWithdrawFailExcess),
    0x000021E8: ("EvaaWithdrawFailExcess", EvaaWithdrawFailExcess),
    0x000021EC: ("EvaaWithdrawFailExcess", EvaaWithdrawFailExcess),
    0xCAAE25A1: ("LayerZeroOappExecuteCallback", LayerZeroOappExecuteCallback),
    0xA2B5FBAE: ("LayerzeroChannelSendCallback", LayerzeroChannelSendCallback),
    0x5388CD88: ("ChannelCommitPacket", ChannelCommitPacket),
    0x3CB38090: ("UlnConnectionVerifyCallbackParser", UlnConnectionVerifyCallbackParser),
}


# §12 root-hash bar: Cell-derived BOC container fields render as cell root hash
# ("cellhash:HEX"). comment / bitcoin_txid / pubkey stay raw. BOC_FIELDS comes
# from cpp_surface.json (`ton-mch-engine --surface`) — one source of truth (A3ii).
from cpp_surface import BOC_FIELDS  # noqa: E402


def _cellhash(v):
    # None/other type -> None (caller falls back). Bad BOC str/bytes ->
    # distinguishable DECODE_FAIL marker, never raise. Prefix rule matches the
    # C++ boc_field_cellhash (str -> string[:12]; bytes -> base64[:12]).
    if v is None:
        return None
    if isinstance(v, Cell):
        return "cellhash:" + v.hash.hex().upper()
    if isinstance(v, Slice):
        return "cellhash:" + v.to_cell().hash.hex().upper()
    if isinstance(v, str):
        src, raw = v, None
    elif isinstance(v, (bytes, bytearray)):
        src, raw = base64.b64encode(bytes(v)).decode(), bytes(v)
    else:
        return None
    try:
        if raw is None:
            raw = base64.b64decode(v)
        return "cellhash:" + Cell.one_from_boc(raw).hash.hex().upper()
    except Exception:  # noqa: BLE001
        return "cellhash:DECODE_FAIL:" + src[:12]


def _render_field(k, val):
    if k in BOC_FIELDS:
        ch = _cellhash(val)
        if ch is not None:
            return ch
    return render(val)


def render(v):
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return "str:" + v
    if isinstance(v, bytes):
        # FULL raw bytes (B0/D4): BOC container bytes are byte-comparable
        # cross-language, so payload bytes render as their exact base64.
        return "b64:" + base64.b64encode(v).decode()
    if isinstance(v, Address):
        return f"{v.wc}:{v.hash_part.hex().upper()}"
    if isinstance(v, AccountId):
        return v.as_str() if v.address is not None else "addr_none"
    if isinstance(v, Asset):
        return "asset:TON" if v.is_ton else "asset:" + v.jetton_address.as_str()
    if isinstance(v, ExternalAddress):
        # The C++ adapter carries external addresses as a Str Value.
        return f"str:extern:{v.len}:{v.external_address}"
    if isinstance(v, Cell):
        return "cell:" + v.hash.hex().upper()
    if isinstance(v, Slice):
        return "cell:" + v.to_cell().hash.hex().upper()
    if isinstance(v, dict):
        return "{" + ",".join(f"{k}={_render_field(k, v[k])}" for k in sorted(v)) + "}"
    if isinstance(v, (LayerZeroPacket, LayerZeroPath, LayerZeroMDLzSend)):
        # Nested parser objects render like the C++ Obj: vars(), keys sorted.
        fields = vars(v)
        return "{" + ",".join(f"{k}={_render_field(k, fields[k])}" for k in sorted(fields)) + "}"
    raise TypeError(f"unrenderable value type {type(v).__name__}")


def dump_fixture(path):
    try:
        with open(path, "rb") as f:
            data = msgpack.unpackb(lz4.frame.decompress(f.read()), raw=False)
    except Exception as e:  # noqa: BLE001
        print(f"ERROR: {type(e).__name__}: {e}")
        return
    seen = set()
    lines = []
    for tx in data.get("transactions", []):
        for m in tx.get("messages", []):
            op = m.get("opcode")
            content = m.get("message_content") or {}
            body = content.get("body")
            if op is None or not body:
                continue
            entry = CLASSES.get(op & 0xFFFFFFFF)
            if entry is None:
                continue
            msg_hash = m["msg_hash"]
            if msg_hash in seen:
                continue
            seen.add(msg_hash)
            name, cls = entry
            try:
                obj = cls(Slice.one_from_boc(base64.b64decode(body)))
                fields = vars(obj)
                rendered = "{" + ",".join(
                    f"{k}={_render_field(k, fields[k])}" for k in sorted(fields)) + "}"
            except Exception:  # noqa: BLE001
                rendered = "PARSE_FAIL"
            lines.append(f"{msg_hash} {name} {rendered}")
    for line in sorted(lines):
        print(line)


def main(corpus):
    if os.path.isdir(corpus):
        for name in sorted(n for n in os.listdir(corpus) if n.endswith(".lz4")):
            print(f"=== {name}")
            dump_fixture(os.path.join(corpus, name))
    else:
        print(f"=== {os.path.basename(corpus)}")
        dump_fixture(corpus)


if __name__ == "__main__":
    main(sys.argv[1])
