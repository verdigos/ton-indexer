# W8 handoff — kernel + leaves are DONE, wire the interpreter

Written at the end of W7 by the W2-W7 executor for the FRESH W8 executor. The
plan doc (`docs/ton-abi-plan.md`, esp. §0.14 value schema, §3 T8.1/T8.2/T8.3, §4
TDD protocol) is authoritative; this doc is the delta — the concrete API surface
you inherit and the non-obvious things that aren't in the plan.

## What exists and is green (ctest 1/1, 114 cases / 691 assertions)

Every leaf below is fail-closed (`td::Result`/`td::Status`, never aborts), over
`vm::CellSlice&` (load) / `vm::CellBuilder&` (store). All in namespace `ton_abi`.

- **W2 kernel** — `src/AbiKernel.h`: `AbiKernel::create(const ContractABI&)`
  (non-owning; ABI must outlive it). `struct_fields_of(ty_idx) -> const
  vector<ResolvedField>*` (CELL side, monomorph + client_ty_idx swap + uLabel),
  `alias_target_of`, `create_labels_for_union`, `resolve_union(union_ty_idx,
  optional<uLabel>) -> ResolvedUnion`, `get_struct/get_alias/get_enum`,
  `ty_by_idx`. `ResolvedField{orig, ty_idx, u_label_ty_idx}`, `LabeledVariant{
  variant_ty_idx, prefix_num, prefix_len, is_prefix_implicit, label_str,
  has_value_field}`, `ResolvedUnion{variants (decl order, incl trailing void),
  dispatch_order (sorted, excl void), has_void}`.
- **W3 scalars** — `src/AbiLeavesScalar.h`: `load_int/store_int` (n 1..257),
  `load_uint/store_uint` (1..256), `load_bool/store_bool`, `load_coins/store_coins`,
  `load_varint/store_varint`, `load_varuint/store_varuint` (n = Tolk width; prefix
  = log2(n), power-of-two enforced), `load_bits/store_bits` (bitsN => a
  `Ref<vm::CellSlice>` of n bits / 0 refs). Values are `td::RefInt256` except bool
  (bool) and bitsN (bit-slice).
- **W4 addresses** — `src/AbiLeavesAddress.h`: `AbiAddress{kind None|Std|Extern;
  int workchain; td::Bits256 hash; int ext_bits; Ref<vm::CellSlice> ext_value}`.
  `load/store_address` (std only), `load/store_maybe_address` (None|Std),
  `load/store_external_address`, `load/store_address_any`.
- **W5 refs/strings** — `src/AbiLeavesRef.h`: `load_cell/store_cell`,
  `load_ref_slice` (cellOf: opens the ref for you), `load_maybe_ref/store_maybe_ref`,
  `load_maybe_prefix/store_maybe_prefix` (the nullable 1-bit), `load_string/store_string`
  (snake, raw bytes), `load_remaining/store_remaining`.
- **W6 containers** — `src/AbiLeavesContainer.h` (arrayOf/lispListOf) +
  `src/AbiLeavesDict.h` (mapKV). CALLBACK-driven — see below.
- **W7 prefixes/union** — `src/AbiLeavesPrefix.h`: `load_and_check_prefix(cs,
  num, len)` (struct opcode), `store_prefix`, `lookup_prefix` (peek),
  `dispatch_union(cs, ResolvedUnion) -> UnionDispatch{variant_index, is_void}`,
  `store_union_prefix(cb, LabeledVariant)`.

## The callback contract (W6) — this is the key thing to internalize

arrayOf/lispListOf/mapKV are STRUCTURAL: the leaf walks the container wire form
and calls back into YOUR walker for each inner element/entry. The leaf is
type-agnostic; the callback closes over the AbiValue collection + the inner
`ty_idx` and calls your recursive unpack/pack.

- `load_array(cs, unpack_one)` / `load_lisp_list(cs, unpack_one)` where
  `unpack_one = function<Status(vm::CellSlice& elem)>`. Your closure MUST consume
  exactly one element from `elem` (advance it) and push the AbiValue it produced.
  load_array guards against a callback that consumes nothing (infinite-loop
  protection) — so if your inner unpack is a no-op on a 0-width type, that's an
  error surfaced to you.
- `store_array(cb, count, pack_one)` / `store_lisp_list(cb, count, pack_one)` where
  `pack_one = function<Status(vm::CellBuilder&, size_t idx)>` packs element idx.
- `load_dict(cs, key_bits, on_entry)` where `on_entry = function<Status(
  vm::CellSlice& key, vm::CellSlice& value)>`. You parse the key (per key type:
  load_uint/load_int/load_address from `key`) and the value (inner ty from
  `value`). Entries arrive in ASCENDING key order already (= §0.14 sorted-by-key;
  do NOT re-sort).
- `store_dict(cb, key_bits, count, emit)` where `emit = function<Status(size_t,
  vm::CellBuilder& key_out, vm::CellBuilder& val_out)>` must write exactly
  key_bits into key_out and the value body into val_out.

If a different consumption shape would serve you better, ping the W2-W7 executor
— but the callback shape keeps the leaf/walker split clean and is already proven.

