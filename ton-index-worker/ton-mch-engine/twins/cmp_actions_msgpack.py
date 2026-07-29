# EM2 W2 comparator: the write-back parity gate (em2-writeback-design.md §8, §9).
#
# Diffs the DECODED MAPS of `ton-mch-engine --actions-msgpack` against
# `twins/py_actions_msgpack.py`. Byte equality is explicitly NOT the bar (§2.7 /
# §11): Python's msgpack map order is __dict__ insertion order and the Go
# decoder is order-insensitive, so the contract is per-key equality of what the
# consumer actually sees.
#
#   python <this>/cmp_actions_msgpack.py <cpp_out> <py_out>
#
# Exit 0 iff real-mismatch == 0 and no row is missing on either side. Every
# tolerated difference is BUCKETED AND COUNTED, never skipped — a bucket that
# stops firing is itself a signal.
import base64
import collections
import sys

import msgpack

try:
    from pytoniq_core import Cell
except ImportError:  # noqa: BLE001 — only the boc-framing bucket needs it
    Cell = None

# §2.3: written by Python, deliberately NOT written by us. Go clobbers the first
# five right after decode (models.go:830-836) and has no field for the last two,
# so their absence cannot reach a consumer.
EXPECTED_OMISSIONS = {
    "trace_id", "trace_external_hash", "trace_end_lt", "trace_end_utime",
    "trace_mc_seqno_end", "value_extra_currencies", "trace_start_lt",
}

# §2.6 / Q1: ton-index-go declares these non-string, Python stringifies them
# (its exclusion set is top-level only) and the type error drops the WHOLE
# trace. We emit integers on purpose. Keyed by composite -> field.
WIRE_TYPE_DIVERGENCE = {
    "jvault_stake_data": {"period"},
    "layerzero_packet_data": {"src_eid", "dst_eid", "nonce"},
}

# Order is not part of the contract for these — C++ builds them from a
# std::set, Python from a Python set, and no consumer compares order
# (§2.3). Compared as sorted lists; membership differences still fail.
ORDER_INSENSITIVE = {"tx_hashes", "accounts"}


def parse_dump(path):
    """-> {fixture: [(anchor, decoded_map)]}, plus the fixtures that failed."""
    out, failures = collections.OrderedDict(), []
    fixture = None
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("=== "):
                fixture = line[4:]
                out.setdefault(fixture, [])
            elif line.startswith("ACTION anchor=") or line.startswith("FALLBACK anchor="):
                _, rest = line.split("anchor=", 1)
                anchor, blob = rest.rsplit(" ", 1)
                obj = msgpack.unpackb(base64.b64decode(blob), raw=False, strict_map_key=False)
                # The C++ side ships the production ARRAY-of-actions payload with
                # exactly one row; the Python side ships the bare map.
                if isinstance(obj, list):
                    assert len(obj) == 1, f"{fixture}/{anchor}: array of {len(obj)}, want 1"
                    obj = obj[0]
                out[fixture].append((anchor, obj))
            elif line in ("TRACE_FAIL",) or line.startswith("ERROR:"):
                failures.append((fixture, line))
    return out, failures


def cell_hash(v):
    """Root hash of a BOC-carrying value, or None if it is not a decodable BOC."""
    if Cell is None:
        return None
    try:
        raw = v if isinstance(v, (bytes, bytearray)) else base64.b64decode(v, validate=True)
        return Cell.one_from_boc(bytes(raw)).hash.hex().upper()
    except Exception:  # noqa: BLE001
        return None


def compare_value(path, cpp, py, buckets):
    """Recursive per-key compare. `path` is the dotted key path for reporting."""
    key = path.rsplit(".", 1)[-1]
    composite = path.split(".", 1)[0]

    if isinstance(cpp, dict) and isinstance(py, dict):
        for k in sorted(set(cpp) | set(py)):
            sub = f"{path}.{k}" if path else k
            if k not in cpp:
                # Inside a composite, absence is NOT a tolerated state: the
                # writer keeps every key (§2.7's two-level rule), so a missing
                # one is a real fill gap.
                buckets["real-mismatch"].append((sub, "MISSING in cpp", repr(py[k])[:120]))
            elif k not in py:
                buckets["real-mismatch"].append((sub, "MISSING in py", repr(cpp[k])[:120]))
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

    # --- tolerated difference buckets -------------------------------------
    # §2.6/Q1: we emit an integer where Python emits its decimal string.
    if key in WIRE_TYPE_DIVERGENCE.get(composite, ()):
        if isinstance(cpp, int) and isinstance(py, str) and str(cpp) == py:
            buckets["wire-type-divergence"].append((path, cpp, py))
            return
        buckets["real-mismatch"].append((path, repr(cpp)[:120], repr(py)[:120]))
        return

    # Same cell, different BOC framing (td writes no CRC, pytoniq does). Proven
    # by root hash, which is the §12 parity bar the text twins already use.
    if isinstance(cpp, (str, bytes)) and isinstance(py, (str, bytes)):
        hc, hp = cell_hash(cpp), cell_hash(py)
        if hc is not None and hc == hp:
            buckets["boc-framing"].append((path, hc, f"{len(cpp)}B cpp vs {len(py)}B py"))
            return

    buckets["real-mismatch"].append((path, repr(cpp)[:120], repr(py)[:120]))


