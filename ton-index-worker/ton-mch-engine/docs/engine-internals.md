# Engine internals

## Match and build ordering

Matching is read-only. A matcher walks the existing block tree, records captures
in stable slots, and returns the consumed block set. Inline `where` expressions
run only after the node's kind or opcode anchor succeeds. Child accumulation,
ordered alternatives, excess absorption, recursive frontier walks, parent edges,
and capture-frame alignment are all resolved before the build phase begins.

Each matcher gets a fresh breadth-first traversal. A produced block is not seen
by the matcher that created it because that traversal has already read its
ancestor's outgoing edges, but later matchers see it. Consumed blocks retain
their outgoing edges; detaching a block prevents another build attempt without
removing its descendants from traversal.

The block substrate keeps outgoing edges in stable ascending `min_lt` order after
construction and merge. Frontier discovery deduplicates blocks, and connection
compaction preserves first-seen order. Matching itself never mutates the tree.

Generated build functions execute against capture slots rather than names. A
fault or an explicit rejection is a clean matcher rejection. Successful output
is merged and shaped before a produced block is inserted into the working tree,
so later matchers can consume it. Classification retains raw auxiliary and flow
data for adapters instead of applying output formatting inside the core.

The core exposes two result views. `actions` records matcher fires in order for
the diagnostic classifier dump. `action_rows` records the post-processing spine
and recursive child rows used by production serialization. Composite blocks that
were consumed by later matchers can remain in the first view but not the second;
unconsumed basic blocks and ghost external-message rows can appear only in the
second.

## Two-phase lookups

A build first runs with a collecting lookup table. Known values are returned and
misses are recorded. The requested values are fetched, then the build repeats
until no new lookup keys appear; this is required when one lookup's arguments
depend on another lookup. The final run uses an immutable filled table.

Production lookup resolution has two tiers. The first maps interfaces already
computed for the current block into the same value shapes used by fixtures. The
second is an injectable resolver for persisted account data and chained reads.
Nominator-pool and JVault asset lookups are second-tier-only. A miss in both
tiers returns null.

Lookup keys frame the kind, argument count, value type, payload length, and
payload. Framing prevents embedded NUL bytes, different arities, or equal text in
different value types from colliding.

Only final-pass host rejections are observable. Collecting passes intentionally
lack some values and may reject while discovering dependencies, so logging them
would report false failures. Scoped context arms rejection logging only for the
final pass and restores the previous state on exit.

## Action serialization

Each supported produced block type maps to an action fill function. The fill
functions preserve the wire-facing field names and value shapes expected by the
consumer, including fields that are always null but remain part of the decoded
map contract. Unknown produced block types are skipped. A successful
classification with no actions emits one synthetic unknown action whose ID is
the trace ID; normal action IDs are derived from hashes.

Recursive serialization skips an unsupported block together with its children.
A child with its parent's action ID is suppressed, and contract-deployment
children are omitted because deployment unwinding already promotes them to the
top-level spine. Ancestor types accumulate outward through recursion and are
sorted and deduplicated before emission.
