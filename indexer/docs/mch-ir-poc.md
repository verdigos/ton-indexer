# MCH IR — POC Schema (v0.1-poc)

**Status:** POC contract for branch `mch-ir-poc`. NOT the final M5 IR (that one adds expression bytecode, typed capture tables, conformance triples). This document is the binding contract between the Python emitter and the Go loader. If either side needs a deviation, update this file first.

## Purpose

Serialize the *lowered* matcher graph (what the Python mch compiler builds from `.mch` specs) into a language-neutral JSON artifact. A host engine (Go POC) loads the artifact and constructs its native engine primitives from it — no `.mch` parsing outside Python, ever.

## Container

- Single JSON file, UTF-8, canonical form: object keys in the order given below, arrays in emission order.
- No nulls for absent fields — omit the key entirely.
- All indices are 0-based integers into the top-level `nodes` array.

## Top level

```json
{
  "mch_ir_version": "0.1-poc",
  "frontend": { "generator": "mch-python", "source_files": ["jettons.mch", "..."] },
  "matchers": [ ... ],
  "nodes": [ ... ],
  "registration_order": [0, 1, 2]
}
```

- `registration_order` — matcher indices in engine registration order (= priority order; POC: source order across files, alphabetical by file name, then declaration order).

## Matcher record

```json
{
  "name": "jetton_transfer",
  "anchor": { "kind": "opcode_set", "values": [260734629] },
  "root": 0,
  "produces": ["jetton_transfer"],
  "captures": [ {"name": "transfer", "card": "one"} ],
  "include_excess": true,
  "include_bounces": true,
  "builder": "build_jetton_transfer"
}
```

- `anchor.kind` ∈ `"opcode_set"` (values = ints) | `"btype"` (values = btype strings). Anchor test: block is CallContract with opcode ∈ values, or block.btype ∈ values. Multi-value = T10 anchor groups.
- `root` — node id of the anchor node itself (its `child` / `parent` / `children` edges hang the rest of the pattern).
- `produces` — btype strings (union per T11; first = primary).
- `captures` — informational in POC (Go ignores). `card` ∈ `"one" | "opt" | "many"`.
- `builder` — host registry name. Go POC: ignored (build = stub creating primary-btype block + merge of consumed set). **Omitted** for declarative matchers (build statements, no registered builder — see "Expressions & build programs" below); never emitted as `null`.
- `include_excess` / `include_bounces` — engine auto-append flags (excess opcode `0xd53276db`, bounce `0xffffffff`), default true.
- `build_program` — array, matcher's build statements in source order. **Omitted** when the matcher has no build statements. See "Expressions & build programs" below.

## Node records

