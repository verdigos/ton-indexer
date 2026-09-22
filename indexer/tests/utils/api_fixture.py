from __future__ import annotations

import argparse
import base64
import hashlib
import json
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Iterable

import lz4.frame
import msgpack
from dotenv import dotenv_values
from pytoniq_core import Address, Cell, ExternalMsgInfo, ExternalOutMsgInfo, HashMap, InternalMsgInfo, Transaction


V2_URL = "https://toncenter.com/api/v2/"
V3_URL = "https://toncenter.com/api/v3/"
USER_AGENT = "ton-indexer-fixture-builder/1.0"
NOMINATOR_POOL_CODE_HASH = "mj7BS8CY9rRAZMMFIiyuooAPF92oXuaoGYpwle3hDc8="
VESTING_CONTRACT_CODE_HASH = "tItTGr7DtxRjgpH3137W3J9qJynvyiBHcTc3TUrotZA="
DEDUST_FACTORY = "EQBfBWT7X2BHg9tXAxzhz2aKiNTU1tpt5NsiK0uSDW_YAJ67"

# Methods are ordered from the cheapest, most common probes to narrower contracts.
GET_METHODS = (
    "get_wallet_data", "is_claimed", "get_jetton_data", "get_nft_data", "get_collection_data",
    "get_sale_data", "get_auction_data_v4", "get_fix_price_data_v4", "get_order_data",
    "get_multisig_data", "get_pool_data", "get_assets", "is_stable", "get_reserves",
    "get_trade_fee", "get_auction_info", "get_telemint_token_name", "get_telemint_auction_state",
    "get_telemint_auction_config", "royalty_params",
)


class ApiError(RuntimeError):
    pass


def _api_key() -> str:
    env = dotenv_values(Path(__file__).resolve().parents[3] / ".env")
    key = env.get("API_KEY")
    if not key:
        raise ApiError("API_KEY is missing from the repository .env")
    return str(key)


class ApiClient:
    def __init__(self, api_key: str | None = None, base_v2: str = V2_URL,
                 base_v3: str = V3_URL, retries: int = 10, backoff: float = 0.5,
                 opener=None, sleep=None):
        self.api_key = api_key or _api_key()
        self.base_v2 = base_v2.rstrip("/") + "/"
        self.base_v3 = base_v3.rstrip("/") + "/"
        self.retries = retries
        self.backoff = backoff
        self.opener = opener or (lambda request: urllib.request.urlopen(request, timeout=30))
        self.sleep = sleep or time.sleep

    def request(self, version: int, endpoint: str, *, params: dict | None = None,
                body: dict | None = None, gc_miss: bool = False) -> Any:
        base = self.base_v2 if version == 2 else self.base_v3
        url = urllib.parse.urljoin(base, endpoint)
        if params:
            url += "?" + urllib.parse.urlencode(params)
        payload = None if body is None else json.dumps(body).encode()
        headers = {"X-API-Key": self.api_key, "User-Agent": USER_AGENT,
                   "Accept": "application/json"}
        if payload is not None:
            headers["Content-Type"] = "application/json"

        last_status = 0
        last_gc = False
        for attempt in range(self.retries + 1):
            try:
                with self.opener(urllib.request.Request(url, payload, headers)) as response:
                    try:
                        result = json.loads(response.read())
                    except json.JSONDecodeError:
                        raise ApiError("TON API returned invalid JSON") from None
                if isinstance(result, dict) and result.get("ok") is False:
                    text = str(result.get("error", ""))
                    try:
                        status = int(result.get("code", 500))
                    except (TypeError, ValueError):
                        status = 500
                    gc = "state already gc'd" in text.lower()
                    if gc or status == 429 or status >= 500:
                        raise _Retryable(status, gc)
                    raise ApiError(f"TON API request failed with status {status}")
                return result.get("result", result) if isinstance(result, dict) else result
            except _Retryable as exc:
                last_status, last_gc = exc.status, exc.gc
            except urllib.error.HTTPError as exc:
                last_status = exc.code
                response_body = exc.read().decode("utf-8", "replace")
                last_gc = "state already gc'd" in response_body.lower()
                if exc.code != 429 and exc.code < 500:
                    raise ApiError(f"TON API request failed with status {exc.code}") from None
            except (urllib.error.URLError, TimeoutError):
                last_status = 0
            except ApiError:
                raise
            except Exception:
                raise ApiError("TON API transport failed") from None

            if attempt < self.retries:
                self.sleep(min(self.backoff * (2 ** attempt), 8.0))

        if gc_miss and last_gc:
            return None
        suffix = f" with status {last_status}" if last_status else ""
        raise ApiError("TON API request failed after retries" + suffix)

    def get_v3(self, endpoint: str, **params) -> Any:
        return self.request(3, endpoint, params=params)

    def get_transactions(self, address: str, lt: int, tx_hash: str) -> list[dict]:
        result = self.request(2, "getTransactions", params={
            "address": address, "lt": lt, "hash": tx_hash, "limit": 1,
        })
        return result if isinstance(result, list) else result.get("transactions", [])

    def get_address_information(self, address: str, seqno: int) -> dict:
        return self.request(2, "getAddressInformation", params={"address": address, "seqno": seqno})

    def run_get_method(self, address: str, method: str, seqno: int,
                       stack: list | None = None) -> dict | None:
        result = self.request(2, "runGetMethodStd", body={
            "address": address, "method": method, "stack": stack or [], "seqno": seqno,
        }, gc_miss=True)
        try:
            success = result is not None and int(result.get("exit_code", -1)) in (0, 1)
        except (AttributeError, TypeError, ValueError):
            success = False
        if not success:
            return None
        return result


