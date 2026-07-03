# `.mch` Matcher DSL — Specification

**Version:** 0.1
**Status:** Draft

## 1. Purpose

`.mch` is a domain-specific language for declaring block-tree pattern matchers in the TON event classifier. A matcher recognises a protocol-specific shape inside a trace (a tree of `Block` objects) and collapses it into a richer typed `Block` that downstream code serialises into an `Action`.

The DSL replaces hand-written `BlockMatcher` subclasses with declarative spec files. The matcher engine, block representation, and `build_block` / predicate / merge functions remain in the host language (Python today, C++ in the future).

A single `.mch` file is the source of truth for both Python and C++ implementations. Each host language ships a parser and a compiler that lower the spec to its own engine. **Behaviour parity across host languages is the load-bearing invariant.**

## 2. File structure

- Extension: `.mch`
- Encoding: UTF-8
- One file may contain any number of top-level declarations
- Declaration order is significant only for `opcode` and `predicate` (must be declared before use). Order is not significant for `rule`, `matcher`, `stitch` — forward references are allowed
- Multiple files in the same directory share a single logical namespace; cross-file references are allowed
- Convention: one protocol per file (`jettons.mch`, `stonfi_v2.mch`, `dedust_deposit.mch`)

## 3. Lexical structure

### 3.1 Identifiers

```
identifier := [A-Za-z_][A-Za-z0-9_]*
```

Case-sensitive. By convention:
- `PascalCase` for opcode constant names (`JettonTransfer`)
- `snake_case` for btype names, rule names, matcher names, predicate names, function names

### 3.2 Numeric literals

```
hex_literal      := "0x" [0-9A-Fa-f]+
dec_literal      := [0-9]+
duration_literal := dec_literal ("s" | "m" | "h" | "d")
```

Opcodes accept hex or decimal. TTL values use the duration form.

### 3.3 String literals

Standard double-quoted strings with backslash escapes. Reserved for future use; not currently used in pattern expressions (btype names are unquoted identifiers).

### 3.4 Punctuation and operators

| Token | Meaning |
|---|---|
| `->` | child edge ("then") |
| `<-` | parent edge ("before") |
| `\|` | alternative (or) |
| `^` | exclusive alternative (xor) — see §6.6 |
| `@` | capture prefix |
| `$` | rule reference prefix |
| `{` `}` | block (children set or declaration body) |
| `(` `)` | grouping (alternatives) |
| `=` | assignment (`opcode`, `rule`) |
| `.` | attribute access (in stitch key expressions) |
| `;` | optional statement separator inside children blocks |

### 3.5 Keywords

Reserved. May not be used as identifiers.

```
opcode predicate rule matcher stitch
op btype pred any
maybe where
produces build entry include_excess include_bounces
partial final key ttl merge
true false
```

### 3.6 Comments

```
# this is a line comment
```

Line comments only — extend to end of line. No block comments.

### 3.7 Whitespace

Spaces, tabs, and newlines are token separators with no semantic significance. Indentation is purely cosmetic; the parser must accept any consistent formatting.

**Exception**: inside a children block `{ ... }`, newlines act as item separators (equivalent to `;`). See §6.5.

## 4. Grammar (EBNF)

