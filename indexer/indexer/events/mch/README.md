# The MCH matcher language

## 1. Why this guide exists and who it is for

A TON trace is a tree of transactions and messages.
The classifier turns that tree into `actions` rows, the structured events every API consumer reads.
A matcher is the rule that recognizes one protocol shape inside the tree and collapses it into one typed block.
Matchers are written in `.mch`, a small declarative language that compiles to tables the C++ engine executes.

Read this if you are adding or changing a matcher.
It walks one real matcher from an unclassified trace to a landed spec with its fixture, its action golden and its gates, then serves as the reference for everything the walkthrough touches.
Everything it states is checked against this tree; no other repository is needed.

## 2. The pipeline in one page

```
.mch specs  ->  frontend  ->  mch_ir.json  ->  mch-codegen  ->  C++ tables  ->  engine
                                                                                  |
                              trace (transactions + messages)  ->  block tree  ->  matching
                                                                                  |
                                                                    produced blocks  ->  action rows
```

The compiler frontend lives in `indexer/indexer/events/mch/`.
It lexes, parses, resolves and compiles every `.mch` file under `specs/` into one IR document.
`indexer/indexer/events/mch/ir_emit.py` emits that document; `emit_default()` compiles the production set in `specs/manifest.yaml` order, `emit_manifest(path)` does the same for any manifest, and `emit_dir(path)` compiles a directory name-sorted for ad-hoc runs.
The CLI form is `python -m indexer.events.mch.ir_emit -o <out.json>`.

CMake runs the emitter during a build of `ton-index-worker/ton-mch-engine/`, writes `mch_ir.json` into the build tree, and passes it through `mch-codegen` to generate the matcher, `where` and build tables as C++ sources.
The engine links those tables.
It never reads `.mch` or JSON at run time, so changing a spec means rebuilding.

At run time the engine builds a block tree from the trace.
Leaf construction makes one `call_contract`, `ton_transfer` or `tick_tock` block per event node and hangs marker blocks off it: `contract_deploy` when the transaction activated the account, `gasless_request` when a relayer paid for a signed wallet request.
Matchers then run in manifest order.
Each matcher gets a fresh breadth-first traversal, tests its anchor, walks its pattern, and on success runs its build program to produce one typed block that later matchers can consume.
`ClassifyCore.h::classify_schema_trace` is the single entry a host calls; it returns one trace outcome, the action rows, the matchers that fired, and a rescue row.

Two hosts run the engine in process.
`ton-index-postgres` classifies confirmed traces under `--enable-classifier` and writes `actions` rows to PostgreSQL.
`ton-trace-emulator` classifies emulated traces and writes msgpack rows to Redis through `RedisActionSerialize`.
Both share the same compiled matcher table, so a spec change reaches both at once.

## 3. Walkthrough: `nft_transfer` from unmatched trace to landed matcher

`indexer/indexer/events/mch/specs/nft_transfer.mch` is the worked example.
Every later section quotes a piece of it.

### 3.1 Look at what the trace classifies to today

Run the product binary over the fixture corpus and read the rows it writes:

```
ton-mch-engine --actions -T indexer/tests/traces -O /tmp/actions \
    --fixtures ton-index-worker/ton-mch-engine/goldens/fixtures.json
```

When a classified trace has no action rows after the ghost external fallback, or payload validation drops every row, the engine emits one synthetic `unknown` row keyed by the trace id.
Unmatched serializable leaf messages can produce bare `call_contract` or `ton_transfer` rows.
Either shape can signal a missing matcher.

### 3.2 Find the anchor

The anchor is the one message whose opcode identifies the protocol event.
For an NFT ownership transfer that is the transfer call itself, opcode `0x5FCC3D14`.
The owner-notification message that follows, `NftOwnershipAssigned`, is a consequence, not an identity, so it belongs in the pattern rather than at the head.

### 3.3 Write the pattern

```mch
matcher nft_transfer {
    produces        nft_transfer
    shape           nft_transfer_parent_absorb
    include_excess  false
    include_bounces false

    op NftTransfer @transfer as NftTransfer {
        -> {
            maybe op NftOwnershipAssigned @assigned as NftOwnershipAssignedPrevOwner
            ; maybe op Excess
        }
        <- maybe peek any @funding_parent
    }
```