class _Retryable(Exception):
    def __init__(self, status: int, gc: bool):
        self.status = status
        self.gc = gc


def _b64(value: bytes) -> str:
    return base64.b64encode(value).decode()


def fixture_int(value: int) -> int | str:
    return value if 0 <= value < (1 << 63) else str(value)


def normalize_hash(value: str) -> str:
    value = value.strip()
    if len(value) == 64:
        try:
            return _b64(bytes.fromhex(value))
        except ValueError:
            pass
    try:
        raw = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    except ValueError as exc:
        raise ValueError("hash must be 32-byte hex, base64, or base64url") from exc
    if len(raw) != 32:
        raise ValueError("hash must encode exactly 32 bytes")
    return _b64(raw)


def normalize_address(value: Any) -> str | None:
    if value is None or (isinstance(value, str) and value == ""):
        return None
    if isinstance(value, Address):
        return value.to_str(is_user_friendly=False).upper()
    if isinstance(value, dict):
        value = value.get("address") or value.get("account_address")
    try:
        return Address(str(value)).to_str(is_user_friendly=False).upper()
    except Exception:
        return str(value).upper()


def _trace_list(response: Any) -> list[dict]:
    if isinstance(response, list):
        return response
    for key in ("traces", "result"):
        if isinstance(response, dict) and isinstance(response.get(key), list):
            return response[key]
    return [response] if isinstance(response, dict) and "transactions" in response else []


def resolve_trace(client: ApiClient, input_hash: str) -> dict:
    query_hash = normalize_hash(input_hash)
    traces = _trace_list(client.get_v3("traces", tx_hash=query_hash, limit=1))
    if not traces:
        traces = _trace_list(client.get_v3("traces", msg_hash=query_hash, limit=1))
    if not traces:
        raise ApiError("TON Index API did not find a trace for the supplied hash")
    source = traces[0]
    tx_map = source.get("transactions") or {}
    records = []
    for key, item in (tx_map.items() if isinstance(tx_map, dict) else enumerate(tx_map)):
        item = dict(item)
        tx_hash = item.get("hash") or item.get("tx_hash") or (key if isinstance(key, str) else None)
        block = item.get("block_ref") or item.get("block") or {}
        records.append({
            "account": normalize_address(item.get("account") or item.get("address")),
            "lt": int(item["lt"]),
            "hash": normalize_hash(tx_hash),
            "block_seqno": int(item.get("block_seqno", block.get("seqno", 0))),
            "mc_block_seqno": int(item["mc_block_seqno"]),
        })
    records.sort(key=lambda tx: tx["lt"])
    header = {key: source.get(key) for key in (
        "trace_id", "external_hash", "mc_seqno_start", "mc_seqno_end",
        "start_lt", "start_utime", "end_lt", "end_utime",
    )}
    header["trace_id"] = normalize_hash(header["trace_id"])
    header["external_hash"] = normalize_hash(header["external_hash"]) if header["external_hash"] else None
    for key in ("mc_seqno_start", "mc_seqno_end", "start_lt", "start_utime", "end_lt", "end_utime"):
        header[key] = int(header[key])
    header.update(state="complete", pending_edges_=0, edges_=max(0, len(records) - 1),
                  nodes_=len(records), classification_state="unclassified")
    return {"trace": header, "transactions": records}


