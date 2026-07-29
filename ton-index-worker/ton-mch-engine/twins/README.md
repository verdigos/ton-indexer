# Python A/B twins for ton-mch-engine

Byte-exact Python counterparts of the engine's dump modes, used by the gate set in
`indexer/docs/mch-cpp-plan.md` §1. They import the REAL Python classifier
(`indexer.events.mch_ir` engine, `block_tree_serializer`, post-process passes), so they run
from `S:\Dev\ton-indexer-clear\indexer` with the venv python
(`S:\Dev\H-dump\tonscan-indexator\venv0\Scripts\python.exe`).

| Twin | Engine mode | Invocation (from indexer/) |
|---|---|---|
| py_msg_dump.py | `--parse-dump -T <dir>` | `python twins/py_msg_dump.py tests/traces` * |
| py_match_dump.py | `--match -I <artifact> -T <dir>` | `python <this>/py_match_dump.py <artifact> tests/traces` |
| py_classify.py | `--classify -I <artifact> -T <dir>` | `python <this>/py_classify.py <artifact> tests/traces` |
| py_actions.py | `--actions -I <artifact> -T <dir>` | `python <this>/py_actions.py <artifact> tests/traces` |
| py_actions_msgpack.py | `--actions-msgpack -I <artifact> -T <dir>` | `python <this>/py_actions_msgpack.py <artifact> tests/traces` ** |

(* py_msg_dump takes just the traces dir; the others take the artifact path first.
`<artifact>` is normally `ton-index-worker/ton-mch-engine/ir/mch_ir.json`.)

Gate = `Compare-Object`/diff of engine stdout vs twin stdout, expected 0 lines.

`py_actions` / `--actions` baseline is **277 rows over 165 fixtures** since EM2 W4 (was 227): the 227
matched rows plus one synthetic `unknown` row for each of the 50 fixtures whose trace classifies
cleanly to zero actions. Those rows print with the literal anchor `unknown` — they have no Block, so
there is no `block_key` to key them by — and both sides emit all 50.

## EM2 W7 — phase-1 A/B replay diff (offline tooling)

`ab_replay_diff.py` compares the `actions` payload **Python wrote** (read from a trace HSET dump)
against the payload **our writer produces** for the same traces, bucketed by the
`em2-writeback-design.md` §8 taxonomy. It never touches Redis: the input is a file, and producing
that file from prod is a separate read-only sweep.

    # Python's side, in prod-Redis shape (gzip(msgpack([...])), pendings.py:243-245)
    python <this>/py_actions_msgpack.py <artifact> tests/traces > py.txt
    python <this>/mk_hset_dump.py py.txt > hset.jsonl
    # our side
    ton-mch-engine --actions-msgpack -I <artifact> -T tests/traces > cpp.txt
    python <this>/ab_replay_diff.py --hset hset.jsonl --cpp cpp.txt \
           --ir ../ir/mch_ir.json --fills ../src/ActionBuild.cpp

Exit 0 iff the three **FATAL** buckets are empty: `real-mismatch`, `cpp-only-row`, `py-only-row`.
The coverage buckets (`matcher-runs-fill-missing` — the actionable one — `protocol-not-in-mch`,
`child-action`) and the known-divergence buckets (`wire-type-int-fields`, `boc-framing`,
`top-level-null`, `expected-omission`) are counted, never waived.

`mk_hset_dump.py` is a **dry-run input generator, not the prod reader**: it manufactures the sweep's
output format from the fixture twins so the diff tool can be exercised offline. It defaults to gzip
(what prod holds) and takes `--raw` for the uncompressed path, since our own writer ships
uncompressed (§2.8) and the reader sniffs the magic exactly as ton-index-go does.

### The dump format the prod sweep must produce — READ BEFORE SWEEPING

`ab_replay_diff.py` alone needs only Python's answer, so the two-field form is enough for the
**comparator**:

    {"trace_key": "<base64 ext_in_msg_hash_norm>", "actions_b64": "<base64 of the raw HSET value>"}

The **replay adapter** (W7 iii) also needs the trace's own data, and that is NOT derivable from
`actions`. A sweep capturing only the two fields above cannot be replayed and would have to be
re-run, so capture everything in one pass:

    {"trace_key": "...", "actions_b64": "...",
     "root_node":  "<the root_node HSET field, as stored>",
     "nodes":      {"<node key>": "<base64 of the node blob>"},
     "interfaces": {"<account>":  "<base64 of the interface blob>"}}

All of it comes from a single `HGETALL <trace_key>`, base64'd because the values are binary
(`em2-writeback-design.md` §8 step 1). The node blobs are `msgpack(RedisTraceNode)` exactly as
`TraceInserter` wrote them (`ton-trace-emulator/src/Serializer.hpp:210-218`), and that struct retains
what `schema_to_trace` consumes: account / lt / now / status, the ord description with its compute
phase and `aborted`, and per message the hash, source, destination, value, created_lt/at, opcode,
bounce flags and **`body_boc`**.