The head node carries a relation block, so the children set and the parent edge sit on one node without a chain cursor trap.
`peek` binds the funding parent without consuming it, which leaves it available to the GetGems purchase matcher that runs later.

### 3.4 Write the build program

```mch
    reject when transfer.body == null
    reject when assigned != null and assigned.body == null
    require nft = nft_transfer_data(account(transfer.msg.transaction.account))
    let new_owner = account(transfer.body.new_owner)
    let prev_owner = (account(assigned.body.prev_owner) ?? addr_none()) if assigned != null else (account(transfer.msg.source) ?? addr_none())
```

`reject when` abandons the match so a later matcher may claim the same anchor.
`require` binds a value and rejects on `null` in one statement.
`failed when any_failed(transfer)` then sets the produced block's failure flag, and the `out` block fills the block's `data` map.

### 3.5 Register it in the manifest

`specs/manifest.yaml` lists every matcher name exactly once, top entry first.
`nft_transfer` sits below the jetton family and above the purchase matchers that read the block it produces:

```yaml
  # Build NFT transfers before purchases and sales consume them.
  - nft_transfer
```

A matcher missing from the list fails the build with `R042_MANIFEST_ORDER`.

### 3.6 Build a fixture

From `indexer/`:

```
venv/bin/python -m tests.utils.api_fixture <trace_hash> --out tests/traces/<trace-id>.lz4
```

Then add the fixture to `ton-index-worker/ton-mch-engine/goldens/fixtures.json` with its `group`, `name` and `slug`:

```json
  "mG5NGg-37hpz1Vgs5KvDo0HKgtQKCnMbmGEpdApY5Ls=": {
   "group": "nft",
   "name": "Getgems purchase",
   "slug": "getgems-purchase"
  },
```

A fixture missing from the manifest lands in `misc/<trace-id>.yaml` instead of a named group.

### 3.7 Generate and review the action golden

```
ton-mch-engine --actions -T indexer/tests/traces \
    -O ton-index-worker/ton-mch-engine/goldens/actions \
    --fixtures ton-index-worker/ton-mch-engine/goldens/fixtures.json
```

Review every changed row field by field.
The new matcher shows up as the `matcher:` key on the rows it produced:

```yaml
  - type: nft_transfer
    matcher: nft_transfer
    anchor: nft_transfer@57725394000002
    id: q5pCRDCBRp0QoU9wYs4eYRGAEhcWXKo5UjhN/QVXMcA=
    success: true
```

### 3.8 Run the gates

```
venv/bin/python -m pytest tests/mch -q
ctest --test-dir build/ton-index-worker/ton-mch-engine -R '^mch_'
```

The frontend suite catches compile errors and manifest violations.
The engine suite ends with `mch_actions_golden`, which regenerates the goldens into a scratch directory and diffs them against the committed ones.

## 4. File layout, imports and `manifest.yaml`

One protocol per file, extension `.mch`, encoding UTF-8.
Production specs sit directly under `indexer/indexer/events/mch/specs/`.
Shared declarations live in `specs/lib/` and are only ever imported, never compiled as matcher sources.

Each file is its own namespace.
`import "relative/path.mch"` makes visible exactly the declarations the imported file marks `export`, under their own unqualified names.
There is no alias, no wildcard and no re-export; an imported file's own imports stay private to it.
A name reaching a file twice, from a local declaration and an import or from two imports, is `R041`.

```mch
import "lib/common.mch"
import "lib/getgems.mch"
import "lib/opcodes.mch"
opcode NftTransfer          = 0x5FCC3D14
```

`lib/opcodes.mch` holds the opcodes several protocols share; declare an opcode locally when one file uses it.
`lib/common.mch` and `lib/getgems.mch` hold shared functions:

```mch
export fn getgems_sale(addr) = known_getgems_sale(lookup nft_sale(addr))
```

Top-level declarations are `opcode`, `predicate`, `rule`, `fn` and `matcher`, and they allow forward references.
Two opcodes are seeded into every file before its own declarations: `Excess = 0xd53276db` and `Bounce = 0xffffffff`.

