# Emulator integration

The emulator bridge creates an immutable trace view without loading cells. A
shared cell-store anchor keeps lazy transaction and action cells readable until
classification and serialization finish. Only the bridge translation unit knows
the emulator's native trace types; the classifier actor uses header-independent
view and result types.

Classification is fail-open. Admission is capped, queued work has a deadline,
and a stalled full queue sets a sticky disabled flag. Shed and disabled outcomes
still carry an explicit state marker, while a null marker means the classifier
did not observe the emission because the feature was off. The decorated sink
invokes the insert sink exactly once on every path.

The action payload contains serialized MsgPack rather than MCH values, so no
arena-backed block reference reaches the insert path. It remains uncompressed:
the consumer recognizes gzip framing by magic bytes and otherwise decodes the
payload directly as MsgPack. Action writes carry the minimum transaction
finality of the emission and cannot overwrite a more advanced stored payload.
Absent or malformed stored-finality values are treated as writable.

MsgPack fields use their natural types when they fit. Integer domains that may
exceed signed 64-bit range are decimal strings. The decision is keyed by field
name at every map depth, which also covers integer elements inside string-list
fields such as `claimed_amounts`. Bounded fields such as opcodes, flags, periods,
endpoint IDs, ticks, exit codes, times, and finality remain integers.

Tier-2 lookups synchronously read the shard states used for emulation. One
classifier invocation owns the lookup memo and budget. Interface results always
pass through the same V2-to-Value adapter as tier 1, so the two tiers have one
value-shape implementation. Budget exhaustion and read failures are clean null
results.

JVault asset resolution follows three account states: the stake wallet identifies
the staking pool and minter, the pool identifies its lock wallet, and the lock
wallet identifies the jetton asset. Raw data-cell parsing is separated from
account access so the caller retains ownership of memoization and budgets.

Operational detail of the Redis wire format, finality lifecycle, and the AAI
account index lives in `ton-trace-emulator/README.md`.
