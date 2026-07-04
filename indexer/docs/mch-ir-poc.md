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
- `builder` — host registry name. Go POC: ignored (build = stub creating primary-btype block + merge of consumed set).
- `include_excess` / `include_bounces` — engine auto-append flags (excess opcode `0xd53276db`, bounce `0xffffffff`), default true.

## Node records

Flat tagged records. Common optional fields on every kind: `optional` (bool, default false), `capture` (string, informational), `where` (predicate name, string), `child` (node id), `parent` (node id), `children` (array of node ids — ordered children-set items; item-level optionality lives on the referenced node's own `optional` flag).

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

## Emitter interpretation notes (binding for loaders)

1. **Opcode-set anchors** (e.g. `jetton_mint`): the root node is emitted as kind `"any"`; the membership test lives ONLY in the matcher record's `anchor`. Loaders MUST apply the anchor test as the root's effective `test_self` (or as a candidate prefilter) — both readings are equivalent because the root node itself is permissive.
2. **Duplicate matcher names** can occur across spec files (currently `jetton_transfer` in both `jetton_transfer.mch` and `jettons.mch`, identical patterns). `-matchers` name selection must tolerate duplicates (run each occurrence in registration order; re-runs are naturally idempotent since absorbed blocks don't re-match).
3. **`recursive` nodes never contain back-reference cycles in practice** (Python lowering expands each rule-ref site fresh and strips the trailing `maybe $self`); a matcher may therefore contain several independent `recursive` nodes. `exit` MAY be absent (step-only recursion) — loaders must treat missing `exit` as "no exit matcher" (frontier walk terminates when the frontier empties). All emitted `recursive` nodes currently carry `optional: true`.

## Out of scope for 0.1-poc

Expression bytecode (`exprs` pool), build statements, typed capture extraction, value flow, priority directive, diagnostics. Schema reserves nothing — final M5 IR is a superset redesign; this artifact is disposable.

## Acceptance

1. Emitter: `python -m indexer.events.mch.ir_emit <specs_dir> -o mch-ir.json` compiles all 7 spec files and emits one artifact; running it twice yields byte-identical output.
2. Go: `mchpoc -ir mch-ir.json -matchers jetton_transfer,dedust_swap -trace <fixture>` produces the SAME "Fired matchers" report as the hardcoded-graph mode on the three validated fixtures (`KYQfjYD1…`, `C7bHOfSr…`, `c8jOrHqy…`).