`specs/manifest.yaml` is a flat `order:` list of matcher names, top entry first, each name exactly once.
The list is the run order and therefore semantics, not configuration.
Comments in the file carry the groups and the reason one matcher must precede another:

```yaml
  # DNS, elections, subscriptions and lending.
  # Purchase peeks the raw parent before change_dns consumes it.
  - dns_purchase
  - change_dns
```

Place a matcher below every matcher producing a btype it anchors on or matches.
Place it above any matcher that would otherwise claim its anchor or a block it consumes or peeks.
A new file under `specs/` needs no other registration; only its matcher names go into the list.

## 5. The language tour

### 5.1 Anchors

The head atom of a matcher's pattern is its anchor, and the engine tests it before anything else.
A plain `op`, `btype` or `pred` node anchors directly.
`maybe`, `peek`, `many`, a rule reference, `any` and a non-exclusive alternative cannot head a matcher (`R007`).

An exclusive alternative of bare `op` or `btype` nodes sharing one capture is an eligible head group and lowers to an opcode-set, btype or mixed anchor.
The opcode-set spelling is the readable one:

```mch
    op [JettonMint, MinterJettonMint] @mint as MinterJettonMint | JettonMint
```

### 5.2 Nodes, captures and predicates

| Form | Matches |
|---|---|
| `op IDENT` | a `call_contract` block with that opcode |
| `btype IDENT` | a block whose btype equals `IDENT` |
| `pred IDENT` | a block satisfying the named host predicate |
| `op [A, B]` | a `call_contract` block whose opcode is any member |
| `any` | any block |

A `btype` head must name a leaf btype the engine builds itself (`call_contract`, `contract_deploy`, `empty`, `gasless_request`, `jetton_transfer`, `root`, `tick_tock`, `ton_transfer`) or a btype some matcher's `produces` names.
Anything else is `R043_UNKNOWN_BTYPE`, checked across the whole compiled spec set.

`@name` binds the matched block for the build phase.
A capture on an `op` node may carry a parse annotation, `as T1 | T2`, which is the pattern-side spelling of a `parse` statement.

`where` filters a candidate synchronously after its kind test.
`where IDENT` calls a declared host predicate; `where (expr)` takes a full expression:

```mch
    btype nft_transfer @nft where (.is_purchase) {
        -> { maybe btype ton_transfer @payout where (.destination == nft.data.real_prev_owner) }
```

Inside the clause a bare `.` is the candidate block, `.msg` its message envelope, and any other `.name` reads the candidate's `data`.
The only bare name the clause may reference is the matcher's anchor capture (`C015`).
The clause stays synchronous: no `lookup`, no host function, no comprehension, no strict `parse!` (`C013`).

### 5.3 Edges, children sets and relation blocks

`A -> B` makes `B` a child of `A`; `A <- B` makes `B` a parent of `A`.
`A -> { item; item }` declares an ordered children set on `A`, matched per item in declaration order against the left sibling's children, first remaining candidate wins.
A children block binds to the node immediately on its left and ends that chain, so an edge after it is `C009`.

A node states all three of its edges in a relation block instead:

```mch
    op NftTransfer @transfer as NftTransfer {
        -> {
            maybe op NftOwnershipAssigned @assigned as NftOwnershipAssignedPrevOwner
            ; maybe op Excess
        }
        <- maybe peek any @funding_parent
    }
```

The `{` follows the node on the same line with no arrow between.
One child chain and one parent per block; several children-set items concatenate in source order.
Block form and arrow form emit identical IR.

`maybe X` makes `X` optional and binds its captures to `null` when absent.
`peek X` binds without consuming, which leaves the block available to later patterns and later matchers; everything below a peek must itself be peek (`C014`).
`many X` as a children-set item takes every remaining candidate that matches and binds a list on the repeated block; bounds are `many[n..k]` and `many[0..]`.

### 5.4 Alternatives

`or { A ; B }` tries arms in declaration order and the first whose whole subtree matches wins.
`xor { A ; B }` first counts the arms whose head passes; with exactly one it matches that arm in full and tries no other, and with none or several the node fails.
Either block is an atom and needs at least two arms (`P021`).

