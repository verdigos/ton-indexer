# MCH quality-of-life gap journal — jvault declarative rewrite

> This file has two legs. **Part A** (below) is the Python authoring journal.
> **Part B** ("C++ authoring leg", near the end) journals porting the same jvault
> work into the C++ engine (`ton-index-worker/ton-mch-engine`), with a
> Python-vs-C++ friction comparison. Read Part A first — Part B's headline
> finding (the host-fn representation wall) is the C++ face of Part A's F1.

**Author context:** rewriting the jvault family (`specs/jvault.mch`: stake /
unstake / unstake_request / claim) from the consumed-set builder tier to a fully
declarative M4 build, at full effort, using every language feature now
available. This is a dogfooding journal — every friction hit while authoring is
logged below, classified, with the current form, the form I wanted, and the
blast radius (jvault-only vs every future matcher).

**Outcome (code):**

| Matcher | Result | Host escapes |
|---|---|---|
| `jvault_stake` | declarative | 2 host fns: `jvault_transfer_amount` (raw leg amount), `jvault_stake_period` (forward_payload sub-cell) |
| `jvault_unstake` | declarative | 1 record lookup `jvault_assets` (G10) |
| `jvault_unstake_request` | declarative | shares `jvault_assets` |
| `jvault_claim` | declarative | 2 host fns: `jvault_transfer_amount`, `jvault_account_list` (constructor-map) |

No matcher stayed on its builder. Zero `build` directives remain. Tests:
`tests/mch/integration/test_jvault.py` 11 passed (strict field/flag/value_flow/
consumed-node parity, adapted to the wave-B btype+dict rule); parity sweep 1197
passed / 2 skipped; full `tests/mch tests/mch_ir tests/test_actions.py` 1936
passed / 2 skipped (baseline unchanged).

Files touched: `indexer/events/mch/specs/jvault.mch`,
`indexer/events/mch/builders/jvault.py`,
`indexer/events/blocks/utils/block_tree_serializer.py` (the four `_fill_jvault_*`
flipped to `_dget`, sanctioned), `tests/mch/integration/test_jvault.py` (assert
adaptation).

The record-returning `jvault_assets` lookup and `msg.exit_code` were the two
features that carried the most weight and worked exactly as specified — the
frictions below are everything that fought me on the way.

---

## Frictions

### F1 — `parse` on a composed btype capture silently does not round-trip
**Class:** diagnostics + missing accessor. **Blast radius:** every future matcher.

The single biggest friction. I initially wrote, per the spec's own §14.4 idiom:

```
parse transfer as JettonTransfer      # transfer is `btype jetton_transfer @transfer`
out { staked_amount: transfer.body.amount }
```

`transfer.body.amount` came back **null**. `parse` calls
`MessageType(block.get_body())`, and `Block.get_body()` reads
`event_nodes[0].message.message_content.body`. For a *composed* `JettonTransferBlock`
(built by a prior matcher) that body does not re-parse as a `JettonTransfer`:
`_soft_parse` (`mch_ir/program.py`) swallows the exception and stores `None`, so
`.body` null-propagates and the field silently becomes null. Same failure for
claim's `withdraw.body.amount`. I only found it because the parity test failed on
`staked_amount: ir=None legacy=12266000000000` — nothing pointed at the parse.

The composed block's *already-parsed* protocol message
(`block.jetton_transfer_message`, a real attribute holding the correct int
`amount` and `forward_payload`) is **invisible to the expression layer** —
`_access` exposes only `msg / body / data / failed / broken / btype`. So the only
way to reach a leg's transfer scalar is a host fn that takes the block and reads
the attribute:

```python
async def jvault_transfer_amount(b): return b.jetton_transfer_message.amount
```

**Current form:** `parse leg as JettonTransfer` → `leg.body.amount` → silent null;
fall back to a host fn per scalar.
**Desired form:** either (a) `parse` on a typed btype capture should parse the
block's canonical protocol message, not raw `get_body`; or (b) an accessor that
reaches a composed block's typed message fields (`leg.transfer.amount`); or (c)
have jetton blocks expose the raw scalars in `.data` (they only expose the
`Amount`-wrapped form — see F2). Any of the three retires a whole category of
host fns.
**Blast radius:** every matcher that composes on `jetton_transfer` / `jetton_burn`
/ swap legs and needs a scalar off the leg (amount, forward_amount, query_id).
This is the common case for swap/liquidity/staking families, not a jvault
peculiarity.

