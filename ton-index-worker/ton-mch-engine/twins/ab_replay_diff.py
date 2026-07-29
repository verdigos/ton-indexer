# EM2 W7 phase-1 A/B replay diff (em2-writeback-design.md §8).
#
# Compares the `actions` payload PYTHON wrote (read out of a trace HSET dump)
# against the payload OUR writer produces for the same traces, and buckets every
# difference by the §8 taxonomy. Decoded-map comparison throughout — byte
# equality is neither achievable nor required (§2.7 / §11).
#
#   python <this>/ab_replay_diff.py --hset hset.jsonl --cpp cpp.txt \
#          --ir ../ir/mch_ir.json --fills ../src/ActionBuild.cpp
#
# Exit 0 iff the FATAL buckets are empty: real-mismatch, cpp-only-row,
# py-only-row. Everything else is a named, counted, expected divergence — a
# bucket that stops firing is itself a signal, so none of them are silent.
#
# NO NETWORK, NO REDIS. Input is a file. Producing that file from prod is a
# separate read-only sweep; mk_hset_dump.py manufactures an equivalent one from
# the fixture corpus so this tool can be dry-run offline.
import argparse
import base64
import collections
import gzip
import json
import re
import sys

import msgpack

try:
    from pytoniq_core import Cell
except ImportError:  # noqa: BLE001 — only the boc-framing bucket needs it
    Cell = None

# --- §2.3 / §2.6 / W2 known divergences (shared with cmp_actions_msgpack.py) --
EXPECTED_OMISSIONS = {
    "trace_id", "trace_external_hash", "trace_end_lt", "trace_end_utime",
    "trace_mc_seqno_end", "value_extra_currencies", "trace_start_lt",
}
WIRE_TYPE_DIVERGENCE = {
    "jvault_stake_data": {"period"},
    "layerzero_packet_data": {"src_eid", "dst_eid", "nonce"},
}
ORDER_INSENSITIVE = {"tx_hashes", "accounts"}

# §8: Python rows that mch structurally does not emit. `ancestor_type` /
# `parent_action_id` mark a CHILD action — serialize_blocks' recursion is out of
# scope for mch (§2.5), so these are expected-absent, not coverage loss.
CHILD_MARKERS = ("ancestor_type", "parent_action_id")


def load_hset(path):
    """-> {trace_key: [action maps]} from the Redis-shaped dump."""
    out = collections.OrderedDict()
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rec = json.loads(line)
            raw = base64.b64decode(rec["actions_b64"])
            # Same sniff ton-index-go does (models.go:750-765): gzip magic, else
            # pass through. Our own writer ships uncompressed (§2.8 / Q2).
            if raw[:2] == b"\x1f\x8b":
                raw = gzip.decompress(raw)
            out[rec["trace_key"]] = msgpack.unpackb(raw, raw=False, strict_map_key=False)
    return out


def load_cpp(path):
    """-> {trace_key: [action maps]} from `ton-mch-engine --actions-msgpack`."""
    out, key = collections.OrderedDict(), None
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("=== "):
                key = line[4:]
                out.setdefault(key, [])
            elif line.startswith("ACTION anchor=") or line.startswith("FALLBACK anchor="):
                _, rest = line.split("anchor=", 1)
                _anchor, blob = rest.rsplit(" ", 1)
                obj = msgpack.unpackb(base64.b64decode(blob), raw=False,
                                      strict_map_key=False)
                out[key].append(obj[0] if isinstance(obj, list) else obj)
    return out


def ir_btypes(ir_path):
    """Btypes some matcher in the IR can produce — the bucket B/C boundary."""
    d = json.load(open(ir_path, encoding="utf-8"))
    seen = set()

    def walk(e):
        if isinstance(e, dict):
            if "btype" in e and isinstance(e["btype"], str):
                seen.add(e["btype"])
            for v in e.values():
                walk(v)
        elif isinstance(e, list):
            for it in e:
                walk(it)

    for rec in d["matchers"]:
        seen.update(rec.get("produces", []) or [])
        if rec.get("outcome"):
            seen.add(rec["outcome"])
        walk(rec.get("build_program", []))
    return seen


def cpp_fill_btypes(action_build_cpp):
    """Action TYPES our fills can produce, parsed from ActionBuild.cpp.

    NOT just the dispatch btypes. Several fills REWRITE `type` — a
    nominator/tonstakers row goes out as stake_withdrawal(_request), a tgbtc row
    can gain a _fallback suffix — and Python puts that rewritten name on the
    wire. Matching a py-only row against btypes alone therefore misfiles a real
    staking coverage failure as `protocol-not-in-mch`, i.e. into an
    expected-and-ignored bucket. Caught by --selftest.

    Single-source on purpose: a hand-kept copy of this list is exactly the thing
    that goes stale. The count assertion makes a parse break loud rather than
    silently emptying the set.
    """
    src = open(action_build_cpp, encoding="utf-8").read()
    body = src[src.index("bool fill_action("):]
    body = body[:body.index("\n}")]
    btypes = set(re.findall(r't == "([a-z0-9_]+)"', body))
    if len(btypes) < 15:
        raise SystemExit(f"fill_action parse looks wrong: only {len(btypes)} btypes")
    assigned = set(re.findall(r'a\.type\s*=\s*"([a-z0-9_]+)"', src))
    appended = set(re.findall(r'a\.type\s*\+=\s*"([a-z0-9_]+)"', src))
    return btypes | assigned | {t + s for t in assigned for s in appended}