```mch
    op [JettonMint, MinterJettonMint] @mint as MinterJettonMint | JettonMint
      -> maybe or {
                   op JettonNotify
                   op JettonInternalTransfer @internal as JettonInternalTransfer -> maybe op JettonNotify
               }
```

A capture name may repeat across arms of an alternative; only the winning arm binds it.

### 5.5 Rules and recursion

`rule NAME = pattern` names a reusable fragment referenced as `$NAME`, and a rule's captures are visible to every matcher using it.
A self-referential rule compiles under one of two strategies.

The default frontier strategy splits the rule's top-level alternative into step branches containing the self-reference and exit branches without it, then walks the frontier trying the exit first.
Captures inside the rule bind lists in unrolling order, and a `maybe`-guarded self-reference provides termination; a cycle without one is `R006_UNBOUNDED_RECURSION`.

`rule NAME cyclic = ...` lowers self-references to graph back-edges and matches by plain recursive descent bounded by trace depth.
Reach for it when continuation and final branches share a prefix that the exit-first frontier order would cut short.
A cyclic body holds no captures, its self-reference is the last atom of its chain, and it is never `maybe`-wrapped (`C012`).

## 6. Build statements

Build statements run after a successful structural match, in source order, and construct the produced block.
They are the only way a matcher produces output.

### 6.1 `parse`

`parse CAPTURE as T (| T)*` parses the captured block's message body with a registry parser and puts the result on `CAPTURE.body`.
Failure is soft: `CAPTURE.body` stays `null` and the match survives.
The preferred spelling is the annotation on the capture itself, `@transfer as NftTransfer`, which synthesizes the same statement ahead of the written ones.

### 6.2 `let` and `require`

`let NAME = expr` binds a local name.
`require NAME = expr` is the same statement followed by a rejection on `null`, and only `null` rejects:

```mch
    require wallet   = lookup jetton_wallet(internal.msg.destination)
```

### 6.3 `reject when` and `failed when`

`reject when expr` abandons the match exactly as a structural failure does: nothing is consumed, nothing is appended, and a later matcher in manifest order may claim the same anchor.
This is where async-data disambiguation belongs, because a lookup cannot run inside `where`.

```mch
    reject when nft == null or (nft.code_hash != "i1/8nr/TkGTY1fVuRlnIJrt1k5I/XKSHKL5NYK9vUfk=" and nft.collection_address != "0:B774D95EB20543F186C06B371AB88AD704F7E256130CAF96189368A7D0CB6CCF")
```

`failed when expr` sets the produced block's `failed` flag rather than abandoning the match.
It may appear several times and the results OR together.

A block-level failure flag and an aborted transaction are two different facts, and most events want both.
`lib/common.mch` carries the convention:

```mch
export fn tx_failed(x) = tx_aborted(x) ?? false

# The block failure flag or an aborted transaction; tx_failed alone reads only the abort.
export fn any_failed(x) = x.failed or tx_failed(x)
```

Import `lib/common.mch` and write `failed when any_failed(transfer)`.
Use bare `x.failed` only when the abort genuinely must not count.

### 6.4 `out` and `produces switch`

`out { field: expr }` fills the produced block's `data` map, and `name?: expr` omits the key entirely when the value is `null`.

```mch
    out {
        owner:         account(wallet.owner)
        jetton_wallet: burn.msg.destination
        amount:        amount(burn_amount)
        asset:         asset(wallet.jetton)
    }
```

`produces switch` picks the produced btype from content, branches tested in order, first true branch winning.
Every branch btype appears in the matcher's `produces` union (`R020`) and an `else` branch is required (`R021`):

```mch
    produces switch {
        when dns.body.has_value => change_dns out {
            source:               dns.msg.source
            destination:          dns.msg.destination
            key:                  dns.body.key
            value:                dns.body.value
            collection_address?:  account(nft.collection_address)
        }
        # A missing value with a key of 256 zero bits renews the DNS record.
        when b64(dns.body.key) == "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=" => renew_dns out {
```

### 6.5 The expression language

