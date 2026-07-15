# ton-abi value-JSON schema — W0 validated subset

This documents what `tools/gen_vectors.mjs` actually produces and validates at
W0. It does NOT independently legislate — the full canonical schema is frozen
in `docs/ton-abi-plan.md` §0.14; this file records what's been PROVEN so far
by the round-trip self-validation, and what's still open.

## Validated at W0

Kinds covered by chosen fixtures (scalar-only structs, no unions/addresses/
cells/arrays/generics/customs):

- `intN` / `uintN` / `coins` (also `varintN`/`varuintN`, same code path):
  value-JSON = decimal string (`v.toString()` on the JS `bigint`, `BigInt(s)`
  to invert). Negative `intN` values render with a leading `-` (e.g.
  `CounterDecrement.dec_by = -38` -> `"-38"`).
- `bool`: value-JSON = JSON bool (code path exists, unexercised by current
  vectors — no bool-typed field in the 3 chosen fixtures' picked structs).
- `StructRef` (plain struct, not inside a union): value-JSON = object,
  `"$": <struct name>` first, then fields **in schema (declaration) order**.
  This matches the reference's own `dynamicUnpack` StructRef case
  (`dynamic-serialization.ts:347`, `result = { $: ty.struct_name }`) — i.e.
  struct dumps always carry `$`, not only inside unions. Pack side
  (`dynamic-serialization.ts:208-210`) reads fields by name only and ignores
  `$`, so `$` is redundant-but-harmless on the pack path.

## Proof performed (T0.2 self-validation, all 12 vectors)

1. **(a) Lossless round-trip**: for every vector, `value` (the JSON above) is
   parsed back to a JS object and re-packed; the resulting hex is asserted
   equal to `golden_hex`. Ran in `gen_vectors.mjs::roundTripCheckOrThrow`.
2. **(b) Literal spec golds**: the 5 `lots-of-wrappers` vectors
   (MsgSinglePrefix32, CounterIncrement, CounterDecrement, CounterReset0,
   CounterResetTo) use values and hex transcribed VERBATIM from
   `tests/LotsOfWrappers.spec.ts` (lines 795, 812-815) in the reference repo —
   not just self-consistent, but pinned to the reference test's own asserted
   ground truth.
3. **(c) Custom serializers**: NONE of the chosen fixtures/structs need one —
   all are plain intN/uintN/coins fields. No porting was needed at W0; the
   registry seam (§0.17) and the 10 LotsOfWrappers custom-serializer types
   remain fully deferred to W8/W9.
4. **Byte-stability**: ran the generator twice; `diff` of the two
   `abi_vectors.json` outputs is empty (0 bytes different). Object key order
   is fixed by construction (schema field order for structs; the vectors
   array and header are built in fixed iteration order, no incidental sorting
   needed since nothing here has a map/set).

## Golden hex format

`golden_hex` is **`Slice.toString()` from `@ton/core`** (the reference
library's own recursive fift-hex dump: `x{...HEX..._}` with child refs on
following indented lines) — the exact same string form
`tests/LotsOfWrappers.spec.ts`'s `run()` helper asserts against
(`expect(s.toString()).toBe(ans)`, line 224/236). This is NOT a custom
serializer we invented; it is `@ton/core`'s builtin, used as-is. None of the
W0 vectors have refs, so no vector here exercises the multi-line form — that
first shows up once a fixture/struct with a `cell`/`cellOf`/ref field is added
(W5+).

## BOC serialization mode (W5 — resolves the W0-deferred §0.14 item)

The `cell` value-JSON is a **BOC base64**. The serialization flags are pinned to
match what the Node oracle (`@ton/core`) emits, so a generated `to_abi_value()`
dump is byte-identical to the vector:

- `@ton/core` `cell.toBoc()` defaults (Cell.js:170-173) to `{ idx: false,
  crc32: true }` → no index, CRC32C appended, no cache bits.
- The equivalent C++ call is **`vm::std_boc_serialize(root, 2)`** — mode bit
  `Mode::WithCRC32C == 2` (boc.h:261), with `WithIndex(1)` / `WithCacheBits(16)`
  OFF.
- **Byte-identity proven** (`test/AbiLeavesRefTest.cpp`): a hand-built root cell
  (`0x1234` + ref `0xAB`) serialized via `std_boc_serialize(_, 2)` equals, byte
  for byte (base64), the `root.toBoc()` output recorded from a Node run:
  `te6cckEBAgEACAABBBI0AQACqyFjJBU=`. Three `storeStringRefTail` cases (short /
  empty / 200-byte multi-cell snake) are likewise asserted byte-identical to
  `@ton/core`, exercising refs + the snake chunking.

So: **`cell` dump = base64 of `vm::std_boc_serialize(root, 2)`**; W8's
`to_abi_value` and the gen_vectors oracle agree by construction.

## Deviations from §0.14 — none found (so far)

No deviation from the plan's frozen schema was discovered while building this
subset. The following parts of §0.14 are simply **not yet exercised** (no
vector needs them yet), not contradicted:

- bigint width-preserving external-address `{"extern": {"bits": N, "value":
  hex}}` — needs W4 (addresses).
- `cell` = BOC base64 with pinned serialization flags — **RESOLVED at W5**, see
  "BOC serialization mode" below.
- union `hasValueField` distinction, `map` sorted-entries, `arrayOf`/tensor —
  no union/map/array field exists among the picked structs; needs W6/W7/W8.
