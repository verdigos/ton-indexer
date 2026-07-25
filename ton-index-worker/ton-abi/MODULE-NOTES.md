# ton-abi module notes

Durable, non-obvious facts about this module: things that are not derivable from
the code at a glance, that bit someone once, or that look like bugs but are
faithful ports. Salvaged from W8-HANDOFF.md (deleted in R1a — the rest of that
doc was a wave-scoped "here is what to build next" brief, long overtaken).

Authoritative docs elsewhere: `indexer/docs/ton-abi-plan.md` (plan + wave log,
§0.14 value schema), `W11-SHAPE-SPEC.md` (emitter contract), `src/AbiValue.h`
(value dump table), `testdata/ERROR_MAPPING.md` (error categories),
`ABI-PROTOCOL-CHECKLIST.md` (every touch point for putting a NEW protocol through
the ABI path, end to end — start there, not here, when adding one).

## Semantics that are easy to get wrong

1. **tensor / shapedTuple have NO leaf** — they are inline recursion over
   `items_ty_idx` in order.
2. **Custom-serializer gate ORDERING** (W2 kernel invariant ii): the kernel NEVER
   looks at `custom_pack_unpack`. A consumer must check
   `custom_pack_unpack.unpack_from_slice` / `.pack_to_builder` on the
   struct/alias/enum BEFORE resolving fields (`struct_fields_of` /
   `alias_target_of`) — exactly as `dynamic-serialization.ts:335-374`. The
   emitter does this at generation time.
3. **uLabelTyIdx threading** (W2 kernel invariant i): recursing into a field
   carries `ResolvedField::u_label_ty_idx` down; on reaching a `union`, resolve
   with that uLabel (`resolve_union(union_ty_idx, uLabel)`) so generic-union
   labels come out as `T1|T2`, not `int32|int64`. Same via
   `alias_target_of(...).u_label_ty_idx`. NOTE nullable DROPS the uLabel — that
   is reference behavior; do not reintroduce it.
4. **Union prefixes: implicit is EATEN by dispatch, explicit is NOT** — an
   explicit prefix is left in place and re-read by the variant's own loader
   (`load_and_check_prefix` on the struct's own prefix). Void variant matches an
   EMPTY slice, consumes nothing, and dumps `{"$":"void"}`. The generated code
   bakes these rules per union; see `src/AbiLeavesPrefix.h` for the full form
   list and `test/AbiABGateTest.cpp` ("union dispatch forms") for the gate.
5. **Union value shape** (§0.14): driven by `LabeledVariant::has_value_field` /
   `label_str`. false (struct carrying its own label) → inline the variant object
   (it already has its `$`); true → `{"$": label_str, "value": ...}`. A
   nullLiteral variant has `label_str == ""` and dumps as JSON null.
6. **A StructRef dump always carries `$`**, even outside a union.
7. **`cell` dump = base64 of `vm::std_boc_serialize(root, 2)`** (mode
   `WithCRC32C` = @ton/core `toBoc()` default) — proven byte-identical.
   `cellOf` dumps as `{"ref": inner}`.
8. **address dump**: Std → `"wc:hex"`, Extern →
   `{"extern":{"bits":n,"value":<hex>}}`, None → `"none"` for any_address, JSON
   null for an absent `address?`.
9. **bitsN / remaining dump** = `{"bits": "0101…", "refs":[BOC-b64, …]}`.
10. **varint PACK byte-count is @ton/core-exact** (`ceil((mag_bits+1)/8)`), which
    diverges from a two's-complement bit size at exact powers of two (e.g. -128).
    Handled inside `store_varint` — do not "fix" it when a vector looks off.
11. **Exotic/special cells are rejected at the top-level body open.** The leaves
    already reject them when opening inner refs (`load_cell_slice_special`); the
    ROOT open belongs to the consumer — the mch bridge does it in its trampoline.
12. **hex case**: `td::Bits256::to_hex()` / `Cell::get_hash().to_hex()` return
    UPPERCASE; the value-JSON schema and the Node oracle use lowercase. Dump
    lowercase.

## Reference quirks inherited (faithful — do not "fix")

- lispList only round-trips element types WITHOUT their own refs (tail is ref[0]).
- Anycast addresses are REJECTED (plan divergence #2, §0.12b).
- Generic-union reshape after monomorphization, non-standard map keys, and
  customs on generics are upstream gaps (plan §5 deferred register).
- Enums do NOT validate their member set in either direction — the reference
  doesn't either, so an out-of-range value must round-trip (W11-SHAPE-SPEC §5).

## Test-suite conventions

- **The CHECK-macro guard is centralized**: td's `check.h` `#define`s `CHECK` and
  shadows doctest's. Test TUs include `test/AbiTestSupport.h` FIRST — it does the
  `push_macro`/`#undef`/`pop_macro` dance once around every td/vm/ton-abi header
  and exposes `read_file` / `fixture_path` / `load_fixture_abi`. After it, a TU
  can include anything (generated headers included) with no ceremony.
- **Build**: PowerShell, `cmake --build cmake-build-debug-visual-studio --target
  ton-abi-test` under vcvars64 (`C:\Program Files\Microsoft Visual Studio\18\
  Community\VC\Auxiliary\Build\vcvars64.bat`), `$env:CMAKE_BUILD_PARALLEL_LEVEL=4`.
  Plain-env reconfigure only after a CMakeLists edit.
- **Node oracle** for byte-identity spot checks: `tools/node_modules` has
  `@ton/core` vendored, so `node -e "…"` from `tools/` works. Vector regeneration
  is `tools/gen_vectors.mjs` (needs the reference-repo checkout); production ABI
  declarations in `abi/` are compiled by `tools/gen_prod_abi.mjs` (tolk-js only).
