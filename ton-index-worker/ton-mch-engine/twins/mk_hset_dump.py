# EM2 W7 dry-run input generator (em2-writeback-design.md §8 phase 1).
#
# Builds a SYNTHETIC "trace HSET dump" out of local corpus data, in exactly the
# shape a read-only prod sweep would produce, so ab_replay_diff.py can be
# exercised end-to-end without touching Redis. This is NOT the prod reader —
# the prod reader is a SCAN/HGETALL sweep the lead runs; this only manufactures
# its output format from the fixture twins.
#
# Payload fidelity: Python writes `actions` as gzip(msgpack([a.to_dict() ...]))
# (pendings.py:243-245, gzip level 1). This reproduces that byte-for-byte from
# the per-action blobs py_actions_msgpack.py already emits — same maps, same
# compression — so the diff tool sees what prod Redis holds, including the gzip
# framing its magic sniff has to handle.
#
#   python <this>/py_actions_msgpack.py <artifact> tests/traces > py.txt
#   python <this>/mk_hset_dump.py py.txt > hset.jsonl
#
# Output: JSONL, one object per trace:
#   {"trace_key": "<fixture or trace key>", "actions_b64": "<base64 of the raw
#    HSET value>", "n": <row count>}
# base64 because the value is binary; the diff tool gzip-sniffs it exactly as
# ton-index-go does (models.go:750-765).
import base64
import collections
import gzip
import json
import sys

import msgpack


def main(py_dump, out=sys.stdout, compress=True):
    per_trace = collections.OrderedDict()
    key = None
    with open(py_dump, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("=== "):
                key = line[4:]
                per_trace.setdefault(key, [])
            elif line.startswith("ACTION anchor="):
                _, rest = line.split("anchor=", 1)
                _anchor, blob = rest.rsplit(" ", 1)
                obj = msgpack.unpackb(base64.b64decode(blob), raw=False,
                                      strict_map_key=False)
                # The twin emits one map per line; Python's Redis value is the
                # ARRAY of them for the whole trace.
                per_trace[key].append(obj[0] if isinstance(obj, list) else obj)

    for trace_key, rows in per_trace.items():
        packed = msgpack.packb(rows)
        value = gzip.compress(packed, compresslevel=1) if compress else packed
        out.write(json.dumps({
            "trace_key": trace_key,
            "actions_b64": base64.b64encode(value).decode(),
            "n": len(rows),
        }) + "\n")
    print(f"# {len(per_trace)} traces, "
          f"{sum(len(v) for v in per_trace.values())} rows, "
          f"{'gzip' if compress else 'raw'}", file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv[1], compress="--raw" not in sys.argv)
