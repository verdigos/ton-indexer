# Emitted C++ shape specification

This document defines the binding output contract for `ton-abi-gen`. Literal
constants such as prefixes, type indices, and field names come from the input
ABI at generation time. Examples use real fixture declarations and should be
checked against the corresponding `.abi.json`.

## 1. Output layout

Per ABI: `generated/<contract_snake>_gen.h` + `_gen.cpp`, namespace
`ton_abi::gen::<contract_snake>` (sanitized snake case of `contract_name`). Both are committed;
`ton-abi-gen --check` regenerates to temp + byte-compares (msgs_gen discipline).
The hand-written shared support header `src/AbiGenSupport.h` provides
`CellOf<T>`, the typed custom-serializer registry, common includes, and value
helpers. `CellOf<T>` uses `std::shared_ptr<T>` so recursive cell boundaries work
and the inner struct need only be forward-declared. Custom serializers use a
runtime registry rather than template specializations.

A generated header includes only `AbiGenSupport.h`, and a generated source
includes only its own header. Leaf headers, `AbiValue.h`, and standard TON
includes arrive transitively through this support contract. Generated code
never includes `AbiKernel`, `AbiLoader`, or an interpreter; all
resolution and dispatch are baked at generation time.

## 2. Identifier rules

- A declaration name that is a valid C++ identifier is emitted verbatim.
  Otherwise non-`[A-Za-z0-9_]` characters become `_`; for example,
  `Storage@me` becomes `Storage_me`. The original spelling remains in `$`.
- Monomorphs resolve by type index, never by display name. `ResetTo<int64>`
  becomes `ResetTo_int64`; punctuation becomes `_`, runs collapse, and trailing
  underscores are trimmed. Collisions gain an occurrence counter (`_2`, `_3`, ...).
- Reserved words and collisions with support names gain a trailing `_`.

## 3. Field type mapping (typed struct members)

| ABI ty | C++ member |
|---|---|
| intN with n<=64 | td::int64 via load_int64/store_int64 |
| uintN with n<=64 | td::uint64 via load_uint64/store_uint64; n=64 fits uint64 |
| intN/uintN with n>64, varintN/varuintN/coins | td::RefInt256 |
| bool | bool |
| bitsN / remaining | td::Ref<vm::CellSlice> |
| string | std::string (raw bytes; lossy-UTF8 only in to_abi_value dump) |
| address/addressOpt/addressExt/addressAny | ton_abi::AbiAddress |
| cell | td::Ref<vm::Cell> |
| cellOf<T> | CellOf<TypeOf(T)> |
| nullable<T> | std::optional<TypeOf(T)> |
| arrayOf<T> / lispListOf<T> | std::vector<TypeOf(T)> |
| tensor/shapedTuple | std::tuple<TypeOf(items)...> |
| mapKV | std::vector<std::pair<KeyT, ValT>> in wire order |
| EnumRef | td::RefInt256 with bigint-valued free helpers, not enum class. This remains RefInt256 when encoded_as is at most 64 bits, avoiding separate free-function signatures without changing member shape |
| StructRef (concrete or monomorph) | the generated struct type |
| union | std::variant<TypeOf(variants)...>; nullLiteral and void variants use std::monostate |
| int/slice/builder/callable/unknown | field not representable; emit the struct with representable members only and stub affected directions as described below |

Field types come from the kernel's cell-side `struct_fields_of` resolution,
which substitutes `client_ty_idx` when present. The emitter consumes the
resolved field list, never the raw declared type.

## 4. Example: simple prefixed struct

From lots-of-messages `IncreaseBy` (32-bit prefix; two int32 fields, inc_by with
default 1; all constants come from the ABI). Widths at most 64 bits use the
native integer types and leaves shown in the field mapping above:

```cpp
// _gen.h
struct IncreaseBy {
  static constexpr td::uint64 PREFIX = 305419896ULL;  // from ABI
  static constexpr int PREFIX_LEN = 32;
  td::int64 counter_id;
  td::int64 inc_by;

  struct CreateArgs {                                 // defaults per ABI ConstExpr
    td::int64 counter_id;                             // required (no default)
    std::optional<td::int64> inc_by;                  // default: 1
  };
  static IncreaseBy create(CreateArgs args);

  static td::Result<IncreaseBy> from_slice(vm::CellSlice& cs);   // checks PREFIX
  td::Status store(vm::CellBuilder& cb) const;                   // writes PREFIX
  td::Result<AbiValue> to_abi_value() const;                     // canonical shape, "$" = "IncreaseBy"
};
```