def compare_row(fixture, anchor, cpp, py, buckets):
    for k in sorted(set(cpp) | set(py)):
        if k not in cpp:
            if k in EXPECTED_OMISSIONS:
                buckets["expected-omission"].append((f"{anchor}.{k}", "", repr(py[k])[:60]))
            elif py[k] is None:
                # TOP LEVEL ONLY. Python's _fill_* assigns some columns an
                # explicit None (e.g. `action.asset = None` for a TON asset),
                # which a fixed-schema C++ Action cannot distinguish from
                # "never assigned" — both are a Null Value. §2.1 sanctions
                # omitting either ("EM2 may omit any field it cannot fill"),
                # and every field this fires on is a POINTER in Go
                # (models.go:530-601), so absent and nil decode identically.
                # Counted, not waived: a key appearing here that Python fills
                # with a real value would land in real-mismatch instead.
                buckets["top-level-null"].append((f"{anchor}.{k}", "", "None"))
            else:
                buckets["real-mismatch"].append(
                    (f"{fixture}/{anchor}.{k}", "MISSING in cpp", repr(py[k])[:120]))
        elif k not in py:
            buckets["real-mismatch"].append(
                (f"{fixture}/{anchor}.{k}", repr(cpp[k])[:120], "MISSING in py"))
        else:
            compare_value(k, cpp[k], py[k], buckets)


def main(cpp_path, py_path):
    cpp_all, cpp_fail = parse_dump(cpp_path)
    py_all, py_fail = parse_dump(py_path)
    buckets = collections.defaultdict(list)
    rows = fixtures = 0

    if cpp_fail or py_fail:
        print(f"trace failures: cpp={cpp_fail} py={py_fail}")

    for fixture in sorted(set(cpp_all) | set(py_all)):
        fixtures += 1
        # Join on (anchor, action_id): the id is a content hash computed by the
        # same algorithm on both sides (§2.3), so it pairs rows even when one
        # anchor produces several.
        def index(rowlist):
            m = collections.defaultdict(list)
            for anchor, obj in rowlist:
                m[(anchor, obj.get("action_id"))].append(obj)
            return m

        ci, pi = index(cpp_all.get(fixture, [])), index(py_all.get(fixture, []))
        for key in sorted(set(ci) | set(pi), key=repr):
            anchor = key[0]
            c_rows, p_rows = ci.get(key, []), pi.get(key, [])
            for extra in c_rows[len(p_rows):]:
                buckets["cpp-only-row"].append((f"{fixture}/{anchor}", extra.get("type"), ""))
            for extra in p_rows[len(c_rows):]:
                buckets["py-only-row"].append((f"{fixture}/{anchor}", "", extra.get("type")))
            for c, p in zip(c_rows, p_rows):
                rows += 1
                compare_row(fixture, anchor, c, p, buckets)

    print(f"fixtures={fixtures} rows_compared={rows}")
    order = ["real-mismatch", "cpp-only-row", "py-only-row", "wire-type-divergence",
             "boc-framing", "top-level-null", "expected-omission"]
    for name in order:
        entries = buckets.get(name, [])
        print(f"\n== {name}: {len(entries)}")
        if name != "real-mismatch" and not name.endswith("-row"):
            # Tolerated: summarize by the FIELD (drop fixture/anchor) so a
            # bucket that stops firing is visible at a glance.
            per_key = collections.Counter(e[0].rsplit(".", 1)[-1] for e in entries)
            for k, n in sorted(per_key.items()):
                print(f"   {n:6d}  {k}")
        else:
            for e in entries[:40]:
                print(f"   {e[0]}\n      cpp: {e[1]}\n      py : {e[2]}")
            if len(entries) > 40:
                print(f"   ... {len(entries) - 40} more")

    fatal = len(buckets["real-mismatch"]) + len(buckets["cpp-only-row"]) + len(buckets["py-only-row"])
    print(f"\nGATE {'PASS' if fatal == 0 else 'FAIL'} (fatal={fatal})")
    return 0 if fatal == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
