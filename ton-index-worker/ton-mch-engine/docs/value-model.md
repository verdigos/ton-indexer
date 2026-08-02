# Runtime value model

`Value` is shared by expression evaluation, generated builds, host functions,
lookups, and action serialization. Values are immutable after construction;
lists and maps use shared payloads so copying a `Value` remains inexpensive.

Expression integers and integer-backed amounts use `td::RefInt256`. Inputs with
narrower integer types are widened on entry. Fixture-only floating-point amounts
can be rendered and compared structurally but fault when used by the expression
language, which has no floating-point operations.

An account can hold a normalized raw `workchain:HEX` address or a distinct
`addr_none` state. `addr_none` represents a present address object with no
address and is not a null `Value`. Assets distinguish TON from jetton masters.
Comparing a jetton asset to a string is case-insensitive; comparing TON to a
string faults because the operation requires a jetton-master address.

`Dict` and `Obj` store the same field representation but expose different
semantics. A `Dict` supports key lookup. An `Obj` supports attribute access and
the message-envelope `exit_code` accessor. A `Block` is a non-owning reference
whose arena must outlive every value that contains it.

Cell-derived output is compared and rendered by root hash, not serialized BOC
container bytes. Serialization therefore uses the deterministic native writer;
container ordering does not affect value equality.

Expression faults are explicit `EvalResult` values rather than exceptions.
Generated expressions and the tree-walking interpreter call the same runtime
operations. Strict operations implement their own null behavior, while lazy
operators retain control of evaluation at the call site.
