# `.mch` Matcher DSL — Specification

**Version:** 0.2
**Status:** Draft
**Supersedes:** 0.1 (archived at `docs/mch-spec-v0.1-backup.md`)

## 0. Changes from 0.1

- **`stitch` removed, no replacement in the language.** Cross-trace joining (pending tables, TTL, persistence) is permanently out of scope — it contradicts the domain model. Intra-trace merging of paired matcher results (dedust partial/final deposit) stays **host-side post-process code** (Python `post_process_dedust_liquidity`, C++ equivalent) — too rare and protocol-specific to warrant language surface (§5.5).
- **Milestones introduced** (§1.1). Every feature carries a milestone tag `[M1]`…`[M5]`. Untagged = M1.
- **Build language added `[M4]`** (§9): `parse`, `let`, `lookup`, `out`, `reject when`, `failed when`, `broken when`, `flow`, `omit`, `produces switch`, host `fn` declarations. Moves the mechanical majority of `build_block` bodies into the spec file; host functions remain as escape hatches for genuine corner cases.
- **Inline field predicates `[M4]`** promoted from future-extensions (§6.8).
- **Anchor opcode-sets `[M2]`** (§8.3): `entry` may reference a capture on an XOR alternative of concrete heads (needed for matchers like jetton_mint that anchor on two opcodes).
- **`produces` union `[M2]` and `produces switch` `[M4]`** (§8.1): one matcher may produce different block types depending on parsed body content (needed for DNS).
- **Recursion capture semantics fixed** (§7.2): v0.1's §14.2 example contradicted §7.2. New rule (final form settled by T18): captures inside a recursive rule bind per-unrolling lists; step-branch captures are gap-aligned (one element per unrolling, `None` where the capture's branch lost), exit-branch captures bind once on termination via the exit, and captures positioned after the recursion in traversal order bind as ordinary scalars.
- **Determinism, ordering, and build-rejection semantics pinned** (§10.5–10.7). These were parity holes: registration order, children-candidate ordering, and builder-rejection behavior are now normative.
- **Compilation artifact strategy `[M5]`** (§11.4): one frontend, a language-neutral IR, thin per-host interpreters — instead of two independent parser+compiler stacks.

## 1. Purpose

`.mch` is a domain-specific language for declaring block-tree pattern matchers in the TON event classifier. A matcher recognises a protocol-specific shape inside a trace (a tree of `Block` objects) and collapses it into a richer typed `Block` that downstream code serialises into an `Action`.

The DSL replaces hand-written `BlockMatcher` subclasses with declarative spec files. Through M3, `build` / predicate functions remain in the host language (Python today, C++ later). From M4, most build logic is expressed in the DSL itself; host functions remain for corner cases. Post-process passes (block merging, deployment unwinding) stay host code permanently (§5.5).

A single `.mch` file is the source of truth for all host languages. **Behaviour parity across host languages is the load-bearing invariant.** From M5 parity is achieved structurally: one frontend compiles `.mch` to a neutral IR consumed by every engine (§11.4).

### 1.1 Milestones

| Milestone | Scope | Matchers it unlocks |
|---|---|---|
| **M1 — Core** | Linear chains, `maybe`, alternatives, children sets, recursion, captures, external builders/predicates, mandatory `entry` | jetton_transfer, jetton_burn, dns (host build picks btype), simple single-opcode matchers |
| **M2 — Pattern completeness** | XOR `^`, attach-to-left children blocks, multi-hop parent chains, list bindings, opcode-set anchors, `produces` union, same-name captures across exclusive branches | jetton_mint, stonfi_v2_swap, dedust_swap, dedust_withdraw, tonco, coffee |
| **M3 — Integration** | `priority`, determinism pins, build-rejection contract, production wiring behind feature flag, corpus parity harness | dedust_deposit_liquidity (host post-process merge), production rollout |
| **M4 — Build language** | `parse`, `let`/`lookup`, `out`, `reject/failed/broken when`, inline field predicates, `produces switch`, `flow`, `fn` | build bodies become declarative; host fns only for corner cases |
| **M5 — IR & C++** | Neutral IR artifact, Python engine consumes IR, C++ engine consumes IR, cross-language fidelity harness | C++ indexer runs the same matchers |

Status tracking and the ordered task list live in `docs/mch-roadmap.md`.

## 2. File structure

- Extension: `.mch`
- Encoding: UTF-8
- One file may contain any number of top-level declarations
- Declaration order is significant only for `opcode` and `predicate` (must be declared before use). Order is not significant for `rule`, `matcher` — forward references are allowed
- Multiple files in the same directory share a single logical namespace; cross-file references are allowed `[M2]`
- Convention: one protocol per file (`jettons.mch`, `stonfi_v2.mch`, `dedust_deposit.mch`)

## 3. Lexical structure

### 3.1 Identifiers

```
identifier := [A-Za-z_][A-Za-z0-9_]*
```

Case-sensitive. By convention:
- `PascalCase` for opcode constant names (`JettonTransfer`) and message type names
- `snake_case` for btype names, rule names, matcher names, predicate names, function names

### 3.2 Numeric literals

```
hex_literal      := "0x" [0-9A-Fa-f]+
dec_literal      := [0-9]+
```

Opcodes accept hex or decimal. (Duration literals existed in v0.1 for stitch TTL; removed.)

### 3.3 String literals

Standard double-quoted strings with backslash escapes. Used in `[M4]` expressions; reserved otherwise.

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
| `(` `)` | grouping (alternatives); call syntax `[M4]` |
| `=` | assignment (`opcode`, `rule`, `let`) |
| `.` | attribute access (key expressions; `[M4]` field access) |
| `;` | optional statement separator inside children blocks |
| `:` | field separator in `out` blocks `[M4]` |
| `??` | default-value operator `[M4]` |
| `==` `!=` `<` `<=` `>` `>=` | comparison `[M4]` |
| `=>` | switch branch arrow `[M4]` |

### 3.5 Keywords

Reserved. May not be used as identifiers.

```
opcode predicate rule matcher
op btype pred any
maybe where
produces build entry include_excess include_bounces priority
parse as let out reject failed broken when switch else fn flow omit lookup
true false null
and or not
```

The v0.1 keywords `stitch`, `partial`, `final`, `key`, `ttl`, `merge` are removed from the language but remain reserved so the parser can emit a targeted migration error (`P012_STITCH_REMOVED`: "stitch was removed in 0.2; paired results are merged by host post-process code").

### 3.6 Comments

```
# this is a line comment
```

Line comments only — extend to end of line. No block comments.

### 3.7 Whitespace

Spaces, tabs, and newlines are token separators with no semantic significance. Indentation is purely cosmetic; the parser must accept any consistent formatting.

**Exception**: inside a children block `{ ... }` and inside an `out` block `[M4]`, newlines act as item separators (equivalent to `;`).

## 4. Grammar (EBNF)

Productions marked `(* M4 *)` belong to the build language and are absent until M4. Productions marked `(* M3 *)` belong to priority ordering.