```cpp
// _gen.cpp: bodies call only ton_abi leaf functions
td::Result<IncreaseBy> IncreaseBy::from_slice(vm::CellSlice& cs) {
  TRY_STATUS_PREFIX(load_and_check_prefix(cs, PREFIX, PREFIX_LEN), "IncreaseBy: ");
  IncreaseBy r;
  TRY_RESULT_PREFIX_ASSIGN(r.counter_id, (load_int64(cs, 32)), "IncreaseBy.counter_id: ");
  TRY_RESULT_PREFIX_ASSIGN(r.inc_by,     (load_int64(cs, 32)), "IncreaseBy.inc_by: ");
  return r;
}
```
Field-path error prefixes exactly as shown (`Type.field: `). Store mirrors. No
consumed-fully check inside struct loaders; that check belongs to the caller.

## 5. Unions, enums

Union: `using <Name> = std::variant<...>` (or named alias per use-site type). from_slice
= BAKED dispatch chain from the generation-time ResolvedUnion (same table dispatch_union
uses): exact-T|void form emits empty-check-first + unconditional-T; general form probes
sorted prefixes (implicit: eat + construct; explicit: peek only, variant's from_slice
re-reads), then checks void last. store =
`std::visit` over alternatives; union writes IMPLICIT prefixes only (explicit written by
variant's own store). Dispatch never matches string labels.

Enums use no `enum class` and perform no member-set validation. An enum emits free functions over
`td::RefInt256` (`<Name>_from_slice` / `_store` / `_to_abi_value`) that read and
write the encoded_as scalar and pass the value through unchecked. The reference does
not validate either: `dynamic-serialization.ts:335-341` (unpack) and `:188-197` (pack)
just serialize the underlying scalar. Therefore an out-of-range value must round-trip
rather than fault. Adding validation would break oracle parity, and a vector carrying an
out-of-range enum value proves it.

## 6. Custom serializers

Type declared custom_pack_unpack → generated from_slice/store bodies DELEGATE to the
typed runtime registry: `abi_custom_lookup<T>("<contract>::<decl>")` → not-registered =
runtime error (NEVER compile/link error). to_abi_value for such types = the custom
value's own shape through the registry's third `to_abi_value` hook. Custom
encoders may be non-idempotent; `MyBorderedInt` provides the focused coverage.

## 7. Stub versus hard failure

Per-DIRECTION CantGeneratePackUnpack (field of kind int/slice/builder/callable/unknown/
genericT-unresolved in the (de)serialization closure): emit that direction as a STUB:
```cpp
td::Result<NotPackable1> NotPackable1::from_slice(vm::CellSlice&) {
  return td::Status::Error("NotPackable1: not serializable: field 'a[1]' has type 'int'");
}
```
(One direction can be real while the other stubs, as in OnlyWithPack and OnlyWithUnpack;
gate each direction independently. Do not copy the
reference emitter's pack_to_builder-flag bug.) Decl-level NonStandardDictKey (mapKV key
∉ intN/uintN/address anywhere in the file) → ton-abi-gen HARD FAILURE with the map-key
error (ErrorsAtCodegen parity, err-invalid-map-key fixtures are the test).
Struct with stubbed directions still emits members for REPRESENTABLE fields when the
struct itself is representable; if member typing impossible (bare int field), emit the
struct with the unrepresentable members OMITTED + both directions stubbed.

## 8. create() + defaults

create(CreateArgs) materializes ABI default_value ConstExprs (int/bool/null/slice(hex
bits)/address/string → constants; tensor/shapedTuple/object → nested braced init;
castTo → inner). Unsupported-default field (isDefaultValueSupported analog: union/mapKV
defaults) results in a required argument because the reference silently omits the
default. LotsOfStorage's default-fill test validates against
StWithAllDefaults incl. the big bigint.

## 9. to_abi_value

Per struct: builds AbiValue::Struct{"$"=original decl name, fields in resolved order}
via leaf-value→AbiValue converters in AbiGenSupport.h (int→Int, AbiAddress→Address,
vector→List, variant→(Union kind only for has_value_field variants; struct-labeled
variants contribute their inner struct's to_abi_value directly), optional
absent→Null, CellOf→CellOf kind, tuple→List, map→Map). Output must byte-match the
reference interpreter's dump on identical wire input.

## 10. Emission order + determinism

Emit ALL declarations (TS parity), topologically sorted (dependencies before users;
cycle only possible through cellOf/ref; forward-declare and emit structs in declaration order with
out-of-line bodies, so topology is only needed for member-type completeness: sort by
dependency, ties by declaration index). Byte-stable: no timestamps, iteration over
model vectors in index order, fixed formatting. Banner: `// Generated by ton-abi-gen
from <fixture>.abi.json. DO NOT EDIT (regen: ton-abi-gen --check)`.

## 11. CLI

`ton-abi-gen <abi.json>... --out-dir <dir>` (writes pairs) | `--check` (regen to temp,
byte-compare, exit≠0 on drift) | errors to stderr, non-zero exit. The CLI test
checks that its output contains the expected struct signature.