## Non-obvious things you MUST know (not in the plan)

1. **tensor / shapedTuple have NO leaf** — they are walker-level inline recursion
   (plan T8.2 says so explicitly). Just recurse over `items_ty_idx` in order.
2. **Custom-serializer gate ORDERING (W2 invariant ii)**: the kernel NEVER checks
   `custom_pack_unpack`. YOU must check `custom_pack_unpack.unpack_from_slice` /
   `.pack_to_builder` on the struct/alias/enum BEFORE calling `struct_fields_of` /
   `alias_target_of` — exactly as dynamic-serialization.ts:335-374. The custom
   registry is your (W8) code; the two-registry design is §0.17.
3. **uLabelTyIdx threading (W2 invariant i)**: when you recurse into a field, pass
   `ResolvedField::u_label_ty_idx` down; when you hit a `union`, call
   `resolve_union(union_ty_idx, that_uLabel)` so generic-union labels come out as
   `T1|T2` not `int32|int64`. Same for `alias_target_of(...).u_label_ty_idx`.
   NOTE nullable DROPS uLabel (reference behavior — don't reintroduce it).
4. **union dispatch already consumed the IMPLICIT prefix** but left EXPLICIT
   prefixes in place. After `dispatch_union`, unpack `variants[variant_index].
   variant_ty_idx` normally — if that variant is a prefixed struct with an
   explicit prefix, its OWN unpack (your StructRef case, via
   `load_and_check_prefix` on the struct's `prefix`) re-reads it. is_void =>
   emit `{$:"void"}`, consume nothing.
5. **union value shape (§0.14)**: use `LabeledVariant::has_value_field` /
   `label_str`. has_value_field false (struct-with-own-label) => inline the
   variant object (it already carries its own `$`); true => `{$: label_str,
   value: ...}`. nullLiteral variant => `label_str==""`, unpack to JSON null.
6. **StructRef dump carries `$`**: `{ "$": struct_name, ...fields }` even outside
   unions (VALUE_SCHEMA.md, matches dynamicUnpack StructRef).
7. **`cell` dump = base64 of `vm::std_boc_serialize(root, 2)`** (mode
   `WithCRC32C`, = @ton/core toBoc() default). PROVEN byte-identical (VALUE_SCHEMA.md
   "BOC serialization mode"). `cellOf` dumps as `{"ref": inner}`.
8. **address dump**: Std => `"wc:hex"` (`hash.to_hex()` — NB vm returns UPPERCASE
   hex; the oracle/JSON uses lowercase, so lowercase it), Extern =>
   `{"extern":{"bits":n,"value":<hex of ext_value bits>}}`, None => `"none"`
   (any_address) / null (address?).
9. **bitsN / remaining dump** = slice-ish `{"bits": binary-string, "refs":[BOC-b64]}`
   (§0.14). The leaf hands you a `Ref<vm::CellSlice>`; you format it.
10. **varint PACK byte-count is @ton/core-exact** (`ceil((mag_bits+1)/8)`), which
    diverges from a two's-complement bit_size at exact powers of two (e.g. -128).
    Already handled inside store_varint — just don't second-guess it if a vector
    looks "off".
11. **top-level body open must reject exotic/special cells** (plan T8.2, §R.6).
    The leaves already reject special cells when they OPEN inner refs
    (load_cell_slice_special), but the ROOT body open is yours — reject there too.
12. **hex case**: `td::Bits256::to_hex()` and `Cell::get_hash().to_hex()` are
    UPPERCASE. The value-JSON schema (§0.14) and the Node oracle use lowercase for
    address hashes. Lowercase in your dump.

## Vectors / gates

- 12 vectors exist (`testdata/abi_vectors.json`), all `enabled_from: W8`. They
  are currently SKIPPED (VectorRunner green-empty). W8 is where they light up —
  T8.3 runs the full gen_vectors set through your walker.
- Build: PowerShell, `cmake --build cmake-build-debug-visual-studio --target
  ton-abi-test` under vcvars64 (`C:\Program Files\Microsoft Visual Studio\18\
  Community\VC\Auxiliary\Build\vcvars64.bat`), `$env:CMAKE_BUILD_PARALLEL_LEVEL=4`.
  Plain-env reconfigure only after a CMakeLists edit (Ninja auto-reconfigures).
- **CHECK-macro guard** in every new test TU: `#include "doctest.h"` then wrap the
  ton-abi/td includes in `#pragma push_macro("CHECK") / #undef CHECK / ... /
  #pragma pop_macro("CHECK")` (td's check.h #defines CHECK and shadows doctest's).
- Node oracle for byte-identity spot-checks: `tools/` has `node_modules/@ton/core`
  vendored; `node -e "..."` works (I used it for BOC/dict/array reference values).

## Known reference quirks inherited (faithful, do not "fix")

- lispList only round-trips element types WITHOUT their own refs (tail is ref[0]).
- Anycast addresses are REJECTED (plan divergence #2, §0.12b) — not processed.
- Generic-union reshape after monomorphization, non-standard map keys, customs on
  generics — all upstream gaps (§5 deferred register).