### F2 — no `Amount` → int unwrap in the expression language
**Class:** missing stdlib helper. **Blast radius:** every future matcher.

Tightly coupled to F1 and half its cause. The composed jetton_transfer's `.data`
*does* carry the amount — as `data.desired_amount = Amount(raw_int)`. But the
legacy `JVaultStakeData.staked_amount` / `JVaultClaimData.claimed_amounts` fields
are **raw ints**, and there is no way to turn an `Amount` back into an `int` in
an expression. `amount(x)` wraps to `Amount`; there is no inverse. The only trick
available is `data.desired_amount + 0` (integer arithmetic coerces `Amount → int`
and returns a plain int), which is an obscure hack no author would guess.

Had `.value` (or an `int(x)` builtin) existed, `staked_amount:
transfer.data.desired_amount.value` and `claimed_amounts:
[withdraw.data.amount.value]` would have been fully declarative with zero host
fns — F1's host fn exists *only* because F2 forced it.

**Current form:** no unwrap; `x + 0` coercion hack, or a host fn.
**Desired form:** `.value` accessor on `Amount`, or an `int(x)` builtin (both
trivial and C++-clean: `Amount` is `{value:int}`).
**Blast radius:** every output field that is a raw int sourced from a
jetton/DEX block's `Amount`-typed `data`.

### F3 — `produces ClassName` silently mis-normalizes when the class is not in `btypes.py`
**Class:** registration boilerplate + docs gap. **Blast radius:** every ported matcher.