Two losses are inherent to replaying from Redis rather than in-process, both expected rather than
defects: message `value` is a `uint64` with **no extra currencies** (`emu-integration-plan.md` §2.2),
and the description is ord-only, so tick-tock transactions do not round-trip. Neither affects the
action diff, which is what phase 1 measures — but it is exactly why replay-from-Redis is not a
substitute for the in-process converter.

### The replay adapter — `mch-ab-replay` (W7 iii)

`ab_replay_diff.py` needs OUR side of the comparison in `--actions-msgpack` line format.
`ton-mch-engine --actions-msgpack` produces that from a fixture; `mch-ab-replay` produces it from a
sweep dump, which is what phase 1 actually has. It links `mch-classify` + `mch-emu` and runs the
production path unchanged — `classify_trace_fixpoint` (`ClassifyDriver.h`) then `serialize_actions`
(`emu/EmuActionSerialize.h`) — so the only new code is the decode from Redis bytes to
`schema::Transaction` (`src/abreplay/RedisReplay.cpp`, the one TU that includes
`ton-trace-emulator/src/Serializer.hpp`). No Redis client, no DB, no emulator run: the input is a
file.

    mch-ab-replay --replay dump.jsonl -I ../ir/mch_ir.json > cpp.txt
    python <this>/ab_replay_diff.py --hset dump.jsonl --cpp cpp.txt \
           --ir ../ir/mch_ir.json --fills ../src/ActionBuild.cpp

#### Two traps in the decoder — read these before touching `RedisReplay.cpp`

**1. `MSGPACK_USE_DEFINE_MAP` is load-bearing, and it is set on the WRITER, not the header.**
`ton-trace-emulator/CMakeLists.txt:43` compiles the emulator with `-DMSGPACK_USE_DEFINE_MAP`, which
changes what `MSGPACK_DEFINE` expands to: every struct `TraceInserter` stores in Redis is a **map
keyed by field name**, not the positional array the same header produces without the define. Nothing
in `Serializer.hpp` says so. A reader that includes the header and forgets the define compiles
cleanly and then fails on the first field of every node blob, so `mch-ab-replay`'s target repeats it
(`ton-mch-engine/CMakeLists.txt`, next to the `ton-trace-emulator-core` link). The upside of the map
form is that a decode mirror only has to match field NAMES, and missing keys are skipped rather than
misaligned.

**2. `AddressInterfaces` cannot be decoded with the real struct — the mirror is forced, not
preferred.** `tondb-scanner/src/msgpack-utils.h` defines `convert<td::RefInt256>` as an unconditional
`throw std::runtime_error("Deserializion of td::RefInt256 is not implemented")`, and every interface
variant carries at least one (`balance`, `index`, `full_price`, `last_bid`). Its *pack* side writes
`to_dec_string()`, so `RedisReplay.cpp` mirrors the six variants with `std::string` where the real
struct has `RefInt256` and converts back through `td::dec_string_to_int256`. `RedisTraceNode` has no
`RefInt256` anywhere and therefore uses the **real** struct on both sides (that is the
`load_existing_node` precedent, `TraceInserter.cpp:325`).

Two rules keep the mirror honest. `--make-dump` packs with the **real** `AddressInterfaces`, so a
field-name drift breaks the corpus round-trip loudly instead of silently decoding a default. And the
variant **order** must match `Serializer.hpp` exactly — msgpack packs `std::variant` as
`[alternative_index, value]` (`external/msgpack-c/.../cpp17/variant.hpp`), so a reordered alternative
list decodes one interface as another with no error at all.

**The dry run** manufactures the extended dump from the corpus. The node/interface blobs are packed
by the C++ side with the emulator's OWN structs, so the round-trip tests the reader against the real
wire format rather than against a private copy of it; `mk_replay_dump.py` then joins Python's
`actions` half onto it, reusing `mk_hset_dump.py` for the payload so the two generators cannot
disagree about what prod Redis holds:

    mch-ab-replay --make-dump -T tests/traces -o nodes.jsonl
    python <this>/py_actions_msgpack.py <artifact> tests/traces > py.txt
    python <this>/mk_replay_dump.py --nodes nodes.jsonl --py py.txt > dump.jsonl
    mch-ab-replay --replay dump.jsonl -I <artifact> --twin-view --fixture-lookups tests/traces

`--twin-view` substitutes the same stub emission `--actions-msgpack` uses (§2.5's two
emission-derived keys: `trace_external_hash_norm` = `"twin"`, `finality` = finalized). A fixture is
not an emission, so without it the round-trip would diff on those two keys and nothing else. A prod
sweep must NOT pass it — there the dump's real trace key and node finalities are the answer.

**Two gates, because they measure different things.**

- `--fixture-lookups` serves lookups from the fixture corpus while the transactions still come from
  the node blobs. That isolates the CONVERTER, and it is byte-exact: **0 diff lines against
  `ton-mch-engine --actions-msgpack` over 165 fixtures / 277 rows**, `ab_replay_diff.py` fatal
  buckets all zero at the documented baseline (11 / 97 / 42 / 1662). No ordering had to be
  canonicalized — the breadth-first walk from `root_node` mirrors `TraceInserter`'s own flatten, and
  `to_tree` re-sorts by lt regardless.