```ebnf
file              ::= top_decl*

top_decl          ::= opcode_decl
                    | predicate_decl
                    | rule_decl
                    | matcher_decl
                    | stitch_decl

opcode_decl       ::= "opcode" IDENT "=" (HEX | DEC)
predicate_decl    ::= "predicate" IDENT

rule_decl         ::= "rule" IDENT "=" pattern_expr

matcher_decl      ::= "matcher" IDENT "{" matcher_body "}"
matcher_body      ::= matcher_directive* pattern_expr

matcher_directive ::= "produces"        IDENT
                    | "build"           IDENT
                    | "entry"           "@" IDENT
                    | "include_excess"  bool_literal
                    | "include_bounces" bool_literal

stitch_decl       ::= "stitch" IDENT "{" stitch_directive+ "}"
stitch_directive  ::= "partial" IDENT
                    | "final"   IDENT
                    | "key"     capture_path
                    | "ttl"     DURATION
                    | "merge"   IDENT

pattern_expr      ::= sequence ("|" sequence)*
                    | sequence ("^" sequence)*    (* exclusive alternative — exactly one branch matches; mixing | and ^ at the same nesting level is a parser error P010 *)
sequence          ::= atom (edge atom)*
edge              ::= "->" | "<-"

atom              ::= node
                    | "(" pattern_expr ")"
                    | rule_ref
                    | children_block
                    | "maybe" atom

node              ::= node_head capture? predicate_clause?
node_head         ::= "op"    opcode_ref
                    | "btype" IDENT
                    | "pred"  IDENT
                    | "any"

opcode_ref        ::= IDENT | HEX | DEC
capture           ::= "@" IDENT
predicate_clause  ::= "where" IDENT

rule_ref          ::= "$" IDENT

children_block    ::= "{" children_item (children_sep children_item)* children_sep? "}"
children_item     ::= pattern_expr
children_sep      ::= ";" | NEWLINE

capture_path      ::= "@" IDENT ("." IDENT)*

bool_literal      ::= "true" | "false"
```

## 5. Top-level declarations

### 5.1 `opcode IDENT = VALUE`

Declares a named opcode constant. Used to refer to opcodes symbolically inside the file. The host language must register a binding `IDENT → integer` at startup; the compiler verifies the host and DSL agree on the value.

```
opcode JettonTransfer = 0x0f8a7ea5
```

Duplicate declarations within a file are an error. Across files, the same name must agree on value.

### 5.2 `predicate IDENT`

Declares that a predicate function with this name is available in the host language. The function signature is host-specific (see §11). The compiler does not introspect the body; it only verifies the name is registered.

```
predicate pton_self_transfer
```

### 5.3 `rule IDENT = pattern_expr`

Defines a named sub-pattern. Rules may be referenced as `$IDENT` from anywhere in the namespace. Rules support self-reference and mutual recursion; cycles are resolved at compile time (§10.3).

```
rule deposit_leg = ( op DedustDepositTONToVault @deposit | ... )
```

### 5.4 `matcher IDENT { ... }`

Defines a matcher. The body contains zero or more directives followed by exactly one `pattern_expr`.

Required directives: `produces`, `build`.
Optional directives: `entry`, `include_excess`, `include_bounces`.

```
matcher jetton_transfer {
    produces JettonTransferBlock
    build    build_jetton_transfer

    op JettonTransfer @transfer
      -> op JettonInternalTransfer @internal
      -> maybe op JettonNotify @notify
}
```

### 5.5 `stitch IDENT { ... }`

Declares a cross-trace join between two matchers (typically `partial` and `final`). Maintains a keyed pending-action table; when both halves are present, the `merge` function is invoked to produce the unified block.

Required directives: `partial`, `final`, `key`, `ttl`, `merge`.

```
stitch dedust_deposit {
    partial dedust_deposit_partial
    final   dedust_deposit_final
    key     @topup.destination
    ttl     6h
    merge   merge_dedust_deposit
}
```

## 6. Pattern expressions

### 6.1 Nodes

A node tests a single block.

| Form | Matches |
|---|---|
| `op IDENT` | `CallContractBlock` with opcode resolved from a declared `opcode` constant |
| `op HEX` / `op DEC` | `CallContractBlock` with literal opcode (discouraged — prefer named constants) |
| `btype IDENT` | block whose `btype` field equals the literal string `IDENT` (unquoted identifier) |
| `pred IDENT` | block satisfying predicate `IDENT` (no opcode/btype constraint) |
| `any` | any block |

### 6.2 Captures

`@name` after a node head binds the matched block under that name. Captures are visible to:

- The `build` function (as fields of the generated Match dataclass — §7)
- The `entry` directive (§8.3)
- The `stitch.key` expression (§9.2)