I first wrote `produces JVaultStakeBlock` (following `evaa.mch`). It compiles and
runs, but `mch/btypes.py::CLASS_TO_BTYPE` has no jvault entry, so
`produces_btype("JVaultStakeBlock")` falls through to the *class name string* —
the produced block gets `btype == "JVaultStakeBlock"`, not `"jvault_stake"`. The
serializer and the legacy blocks key on `"jvault_stake"`, so this is a silent
btype mismatch downstream, with no compile error. The fix is to use the bare
string `produces jvault_stake` and register `block_types["jvault_stake"]` — the
nominator idiom. Which of the two forms to use is discoverable only by reading
`btypes.py` (whose own comment says "POC: static map; M5 resolves via the real
block-type registry").

**Current form:** hand-maintained `CLASS_TO_BTYPE` side table; a missing entry is
a silent wrong-btype, and the workaround (bare string) is undocumented.
**Desired form:** resolve class → btype from the registered block class's instance
`btype` at emit time (the block classes already carry it:
`JVaultStakeBlock(...).btype == "jvault_stake"`). Then `produces JVaultStakeBlock`
would Just Work and the side table disappears.
**Blast radius:** every matcher whose produced class is not already in the map —
a silent, once-per-matcher trap.

### F4 — conditional `failed` needs guard gymnastics because there is no assignment form and `.failed` on a null block faults the boolean
**Class:** syntax sugar + missing helper. **Blast radius:** every branch-conditional matcher.

`failed when` only OR-accumulates; there is no `failed = expr`. For
`unstake_request`, legacy sets `failed = True` on the early branch and
`failed = anchor.failed or request.failed` on the full branch. The natural
transcription

```
failed when early
failed when request_anchor.failed or request.failed
```

is **wrong and would fault**: when `early`, `request` is null, and
`request.failed` evaluates to null, which `or` rejects (`_require_bool(None)`
raises → whole match rejected). So I had to guard every conditional boolean with
the branch condition purely to avoid touching a null capture:

```
failed when (not early) and (request_anchor.failed or request.failed)
```

The `(not early) and` prefix carries no domain meaning — it exists only to
short-circuit before `request.failed`. Same shape appears wherever a
maybe-captured block's flag feeds a boolean.

**Current form:** `failed when (guard) and (real condition)`, guard duplicated
across statements.
**Desired form:** either a `failed = expr` assignment (so the ternary lives in
one place), or null-tolerant boolean coercion (`null` in a boolean context →
`false`, matching the "missing field → false" rule inline `where` already uses),
which would let `request.failed` be written directly.
**Blast radius:** any matcher whose failed/broken depends on an optional
(maybe-captured) block — i.e. most DEX/staking matchers with a fail branch.

### F5 — two-shape single-btype output forces a ternary on every field
**Class:** syntax sugar. **Blast radius:** every early-fail / full-success matcher.

`unstake` and `unstake_request` both produce one btype but with two data shapes
(early = resolve from the lookup + anchor exit code; full = resolve from the
downstream calls). Because `produces switch` only dispatches when the *btype*
differs, a single-btype two-shape matcher can't use it, so **every field** carries
`X if not early else Y`:

```
staking_pool:      account(request.msg.destination) if not early else assets.staking_pool
unstake_fee_taken: (fee.msg.value ?? 0) if not early else null
asset:             withdraw.data.asset if not early else assets.asset
exit_code:         null if not early else unstake.msg.exit_code
```

The `early` discriminant is repeated seven times; a reader has to diff the two
arms field by field to see the two shapes.

**Current form:** per-field ternary keyed on a repeated `let` discriminant.
**Desired form:** a branch-level `out` on the same btype —
`when early => out { … } else => out { … }` (produces switch with a shared
btype), or field-group defaults. Either lets the two shapes read as two blocks.
**Blast radius:** common — most withdraw/unstake/claim matchers have an
"early failure vs completed" split under one action type.

### F6 — soft-parse failures are invisible; no diagnostic channel at author time
**Class:** diagnostics. **Blast radius:** every `parse` statement.

Directly downstream of F1: `_soft_parse` (`program.py`) catches the parse
exception and returns `None` with no log. The first signal I got was a parity
mismatch three layers away (`staked_amount: ir=None`). A single
`logger.warning("parse %s as %s failed: %s")` on the swallow — or a
strict-parse mode for authoring — would have turned a 20-minute detour into an
immediate diagnosis. (By contrast the host-fn path *does* log:
`"host fn jvault_stake_period failed: bitstring underflow"` was what finally told
me the forward_payload wasn't round-tripping — that message was genuinely
helpful, so the asymmetry is stark.)

**Current form:** parse failure → silent null → distant parity failure.
**Desired form:** warn-level log on soft-parse failure, and/or a `parse!`
strict variant that rejects the match loudly.
**Blast radius:** every declarative build using `parse`, especially the first
time an author composes on an unfamiliar block type.

### F7 — nullable-lookup rejection is verbose (no null-safe chaining / `require`)
**Class:** syntax sugar. **Blast radius:** every nullable-lookup matcher.

The G10 record lookup itself is clean and worked first try. But guarding it is
repetitive: "reject unless the record resolved AND the required field is present"
is two statements, and the branch-conditional variant repeats the discriminant:

```
reject when assets == null
reject when assets.asset == null
# and in unstake_request:
reject when early and assets == null
reject when early and assets.asset == null
```

A null-safe field access (`assets?.asset`) plus a `require expr` sugar
(`require assets.asset` ≡ `reject when assets.asset == null`) would collapse each
pair to one line and read as intent.

**Current form:** paired `reject when x == null` statements, discriminant
repeated.
**Desired form:** `require <expr>` sugar and/or null-safe `?.` chaining so a
null anywhere in the chain is a single clean reject.
**Blast radius:** every matcher using a nullable lookup (the sanctioned G10
pattern is meant to be common, so this compounds).

### F8 — declarative conversion forces per-`_fill` serializer edits (`_dget` flip)
**Class:** registration boilerplate. **Blast radius:** every matcher converted to declarative.

Converting a matcher to a declarative build changes its produced `block.data`
from a dataclass to a plain dict, so every `_fill_*` in
`block_tree_serializer.py` that reads `block.data.field` must be hand-flipped to
`_dget(block.data, 'field')` to serve both shapes. Four functions, ~30 lines of
mechanical edits, entirely outside the spec — a hidden cost of "just rewrite the
spec." The `_dget` helper exists precisely for this, but the flip is manual and
easy to forget (it is silent on the legacy path since `_dget` on a dataclass is
`getattr`).

**Current form:** manually flip each `block.data.x` → `_dget(block.data, 'x')`.
**Desired form:** the serializer reads `data` through one uniform accessor
always (make `_dget` the default read path for all `_fill_*`), so converting a
spec touches only the spec.
**Blast radius:** every matcher moved from builder/dataclass to declarative/dict.

### F9 — registration ceremony is duplicated per builder file
**Class:** registration boilerplate. **Blast radius:** every matcher file (each small).

Even after going fully declarative, `builders/jvault.py` still hand-writes a
`BLOCK_TYPES` dict, a `MESSAGE_TYPES` dict, a `register_jvault(reg)` and an
`jvault_registries()` wrapper — the same skeleton every builder file repeats. For
a declarative spec the `block_types` entries are pure ceremony the compiler
demands (`produces` name must be a registered key), even though the produced
runtime object is a generic `Block`. `message_types` likewise just re-exports the
parser classes under their own names.

**Current form:** per-file `BLOCK_TYPES` / `MESSAGE_TYPES` dicts + two wrapper
functions.
**Desired form:** infer `block_types` from the spec's `produces` set (declarative
matchers produce generic `Block`), and register message types by their class name
automatically from the `parse` references. Reduce a pure-declarative file to just
its lookups + host fns.
**Blast radius:** low per file, but every matcher file pays it.