```ebnf
file              ::= top_decl*

top_decl          ::= opcode_decl
                    | predicate_decl
                    | fn_decl            (* M4 *)
                    | rule_decl
                    | matcher_decl

opcode_decl       ::= "opcode" IDENT "=" (HEX | DEC)
predicate_decl    ::= "predicate" IDENT
fn_decl           ::= "fn" IDENT "(" [IDENT ("," IDENT)*] ")"          (* M4 *)

rule_decl         ::= "rule" IDENT "=" pattern_expr

matcher_decl      ::= "matcher" IDENT "{" matcher_body "}"
matcher_body      ::= matcher_directive* pattern_expr build_stmt*      (* build_stmt: M4 *)

matcher_directive ::= "produces"        produces_spec
                    | "build"           IDENT
                    | "entry"           "@" IDENT
                    | "include_excess"  bool_literal
                    | "include_bounces" bool_literal
                    | "priority"        DEC                            (* M3 *)

produces_spec     ::= IDENT ("|" IDENT)*                               (* union: M2 *)
                    | "switch" "{" switch_branch+ ["else" "=>" IDENT out_block?] "}"   (* M4 *)
switch_branch     ::= "when" expr "=>" IDENT out_block?                (* M4 *)

build_stmt        ::= "parse" IDENT "as" IDENT                         (* M4 *)
                    | "let" IDENT "=" expr                             (* M4 *)
                    | "reject" "when" expr                             (* M4 *)
                    | "failed" "when" expr                             (* M4 *)
                    | "broken" "when" expr                             (* M4 *)
                    | "omit" "@" IDENT "when" expr                     (* M4 *)
                    | "flow" IDENT "(" [expr ("," expr)*] ")"          (* M4 *)
                    | out_block                                        (* M4 *)
out_block         ::= "out" "{" out_field (out_sep out_field)* out_sep? "}"
out_field         ::= IDENT ":" expr
out_sep           ::= ";" | NEWLINE

pattern_expr      ::= sequence ("|" sequence)*
                    | sequence ("^" sequence)*
                      (* exclusive alternative; mixing | and ^ at the same
                         nesting level is parser error P010 *)
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
                    | "where" field_test                               (* M4 *)
field_test        ::= "." IDENT ("==" | "!=") literal                  (* M4 *)

rule_ref          ::= "$" IDENT

children_block    ::= "{" children_item (children_sep children_item)* children_sep? "}"
children_item     ::= pattern_expr
children_sep      ::= ";" | NEWLINE

capture_path      ::= "@" IDENT ("." IDENT)*

bool_literal      ::= "true" | "false"
literal           ::= HEX | DEC | STRING | bool_literal | "null"

(* M4 expression grammar — deliberately small; see §9.6 *)
expr              ::= or_expr
or_expr           ::= and_expr ("or" and_expr)*
and_expr          ::= not_expr ("and" not_expr)*
not_expr          ::= ["not"] cmp_expr
cmp_expr          ::= default_expr [("==" | "!=" | "<" | "<=" | ">" | ">=") default_expr]
default_expr      ::= postfix_expr ("??" postfix_expr)*
postfix_expr      ::= primary ("." IDENT | "(" [expr ("," expr)*] ")")*
primary           ::= IDENT | literal | "(" expr ")"
                    | expr "if" expr "else" expr
```

## 5. Top-level declarations

### 5.1 `opcode IDENT = VALUE`

Declares a named opcode constant. Used to refer to opcodes symbolically inside the file. The host language must register a binding `IDENT → integer` at startup; the compiler verifies the host and DSL agree on the value.

```
opcode JettonTransfer = 0x0f8a7ea5
```

Duplicate declarations within a file are an error. Across files, the same name must agree on value.

### 5.2 `predicate IDENT`

Declares that a predicate function with this name is available in the host language. The function is synchronous and pure: `(Block) → bool` (§11). The compiler does not introspect the body; it only verifies the name is registered.

```
predicate pton_self_transfer
```

Note `[M4]`: predicates stay synchronous. Decisions that need async data (interface repository lookups) do not belong in predicates — they belong in the build phase via `lookup` + `reject when` (§9.3), which runs after the structural match.

### 5.3 `rule IDENT = pattern_expr`

Defines a named sub-pattern. Rules may be referenced as `$IDENT` from anywhere in the namespace. Rules support direct self-reference; cycles are resolved at compile time (§10.3). Indirect (mutual) recursion is rejected (`C005`).

```
rule deposit_leg = ( op DedustDepositTONToVault @deposit | ... )
```

`[M2R, R11/T28]` A self-referential rule selects its recursion strategy with the contextual modifier `cyclic` between the name and `=` (§10.3):

```
rule tonco_hop cyclic = ( op Swap -> op PayTo -> ( … -> $tonco_hop | …final… ) )
```

Without the modifier the default frontier strategy applies. The modifier on a non-recursive rule is ignored.

### 5.4 `matcher IDENT { ... }`

Defines a matcher. The body contains zero or more directives, exactly one `pattern_expr`, and `[M4]` zero or more build statements.

Required directives: `produces`, and `build` (until M4; from M4 a matcher with a complete `out` mapping may omit `build`).
Optional directives: `entry` (currently required in practice — see §10.2), `include_excess`, `include_bounces`, `priority` `[M3]`.

```
matcher jetton_transfer {
    produces JettonTransferBlock
    build    build_jetton_transfer

    op JettonTransfer @transfer
      -> op JettonInternalTransfer @internal
      -> maybe op JettonNotify @notify
}
```

### 5.5 Result merging is host-side (non-feature)

Some protocols produce two related blocks in one trace that are better presented as one action (dedust two-branch liquidity deposit: partial + final). This merging is **not part of the DSL** and never will be:

- **Cross-trace** joining (v0.1's `stitch`: pending tables, TTL, persistence) contradicts the domain model — a trace is processed in isolation, always.
- **Intra-trace** merging is real but rare and protocol-specific — one known production case. It stays hand-written host code: a post-matching pass in each host (Python `post_process_dedust_liquidity`, C++ equivalent), maintained alongside the other host registries.

The DSL's responsibility ends at producing the partial and final blocks (§14.3). One constraint for ported matchers: mch-produced blocks must instantiate the same registered block classes the host post-process pass dispatches on, so the pass works identically whether the blocks came from hand-written or mch matchers.

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
- `[M4]` build statements (§9)

**Capture name conflicts:**
- A capture name may appear more than once only if (a) all occurrences are inside alternative branches that are mutually exclusive at match time — either an XOR alternative `( A ^ B ^ … )` or an OR alternative whose branches happen to be exclusive — in which case only the winning branch's capture binds and the losers leave the field at default (`None` for scalars, `[]` for list bindings), or (b) all occurrences are inside a recursive rule, in which case the capture binds to a list (§7.2).
- Defining the same capture name in incompatible positions is a compile-time error (`R005`).

### 6.3 Modifiers

`maybe X` — `X` is optional. If `X` fails to match, the surrounding pattern still succeeds. Captures inside `X` bind to `None` (or `[]` if inside a recursive rule).

`maybe` may wrap any atom: a node, a parenthesised group, a rule reference, or a children block.

### 6.4 Edges

- `A -> B` — `B` is a child of `A`. Structurally: `A.next_blocks` contains `B`; `B.previous_block` is `A`.
- `A <- B` — `B` is a parent of `A`. Equivalent to `B -> A`; compiles to the same constraint.

Edge chains are left-associative: `A -> B -> C` parses as `(A -> B) -> C` and means "C is a child of B which is a child of A". The parser flattens linear chains regardless of line breaks. Indentation is cosmetic.

Multi-hop parent chains (`A <- B <- C`) are supported `[M2]`.

Mixing directions in a single chain is constrained. Child-chain collection stops at the first parent edge. A parent edge whose left side is the anchor (`A <- B`, and multi-hop `A <- B <- C`) lowers to the anchor's `parent_matcher` chain. A parent edge whose left side is a **non-anchor** node reached through a preceding child edge (`A -> B <- C`, where `<- C` would attach to `B` — itself a child of `A`) is rejected with `C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED`: its only faithful lowering sets `B.parent_matcher = C`, and at runtime that tests `B.previous_block` (which is the anchor `A`) against opcode `C`, so it can never match. Split the pattern or re-anchor. Children blocks do not advance the chain cursor (§6.5), so a parent edge preceded only by children blocks (`A -> {B; C} <- D`) still belongs to the anchor and is legal `[M2, T15]`.

### 6.5 Children sets `[M2 for non-anchor positions]`

`A -> { item1; item2; item3 }` declares an ordered children set on `A`. Each item is a `pattern_expr`.

**Attach-to-left semantics.** A children block always binds to the **immediately preceding lowered matcher** in its containing sequence. Equivalent rules in the lowered tree:

- `op A -> {B; C}` — A has `children_matchers=[B, C]`.
- `op A -> op X -> {B; C}` — X has `children_matchers=[B, C]` (mid-chain CB; X is the left sibling).
- `op A -> {B; C} -> op D` — A has BOTH `children_matchers=[B, C]` AND `child_matcher=D`. A children block does NOT advance the chain cursor.
- `op A -> {B; C} <- op D` — A has BOTH `children_matchers=[B, C]` AND `parent_matcher=D` `[M2, T15]`. Because the CB did not advance the cursor, the parent edge still belongs to A; this is NOT the C009 shape (the dedust-swap anchor uses it).
- `op A -> {B; C} -> {D; E}` — A has `children_matchers=[B, C, D, E]` in source order (back-to-back CBs flatten).
- `op A <- op B -> {C; D}` — A's parent is B; B has `children_matchers=[C, D]` (CB after a PARENT edge attaches to the parent matcher, NOT the anchor).
- `op A -> maybe {B; C}` — A has `children_matchers=[B(optional=True), C(optional=True)]`. Each item independently optional.

**Bare children blocks are rejected** with `C008_CHILDREN_BLOCK_BARE`:

- `{B; C}` as a Sequence head (no left sibling).
- `{B; C}` inside an Alternative branch (no sequence context).
- `maybe {B; C}` standalone (no left sibling).
- `rule r = {A; B}` referenced as a bare RuleRef — RuleRef expansion does not inherit attach-to-left context (v0 limitation; inline the CB at the use site).

**Matching algorithm** (preserves current `children_matchers` semantics):

1. Take the left-sibling matcher's `next_blocks` as the working set, in canonical candidate order (§10.5).
2. For each item in declaration order:
   a. Find the first remaining block (in candidate order) that matches the item.
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

**Engine contract (normative, parity-critical):** `ExclusiveOrMatcher` overrides only `test_self` (sum == 1); `try_build` is inherited from `OrMatcher` (first-match-wins, branch order preserved, `test_self` not consulted). The heuristic `|`→XOR upgrade is behaviorally inert precisely because of this split — walk-level comparison (2026-07-03) confirmed identical try_build sequences under both classes. Any host implementation must replicate exactly this: the exclusivity check lives in `test_self` only, and becomes observable solely where an alternative's `test_self` is consulted directly (e.g. as a recursion step head or nested inside another alternative's probe). A conformance test must pin that placement (roadmap T19).

