# MCH IR — Normative Specification (v1.0)

**Status: NORMATIVE.** This document supersedes `docs/mch-ir-poc.md` (kept for history). It is the binding contract between the Python emitter (`indexer/events/mch/ir_emit.py` + `expr_ir.py`), the loader/engine (`indexer/events/mch_ir/`), and any future host (the C++ engine). It documents the artifact **as actually emitted and consumed today** — where wording below says MUST/omitted/rejected, that reflects `ir_emit.py`/`loader.py`/`engine.py`/`program.py`/`expr_eval.py` behavior at the time of writing.

> **Version-tag note (inconsistency, not normalized here):** the on-the-wire `mch_ir_version` string is still `"0.1-poc"` (`loader.IR_VERSION` / `expr_ir.IR_VERSION`); the loader rejects any other value. This document is *spec* version 1.0; the *artifact* version tag is unchanged pending a deliberate M5 bump. A host must emit/accept exactly `"0.1-poc"`.

## 1. Container

- Single JSON object, UTF-8.
- **No nulls for absent fields** — an optional field is either present with a real value or omitted entirely. (Consumers therefore read "absent" as the documented default.)
- All node references are 0-based integer indices into the top-level `nodes` array.
- Emission is deterministic: emitting the same specs twice yields byte-identical JSON (relied on by the byte-identity CI check).

## 2. Top level

```json
{
  "mch_ir_version": "0.1-poc",
  "frontend": { "generator": "mch-python", "source_files": ["vesting.mch"] },
  "matchers": [ <matcher record>, ... ],
  "nodes":    [ <node record>, ... ],
  "registration_order": [0, 1, 2]
}
```

