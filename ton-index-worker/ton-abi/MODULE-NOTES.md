# ton-abi module notes

This document collects protocol and implementation details that are easy to
miss when reading one source file in isolation. `W11-SHAPE-SPEC.md` defines the
generated C++ surface, while `ABI-PROTOCOL-CHECKLIST.md` covers adding a new
message family end to end.

## Resolution invariants

`AbiKernel` implements cell-side resolution only. Stack and get-method width
calculation are outside this module.

1. **Union labels belong to a use site.** The same interned union can receive a
   different `uLabelTyIdx` from each generic instantiation. For example,
   `GenericPair<int32,int64>.value` labels its variants `T1|T2`, not
   `int32|int64`. `ResolvedField::u_label_ty_idx` and
   `AliasTarget::u_label_ty_idx` must be threaded into `resolve_union`. Nullable
   intentionally drops this value to match the reference implementation.
2. **Custom serialization is checked before normal resolution.** The kernel
   does not inspect `custom_pack_unpack` and does not collapse custom
   struct/alias/enum nodes into their targets. Consumers must select the custom
   pack or unpack hook before calling `struct_fields_of` or `alias_target_of`.
   The emitter enforces this ordering while generating each direction.
3. **Prefix dispatch depends on loader validation.** The loader rejects union
   variants whose bit strings are strict prefixes of one another. Therefore a
   slice can match at most one prefix, and sorting dispatch entries by
   `(prefix_len, prefix_num)` is equivalent to declaration-order first match.
4. **Only use-site-independent data is cached.** Resolved struct fields, alias
   targets, union dispatch order, and the trailing-void flag are built during
   `AbiKernel::create`. Generic union labels are recomputed for each use site.
   The caches are immutable after construction, so concurrent reads need no
   locking.
5. **Symbol names are not monomorph display names.** `StructRef.struct_name` and
   `AliasRef.alias_name` contain declared names. Display names such as
   `ResetTo<int64>` occur in instantiation records, which are matched by type
   index. Stripping generic syntax during symbol lookup would hide malformed
   input and diverge from the reference.

## AbiValue and canonical JSON

Generated parsers materialize a typed struct and then an `AbiValue`; downstream
bridges may adapt that tree into their own value representation. The extra
intermediate keeps ton-abi independent of downstream consumers and makes the
generated-code conformance vectors compare the same canonical value format.

| Kind | Payload | Canonical JSON |
|---|---|---|
| `Int` | `RefInt256` | Decimal JSON string; enums use the same form |
| `Bool` | `bool` | JSON boolean |
| `Address` | `AbiAddress` | Std: `"wc:hex"`; Extern: `{"extern":{"bits":n,"value":"hex"}}`; None: `"none"` for `any_address` |
| `Cell` | `Ref<Cell>` | Base64 of `std_boc_serialize(root, 2)` |
| `CellOf` | one nested value | `{"ref":<inner>}` |
| `Bits` | `Ref<CellSlice>` | `{"bits":"0101...","refs":[<BOC base64>,...]}` |
| `String` | raw bytes | JSON string after lossy UTF-8 decoding |
| `List` | ordered values | JSON array for arrays, lisp lists, tensors, and shaped tuples |
| `Struct` | name and declared fields | `{"$":"name",...}` with fields in declaration order |
| `Union` | label and nested value | `{"$":"label","value":<inner>}` when `has_value_field` is true |
| `Void` | none | `{"$":"void"}` |
| `Null` | none | JSON null for `nullLiteral`, absent `address?`, and absent nullable values |
| `Map` | key/value pairs | `[[<key>,<value>],...]` in ascending raw-key-bit wire order |

A union variant with `has_value_field == false` contributes its nested value
directly. A struct variant already carries its own `$` label; a `nullLiteral`
variant dumps as JSON null. Every `StructRef` dump carries `$`, even outside a
union.

`CellOf` and wrapped union values hold one recursive `AbiValue` through
`unique_ptr`. A class cannot contain itself directly by value. Standard
containers such as `vector<AbiValue>` and
`vector<pair<AbiValue, AbiValue>>` need no explicit indirection because their
element types are instantiated after `AbiValue` is complete.

Hex produced by `Bits256::to_hex()` and `Cell::get_hash().to_hex()` is uppercase,
but canonical value JSON uses lowercase to match the Node oracle. Map dumping
does not sort: `AbiLeavesDict` already supplies canonical wire order.

### Lossy UTF-8 behavior

Strings retain their raw bytes for pack/store round trips. Lossy UTF-8 decoding
occurs only while dumping JSON and matches Node `Buffer.toString('utf8')`.
Malformed-input coverage includes lone continuation bytes, truncation at every
position in two-, three-, and four-byte sequences, invalid lead bytes, overlong
encodings, UTF-16 surrogate encodings, and code points beyond U+10FFFF.