**Capture name conflicts:**
- A capture name may appear more than once only if (a) all occurrences are inside alternative branches that are mutually exclusive at match time — either an XOR alternative `( A ^ B ^ … )` or an OR alternative `( A | B | … )` whose branches happen to be exclusive — in which case only the winning branch's capture binds and the losers leave the field at default (`None` for scalars, `[]` for list bindings), or (b) all occurrences are inside a recursive rule, in which case the capture binds to a list (§7.2).
- Defining the same capture name in incompatible positions is a compile-time error.

### 6.3 Modifiers

`maybe X` — `X` is optional. If `X` fails to match, the surrounding pattern still succeeds. Captures inside `X` bind to `None` (or `[]` if inside a recursive rule).

`maybe` may wrap any atom: a node, a parenthesised group, a rule reference, or a children block.

### 6.4 Edges

- `A -> B` — `B` is a child of `A`. Structurally: `A.next_blocks` contains `B`; `B.previous_block` is `A`.
- `A <- B` — `B` is a parent of `A`. Equivalent to `B -> A`; compiles to the same constraint.

Edge chains are left-associative: `A -> B -> C` parses as `(A -> B) -> C` and means "C is a child of B which is a child of A". The parser flattens linear chains regardless of line breaks. Indentation is cosmetic.

Mixing directions in a single chain (`A -> B <- C`) is permitted but generally avoided — `A -> B <- C` means "B has child A and B has parent C".

### 6.5 Children sets

`A -> { item1; item2; item3 }` declares an ordered children set on `A`. Each item is a `pattern_expr`.

**Attach-to-left semantics.** A children block always binds to the **immediately preceding lowered matcher** in its containing sequence. Equivalent rules in the lowered tree:

- `op A -> {B; C}` — A has `children_matchers=[B, C]`.
- `op A -> op X -> {B; C}` — X has `children_matchers=[B, C]` (mid-chain CB; X is the left sibling).
- `op A -> {B; C} -> op D` — A has BOTH `children_matchers=[B, C]` AND `child_matcher=D`. A children block does NOT advance the chain cursor.
- `op A -> {B; C} -> {D; E}` — A has `children_matchers=[B, C, D, E]` in source order (back-to-back CBs flatten).
- `op A <- op B -> {C; D}` — A's parent is B; B has `children_matchers=[C, D]` (CB after a PARENT edge attaches to the parent matcher, NOT the anchor).
- `op A -> maybe {B; C}` — A has `children_matchers=[B(optional=True), C(optional=True)]`. Each item independently optional.

**Bare children blocks are rejected** with `C008_CHILDREN_BLOCK_BARE`:

- `{B; C}` as a Sequence head (no left sibling).
- `{B; C}` inside an Alternative branch (no sequence context).
- `maybe {B; C}` standalone (no left sibling).
- `rule r = {A; B}` referenced as a bare RuleRef — RuleRef expansion does not inherit attach-to-left context (v0 limitation; inline the CB at the use site).

**Matching algorithm** (preserves current `children_matchers` semantics):

1. Take the left-sibling matcher's `next_blocks` as the working set.
2. For each item in declaration order:
   a. Find any remaining block that matches the item (first match wins).
   b. If found, consume it from the working set; move to next item.
   c. If the item is `optional=True` (from `maybe {...}`) and no match found, skip.
3. All required items must match for the children block to succeed.

Items are separated by `;` or newlines; either or both are acceptable. Empty items are not allowed.

### 6.6 Alternatives

Two separator forms:

- `A | B | C` — **or-alternative**. Match any one; first that succeeds is chosen. Lowers to `OrMatcher` by default. **Heuristic exclusive-or upgrade**: if every branch is a bare concrete node (`op X` or `btype Y`, no `where` clause) and all heads are pairwise distinct, the compiler lowers to `ExclusiveOrMatcher` automatically — behaviorally equivalent (at most one branch can match) but engine-faster.
- `A ^ B ^ C` — **exclusive-or alternative (XOR)**. Always lowers to `ExclusiveOrMatcher`. `test_self` requires exactly one branch to match (sum == 1). Used when the author wants compiler-verified exclusivity. If two branches share the same concrete head, `W202_EXCLUSIVE_OR_REDUNDANT_HEADS` warns.