- `mch_ir_version` — MUST equal `"0.1-poc"` (loader hard-rejects otherwise).
- `frontend` — informational (`generator`, `source_files`). Loader keeps it, does not validate its contents.
- `matchers` — the matcher table (§3), array (required).
- `nodes` — the shared, flat node pool (§5), array (required). Node records reference each other by index; cyclic references are legal (see `recursive`/cyclic, §5.5).
- `registration_order` — array of matcher indices, **source order** (emit concatenates files in name order, matchers in source position). Default (absent) = `range(len(matchers))`.
  - **Determinism note (see §8):** this array is NOT the final run order when priorities differ. `build_matchers` returns matchers **stable-sorted by ascending `priority`** over `registration_order`, yielding the total order `(priority asc, file name, source position)`. So `registration_order` encodes only the file+position tie-break; priority is applied at load. (This differs from the POC doc's claim that `registration_order` *is* priority order — flagged, not changed.)

**Load-time global checks (loader.load_artifact):** root is an object; `mch_ir_version` matches; `matchers`/`nodes` are arrays; every node validates (§5); every matcher validates (§3); **no two matchers share a `root` node id**; every `registration_order` entry is an in-range matcher index.

## 3. Matcher record

```json
{
  "name": "vesting_send_message",
  "anchor": { "kind": "opcode_set", "values": [2809022157] },
  "root": 0,
  "produces": ["vesting_send_message"],
  "captures": [ {"name": "send", "card": "one"}, {"name": "response", "card": "one"} ],
  "include_excess": true,
  "include_bounces": true,
  "build_program": [ <statement>, ... ]
}
```

| field | type | required | meaning |
|---|---|---|---|
| `name` | string | yes | matcher name (may duplicate across files; run each in order — idempotent) |
| `anchor` | object | yes | anchor test (§4) |
| `root` | int | yes | node id of the anchor node (its `child`/`parent`/`children` hang the pattern); unique across matchers |
| `produces` | string[] | yes, non-empty | produced btype(s); `[0]` is primary. A union (`change_dns | renew_dns | delete_dns`) lists all; a `produces switch` (§6.8) selects one at build time. Class names are normalized to btype strings at emit (`btypes.py`). |
| `captures` | object[] | no | informational; `{name, card}`, `card ∈ {"one","opt","many"}`. `"many"` = list capture under recursion |
| `include_excess` | bool | no (default true) | append excess-opcode (`0xd53276db`) sibling of the matched root |
| `include_bounces` | bool | no (default true) | append bounce-opcode (`0xffffffff`) sibling |
| `priority` | int | **no — emitted only when ≠ 100** | registration weight, lower runs earlier; absent ⇒ 100 (§8) |
| `builder` | string | no | host builder-registry name; **omitted for declarative matchers**; never `null` |
| `build_program` | object[] | no | declarative build statements, source order (§6); **omitted when none** |
| `shape` | string | no | host shaper-registry name (`shape IDENT`, §8.7 of the DSL); omitted when absent |

A matcher has **either** `builder` **or** `build_program` (or neither → the engine's stub build). It never has both.

**Load-time checks (`_check_matcher`):** `name` is a string; `anchor` is an object with a known `kind` (§4); `root` in range; `produces` a non-empty string array; each capture `{name:str, card ∈ {one,opt,many}}`; `include_excess`/`include_bounces` bool if present; `builder`/`shape` string if present; `priority` an int (never a bool) if present; `build_program` an array (each statement validated, §6). Registry-name existence (builder, shaper, predicate, message type) is checked later in `build_matchers`, not here.

## 4. Anchor records

`anchor.kind ∈ {"opcode_set", "btype", "mixed", "pred"}`. The root node itself is emitted permissively (kind `"any"`); the discriminating test lives in the anchor record and is applied as the root's effective head test / candidate prefilter.

```json
{ "kind": "opcode_set", "values": [260734629, 21] }          // CallContract, opcode ∈ values (unsigned 32-bit)
{ "kind": "btype",      "values": ["jetton_transfer"] }       // block.btype ∈ values
{ "kind": "pred",       "pred": "pton_self_transfer" }        // full-scan; test_self = named predicate, no prefilter
{ "kind": "mixed", "branches": [ {"op": 260734629}, {"btype": "jetton_transfer", "where": "p"} ] }
```

- **opcode_set** — `values` non-empty int array; each compared as the **unsigned 32-bit** opcode (emitter emits unsigned; a host normalizes its DB opcodes `& 0xFFFFFFFF`). Multi-value = a T10 anchor group (union).
- **btype** — `values` non-empty string array; `block.btype ∈ values`.
- **pred** — `pred` is a host predicate name; anchor test = that predicate over every candidate (no opcode/btype prefilter). Missing predicate ⇒ load error in `build_matchers`.
- **mixed** — `branches`, a non-empty array; each branch has **exactly one** of `op` (int) / `btype` (str), plus an optional `where` (predicate name). Test = union membership over the branch heads, then the matching branch's `where` if present; a head match whose `where` fails falls through to a later same-head branch. The frontend emits `mixed` only when a per-branch `where` or a genuine op/btype mix is present — a plain single-kind group stays `opcode_set`/`btype`.

## 5. Node records

Flat tagged records in the shared `nodes` pool. `kind ∈ {"contract","block_type","pred","any","or","recursive"}`.

Common optional fields (any kind): `optional` (bool, default false — item/edge optionality), `capture` (string, informational), `where` (predicate name) **xor** `where_expr` (an inline expression node, §7 — its own head-test predicate), `child` (node id — the `->` edge), `parent` (node id — the `<-` edge), `children` (int[] — ordered children-set item node ids; each item's own `optional` governs it).

```json
{ "kind": "contract",   "opcode": 260734629, "capture": "transfer", "child": 1 }
{ "kind": "block_type", "btype": "jetton_transfer", "capture": "x" }
{ "kind": "pred",       "pred": "pton_self_transfer" }
{ "kind": "any" }
{ "kind": "or",         "branches": [4, 5], "exclusive": false }
{ "kind": "recursive",  "step": 7, "exit": 9 }
{ "kind": "recursive",  "step": 7, "strategy": "cyclic" }
```

### 5.1 contract
`opcode` — int (signed or unsigned accepted; compare on unsigned 32-bit). Matches a `call_contract` block with that opcode.

### 5.2 block_type
`btype` — string; matches `block.btype == btype`.

### 5.3 pred
`pred` — host predicate name; `predicate(block) -> bool`. Missing ⇒ load error.

### 5.4 any
No head test of its own (used as the permissive root under an opcode-set/btype/pred/mixed anchor, and wherever a node's discriminator lives elsewhere).

### 5.5 or
`branches` — non-empty int array of node ids. `exclusive` (bool, default false). Semantics: **first branch whose full subtree matches wins** (rollback between attempts). `exclusive` changes only the HEAD test (exactly one branch head must match); as an edge target it is `try_build`-inert — legacy parity, `specs/stonfi_v2_swap.mch` depends on ordered first-match resolution of overlapping `^` branches.

### 5.6 recursive
`step` (required node id) + optional `exit` (node id). Frontier walk (spec §10.3 / T18): per frontier block, try `exit` FIRST (if it builds, recursion terminates successfully); else try `step`; the stepped blocks' `next_blocks` (in **canonical order**, §8) become the new frontier; one step per round. Frontier exhaustion succeeds iff ≥1 step landed (legacy rule; `exit` presence does not force failure). `exit` MAY be absent (step-only). Emitted frontier `recursive` nodes currently carry `optional: true`.

`strategy` (optional): omitted = the frontier walk above. `"cyclic"` = cyclic descent (`[M2R, R11/T28]`): `step` is the rule-body root whose subgraph carries **back-edges** to itself (an in-body self-reference is emitted as a bare node-id back-reference — the `cyclic_ref` frontend node has no record of its own; the shared body root's id is reused, and the `memo` guarantees the cycle terminates). Matching the node is ONE full `_match_node(step)` descent — the graph cycle IS the recursion, bounded by trace depth. Several outer `recursive`+`cyclic` nodes may share one body subgraph (one per reference site).

**Load-time checks (`_check_node`):** kind known; `contract.opcode` int; `block_type.btype` str; `pred.pred` str; `or.branches` non-empty in-range int array; `recursive` has `step` (in range), `exit` in range if present; `strategy` if present MUST be `"cyclic"` and MUST NOT carry `exit`; `child`/`parent`/`children` ids in range; `optional` bool; `capture` str; `where` str; `where_expr` validated as an expression (§7) AND checked sync-evaluable (no `lookup`, builtin-only calls — §7.4).

## 6. Build program

`build_program` is an array of statements (`"s"` tag), executed in source order by `BuildProgram.run`. Statement vocabulary: `parse`, `let`, `reject`, `failed`, `broken`, `flow`, `out`, `switch`.

```json
{ "s": "parse",  "target": "transfer", "types": ["JettonTransfer", "MinterJettonMint"] }
{ "s": "let",    "name": "w", "expr": <expr> }
{ "s": "reject", "when": <expr> }
{ "s": "failed", "when": <expr> }
{ "s": "broken", "when": <expr> }
{ "s": "flow",   "kind": "jetton" | "ton" | "fees", "args": [<expr>, ...] }
{ "s": "out",    "fields": [ {"name": "sender", "expr": <expr>, "optional": true}, ... ] }
{ "s": "switch", "branches": [ {"when": <expr>, "btype": "change_dns", "fields": [...]}, {"btype": "delete_dns", "fields": [...]} ] }
```

- **6.1 parse** — `target` (capture name), `types` (non-empty string array of registered message-type names). Soft, first-success-wins over alternatives; parsing an absent optional capture yields `null` body; a list capture parses element-wise. A parse fault (all alternatives fail) leaves `body` null (soft), NOT a rejection — expressions handle it with `??`/`reject`. (Message-type registration is checked at load in `build_matchers`.)
- **6.2 let** — `name`, `expr`. Binds a local; `lookup` may appear only in a `let` (async-batched conceptually; here evaluated inline).
- **6.3 reject** — `when` (expr). If truthy, `run` returns `None` (clean match rejection, §10.7 of the DSL).
- **6.4 failed / broken** — `when` (expr). OR-accumulate into the produced block's `failed`/`broken` flags.
- **6.5 flow** — `kind ∈ {jetton(3 args), ton(2), fees(2)}`, `args` expr array of exactly the kind's arity. Records a value-flow effect; **an effect with any null argument is skipped** (a leg tied to an absent optional capture degrades to a no-op).
- **6.6 out** — `fields`, array of `{name, expr, optional?}`. Populates the produced `data` map. An `optional: true` field whose value is **null** is **omitted** from the map (present-vs-absent key); a plain field always inserts. `optional` is emitted only when true.
- **6.7 switch (`produces switch`)** — `branches`, non-empty array of `{btype, fields, when?}`. The first branch whose `when` is truthy (or the `else` arm, which has no `when`) selects the produced **btype** and its `out` fields (same optional-key rule). No branch matching (no true `when`, no `else`) is a fault → rejection. Exactly one of `out`/`switch` per matcher (resolver R017). The engine uses `outcome.btype or produces[0]` when constructing the produced block.

**BuildOutcome** = `{data, failed, broken, flows, btype?}`. The produced block is `Block(btype or produces[0], [], data)` with the flags set, consumed set merged, flows applied, and any `shape` shaper invoked last.

**Load-time checks (`_check_stmt`):** tag in vocabulary; `parse` has string `target` + non-empty string `types`; `let` has string `name` + valid `expr`; `reject`/`failed`/`broken` have a valid `when` expr; `flow` kind ∈ {jetton,ton,fees} with matching arity, args valid; `out` fields each `{name:str, expr, optional?:bool}`; `switch` non-empty `branches`, each `{btype:str, fields, when?}`.

## 7. Expression encoding

Expressions are inlined trees tagged by `"k"`. Vocabulary: `int str bool null name dotfield attr call lookup unary bin ternary list record`.

```json
{ "k": "int",  "v": 42 }
{ "k": "str",  "v": "abc" }
{ "k": "bool", "v": true }
{ "k": "null" }
{ "k": "name",     "id": "transfer" }
{ "k": "dotfield", "name": "value" }
{ "k": "attr",   "of": <expr>, "name": "source" }
{ "k": "call",   "fn": "account", "args": [<expr>, ...] }
{ "k": "lookup", "name": "jetton_wallet", "args": [<expr>, ...] }
{ "k": "unary",  "op": "not" | "-", "x": <expr> }
{ "k": "bin",    "op": "and"|"or"|"=="|"!="|"<"|"<="|">"|">="|"??"|"+"|"-"|"*", "l": <expr>, "r": <expr> }
{ "k": "ternary", "cond": <expr>, "then": <expr>, "else": <expr> }
{ "k": "list",   "items": [<expr>, ...] }
{ "k": "record", "fields": [ {"name": "legs", "expr": <expr>}, ... ] }
```

### 7.1 Leaves & references
- `int`/`str`/`bool` — literal; loader requires exact type on `v`.
- `null` — the null value.
- `name` — a bound name: a pattern capture or a `let`. Unbound ⇒ `EvalError` (rejection).
- `dotfield` — leading-dot `.field`: valid ONLY inside a node `where_expr`, where it reads the candidate block's `data` field. In a build program it is an error (build statements have no "current block").

### 7.2 Accessors (`attr`)
`x.field`. Resolution (`expr_eval._access`):
- On a **Block**: `msg` → the message envelope (`get_message()`); `body` → the parsed message (requires a prior `parse` on that capture; else fault); `data` → the block's produced data map; `failed`/`broken`/`btype` → those attributes. Any other field on a Block ⇒ fault.
- On the **message envelope**: `source`, `destination`, `value`, `opcode` are the envelope fields (host attributes); `exit_code` is a **defined** field = the message transaction's `compute_exit_code` (null if none). `exit_code` requires the object to carry a `transaction` (message envelope); on anything else it faults. All other fields resolve by host attribute access.
- On a **dict** (a block `data` map / record): pure key lookup; a missing key is a fault.
- On a **list** capture: only `body` is defined (the parallel parsed-body list); anything else faults.
- On any other value: host attribute access; missing attribute faults.
- On `null`: yields `null` (null propagation).

### 7.3 Builtins (`call.fn`)
`call.fn` is always a bare builtin name (the evaluator never invokes a non-builtin callee via `call`; host `fn`s are also `call`-dispatched but resolve through the registries when the name is not a builtin). **A `call` short-circuits to `null` if any argument is `null`** (builtins are null-strict). Builtin table with exact semantics:

| builtin | arity | result |
|---|---|---|
| `account(x)` | 1 | `AccountId(x)` from a raw/user address string, `Address`, or `AccountId`. `account(null)` → null (short-circuit). |
| `addr_none()` | 0 | the addr_none `AccountId` (present but empty: repr `addr_none`, `as_str()` → null). Use `account(x) ?? addr_none()` for a present-but-empty address. |
| `amount(x)` | 1 | `Amount(x.value if Amount else int(x))`. |
| `asset(m)` | 1 | jetton `Asset(is_ton=false, jetton_address=m)`. |
| `ton_asset()` | 0 | the TON `Asset(is_ton=true)`. |
| `b64(x)` | 1 | base64 string of `x`; a raw `Cell` is BOC-encoded first (`cell.to_boc()`), bytes pass through. |
| `first(xs)` | 1 | `xs[0]` or null if empty. |
| `last(xs)` | 1 | `xs[-1]` or null if empty. |
| `len(xs)` | 1 | element count (int). |
| `sum(xs)` | 1 | integer sum; each element coerced to int (`Amount`→value, `int`→itself, `bool`/other → fault). Empty → 0. |
| `zip(xs, ys)` | 2 | list of 2-element lists, truncated to the shorter input. |
| `map(xs, "f")` | 2 | project field `"f"` (string) from each element (dict key then attribute; missing field → fault). |

List combinators require a list argument (else fault). `map`'s field arg is a string literal.

### 7.4 Operators
- `unary`: `not` (bool operand required, result bool); `-` (numeric negate; `-null` → null).
- `bin` — lazy: `and`/`or` (each side required bool, short-circuit), `??` (left unless null, else right). Strict: `==`/`!=` (structural null handling — two nulls are equal, null vs non-null unequal; otherwise host `==`; **note** `Asset == string` is case-insensitive on the master address); `<`/`<=`/`>`/`>=` (null on either side → false; else host ordering; non-orderable types → fault); **integer arithmetic** `+`/`-`/`*` (null on either side → null; operands coerce to int — `Amount`→its value, `int`→itself, `bool`/other → fault; result is a plain int; no division). Precedence: comparison < additive (`+ -`) < multiplicative (`*`), all left-associative.
- `ternary`: `cond` (required bool) selects `then`/`else`.
- `list` → a Python list of the evaluated items; `record` → a dict `{name: value}`.

### 7.5 where_expr (node head predicate)
A node's `where_expr` is evaluated by the **sync** evaluator against the candidate block. `dotfield` reads the block's `data`; a missing field / `data is None` / any fault / a non-bool result all fail the test (False) — **never** raised out of matching. `lookup` and non-builtin `call` are rejected at load (`_check_where_expr_sync`) and by the frontend compiler — a `where_expr` is builtin-only and synchronous.

**Load-time checks (`_check_expr`):** `k` in vocabulary; literal `v` exact type; `name.id`/`dotfield.name`/`attr.name` strings; `attr.of` recursively valid; `call`/`lookup` have string name + array args (recursively valid); `unary.op ∈ {not,-}`; `bin.op ∈ {and,or,==,!=,<,<=,>,>=,??,+,-,*}`; `ternary` has cond/then/else; `list.items` array; `record.fields` each `{name:str, expr}`.

## 8. Determinism guarantees (normative for any engine)

Two guarantees a conforming engine MUST reproduce:

- **T21 canonical candidate order** — wherever the walk chooses among sibling candidate blocks (child edge, children set, recursion frontier, excess/bounce absorption), candidates are visited in **ascending `min_lt`, ties broken by existing `next_blocks` order** (a stable sort by `min_lt`). This makes produced/consumed output independent of the tree's underlying `next_blocks` assembly order. Or-branches iterate spec-declared order (already canonical).
- **T20 priority order** — matchers run in ascending `priority` (absent ⇒ 100), ties broken by `(file name, source position)`. Concretely: `build_matchers` returns a **stable sort of `registration_order` by `priority`**. `priority` is present on a matcher record only when ≠ 100.

## 9. Registry references (host bindings a runtime must provide)

The artifact names host bindings by string; a runtime MUST supply them or fail to load/build:

| reference | where | contract |
|---|---|---|
| predicate | `pred` node, `where` node, `pred`/`mixed` anchor | `(Block) -> bool`, sync, pure |
| builder | matcher `builder` | `async (match) -> produced block \| None` |
| shaper | matcher `shape` | sync `(produced_block, match) -> None`, mutates produced region only |
| lookup | expr `lookup.name` | `async (key…) -> record \| None`; single-shape (name + key → record or null) |
| message type | `parse.types[]` | constructible from a `pytoniq_core.Slice` (the block body) |
| host fn | expr `call.fn` not in the builtin table | `async (args…) -> value`; null args pass through (unlike builtins) |

Predicate/mixed/pred-anchor and message-type names are validated at load in `build_matchers`; builder/shaper likewise. `where_expr` may reference builtins only (§7.5).

## 10. Load-time validation summary (what a loader MUST reject)

Structural (registry-independent, `load_artifact`): wrong/absent `mch_ir_version`; non-object root; missing `matchers`/`nodes` arrays; any node/matcher/expr/statement shape violation above; out-of-range node/matcher/child/parent/children/branch/root indices; two matchers sharing a `root`; a `where_expr` containing a `lookup` or a non-builtin `call`; a `cyclic` recursion carrying `exit`. Registry-dependent (`build_matchers`): an unregistered predicate (node/anchor), builder, shaper, `parse` message type, or **host fn** (a build-program `call` whose name is not a builtin and is absent from `Registries.fns`). A raising expression/builder/parse at RUN time is a clean match rejection (`[]`), never a crash.

## 11. Annotated example — `specs/vesting.mch`

The declarative vesting matcher (`op VestingSendMessage @send -> op VestingSendMessageResponse @response`, then a build that reads the parsed body). Emitted artifact (nodes elided to the two that matter):

```json
{
  "mch_ir_version": "0.1-poc",
  "frontend": { "generator": "mch-python", "source_files": ["vesting.mch"] },
  "matchers": [
    {
      "name": "vesting_send_message",
      "anchor": { "kind": "opcode_set", "values": [2809022157] },   // 0xA7733ACD unsigned
      "root": 0,
      "produces": ["vesting_send_message"],
      "captures": [ {"name": "send", "card": "one"}, {"name": "response", "card": "one"} ],
      "include_excess": true,
      "include_bounces": true,
      "build_program": [
        { "s": "parse", "target": "send", "types": ["VestingSendMessage"] },
        { "s": "out", "fields": [
          { "name": "sender",              "expr": {"k":"call","fn":"account","args":[
                                              {"k":"attr","of":{"k":"attr","of":{"k":"name","id":"send"},"name":"msg"},"name":"source"}]} },
          { "name": "vesting",             "expr": {"k":"call","fn":"account","args":[
                                              {"k":"attr","of":{"k":"attr","of":{"k":"name","id":"send"},"name":"msg"},"name":"destination"}]} },
          { "name": "query_id",            "expr": {"k":"attr","of":{"k":"attr","of":{"k":"name","id":"send"},"name":"body"},"name":"query_id"} },
          { "name": "message_boc_str",     "expr": {"k":"call","fn":"b64","args":[
                                              {"k":"attr","of":{"k":"attr","of":{"k":"name","id":"send"},"name":"body"},"name":"message_cell"}]} },
          { "name": "message_destination", "expr": {"k":"call","fn":"account","args":[
                                              {"k":"attr","of":{"k":"attr","of":{"k":"name","id":"send"},"name":"body"},"name":"message_destination"}]} },
          { "name": "message_value",       "expr": {"k":"call","fn":"amount","args":[
                                              {"k":"attr","of":{"k":"attr","of":{"k":"name","id":"send"},"name":"body"},"name":"message_value"}]} }
        ] }
      ]
    }
  ],
  "nodes": [
    { "kind": "any", "capture": "send", "child": 1 },                 // root: opcode-set test lives in `anchor`
    { "kind": "contract", "opcode": 4151002829, "capture": "response" } // 0xF7733ACD unsigned
  ],
  "registration_order": [0]
}
```

Reading it: anchor prefilters `call_contract` blocks with opcode `0xA7733ACD`; node 0 (`any`, the permissive root) captures `send` and requires a child node 1 (`0xF7733ACD`) captured `response`; the build parses `send`, then emits six data fields — note `b64(send.body.message_cell)` BOC-encodes the parsed `Cell`, and each `account(...)` wraps an envelope/body address. `priority` is absent (⇒ 100); `builder`/`shape` absent (declarative). A `produces switch` example is `specs/dns.mch`.

## 12. Value-encoding convention (shared with the conformance vectors, §T42)

The conformance vectors (`tests/mch_ir/vectors/expr_vectors.json`) encode typed inputs/outputs in a language-neutral tagged form so any host can replay them:

| JSON | decodes to |
|---|---|
| `null`, int, string, bool | the primitive |
| `{"$amount": N}` | `Amount(N)` |
| `{"$account": "0:HEX"}` / `{"$account": null}` | `AccountId(addr)` / addr_none |
| `{"$asset_ton": true}` | TON `Asset` |
| `{"$asset_jetton": "0:HEX"}` | jetton `Asset` |
| `{"$bytes_b64": "…"}` | raw `bytes` (base64-decoded) — for `b64(bytes)` |
| `{"$cell_b64": "…"}` | a `Cell` (BOC base64-decoded) — for `b64(Cell)` |
| `{"$obj": {field: <val>}}` | a struct with those fields (message envelope / body / lookup record) — nested values decode recursively |
| `{"$list": [<val>, …]}` | a list of decoded values |
| `{"$dict": {key: <val>}}` | a plain map (a block `data` map) |
| any other JSON object | a plain map with decoded values (record-literal expectation) |

Expectation forms: a decoded value (compared by type + structural equality; `Amount`/`AccountId`/`Asset` by their `==`), `{"$fault": true}` (the expression must reject — `EvalError`), or `null`.