The replacement grouping is significant. If the first continuation byte fails
its lead-specific range, that byte is reprocessed on the next iteration. Thus
`E0 80 80` yields three replacement characters. Once a continuation byte has
been accepted, a later failure replaces the valid prefix as one unit and leaves
the failing byte for normal processing; `E2 82 41` yields one replacement
followed by `A`.

## Leaf wire-format invariants

The `AbiLeaves*.h` headers expose narrow load/store operations. Composite type
recursion belongs to generated code through callbacks; leaf functions only
move or validate their wire representation.

### Scalars

- `intN` is two's-complement signed and `uintN` is unsigned. Widths up to 64
  bits have native overloads with the same range and error behavior as the
  `RefInt256` path.
- `bitsN` returns exactly `n` bits and no references.
- `varintN` and `varuintN` use a `log2(n)`-bit byte-count prefix, and `n` must be
  a positive power of two. The signed pack byte count is
  `ceil((magnitude_bits + 1) / 8)`, matching @ton/core even at exact powers of
  two such as `-128`. Coins use the same unsigned scheme with a four-bit prefix.

### Addresses

Address tags are `00` none, `01` external, `10` standard, and `11` variable.
`address` accepts only standard, `address?` accepts none or standard,
`ext_address` accepts only external, and `any_address` accepts none, external,
or standard. Truncated bodies, wrong tags, `addr_var`, and anycast are rejected.
A standard body contains an int8 workchain and a 256-bit hash; an external body
contains a uint9 bit length and up to 511 value bits.

### References, strings, and cells

- `cellOf` opens a referenced ordinary cell and leaves inner parsing to the
  caller. Special or exotic cells are rejected both here and at the top-level
  body open.
- Nullable values use one presence bit. `remaining` snapshots and drains all
  remaining bits and references.
- Snake strings store byte-aligned chunks of at most 127 bytes, with at most one
  continuation reference per cell. The leaf returns raw bytes.
- A cell dump uses BOC mode `WithCRC32C`, equivalent to the @ton/core `toBoc()`
  default `{idx:false, crc32:true}`.

### Containers and dictionaries

`arrayOf` stores a uint8 length and a maybe-ref chain built in reverse. The
writer emits one element per chunk. The reader first loads each next-chunk ref,
then drains elements from the remaining bits and refs; this accepts both the
one-element form and compiler-chunked cells. The decoded count must match the
length prefix.

`lispListOf` folds from element zero. Each node stores its element followed by a
reference to the previous tail, and the empty cell terminates the list. Reading
loads the tail reference before decoding the element and then prepends the
element. This inherits the reference implementation's limitation: elements
with their own references do not round trip because the tail is assumed to be
reference zero.

`mapKV` uses HashmapE and accepts only fixed-width `intN`, `uintN`, and address
keys. `vm::Dictionary::check_for_each` yields ascending raw-key-bit order, the
same order used by @ton/core serialization. This is numeric order for unsigned
and address keys; negative signed keys sort after non-negative keys because the
sign bit is high. No dump-time re-sort is needed. Duplicate keys are rejected.

### Prefixes and unions

Union prefixes are limited to 32 bits by the loader. Struct opcode prefixes may
be wider, so leaf functions accept a uint64 prefix number and a width up to the
cell limit; widths above 64 require all high bits to be zero.

Generated union dispatch follows these rules:

- A trailing `void` variant does not participate in prefix probing and matches
  an empty slice.
- For exactly `T|void`, emptiness selects void first; otherwise `T` is selected
  without probing its prefix.
- General dispatch probes non-void variants without consuming. An implicit
  match consumes its prefix; an explicit match leaves the prefix for the
  variant loader to read again. A non-empty no-match is an error.
- Packing writes a union prefix only for an implicit prefix.

## Other reference-compatible behavior

- `tensor` and `shapedTuple` have no leaf; generated code recursively processes
  their `items_ty_idx` in order.
- Generic-union reshaping after monomorphization, non-standard map keys, and
  custom serializers on generics remain unsupported.
- Enums do not validate membership in either direction. An out-of-range encoded
  value must round trip because the reference also treats the encoded scalar as
  unchecked.

## Test and tooling conventions

- td's `check.h` defines `CHECK`, shadowing doctest's macro. Test translation
  units include `test/AbiTestSupport.h` first; it centralizes the macro guard
  and exposes fixture-loading helpers.
- `tools/gen_vectors.mjs` regenerates oracle fixtures and requires a checkout of
  the reference repository. `tools/gen_prod_abi.mjs` compiles production
  declarations and needs only @ton/tolk-js.
- `ton-abi-gen --check` byte-compares every committed generated C++ pair with a
  fresh emission from its ABI JSON.
