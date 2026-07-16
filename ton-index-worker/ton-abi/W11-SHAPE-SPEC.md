# W11 emitted-shape spec (lead-authored, per plan §0.13)

Normative for ton-abi-gen (T11.1). The SHAPES below are binding; literal constants
(prefixes, ty indices, field names) come from the input ABI at generation time. Golden
examples use real fixture decls — validate emitted text against the actual .abi.json.
Plan §0.17/§0.18/§0.19 + wave-log W10 observations are incorporated; read them first.

## 1. Output layout

Per ABI: `generated/<contract_snake>_gen.h` + `_gen.cpp`, namespace
`ton_abi::gen::<contract_snake>` (contract_name → snake, sanitized). Both committed;
`ton-abi-gen --check` regenerates to temp + byte-compares (msgs_gen discipline).
Shared support header `src/AbiGenSupport.h` (hand-written, T11.1): `CellOf<T>` thin
wrapper (`struct CellOf { T ref; }`), typed custom-registry API
(`register_abi_custom<T>` / lookup, §0.17 — runtime registry, NOT specializations),
common includes. Generated files include ONLY AbiGenSupport.h + leaf headers + AbiValue.h
— NEVER AbiKernel/AbiLoader/AbiInterp (kernel runs at GENERATION time; all dispatch
tables are BAKED into the emitted code).

## 2. Identifier rules

- Decl name valid C++ ident → verbatim. Else sanitize: non-[A-Za-z0-9_] → `_`
  (`Storage@me` → `Storage_me`); keep original in the `$`-name string.