def _original_message_cells(root: Cell) -> tuple[Cell | None, list[Cell]]:
    tx_slice = root.begin_parse()
    tx_slice.skip_bits(695)
    messages = tx_slice.load_ref().begin_parse()
    incoming = messages.load_ref() if messages.load_bit() else None
    outgoing = messages.load_dict(15, value_deserializer=lambda value: value.load_ref()) or {}
    return incoming, [outgoing[key] for key in sorted(outgoing)]


def _message(message: Any, cell: Cell, direction: str, tx_hash: str,
             tx_lt: int, trace_id: str) -> dict:
    info = message.info
    internal = isinstance(info, InternalMsgInfo)
    src = normalize_address(info.src) if internal or isinstance(info, ExternalOutMsgInfo) else None
    dst = normalize_address(info.dest) if internal or isinstance(info, ExternalMsgInfo) else None
    body = message.body
    body_boc = _b64(body.to_boc())
    body_slice = body.begin_parse()
    opcode = body_slice.load_int(32) if len(body_slice.bits) >= 32 else None
    value = info.value.grams if internal else None
    extra_values = (info.value.other.dict or {}) if internal else {}
    extra = {str(k): fixture_int(v) for k, v in extra_values.items()}
    result = {
        "msg_hash": _b64(cell.hash), "tx_hash": tx_hash, "tx_lt": tx_lt,
        "direction": direction, "trace_id": trace_id, "source": src,
        "destination": dst, "value": value, "fwd_fee": getattr(info, "fwd_fee", None),
        "ihr_fee": getattr(info, "ihr_fee", None), "created_lt": getattr(info, "created_lt", None),
        "created_at": getattr(info, "created_at", None), "opcode": opcode,
        "ihr_disabled": getattr(info, "ihr_disabled", None),
        "bounce": getattr(info, "bounce", None), "bounced": bool(getattr(info, "bounced", False)),
        "import_fee": getattr(info, "import_fee", None), "body_hash": _b64(body.hash),
        "init_state_hash": None, "value_extra_currencies": extra,
        "message_content": {"hash": _b64(body.hash), "body": body_boc},
    }
    if message.init is not None:
        init = message.init.serialize()
        result["init_state_hash"] = _b64(init.hash)
        result["init_state"] = {"hash": _b64(init.hash), "body": _b64(init.to_boc())}
    return result


