# Python twin of ton-mch-engine's `--actions` (Classify.cpp run_actions).
# Same collect->pass pipeline as py_classify.py (the classify twin), but the row
# set is serialize_blocks': every POST-PASS SPINE block (matcher products AND the
# basic leaves no matcher consumed), rendered via the REAL
# block_tree_serializer.block_to_action (ground truth), then printed in the C++
# render_action column set (sorted keys). Scoped to the btypes the C++
# fill_action ports; other btypes are skipped on both sides.
#
# Not rendered (matches C++): roles (this branch's action_accounts are
# role-less) and trace_id / trace_* / finality (constant per fixture; a stub
# Trace satisfies the API). accounts and the initiating-tx tx_hashes extension
# ARE rendered — both come straight from the real serializer (Step 2a).
import asyncio
import base64
import os
import sys
import types

import lz4.frame
import msgpack

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "indexer"))

import indexer.events.context as context  # noqa: E402
from pytoniq_core import Address, Cell  # noqa: E402

from tests.utils.trace_deserializer import deserialize_transaction  # noqa: E402
from tests.utils.repository import TestInterfaceRepository  # noqa: E402
from indexer.events.blocks.core import Block  # noqa: E402
from indexer.events.blocks.utils import AccountId, Amount, Asset  # noqa: E402
from indexer.events.blocks.utils.tree_utils import to_tree  # noqa: E402
from indexer.events.event_processing import (  # noqa: E402
    init_block, init_from_external, matchers_for_failed_externals, unwind_deployments,
)
from indexer.events.blocks.liquidity import (  # noqa: E402
    post_process_dedust_liquidity, post_process_stonfi_v2_liquidity,
)
from indexer.events.mch_ir import build_matchers, load_artifact  # noqa: E402
from indexer.events.blocks.utils.block_tree_serializer import (  # noqa: E402
    _calc_action_id, block_to_action, create_unknown_action, v1_ops,
)

# Combined host-builder registries: ONE shared factory list for all three
# twins (twin_registries.py) — adding a protocol is a single edit there.
from twin_registries import combined_registries  # noqa: E402


# C++ registry surface (parsers/lookup_kinds/shapers/preds/fns/boc_fields) from
# cpp_surface.json (`ton-mch-engine --surface`) — one source of truth, no
# per-twin drift (A3ii). BUILTINS is the fixed expr-builtin set, not a host
# registry, so it stays inline.
from cpp_surface import (  # noqa: E402
    BOC_FIELDS, CPP_FNS, CPP_LOOKUP_KINDS, CPP_PARSERS, CPP_PREDS, CPP_SHAPERS,
    assert_no_skips,
)

BUILTINS = {"account", "amount", "asset", "ton_asset", "addr_none", "b64",
            "first", "last", "len", "sum", "zip", "map", "concat", "contains"}

# The composite btypes the C++ fill_action ports (ActionBuild.cpp fill_action).
# Gate emit on the BLOCK btype (not the rewritten action.type), matching C++.
# vesting_send_message + tgbtc_mint re-included after the serializer _dget
# dual-dispatch flip (both fills now serve the declarative dict block).
PORTED_BTYPES = {
    # The BASIC leaves serialize_blocks emits for every spine block a matcher did
    # not consume (fill_call_contract / fill_ton_transfer).
    "call_contract", "contract_deploy", "ton_transfer",
    "jetton_transfer", "jetton_burn", "jetton_mint", "jetton_swap",
    "nft_transfer", "dedust_deposit_liquidity", "dedust_deposit_liquidity_partial",
    "tonco_deposit_liquidity", "evaa_withdraw", "jvault_stake", "jvault_unstake",
    "jvault_unstake_request", "jvault_claim", "ethena_deposit",
    "layerzero_receive", "coffee_create_vault", "vesting_send_message",
    "tgbtc_mint", "dedust_v2_claim_reward",
    # EM2 W6 bucket-B fills. NONE of these fire on the current 165 fixtures, so
    # widening the set changes no output today — it keeps the twin
    # correct-by-construction: the day a fixture carrying one is added, Python
    # emits its row instead of silently skipping it while C++ emits one, which
    # would read as a bogus cpp-only-row rather than the parity it is.
    "renew_dns", "change_dns", "delete_dns", "nominator_pool_withdraw_request",
    "tonstakers_withdraw", "tonstakers_withdraw_request",
    "dex_deposit_liquidity", "dex_withdraw_liquidity", "nft_purchase",
    # Teleitem auction port (2026-07-30) — both fire on the two (ab-gap)
    # fixtures; C++ fills them via fill_nft_put_on_auction / fill_cancel_nft_trade.
    "teleitem_start_auction", "teleitem_cancel_auction",
    # Coffee liquidity port (2026-07-30): deposit btype, C++ fills via
    # fill_dedust_deposit (same routing as Python serializer).
    "coffee_deposit_liquidity",
    # NFT mint port (2026-07-30) — fires on the six corpus fixtures that deploy
    # an account carrying an NftItem interface; C++ fill is fill_nft_mint, the
    # one producer of the nft_mint_data composite.
    "nft_mint",
}

