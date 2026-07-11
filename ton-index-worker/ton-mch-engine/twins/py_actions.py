# Python twin of ton-mch-engine's `--actions` (Classify.cpp run_actions).
# Same collect->pass pipeline as py_classify.py (the classify twin), but each
# SURVIVING post-pass produced block is rendered as a serialized Action row via
# the REAL block_tree_serializer.block_to_action (ground truth), then printed in
# the C++ render_action column set (sorted keys). Scoped to the 18 composite
# btypes the C++ fill_action ports; other btypes are skipped on both sides.
#
# Staged out (matches C++): accounts / role_assignment / the tx_hashes extension
# by initiating_event_node. tx_hashes here is the BASE set (event_nodes only) —
# recomputed, NOT read from the mutated action. trace_id / trace_* / finality
# are constant-per-fixture and not rendered (a stub Trace satisfies the API).
import asyncio
import base64
import os
import sys
import types

import lz4.frame
import msgpack

sys.path.insert(0, r"S:\Dev\ton-indexer-clear\indexer")

import indexer.events.context as context  # noqa: E402
from pytoniq_core import Address, Cell  # noqa: E402

from tests.utils.trace_deserializer import deserialize_transaction  # noqa: E402
from tests.utils.repository import TestInterfaceRepository  # noqa: E402
from indexer.events.blocks.core import Block  # noqa: E402
from indexer.events.blocks.utils import AccountId, Amount, Asset  # noqa: E402
from indexer.events.blocks.utils.tree_utils import to_tree  # noqa: E402
from indexer.events.event_processing import init_block, unwind_deployments  # noqa: E402
from indexer.events.mch.registry import Registries  # noqa: E402
from indexer.events.blocks.liquidity import (  # noqa: E402
    post_process_dedust_liquidity, post_process_stonfi_v2_liquidity,
)
from indexer.events.mch_ir import build_matchers, load_artifact  # noqa: E402
from indexer.events.blocks.utils.block_tree_serializer import block_to_action  # noqa: E402

# Combined registries: same factory set as py_classify.py.
from indexer.events.mch.builders import (  # noqa: E402
    coffee_registries, dedust_hybrid_registries, dedust_registries,
    dedust_withdraw_registries, dns_registries, jetton_decl_registries,
    jetton_registries, stonfi_v2_registries,
)
from indexer.events.mch.builders.coffee import coffee_liquidity_registries  # noqa: E402
from indexer.events.mch.builders.dedust import dedust_deposit_registries  # noqa: E402
from indexer.events.mch.builders.ethena import ethena_registries  # noqa: E402
from indexer.events.mch.builders.evaa import evaa_registries  # noqa: E402
from indexer.events.mch.builders.getgems import getgems_registries  # noqa: E402
from indexer.events.mch.builders.jvault import jvault_registries  # noqa: E402
from indexer.events.mch.builders.layerzero import layerzero_receive_registries  # noqa: E402
from indexer.events.mch.builders.nft_transfer import nft_transfer_registries  # noqa: E402
from indexer.events.mch.builders.nominator import nominator_registries  # noqa: E402
from indexer.events.mch.builders.stonfi_liquidity import stonfi_liquidity_registries  # noqa: E402
from indexer.events.mch.builders.stonfi_provide import stonfi_provide_registries  # noqa: E402
from indexer.events.mch.builders.stonfi_v1 import stonfi_v1_registries  # noqa: E402
from indexer.events.mch.builders.telegram_nft import telegram_nft_registries  # noqa: E402
from indexer.events.mch.builders.tgbtc import tgbtc_registries  # noqa: E402
from indexer.events.mch.builders.tonco import tonco_liquidity_registries  # noqa: E402
from indexer.events.mch.builders.tonco_swap import tonco_swap_registries  # noqa: E402
from indexer.events.mch.builders.tonstakers import tonstakers_registries  # noqa: E402
from indexer.events.mch.builders.vesting import vesting_registries  # noqa: E402

_FACTORIES = [
    coffee_registries, dedust_hybrid_registries, dedust_registries,
    dedust_withdraw_registries, dns_registries, jetton_decl_registries,
    jetton_registries, stonfi_v2_registries, coffee_liquidity_registries,
    dedust_deposit_registries, ethena_registries, evaa_registries,
    getgems_registries, jvault_registries, layerzero_receive_registries,
    nft_transfer_registries,
    nominator_registries, stonfi_liquidity_registries, stonfi_provide_registries,
    stonfi_v1_registries, telegram_nft_registries, tgbtc_registries,
    tonco_liquidity_registries, tonco_swap_registries, tonstakers_registries,
    vesting_registries,
]
_DICTS = ("opcodes", "predicates", "block_types", "builders",
          "message_types", "lookups", "fns", "shapers")