---

## Positives worth pinning (so they are not "fixed")

- **Record-returning lookup (G10)** — `let assets = lookup jvault_assets(addr)`
  then `assets.staking_pool` / `assets.asset` is exactly the right shape for
  "multi-value async resolution." First real use; it collapsed a 30-line
  two-hop `extract_jvault_assets` into one build line + field reads. Keep as is.
- **`msg.exit_code`** — `unstake.msg.exit_code` reached transaction state with no
  host fn, exactly as designed. Both early-branch exit-code fields fell out for
  free.
- **List literal + host-fn element** — `claimed_amounts: [ jvault_transfer_amount(withdraw) ]`
  composed a one-element list around a host call with no ceremony.
- **Host-fn error logging** — the `EvalError` wrap that surfaced
  `"host fn jvault_stake_period failed: bitstring underflow"` was the one
  diagnostic that actually pointed at the real problem (contrast F6).

---

## Top 5 by leverage

1. **F1 — composed-block `parse` round-trip + reach the typed message.** Highest
   leverage by far: it blocks the natural way to read any scalar off a composed
   jetton/DEX leg, and it fails *silently*. Every swap/liquidity/staking matcher
   that composes on legs hits it. Fixing it (parse the canonical message, or
   expose the typed message) plus F2 would let the jvault stake/claim host fns
   disappear entirely.
2. **F2 — `Amount` → int unwrap (`.value` / `int(x)`).** Trivial to add, and it
   is half the *reason* F1 needs host fns. Together, F1+F2 are the difference
   between "declarative with 0 host fns" and "declarative with 3."
3. **F3 — class→btype resolution from the registry.** A silent wrong-btype trap
   that every ported matcher can fall into; the fix is already the stated M5
   plan and removes a hand-maintained side table.
4. **F4 — conditional `failed`/`broken` (assignment form or null-tolerant
   bools).** Removes meaningless guard prefixes from every branch-conditional
   matcher and eliminates a real fault footgun (a bare `.failed` on a maybe-block
   rejects the match).
5. **F5 — same-btype two-shape `out` selection.** The early-fail/full-success
   split is the single most common matcher shape after linear transfer; a
   branch-level `out` would DRY the per-field ternaries that currently obscure
   it.

(F6 diagnostics and F7 `require`/`?.` are close behind and cheap; F8/F9 are pure
boilerplate cleanups with broad but individually-small impact.)

---

# Part B — C++ authoring leg (ton-mch-engine)

Porting the jvault work into the C++ engine (the "author a matcher in C++ today"
experience). Same classification as Part A; a Python-vs-C++ comparison closes it.

## Per-piece outcome