- Monomorph instantiation (W10 obs #2 — resolve by TY_IDX, never name):
  `ResetTo<int64>` → `ResetTo_int64` (strip `<>,` → `_`, collapse runs, trim trailing).
  Collision after mangling → append `_t<ty_idx>`.
- Reserved-word / collision with support names → trailing `_`.

## 3. Field type mapping (typed struct members)

| ABI ty | C++ member |
|---|---|
| intN/uintN/varintN/varuintN/coins | td::RefInt256 |
| bool | bool |
| bitsN / remaining | td::Ref<vm::CellSlice> |
| string | std::string (raw bytes; lossy-UTF8 only in to_abi_value dump) |
| address/addressOpt/addressExt/addressAny | ton_abi::AbiAddress |
| cell | td::Ref<vm::Cell> |
| cellOf<T> | CellOf<TypeOf(T)> |
| nullable<T> | td::optional<TypeOf(T)> |
| arrayOf<T> / lispListOf<T> | std::vector<TypeOf(T)> |
| tensor/shapedTuple | std::tuple<TypeOf(items)...> |
| mapKV | std::vector<std::pair<KeyT, ValT>> in wire order |
| EnumRef | generated enum class |
| StructRef (concrete or monomorph) | the generated struct type |
| union | std::variant<TypeOf(variants)...> (nullLiteral variant → std::monostate; void variant → struct <Name>_Void{} tag type emitted alongside) |
| int/slice/builder/callable/unknown | field NOT representable → whole struct gets STUBS (§0.18); still emit the struct decl with a doc comment, members for representable fields omitted — see §7 |

CRITICAL (W10 obs #1, easy to get backwards): field types come from the kernel's
struct_fields_of CELL-side resolution, which SWAPS TO client_ty_idx when present. The
emitter consumes the kernel's resolved field list — never the raw declared ty.

## 4. Golden example — simple prefixed struct (shape normative)

From lots-of-messages `IncreaseBy` (32-bit prefix; int32 inc_by with default 1, int8
counter_id — constants from the ABI):

```cpp
// _gen.h
struct IncreaseBy {
  static constexpr td::uint64 PREFIX = 0x2E4F1D33;   // from ABI
  static constexpr int PREFIX_LEN = 32;
  td::RefInt256 counter_id;
  td::RefInt256 inc_by;

  struct CreateArgs {                                 // defaults per ABI ConstExpr
    td::RefInt256 counter_id;                         // required (no default)
    td::optional<td::RefInt256> inc_by;               // default: 1
  };
  static IncreaseBy create(CreateArgs args);

  static td::Result<IncreaseBy> from_slice(vm::CellSlice& cs);   // checks PREFIX
  td::Status store(vm::CellBuilder& cb) const;                   // writes PREFIX
  td::Result<AbiValue> to_abi_value() const;                     // §0.14 shape, "$" = "IncreaseBy"
};
```

```cpp
// _gen.cpp — bodies call ONLY ton_abi leaf functions (one-semantics)
td::Result<IncreaseBy> IncreaseBy::from_slice(vm::CellSlice& cs) {
  TRY_STATUS_PREFIX(load_and_check_prefix(cs, PREFIX, PREFIX_LEN), "IncreaseBy: ");
  IncreaseBy r;
  TRY_RESULT_PREFIX_ASSIGN(r.counter_id, load_int(cs, 8),  "IncreaseBy.counter_id: ");
  TRY_RESULT_PREFIX_ASSIGN(r.inc_by,     load_int(cs, 32), "IncreaseBy.inc_by: ");
  return r;
}
```
Field-path error prefixes exactly as shown (`Type.field: `). Store mirrors. No
consumed-fully check inside struct loaders (caller's concern — matches reference).

## 5. Unions, enums

Union: `using <Name> = std::variant<...>` (or named alias per use-site type). from_slice
= BAKED dispatch chain from the generation-time ResolvedUnion (same table dispatch_union
uses): exact-T|void form emits empty-check-first + unconditional-T; general form probes
sorted prefixes (implicit: eat + construct; explicit: peek only, variant's from_slice
re-reads), void LAST (W7 semantics — copy the pinned form-dependence). store =
`std::visit` over alternatives; union writes IMPLICIT prefixes only (explicit written by
variant's own store — W7 pin). NO string-label matching anywhere (W10 obs #3).
Enum: `enum class <Name> : <underlying per encoded_as width>` with ABI member values;
from_slice/store via the encoded_as scalar leaf + range validation (member set check —
reference faults on unknown enum value: mirror it).

## 6. Custom serializers (§0.17 + W10 obs #4/#5)

Type declared custom_pack_unpack → generated from_slice/store bodies DELEGATE to the
typed runtime registry: `abi_custom_lookup<T>("<contract>::<decl>")` → not-registered =
runtime error (NEVER compile/link error). to_abi_value for such types = the custom
value's own shape (registry provides to_abi_value hook too — 3-fn registration).
Doc-comment on the generated decl: "custom-serialized; round-trip not guaranteed
(non-idempotent encoders exist)".

## 7. Stub-vs-fail (§0.18 — binding)

Per-DIRECTION CantGeneratePackUnpack (field of kind int/slice/builder/callable/unknown/
genericT-unresolved in the (de)serialization closure): emit that direction as a STUB:
```cpp
td::Result<NotPackable1> NotPackable1::from_slice(vm::CellSlice&) {
  return td::Status::Error("NotPackable1: not serializable: field 'a[1]' has type 'int'");
}
```
(One direction can be real while the other stubs — OnlyWithPack/OnlyWithUnpack pattern;
gate per direction independently, dynamic-path gate rule §0.19 — do NOT copy the
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
defaults) → NO default, required arg (silently-dropped is the reference behavior for
those — mirror: required). LotsOfStorage's default-fill test (W11.3) validates against
StWithAllDefaults incl. the big bigint.

## 9. to_abi_value

Per struct: builds AbiValue::Struct{"$"=original decl name, fields in resolved order}
via leaf-value→AbiValue converters in AbiGenSupport.h (int→Int, AbiAddress→Address,
vector→List, variant→(Union kind ONLY for has_value_field variants — struct-labeled
variants contribute their inner struct's to_abi_value directly, T8.1 rule), optional
absent→Null, CellOf→CellOf kind, tuple→List, map→Map). Output must byte-match the
interpreter's dump on identical wire input — that IS the T11.2 A/B gate.

## 10. Emission order + determinism

Emit ALL declarations (TS parity), topologically sorted (dependencies before users;
cycle only possible via cellOf/ref — forward-declare + emit structs in decl order with
out-of-line bodies, so topology is only needed for member-type completeness: sort by
dependency, ties by declaration index). Byte-stable: no timestamps, iteration over
model vectors in index order, fixed formatting. Banner: `// Generated by ton-abi-gen
from <fixture>.abi.json — DO NOT EDIT (regen: ton-abi-gen --check)`.

## 11. CLI

`ton-abi-gen <abi.json>... --out-dir <dir>` (writes pairs) | `--check` (regen to temp,
byte-compare, exit≠0 on drift) | errors to stderr, non-zero exit. Cli-analog test
(T11.3): output file contains the struct signature line.
