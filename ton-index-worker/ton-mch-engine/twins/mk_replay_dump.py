# EM2 W7 dry-run input generator, EXTENDED form (em2-writeback-design.md §8
# phase 1; the "READ BEFORE SWEEPING" format in README.md).
#
# mk_hset_dump.py manufactures the two-field record ab_replay_diff.py needs
# (trace_key + Python's `actions` value). That is enough for the COMPARATOR and
# not enough for the REPLAY ADAPTER, which also needs the trace's own data —
# root_node, the node blobs and the interface blobs — none of which is derivable
# from `actions`. This script joins the two halves into the single-pass record a
# prod sweep produces, so mch-ab-replay and ab_replay_diff.py can both be
# exercised offline against the fixture corpus.
#
# The trace half comes from the C++ side on purpose: the node/interface blobs are
# msgpack of ton-trace-emulator's OWN structs (Serializer.hpp), so they are packed
# by `mch-ab-replay --make-dump`, not re-implemented here. This script never
# touches binary formats — it only merges JSON objects.
#
#   mch-ab-replay --make-dump -T tests/traces -o nodes.jsonl
#   python <this>/py_actions_msgpack.py <artifact> tests/traces > py.txt
#   python <this>/mk_replay_dump.py --nodes nodes.jsonl --py py.txt > dump.jsonl
#
# `--raw` mirrors mk_hset_dump.py: gzip (what prod holds) by default, raw for the
# uncompressed path our own writer ships (§2.8).
#
# NOT the prod reader. The prod reader is a read-only SCAN/HGETALL sweep; this
# only manufactures its output shape from local corpus data.
import argparse
import io
import json
import sys

import mk_hset_dump


def main(argv=None):
    ap = argparse.ArgumentParser(description="EM2 phase-1 extended dry-run dump (offline)")
    ap.add_argument("--nodes", required=True,
                    help="`mch-ab-replay --make-dump` output (trace/node/interface half)")
    ap.add_argument("--py", required=True,
                    help="`py_actions_msgpack.py` output (Python's actions half)")
    ap.add_argument("-o", "--out", help="write here instead of stdout")
    ap.add_argument("--raw", action="store_true",
                    help="store `actions` uncompressed instead of gzip")
    a = ap.parse_args(argv)

    # Reuse mk_hset_dump for the payload itself — same maps, same gzip level, so
    # the two generators can never disagree about what Python's Redis value is.
    buf = io.StringIO()
    mk_hset_dump.main(a.py, out=buf, compress=not a.raw)
    py_by_key = {}
    for line in buf.getvalue().splitlines():
        rec = json.loads(line)
        py_by_key[rec["trace_key"]] = rec

    out = open(a.out, "w", encoding="utf-8") if a.out else sys.stdout
    merged = 0
    no_actions = []
    seen = set()
    with open(a.nodes, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rec = json.loads(line)
            key = rec["trace_key"]
            seen.add(key)
            py = py_by_key.get(key)
            if py is None:
                # A trace Python classified to nothing at all still gets a record:
                # dropping it would silently shrink the diff's denominator.
                no_actions.append(key)
                rec["actions_b64"], rec["n"] = "", 0
            else:
                rec["actions_b64"], rec["n"] = py["actions_b64"], py["n"]
            out.write(json.dumps(rec) + "\n")
            merged += 1
    if a.out:
        out.close()

    missing_trace = sorted(set(py_by_key) - seen)
    print(f"# {merged} traces merged, {len(no_actions)} with no Python actions, "
          f"{'gzip' if not a.raw else 'raw'}", file=sys.stderr)
    if missing_trace:
        # Python has a trace the node dump does not: the replay side cannot
        # produce rows for it, so every row would land in py-only-row. Loud.
        print(f"# WARNING {len(missing_trace)} traces in --py have no node data: "
              f"{missing_trace[:5]}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