| Matcher | C++ outcome | What it needed |
|---|---|---|
| `jvault_unstake` | **ported** (declarative, A/B diff 0) | `JVaultUnstakeJettons` parser + `jvault_assets` record lookup |
| `jvault_unstake_request` | **ported** (declarative, A/B diff 0) | `JVaultUnstakeRequest` parser + `jvault_assets` lookup |
| `jvault_stake` | **walled off** (SKIP both sides) | host fn reads legacy `.jetton_transfer_message` — no C++ analog (C5) |
| `jvault_claim` | **walled off** (SKIP both sides) | same host-fn wall + a hashmap-of-addresses parse |

Neither ported matcher needs a host fn — they are `parse` + `lookup` + builtins
only. The two that *do* need host fns are exactly the two that cannot be ported.

**Classify coverage: 15/35 -> 17/35.** Gate line (all green): pytest 1936 passed /
2 skipped; vectors 91/91 x2; build-vectors 36/36; parse-dump A/B diff 0; match A/B
diff 0; testbed classify A/B diff 0 (6 jvault ACTIONs: 2 unstake + 4
unstake_request); contract classify A/B diff 0 (6 jvault ACTIONs + 2 jvault SKIPs).

**Files touched (C++ tree):** `src/parse/ParseJvault.cpp` (new),
`src/parse/Parsers.h`, `src/MsgParse.cpp` (2 registry rows), `src/BuildRuntime.cpp`
(`jvault_assets` in `FixtureLookupSource::kinds()` + a ~90-line `fetch()` branch),
`CMakeLists.txt` (1 source), `ir/mch_ir.json` (regenerated — the declarative
rewrite made the committed contract stale, see C6). Twin: scratchpad
`classify/py_classify.py` surface lists. No host-binding file was needed. Not
committed.

## The decisive finding (C5, stated up front)

`jvault_stake` and `jvault_claim` cannot be ported, and the reason is not a missing
C++ feature — it is that my own Python host fns are un-portable *by construction*.
`jvault_transfer_amount` / `jvault_stake_period` (Part A, F1) read
`block.jetton_transfer_message`, a **legacy `JettonTransferBlock` attribute**. The
IR engine — the only thing the C++ engine mirrors — produces *generic* blocks with
a `.data` map and no such attribute. So in a fully-declarative pipeline these fns
raise `'Block' object has no attribute 'jetton_transfer_message'` and the match
rejects **in Python too**; I verified it directly on the declarative corpus. Part
A's jvault parity test only passed because it wired *legacy* jetton prerequisites,
which the declarative classify corpus never has. The C++ port didn't hit a C++
limitation here — it surfaced that those Python matchers were quietly
non-declarative. Left as SKIPs (bindings unregistered -> both sides skip -> parity
holds trivially).

## Frictions

### C4 — a record lookup that does real work does not fit the fixture-source model
**Class:** missing abstraction / dual-impl liability. **Blast radius:** every non-trivial lookup. **(top gap)**

Every existing `FixtureLookupSource` kind is a one-liner: map lookup + project a
few fields (`iface_obj`). `jvault_assets` (the Part A G10 record lookup) is not a
data source — it is *imperative logic*: base64-decode a nested `data_boc` string,
BOC-deserialize, open a `CellSlice`, `load_address_py` twice, upper-case, then
chain a second extra-data read (pool -> `lock_wallet_address`) and a
`jetton_wallet` read (lock wallet -> `jetton`), with three distinct outcomes —
Null / partial record / full record — each hand-mapped to Python's
`try/except: return (None,None,None)` sentinel. That is ~90 lines inside `fetch()`
reproducing `extract_jvault_assets` byte-for-byte in a second language. The
`lookup` abstraction assumes the host just *fetches* a value; the moment a lookup
computes (parse + chained resolution), the C++ side is a full re-implementation
whose parity rests on replicating Python's exception-to-None mapping exactly. This
is the single largest C++ authoring cost and the maximal form of the "each host
binding is a dual-implementation parity liability" warning.
**Desired:** either (a) lookups stay *pure fetch* and the chaining/parse lives in
the language (needs `let x = lookup ...` results to feed further lookups — today
blocked by `B004_LOOKUP_DEPENDS_ON_LOOKUP`), or (b) a shared cross-language
"resolver" description so the BOC-walk + chain is written once. Neither is cheap;
the point is that G10 record lookups quietly carry the highest parity tax.

