# Action goldens

`goldens/actions/<group>/<slug>.<id8>.yaml` — one file per fixture trace in
`indexer/tests/traces/`, holding every action row the engine serializes for
it, exactly as production would write them. A diff is a regression, never a
re-baseline; a deliberate output change re-generates the files in its own
commit and the diff is reviewed field by field.

Regenerate (repo root):

```
ton-mch-engine --actions -T indexer/tests/traces -O ton-index-worker/ton-mch-engine/goldens/actions \
    --fixtures ton-index-worker/ton-mch-engine/goldens/fixtures.json
```

Gate: ctest `mch_actions_golden` (`cmake/ActionsGoldenCheck.cmake`) regenerates
into a scratch dir and `diff -r`s it against this directory.

## Layout

`fixtures.json` names every fixture: `group` (protocol, the directory), `name`
(human title, the file header), `slug` (file name), optional `note`. A trace
missing from the manifest lands in `misc/<trace-id>.yaml`. Trace files keep
their trace-id names; only the goldens are grouped.

## Row format

```yaml
# gasless / Gasless jetton transfer by a wallet v5
# trace: tF9rJ18uKOZuwJaQ0z2ZB2ln0OcFMEa0fy6rYxMgQNY=
actions:
  - type: jetton_transfer
    matcher: jetton_transfer            # spec matcher that produced the block; absent on engine-made rows
    anchor: jetton_transfer@100905990000006   # btype[#opcode]@lt of the block's earliest node
    id: sOyNIEgTzUdJkftksEQMqspgI9IxemYnp3uD3Mc6Xbo=
    success: true
    source: 0:16C2…                      # the ten directional/scalar columns are always
    source_secondary: 0:9D65…            # present, null spelled out
    ...
    amount: 2093172
    value: null
    opcode: null                         # hex when set
    data:                                # one flat map, only when a payload field is set
      comment: null
      custom_payload: null
      ...
    tx_hashes:                           # starting transaction first, then the rest sorted
      - EirR…
    accounts:
      - 0:16C2…
    start_lt: 100905990000006
    ...
    mc_seqno_end: 90192469
    parent_action_id: …                  # child rows only
    ancestor_type: [...]
```

Rows are sorted by anchor. The emulator Redis envelope stores the codec-prefixed
payload bytes separately under its binary `payload` key; the action golden shows
the named flat `data` map. Codec `01` stores a single MessagePack map with numeric
key ids in ascending order at every depth; `01 80` is the empty payload. Typed
addresses, BOCs and hashes are binary, and unsigned decimals are compact uints
or minimal big-endian binary above uint64. In encoded payloads, null fields and list elements
are omitted recursively, except `legs` retains null placeholders before filled
legs, trims trailing placeholders, and omits the key when all legs are empty.
For classified actions, `tx_hashes` starts with the initiating transaction, or the root-level block's earliest transaction by `tx->lt`, followed by the remaining hashes sorted ascending.
A trace that classifies to nothing carries the
synthetic `unknown` row; a trace whose classification throws writes
`failure: <reason>` followed by its basic-fallback rows (no corpus trace does).
BOC-valued fields render as the cell hash.

## Editing a synthetic fixture

Fixture-path code-hash gates read the `code_hashes` map; `code_hashes` added by hand from `VESTING_CONTRACT_CODE_HASH`: 03RZCW7m, 03XZQ1Ck, 1NL0tCMr, IG3rAERJ.
NftSale/NftAuction `code_hash` added by hand from toncenter v3 account state: EzvOGPJe, F73aJsg-, LQB6Qv2a, M2rUrfJX, Mk9O4OZz, _4VnZDY_, mG5NGg-.

A fixture note is an optional one-line scenario of at most 100 characters. Only
synthetic fixtures prefix the note with `synthetic: `; chain and API-built
fixtures have no label. A
synthetic fixture has no unmodified chain or API capture, so its `.lz4` is edited
in place through a JSON round trip. From `indexer/`:

```
python -m tests.utils.fixture_json unpack indexer/tests/traces/<trace-id>.lz4 -o /tmp/fixture.json
# edit /tmp/fixture.json
python -m tests.utils.fixture_json pack /tmp/fixture.json -o indexer/tests/traces/<trace-id>.lz4
```

The JSON form is the fixture map 1:1; msgpack bin values spell as
`{"$b64": "<base64>"}` and a map with non-string keys as
`{"$map": [[key, value], ...]}`. Re-generate the golden with the regenerate
command above and review the diff.
