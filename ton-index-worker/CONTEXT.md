# Context: MCH engine (ton-index-worker)

Ubiquitous language for the C++ classification engine. Glossary only — no implementation.

## Terms

**Hand parser** — a message parser written manually in C++ (`src/parse/`). Being retired
in favor of ABI parsers where faithful; 12 registered today.

**ABI parser / ABI row** — a message parser generated from a Tolk declaration file
(`abi/*.tolk`) via the ton-abi pipeline. 73 registered today.

**Registry** — the named map of message parsers the DSL `parse` expression searches.
Payload-fragment parsers are deliberately NOT in it.

**Payload fragment** — a declared type that describes part of a message body (e.g. a
jetton transfer's forward payload), never a whole message. Parsed only via explicit
helper calls from adapters.

**Semantic adapter** — a hand-written parsing hook that intentionally survives ABI
migration because its behavior is tolerance or bug-compat, not wire schema. End state:
three (jetton transfer tail, DNS change-record, vesting send).

**Tolerance semantics** — deliberately accepting partial or malformed input and
producing a correct-but-incomplete result instead of failing (e.g. vesting inner
message unreadable -> record kept, body hash null). Desired product behavior; distinct
from a legacy bug.

**Legacy bug** — wrong output faithfully ported from the python classifier (wrong wire
decode, corrupted value). Policy: surface to the user, rule fix-vs-preserve; never
silently replicate ("parity, not bugs").

**Projection** — a derived rendering of already-decoded wire data (enum member name,
legacy hex string forms). Lives in a shared projection layer, never in generated
decoders.

**Tier-1 / tier-2 lookup** — interface resolution during classification: tier 1 reads
interfaces captured with the trace; tier 2 falls back to reading account state from
celldb shard states.

**Anycast** — optional address-rewrite prefix in TL-B addresses. ABI parsers reject it
(fail-closed); legacy ignored the rewrite (bug, dies with migration).
