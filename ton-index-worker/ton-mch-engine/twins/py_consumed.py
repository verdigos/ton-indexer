# Python CONSUMPTION capture — the reference set for the collect-pass key-set
# gate (em2-tier2-design §10 W2).
#
# The fixture interface map is the WRONG reference: it is what Python's eager
# `address_selectors` PREDICTED it might need (every participant's interfaces),
# not what the matchers READ. This runs the real Python classify path (py_classify's
# own loop, unmodified) with `TestInterfaceRepository` instrumented, so what comes
# out is exactly the set of interface reads Python's matchers performed.
#
# The load-bearing claim it tests: the C++ collect pass demands a SUPERSET of
# what Python actually consumed, which is what "do not port address_selectors"
# rests on.
#
# Resolver-internal reads are suppressed, not recorded: `extract_jvault_assets`
# chains stake wallet -> data_boc -> pool data_boc -> lock wallet's jetton_wallet,
# and the C++ side performs that chain INSIDE the tier-2 resolver behind ONE
# `jvault_assets` key (§6.2/D5). Recording its inner reads would demand keys the
# engine can never ask for. So the wrapper records `jvault_assets(stake_wallet)`
# and silences everything the chain reads underneath it.
#
# Usage (from twins/, with the indexer venv python):
#   py_consumed.py <artifact.json> <corpus-dir>                    # dump
#   py_consumed.py <artifact.json> <corpus-dir> --compare <lookup_keys.txt>
#
# `--compare` takes the C++ `--lookup-keys` output and prints the containment
# both directions, per kind. Exit 1 if any consumed key was never demanded.
import asyncio
import contextlib
import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import py_classify as twin  # noqa: E402  (sets up the indexer sys.path itself)

from indexer.events.mch.builders import jvault as mch_jvault  # noqa: E402
from indexer.events.mch_ir import build_matchers, load_artifact  # noqa: E402
from tests.utils.repository import TestInterfaceRepository  # noqa: E402
from twin_registries import combined_registries  # noqa: E402

# (kind, address) reads of the fixture currently being classified.
_reads: list[tuple[str, str]] = []
# >0 while inside a resolver chain the C++ tier-2 resolver runs internally.
_internal = 0


def _record(kind: str, address) -> None:
    if _internal == 0 and isinstance(address, str):
        _reads.append((kind, address))


def _instrument_repository() -> None:
    # One kind per getter, matching ParsedBlockLookupSource's kind names. The
    # generic `get_interfaces` keeps its own bucket: it backs the nominator_pool
    # lookup but also non-lookup checks (auction.py, staking.py), so collapsing
    # it into a lookup kind here would invent demand that never existed.
    getters = {
        "get_jetton_wallet": "jetton_wallet",
        "get_nft_item": "nft_item",
        "get_nft_sale": "nft_sale",
        "get_nft_auction": "nft_auction",
        "get_dedust_pool": "dedust_pool",
        "get_multisig_order": "multisig_order",
        "get_interfaces": "interfaces",
    }
    for name, kind in getters.items():
        original = getattr(TestInterfaceRepository, name)

        def make(original=original, kind=kind):
            async def patched(self, address, *args, **kwargs):
                _record(kind, address)
                return await original(self, address, *args, **kwargs)
            return patched

        setattr(TestInterfaceRepository, name, make())

    original_extra = TestInterfaceRepository.get_extra_data

    async def patched_extra(self, address, request, *args, **kwargs):
        _record("extra_data:" + str(request), address)
        return await original_extra(self, address, request, *args, **kwargs)

    TestInterfaceRepository.get_extra_data = patched_extra

    # The jvault chain: one recorded key, its internals silenced (see header).
    original_jvault = mch_jvault.extract_jvault_assets

    async def patched_jvault(stake_wallet, *args, **kwargs):
        global _internal
        _record("jvault_assets", stake_wallet)
        _internal += 1
        try:
            return await original_jvault(stake_wallet, *args, **kwargs)
        finally:
            _internal -= 1

    mch_jvault.extract_jvault_assets = patched_jvault


def _norm(address: str) -> str:
    return address.strip().upper()


def capture(artifact_path: str, corpus: str) -> dict[str, set[tuple[str, str]]]:
    _instrument_repository()
    art = load_artifact(artifact_path)
    matchers = build_matchers(art, combined_registries())
    order = sorted(art.registration_order,
                   key=lambda mi: art.matchers[mi].get("priority", 100))
    reasons = [twin.skip_reason(art.matchers[mi], art.nodes) for mi in order]
    included = [matchers[i] for i in range(len(matchers)) if reasons[i] is None]

    names = ([corpus] if not os.path.isdir(corpus) else
             [os.path.join(corpus, n) for n in sorted(os.listdir(corpus)) if n.endswith(".lz4")])
    out: dict[str, set[tuple[str, str]]] = {}
    for path in names:
        global _reads
        _reads = []
        # py_classify's loop writes its dump to stdout; only the reads matter here.
        with contextlib.redirect_stdout(io.StringIO()):
            asyncio.run(twin.classify_fixture(path, included))
        out[os.path.basename(path)] = {(k, _norm(a)) for k, a in _reads}
    return out


def parse_cpp_demand(path: str) -> dict[str, set[tuple[str, str]]]:
    """REQ lines of `ton-mch-engine --lookup-keys`, per fixture."""
    out: dict[str, set[tuple[str, str]]] = {}
    fixture = None
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "===":
                fixture = parts[1]
                out.setdefault(fixture, set())
            elif parts[0] == "REQ" and fixture is not None and len(parts) >= 3:
                out[fixture].add((parts[1], _norm(parts[2])))
    return out


def main(argv: list[str]) -> int:
    artifact, corpus = argv[1], argv[2]
    compare_path = None
    if len(argv) > 4 and argv[3] == "--compare":
        compare_path = argv[4]

    consumed = capture(artifact, corpus)
    for fixture in sorted(consumed):
        print(f"=== {fixture}")
        for kind, address in sorted(consumed[fixture]):
            print(f"CONSUMED {kind} {address}")
    kinds: dict[str, int] = {}
    for keys in consumed.values():
        for kind, _ in keys:
            kinds[kind] = kinds.get(kind, 0) + 1
    print("SUMMARY consumed=%d consumed_by_kind{%s}" % (
        sum(len(v) for v in consumed.values()),
        ",".join(f"{k}={n}" for k, n in sorted(kinds.items()))))
    if compare_path is None:
        return 0

    demand = parse_cpp_demand(compare_path)
    gaps: dict[str, int] = {}
    extra: dict[str, int] = {}
    n_gap = n_extra = 0
    for fixture in sorted(consumed):
        want = demand.get(fixture, set())
        for key in sorted(consumed[fixture] - want):
            print(f"GAP {fixture} {key[0]} {key[1]}")
            gaps[key[0]] = gaps.get(key[0], 0) + 1
            n_gap += 1
        for key in sorted(want - consumed[fixture]):
            print(f"EXTRA {fixture} {key[0]} {key[1]}")
            extra[key[0]] = extra.get(key[0], 0) + 1
            n_extra += 1
    print("SUMMARY gap=%d gap_by_kind{%s}" % (
        n_gap, ",".join(f"{k}={n}" for k, n in sorted(gaps.items()))))
    print("SUMMARY extra=%d extra_by_kind{%s}" % (
        n_extra, ",".join(f"{k}={n}" for k, n in sorted(extra.items()))))
    return 1 if n_gap else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