Field access, literals, comparisons, `and`/`or`/`not`, the `??` default, `x if cond else y`, integer `+ - *`, and a fixed builtin set.
Every build expression can read `anchor`, the anchor block, and `consumed`, the de-duplicated consumed-block list.

`CAPTURE.msg` is the message envelope with `msg_hash`, `tx_hash`, `tx_lt`, `direction`, `source`, `destination`, `value`, `opcode`, `created_lt`, `created_at`, `bounce`, `bounced` and `transaction`.
The envelope set is closed: a field the trace loader does not carry (such as `fwd_fee`) faults, so a spec reads only the fields listed above (`msg_envelope` in `ton-index-worker/ton-mch-engine/src/ExprRuntime.cpp`).
`CAPTURE.msg.transaction` carries `hash`, `lt`, `now`, `account`, `aborted`, `orig_status`, `end_status` and `compute_exit_code`.
`msg.source`, `msg.destination` and `msg.transaction.account` are already `AccountId` values, so a spec never wraps them in `account(...)`.

Constructors are `asset(master)`, `ton_asset()`, `account(addr)`, `amount(x)` and `addr_none()`.
`account(null)` is `null`, a missing address; `addr_none()` is the present-but-empty address, so a field that must carry one is written `account(x) ?? addr_none()`.
List combinators are `first`, `last`, `len`, `at`, `zip`, `concat` and `map(xs, "field")`, plus the comprehensions `map`, `any`, `all`, `find` and `stable_sort_by`.

`fn NAME(a, b) = expr` declares a pure function the resolver inlines at every call site.
Substitution is call-by-name, so bind an expensive or faulting argument with `let` first.
Any call name that is neither a builtin nor a same-file `fn` resolves to a host function in the C++ registry, and engine setup excludes a matcher whose host function is missing.
A growing host-function count signals a missing language feature or a matcher fighting its domain.

A runtime fault inside a build program rejects that matcher's match and leaves the trace and the other matchers untouched.

## 7. Parsing message bodies and the ABI bridge

A message type named in `parse` resolves through the engine's message-parser registry.
The registry merges generated ABI rows with hand-written parsers; a name claimed by both sources is poisoned rather than resolved arbitrarily, and startup validation fails on a poisoned entry.

Generated rows come from Tolk declarations under `ton-index-worker/ton-abi/abi/`.
The declaration is the wire schema and the field names the spec reads:

```tolk
@overflow1023_policy("suppress")
struct (0x5fcc3d14) NftTransfer {
    query_id: uint64;
    new_owner: any_address;
    response_destination: any_address;
    custom_payload: cell?;
    forward_amount: coins;
}
```

A generated row validates its authored opcode prefix, so guard a parse by the matching opcode or tolerate a null result.
`ton-index-worker/ton-mch-engine/src/AbiBridge.cpp` registers the rows and the few hand adapters that widen one.
The `NftTransfer` row is such an adapter: it decodes the declared prefix and then the trailing forward payload, which is why `transfer.body.forward_payload` reads in the spec although the Tolk struct stops at `forward_amount`.

Hand-written parsers under `ton-index-worker/ton-mch-engine/src/parse/` keep behavior that is tolerance rather than wire schema: the opcode is skipped instead of validated, `addr_none` becomes a null value, external addresses render as `extern:<bit-length>:<hex>`, optional cell payloads in the jetton and NFT paths serialize as CRC32C BOCs, and conditionally present attributes stay conditionally present.

A payload fragment, such as a jetton transfer's forward payload, is not in the registry.
Reach it with the `parse` expression form instead:

```mch
    let carrier_cell = (parse? carrier.data.forward_payload as DedustV2PayJetton).payment_payload if jetton_in else carrier.body.payment_payload
    let payload       = parse? carrier_cell as DedustV2SwapPayload
```

`parse!` faults the match on failure; `parse?` returns `null` and lets attribute access null-propagate.
The suffix is mandatory in expression position.

Never `parse` a `btype` capture of a block a prior matcher produced (`R023`).
Its body does not round-trip after the prior matcher's merge.
Read its fields through `CAPTURE.data.<field>`, the way the GetGems purchase matcher reads the NFT transfer it consumes:

```mch
        nft_address:              nft.data.nft.address
        collection_address:       nft.data.nft.collection.address
```