Flat tagged records. Common optional fields on every kind: `optional` (bool, default false), `capture` (string, informational), `where` (predicate name, string), `where_expr` (inline `where (expr)` clause — an expression node, see "Expressions & build programs" below; mutually exclusive with `where`), `child` (node id), `parent` (node id), `children` (array of node ids — ordered children-set items; item-level optionality lives on the referenced node's own `optional` flag).

Kinds:

```json
{ "kind": "contract", "opcode": 260734629, ... }
{ "kind": "block_type", "btype": "jetton_transfer", ... }
{ "kind": "pred", "pred": "pton_self_transfer", ... }
{ "kind": "any", ... }
{ "kind": "or",  "branches": [4, 5], "exclusive": false, ... }
{ "kind": "recursive", "step": 7, "exit": 9, ... }
```

- `contract.opcode` — signed or unsigned int accepted; hosts MUST compare on the unsigned 32-bit value (Python stores opcodes as signed int32 in DB — emitter emits the canonical **unsigned** value; loader normalizes its message opcodes the same way).
- `or.exclusive` — engine contract per spec §6.6: exclusivity affects `test_self` ONLY (sum of branch test_self == 1); `try_build` behaves exactly like `or` (first branch that fully builds wins).
- `recursive` — frontier walk per spec §10.3 / T18 semantics: at each frontier block, test `exit` first; if exit builds, recursion terminates successfully; else try `step`; step's built blocks' `next_blocks` become the new frontier. `optional` recursion = zero iterations allowed.
- Cycles are legal (e.g. a step node graph that references back). Loaders must build via two passes (allocate all nodes, then wire edges by id).

## Walk semantics (normative for POC)

Go engine must match the Python engine (`indexer/events/blocks/basic_matchers.py`) behavior already ported in `mch-go-poc/engine`:

1. `try_build(block)`: `test_self` → child matcher loop over `block.next_blocks` (all children tried, matches accumulate) → `children` set algorithm (ordered, first-match-wins, consume, optional skip) → parent matcher on `block.previous_block` → on success append excess/bounce siblings per flags.
2. Match pass = BFS over live blocks (skip absorbed: `parent != nil`), registration order across matchers (pass per matcher for POC).
3. Build stub: new Block with `produces[0]` btype, merge consumed set (rewire prev/next, mark absorbed).

## Predicates

`where` / `pred` reference host-registered predicate names. Go POC: registry `map[string]func(*Block) bool`; unknown name → register always-true stub and print a warning to stderr once per name. (Real impl will hard-fail; POC must stay runnable for specs whose predicates aren't ported.)

## Expressions & build programs

M4 build statements (spec §9) and expressions (spec §9.6), inlined as trees (not a pooled/deduplicated bytecode form — see "Out of scope" below).

### Expressions

Tagged by `"k"`:

```json
{ "k": "int",  "v": 42 }
{ "k": "str",  "v": "abc" }
{ "k": "bool", "v": true }
{ "k": "null" }
{ "k": "name",     "id": "transfer" }
{ "k": "dotfield", "name": "value" }
{ "k": "attr",   "of": <expr>, "name": "msg" }
{ "k": "call",   "fn": "account", "args": [<expr>, ...] }
{ "k": "lookup", "name": "jetton_wallet", "args": [<expr>, ...] }
{ "k": "unary",  "op": "not" | "-", "x": <expr> }
{ "k": "bin",    "op": "and" | "or" | "==" | "!=" | "<" | "<=" | ">" | ">=" | "??", "l": <expr>, "r": <expr> }
{ "k": "ternary", "cond": <expr>, "then": <expr>, "else": <expr> }
```

- `name` — a bare identifier value: a pattern capture or a `let` binding.
- `dotfield` — leading-dot field access (`.field`); valid only inside a node's inline `where (expr)` clause (the current block's data field).
- `attr` — `<expr>.field` (message/body/capture-object accessor chains, e.g. `a.msg.source`).
- `call.fn` — always a bare builtin name (`account`, `amount`, `asset`, `ton_asset`, `b64`). The grammar technically allows postfix-chaining a call off any expression (e.g. `x.y(...)`), but the evaluator (`eval.py Evaluator._eval_call`) only ever invokes a bare builtin name — a non-name callee never evaluates and has no IR encoding (the Python emitter raises rather than silently drop it).
- `lookup.name` — interface-repository lookup kind (`jetton_wallet`, `nft_item`, ...).

### Build statements

New matcher-record key `build_program` (array, source order; omitted when the matcher declares none). Tagged by `"s"`:

```json
{ "s": "parse",  "target": "transfer", "types": ["JettonTransfer", "..."] }
{ "s": "let",    "name": "w", "expr": <expr> }
{ "s": "reject", "when": <expr> }
{ "s": "failed", "when": <expr> }
{ "s": "broken", "when": <expr> }
{ "s": "flow",   "kind": "jetton" | "ton" | "fees", "args": [<expr>, ...] }
{ "s": "out",    "fields": [ {"name": "sender", "expr": <expr>}, ... ] }
```

`out.fields` is in source order.

### Node-level `where_expr`

`Node.where_expr` (inline `where (expr)`, mutually exclusive with the named `where` predicate form) → node record key `"where_expr": <expr>`. **Not emitted for a matcher's own anchor node** in this POC — the anchor is lowered through a separate code path that doesn't currently carry `where_expr` through; no current spec (real or test) exercises this combination.

Go POC: ignores `build_program` and `where_expr` entirely (unknown-field-tolerant parser); these fields are frontend-only in 0.1-poc.

## Emitter interpretation notes (binding for loaders)

1. **Opcode-set anchors** (e.g. `jetton_mint`): the root node is emitted as kind `"any"`; the membership test lives ONLY in the matcher record's `anchor`. Loaders MUST apply the anchor test as the root's effective `test_self` (or as a candidate prefilter) — both readings are equivalent because the root node itself is permissive.
2. **Duplicate matcher names** can occur across spec files (currently `jetton_transfer` in both `jetton_transfer.mch` and `jettons.mch`, identical patterns). `-matchers` name selection must tolerate duplicates (run each occurrence in registration order; re-runs are naturally idempotent since absorbed blocks don't re-match).
3. **`recursive` nodes never contain back-reference cycles in practice** (Python lowering expands each rule-ref site fresh and strips the trailing `maybe $self`); a matcher may therefore contain several independent `recursive` nodes. `exit` MAY be absent (step-only recursion) — loaders must treat missing `exit` as "no exit matcher" (frontier walk terminates when the frontier empties). All emitted `recursive` nodes currently carry `optional: true`.

## Out of scope for 0.1-poc

Expression *bytecode* — a shared, deduplicated `exprs` pool addressed by index (final M5 IR); this POC inlines expression trees per-occurrence instead, with no dedup. Also out of scope: typed capture extraction, value flow, priority directive, diagnostics, anchor-node `where_expr` (see above). Schema reserves nothing — final M5 IR is a superset redesign; this artifact is disposable.

## Acceptance

1. Emitter: `python -m indexer.events.mch.ir_emit <specs_dir> -o mch-ir.json` compiles all 7 spec files and emits one artifact; running it twice yields byte-identical output.
2. Go: `mchpoc -ir mch-ir.json -matchers jetton_transfer,dedust_swap -trace <fixture>` produces the SAME "Fired matchers" report as the hardcoded-graph mode on the three validated fixtures (`KYQfjYD1…`, `C7bHOfSr…`, `c8jOrHqy…`).