**Mixing `|` and `^` at the same nesting level is forbidden** — emits `P010_ALTERNATIVE_SEPARATOR_MIX`. Use parentheses to nest:

```
(op A ^ (op B | op C))    # outer XOR, inner OR — legal (different nesting levels)
(op A | op B ^ op C)      # ERROR: P010 — mixed | and ^
```

Alternatives may appear at any position where an atom is expected; grouping with `( … )` is required when alternatives appear inside a chain or children block:

```
A -> ( B | C ) -> D
```

**Same-name captures across XOR branches** (e.g. `(op A @x ^ op B @x)`) — only the winning branch's capture binds; losing branches leave `@x` at default (`None` for scalars, `[]` for list bindings under recursion). No diagnostic.

### 6.7 Rule references

`$RULENAME` substitutes the pattern bound to `rule RULENAME = …`. References may be self-referential or mutually recursive; the compiler resolves cycles as deferred edges in the structural graph.

A rule reference may be wrapped in `maybe`:

```
maybe $swap_step
```

### 6.8 Predicate clauses

`X where FN` — node `X` matches only if predicate `FN` returns true. The predicate is called with the candidate `Block` after opcode/btype checks pass. On false, the node does not match; the engine backtracks normally.

```
op JettonInternalTransfer @out where pton_self_transfer
```

A node may carry at most one `where` clause. Predicates declared with `predicate FN` (§5.2) are the only valid references.

## 7. Capture semantics

### 7.1 Type bindings

Each capture binds to a typed block:

| Node head | Bound type |
|---|---|
| `op IDENT` / `op HEX` | `CallContractBlock` |
| `btype IDENT` | the typed block class registered for that btype (e.g. `JettonTransferBlock`) |
| `pred IDENT` | `Block` |
| `any` | `Block` |

The code generator emits a typed Match dataclass per matcher with one field per capture. Field types follow §7.2 and §7.3 below.

### 7.2 List bindings under recursion

When a capture appears inside a rule that self-references (directly or indirectly), every successful unrolling produces one binding. The Match dataclass exposes the capture as `list[T]` in trace order, parallel-aligned with peer captures from the same rule unrolling.

Example: `rule swap_step = op SWAP @swap -> op PAY_TO @payout -> ( … | $swap_step )` matched against a three-hop chain produces:
- `swap: list[CallContractBlock]` — length 3
- `payout: list[CallContractBlock]` — length 3, parallel to `swap`

### 7.3 Optional bindings

Captures inside a `maybe`-wrapped subtree bind as `Optional[T]`. If the subtree did not match, the capture is `None`.

Captures inside a `maybe`-wrapped recursive rule remain `list[T]`; absence of the rule means the list is empty (`[]`), not `None`.

### 7.4 Capture scope

A capture defined inside a `rule` is visible to any matcher whose pattern uses `$rule`, and to any stitch declaration whose `key` references it through one of the paired matchers.

## 8. Matcher directives

### 8.1 `produces IDENT`

Host-language class name of the block type this matcher produces. Must exist in the host's block-type registry.

### 8.2 `build IDENT`

Host-language function name for the build callback. Receives a typed Match (§7) and returns one instance of the `produces` type. Host-specific call shape — see §11.

### 8.3 `entry @CAPTURE`

Pins the runtime anchor (`test_self` equivalent) to the captured node. Overrides the compiler's selectivity pick (§10.2).

Constraints:
- The referenced capture must exist in the pattern.
- The referenced node must have a concrete opcode or btype (`op`, `btype`).
- The node must not be inside a `maybe`-wrapped subtree.
- The node must not be inside an alternative branch that may not match.
- The node must not be inside a recursive rule (would produce a list binding, ambiguous).

### 8.4 `include_excess BOOL`

Default: `true`. Controls whether messages with the excess opcode (`ExcessMessage.opcode = 0xd53276db` or equivalent in host) emitted as siblings of matched blocks are auto-appended to the matched block set.