The three base blocks `call_contract`, `ton_transfer` and `tick_tock` are exempt, because their first event node is their own message.

## 8. Lookups and tiers

`lookup KIND(key_expr)` queries account interface data and returns a record or `null`.
Lookups run only in the build phase, never in `where`.
Two passes fill them: a collecting pass records the misses, the recorded keys are fetched, and the build repeats until no new keys appear, so one lookup's key may come from another lookup's result.

Resolution has two tiers.
Tier 1 reads interfaces already computed for the trace, in the indexer the completing block's own detections and in the emulator the trace's own emulation results.
Tier 2 reads account state from celldb shard states through `CelldbTier2Lookup`.
Both tiers render through the same mapper in `TieredLookupSource`, so the tier that served a value cannot change its shape.
For an interface lookup, a tier-1 miss is final only for a trace transaction account that has no active-to-inactive transition and is present in the tier-1 map.
An account outside that map still reaches tier 2.
A miss in both tiers is `null`; a tier-2 read that fails logs one WARNING and returns a plain miss, so a lookup can never drop a trace's actions.

| Kind | Tier 1 | Tier 2 | Record fields the specs read |
|---|---|---|---|
| `jetton_wallet` | yes | yes | `balance`, `address`, `owner`, `jetton` |
| `nft_item` | yes | yes | `address`, `init`, `index`, `collection_address`, `owner_address`, `content`, `code_hash` |
| `nft_sale` | yes | yes | `marketplace_address`, `nft_address`, `full_price`, `nft_owner_address`, `code_hash` |
| `nft_auction` | yes | yes | `mp_addr`, `nft_addr`, `last_bid`, `nft_owner`, `code_hash` |
| `dedust_pool` | yes | yes | `assets`, each `{is_ton, address}` |
| `nominator_pool` | yes | yes | `address` |
| `multisig_order` | yes | yes | `signers` |
| `telemint_item` | yes | yes | `address` |
| `jvault_assets` | no | yes | the chained stake wallet, pool and lock wallet record |
| `code_hash` | no | yes | the account's code hash as a padded base64 string |

`code_hash` is tier-2 only because tier 1 holds detected interfaces, not account code.
It returns `null` for an account with no state or no code, a destroyed account included, and the fixture path serves it from the fixture's `code_hashes` map.

A version-gated protocol compares `code_hash` against known constants and degrades to `null` on an unknown one:

```mch
# Unknown code hashes are not GetGems contracts.
export fn getgems_sale_version(h) = (
    1 if h == "2pufziLofEllctIDZSWVebzO+RpyA1fMvowFLvyb4I8=" else
```

A lookup key expression should reference captures, parse results and non-lookup `let` bindings.
Chaining a lookup off another lookup's result works, and each level costs one extra fetch round on the traces that hit it.

## 9. Writing action rows

A matcher's `out` block fills the produced block's `data` map.
`ton-index-worker/ton-mch-engine/src/ActionBuild.cpp` maps each supported btype to a fill function that builds an `Action` row.
An unsupported btype skips its row; its serializable children remain eligible as separate rows.

The action payload stores protocol-specific data as one flat map.
It has no per-protocol composite columns or group wrappers. Shared fields such as
`source`, `destination`, `asset`, `amount` and `opcode` remain ordinary action fields;
all other non-null action fields live directly under `data`.
In encoded payloads, null map fields are omitted recursively and lists retain null
elements. `legs` trims trailing null placeholders and omits the key when all legs
are empty.

`fill_nft_transfer` is a worked example. The spec writes names such as:

```mch
    out {
        is_purchase:          is_purchase
        prev_owner:           prev_owner
        new_owner:            new_owner
        query_id:             transfer.body.query_id
        forward_amount:       amount(transfer.body.forward_amount)
```

Payload key names and ids are generated in `ton-index-worker/ton-mch-engine/docs/KEYS.md`
from `ton-index-worker/ton-mch-engine/payload-keys.yaml`. Reuse an existing key
first; a new key needs written justification, then append the next id by hand
in the YAML and run `python -m indexer.events.mch.payload_keys gen` from
`indexer/`. This changes the payload contract and needs no DDL migration.

