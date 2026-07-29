# Python twin of ton-mch-engine's `--actions-msgpack` (EM2 W2, the write-back
# parity gate; em2-writeback-design.md §9).
#
# This is py_actions.py's pipeline with ONE substitution: instead of rendering
# the Action into the C++ text-dump column set, it emits what pendings.py puts
# in Redis — msgpack(Action.to_dict()) — base64'd so it survives a text dump.
# The pipeline itself (matcher loop, post-process passes, the REAL
# block_tree_serializer.block_to_action) is imported, not copied, so the two
# gates can never drift apart on anything except the per-row render.
#
# Invocation (from indexer/, with the venv python):
#   python <this>/py_actions_msgpack.py <artifact> tests/traces
# The C++ side is `ton-mch-engine --actions-msgpack -I <artifact> -T tests/traces`.
# Neither side is a diff of the other's TEXT: run both through
# cmp_actions_msgpack.py, which compares DECODED MAPS (§2.7).
import base64
import sys
import types

import msgpack

import py_actions

# The stub emission. `finality` and `trace_external_hash_norm` are the two wire
# keys that come from the EMISSION rather than the Action (§2.5), and a fixture
# is not an emission — so both sides use the same sentinels (C++: twin_view() in
# Classify.cpp) and the comparator asserts them like any other key.
#
# finality=2 is the emulator's `finalized`, matching the C++ stub view's single
# finalized node. py_actions.py's own stub uses None, which is fine there
# because its dump renders no finality column; here the key is on the wire.
_STUB_TRACE = types.SimpleNamespace(
    finality=2, end_lt=0, end_utime=0, external_hash=None,
    external_hash_norm="twin", mc_seqno_end=0,
)


def msgpack_render(action):
    """Action -> pendings.py's per-action wire bytes.

    to_dict() is where convert_numerics_to_strings runs (§2.2), so this is the
    exact map ton-index-go decodes today — including the §2.6 fields Python
    stringifies and Go rejects, which the comparator buckets rather than hides.
    """
    # pendings.py:239 — applied to EVERY action on the emulated path, including
    # W4's synthetic `unknown` row, which create_unknown_action does not set it
    # on. Without this the unknown row would ship without the one key
    # ton-index-go needs to surface a normalized external hash (§2.3).
    action.trace_external_hash_norm = _STUB_TRACE.external_hash_norm
    return base64.b64encode(msgpack.packb(action.to_dict())).decode()


# The substitution. render_action is the ONE render seam every row kind funnels
# through, so this swaps the matched rows AND W4's synthetic `unknown` row in a
# single rebinding. _STUB_TRACE is rebound too: py_actions reads it at call time
# for both block_to_action's trace and the unknown row's finality.
py_actions.render_action = msgpack_render
py_actions._STUB_TRACE = _STUB_TRACE


if __name__ == "__main__":
    py_actions.main(sys.argv[1], sys.argv[2])