def combined_registries() -> Registries:
    out = Registries()
    for f in _FACTORIES:
        r = f()
        for d in _DICTS:
            getattr(out, d).update(getattr(r, d))
    return out


# --- C++ surface (shared contract with BuildDriver.cpp / py_classify.py) ------
CPP_PARSERS = {"JettonTransfer", "JettonInternalTransfer", "JettonBurn",
               "JettonNotify", "JettonMint", "MinterJettonMint",
               "JettonBurnNotification", "ChangeDnsRecordMessage",
               "VestingSendMessage", "PTonTransfer", "StonfiV2ProvideLiquidity",
               "DedustPayoutFromPool",
               "DedustSwapNotification", "CoffeeCreateVault", "EvaaWithdrawMaster",
               "EvaaWithdrawCollateralized", "EvaaWithdrawFailExcess",
               "LayerZeroOappExecuteCallback",
               "JVaultUnstakeJettons", "JVaultUnstakeRequest", "JVaultClaim"}
CPP_LOOKUP_KINDS = {"jetton_wallet", "nft_item", "nominator_pool", "jvault_assets",
                    "nft_sale", "nft_auction"}
CPP_SHAPERS = {"getgems_proxy_insert", "nft_transfer_parent_absorb",
               "tgbtc_deploy_absorb"}
CPP_PREDS = {"nominator_pool_withdraw_parent", "evaa_user_withdraw_user",
             "evaa_user_withdraw_success", "evaa_user_withdraw_fail",
             "evaa_service_comment", "getgems_purchase", "getgems_seller_payout",
             "stonfi_v1_sender_payment",
             "no_internal_transfer", "pton_self_transfer"}
CPP_FNS = {"dedust_swap_legs", "dedust_swap_failed",
           "layerzero_dst_oapp_matches", "tonstakers_minted_nft",
           "jvault_stake_period", "jvault_account_list",
           "nft_transfer_data", "getgems_nft_purchase_data",
           "telegram_nft_purchase_data", "tgbtc_mint_data",
           "coffee_swap_data", "stonfi_v1_swap_data",
           "dedust_deposit_final_data", "dedust_deposit_partial_data",
           "tonco_deposit_liquidity_data", "tonco_swap_data",
           "stonfi_v2_swap_data"}
BUILTINS = {"account", "amount", "asset", "ton_asset", "addr_none", "b64",
            "first", "last", "len", "sum", "zip", "map"}

# The 18 composite btypes the C++ fill_action ports (Classify.cpp fill_action).
# Gate emit on the BLOCK btype (not the rewritten action.type), matching C++.
# vesting_send_message + tgbtc_mint re-included after the serializer _dget
# dual-dispatch flip (both fills now serve the declarative dict block).
PORTED_BTYPES = {
    "jetton_transfer", "jetton_burn", "jetton_mint", "jetton_swap",
    "nft_transfer", "dedust_deposit_liquidity", "dedust_deposit_liquidity_partial",
    "tonco_deposit_liquidity", "evaa_withdraw", "jvault_stake", "jvault_unstake",
    "jvault_unstake_request", "jvault_claim", "ethena_deposit",
    "layerzero_receive", "coffee_create_vault", "vesting_send_message",
    "tgbtc_mint",
}

# The C++ render_action column set (Classify.cpp render_action), in any order —
# render() sorts keys, so both sides align.
ACTION_COLUMNS = [
    "source", "source_secondary", "destination", "destination_secondary",
    "asset", "asset_secondary", "asset2", "asset2_secondary",
    "amount", "value", "opcode",
    "ton_transfer_data", "jetton_transfer_data", "jetton_swap_data",
    "nft_transfer_data", "nft_listing_data", "dex_deposit_liquidity_data",
    "staking_data", "evaa_withdraw_data", "vesting_send_message_data",
    "layerzero_packet_data", "jvault_stake_data", "jvault_claim_data", "extra",
]

# Stub Trace: block_to_action requires a non-None trace and reads these attrs,
# but the C++ dump renders NONE of them, so any values work.
_STUB_TRACE = types.SimpleNamespace(
    finality=None, end_lt=0, end_utime=0, external_hash=None,
    external_hash_norm=None, mc_seqno_end=0,
)


