# MCH spec authoring notes

This note collects matcher-language rules that are easy to miss when reading a
single compiler call site.

## Anchors and chain cursors

An `entry` capture normally identifies one concrete `op`, `btype`, or `pred`
node. It may instead cover every branch of one exclusive alternative. In that
form the entire alternative is the anchor, so edges around the group behave as
if they surrounded one node. Every branch must be a bare concrete `op` or
`btype` node. The branches may mix the two kinds and may use named `where`
predicates; membership is checked before the predicate on the matching branch.
Inline `where (expr)` clauses are not supported on group-anchor branches.

A children block, such as `{ op B; maybe op C }`, attaches all of its items to
the node immediately on its left. It does not advance the chain cursor.
Consecutive children blocks attach to that same node in source order. Thus in
`op A -> { op B; op C } <- op P`, the parent edge still belongs to `A`.

An ordinary child node does advance the cursor. A parent edge after such a
child, as in `op A -> op B <- op C`, cannot be represented faithfully: the
lowered parent test would inspect `A` while expecting `C`. The compiler reports
`C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED`; re-anchor or split the pattern.
A children block also cannot appear without a node to its left. Bare forms are
reported as `C008_CHILDREN_BLOCK_BARE`.

## Alternative captures

The same capture name may appear in different branches of one exclusive
alternative when those positions cannot match together. The capture is
required only if every branch of a common exclusive alternative binds it and no
occurrence is under `maybe`; otherwise it is optional. Repeated bindings from
recursion are list-valued and preserve traversal order.

Do not place a capture after a direct-recursive rule reference in the same
sequence. The walker cannot reach that position after recursion deduplication,
so the compiler reports `C007_CAPTURE_AFTER_RECURSION`.

## Recursive rules

Frontier recursion tries the exit branch first for each candidate, takes at
most one step per round, and uses the stepped blocks' children as the next
frontier. A direct-recursive frontier rule needs a terminating, `maybe`-guarded
self-reference.

Use `rule NAME cyclic = ...` when alternatives with the same prefix must be
distinguished by looking farther down the actual chain. Cyclic rules lower
self-references to graph back-edges and are bounded by trace depth. Their bodies
cannot contain captures, a self-reference must be the last atom of its chain,
and self-references cannot be wrapped in `maybe`. A non-recursive alternative
provides termination. Violations report `C012_CYCLIC_RULE_SHAPE`.

## ABI-backed message parsers

An ABI-backed `parse` capture must sit behind the matching `op` guard. The C++
ABI parser validates the opcode prefix, while Python hand parsers conventionally
skip the first 32 bits. Without the guard, the same body can fail in C++ but
produce a meaningless Python object.

Python parser twins must also match the values produced by the ABI adapter:
integers and enums become `int`, booleans become `bool`, standard addresses
become `AccountId`, cells remain cells, lists remain lists, structures become
field dictionaries, and maps become wire-ordered lists of `[key, value]` pairs.
The pair-list representation is required because map keys may be integers or
addresses. Preserve ABI field names and declaration order.