# The C++ render_action column set (Classify.cpp render_action), in any order —
# render() sorts keys, so both sides align.
ACTION_COLUMNS = [
    "source", "source_secondary", "destination", "destination_secondary",
    "asset", "asset_secondary", "asset2", "asset2_secondary",
    "amount", "value", "opcode",
    "ton_transfer_data", "jetton_transfer_data", "jetton_swap_data",
    "nft_transfer_data", "nft_listing_data", "nft_mint_data",
    "dex_deposit_liquidity_data",
    "dex_withdraw_liquidity_data",
    "staking_data", "evaa_withdraw_data", "vesting_send_message_data",
    "layerzero_packet_data", "jvault_stake_data", "jvault_claim_data",
    "change_dns_record_data", "extra",
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


# §12 root-hash bar: Cell-derived BOC container fields (BOC_FIELDS, from
# cpp_surface) render as their cell root hash ("cellhash:HEX") so pytoniq-vs-td
# serialization provenance is invisible to the byte-level A/B comparison.
# comment/bitcoin_txid/pubkey are raw non-BOC bytes and stay as-is.


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


def render_action(action, parent_id=None, ancestor=()):
    """Action -> the C++ render_action column dict.

    THE render seam: every row kind funnels through here, so py_actions_msgpack
    swaps the whole dump to msgpack by rebinding this one name.

    parent_id/ancestor come from the serialize_rows recursion rather than off the
    Action, because the two row kinds that bypass it (the `unknown` stub and the
    ghost rows) never assign them — and C++ renders those as null/[] too.
    """
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
    d["parent_action_id"] = parent_id
    d["ancestor_type"] = sorted(ancestor)
    return render(d)


def render_action_row(row):
    """A surviving serialize_rows row -> its rendered line (the real serializer)."""
    block, parent_id, ancestor = row
    return render_action(block_to_action(block, "twin", _STUB_TRACE), parent_id, ancestor)


def unknown_trace_stub(data, txs):
    """The Trace create_unknown_action reads, from the fixture's `trace` record.

    The five lt/utime/seqno values are TRACE-ROW values on both sides (C++:
    TraceLoader decodes the same record into mch::Trace) — recomputing them off
    the transactions would be the twin inventing semantics Python does not have.
    `finality` mirrors the C++ stub emission; `trace_id` must be the REAL id,
    since create_unknown_action uses it as the action_id.
    """
    t = data["trace"]
    return types.SimpleNamespace(
        trace_id=t["trace_id"], transactions=txs,
        start_lt=t["start_lt"], end_lt=t["end_lt"],
        start_utime=t["start_utime"], end_utime=t["end_utime"],
        mc_seqno_end=t["mc_seqno_end"], external_hash=t["external_hash"],
        finality=_STUB_TRACE.finality, external_hash_norm=_STUB_TRACE.external_hash_norm,
    )


def render_unknown_row(trace):
    """W4: the synthetic `unknown` row for a trace that matched nothing.

    trace_processor.py:44-46 takes this branch when classification SUCCEEDS with
    zero actions; create_unknown_action is the real serializer's function, called
    here exactly as production calls it.
    """
    return render_action(create_unknown_action(trace))


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


def bfs_canonical(start):
    queue = [start]
    while queue:
        cur = queue.pop(0)
        yield cur
        queue.extend(sorted(cur.next_blocks, key=lambda x: x.min_lt))


def serializes_to_row(b):
    """serialize_blocks' row filter (block_tree_serializer.py:1648-1655).

    Also the filter try_classify_unknown_trace applies inline (event_processing.py
    :370-375) — the same three exclusions, so ONE predicate serves both paths.
    """
    if b.btype in ("root", "empty"):
        return False
    if b.btype == "call_contract" and (b.event_nodes[0].message.destination is None
                                       or b.event_nodes[0].message.source is None):
        return False
    return True


def serialize_rows(blocks, parent_id=None):
    """serialize_blocks (block_tree_serializer.py:1645-1681) — rows + CHILD RECURSION.

    Mirrors the real serializer rather than calling it, for the same reason
    serializes_to_row above does: the dump needs the BLOCK behind each row (for
    the anchor and the PORTED_BTYPES gate), which the Action does not carry.
    Each element is (block, parent_action_id, ancestor_type).

    v1_ops is imported from the serializer, so the two sets cannot drift; the
    C++ copy (ClassifyCore.cpp is_v1_op) is the one to keep in step.
    """
    out = []
    ids = []
    for b in blocks:
        if not serializes_to_row(b):
            continue
        aid = _calc_action_id(b)
        if parent_id is not None and aid == parent_id:
            continue
        out.append((b, parent_id, []))
        ids.append(aid)
        if b.btype in v1_ops:
            continue
        for cb, cparent, canc in serialize_rows(b.children_blocks, aid):
            if cb.btype == "contract_deploy":
                continue
            canc = sorted(set(canc + [b.btype]))
            cid = _calc_action_id(cb)
            if cid in ids:
                raise Exception(f"Duplicate action id {cid}")
            ids.append(cid)
            out.append((cb, cparent, canc))
    return out


async def ghost_external_rows(txs):
    """try_process_unknown_event (event_processing.py:345-366) — the GHOST path.

    A trace whose root is a lone ext-in transaction with no children serialized
    nothing, so Python re-reads the wallet external's body, synthesizes a ghost
    child node per UNSENT outgoing message (init_from_external, :159-196) and
    serializes those instead of the `unknown` row.

    The tree is rebuilt from scratch (`to_tree` again) exactly as production
    does: init_from_external MUTATES the root node (forces failed, appends
    children), and the caller's tree is already wired into the main block tree.

    Post-pass chain: the twin's three (the C++ ClassifyCore chain), not
    production's `trace_post_processors` list. Immaterial here — a single-tx
    trace can carry no dedust/stonfi deposit — and keeping it identical to the
    main path is what makes this A/B-comparable.

    Any exception = Python's own `except` in try_classify_unknown_trace: the
    trace falls back to the `unknown` row rather than failing.
    """
    try:
        node = to_tree(txs)
        if len(node.children) != 0 or node.message is None or node.message.source is not None:
            return []
        root = Block("root", [])
        b = init_from_external(node)
        if b is None:
            return []
        root.connect(b)
        if len(b.next_blocks) == 0:
            return []
        for m in matchers_for_failed_externals:
            for cur in bfs_canonical(root):
                if cur.parent is None:
                    await m.try_build(cur)
        spine = list(bfs_canonical(root))
        spine = await post_process_dedust_liquidity(spine)
        spine = await post_process_stonfi_v2_liquidity(spine)
        spine = await unwind_deployments(spine)
        # FLAT: try_classify_unknown_trace open-codes the row filter instead of
        # calling serialize_blocks, so no child recursion on this path.
        return [(b2, None, []) for b2 in spine if serializes_to_row(b2)]
    except Exception:  # noqa: BLE001
        return []


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

    try:
        # Phase E: run the matcher loop, then the post-process passes; rows come
        # from the FINAL spine. (Same collect->pass pipeline as py_classify.py.)
        for m in included:
            for b in bfs_canonical(root):
                if b.parent is not None:
                    continue
                await m.try_build(b)

        spine = [root]
        for cur in bfs_canonical(root):
            spine.append(cur)
        spine = await post_process_dedust_liquidity(spine)
        spine = await post_process_stonfi_v2_liquidity(spine)
        spine = await unwind_deployments(spine)  # Step 2b: promotes ContractDeploy

        # serialize_blocks (block_tree_serializer.py:1648-1655) verbatim: skip the
        # root wrapper, `empty` proxies, and a call_contract with a null source or
        # destination. Everything else is a row — including every basic leaf no
        # matcher consumed, and EXCLUDING a product a later matcher consumed
        # (that one is off the spine) — UNLESS its consumer's btype is outside
        # v1_ops, which brings it back as a child row carrying parent_action_id.
        rows = serialize_rows(spine)

        # Spine ORDER is not comparable: Python's compact_connections rebuilds
        # next_blocks through a `set()`, so it varies run to run, and nothing
        # downstream of the serializer depends on row order (they are DB rows
        # keyed by action_id). Both sides sort the rendered lines instead.
        lines = [f"ACTION anchor={block_key(r[0])} {render_action_row(r)}"
                 for r in rows if r[0].btype in PORTED_BTYPES]
        for line in sorted(lines):
            print(line)

        # W4: trace_processor.py:44-46 — classification succeeded and serialized
        # nothing. Gated on `rows`, NOT on the lines actually printed: the
        # PORTED_BTYPES filter above is a twin-scoping device with no C++
        # counterpart (there, build_action simply declines the row after the
        # fact), so a trace whose only rows are unported btypes is NOT unknown on
        # either side.
        #
        # try_classify_unknown_trace (:367-388) tries the GHOST path FIRST and
        # only falls back to the `unknown` row when it yields nothing — so the
        # unknown gate is `ghost`, again the row list rather than the printed
        # lines.
        if not rows and txs:
            ghost = await ghost_external_rows(txs)
            if ghost:
                lines = [f"ACTION anchor={block_key(r[0])} {render_action_row(r)}"
                         for r in ghost if r[0].btype in PORTED_BTYPES]
                for line in sorted(lines):
                    print(line)
            else:
                print(f"ACTION anchor=unknown {render_unknown_row(unknown_trace_stub(data, txs))}")
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
    # A skip against ir/mch_ir.json is stale-mirror drift, never a run property
    # — abort instead of silently emitting a smaller row set (cpp_surface.py).
    assert_no_skips(skips)
    included = [matchers[i] for i in range(len(matchers)) if reasons[i] is None]

    names = ([corpus] if not os.path.isdir(corpus) else
             [os.path.join(corpus, n) for n in sorted(os.listdir(corpus)) if n.endswith(".lz4")])
    for path in names:
        print(f"=== {os.path.basename(path)}")
        asyncio.run(actions_fixture(path, included))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