### C5 — host fns reach block internals that have no C++ representation
Covered above. **Class:** substrate mismatch. **Blast radius:** any host fn reading a
Python block object beyond the `.data` subset. The C++ `Block` is `{btype, data
(map subset), topology}`; Python host fns can reach any attribute of the concrete
block class. Anything they read that isn't in `.data` is unportable — and, as C5
shows, often means the matcher was never truly declarative.
**Desired:** a lint/gate that flags a matcher whose host fn reads outside the
declared `.data` + envelope surface, so the wall is caught at authoring time in
Python, not discovered at C++ port time.

### C2 — three-plus parallel twin surface lists, hand-synced
**Class:** twin-sync ceremony. **Blast radius:** every binding added. **(top gap)**

To register a parser/lookup in C++ I must *also* hand-edit the Python twin's
surface sets — `CPP_PARSERS`, `CPP_LOOKUP_KINDS`, `CPP_FNS`, `CPP_PREDS` — which
the twin uses to compute a skip table matching what C++ skips. These sets are
duplicated across `py_classify.py`, `py_match_dump.py`, `py_build_smoke.py`,
`py_msg_dump.py`. Miss one and the twin diverges from C++'s actual registries ->
false A/B diffs that look like port bugs. No single source: C++ computes
runnability from its real registries; the twin re-declares that surface by hand.
**Desired:** the exe emits its registered surface (a `--surface` dump of
parser/lookup/fn/pred names); the twin reads that instead of a hardcoded copy.

### C6 — dual-artifact regen ceremony, and a silent stale-contract break
**Class:** build/gate ergonomics. **Blast radius:** every spec change that alters a matcher's build tier.

Landing the port touched *two* artifacts: the testbed (`classify_ir.json`, pointed
at by `MCH_TESTBED_IR`) and the committed contract (`ir/mch_ir.json`). The Python
declarative rewrite silently **broke the contract**: the committed `ir/mch_ir.json`
still referenced `build_jvault_stake`, a builder I had deleted, so the twin's
`build_matchers` threw `IrLoadError` — discovered only at gate time, three steps
removed from the edit. Re-syncing means: emit_dir -> overwrite the testbed json AND
regenerate the contract json -> rebuild (which regenerates three baked codegen
tables selected at runtime by source-sha) -> run *both* classify gates. Nothing
warns at rewrite time that a committed artifact has gone stale.
**Desired:** an artifact-staleness check (the `ir/` copies vs the current frontend
emit) in one command, ideally a single "sync + rebuild + gate" script.

### C3 — trivial parsers are boilerplate a TL-B/ABI generator would erase
**Class:** parser-adapter boilerplate. **Blast radius:** every op+scalars message.

`parse_jvault_unstake_jettons` / `_request` are pure ceremony: `open_body`,
bit-check, skip 32, fetch u64, `load_coins_py`, `emplace_back` x2, `make_obj` — and
both bodies are *identical* (I collapsed them to one helper). This is squarely the
"clean TL-B" tier of `docs/mch-tlb-codegen-study.md`: a generated Record->Value
adapter would remove them. (Contrast C4: no generator helps the lookup — it is
irreducibly imperative.) The friction is that the trivial 60% and the irreducible
40% share one hand-written surface with no tooling boundary between them.
**Desired:** wire the TL-B codegen tier for the mechanical parsers so hand-written
`Parse*.cpp` shrinks to the genuinely-quirky families.

### C7 — gate-running ergonomics: no one-shot harness, CRLF + path traps
**Class:** testing ergonomics. **Blast radius:** every C++ change.

Seven gates, each a manual `exe | sed 's/^[^\t]*\t//' > a; twin > b; diff a b`. Two
traps cost real time: (1) **CRLF** — the exe emits `\r\n`, the twin `\n`, so a
plain `diff` reports *every* line changed; my first classify run looked like a
total failure until I recalled `--strip-trailing-cr`. (2) **path forms** — the exe
accepts `S:/...`, but a Git-Bash `/s/...` path handed to Windows Python silently
`FileNotFoundError`s, which can make a twin produce empty output and a diff
*falsely pass*. No single "run all gates, report pass/fail" script exists; the gate
list lives in prose in `docs/mch-cpp-plan.md` section 1.
**Desired:** a `run_gates.sh` that runs all seven with the right strip/paths and
prints one pass/fail line each.