The emulator path writes the same codec-prefixed payload bytes as the batch database
writer under each Redis action row's binary `payload` field. It emits no `*_data`,
`listing`, `packet`, `comment_data` or `extra` row fields. Codec byte `01` is followed
by one MessagePack map, with numeric registry ids in ascending order at every
depth and no `data` wrapper. The empty payload is `01 80`. Addresses, BOCs,
and hashes use binary values; unsigned decimals use MessagePack uint through
uint64, then minimal big-endian binary. The get-method integer fields `nft_index`,
`price`, `full_price`, `marketplace_fee`, `max_bid`, `min_bid`, `mp_fee_base`,
`mp_fee_factor`, `royalty_amount` and `royalty_fee_base` use signed decimal strings;
`tick_spacing` is also an integer string. The action goldens still show names
and decimal text.

An invalid payload value skips its action. Other actions in the trace remain
classified, while the trace records the bounded payload error. If all actions
are skipped, storage writes the unknown rescue row with that error record.

## 10. Comment rules for specs

Keep comments that change what the next matcher author does: contract facts hidden by opcodes, traps, or intentional degradation on a lookup miss.
Cite a contract fact by contract, file and line in the contract's public repository only when the source is already named or the contract and symbol are certain.
Keep a verified fact without an unverifiable citation; delete an unverifiable fact.
A pattern with no contract source may say once: "no contract source; shape observed on mainnet".

Every `.mch` file needs a header sentence describing its protocol flow beyond its filename, and up to three sentences only for non-obvious facts.
Above a matcher, use at most one line, and only when its name does not identify what it catches.
Place a local comment above the line it explains, at most two lines, and only for a non-obvious rejection, a magic constant, an order-dependent edge, or a degrading lookup.

Write English statements in present tense, one sentence per line, without hedging, "we", "note that", "deliberately", or dashes as punctuation.
Never include reference-implementation, port, parity or migration narrative, task or review identifiers, internal links, measurements, benchmarks, rejected alternatives, language semantics, capture or recursion explanations, registration order, next-line narration, capture-to-role maps, section titles, underlines, or ASCII chain diagrams.
Registration-order facts belong in `specs/manifest.yaml` beside the affected entries.
Prefer a clear name to an explanatory comment.
Keep full-line comments at or below 15 percent of each file's nonblank lines.

The three shapes that earn their place:

```mch
# Matches TON DNS purchases, record changes, renewals, deletions, and balance releases.
```

```mch
    # NFT item lookup miss keeps the bid row with null item index and collection.
```

```mch
    # The sibling payout takes precedence when both payout forms match.
```

## 11. The fixture and golden workflow

Fixtures are `.lz4` trace captures under `indexer/tests/traces/`, named by trace id.
Build one from a hash with `venv/bin/python -m tests.utils.api_fixture <trace_hash> --out tests/traces/<trace-id>.lz4` from `indexer/`.

`ton-index-worker/ton-mch-engine/goldens/fixtures.json` names every fixture: `group` is the golden directory, `name` is the file header, `slug` is the file name, and `note` is an optional one-line scenario of at most 100 characters.
A synthetic fixture prefixes its note with `synthetic: `; chain and API captures carry no label.

`goldens/actions/<group>/<slug>.<id8>.yaml` holds every action row the engine serializes for that fixture, exactly as production writes them.
Rows are sorted by anchor, the ten directional and scalar columns are always present with nulls spelled out, the flat `data` map appears only when set, `opcode` is hex, `matcher` is absent on engine-made leaves, and a BOC-valued field renders as its cell hash.
For classified actions, the `tx_hashes` row column starts with the initiating transaction, or the root-level block's earliest transaction by `tx->lt`, followed by the remaining hashes sorted ascending.

Regenerate from the repository root:

```
ton-mch-engine --actions -T indexer/tests/traces \
    -O ton-index-worker/ton-mch-engine/goldens/actions \
    --fixtures ton-index-worker/ton-mch-engine/goldens/fixtures.json
```

A diff is a regression, never a re-baseline.
A deliberate output change regenerates the files in its own commit, and the diff is reviewed field by field with the reason for every changed row stated in the commit.

