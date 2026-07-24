# Python twin of ton-mch-engine's `--match` dump, for match-phase A/B.
#
# Uses the REAL mch_ir loader + engine walker (match path only: test_self +
# _match_node(root, ..., absorb=False) + _assemble_captures — NO build), over
# the same shared leaf block tree the C++ substrate builds. Emits the exact same
# dump format (see MatchRunner.h contract): SKIP lines once at top, then per
# fixture `=== <name>` + MATCH lines. The skip set (pred / where_expr dependent
# matchers) is computed from the artifact identically to the C++ loader, so the
# skip set itself is A/B'd.
import os
import sys

import lz4.frame
import msgpack

sys.path.insert(0, r"S:\Dev\ton-indexer-clear\indexer")

import indexer.events.context  # noqa: E402,F401  (break circular import)
from tests.utils.trace_deserializer import deserialize_transaction  # noqa: E402
from indexer.events.blocks.utils.tree_utils import to_tree  # noqa: E402
from indexer.events.event_processing import init_block  # noqa: E402
from indexer.events.mch.registry import Registries  # noqa: E402
from indexer.events.mch_ir import build_matchers, load_artifact  # noqa: E402
from indexer.events.mch_ir.engine import _MatchContext  # noqa: E402

# --- combined registries (all per-spec factories merged) -------------------
from indexer.events.mch.builders import (  # noqa: E402
    coffee_registries, dedust_hybrid_registries, dedust_registries,
    dedust_withdraw_registries, dedust_v2_claim_reward_registries,
    dns_registries, jetton_decl_registries,
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
    dedust_withdraw_registries, dedust_v2_claim_reward_registries,
    dns_registries, jetton_decl_registries,
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


# --- skip-set computation (mirror IrLoader runnability, from the artifact) ---
def _edge_refs(rec):
    if "child" in rec:
        yield rec["child"]
    yield from rec.get("children", ())
    if "parent" in rec:
        yield rec["parent"]
    yield from rec.get("branches", ())
    if "step" in rec:
        yield rec["step"]
    if "exit" in rec:
        yield rec["exit"]


def _reachable(nodes, root):
    seen = {root}
    out = [root]
    i = 0
    while i < len(out):
        for r in _edge_refs(nodes[out[i]]):
            if r not in seen:
                seen.add(r)
                out.append(r)
        i += 1
    return out


# Host predicate surface from cpp_surface.json (`ton-mch-engine --surface`) —
# one source of truth for all twins (A3ii).
from cpp_surface import CPP_PREDS  # noqa: E402


def skip_reason(rec, nodes):
    """None if runnable, else the reason string (matches IrLoader exactly).

    Inline where_exprs are runnable on both sides now (walker evaluates them);
    only named host predicates make a matcher unrunnable."""
    pred_names = set()
    anchor = rec["anchor"]
    if anchor["kind"] == "pred":
        pred_names.add(anchor["pred"])
    if anchor["kind"] == "mixed":
        for br in anchor["branches"]:
            if "where" in br:
                pred_names.add(br["where"])
    for nid in _reachable(nodes, rec["root"]):
        n = nodes[nid]
        if n["kind"] == "pred":
            pred_names.add(n["pred"])
        if "where" in n:
            pred_names.add(n["where"])
    missing = pred_names - CPP_PREDS
    if missing:
        return "pred:" + ",".join(sorted(missing))
    if anchor["kind"] == "mixed" and pred_names:
        return "mixed_anchor_preds"
    return None


# --- dump ------------------------------------------------------------------
def block_key(b):
    op = getattr(b, "opcode", None)
    s = b.btype
    if op is not None:
        s += "#%08x" % (op & 0xFFFFFFFF)
    s += "@%d" % b.min_lt
    return s


def cap_val(v):
    if isinstance(v, list):
        return "[" + ",".join("null" if x is None else block_key(x) for x in v) + "]"
    return "null" if v is None else block_key(v)


def gather_blocks(root):
    out = []
    seen = set()
    queue = [root]
    while queue:
        cur = queue.pop(0)
        if id(cur) in seen:
            continue
        seen.add(id(cur))
        out.append(cur)
        for n in sorted(cur.next_blocks, key=lambda b: b.min_lt):
            queue.append(n)
    return out


def dump_fixture(path, runnable):
    try:
        with open(path, "rb") as f:
            data = msgpack.unpackb(lz4.frame.decompress(f.read()), raw=False)
        txs = [deserialize_transaction(t) for t in data["transactions"]]
        root_block = init_block(to_tree(txs))
    except Exception as e:  # noqa: BLE001
        print(f"ERROR: {type(e).__name__}: {e}")
        return
    for block in gather_blocks(root_block):
        for m in runnable:
            if not m.test_self(block):
                continue
            ctx = _MatchContext()
            if not m._match_node(m._root, block, ctx, absorb=False):
                continue
            caps = m._assemble_captures(ctx)
            consumed = sorted(block_key(cb) for cb in ctx.consumed)
            caps_s = ",".join(f"{k}={cap_val(v)}" for k, v in caps.items())
            print(f"MATCH {m.name} anchor={block_key(block)} "
                  f"consumed=[{','.join(consumed)}] captures={{{caps_s}}}")


def _stub_missing(art, reg):
    """Register no-op stubs for host names a synthetic artifact references
    (build_matchers validates them; the match phase never calls them)."""
    async def _noop(*_a, **_k):
        return None
    for m in art.matchers:
        if "builder" in m and m["builder"] not in reg.builders:
            reg.builders[m["builder"]] = _noop
        if "shape" in m and m["shape"] not in reg.shapers:
            reg.shapers[m["shape"]] = _noop


def main(artifact_path, corpus):
    art = load_artifact(artifact_path)
    reg = combined_registries()
    _stub_missing(art, reg)
    matchers = build_matchers(art, reg)

    # build_matchers order == registration_order stable-sorted by priority.
    order = sorted(art.registration_order,
                   key=lambda mi: art.matchers[mi].get("priority", 100))
    reasons = [skip_reason(art.matchers[mi], art.nodes) for mi in order]

    skips = sorted((matchers[i].name, reasons[i])
                   for i in range(len(matchers)) if reasons[i] is not None)
    for name, reason in skips:
        print(f"SKIP {name} reason={reason}")

    runnable = [matchers[i] for i in range(len(matchers)) if reasons[i] is None]

    if os.path.isdir(corpus):
        names = sorted(n for n in os.listdir(corpus) if n.endswith(".lz4"))
        for name in names:
            print(f"=== {name}")
            dump_fixture(os.path.join(corpus, name), runnable)
    else:
        print(f"=== {os.path.basename(corpus)}")
        dump_fixture(corpus, runnable)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