def parse_transaction(boc_base64: str, metadata: dict, trace_id: str) -> dict:
    root = Cell.one_from_boc(base64.b64decode(boc_base64))
    tx = Transaction.deserialize(root.begin_parse())
    tx_hash = _b64(root.hash)
    expected_address = Address(metadata["account"])
    if tx_hash != normalize_hash(metadata["hash"]) or tx.lt != int(metadata["lt"]):
        raise ValueError("raw transaction does not match the requested hash and lt")
    if tx.account_addr != expected_address.hash_part:
        raise ValueError("raw transaction does not match the requested account")
    incoming, outgoing = _original_message_cells(root)
    if (tx.in_msg is None) != (incoming is None) or len(tx.out_msgs) != len(outgoing):
        raise ValueError("raw transaction message dictionary is inconsistent")
    messages = []
    if tx.in_msg is not None and incoming is not None:
        messages.append(_message(tx.in_msg, incoming, "in", tx_hash, tx.lt, trace_id))
    messages.extend(_message(msg, cell, "out", tx_hash, tx.lt, trace_id)
                    for msg, cell in zip(tx.out_msgs, outgoing))

    descr = tx.description
    descr_name = {"ordinary": "ord"}.get(descr.type_, descr.type_)
    compute = getattr(descr, "compute_ph", None)
    compute_vm = compute is not None and compute.type_ == "vm"
    return {
        "block_seqno": int(metadata.get("block_seqno", 0)),
        "mc_block_seqno": int(metadata["mc_block_seqno"]), "trace_id": trace_id,
        "account": normalize_address(metadata["account"]), "hash": tx_hash, "lt": tx.lt,
        "prev_trans_hash": _b64(tx.prev_trans_hash), "prev_trans_lt": tx.prev_trans_lt,
        "now": tx.now, "orig_status": {"uninitialized": "uninit"}.get(tx.orig_status.type_, tx.orig_status.type_),
        "end_status": {"uninitialized": "uninit"}.get(tx.end_status.type_, tx.end_status.type_),
        "total_fees": tx.total_fees.grams,
        "account_state_hash_before": _b64(tx.state_update.old_hash),
        "account_state_hash_after": _b64(tx.state_update.new_hash),
        "descr": descr_name, "aborted": getattr(descr, "aborted", None),
        "destroyed": getattr(descr, "destroyed", None),
        "credit_first": getattr(descr, "credit_first", None),
        "is_tock": getattr(descr, "is_tock", None), "installed": getattr(descr, "installed", None),
        "compute_mode": compute.mode if compute_vm else None,
        "compute_exit_code": compute.exit_code if compute_vm else None,
        "compute_exit_arg": compute.exit_arg if compute_vm else None,
        "bounce": getattr(getattr(descr, "bounce", None), "type_", None),
        "emulated": False, "messages": messages,
    }


def stack_number(entry: dict) -> int:
    if entry.get("@type") != "tvm.stackEntryNumber":
        raise ValueError("expected TVM number")
    number = entry["number"]
    if isinstance(number, dict):
        number = number["number"]
    return int(number, 0) if isinstance(number, str) else int(number)


def stack_cell(entry: dict) -> tuple[Cell, str]:
    type_ = entry.get("@type")
    if type_ not in ("tvm.stackEntrySlice", "tvm.stackEntryCell"):
        raise ValueError("expected TVM slice or cell")
    field = "slice" if type_ == "tvm.stackEntrySlice" else "cell"
    value = entry[field]
    boc = value["bytes"] if isinstance(value, dict) else value
    return Cell.one_from_boc(base64.b64decode(boc)), boc


def stack_address(entry: dict) -> str | None:
    if entry.get("@type") != "tvm.stackEntrySlice":
        raise ValueError("expected TVM address slice")
    cell, _ = stack_cell(entry)
    return normalize_address(cell.begin_parse().load_address())


def _number_entry(value: int) -> dict:
    return {"@type": "tvm.stackEntryNumber",
            "number": {"@type": "tvm.numberDecimal", "number": str(value)}}


def _method(client: ApiClient, address: str, method: str, seqno: int,
            stack: list | None = None) -> list[dict] | None:
    try:
        result = client.run_get_method(address, method, seqno, stack or [])
    except Exception:
        return None
    return result.get("stack") if result else None


_CONTENT_FIELDS = ("uri", "name", "description", "image", "image_data",
                   "symbol", "decimals", "amount_style", "render_type")


def _content_value(value) -> bytes:
    if value.remaining_bits == 0 and value.remaining_refs == 1:
        value = value.load_ref().begin_parse()
    tag = value.load_uint(8)
    if tag == 0:
        return value.load_snake_bytes()
    if tag != 1:
        raise ValueError("unknown content value")
    chunks = value.load_dict(32, value_deserializer=lambda item: item.load_ref()) or {}
    return b"".join(chunks[key].begin_parse().load_snake_bytes() for key in sorted(chunks))


