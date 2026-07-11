# Python twin of ton-mch-engine's `--classify` (src/Classify.h format contract).
# Runs the REAL IR engine end-to-end: build_matchers + the event_processing
# outer loop VERBATIM (per matcher: bfs_iter with the parent-is-None guard,
# root wrapper block) + IrMatcher.try_build (match -> BuildProgram.run ->
# merge -> aux append). BuildProgram.run is wrapped per matcher to capture the
# BuildOutcome (flows/btype) that try_build does not expose.
import asyncio
import base64
import os
import sys

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

# C++ registry surface (parsers/lookup_kinds/shapers/preds/fns/boc_fields),
# loaded from twins/cpp_surface.json — emitted by `ton-mch-engine --surface`,
# one source of truth for all twins (A3ii). Regenerate via cpp_surface.py's
# module docstring.
from cpp_surface import (  # noqa: E402
    BOC_FIELDS, CPP_FNS, CPP_LOOKUP_KINDS, CPP_PARSERS, CPP_PREDS, CPP_SHAPERS,
)

# Combined registries: same factory set as py_match_dump.py.
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


# CPP_PARSERS/CPP_LOOKUP_KINDS/CPP_SHAPERS/CPP_PREDS/CPP_FNS come from
# cpp_surface (imported above). BUILTINS is not a host registry (it's the fixed
# expr-builtin set), so it stays inline here.
BUILTINS = {"account", "amount", "asset", "ton_asset", "addr_none", "b64",
            "first", "last", "len", "sum", "zip", "map"}


def block_key(b):
    op = getattr(b, "opcode", None)
    s = b.btype
    if op is not None:
        s += "#%08x" % (op & 0xFFFFFFFF)
    s += "@%d" % b.min_lt
    return s


# §12 root-hash bar: Cell-derived BOC container fields (BOC_FIELDS, from
# cpp_surface) render as their cell root hash ("cellhash:HEX"),
# provenance-invisible. Raw non-BOC bytes (comment/bitcoin_txid/pubkey) stay as-is.
def _cellhash(v):
    # None/other type -> None (caller falls back). Bad BOC str/bytes ->
    # distinguishable DECODE_FAIL marker, never raise. Prefix rule matches the
    # C++ boc_field_cellhash (str -> string[:12]; bytes -> base64[:12]).
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
        # tuples (e.g. dedust vault_excesses (Asset, Amount) pairs) render
        # list-style; the C++ side encodes them as a 2-element List so the
        # bracketed forms line up.
        return "[" + ",".join(render(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{" + ",".join(f"{k}={_render_field(k, v[k])}" for k in sorted(v)) + "}"
    if hasattr(v, "__dict__"):
        d = vars(v)
        return "obj{" + ",".join(f"{k}={_render_field(k, d[k])}" for k in sorted(d)) + "}"
    raise TypeError(f"unrenderable {type(v).__name__}")


def _refs(rec):
    types, lookups, fns = set(), set(), set()

    def scan(e):
        if isinstance(e, dict):
            if e.get("s") == "parse":
                types.update(e["types"])
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
    return types, lookups, fns


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
    types, lookups, fns = _refs(rec)
    parts = []
    missing_t = sorted(t for t in types if t not in CPP_PARSERS)
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


def wrap_program(m):
    """Capture the BuildOutcome try_build swallows (flows/btype for the dump)."""
    prog = m._program
    if prog is None or getattr(prog, "_wrapped", False):
        return
    orig = prog.run

    async def run(env):
        out = await orig(env)
        prog._last_outcome = out
        return out

    prog.run = run
    prog._wrapped = True


async def classify_fixture(path, included):
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

    # Canonical-order BFS (T21 extension, DELIBERATE deviation from
    # event_processing.bfs_iter's raw insertion order): the C++ substrate keeps
    # next_blocks canonical (min_lt asc, stable) at all times, so the raw
    # Python insertion order is not representable there. Both sides walk the
    # canonical order; generator semantics (children read AFTER try_build ran
    # on the parent) are preserved.
    def bfs_canonical(start):
        queue = [start]
        while queue:
            cur = queue.pop(0)
            yield cur
            queue.extend(sorted(cur.next_blocks, key=lambda x: x.min_lt))

    # Per-trace exception boundary: event_processing.py wraps the whole matcher
    # loop in `except Exception` and falls back to basic actions, discarding the
    # trace's regular actions. The C++ engine mirrors this at its classify trace
    # boundary (Classify.cpp). Here a pred/builder/parser exception escaping the
    # loop prints a single `TRACE_FAIL` line on stdout (the A/B channel), the
    # detail on stderr. No corpus trace triggers this.
    try:
        # Phase E: COLLECT produced actions during the matcher loop; the
        # post-process passes (which merge/remove blocks) run AFTER, then the
        # surviving actions are emitted from their FINAL state. flows/aux are
        # fire-time (unchanged by pairing); data/consumed are post-pass.
        pending = []  # (matcher_name, anchor_key, produced, flows_s, aux_s)
        for m in included:
            for b in bfs_canonical(root):
                if b.parent is not None:
                    continue
                m._program._last_outcome = None
                r = await m.try_build(b)
                if not r:
                    continue
                produced, aux = r[0], r[1:]
                outcome = m._program._last_outcome
                flows_s = "[" + ",".join(
                    f.kind + "(" + ",".join(render(a) for a in f.args) + ")"
                    for f in outcome.flows) + "]"
                aux_s = "[" + ",".join(block_key(a) for a in aux) + "]"
                pending.append((m.name, block_key(b), produced, flows_s, aux_s))

        # Spine list ([root] + bfs), then the REAL host passes. removed = the
        # produced blocks a pass merged away (spine diff); every other collected
        # action is still emitted (per-fire dump).
        spine = [root]
        for cur in bfs_canonical(root):
            spine.append(cur)
        before = set(id(x) for x in spine)
        spine = await post_process_dedust_liquidity(spine)
        spine = await post_process_stonfi_v2_liquidity(spine)
        spine = await unwind_deployments(spine)  # Step 2b (production chain parity)
        removed = before - set(id(x) for x in spine)

        for (name, anchor_key, produced, flows_s, aux_s) in pending:
            if id(produced) in removed:
                continue
            data_s = "{" + ",".join(
                f"{k}={_render_field(k, produced.data[k])}" for k in sorted(produced.data)) + "}"
            consumed_s = "[" + ",".join(sorted(block_key(c) for c in produced.children_blocks)) + "]"
            print(f"ACTION {name} anchor={anchor_key} btype={produced.btype} "
                  f"failed={1 if produced.failed else 0} broken={1 if produced.broken else 0} "
                  f"data={data_s} flows={flows_s} consumed={consumed_s} auto={aux_s}")
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
    for m in included:
        wrap_program(m)

    names = ([corpus] if not os.path.isdir(corpus) else
             [os.path.join(corpus, n) for n in sorted(os.listdir(corpus)) if n.endswith(".lz4")])
    for path in names:
        print(f"=== {os.path.basename(path)}")
        asyncio.run(classify_fixture(path, included))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