def cell_hash(v):
    if Cell is None:
        return None
    try:
        raw = v if isinstance(v, (bytes, bytearray)) else base64.b64decode(v, validate=True)
        return Cell.one_from_boc(bytes(raw)).hash.hex().upper()
    except Exception:  # noqa: BLE001
        return None


def compare_value(path, cpp, py, buckets):
    key = path.rsplit(".", 1)[-1]
    composite = path.split(".", 1)[0]

    if isinstance(cpp, dict) and isinstance(py, dict):
        for k in sorted(set(cpp) | set(py)):
            sub = f"{path}.{k}" if path else k
            if k not in cpp:
                buckets["real-mismatch"].append((sub, "MISSING in cpp", repr(py[k])[:110]))
            elif k not in py:
                buckets["real-mismatch"].append((sub, repr(cpp[k])[:110], "MISSING in py"))
            else:
                compare_value(sub, cpp[k], py[k], buckets)
        return
    if isinstance(cpp, list) and isinstance(py, list):
        if key in ORDER_INSENSITIVE:
            cpp, py = sorted(cpp), sorted(py)
        if len(cpp) != len(py):
            buckets["real-mismatch"].append((path, f"len {len(cpp)}", f"len {len(py)}"))
            return
        for i, (a, b) in enumerate(zip(cpp, py)):
            compare_value(f"{path}[{i}]", a, b, buckets)
        return
    if cpp == py and type(cpp) is type(py):
        return

    if key in WIRE_TYPE_DIVERGENCE.get(composite, ()):
        if isinstance(cpp, int) and isinstance(py, str) and str(cpp) == py:
            buckets["wire-type-int-fields"].append((path, cpp, py))
            return
    if isinstance(cpp, (str, bytes)) and isinstance(py, (str, bytes)):
        hc = cell_hash(cpp)
        if hc is not None and hc == cell_hash(py):
            buckets["boc-framing"].append((path, hc, f"{len(cpp)}B vs {len(py)}B"))
            return
    buckets["real-mismatch"].append((path, repr(cpp)[:110], repr(py)[:110]))


def compare_row(tag, cpp, py, buckets):
    for k in sorted(set(cpp) | set(py)):
        if k not in cpp:
            if k in EXPECTED_OMISSIONS:
                buckets["expected-omission"].append((f"{tag}.{k}", "", repr(py[k])[:50]))
            elif py[k] is None:
                buckets["top-level-null"].append((f"{tag}.{k}", "", "None"))
            else:
                buckets["real-mismatch"].append(
                    (f"{tag}.{k}", "MISSING in cpp", repr(py[k])[:110]))
        elif k not in py:
            buckets["real-mismatch"].append((f"{tag}.{k}", repr(cpp[k])[:110], "MISSING in py"))
        else:
            compare_value(k, cpp[k], py[k], buckets)


def classify_py_only(row, matcher_btypes, fill_btypes):
    """Why does Python have a row we don't? The §8 coverage taxonomy."""
    if any(row.get(m) for m in CHILD_MARKERS):
        return "child-action"
    t = row.get("type") or "?"
    # `type` is the ACTION type, which a fill may rewrite (stake_withdrawal and
    # friends), so a hit on either side of the rename counts as covered.
    if t in fill_btypes:
        return "real-mismatch"
    if t in matcher_btypes:
        return "matcher-runs-fill-missing"
    return "protocol-not-in-mch"