def token_content(entry: dict) -> dict[str, str] | None:
    try:
        if entry.get("@type") != "tvm.stackEntryCell":
            return None
        cell, _ = stack_cell(entry)
        value = cell.begin_parse()
        tag = value.load_uint(8)
        if tag == 1:
            return {"uri": value.load_snake_string()}
        if tag != 0:
            return None
        values = value.load_dict(256) or {}
        result = {}
        for name in _CONTENT_FIELDS:
            item = values.get(int.from_bytes(hashlib.sha256(name.encode()).digest(), "big"))
            if item is None:
                continue
            raw = _content_value(item)
            result[name] = _b64(raw) if name == "image_data" else raw.decode()
        return result
    except Exception:
        return None


def _signers(entry: dict) -> list[str]:
    if entry.get("@type") != "tvm.stackEntryCell":
        raise ValueError("expected TVM signer dictionary cell")
    cell, _ = stack_cell(entry)
    values = HashMap.parse(cell.begin_parse(), 8) or {}
    return [normalize_address(values[key].load_address()) for key in sorted(values)]


def _interface_methods(client: ApiClient, address: str, seqno: int, code_hash: str) -> dict:
    found: dict[str, Any] = {}
    stacks = {name: _method(client, address, name, seqno) for name in GET_METHODS}
    try:
        s = stacks["get_wallet_data"]
        if s and len(s) == 4:
            owner, master = stack_address(s[1]), stack_address(s[2])
            verified = _method(client, master, "get_wallet_address", seqno, [s[1]])
            if not verified or len(verified) != 1 or stack_address(verified[0]) != address:
                raise ValueError("jetton master returned another wallet address")
            found["JettonWallet"] = {"balance": fixture_int(stack_number(s[0])), "address": address,
                                      "owner": owner, "jetton": master}
    except Exception:
        pass
    try:
        s = stacks["get_nft_data"]
        if s and len(s) == 5:
            collection = stack_address(s[2])
            content = token_content(s[4])
            if collection:
                index = stack_number(s[1])
                verified = _method(client, collection, "get_nft_address_by_index", seqno,
                                   [_number_entry(index)])
                complete = _method(client, collection, "get_nft_content", seqno,
                                   [_number_entry(index), s[4]])
                if not verified or len(verified) != 1 or stack_address(verified[0]) != address:
                    raise ValueError("NFT collection returned another item address")
                if not complete or len(complete) != 1:
                    raise ValueError("NFT collection did not return complete content")
                content = token_content(complete[0])
                if content is None:
                    raise ValueError("NFT content is invalid")
            found["NftItem"] = {"address": address, "init": bool(stack_number(s[0])),
                                "index": fixture_int(stack_number(s[1])), "collection_address": collection,
                                "owner_address": stack_address(s[3]), "content": content,
                                "code_hash": code_hash}
    except Exception:
        pass
    try:
        s = stacks["get_sale_data"]
        if s and len(s) == 11 and stack_number(s[0]) == 0x46495850:
            found["NftSale"] = _sale(address, s, code_hash)
        elif s and len(s) >= 20 and stack_number(s[0]) == 0x415543:
            found["NftAuction"] = _auction(s, code_hash)
    except Exception:
        pass
    try:
        s = stacks["get_fix_price_data_v4"]
        if s and len(s) == 13:
            found["NftSale"] = _sale(address, s, code_hash, offset=-1)
    except Exception:
        pass
    try:
        s = stacks["get_auction_data_v4"]
        if s and len(s) == 26:
            found["NftAuction"] = _auction(s, code_hash)
    except Exception:
        pass
    try:
        s = stacks["get_order_data"]
        if s and len(s) == 9:
            multisig, seqno_value = stack_address(s[0]), stack_number(s[1])
            verified = _method(client, multisig, "get_order_address", seqno,
                               [_number_entry(seqno_value)])
            if not verified or len(verified) != 1 or stack_address(verified[0]) != address:
                raise ValueError("multisig returned another order address")
            _, order_boc = stack_cell(s[8])
            found["MultisigOrder"] = {
                "address": address, "multisig_address": multisig,
                "order_seqno": fixture_int(seqno_value), "threshold": stack_number(s[2]),
                "sent_for_execution": bool(stack_number(s[3])), "approvals_mask": fixture_int(stack_number(s[5])),
                "approvals_num": stack_number(s[6]), "expiration_date": fixture_int(stack_number(s[7])),
                "signers": _signers(s[4]), "order_boc": order_boc, "last_transaction_lt": None,
                "code_hash": code_hash, "data_hash": None,
            }
    except Exception:
        pass
    try:
        v2 = bool(stacks["get_pool_data"])
        s = stacks["get_pool_data"] or stacks["get_assets"]
        if s and len(s) >= 2:
            pair = s[3:5] if len(s) >= 11 else s[:2]
            if not v2:
                stable = stacks["is_stable"]
                verified = _method(client, DEDUST_FACTORY, "get_pool_address", seqno,
                                   [_number_entry(stack_number(stable[0])), *pair]) if stable else None
                if not verified or len(verified) != 1 or stack_address(verified[0]) != address:
                    raise ValueError("DeDust factory returned another pool address")
            found["DedustPool"] = {"assets": [_asset(item, v2) for item in pair]}
    except Exception:
        pass
    try:
        token = stacks["get_telemint_token_name"]
        config = stacks["get_telemint_auction_config"]
        royalty = stacks["royalty_params"]
        null_types = {"tvm.stackEntryUnsupported", "tvm.stackEntryNull"}
        if (len(token) != 1 or token[0].get("@type") != "tvm.stackEntrySlice"
                or len(config) != 6 or len(royalty) != 3):
            raise ValueError("invalid Telemint stack")
        expected = ("tvm.stackEntrySlice",) + ("tvm.stackEntryNumber",) * 5
        if any(item.get("@type") not in ({type_} | null_types)
               for item, type_ in zip(config, expected)):
            raise ValueError("invalid Telemint auction config")
        stack_number(royalty[0])
        stack_number(royalty[1])
        stack_address(royalty[2])
        found["Telemint"] = {"address": address}
    except Exception:
        pass
    return found


