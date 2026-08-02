# Trace emulator operational contracts

## Redis wire layout

A trace hash is keyed by the base64-encoded normalized external inbound message
hash. Transaction nodes are msgpack values stored under their base64 inbound
message hashes. Interface values use canonical `workchain:HEX` field names. The
hash also carries `root_node`, `depth_limit_exceeded`, and optional classification
fields.

`root_node` contains the base64-encoded original external inbound message hash.
`tr_in_msg:<original-hash>` maps that hash to the normalized trace key for 600
seconds. `new_trace` is published for every inserted trace, and
`new_pending_trace` is published when an inserted node remains pending.
Confirmed and finalized notifications use
`<trace-key>:<comma-separated-transaction-hashes>` payloads. Non-pending
transactions are also announced on `new_commited_txs`.

The Redis `interfaces` field is a stable msgpack variant contract shared with
the Go readers. Detector variants used only by in-process lookup must not be
added to that payload without updating every reader.

## Action payload encoding

The `actions` field is an uncompressed msgpack array. An empty array is a valid
zero-row payload; an absent field means that no action payload was produced.
`mch_classify_state` records the classification outcome, and `actions_finality`
stores the finality used by the write guard.

Action integers that may exceed signed 64-bit range are encoded as decimal
strings. Bounded integers use native msgpack integer types. Composite maps retain
their complete key sets and encode unfilled members as nil, while unfilled
top-level action fields are omitted. Go consumers accept both string and native
integer encodings for scalar fields. Numeric elements of `[]string` fields must
always be decimal strings because element decoding bypasses scalar adapters.

Do not pass this payload through `td::gzencode`: that API produces zlib framing,
while consumers distinguish gzip by its two-byte magic and otherwise decode the
bytes directly as msgpack. If compression is introduced, it must use actual gzip
framing and remain readable by every consumer.

## Finality lifecycle

Finality advances from pending to confirmed to finalized. A trace can be emitted
at each stage. Node writes skip a subtree when the stored node has more advanced
finality, so a later-arriving pending emission cannot replace confirmed or
finalized node data.

The action payload uses the minimum finality of the emission's nodes. That same
value appears inside the payload and in `actions_finality`; both values must come
from the same derivation.

## Atomic action writeback

Trace fields, accepted action fields, account-index updates, and notifications
are queued in the same Redis transaction. Before queueing action data, the
inserter reads `actions_finality` and rejects an emission whose finality is lower
than the observed value. Rejection covers `actions`, `actions_finality`,
`mch_classify_state`, and the related index updates. A missing or malformed
stored finality permits a write, allowing a valid emission to repair the hash.

Do not rely on actor creation order to preserve finality. Separate inserter
actors share a Redis connection pool, and their transactions may execute in a
different order. The finality read precedes the queued transaction rather than
forming a Redis compare-and-set, so maintainers should independently verify its
race behavior before relying on it as the sole concurrency control.

When classification is disabled, the payload state is null. The inserter then
performs no classification telemetry, finality read, action write, index update,
or classification notification. A non-null degraded state is written without an
`actions` field, so consumers report the trace as unclassified while retaining
the failure reason.

## AAI account index

Each action account is indexed in the sorted set `_aai:<workchain:HEX>`. Members
have the form `<trace-key>:<action-id>` and use the trace start logical time as
their score. The member suffix must preserve the action ID byte for byte so
consumers can join index entries to decoded actions.

Accounts referenced by actions but absent from the trace's transactions are
published on `referenced_accounts` as `<workchain:HEX>;<trace-key>`. The TTL
tracker uses these notifications to remove index members when a trace expires.
Both payload components must remain canonical strings that cannot contain the
semicolon separator.

Redis sorted-set scores are IEEE-754 doubles. Logical times above 2^53 therefore
lose precision; consumers must not treat the score as an exact 64-bit integer.

## Invalidation and reemission

Confirmed traces are recorded against the full block identity. When a finalized
block with the same shard coordinates and sequence number has a different block
identity, confirmed traces absent from that finalized block become invalidation
candidates. Publication is deferred until the next finalized block for the same
shard. Candidates reemitted there are not invalidated; the remaining normalized
trace hashes are published individually on `invalidated_traces` as base64.

Reemissions are expected as finality advances. Per-node writes reject lower
finality, while action writes apply the preflight check described above.

Classifier-side invariants for the same pipeline are documented in
`ton-mch-engine/docs/emulator-integration.md`.