### 8.5 `include_bounces BOOL`

Default: `true`. Controls whether bounce messages (opcode `0xffffffff`) emitted as siblings of matched blocks are auto-appended to the matched block set.

## 9. Stitch directives

### 9.1 `partial IDENT` / `final IDENT`

Matcher names that produce the two halves of the joined action. Both must be declared (in this file or any file in the namespace).

### 9.2 `key capture_path`

A capture path identifying corresponding pairs. Format: `@capture` or `@capture.field` (nested attribute access is allowed: `@capture.parent.field`). The root capture must be defined by BOTH the partial and the final matcher. The path is evaluated against the matched block at runtime.

Equality semantics: the host language compares values with its standard equality operator (string equality for addresses, integer equality for IDs, etc.).

### 9.3 `ttl DURATION`

How long a half waits for its pair before expiring. Format: `<integer><unit>`, unit ∈ `{s, m, h, d}`.

Constraints: must be positive; must not exceed 7 days (`7d`).

### 9.4 `merge IDENT`

Host-language function that receives both halves and returns the unified block. Signature is host-specific — see §11.

## 10. Compilation

### 10.1 Lowering target

Each matcher compiles to one instance of the host engine's matcher type. In Python (today), this is a `BlockMatcher` subclass. In C++ (future), this is a `MatcherProgram` value interpreted by the engine.

Lowering steps:

1. Parse `.mch` files into an AST.
2. Resolve `opcode` and `predicate` references across the namespace.
3. Resolve `$rule` references; flatten linear `->` and `<-` chains; lower `maybe` into `optional=true` on the relevant engine constructs.
4. Select runtime anchor (§10.2).
5. Build the dependency graph; detect cycles (allowed for recursion).
6. Emit the compiled matcher and the typed Match dataclass.
7. Register in the engine's matcher list and any stitch tables.

### 10.2 Anchor selection

If the matcher declares `entry @CAPTURE`, that node is the runtime anchor.

Otherwise, the compiler:

1. Walks the pattern AST.
2. Collects all node heads with a concrete opcode or btype, EXCLUDING nodes inside `maybe`-wrapped subtrees, alternative branches, or recursive rule references.
3. Ranks candidates by selectivity:
   - Opcodes ranked by frequency in `opcode_freq.json` (corpus statistic file shipped alongside matchers). Rarer opcode = better anchor.
   - Btypes ranked similarly via `btype_freq.json`.
4. Picks the rarest. Ties broken by earliest source position.
5. If selectivity stats are missing for a candidate, emits a warning and picks the syntactically first candidate.

The selected anchor's structural position determines whether the runtime walker traverses `parent_matcher` first, `child_matcher` first, or `children_matchers` — derived mechanically from the AST.

### 10.3 Recursion resolution

A `$rule_name` reference compiles to a back-edge in the matcher's structural graph. Mutual recursion across rules is allowed and forms a cyclic graph. The engine walks this graph with depth bounded by the trace size (which is always finite). No fixpoint computation is required.

A `maybe $rule` is the natural termination case for unbounded recursion — the engine treats it as the "stop here" branch.

### 10.4 Backtracking semantics

The engine matches greedily within a single node head but backtracks across alternatives and optional skips. Captures bound during a failed branch are discarded; only captures from the eventually-successful branch persist in the final Match value.

When multiple matchers could match the same anchor block, the engine resolves conflicts in matcher registration order (host-specific; see §11). The first matcher whose pattern fully succeeds consumes the blocks. Subsequent matchers see the already-consumed blocks as upstream-produced and may skip or fail their own attempts.

## 11. Host-language contract

A host language provides five registries, populated at startup:

| Registry | Keyed by | Value type |
|---|---|---|
| Opcodes | identifier from `opcode IDENT = …` | integer |
| Block types | identifier from `matcher.produces` | class / type tag |
| Predicates | identifier from `predicate IDENT` | callable `(Block) → bool` |
| Builders | identifier from `matcher.build` | callable `(Match) → Block` |
| Mergers | identifier from `stitch.merge` | callable `(partial_match, final_match) → Block` |