def _sale(address: str, s: list, code_hash: str, offset: int = 0) -> dict:
    return {"address": address, "is_complete": bool(stack_number(s[1 + offset])),
            "marketplace_address": stack_address(s[3 + offset]), "nft_address": stack_address(s[4 + offset]),
            "nft_owner_address": stack_address(s[5 + offset]), "full_price": fixture_int(stack_number(s[6 + offset])),
            "marketplace_fee_address": stack_address(s[7 + offset]), "marketplace_fee": fixture_int(stack_number(s[8 + offset])),
            "royalty_address": stack_address(s[9 + offset]), "royalty_amount": fixture_int(stack_number(s[10 + offset])),
            "code_hash": code_hash}


def _auction(s: list, code_hash: str) -> dict:
    return {"nft_addr": stack_address(s[4]), "nft_owner": stack_address(s[5]),
            "last_bid": fixture_int(stack_number(s[6])), "last_member": stack_address(s[7]),
            "mp_addr": stack_address(s[3]), "mp_fee_addr": stack_address(s[9]),
            "mp_fee_factor": stack_number(s[10]), "mp_fee_base": stack_number(s[11]),
            "royalty_fee_addr": stack_address(s[12]), "royalty_fee_factor": stack_number(s[13]),
            "royalty_fee_base": stack_number(s[14]), "max_bid": fixture_int(stack_number(s[15])),
            "min_bid": fixture_int(stack_number(s[16])), "code_hash": code_hash}


def _asset(entry: dict, v2: bool) -> dict:
    cell, _ = stack_cell(entry)
    value = cell.begin_parse()
    if v2:
        if value.preload_uint(2) == 0:
            return {"is_ton": True, "address": None}
        address = normalize_address(value.load_address())
    else:
        tag = value.load_uint(4)
        if tag not in (0, 1):
            raise ValueError("unsupported DeDust asset")
        address = None if tag == 0 else f"{value.load_int(8)}:{value.load_bytes(32).hex().upper()}"
    return {"is_ton": address is None, "address": address}


def _jvault_wallet(data_boc: str) -> tuple[str, str]:
    value = Cell.one_from_boc(base64.b64decode(data_boc)).begin_parse()
    return normalize_address(value.load_address()), normalize_address(value.load_address())