### C1 — the "add a family to C++" workflow is undocumented as a checklist
**Class:** docs gap. **Blast radius:** every new family.

No single "to add a parser/lookup, edit these N places" doc. I reconstructed the
path from memory notes plus reading eight files: the `Parsers.h` split-decl
convention, the `MsgParse.cpp` registry row vs the separate `kDumpTypes`, the
`PSlice` primitive set, `HostImpls.h`/`HostRegistry.cpp`, the `FixtureLookupSource`
dispatch, the CMake source list, the twin surface lists, and the
testbed-vs-contract sha mechanism.
**Desired:** a short `docs/mch-cpp-add-family.md` checklist mirroring the Python
`add-matcher` skill.

### C8 — fixture interface-data discovery is a manual msgpack dig
**Class:** docs/tooling gap. **Blast radius:** every lookup-bearing matcher.

Deciding whether `jvault_assets` was fixture-backable meant writing a throwaway
script to msgpack-unpack the `.lz4` `interfaces` section and eyeball its shape —
which revealed the non-obvious nested
`interfaces[acct]["data_boc"]["data_boc"]` (a b64 BOC string) plus a sibling
`lock_wallet_address`, present only on the unstake/request fixtures. Nothing
documents what interface shapes each fixture bundles or what `get_extra_data`
returns.
**Desired:** a `--dump-interfaces <fixture>` mode (or a doc) so a lookup author can
see the available shape without writing a probe.

## Top 5 C++ gaps by leverage

1. **C4 — record lookups that compute don't fit the source model.** Biggest
   authoring cost and the maximal dual-impl parity tax; every future G10 lookup
   pays it.
2. **C5 — host fns reaching block internals with no C++ analog.** Not just hard to
   port — it means the matcher isn't really declarative; needs an authoring-time
   gate so the wall is caught in Python.
3. **C2 — the hand-synced twin surface lists.** Pure drift risk on every binding;
   fixed cheaply by an exe `--surface` dump.
4. **C6 — dual-artifact regen + silent stale-contract break.** A spec change can
   silently invalidate a committed artifact, surfacing only at gate time.
5. **C1 + C7 — no add-a-family checklist and no one-shot gate harness** (with the
   CRLF/path traps that make both failures and false-passes easy).

## Python vs C++ friction comparison

| Dimension | Python leg | C++ leg |
|---|---|---|
| Cleanest matchers | stake/claim (no lookup) | the **opposite**: unstake/request (lookup) port; stake/claim walled off |
| Headline gap | F1 — composed-block internals invisible to the language | C5 — those internals have **no C++ representation at all** (the wall Python hid) |
| Record lookup (G10) | one clean `let assets = lookup ...` line | ~90 lines of imperative BOC-walk + chained reads byte-matching Python |
| Host fn cost | a small Python fn | often *unportable* (reaches block internals) — the real ceiling |
| Trivial parsers | free (parser classes exist) | hand-written boilerplate (C3), TL-B-generatable |
| Iteration loop | `pytest` (seconds, one command) | edit -> MSVC rebuild (minutes) -> 7 manual gates with CRLF/path traps |
| Sync surface | none | 3+ parallel twin lists (C2) + 2 artifacts + 3 baked tables (C6) |
| Diagnostics | soft-parse swallows silently (F6) | worse: a stale committed artifact breaks only at gate time (C6) |
| Net | language ergonomics (sugar, unwraps) | *parity infrastructure* — the cost is keeping two engines provably identical, doubled by every host escape |

The through-line: in Python the frictions are language ergonomics; in C++ they are
the tax of a second engine that must stay byte-identical. Every host binding is
where that tax concentrates — and a host binding that reaches past `.data` (C5) or
computes inside a lookup (C4) is where it becomes a wall rather than a cost. The
language features that retire host bindings (Part A F1/F2, the constructor-map
G-LC) are therefore worth *double* on the C++ ledger.