The compiler validates at load time that every reference resolves; otherwise the load fails with a precise error citing the unresolved name and its source location.

### 11.1 Python contract

- Generated Match dataclasses live alongside the spec compiler's output module; the builder imports them by name.
- Builders are `async` functions:
  ```python
  async def build_jetton_transfer(m: JettonTransferMatch) -> JettonTransferBlock: ...
  ```
- Predicates are sync functions:
  ```python
  def pton_self_transfer(block: Block) -> bool: ...
  ```
- Mergers are `async`:
  ```python
  async def merge_dedust_deposit(p: DedustDepositPartialMatch,
                                  f: DedustDepositFinalMatch) -> DedustDepositLiquidity: ...
  ```
- Registration is via decorator: `@build("matcher_name")`, `@predicate("name")`, `@merge("stitch_name")`.

### 11.2 C++ contract

- Match types are generated headers (`generated/matchers/<matcher_name>_match.h`) emitted by the build pipeline alongside the engine.
- Builders are registered via `register_builder("name", &fn)`. Signatures use `std::function<Block(Match const&)>` or coroutine equivalents.
- Predicates: `bool fn(Block const&)`.
- Merge contract mirrors Python's.

### 11.3 Cross-language invariants

For a given `.mch` file and a given trace:
- The set of matchers that fire is identical in both languages.
- The set of blocks each matcher consumes is identical.
- The captures bound for each fired matcher are identical (modulo trivial type differences in block field representations).

The implementation must include a cross-language fidelity test harness that runs the same trace corpus through both engines and asserts byte-equivalent matched-block trees.

## 12. Validation rules

### 12.1 Errors (abort compilation)

- An `opcode IDENT = V` declaration's value disagrees with the host registry.
- A `op IDENT` references an undeclared opcode name.
- A `pred IDENT` or `where IDENT` references a predicate not in the registry.
- A `$rule` references a rule with no definition.
- A `matcher` lacks `produces` or `build`.
- A `matcher.entry @X` references a capture not defined in the pattern.
- An `entry` capture violates §8.3 constraints (inside `maybe`, alternative, or recursive rule).
- A `stitch.key` capture is not defined in both `partial` and `final` matchers.
- A `stitch.ttl` is non-positive or exceeds `7d`.
- A capture name is defined multiple times in incompatible positions (§6.2).
- A matcher's `produces` type is not in the block-type registry (`C003_UNKNOWN_BLOCK_TYPE`).
- A matcher's `build` function is not in the builder registry (`C002_UNKNOWN_BUILDER`).
- A stitch's `merge` function is not in the merger registry.
- A children block contains an empty item.
- A children block appears without a left sibling — Sequence head, bare in Alternative, bare `maybe {...}`, or rule body root referenced standalone (`C008_CHILDREN_BLOCK_BARE`).
- A cycle exists in rule references with no `maybe`-wrapped termination — the recursion is unbounded.
- Indirect (mutual) recursion in rule references (`C005_INDIRECT_RECURSION_UNSUPPORTED`).
- Unsupported recursion shape — e.g. a recursive rule whose step branch has no concrete head (`C001_RECURSION_SHAPE_UNSUPPORTED`).
- A capture is bound at a position after a recursive rule reference, which would silently lose values (`C007_CAPTURE_AFTER_RECURSION`).
- Mixing `|` and `^` separators at the same nesting level (`P010_ALTERNATIVE_SEPARATOR_MIX`).
- Matcher lacks the required `entry @CAPTURE` directive and no selectivity stats are available (`R008_ENTRY_REQUIRED`).

### 12.2 Warnings (non-fatal, emitted to stderr)