- Without it, lookups come from the dump's `AddressInterfaces` blobs — what EM2 will really see —
  and the corpus loses **14 rows**: 6 jvault (`jvault_assets` is tier-2-only), 4 dedust
  `jetton_swap` (`dedust_pool` has no variant in `AddressInterfaces`), 4 `nft_transfer`
  (`ParsedBlockLookupSource` renders `nft_item.content` as Null and `HostNft.cpp:230-233` faults on a
  non-Dict content). Nine of the affected traces drop to zero actions and gain W4's `unknown` row.
  None of this is replay-specific: `emu/EmuInterfaces.cpp` has the same four `to_v2` overloads, so
  an emulated trace in production sees exactly the same interface set.

**`--selftest` is not optional.** The three coverage buckets all read zero on the 165 fixtures —
every corpus row is a ported btype — so the dry run alone proves nothing about the half of the
taxonomy phase 1 exists to populate. `--selftest` drives each classification with a row shape phase 1
will really meet, needs no dumps, and has already earned its keep: it caught the classifier filing a
renamed action type (`stake_withdrawal`, from the nominator/tonstakers fills) as
`protocol-not-in-mch` rather than `real-mismatch` — a miscategorisation in the direction that hides
problems.

    python <this>/ab_replay_diff.py --selftest --ir ../ir/mch_ir.json --fills ../src/ActionBuild.cpp

## ** The EM2 write-back gate is NOT a text diff

`py_actions_msgpack.py` is `py_actions.py`'s pipeline (imported, not copied — it rebinds
`render_action`) emitting what `pendings.py` writes to Redis: `msgpack(Action.to_dict())`,
base64'd. Its gate is `cmp_actions_msgpack.py`, which compares **decoded maps**, because byte
equality is neither achievable nor required — Python's map order is `__dict__` insertion order and
the Go decoder is order-insensitive (`em2-writeback-design.md` §2.7, §11).

    ton-mch-engine --actions-msgpack -I <artifact> -T tests/traces > cpp.txt
    python <this>/py_actions_msgpack.py <artifact> tests/traces > py.txt
    python <this>/cmp_actions_msgpack.py cpp.txt py.txt      # exit 0 == gate pass

Pass condition: `real-mismatch`, `cpp-only-row` and `py-only-row` all zero. Every other difference
lands in a NAMED bucket that is counted, never waived — a bucket that stops firing is itself a
signal. Baseline at 165 fixtures / **277 rows** (W4): `wire-type-divergence` 11 (the §2.6/Q1
integers), `boc-framing` 97 (td writes no CRC32C, pytoniq does; same cell, proven by root hash — the
§12 root-hash bar, expressed on the msgpack side), `top-level-null` 42, `expected-omission` 1662
(277 × the six §2.3 keys). The 50 `unknown` rows add no divergence of their own: they carry no
composite, float or BOC field, and their key set matches Python's exactly.

`build_mch.bat` = the ONLY sanctioned local build entrypoint (vcvars64 + CLion cmake, see
plan §0.9). Cap parallelism when the user works in parallel:
`$env:CMAKE_BUILD_PARALLEL_LEVEL=4` (or copy the bat and add `-j 4`).

## C++ registry surface (cpp_surface.json / cpp_surface.py)

The twins gate matcher runnability against the C++ host/parse registries
(message parsers, host preds/fns/shapers, lookup kinds, BOC field names). Those
sets used to be hand-mirrored inline in each twin (CPP_PARSERS etc.) and drifted
independently. They now live in ONE file:

- `cpp_surface.json` — emitted by `ton-mch-engine --surface` from the LIVE C++
  registries (stable-sorted JSON). Regenerate after adding a parser / host
  binding / lookup kind / BOC field:

      ton-mch-engine --surface > ton-index-worker/ton-mch-engine/twins/cpp_surface.json

- `cpp_surface.py` — loads that JSON and exposes `CPP_PARSERS`,
  `CPP_LOOKUP_KINDS`, `CPP_SHAPERS`, `CPP_PREDS`, `CPP_FNS`, `BOC_FIELDS`. All
  four twins import from it (each imports only what it uses).

`BUILTINS` stays inline in py_classify/py_actions — it is the fixed expr-builtin
set, not a host registry. `lookup_kinds` includes `dedust_pool` (fetched by the
dedust_swap_legs host fn, never a `lookup` node) — an honest registry superset
that moves no skip decision. Drift between the committed JSON and the live engine
is guarded by `indexer/tests/mch/test_cpp_surface.py` and, for output-affecting
drift, by the classify/parse A/B gates themselves.

History: these lived in session scratchpads (plan §0.4 originally said "NOT committed");
moved into the repo 2026-07-11 for fresh-context durability — they encode gate semantics
(field-keyed cellhash renders, canonical sorting, skip tables) that are expensive to
reconstruct.