### 6.7 Rule references

`$RULENAME` substitutes the pattern bound to `rule RULENAME = …`. References may be self-referential; the compiler resolves the cycle as a deferred edge in the structural graph. Mutual recursion is rejected (`C005`).

A rule reference may be wrapped in `maybe`:

```
maybe $swap_step
```

### 6.8 Predicate clauses

`X where FN` — node `X` matches only if predicate `FN` returns true. The predicate is called with the candidate `Block` after opcode/btype checks pass. On false, the node does not match; the engine backtracks normally. This applies to **every** node, including the anchor.

```
op JettonInternalTransfer @out where pton_self_transfer
```

A node may carry at most one `where` clause. Predicates declared with `predicate FN` (§5.2) are the only valid host references.

**Inline field predicates `[M4]` — implemented on both paths `[M2R, T33]`:**

```
btype jetton_transfer @t where (.has_internal_transfer == false)
```

`where (.field == literal)` / `where (.field != literal)` tests a field of the candidate block's `data` against a literal (the expression form is always parenthesized, distinguishing it from the named-predicate form). Missing field evaluates the test to false (never an error at match time). This removes the need to register a host predicate for every trivial data check — each host predicate is a dual-implementation parity liability, so cheap checks belong in the language. Only equality/inequality against literals; anything richer is a host predicate.

`[M2R, T33]` Both engines evaluate the clause at match time through one shared sync evaluator (`mch/expr_eval.py`): the compiled path composes it onto `test_self` over the compile-time IR encoding of the expression, the IR engine evaluates the artifact's `where_expr` node field — same bytes, same evaluator, no drift. Anchor nodes carry it like any other node. Async constructs (lookups, host fns) inside the clause are compile-time errors (C013) and IR-load-time errors; opcode-set anchor branches accept only the named-predicate form (C013).

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

### 7.2 List bindings under recursion `[M2]`