- Selectivity stats missing for some anchor candidates (`W201_NO_SELECTIVITY_STATS`).
- Two branches of an explicit exclusive alternative (`^`) share the same concrete head — sum-check guarantees the alternative never matches that head (`W202_EXCLUSIVE_OR_REDUNDANT_HEADS`). Non-blocking; lowering still produces `ExclusiveOrMatcher`.
- Stitch declaration deferred — parser accepts, compiler skips (`W900_STITCH_DEFERRED`).
- A rule is declared but never referenced.
- A capture is declared but unused by the builder (when the compiler can introspect the builder signature).
- An opcode constant is declared but never referenced.
- A matcher overrides `include_excess` / `include_bounces` to a non-default value (informational; helps reviewers spot intentional deviations).

## 13. Error messages

All diagnostics carry source location: file path, line, column, span. Format:

```
matchers/dedust_deposit.mch:42:8: error: undefined rule reference $exces_block (did you mean $excess_block?)
        children { $exces_block; $excess_block }
                   ^^^^^^^^^^^^
```

Each diagnostic includes:
- File path (relative to the matcher root or absolute, consistent within a run)
- Line and column of the first offending token
- A `^^^` underline marking the span
- A precise human-readable message
- Optional "did you mean …" suggestion based on edit distance against in-scope identifiers

Warnings use the same format with `warning:` instead of `error:`.

## 14. Worked examples

These are canonical reference inputs. Implementations must produce the documented Match types and observe the documented matching behaviour.

### 14.1 Jetton transfer (simple linear)

```
opcode JettonTransfer         = 0x0f8a7ea5
opcode JettonInternalTransfer = 0x178d4519
opcode JettonNotify           = 0x7362d09c

matcher jetton_transfer {
    produces JettonTransferBlock
    build    build_jetton_transfer

    op JettonTransfer @transfer
      -> op JettonInternalTransfer @internal
      -> maybe op JettonNotify @notify
}
```

Generated Python Match:

```python
@dataclass
class JettonTransferMatch:
    transfer: CallContractBlock
    internal: CallContractBlock
    notify:   CallContractBlock | None
```

### 14.2 Stonfi V2 swap (recursion + bidirectional + predicate)

```
opcode StonfiV2Swap           = 0x6664de2a
opcode StonfiV2PayTo          = 0x657b54f5
opcode JettonNotify           = 0x7362d09c
opcode PTonTransfer           = 0x01f3835d
opcode JettonInternalTransfer = 0x178d4519

predicate pton_self_transfer

rule in_transfer = (
    btype jetton_transfer @in_transfer
  | op PTonTransfer @in_transfer -> op JettonNotify
)

rule swap_step =
    op StonfiV2Swap @swap
      -> op StonfiV2PayTo @payout
      -> (
          op JettonInternalTransfer @out_transfer where pton_self_transfer
            -> op JettonNotify -> maybe $swap_step
        | btype jetton_transfer @out_transfer -> maybe $swap_step
        | $swap_step
        )

matcher stonfi_v2_swap {
    produces JettonSwapBlock
    build    build_stonfi_v2_swap
    entry    @swap

    $in_transfer -> $swap_step
}
```

Generated Python Match:

```python
@dataclass
class StonfiV2SwapMatch:
    in_transfer:  Block                    # JettonTransferBlock | CallContractBlock
    swap:         list[CallContractBlock]  # one per hop
    payout:       list[CallContractBlock]  # parallel to swap
    out_transfer: Block | None             # only set on the terminating hop
```

### 14.3 Dedust deposit (partial + final + stitch)