def _jvault_pool(data_boc: str) -> str:
    value = Cell.one_from_boc(base64.b64decode(data_boc)).begin_parse()
    value.skip_bits(33)
    value.load_address()
    value.load_address()
    return normalize_address(value.load_address())


def collect_interfaces(client: ApiClient, addresses: Iterable[str], seqno: int,
                       jvault_addresses: Iterable[str] = ()) -> dict[str, dict]:
    result: dict[str, dict] = {}
    states: dict[str, dict] = {}

    def collect_one(address: str) -> None:
        if address in result:
            return
        record: dict[str, Any] = {}
        result[address] = record
        try:
            state = client.get_address_information(address, seqno)
        except ApiError:
            return
        states[address] = state
        data_boc, code_boc = state.get("data"), state.get("code")
        if data_boc:
            record["data_boc"] = data_boc
        if not data_boc or not code_boc:
            return
        try:
            code_hash_bytes = Cell.one_from_boc(base64.b64decode(code_boc)).hash
        except Exception:
            return
        code_hash_b64 = _b64(code_hash_bytes)
        record["code_hash"] = code_hash_bytes.hex().upper()
        if code_hash_b64 == NOMINATOR_POOL_CODE_HASH:
            record["NominatorPool"] = {"address": address}
        if code_hash_b64 == VESTING_CONTRACT_CODE_HASH:
            record["Vesting"] = {"address": address}
        record.update(_interface_methods(client, address, seqno, code_hash_b64))

    for address in sorted(filter(None, {normalize_address(item) for item in addresses})):
        collect_one(address)

    for stake in filter(None, {normalize_address(item) for item in jvault_addresses}):
        collect_one(stake)
        data_boc = states.get(stake, {}).get("data")
        if not data_boc:
            continue
        result[stake]["data_boc"] = {"data_boc": data_boc}
        try:
            pool, _ = _jvault_wallet(data_boc)
            if not pool:
                continue
            collect_one(pool)
            pool_boc = states.get(pool, {}).get("data")
            if not pool_boc:
                continue
            lock = _jvault_pool(pool_boc)
            if not lock:
                continue
            result[pool]["data_boc"] = {"data_boc": pool_boc,
                                         "lock_wallet_address": lock}
            collect_one(lock)
        except Exception:
            continue
    return result


def build_fixture(input_hash: str, client: ApiClient | None = None) -> dict:
    client = client or ApiClient()
    resolved = resolve_trace(client, input_hash)
    header = resolved["trace"]
    transactions = []
    addresses: set[str] = set()
    jvault_addresses: set[str] = set()
    for metadata in resolved["transactions"]:
        rows = client.get_transactions(metadata["account"], metadata["lt"], metadata["hash"])
        if len(rows) != 1 or not rows[0].get("data"):
            raise ApiError("v2 did not return the requested raw transaction")
        tx = parse_transaction(rows[0]["data"], metadata, header["trace_id"])
        transactions.append(tx)
        addresses.add(tx["account"])
        for message in tx["messages"]:
            addresses.update(filter(None, (message["source"], message["destination"])))
            if message["destination"] and message["opcode"] is not None and (
                    message["opcode"] & 0xFFFFFFFF) in {
                        0xD68A4AC1, 0x499A9262, 0x0168D4B7, 0x78D9F109,
                    }:
                jvault_addresses.add(message["destination"])
    return {"trace": header, "transactions": transactions,
            "interfaces": collect_interfaces(client, addresses, header["mc_seqno_end"],
                                             jvault_addresses)}


def write_fixture(fixture: dict, path: str | Path) -> None:
    packed = msgpack.packb(fixture, use_bin_type=True)
    Path(path).write_bytes(lz4.frame.compress(packed))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build an LZ4 trace fixture from TON HTTP APIs")
    parser.add_argument("hash", help="transaction or message hash in hex, base64, or base64url")
    parser.add_argument("--out", required=True, type=Path, help="output .lz4 path")
    args = parser.parse_args(argv)
    try:
        write_fixture(build_fixture(args.hash), args.out)
    except (ApiError, ValueError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