When a capture appears inside a rule that self-references, the Match dataclass exposes it as `list[T]`. Elements are appended in unrolling order: each unrolling descends one step deeper along the chain, so lists follow the trace outward from the entry block (ascending `min_lt` of the step's head block on real traces). "The terminal element" is `list[-1]`.

**Alignment rule (T18; replaces the v0.2 "shorter list" wording, which itself replaced v0.1 §7.2/§14.2):**

The rule's top-level alternative splits into **step branches** (those containing the `$self` reference) and **exit branches** (those without it, §10.3).

- A capture inside a **step branch** produces **exactly one element per unrolling** — all of a rule's step captures are parallel-aligned lists of equal length. When an unrolling did not bind the capture (its alternative branch lost, or its `maybe` subtree was absent), that unrolling's element is `None`. Lists are never silently shorter: gaps are explicit `None`s, preserving per-unrolling correlation.
- A capture inside an **exit branch** binds **at most once**: when the recursion terminates through the pure exit, one element is appended — the bound block, or `None` if the capture's sub-branch within the exit lost. Exit captures receive no per-unrolling padding; if the recursion ends by frontier exhaustion instead (step path dead-ends without the exit firing) or never runs, they stay `[]`. A name captured in both a step and an exit branch (mutually exclusive positions, §6.2) accumulates its step elements first, then the exit element — `list[-1]` is still the terminal binding.
- A capture positioned **after** the recursion in matcher traversal order — a later item of the same children set, or the parent chain — is an ordinary scalar/`Optional` binding (§7.1/§7.3) and binds normally whether the recursion ran zero, one, or many unrollings. Only chaining further *sequence* atoms after a `$rule` in the same chain is rejected (`C007_CAPTURE_AFTER_RECURSION`, §10.3): the engine structurally ignores such atoms, so captures there would always lie.

Example: `rule swap_step = ( op SWAP @swap -> op PAY_TO @payout -> maybe op NOTIFY @note -> maybe $swap_step ^ op PAY_OUT @out ... )` matched against a three-hop chain produces `swap`, `payout` and `note` as length-3 parallel lists (`note` holding `None` for hops without a notification), and `out` as a one-element list bound on the terminating payout.

`[M2R, R11/T28]` This section applies to the frontier strategy only. Captures cannot appear inside a `cyclic` rule body at all (`C012`, §10.3) — matchers using cyclic rules drive their builders from `Match._mch_consumed`.

### 7.3 Optional bindings

Captures inside a `maybe`-wrapped subtree bind as `Optional[T]`. If the subtree did not match, the capture is `None`.

Captures inside a `maybe`-wrapped recursive rule remain `list[T]`; absence of the rule means the list is empty (`[]`), not `None`.

### 7.4 Capture scope

A capture defined inside a `rule` is visible to any matcher whose pattern uses `$rule`.

## 8. Matcher directives

### 8.1 `produces`

Host-language class name(s) of the block type(s) this matcher can produce. Every named type must exist in the host's block-type registry.

- `produces X` — single type (M1).
- `produces X | Y | Z` `[M2]` — union: the build function decides which type to return based on content (the DNS case: one anchor opcode, three semantic outcomes). The compiler verifies the returned type is one of the declared ones only where the host can check cheaply; the declaration is primarily documentation + registry validation.
- `produces switch { … }` `[M4]` — fully declarative dispatch; see §9.5.

### 8.2 `build IDENT`

Host-language function name for the build callback. Receives a typed Match (§7) and returns the produced block(s). Host-specific call shape — see §11.

`[M4]` A matcher whose build statements fully specify the output (`out` block or `produces switch` with out blocks) may omit `build`. If both are present, the host `build` runs after the declarative statements and receives the pre-populated block — the escape-hatch composition mode.

### 8.3 `entry @CAPTURE`

Pins the runtime anchor (`test_self` equivalent) to the captured node. Currently required on every matcher (`R008`); automatic anchor selection by corpus selectivity is a deferred extension (§16).

Constraints:
- The referenced capture must exist in the pattern.
- The referenced node must have a concrete opcode or btype (`op`, `btype`).
- `[M2]` The capture may sit on an XOR alternative whose every branch is a concrete `op`/`btype` node carrying the same capture name (e.g. `(op JettonMint @m ^ op MinterJettonMint @m)`). The anchor test compiles to an opcode/btype **set** membership check. This is required for protocols whose anchor legitimately spans several opcodes (jetton_mint).
- `[M2R, R09]` The branches of such an XOR may **mix `op` and `btype` heads** (the anchor test is union membership over the opcode set ∪ the btype set), and each branch may carry a `where <pred>` clause. The anchor tests union membership first, then the matching branch's `where` predicate (a head match whose predicate fails falls through to any later branch sharing the head; no membership match ⇒ no anchor). This expresses legacy anchors shaped `opcode == X or block is TonTransfer` (e.g. `MultisigApproveBlockMatcher`, the EVAA supply/withdraw/liquidate matchers). Per-branch `where` is only available under `^` (or a heuristic-exclusive `|` of distinct bare heads); a `where` on a plain `|` branch makes the alternative non-exclusive and cannot anchor.
- `[M2R, R10]` The referenced node may have a `pred` head (`entry` on `pred foo @x`). There is no opcode/btype prefilter, so the engine full-scans `test_self` (= the predicate) over every candidate block. This is legal and measured-cheap: on the 1477-block corpus a representative pred anchor (`len(block.contract_deployments) == 1`, NftMint-style) costs 58.9 ns/block vs 54.6 ns/block for an opcode anchor — 1.08×, 4.3 ns/block extra — because the engine already runs one `test_self` per (block, matcher) pair regardless of anchor kind; a pred anchor only swaps an int compare for a property read (no extra scan). A pred anchor whose predicate does heavy per-block work (BOC parsing, lookups) is the predicate author's cost, same as any mid-pattern `pred` node.
- The node must not be inside a `maybe`-wrapped subtree.
- The node must not be inside a non-exclusive alternative branch.
- The node must not be inside a recursive rule (would produce a list binding, ambiguous).

### 8.4 `include_excess BOOL`

Default: `true`. Controls whether messages with the excess opcode (`ExcessMessage.opcode = 0xd53276db` or equivalent in host) emitted as siblings of matched blocks are auto-appended to the matched block set.

### 8.5 `include_bounces BOOL`

Default: `true`. Controls whether bounce messages (opcode `0xffffffff`) emitted as siblings of matched blocks are auto-appended to the matched block set.

### 8.6 `priority DEC` `[M3]`

Registration-order weight. Lower runs earlier. Matchers with equal priority are ordered by (file name, source position) — fully deterministic across hosts. Default priority is `100`.

Matcher order is semantics, not configuration: earlier matchers consume blocks first (the jetton_transfer → fallback_jetton_transfer relationship depends on it). v0.1 left ordering "host-specific", which is a parity hole; from M3 the order is derived entirely from the `.mch` namespace and is part of the compiled artifact.

Implementation notes: `build_matchers` returns the matchers stable-sorted by ascending `priority` (the artifact's `registration_order` is already file+source order, so a stable sort gives the `(priority, file name, source position)` total order). The IR matcher record carries a `"priority"` key **only when non-default** — an absent key means `100` — so specs that never set priority emit byte-identical artifacts. Declaring the same non-default priority on two matchers in a file warns (`W010_DUPLICATE_PRIORITY`): their order then depends on source position, usually not intended. Redeclaring `priority` on one matcher is an error (`P011`).

### 8.7 `shape IDENT` `[M2R, R06]`

Post-build tree surgery in a registered host function. Optional; at most one per matcher (a duplicate is a parse error, `P011`). The named shaper must exist in the host's shaper registry (§11.1), otherwise the load fails (`C011_UNKNOWN_SHAPER`).

```
matcher getgems_nft_purchase {
    produces NftPurchaseBlock
    build    build_getgems_nft_purchase
    shape    getgems_proxy_insert
    entry    @t
    ...
}
```

The engine invokes the shaper as the **last step of the build phase** — after the produced block's merge (and, on declarative matchers, after flow effects) — on both the compiled path and the IR engine at the same point, so block-topology parity holds by construction. Shapers are synchronous:

```
shaper(produced_block, match) -> None
```

`match` is the same match environment the build phase received (the typed Match for builder matchers; the capture env including the well-known `consumed`/`anchor` names for declarative matchers).

**Region contract (normative).** A shaper may mutate only the *produced block's region*: the produced block's own `next_blocks` / `children_blocks` and the blocks it consumed (e.g. inserting an `EmptyBlock` proxy between two consumed blocks via `insert_between`). It must never mutate upstream siblings or any block outside the region, and it must not un-consume matched blocks. `merge_blocks` is already licensed build-phase tree mutation; `shape` is the same class of mutation, kept host-side but *declared* in the spec so both engines can run it at the identical point. Motivating legacy sites: `GetgemsNftPurchaseBlockMatcher` (`blocks/nft.py`) and `StonfiV2ProvideLiquidityMatcher` (`blocks/liquidity.py`), both `EmptyBlock()` + `insert_between` proxy insertion.

## 9. Build statements `[M4]`

Build statements move the mechanical majority of `build_block` bodies into the spec. Analysis of the production matchers (jettons, dns, stonfi v1/v2, dedust swap/liquidity, tonco, coffee) shows build bodies decompose into: re-navigation of the matched tree (eliminated by captures), BOC body parsing, interface-repository lookups, field mapping into `data`, success/failure calculation, and a ~20% residue of genuine corner cases. The first five are declarative here; the residue stays in host `fn`s.

Build statements execute after the structural match succeeds, in source order, in three phases (§9.7).

### 9.1 `parse CAPTURE as MessageType`

Parses the captured block's message body with a registered message parser (the classes in `blocks/messages/`). The parsed value is available in expressions as `CAPTURE.body`.

```
parse transfer as JettonTransfer
parse internal as JettonInternalTransfer
```

- The message type must exist in the host's message-type registry (`B001_UNKNOWN_MESSAGE_TYPE`).
- Parsing a `maybe` capture that is `None` yields `null` for `CAPTURE.body`; expressions must handle it (`??` or `reject when`).
- A parse failure (malformed body) rejects the match (§10.7) — identical to a builder throwing today.
- For a list capture, `CAPTURE.body` is the parallel list of parsed bodies.

### 9.2 `let NAME = expr`

Binds a local name. `let` is also the only place `lookup` may appear:

```
let wallet = lookup jetton_wallet(internal.msg.destination)
```

`lookup KIND(key_expr)` queries the interface repository (`jetton_wallet`, `nft_item`, `dedust_pool`, … — the set is the host's interface-repository surface, mirrored in a registry). Result is a record or `null`.

### 9.3 `reject when expr`

If the expression is true, the match is rejected: the matcher behaves as if the structural match failed, and the engine continues with other matchers (§10.7). This is the sanctioned home for async-data disambiguation (e.g. pton-master checks) that hand-written matchers today perform inside `build_block` by returning `[]`.

```
reject when wallet == null
reject when wallet.jetton not_in pton_masters   # via host fn if set-membership needed
```

### 9.4 `failed when expr` / `broken when expr`

Set the produced block's `failed` / `broken` flags. May appear multiple times; results OR together. Nearly every DEX matcher computes these from exit codes — first-class support removes a whole class of host code.

```
failed when transfer.failed or internal.failed
failed when payout.body.exit_code != 0xc64370e5
```

### 9.5 `out { … }` and `produces switch`

`out { field: expr, … }` populates the produced block's `data` mapping declaratively.

```
out {
    sender:          transfer.msg.source
    sender_wallet:   transfer.msg.destination
    receiver:        wallet.owner
    amount:          internal.body.amount ?? transfer.body.amount
    asset:           jetton(wallet.jetton)
    query_id:        transfer.body.query_id
}
```

`produces switch` dispatches the produced block type on content, each branch with its own `out`:

```
produces switch {
    when msg.body.has_value       => change_dns  out { key: msg.body.key, value: msg.body.value }
    when msg.body.key == ZERO_KEY => renew_dns   out { }
    else                          => delete_dns  out { key: msg.body.key }
}
```

Branches are tested in order; first true branch wins, selecting both the produced btype and its `out` fields. `else` is required (its condition is absent; exhaustiveness is not proven — `R021`), must be unique and last (`R019`), and every branch btype must appear in the matcher's `produces` union (`R020`). A matcher uses **either** a plain `out` **or** a `produces switch`, never both (`R017`). At runtime a switch selects the produced block's btype per the winning branch; a non-switch matcher uses its single declared `produces` type.

**Optional keys.** A field written `name?: expr` is omitted from the produced `data` map when its value evaluates to null (a present-vs-absent key, distinct from a present key whose value is null). This is the sanctioned way to reproduce a legacy conditional `data[key] = …`:

```
out {
    source:               account(dns.msg.source)
    collection_address?:  account(nft.collection_address)   # key omitted when the lookup missed
}
```

Plain (non-`?`) fields always emit their key. C++: skip the map insert when the value is null.

### 9.6 Expression language

Deliberately small, **pure and total**. Features: capture/local references, field access (`x.y`), the implicit accessors `CAPTURE.msg` (message envelope: `source`, `destination`, `value`, `opcode`, `exit_code`), `CAPTURE.body` (parsed body after `parse`), and `CAPTURE.data` (a composed block's produced data), literals, comparisons, `and`/`or`/`not`, `?? ` default, `x if cond else y`, and a fixed builtin set:

`CAPTURE.msg.exit_code` is a **defined** envelope field: the compute-phase exit code of the message's transaction (null when the transaction records none). It is the only non-topology way to reach transaction state and exists specifically so success/rejection can gate on the exit code without a host `fn`; it is spec'd here rather than reached by generic attribute chaining (the C++ envelope has a fixed field set).

- `CAPTURE.data.<field>` — reads a field of a captured block's produced `data` (the block a *prior* matcher built, e.g. `transfer.data.sender` where `transfer` is a composed `jetton_transfer`). Access is a plain map lookup; a missing field rejects the match (mirrors the host's `data[key]` KeyError). Host semantics: dict lookup first, attribute fallback (dataclass interop during the migration); the C++ engine's block data is a `map<string, Value>`, so the accessor is a pure map lookup there.
- Constructors: `asset(master)`, `ton_asset()`, `account(addr)`, `amount(x)` — the `Asset`/`AccountId`/`Amount` value objects. `account(null)` short-circuits to null (a *missing* address); `addr_none()` builds the distinct **addr_none** `AccountId` (present but empty — repr `addr_none`, `as_str()` → null). A field that must carry a present-but-empty address uses `account(x) ?? addr_none()`, mirroring the host's `AccountId(None)`.
- Encoding: `b64(x)` base64-encodes `x`; a raw `Cell` is BOC-encoded first (mirrors `cell.to_boc()`), so a field carrying either boc bytes or a parsed Cell serialises identically.
- Composite literals: a **list literal** `[expr, ...]` (evaluates to an ordered list) and a **record literal** `{name: expr, ...}` (evaluates to a string-keyed map). Both nest freely and are the way to build nested composite output fields (legs, packet data) without a host `fn`. Trailing commas and newlines are allowed. IR encoding: `{"k":"list","items":[…]}` and `{"k":"record","fields":[{"name":…,"expr":…}]}`. C++: `vector` / `map` construction.
- List combinators over list captures / list literals: `first(xs)`, `last(xs)`, `len(xs)`, `sum(xs)`, `zip(xs, ys)`, `map(xs, "field")` (field projection only, no lambdas). Fixed, total edge semantics (each is a one-line loop in C++): a **null** list argument yields null (the call short-circuit); on a real list — `first`/`last` of an **empty** list is null; `len` is the element count (empty → 0); `sum` coerces each element to an integer (`Amount` → its value, `int` → itself, anything else is a fault) and returns an integer (empty → 0); `zip` truncates to the shorter input and yields 2-element lists; `map(xs, "field")` projects one field per element — the field is a **string literal** (dict key lookup, attribute fallback; a missing field is a fault). A missing field or non-numeric `sum` element rejects the match (spec §10.7). `map` takes a quoted field name, not a bare identifier — a bare name is ambiguous with a value reference and not C++-clean; there is no map-with-constructor form (that stays a host `fn`).
- Declared host functions: `fn` names, called like builtins.

No user-defined loops, no recursion, no mutation, no string manipulation. If a computation does not fit, it belongs in a host `fn`. The grammar is bytecode-friendly by design: every expression compiles to a short stack program for the M5 IR (§11.4), and every host implements the same ~20-op VM with shared conformance vectors.

### 9.7 Execution phases and lookup batching

Build statements run in three engine-driven phases:

1. **Collect** — after the structural match, evaluate all `lookup` key expressions (these may reference only captures and `parse` results, not other `let`s that themselves contain lookups — enforced at compile time, `B004_LOOKUP_DEPENDS_ON_LOOKUP`).
2. **Fetch** — the engine batch-fetches all lookups at once (single async round in Python; single actor request batch in C++).
3. **Evaluate** — all remaining statements run synchronously and deterministically on the fetched results.

This removes async interleaving as a source of cross-host divergence and improves throughput (one await instead of N).

### 9.8 `flow KIND(args)`

Records value-flow effects on the produced block (mirrors `AccountValueFlow`):

```
flow jetton(owner_expr, master_expr, amount_expr)
flow ton(account_expr, amount_expr)
flow fees(account_expr, amount_expr)
```

### 9.9 `omit @CAPTURE when expr` — **DROPPED (not implemented)**

Originally proposed to exclude a captured subtree from the consumed set when a condition held. **Cut from the language:** the one motivating case (stonfi v1 excluding the referral-payout subtree for certain exit codes) was resolved the other way by R12 — predicate-gating at *match* time (an inline `where`/predicate on the subtree node) keeps the blocks out of the match to begin with, so there is no post-match consumption surgery to sanction. With zero users and a match-time replacement, `omit` is not part of the grammar, the IR, or any engine. Do not use it; gate consumption in the pattern instead.

### 9.10 `fn IDENT(params)`

Declares a host function callable from expressions. The host registers the implementation; the compiler checks the name and arity at load time (`B002_UNKNOWN_FN`, `B003_FN_ARITY`). Host fns are synchronous and pure over their inputs (they receive already-fetched lookup results, never the repository itself).

```
fn stonfi_referral_split(payouts)     # corner case: referral vs out disambiguation
```

Every `fn` is a dual-implementation parity liability. The roadmap tracks the count; a growing count is a signal the expression language is missing a feature (or the matcher is fighting the domain).

## 10. Compilation

### 10.1 Lowering target

Each matcher compiles to one instance of the host engine's matcher type. In Python (today), this is a dynamically synthesized `BlockMatcher` subclass composed from the engine primitives (`ContractMatcher`, `BlockTypeMatcher`, `OrMatcher`, `ExclusiveOrMatcher`, `RecursiveMatcher`, `GenericMatcher`). From M5, lowering targets the neutral IR (§11.4) and each engine interprets it.

Lowering steps:

1. Parse `.mch` files into an AST.
2. Resolve `opcode` and `predicate` references across the namespace.
3. Resolve `$rule` references; flatten linear `->` and `<-` chains; lower `maybe` into `optional=true` on the relevant engine constructs; apply attach-to-left for children blocks (§6.5).
4. Validate the `entry` anchor (§10.2).
5. Build the dependency graph; detect cycles (allowed for direct self-recursion only).
6. Emit the compiled matcher and the typed Match dataclass.
7. Register in the engine's matcher list in priority order (§8.6).

### 10.2 Anchor

The `entry @CAPTURE` directive is required (`R008_ENTRY_REQUIRED`). The anchor's structural position determines whether the runtime walker traverses `parent_matcher` first, `child_matcher` first, or `children_matchers` — derived mechanically from the AST.

Automatic anchor selection from corpus selectivity statistics (v0.1 §10.2) is deferred to a future extension (§16); mandatory `entry` has proven acceptable in practice and keeps host behaviour trivially identical.

### 10.3 Recursion resolution

`[M2R, R11/T28]` A self-referential rule compiles under one of two **recursion strategies**, selected per rule (§5.3). Indirect (mutual) recursion is rejected (`C005_INDIRECT_RECURSION_UNSUPPORTED`) under both.

**Frontier strategy (default).** The rule's top-level alternative splits into step branches (containing `$self`) and exit branches (without it); trailing `maybe $self` is stripped from step branches and the engine runs the T18 frontier walk: at each frontier block the exit is tried FIRST, then one step per round; the stepped blocks' children become the new frontier. A `maybe $rule` is the natural termination case — a cycle with no `maybe`-wrapped termination is an error (`R006_UNBOUNDED_RECURSION`). A recursive rule whose step branch has no concrete head is rejected (`C001_RECURSION_SHAPE_UNSUPPORTED`). A capture on a sequence atom chained after a recursive rule reference in the same chain is rejected (`C007_CAPTURE_AFTER_RECURSION`) — the engine structurally ignores such atoms; captures in *other* positions after the recursion (later children-set items, the parent chain) are legal and bind normally (§7.2).

**Cyclic strategy (`rule NAME cyclic = …`).** The rule body lowers ONCE to a matcher subgraph whose in-body self-references (which must be the last atom of their chain) become true back-edges to the body root; matching is plain recursive descent through the cycle, bounded by trace depth. There is no step/exit split and no frontier: a branch whose self-reference is *mandatory* only matches when the chain actually continues — this expresses "hop continuation is mandatory" patterns whose continuation and final branches share a prefix, which the frontier walk's exit-first order would terminate prematurely (the T28 tonco shape). Termination is a non-recursive alternative branch inside the body (`R006` does not apply; `maybe $self` inside the body is in fact rejected — an IR back-edge carries no per-site optionality). Shape violations are `C012_CYCLIC_RULE_SHAPE`: captures anywhere in the body (they can never bind — builders must be consumed-set driven via `Match._mch_consumed`), a self-reference that is not its chain's last atom, a `maybe`-wrapped in-body self-reference, or a reference to another recursive rule. Outer references (`$rule` outside the body) may be plain or `maybe`-wrapped; each reference site is independent. In the IR a cyclic rule reference is `{"kind": "recursive", "strategy": "cyclic", "step": <body root>}` with no `exit`, and in-body self-references are plain node back-edges — cycles in the node graph are legal exactly there.

Conformance: `tests/mch/test_cyclic_recursion.py`; first production use: `specs/tonco_swap.mch`.

### 10.4 Backtracking semantics

The engine matches greedily within a single node head but backtracks across alternatives and optional skips. Captures bound during a failed branch are discarded; only captures from the eventually-successful branch persist in the final Match value.

### 10.5 Determinism (normative) `[M3]`

Wherever the engine chooses among several candidate blocks, the candidate order is **ascending `min_lt`, ties broken by tree-construction order** (the stable order of `next_blocks`). This applies to `child_matcher` scanning, children-set matching (§6.5), and recursion frontier expansion. Two conforming hosts presented with the same trace must consume identical block sets. v0.1 left this implicit; it is now part of the language semantics, not an engine detail.

### 10.6 Matcher ordering and conflicts `[M3]`

When multiple matchers could match the same anchor block, they are attempted in the deterministic priority order of §8.6. The first matcher whose pattern fully succeeds **and whose build phase does not reject** consumes the blocks. Subsequent matchers see the already-consumed blocks as upstream-produced and may skip or fail their own attempts.

### 10.7 Build rejection and error contract `[M3]`

Normative contract (v0.1 was silent; hand-written matchers already rely on this):

- A build function may **reject** by returning an empty result. Rejection ≡ structural-match failure: no blocks are consumed, and the engine proceeds to the next matcher in order.
- An **exception** escaping a build function (or `[M4]` a parse failure / expression runtime error) is also a rejection, additionally reported through the host's diagnostics channel. It must never abort trace processing.
- Consumption is exactly: the pattern-matched blocks, minus `omit`-excluded subtrees `[M4]`, plus auto-appended excess/bounce siblings (§8.4–8.5). Builders must not otherwise edit the consumed set.

**Clean rejection, normative** `[M3, T22/T29]`: a build rejection — whether the builder returns empty, the builder raises, or a declarative `reject when` fires — consumes **nothing** and appends **nothing**: the excess/bounce auto-append of the previous bullet applies only to a *successful* build. The anchor and every candidate block are left exactly as found, so a later matcher in registration order may attempt (and consume) the same anchor. The Python IR engine (`indexer/events/mch_ir/engine.py`) implements this rule for all three rejection paths and is the reference implementation. The compiled path (`indexer/events/mch/compiler.py` via `blocks/basic_matchers.py`) has a known deviation: its base `try_build` auto-appends the anchor's excess/bounce children even when the builder rejects. This is not fixed on the compiled path — it is demoted by T29 stage 1 and removed with the compiler in T29 stage 2.

## 11. Host-language contract

A host language provides these registries, populated at startup:

| Registry | Keyed by | Value type | Since |
|---|---|---|---|
| Opcodes | identifier from `opcode IDENT = …` | integer | M1 |
| Block types | identifier from `matcher.produces` | class / type tag | M1 |
| Predicates | identifier from `predicate IDENT` | callable `(Block) → bool` | M1 |
| Builders | identifier from `matcher.build` | callable `(Match) → Block` | M1 |
| Message types | identifier from `parse … as IDENT` | body parser class | M4 |
| Lookups | identifier from `lookup IDENT(…)` | interface-repository accessor | M4 |
| Functions | identifier from `fn IDENT(…)` | pure callable | M4 |
| Shapers | identifier from `matcher.shape` | sync callable `(produced_block, match) → None` (§8.7) | M2R, R06 |

The compiler validates at load time that every reference resolves; otherwise the load fails with a precise error citing the unresolved name and its source location.

### 11.1 Python contract

- Builders are `async` functions:
  ```python
  async def build_jetton_transfer(m: JettonTransferMatch) -> JettonTransferBlock: ...
  ```
- Predicates are sync functions:
  ```python
  def pton_self_transfer(block: Block) -> bool: ...
  ```
- Registration is via the `Registries` object (`register_builder("name")`, `register_predicate("name")` decorators/methods).
- `[M2, T15/T18]` The runtime Match carries a well-known attribute `_mch_consumed`: the full consumed block set — anchor first, engine traversal order, **de-duplicated** (order-preserving, first occurrence wins; the raw engine list reports a recursion's entry block twice, and the synthesized `build_block` strips repeats before handing the list to the builder). Builders whose logic scans the consumed set (legacy `build_block(block, other_blocks)` bodies doing `find_messages`/`find_call_contracts`) use it; since T18 exit-branch and post-recursion captures bind too (§7.2), so the consumed set is an escape hatch rather than the only access path. Purely additive — builders that ignore it are unaffected.
- Post-process passes (e.g. `post_process_dedust_liquidity`) stay plain host functions outside the registry system (§5.5).
- `[M2R, R06]` Shapers register via `register_shaper("name")` and are plain **sync** functions `def shaper(produced_block, match) -> None`. Both engines invoke them at the end of the build phase, after merge; the region contract is §8.7. Unknown shaper name = `C011_UNKNOWN_SHAPER` at resolve/compile time (compiled path) or `IrLoadError` at load time (`mch_ir`).
- `[M2R, R03]` **ContractDeploy side-effect blocks.** `CallContractBlock.__init__` auto-attaches a `ContractDeploy` block to the call's `children_blocks` (NOT `next_blocks`) when its transaction deploys the account. The engine matches over `next_blocks` only, so no pattern can consume such a block. Builders whose legacy counterpart merges deploy blocks as direct children (topology parity) must sweep them from the consumed set's `children_blocks` and merge them before returning; the engine's own merge composes cleanly after. `unwind_deployments` surfaces them as separate `contract_deploy` actions either way. Recurs for any matched chain containing a deploying transaction.
- Generated Match dataclasses are emitted by codegen next to the spec compiler's output module; the runtime Match object and the emitted dataclass must agree (single source of truth for field names and types).

### 11.2 C++ contract `[M5]`

- The C++ engine consumes the compiled IR artifact (§11.4); it never parses `.mch`.
- Match types are generated headers emitted by the (single, shared) frontend's C++ backend.
- Builders registered via `register_builder("name", &fn)`; predicates `bool fn(Block const&)`.
- Lookups map to batched actor requests; the three-phase build execution (§9.7) makes this a single request round per match.

### 11.3 Cross-language invariants

For a given `.mch` namespace and a given trace:
- The set of matchers that fire is identical in both languages.
- The set of blocks each matcher consumes is identical.
- The captures bound for each fired matcher are identical (modulo trivial type differences in block field representations).

The implementation must include a cross-language fidelity test harness that runs the same trace corpus through both engines and asserts equivalent matched-block trees.

### 11.4 Compilation artifact strategy `[M5]`

One frontend, not two. The Python frontend (lexer → parser → resolver → lowering) is the single implementation of the language; it emits a **versioned, language-neutral IR artifact** containing:

- the matcher table in priority order: anchor opcode/btype sets, structural constraint graphs, capture slots;
- parse bindings (message-type references) and lookup plans;
- expression bytecode (small stack VM, ~20 opcodes, shared conformance test vectors);
- host-fn signatures.

Each engine (Python, C++) ships only a thin loader + interpreter for this IR. Consequences:

- Parity surface shrinks from "two compilers agree on a grammar" to "two small VMs agree on an instruction set" — testable with golden vectors.
- Diagnostics (§13) are implemented once.
- The C++ worker loads the artifact once at startup (or embeds it at build time). Hot reload is explicitly out of scope — no reload, versioning, or invalidation machinery (user decision 2026-07-05).
- Golden IR fixtures make compiler regressions diffable.

Until M5, the Python compiler may keep lowering directly to engine objects; the IR is introduced as the artifact boundary when the C++ engine work starts. Design decisions from M4 onward (especially the expression language) must stay IR-friendly.

## 12. Validation rules

Milestone tags mark when a rule becomes active (untagged = M1, active now).

### 12.1 Errors (abort compilation)

- An `opcode IDENT = V` declaration's value disagrees with the host registry.
- An `op IDENT` references an undeclared opcode name.
- A `pred IDENT` or `where IDENT` references a predicate not in the registry.
- A `$rule` references a rule with no definition.
- A `matcher` lacks `produces`, or lacks both `build` and a complete declarative output.
- A `matcher.entry @X` references a capture not defined in the pattern.
- An `entry` capture violates §8.3 constraints (inside `maybe`, non-exclusive alternative, or recursive rule).
- A capture name is defined multiple times in incompatible positions (§6.2) (`R005`).
- A matcher's `produces` type is not in the block-type registry (`C003_UNKNOWN_BLOCK_TYPE`).
- A matcher's `build` function is not in the builder registry (`C002_UNKNOWN_BUILDER`).
- A children block contains an empty item (`P002`).
- A children block appears without a left sibling (`C008_CHILDREN_BLOCK_BARE`).
- A parent edge follows a chain-advancing (non-children-block) child-edge atom in the same chain, i.e. the parent edge's left side is a non-anchor node reached through a child edge (`A -> B <- C`) (`C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED`). `A -> {B; C} <- D` is legal — children blocks do not advance the cursor (§6.4/§6.5).
- An atom sits **upstream** of the anchor in the anchor's enclosing sequence, reached by a child edge into the anchor (`… -> op Anchor`, e.g. the `$rule -> anchor…` head shape) (`C010_UPSTREAM_ATOMS_UNSUPPORTED`). Only downstream (child/children) and parent-chain atoms are lowered relative to the anchor; upstream atoms would be silently dropped. Re-anchor the pattern so the anchor is the chain head, or restructure. (A flat `… <- op X -> {CB} <- Y` parent chain is **not** this case — the trailing parent edge after a children block lowers correctly, §6.5.)
- A cycle exists in rule references with no `maybe`-wrapped termination.
- Indirect (mutual) recursion (`C005_INDIRECT_RECURSION_UNSUPPORTED`).
- Unsupported recursion shape (`C001_RECURSION_SHAPE_UNSUPPORTED`).
- A capture on a sequence atom chained after a recursive rule reference in the same chain (`C007_CAPTURE_AFTER_RECURSION`); later children-set items and parent-chain captures are legal (§7.2/§10.3).
- Mixing `|` and `^` at the same nesting level (`P010_ALTERNATIVE_SEPARATOR_MIX`).
- Matcher lacks `entry` (`R008_ENTRY_REQUIRED`).
- `stitch` keyword encountered (`P012_STITCH_REMOVED` — hint: paired results are merged by host post-process code).
- `[M4]` `parse … as T` where `T` is not in the message-type registry (`B001_UNKNOWN_MESSAGE_TYPE`).
- `[M4]` Unknown `fn` (`B002_UNKNOWN_FN`) or wrong arity (`B003_FN_ARITY`).
- `[M4]` A `lookup` key expression depends on another lookup (`B004_LOOKUP_DEPENDS_ON_LOOKUP`).
- `[M4]` An `out` field or expression references an undefined name.
- `[M4]` `produces switch` without `else` where exhaustiveness is unprovable.

### 12.2 Warnings (non-fatal)

- Two branches of an explicit `^` alternative share the same concrete head (`W202_EXCLUSIVE_OR_REDUNDANT_HEADS`).
- A rule is declared but never referenced.
- A capture is declared but unused (by the builder where introspectable, or `[M4]` by any build statement).
- An opcode constant is declared but never referenced.
- A matcher overrides `include_excess` / `include_bounces` to a non-default value (informational).
- `[M3]` Two matchers share identical priority and anchor head (informational; order still deterministic via §8.6 tie-break).
- `[M4]` A declared `fn` is never called.

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

### 14.1 Jetton transfer (simple linear — M1)

```
opcode JettonTransfer         = 0x0f8a7ea5
opcode JettonInternalTransfer = 0x178d4519
opcode JettonNotify           = 0x7362d09c

matcher jetton_transfer {
    produces JettonTransferBlock
    build    build_jetton_transfer
    entry    @transfer

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

### 14.2 Stonfi V2 swap (recursion + bidirectional + predicate — M2)

> **Known-idealized (T16, 2026-07-03).** This example does not lower as written: `entry @swap` sits inside the recursive rule (R007 — list binding), and the `$in_transfer -> $swap_step` head shape puts atoms upstream of the anchor, which the current compiler silently drops. The normative, parity-proven pattern is `indexer/events/mch/specs/stonfi_v2_swap.mch` (first hop re-anchored into the matcher pattern, hops 2..n via `rule swap_hop`; data checks as registered host predicates until M4 inline predicates). This example is kept as the *intent* sketch; reconcile it when M4 lands. See roadmap T16 notes for the full deviation list.

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
    swap:         list[CallContractBlock]  # one per hop (unconditional step path)
    payout:       list[CallContractBlock]  # parallel to swap
    out_transfer: list[Block]              # parallel to swap: the hop's transfer, or
                                           # None for hops whose winning branch bound
                                           # none (§7.2 gap alignment);
                                           # terminal transfer = out_transfer[-1]
```

(§7.2: `swap`/`payout`/`out_transfer` are parallel-aligned, one element per hop; `out_transfer` carries explicit `None`s for hops where the bare-`$swap_step` branch won.)

### 14.3 Dedust deposit (two matchers + host-side merge — M2)

> **Known-idealized (port wave, 2026-07-04).** This example does not match the deployed protocol as written: several opcode constants are wrong (real `DedustDepositLiquidityToPool = 0xB56B9598`, `DedustReturnExcessFromVault = 0x6B0B787F`, …) and the rejection/destroy subtree shapes differ. Additionally, the parity-proven port registers AFTER the jetton_transfer matcher (the legacy pair runs before it), so deposit legs and vault excesses match as prebuilt `btype jetton_transfer` with a raw notify-chain fallback arm. The normative pattern is `indexer/events/mch/specs/dedust_deposit.mch`. Kept as the intent sketch, like §14.2.

Deposit requires two assets sent to a deposit contract along two branches of the same trace. The DSL declares two matchers; when both fire within one trace, the host post-process pass (`post_process_dedust_liquidity`, keyed on `deposit_contract`) merges the partial block into the final one — see §5.5. There is no cross-trace pairing.

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

```

An unpaired `dedust_deposit_partial` block (only one asset deposited in this trace) survives as its own action — the host-side merge is opportunistic, never required. The mch matchers must produce the same `DedustDepositLiquidityPartial` / `DedustDepositLiquidity` block classes the post-process pass dispatches on.

### 14.4 Jetton transfer with declarative build (M4)

Same pattern as 14.1, build moved into the language; no host builder needed:

```
matcher jetton_transfer {
    produces jetton_transfer
    entry    @transfer

    op JettonTransfer @transfer
      -> op JettonInternalTransfer @internal
      -> maybe op JettonNotify @notify

    parse transfer as JettonTransfer
    parse internal as JettonInternalTransfer

    let wallet = lookup jetton_wallet(internal.msg.destination)
    reject when wallet == null

    broken when wallet.owner != transfer.body.destination
    failed when transfer.failed or internal.failed

    out {
        has_internal_transfer: true
        sender:                account(transfer.msg.source)
        sender_wallet:         account(transfer.msg.destination)
        receiver:              account(wallet.owner)
        receiver_wallet:       account(internal.msg.destination)
        response_address:      account(transfer.body.response)
        amount:                amount(internal.body.amount ?? transfer.body.amount)
        forward_amount:        amount(internal.body.forward_ton_amount ?? transfer.body.forward_amount)
        desired_amount:        amount(transfer.body.amount)
        query_id:              transfer.body.query_id
        asset:                 jetton(wallet.jetton)
        comment:               transfer.body.comment
        encrypted_comment:     transfer.body.encrypted_comment
        forward_payload:       transfer.body.forward_payload
        custom_payload:        transfer.body.custom_payload
        payload_opcode:        transfer.body.payload_sum_type
    }
}
```

### 14.5 DNS record change with content dispatch (M4)

One anchor opcode, three semantic outcomes — impossible to express with a single `produces` type:

```
opcode ChangeDnsRecord = 0x4eb1f0f9

matcher change_dns_record {
    entry @msg

    op ChangeDnsRecord @msg -> maybe op 0xffffffff
    include_excess false

    parse msg as ChangeDnsRecordMessage
    let item = lookup nft_item(msg.msg.destination)

    produces switch {
        when msg.body.has_value =>
            change_dns out {
                source:      account(msg.msg.source)
                destination: account(msg.msg.destination)
                key:         msg.body.key
                value:       msg.body.value
                collection_address: account(item.collection_address) if item != null else null
            }
        when msg.body.key == ZERO_KEY =>
            renew_dns out {
                source:      account(msg.msg.source)
                destination: account(msg.msg.destination)
                collection_address: account(item.collection_address) if item != null else null
            }
        else =>
            delete_dns out {
                source:      account(msg.msg.source)
                destination: account(msg.msg.destination)
                key:         msg.body.key
                collection_address: account(item.collection_address) if item != null else null
            }
    }

    failed when msg.failed
}
```

(`ZERO_KEY` is a host-registered constant; constants share the opcode registry mechanism.)

## 15. Conventions

- One protocol per file
- Opcode declarations at the top of the file
- Predicates (and `[M4]` fns) declared next
- Shared rules before the matchers that use them
- Matchers ordered most-specific first for readability; actual order is governed by `priority` (§8.6)

## 15b. Deliberately excluded from the language (design decisions)

Recorded so a future reader does not re-litigate these. Each was demanded by a real matcher and resolved WITHOUT a language feature:

- **String / byte manipulation (slice, `lower`, `hex`, endian conversion) — CUT.** Address-tail comparisons (layerzero `dst_oapp`) use `account(a) == account(b)` (the constructor normalizes); protocol-specific byte formatting (tgbtc `bitcoin_txid` little-endian hex) stays a host `fn`. No general string ops in the expression language.
- **Block topology / aggregate navigation (`previous_block`, `next_blocks`, `event_nodes`, `.value`) — NOT a language feature.** Reach a neighbouring block by capturing it in the PATTERN and reading its envelope / `.data`; aggregate a value with `sum(...)` over a captured list. Transaction state is reachable only through the defined `msg.exit_code` envelope field (§9.6), never generic attribute chaining.
- **Multi-return lookups / set-membership — CUT.** A lookup is single-shape (name + key → record | null). Return several values as ONE record and read its fields (`r.staking_pool`); test membership with a targeted nullable lookup + `reject when x == null` (e.g. `lookup nominator_pool(addr)`), not an `in` operator. These are host-registration patterns, not language gaps.
- **`omit` (consumption surgery) — DROPPED** (§9.9): match-time predicate gating replaces it.

## 16. Future extensions (non-normative)

Listed only so implementers do not paint themselves into a corner. NOT scheduled.

- Automatic anchor selection from corpus selectivity statistics (`opcode_freq.json`) — v0.1 §10.2, deferred
- Quantifiers on children-set items: `one_or_more`, `zero_or_more`
- A `using` import system for cross-file rule references with namespacing
- `pre_build_auto_append` finer-grained control over excess/bounce timing
- Pattern fragments parameterised by capture (`rule X(@a) = …`)
- Set-membership literals in inline predicates (`where .x in {A, B}`)

## 17. Glossary

**Anchor / `test_self`**: The block whose opcode/btype the engine checks first when scanning a trace. Determines whether the matcher attempts to match at all for a given block.

**Block**: A node in the matched tree. Either a primitive (`CallContractBlock`, `TonTransferBlock`, `TickTockBlock`) or a typed protocol block produced by an upstream matcher.

**Build statement `[M4]`**: A declarative post-match statement (`parse`, `let`, `out`, …) that constructs the produced block inside the language.

**Capture**: A named binding from a position in the pattern to a matched block. Exposed to the builder as a typed Match dataclass field.

**Children block**: An ordered set of patterns matched against a parent's direct children. Positional, greedy, first-match-wins in canonical candidate order (§10.5).

**IR `[M5]`**: The language-neutral compiled artifact consumed by every host engine.

**Match**: The typed object passed to a builder, containing all captures.

**Rule**: A named pattern fragment, referenced via `$IDENT`. Supports direct self-recursion and reuse.