A synthetic fixture has no unmodified capture, so its `.lz4` is edited in place through a JSON round trip, from `indexer/`:

```
venv/bin/python -m tests.utils.fixture_json unpack tests/traces/<trace-id>.lz4 -o /tmp/fixture.json
# edit /tmp/fixture.json
venv/bin/python -m tests.utils.fixture_json pack /tmp/fixture.json -o tests/traces/<trace-id>.lz4
```

The JSON form is the fixture map one to one: msgpack bin values spell as `{"$b64": "<base64>"}` and a map with non-string keys as `{"$map": [[key, value], ...]}`.
Fixture-path code-hash gates read the fixture's `code_hashes` map, which is added by hand for synthetic fixtures.

## 12. The gates

A spec change runs the frontend suite and the engine suite.

From `indexer/`:

```
venv/bin/python -m pytest tests/mch -q
```

From the repository root, after a build:

```
ctest --test-dir build/ton-index-worker/ton-mch-engine -R '^mch_'
```

Eleven tests are wired into the engine's own build directory.

| Test | What it gates |
|---|---|
| `mch_abi_bridge` | the ABI trampoline, the value adapter and registry policy |
| `mch_actions_msgpack` | the action row to msgpack blob encoding |
| `mch_actions_json` | every golden fixture's rows as JSON against the `actions` DDL |
| `mch_celldb_tier2` | tier-shape equality between tier 1 and tier 2 |
| `mch_ghost_external` | wallet request and ghost decoding |
| `mch_classify_core` | block-tree topology guards and the failure fallback |
| `mch_expr_vectors` | the expression conformance vectors through generated code |
| `mch_smc_pruned_cells` | pruned-cell handling in the SMC envelope |
| `mch_walker_cases` | walker selection, capture binding and clean build rejection |
| `mch_actions_golden` | the committed action goldens |
| `mch_ir_equivalence` | IR emitter determinism between the two emitted copies |

## 13. Traps and FAQ

**A children block ends its chain.**
An edge after `-> { B; C }` is `C009`, and so is a parent edge after an ordinary child, `A -> B <- C`.
Put both relations on the node with a relation block: `A -> B { <- C }`.

**A bare children block has nothing to attach to.**
A children block as a sequence head, inside an alternative branch, or reached through a rule reference is `C008`.
Inline it at the use site.

**A `where` clause cannot look anything up.**
Lookups, host functions, comprehensions and `parse!` are all `C013`.
They belong in build statements, which run after the structural match.

**A `where` clause sees one capture only.**
The matcher's anchor capture resolves to the anchor block; any other name is `C015`.
The walker holds no binding for it at head-test time, so it would test false rather than fail.

**A `many` item starves later items.**
It takes every candidate it matches before the next item runs, and no diagnostic reports the starvation.
Put the single item first.

**A capture below a `many` repeated block is `C017_MANY`.**
`many` binds only the repeated block, as one list in candidate order.
Keep numbered captures when each position has its own role or pairs with a capture below it.

**A capture directly after a recursive rule reference is `C007`.**
The walker cannot reach that position after recursion.
A later children-set item or the parent chain binds normally.

**A produced block's body does not round-trip.**
`parse` on a `btype` capture of a block a prior matcher built is `R023`; read `CAPTURE.data.<field>` instead.

**A soft parse does not reject.**
A total parse failure leaves `CAPTURE.body` as `null`.
Only a later expression that reads `.body` without a guard faults.

**A rejected build consumes nothing.**
Not even the automatic excess and bounce absorption, which applies to a successful build only.
The anchor stays available to the next matcher in manifest order.

**Mutual recursion is not diagnosed.**
Two rules referencing each other loop the inliner until the compile aborts on a recursion limit.
The same holds for a cycle among `fn` declarations and for an import cycle.

**Order is semantics.**
No automatic check here sees two matchers competing for one anchor; the manifest comment and the action goldens protect that.

**One build directory per session.**
A parallel edit of a spec mid-build can be captured by one generator run and not another.
The cross-table source-hash check catches the mismatch at startup, and the fix is isolation.

**A stale binary is the classic false divergence.**
Build the target before trusting a golden diff.
