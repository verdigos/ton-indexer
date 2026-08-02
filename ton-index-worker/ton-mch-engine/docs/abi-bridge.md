# ABI bridge

The ABI bridge converts generated `ton-abi` values into the MCH value model and
registers generated message parsers beside the hand-written parsers. The
dependency is one-way: the MCH engine depends on `ton-abi`; `ton-abi` does not
depend on the engine. Registration is compiled into static parser rows, so no ABI
model or schema is loaded at process startup.

Registry keys are bare generated declaration names because the MCH `parse`
syntax names declarations with a single identifier. Generated structs validate
their own prefixes, convert to `AbiValue`, and then pass through the total
`AbiValue`-to-`Value` adapter. Null and exotic top-level cells return parse
errors rather than aborting.

The adapter preserves ABI structure without adding MCH value kinds. Structures
become objects whose first field is the `$` discriminator. Value-carrying unions
become `{ "$", "value" }` objects, while structure-labeled unions retain the
inner structure's discriminator. Maps become lists of key/value pairs. Bit
slices and external-address payloads become cells, and external addresses become
objects containing their bit length and cell value.

Copying a slice into a fresh cell uses the non-throwing append operation. A slice
fits in one empty cell, but the checked fallback keeps the adapter total even if
that assumption is violated.