```
opcode JettonTransfer                        = 0x0f8a7ea5
opcode JettonInternalTransfer                = 0x178d4519
opcode JettonNotify                          = 0x7362d09c

opcode DedustDepositTONToVault               = 0xd55e4686
opcode DedustAskLiquidityFactory             = 0x21cfe02b
opcode DedustDeployDepositContract           = 0x4a70c0d2
opcode DedustTopUpLiquidityDepositContract   = 0xb56b9598
opcode DedustDepositLiquidityToPool          = 0xb9b8d8e6
opcode DedustReturnExcessFromVault           = 0x474f86cf
opcode DedustPayout                          = 0xad4eb6f5
opcode DedustDestroyLiquidityDepositContract = 0x71f50a9d
opcode DedustRejection                       = 0xe1a36cd4

rule deposit_leg = (
    op DedustDepositTONToVault @deposit
  | op JettonTransfer @user_transfer
      -> op JettonInternalTransfer
      -> op JettonNotify @deposit
)

rule excess_subtree =
    maybe op DedustReturnExcessFromVault
      -> {
          maybe btype jetton_transfer @jetton_excess
          maybe op DedustPayout       @ton_excess
      }

matcher dedust_deposit_final {
    produces DedustDepositLiquidity
    build    build_dedust_final
    entry    @pool

    $deposit_leg
      -> op DedustAskLiquidityFactory
      -> {
          maybe op DedustDeployDepositContract
          op DedustTopUpLiquidityDepositContract @topup
            -> op DedustDepositLiquidityToPool @pool
            -> {
                ( op JettonInternalTransfer @lp_transfer -> maybe op JettonNotify
                | op DedustRejection @rejection -> $excess_subtree )
                maybe op DedustDestroyLiquidityDepositContract -> $excess_subtree
            }
      }
}

matcher dedust_deposit_partial {
    produces DedustDepositLiquidityPartial
    build    build_dedust_partial
    entry    @topup

    $deposit_leg
      -> op DedustAskLiquidityFactory
      -> op DedustTopUpLiquidityDepositContract @topup
}

stitch dedust_deposit {
    partial dedust_deposit_partial
    final   dedust_deposit_final
    key     @topup.destination
    ttl     6h
    merge   merge_dedust_deposit
}
```

Generated Python Matches:

```python
@dataclass
class DedustDepositFinalMatch:
    deposit:       Block
    user_transfer: CallContractBlock | None
    topup:         CallContractBlock
    pool:          CallContractBlock
    lp_transfer:   CallContractBlock | None
    rejection:     CallContractBlock | None
    jetton_excess: Block              | None
    ton_excess:    CallContractBlock  | None

@dataclass
class DedustDepositPartialMatch:
    deposit:       Block
    user_transfer: CallContractBlock | None
    topup:         CallContractBlock

@dataclass
class DedustDepositStitchInput:
    partial: DedustDepositPartialMatch
    final:   DedustDepositFinalMatch
```

## 15. Conventions

- One protocol per file
- Opcode declarations at the top of the file
- Predicates declared next
- Shared rules before the matchers that use them
- Stitch declarations at the end of the file
- Matchers ordered by selectivity within the file (most-specific first) for human readability — the compiler does not depend on this order

## 16. Future extensions (non-normative)

Listed only so implementers do not paint themselves into a corner. NOT part of v0.1.

- Field constraints inline on captures: `op JettonTransfer @t where t.amount > 0`
- Quantifiers on children-set items: `one_or_more`, `zero_or_more`
- Static priority annotation overriding registration order
- Multi-arity merge for stitches (3+ matchers)
- A `using` import system for cross-file rule references with namespacing
- `pre_build_auto_append` finer-grained control over excess/bounce timing
- Pattern fragments parameterised by capture (`rule X(@a) = …`)

## 17. Glossary

**Anchor / `test_self`**: The block whose opcode/btype the engine checks first when scanning a trace. Determines whether the matcher attempts to match at all for a given block.

**Block**: A node in the matched tree. Either a primitive (`CallContractBlock`, `TonTransferBlock`, `TickTockBlock`) or a typed protocol block produced by an upstream matcher.

**Capture**: A named binding from a position in the pattern to a matched block. Exposed to the builder as a typed Match dataclass field.

**Children block**: An ordered set of patterns matched against a parent's direct children. Preserves current `children_matchers` semantics: positional, greedy, first-match-wins.

**Match**: The typed object passed to a builder, containing all captures.

**Rule**: A named pattern fragment, referenced via `$IDENT`. Supports recursion and reuse.

**Stitch**: A cross-trace join that pairs two matchers (typically `partial` + `final`) by a shared key, producing a unified block via a merge function.
