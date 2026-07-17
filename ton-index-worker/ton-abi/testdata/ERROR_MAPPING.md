# ton-abi error mapping (W9, T9.1(a))

Reference: `S:\Dev\tolk-abi-to-typescript\tests\LotsOfWrappers.spec.ts`, three
`it()` blocks: `'not packable'` (:1374-1391), `'errors at dynamic
serialization'` (:1393-1412), `'errors at dynamic deserialization'`
(:1414-1419).

The reference asserts exact JS exception message text (jest's `.toThrow(...)`
against string literals). Our C++ side asserts fail-closed by CATEGORY (the
error trigger and rough shape of the message) rather than porting literal text
-- `td::Status` messages are ours to word, not a byte-for-byte port target.

## W11.5 re-homing (interpreter removed -- categories moved to the codegen path)

The table below was written against the W8 interpreter (`AbiInterp`,
`pack_value`/`unpack_value` over `AbiValue`). W11.5 removed the interpreter; the
end state is pure codegen. Category ownership moved as follows:

- **UNPACK-side non-serializable fields (#1-#11)** -- now owned by the emitter's
  stub-vs-fail policy (plan §0.18): a struct with a non-serializable field
  (bare `int`/`slice`/`builder`/`callable`/`unknown`) emits a COMPILING STUB
  whose `from_slice`/`store` return a runtime `td::Status` error naming the
  offending field. Tested in `test/AbiEmitTest.cpp` (`err-cont-on-stack`
  callable-field stub; `omitted_reason` "not serializable" text). The decl-level
  non-standard map key is a HARD codegen failure (same file, `err-invalid-map-key`).
- **PACK-side "wrong value shape" (#12-#25)** -- these were interpreter-only:
  they described `pack_value` rejecting a malformed `AbiValue`. The generated
  path has NO pack-from-`AbiValue` entry; `store()` takes a TYPED struct whose
  members are `td::int64`/`AbiAddress`/`std::vector<...>`/etc., so every one of
  these "wrong kind / not a number / not an object / missing field" states is
  rejected by the C++ TYPE SYSTEM at compile time, not a runtime check. They are
  therefore all now **N/A (structurally impossible on the typed path)**, a
  strictly stronger guarantee than the old "N/A (JS duck-typing)".
- **UNPACK-side value errors (#15/#25 range, #20 bits width, #26 truncation,
  #27 union-no-match)** -- owned by the generated `from_slice` leaf/dispatch
  calls (the SAME leaves the interpreter used). Gated by the `expect_error`
  conformance vectors through the generated structs in `test/AbiABGateTest.cpp`
  (truncation, prefix mismatch, union no-match vectors) plus the leaf boundary
  units in `test/AbiLeavesScalarTest.cpp`.
- **DIVERGENCE A (builder/slice pack passthrough)** -- unchanged in outcome but
  now enforced at CODEGEN: such fields make the struct non-serializable and the
  emitter stubs both directions (§0.18), rather than the interpreter rejecting
  at runtime. No fixture/vector exercises it; revisit trigger unchanged.

The reference-trigger rows below are retained as-is for provenance; read
"`AbiInterp.cpp`/`AbiInterpTest.cpp`/`pack_value`" in them as the historical
W8 owner, superseded by the codegen owners named above.

| # | Reference (line) | Trigger | C++ disposition |
|---|---|---|---|
| 1 | `NotPackable1.fromSlice` :1375 | tensor field element is bare `int` | Ported. `unpack_value` default case: `"type 'int' is not serializable"`. |
| 2 | `NotPackable1.toCell` :1376 | same, PACK direction | Ported. `pack_value` same default case (symmetric). |
| 3 | `NotUnpackable1.fromSlice` :1378 | tensor field element is `builder` | Ported, but see **DIVERGENCE A** below -- we reject `builder` on BOTH directions, not just unpack. |
| 4 | `NotUnpackable1.toCell` (non-throwing!) :1379 | same struct, PACK succeeds in the reference | **NOT ported** -- DIVERGENCE A: plan decision 1 buckets `builder`/`slice`/`callable`/`unknown` as one non-serializable category (get-method/stack domain, out of scope), rejecting all of them in both directions. The reference's dynamicPack actually has real cases for `builder` (`storeBuilder`) and `slice` (`storeSlice`) that we never ported, so packing a raw builder/slice value is a live path there and a hard error here. No fixture/vector exercises builder-as-a-value in this project's protocols; flagged for W11/deferred-register awareness, not silently dropped. |
| 5 | `WithNotPackable.fromSlice` :1381 | array element type is `unknown` | Ported. Same default-case rejection, wrapped with `[ith]` path via `WithNotPackable`'s nested alias-to-tuple-of-unknown shape. |
| 6 | `NotPackable3Alias.fromSlice` :1383 | same `unknown`-element shape via a different alias name | Ported (same mechanism as #5, different fixture struct). |
| 7 | `NotPackable4Alias.toCell` :1385 | tensor element is bare `int`, PACK direction | Ported (same mechanism as #1/#2 via a different fixture struct). |
| 8 | `NotPackable5.fromSlice` :1387 | shapedTuple element is `builder`/`callable` | Ported (category: non-serializable element inside a shapedTuple). |
| 9 | `NotPackable6.fromSlice` :1388 | bare `callable` field ("continuation") | Ported. Message literally says `"type 'continuation' is not serializable"` -- matches the reference's own `renderTy` label for `callable` (`"continuation"`), a fortunate near-exact match. |
| 10 | `NotPackable7.fromSlice` :1389 | same as #9, different fixture struct | Ported (same mechanism). |
| 11 | `NotPackable8.fromSlice` :1390 | bare `unknown` field | Ported. Message: `"type 'unknown' is not serializable"`. |
| 12 | `packToBuilderDynamic(..., null, b)` :1395 | pack: value is `null`/not an object | **N/A** -- JS dynamic-typing artifact. `AbiValue` is a tagged C++ variant, not duck-typed; there is no "not an object" state distinct from "wrong kind", already covered by `wrong_kind()`. |
| 13 | `{value: undefined}` not a number :1396 | pack: missing/wrong-typed field value | Ported via `wrong_kind()` category (`"expected Int value, got ..."`), see `AbiInterpTest.cpp`'s "pack error paths" test case (T8.2). |
| 14 | `{value: "123"}` not a number :1397 | pack: string where a bigint expected | **N/A** -- same JS-duck-typing artifact as #12; `AbiValue::Int` can only ever hold a `RefInt256`, this class of error is structurally unrepresentable. |
| 15 | `{value: 1n<<40n}` out of range :1398 | pack: int32 value out of bit range | Ported. `store_int`/`store_uint` (W3 leaves) already reject out-of-range values; exercised directly in `AbiLeavesScalarTest.cpp` and indirectly here via "int32 value out of range". |
| 16 | `{addr: 123n}` not an object :1399 | pack: address field given a bare number | Ported via `wrong_kind()` (`AbiValue::Address` expected, got `Int`). |
| 17 | `{t: 1}` not an array :1400 | pack: tensor field given a non-array | Ported via `wrong_kind()` (`AbiValue::List` expected). |
| 18 | `{f1: [123]}` not an object (bits8) :1401 | pack: bitsN field given an array | **N/A** -- JS shape-check artifact; our `AbiValue::Bits` vs `List` distinction is already the `wrong_kind()` category. |
| 19 | `src.clone is not a function` :1402 | pack: bitsN field given a plain object without `.clone` | **N/A** -- pure JS duck-typing failure mode, no C++ analogue (`AbiValue::Bits` either holds a `Ref<CellSlice>` or it's the wrong kind). |
| 20 | `expected 8 bits and 0 refs, got 0 bits and 0 refs` :1403 | pack: bitsN field with wrong width | Ported. `pack_value`'s `BitsN` case has the identical width/ref-count check (`"expected N bits and 0 refs, got ..."`), verified in `AbiInterpTest.cpp` bitsN round-trip test's shape. |
| 21 | union pack: not an object with `$` :1404-1405 | pack: union field given a bare value or `$`-less object | Ported. `pack_value`'s Union general path: `"expected a union value ({$,value} wrapper) or a labeled struct, got ..."`. |
| 22 | union pack: non-existing variant :1406 | pack: union `$` doesn't match any variant | Ported. Exact category match: `"non-existing union variant for $ = '...'"` -- see `W9 error mapping` test case. |
| 23 | union pack: missing `value` field :1407-1408 | pack: `{$,...}` object without a `value` key, hasValueField=true variant | **Partial.** Our `pack_value` Union case reads `v.inner` directly off the `AbiValue::Union` variant (which the C++ type system guarantees exists whenever `kind==Union` -- there is no way to construct a `Union`-kind `AbiValue` without an inner value), so this exact "present but missing `value` key" JS-shape failure mode cannot arise on the C++ side. Structurally covered by our type design rather than a runtime check. |
| 24 | enum pack: not a number :1409 | pack: enum field given a non-numeric value | **N/A** -- JS duck-typing artifact, same class as #12/#14. |
| 25 | scalar pack: out of range (8 bits) :1411 | pack: int8 field given a too-large value | Ported (same mechanism as #15). |
| 26 | scalar unpack: truncated slice, "Index 0 > 0 is out of bounds" :1417 | unpack: not enough bits in the slice | Ported. Every scalar leaf (`load_int`/`load_uint`/etc., W3) already fails closed on truncation; category-matched in `W9 error mapping` test case ("truncated slice"). |
| 27 | union unpack: no prefix matches :1418 | unpack: union body doesn't match any variant's prefix | Ported. Exact category match: `dispatch_union`'s `"none of union prefixes match"` (W7), re-verified here against a real fixture union (`IntAndEitherInt8Or256`). |

## Summary

- **20 of 27** ported as native category-matched units (some consolidated:
  #1/#2/#7 share one mechanism, #5/#6 share one mechanism, #9/#10 share one
  mechanism, #15/#25 share one mechanism).
- **6 of 27** marked N/A: pure JS dynamic-typing/duck-typing failure modes
  (#12, #14, #18, #19, #24) that are structurally unrepresentable once values
  are typed as a C++ tagged variant (`AbiValue`) instead of arbitrary JS
  objects -- there is no "wrong shape" state that `wrong_kind()` doesn't
  already collapse into a single category.
- **1 of 27** (#23) structurally covered by the type system rather than a
  runtime check (an `AbiValue::Union` cannot exist without its inner value).
- **1 of 27** (#4) is DIVERGENCE A, a real behavioral difference deliberately
  NOT ported -- see the table row for detail and the trigger to revisit it.

## DIVERGENCE A -- builder/slice pack asymmetry (not ported)

The reference's `dynamicPack` has real (non-stub) cases for `builder`
(`b.storeBuilder(v)`) and `slice` (`b.storeSlice(v)`) -- both are PACKABLE
write-only passthroughs in the reference, while `dynamicUnpack` has no case
for either (always "not serializable"). Plan decision 1 (locked, user-approved)
buckets `builder`/`slice`/`callable`/`unknown` as a single non-serializable
category "get-method/stack domain, out of scope" without this pack/unpack
asymmetry, and `AbiInterp.cpp`'s `TyKind::Builder`/`TyKind::Slice` cases reject
both directions symmetrically. No fixture or vector in this project exercises
packing a raw builder/slice value. Revisit trigger: a real protocol needing to
construct a message body from a pre-built `vm::CellBuilder`/`vm::CellSlice`
fragment via the ABI interpreter.