def block_key(b):
    op = getattr(b, "opcode", None)
    s = b.btype
    if op is not None:
        s += "#%08x" % (op & 0xFFFFFFFF)
    s += "@%d" % b.min_lt
    return s


# §12 root-hash bar: Cell-derived BOC container fields render as their cell
# root hash ("cellhash:HEX") so pytoniq-vs-td serialization provenance is
# invisible to the byte-level A/B comparison. comment/bitcoin_txid/pubkey are
# raw non-BOC bytes and stay as-is.
BOC_FIELDS = {"forward_payload", "custom_payload", "message_boc",
              "message_boc_str", "order_boc", "order_boc_str", "stonfi_swap_body"}


def _cellhash(v):
    # None/other type -> None (caller falls back to normal render). A bad BOC
    # str/bytes -> distinguishable DECODE_FAIL marker, never raise. Prefix rule
    # matches the C++ boc_field_cellhash (str -> string[:12]; bytes -> base64[:12]).
    if v is None:
        return None
    if isinstance(v, Cell):
        return "cellhash:" + v.hash.hex().upper()
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
    if isinstance(v, float):
        return "float:%.17g" % v
    if isinstance(v, Amount):
        if v.value is None:
            return "amount:nan"
        if isinstance(v.value, float):
            return "amount:%.17g" % v.value
        return "amount:" + str(v.value)
    if isinstance(v, str):
        return "str:" + v
    if isinstance(v, bytes):
        return "b64:" + base64.b64encode(v).decode()
    if isinstance(v, AccountId):
        return v.as_str() if v.address is not None else "addr_none"
    if isinstance(v, Address):
        return f"{v.wc}:{v.hash_part.hex().upper()}"
    if isinstance(v, Asset):
        return "asset:TON" if v.is_ton else "asset:" + v.jetton_address.as_str()
    if isinstance(v, Cell):
        return "cell:" + v.hash.hex().upper()
    if isinstance(v, Block):
        return "block:" + block_key(v)
    if isinstance(v, (list, tuple)):
        return "[" + ",".join(render(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{" + ",".join(f"{k}={_render_field(k, v[k])}" for k in sorted(v)) + "}"
    if hasattr(v, "__dict__"):
        d = vars(v)
        return "obj{" + ",".join(f"{k}={_render_field(k, d[k])}" for k in sorted(d)) + "}"
    raise TypeError(f"unrenderable {type(v).__name__}")


def render_action_row(block):
    """block_to_action (real serializer) -> the C++ render_action column dict."""
    action = block_to_action(block, "twin", _STUB_TRACE)
    # Step 2a: tx_hashes is the EXTENDED set (block_to_action adds the initiating
    # tx) and accounts is the assembled role-less set — both straight from the
    # real serializer now (no longer staged out).
    d = {
        "id": action.action_id,
        "type": action.type,
        "success": action.success,
        "start_lt": action.start_lt,
        "end_lt": action.end_lt,
        "start_utime": action.start_utime,
        "end_utime": action.end_utime,
        "mc_seqno_end": action.mc_seqno_end,
        "tx_hashes": sorted(action.tx_hashes),
        "accounts": sorted(action.accounts),
    }
    for col in ACTION_COLUMNS:
        d[col] = getattr(action, col, None)
    return render(d)


def _refs(rec):
    types_, lookups, fns = set(), set(), set()

    def scan(e):
        if isinstance(e, dict):
            if e.get("s") == "parse":
                types_.update(e["types"])
            if e.get("k") == "lookup":
                lookups.add(e["name"])
            if e.get("k") == "call" and e["fn"] not in BUILTINS:
                fns.add(e["fn"])
            for v in e.values():
                scan(v)
        elif isinstance(e, list):
            for it in e:
                scan(it)

    scan(rec.get("build_program", []))
    return types_, lookups, fns


def _preds(rec, nodes):
    names = set()
    anchor = rec["anchor"]
    if anchor["kind"] == "pred":
        names.add(anchor["pred"])
    if anchor["kind"] == "mixed":
        for br in anchor["branches"]:
            if "where" in br:
                names.add(br["where"])
    seen, stack = set(), [rec["root"]]
    while stack:
        nid = stack.pop()
        if nid in seen:
            continue
        seen.add(nid)
        n = nodes[nid]
        if n["kind"] == "pred":
            names.add(n["pred"])
        if "where" in n:
            names.add(n["where"])
        for key in ("child", "parent", "step", "exit"):
            if key in n:
                stack.append(n[key])
        for key in ("children", "branches"):
            stack.extend(n.get(key, ()))
    return names


def skip_reason(rec, nodes):
    preds = _preds(rec, nodes)
    missing = preds - CPP_PREDS
    if missing:
        return "pred:" + ",".join(sorted(missing))
    if rec["anchor"]["kind"] == "mixed" and preds:
        return "mixed_anchor_preds"
    if "build_program" not in rec:
        return "builder" if "builder" in rec else "no_build_program"
    types_, lookups, fns = _refs(rec)
    parts = []
    missing_t = sorted(t for t in types_ if t not in CPP_PARSERS)
    missing_l = sorted(l for l in lookups if l not in CPP_LOOKUP_KINDS)
    missing_f = sorted(f for f in fns if f not in CPP_FNS)
    if missing_t:
        parts.append("types:" + ",".join(missing_t))
    if missing_l:
        parts.append("lookups:" + ",".join(missing_l))
    if missing_f:
        parts.append("fns:" + ",".join(missing_f))
    if "shape" in rec and rec["shape"] not in CPP_SHAPERS:
        parts.append("shaper:" + rec["shape"])
    return ";".join(parts) or None


async def actions_fixture(path, included):
    try:
        with open(path, "rb") as f:
            data = msgpack.unpackb(lz4.frame.decompress(f.read()), raw=False)
        txs = [deserialize_transaction(t) for t in data["transactions"]]
        tree_root = init_block(to_tree(txs))
    except Exception as e:  # noqa: BLE001
        print(f"ERROR: {type(e).__name__}: {e}")
        return
    context.interface_repository.set(TestInterfaceRepository(data.get("interfaces", {})))

    root = Block("root", [])
    root.connect(tree_root)

    def bfs_canonical(start):
        queue = [start]
        while queue:
            cur = queue.pop(0)
            yield cur
            queue.extend(sorted(cur.next_blocks, key=lambda x: x.min_lt))

    try:
        # Phase E: COLLECT produced blocks during the matcher loop; post-process
        # passes run AFTER, then surviving produced blocks are serialized from
        # their FINAL state. (Same collect->pass pipeline as py_classify.py.)
        pending = []  # (anchor_key, produced)
        for m in included:
            for b in bfs_canonical(root):
                if b.parent is not None:
                    continue
                r = await m.try_build(b)
                if not r:
                    continue
                produced = r[0]
                pending.append((block_key(b), produced))

        spine = [root]
        for cur in bfs_canonical(root):
            spine.append(cur)
        before = set(id(x) for x in spine)
        spine = await post_process_dedust_liquidity(spine)
        spine = await post_process_stonfi_v2_liquidity(spine)
        spine = await unwind_deployments(spine)  # Step 2b (production chain parity)
        removed = before - set(id(x) for x in spine)

        for (anchor_key, produced) in pending:
            if id(produced) in removed:
                continue
            if produced.btype not in PORTED_BTYPES:
                continue  # btype outside the ported set (C++ build_action -> false)
            print(f"ACTION anchor={anchor_key} {render_action_row(produced)}")
    except Exception as e:  # noqa: BLE001
        print("TRACE_FAIL")
        print(f"TRACE_FAIL {os.path.basename(path)}: {type(e).__name__}: {e}", file=sys.stderr)


def main(artifact_path, corpus):
    art = load_artifact(artifact_path)
    matchers = build_matchers(art, combined_registries())
    order = sorted(art.registration_order,
                   key=lambda mi: art.matchers[mi].get("priority", 100))
    reasons = [skip_reason(art.matchers[mi], art.nodes) for mi in order]

    skips = sorted((matchers[i].name, reasons[i])
                   for i in range(len(matchers)) if reasons[i] is not None)
    for name, reason in skips:
        print(f"SKIP {name} reason={reason}")
    included = [matchers[i] for i in range(len(matchers)) if reasons[i] is None]

    names = ([corpus] if not os.path.isdir(corpus) else
             [os.path.join(corpus, n) for n in sorted(os.listdir(corpus)) if n.endswith(".lz4")])
    for path in names:
        print(f"=== {os.path.basename(path)}")
        asyncio.run(actions_fixture(path, included))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