def selftest(ir_path, fills_path):
    """Prove the COVERAGE taxonomy fires, since the corpus cannot.

    matcher-runs-fill-missing / protocol-not-in-mch / child-action all read zero
    on the 165 fixtures — every row there is a ported btype. Those three are
    exactly the buckets phase 1 exists to populate, so leaving them unexercised
    would mean shipping a classifier nobody has ever seen classify. Each case
    below is a row shape phase 1 will really meet.
    """
    matcher_btypes, fill_btypes = ir_btypes(ir_path), cpp_fill_btypes(fills_path)
    cases = [
        # A matcher produces it, no fill_action branch -> the actionable bucket.
        ({"type": "change_dns"}, "matcher-runs-fill-missing"),
        # No matcher anywhere in the IR -> EM3 work, not a defect.
        ({"type": "multisig_create_order"}, "protocol-not-in-mch"),
        # serialize_blocks' child recursion is out of scope for mch (§2.5).
        ({"type": "jetton_transfer", "ancestor_type": ["jetton_swap"]}, "child-action"),
        ({"type": "jetton_transfer", "parent_action_id": "abc"}, "child-action"),
        # Ported btype, no child marker: we should have emitted it.
        ({"type": "jetton_transfer"}, "real-mismatch"),
        # W6 additions must now read as covered, not as a coverage gap.
        ({"type": "nft_purchase"}, "real-mismatch"),
        ({"type": "dex_withdraw_liquidity"}, "real-mismatch"),
    ]
    fails = 0
    for row, want in cases:
        got = classify_py_only(row, matcher_btypes, fill_btypes)
        ok = got == want
        fails += not ok
        print(f"{'PASS' if ok else 'FAIL'} selftest/{row.get('type')}"
              f"{'+child' if any(row.get(m) for m in CHILD_MARKERS) else ''}"
              f" -> {got}" + ("" if ok else f" (want {want})"))
    # The renamed-type case: a fill can rewrite `type`, so the covered check has
    # to hold for the REWRITTEN name too or every staking row looks uncovered.
    for t in ("stake_withdrawal", "stake_withdrawal_request"):
        got = classify_py_only({"type": t}, matcher_btypes, fill_btypes)
        ok = got != "protocol-not-in-mch"
        fails += not ok
        print(f"{'PASS' if ok else 'FAIL'} selftest/renamed:{t} -> {got}")
    print(f"AB-SELFTEST {'ALL PASS' if fails == 0 else 'FAILURES'}")
    return 0 if fails == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="EM2 phase-1 A/B replay diff (offline)")
    ap.add_argument("--hset", help="trace HSET dump (JSONL) = Python's side")
    ap.add_argument("--cpp", help="ton-mch-engine --actions-msgpack output")
    ap.add_argument("--ir", required=True, help="ir/mch_ir.json")
    ap.add_argument("--fills", required=True, help="src/ActionBuild.cpp")
    ap.add_argument("--show", type=int, default=25, help="entries per fatal bucket")
    ap.add_argument("--selftest", action="store_true",
                    help="exercise the coverage taxonomy (no dumps needed)")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest(a.ir, a.fills)
    if not a.hset or not a.cpp:
        ap.error("--hset and --cpp are required unless --selftest")

    py_all, cpp_all = load_hset(a.hset), load_cpp(a.cpp)
    matcher_btypes, fill_btypes = ir_btypes(a.ir), cpp_fill_btypes(a.fills)
    buckets = collections.defaultdict(list)
    rows = 0

    for key in sorted(set(py_all) | set(cpp_all)):
        py_rows, cpp_rows = py_all.get(key, []), cpp_all.get(key, [])
        py_by = {r.get("action_id"): r for r in py_rows}
        cpp_by = {r.get("action_id"): r for r in cpp_rows}
        for aid in sorted(set(py_by) | set(cpp_by), key=repr):
            if aid not in cpp_by:
                row = py_by[aid]
                buckets[classify_py_only(row, matcher_btypes, fill_btypes)].append(
                    (f"{key}/{row.get('type')}", "", f"action_id={aid}"))
                if classify_py_only(row, matcher_btypes, fill_btypes) == "real-mismatch":
                    buckets["py-only-row"].append((f"{key}/{row.get('type')}", "", aid))
            elif aid not in py_by:
                buckets["cpp-only-row"].append(
                    (f"{key}/{cpp_by[aid].get('type')}", aid, ""))
            else:
                rows += 1
                compare_row(f"{key}/{aid}", cpp_by[aid], py_by[aid], buckets)

    print(f"traces={len(set(py_all) | set(cpp_all))} rows_compared={rows}")
    fatal_names = ["real-mismatch", "cpp-only-row", "py-only-row"]
    expected = ["matcher-runs-fill-missing", "protocol-not-in-mch", "child-action",
                "wire-type-int-fields", "boc-framing", "top-level-null",
                "expected-omission"]
    for name in fatal_names:
        e = buckets.get(name, [])
        print(f"\n== {name}: {len(e)}   [FATAL]")
        for x in e[:a.show]:
            print(f"   {x[0]}\n      cpp: {x[1]}\n      py : {x[2]}")
        if len(e) > a.show:
            print(f"   ... {len(e) - a.show} more")
    for name in expected:
        e = buckets.get(name, [])
        print(f"\n== {name}: {len(e)}")
        per = collections.Counter(x[0].rsplit(".", 1)[-1] if name in
                                  ("wire-type-int-fields", "boc-framing", "top-level-null",
                                   "expected-omission") else x[0].split("/")[-1] for x in e)
        for k, n in sorted(per.items()):
            print(f"   {n:6d}  {k}")

    fatal = sum(len(buckets.get(n, [])) for n in fatal_names)
    print(f"\nA/B {'PASS' if fatal == 0 else 'FAIL'} (fatal={fatal})")
    return 0 if fatal == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
