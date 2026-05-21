# `.mch` Matcher DSL — Python v0 Design

**Date:** 2026-05-19
**Status:** Draft (approved through brainstorming)
**Related:** `docs/mch-spec.md` (DSL spec v0.1)
**Author / Owner:** TBD

## 1. Goal

Implement a Python compiler for the `.mch` matcher DSL defined in `docs/mch-spec.md`. The compiler reads `.mch` source, validates it, and produces `BlockMatcher` instances that plug into the existing event-classifier engine — no engine changes required for v0.

This is a *standalone module*. It does not modify the global `matchers` list in `indexer/events/event_processing.py` and does not replace any hand-written matcher. Production behavior of the classifier is unaffected by this work.

## 2. Scope

### 2.1 In scope

- Hand-written lexer + recursive-descent parser for the v0.1 grammar (§4 of `mch-spec.md`).
- Resolver: name binding (opcodes, predicates, rules), capture-type inference, recursion classification.
- Compiler lowering to existing engine classes: `ContractMatcher`, `BlockTypeMatcher`, `OrMatcher`, `ExclusiveOrMatcher`, `GenericMatcher`, `RecursiveMatcher`, and one synthetic subclass per matcher decl.
- Codegen: emit one `.py` Match dataclass file per matcher into a `generated/` subdirectory.
- Diagnostics with file:line:column carets (§13 of `mch-spec.md`).
- Worked examples: `jetton_transfer` (spec §14.1) and `stonfi_v2_swap` (spec §14.2).

### 2.2 Out of scope

- Stitch declarations (§5.5, §9, §14.3). Parser accepts and warns; resolver validates internal references; compiler emits a deferred warning and skips. No runtime side effects.
- Cross-language fidelity harness (§11.3). Python only.
- C++ contract (§11.2).
- Corpus-statistic anchor selection (§10.2). Replaced by an explicit-`entry`-required policy in v0.
- Replacement of any hand-written matcher in the production `matchers` list.
- Production wiring (config flags, engine registration).

### 2.3 Known deviations from `mch-spec.md`

1. **`entry @X` is required on every matcher in v0.** The spec allows it to be optional with selectivity-based fallback. v0 emits a hard error if absent. The §14.1 jetton_transfer example must add `entry @transfer` when used as a v0 test fixture.
2. **`opcode_freq.json` / `btype_freq.json` are not consulted.** Reserved diagnostic codes exist (`W201_NO_SELECTIVITY_STATS`) but are unused in v0.
3. **Stitch declarations are parsed but not lowered.** Compiler emits `W900_STITCH_DEFERRED` and ignores them.
4. **Recursion always lowers to `RecursiveMatcher`.** The compiler uses a pluggable `RecursionStrategy` so a future implementation (bounded unrolling, synthesized subclass per rule, etc.) can replace it without changing the compiler interface.

## 3. Non-goals

- Performance optimization. Compilation happens at startup; clarity wins over speed.
- Backward compatibility with any historical `.mch` syntax. v0.1 is the first version.
- A general-purpose parser combinator library. The recursive-descent code is purpose-built for this grammar.

## 4. Architecture

### 4.1 Pipeline

```
.mch text
   │
   ▼  lexer.tokenize(text, path)         → list[Token]
   │
   ▼  parser.parse(tokens)               → ast_.File           (raw AST)
   │
   ▼  resolver.resolve(file, registries) → ast_.ResolvedFile   (bindings + capture types)
   │
   ▼  compiler.compile(resolved, ...)    → list[CompiledMatcher]
   │
   ▼  codegen.emit(resolved, out_dir)    → generated/<name>_match.py per matcher
```

Each layer is a pure function: immutable input, single output, no globals. Errors are collected into a `DiagnosticBag` and raised as `CompileError(bag)` at end of phase (so a single run reports multiple errors when possible).

### 4.2 Module layout (`indexer/events/mch/`)

| Module | Responsibility |
|---|---|
| `tokens.py` | `Token`, `TokenKind`, `Span` |
| `lexer.py` | text → tokens; lexical errors with spans |
| `ast_.py` | frozen AST dataclasses |
| `parser.py` | tokens → `ast_.File`; parse errors |
| `registry.py` | `Registries` value object + `@predicate` / `@build` decorators |
| `resolver.py` | name binding, capture-type inference, recursion classification |
| `recursion.py` | `RecursionStrategy` protocol + `RecursiveMatcherStrategy` impl |
| `compiler.py` | `ResolvedFile` → `CompiledMatcher` list |
| `codegen.py` | `ResolvedFile` → generated `.py` files |
| `diagnostics.py` | `Diagnostic`, `DiagnosticBag`, caret formatter, `CompileError` |
| `compile.py` / `__init__.py` | `compile_text` / `compile_file` / `compile_dir` facades |

Tests live under `indexer/tests/mch/` (mirrors module layout) plus an `integration/` subdir for worked examples and `.mch` fixture files.

### 4.3 Pluggability

The single intentional extension point is `RecursionStrategy`. The compiler accepts an optional `recursion: RecursionStrategy = RecursiveMatcherStrategy()`. Future strategies (bounded unrolling, synthesized subclass per rule) implement the same protocol and drop in without compiler changes.

No other plug-in points are designed. Adding them prematurely is not warranted.

## 5. Key types and data flow

### 5.1 Tokens and spans

```python
class TokenKind(Enum):
    # literals
    IDENT, HEX, DEC, DURATION, STRING,
    # declaration keywords
    KW_OPCODE, KW_PREDICATE, KW_RULE, KW_MATCHER, KW_STITCH,
    # node-head keywords
    KW_OP, KW_BTYPE, KW_PRED, KW_ANY,
    # modifier / directive keywords
    KW_MAYBE, KW_WHERE, KW_PRODUCES, KW_BUILD, KW_ENTRY,
    KW_INCLUDE_EXCESS, KW_INCLUDE_BOUNCES,
    # stitch keywords (accepted but unused in v0)
    KW_PARTIAL, KW_FINAL, KW_KEY, KW_TTL, KW_MERGE,
    # boolean literals
    KW_TRUE, KW_FALSE,
    # punctuation
    ARROW_R, ARROW_L, PIPE, AT, DOLLAR,
    LBRACE, RBRACE, LPAREN, RPAREN, EQ, DOT, SEMI,
    NEWLINE, EOF

@dataclass(frozen=True)
class Span:
    path: str
    start_line: int; start_col: int
    end_line:   int; end_col:   int
    start_off:  int; end_off:   int

@dataclass(frozen=True)
class Token:
    kind: TokenKind
    text: str
    span: Span
```

The lexer emits `NEWLINE` tokens (used by the parser as children-block separators per §6.5 of `mch-spec.md`); all other whitespace is consumed silently.

### 5.2 AST shape (`ast_.py`)

```python
@dataclass(frozen=True)
class File:
    path: str
    opcodes:    tuple[OpcodeDecl, ...]
    predicates: tuple[PredicateDecl, ...]
    rules:      tuple[RuleDecl, ...]
    matchers:   tuple[MatcherDecl, ...]
    stitches:   tuple[StitchDecl, ...]   # populated, ignored by v0 compiler

@dataclass(frozen=True)
class OpcodeDecl:    name: str; value: int; span: Span
@dataclass(frozen=True)
class PredicateDecl: name: str; span: Span
@dataclass(frozen=True)
class RuleDecl:      name: str; pattern: PatternExpr; span: Span

@dataclass(frozen=True)
class MatcherDecl:
    name: str
    produces: str
    build: str
    entry: str | None         # required in v0
    include_excess: bool
    include_bounces: bool
    pattern: PatternExpr
    span: Span

# PatternExpr is the abstract base.
class PatternExpr: ...

@dataclass(frozen=True)
class Sequence(PatternExpr):
    head: Atom
    tail: tuple[tuple[Edge, Atom], ...]   # (edge, next_atom) pairs

@dataclass(frozen=True)
class Alternative(PatternExpr):
    branches: tuple[PatternExpr, ...]

@dataclass(frozen=True)
class Node(PatternExpr):
    head: NodeHead
    capture: str | None
    where_predicate: str | None
    span: Span

@dataclass(frozen=True)
class RuleRef(PatternExpr):
    name: str
    span: Span

@dataclass(frozen=True)
class ChildrenBlock(PatternExpr):
    items: tuple[PatternExpr, ...]
    span: Span

@dataclass(frozen=True)
class Maybe(PatternExpr):
    inner: PatternExpr
    span: Span

class Edge(Enum):
    CHILD = "->"
    PARENT = "<-"

class NodeHead: ...
@dataclass(frozen=True)
class OpHead(NodeHead):    ref: str | int   # str = opcode identifier, int = literal
@dataclass(frozen=True)
class BTypeHead(NodeHead): name: str
@dataclass(frozen=True)
class PredHead(NodeHead):  name: str
@dataclass(frozen=True)
class AnyHead(NodeHead):   pass
```

Atom = anything assignable in a chain position (`Node`, parenthesised `PatternExpr`, `RuleRef`, `ChildrenBlock`, `Maybe`).

### 5.3 Capture analysis

The resolver computes, per matcher, a `dict[str, CaptureInfo]`:

```python
@dataclass
class CaptureInfo:
    name: str
    typ: type           # CallContractBlock, the btype's class, or Block
    optional: bool      # any `maybe` ancestor in the AST
    list_binding: bool  # inside a recursive rule
    source: ast_.Node   # for error messages and extractor wiring
```

This is the input the codegen layer uses to emit the typed Match dataclass and the input the compiler uses to wire capture extractors.

### 5.4 ResolvedFile

```python
@dataclass(frozen=True)
class ResolvedFile:
    raw: ast_.File
    opcodes:        dict[str, int]             # name → value (validated against host)
    rule_recursion: dict[str, RecursionClass]  # name → DIRECT | INDIRECT | NONE
    matchers:       tuple[ResolvedMatcher, ...]
    stitches:       tuple[ResolvedStitch, ...]
    diagnostics:    DiagnosticBag              # warnings; errors raise

class RecursionClass(Enum):
    NONE, DIRECT_RECURSIVE, INDIRECT_RECURSIVE
```

`ResolvedMatcher` mirrors `MatcherDecl` plus `captures: dict[str, CaptureInfo]` and `anchor_node: ast_.Node` (pre-located).

### 5.5 CompiledMatcher

```python
@dataclass
class CompiledMatcher:
    name: str                    # spec name, e.g. "jetton_transfer"
    matcher: BlockMatcher        # ready-to-register engine instance
    match_cls: type              # generated dataclass
    build_fn_name: str
    produces_cls_name: str
    source_span: Span
```

`matcher` is an instance of a *generated subclass* whose name equals `_MCH_<spec_name>`. The subclass carries:

```python
class _MCH_jetton_transfer(ContractMatcher):       # or BlockTypeMatcher, depending on anchor head
    __mch_name__ = "jetton_transfer"
    __mch_source__ = Span(...)
    async def build_block(self, block, other_blocks):
        m = JettonTransferMatch(
            transfer = _extract_transfer(block, other_blocks),
            internal = _extract_internal(block, other_blocks),
            notify   = _extract_notify(block, other_blocks),
        )
        produced = await registries.builders["build_jetton_transfer"](m)
        return _augment_with_directives(produced, ...)
```

The compiler caches generated subclasses in a `_compiled_subclasses: dict[str, type]` so repeated compilation of the same matcher is idempotent.

### 5.6 Capture extraction

The compiler tags each AST `Node` that owns a capture with a stable "position id" during lowering. The engine, after a successful match, holds a `matched: list[Block]` whose order corresponds to the lowering walk's depth-first ordering of sub-matchers. Each capture's extractor indexes into that list:

- Non-optional, non-list: `matched[i]` for the position id.
- Optional (under `maybe`): `matched[i] if i < len(matched) else None`. Engine result ordering must agree; the compiler ensures `maybe`-wrapped positions are appended in order even when absent (engine convention: optional non-match adds nothing).
- List (under recursive rule): the extractor walks the unrolled chain and collects all values for that capture across recursion levels.

The exact ordering contract is the only place where the compiler depends on engine internals beyond the public `BlockMatcher` API. A unit test pins it down with the smallest possible synthetic Block tree.

### 5.7 Registries

```python
@dataclass
class Registries:
    opcodes:     dict[str, int]                             = field(default_factory=dict)
    predicates:  dict[str, Callable[[Block], bool]]         = field(default_factory=dict)
    block_types: dict[str, type[Block]]                     = field(default_factory=dict)
    builders:    dict[str, Callable[..., Awaitable[Block]]] = field(default_factory=dict)

    def register_predicate(self, name: str): ...   # decorator factory
    def register_builder(self,   name: str): ...

# Module-level helpers (operate on a default registry, used in non-test code)
@predicate("pton_self_transfer")
def pton_self_transfer(b: Block) -> bool: ...

@build("build_jetton_transfer")
async def build_jetton_transfer(m: "JettonTransferMatch") -> JettonTransferBlock: ...
```

For tests, every test constructs a fresh `Registries()` and decorates against it. Module-level decorators register into a module-default `Registries` that production code uses; tests never share this global.

## 6. Anchor selection

`entry @X` is required on every matcher in v0 (deviation from §10.2 of `mch-spec.md`).

Compiler procedure:
1. Locate the `Node` AST owning capture `X`.
2. Validate §8.3 constraints (not inside `maybe`, not inside an alternative branch that may not match, not inside a recursive rule, head must be `op` or `btype`). Resolver does this; compiler asserts.
3. Choose base class:
   - `op` head → `ContractMatcher(opcode=…)`
   - `btype` head → `BlockTypeMatcher(block_type=…)`
4. Treat the anchor node as the pivot. Everything structurally upstream in the pattern (along `<-` edges or as a parent in a `->` chain whose tail crosses the anchor) becomes a `parent_matcher` chain. Everything downstream becomes a `child_matcher` chain or a `children_matchers` list.

The lowering walk is implemented in one helper:

```python
def lower_relative_to_anchor(
    pattern: PatternExpr,
    anchor: ast_.Node,
    bindings: CaptureBindings,
    ctx: CompileCtx,
) -> tuple[BlockMatcher | None, BlockMatcher | None, list[BlockMatcher] | None]:
    """Returns (parent_matcher, child_matcher, children_matchers)."""
```

## 7. Recursion lowering

`RecursionStrategy` is a protocol:

```python
class RecursionStrategy(Protocol):
    def lower(self, rule: ResolvedRule, ctx: CompileCtx) -> BlockMatcher: ...
```

v0 ships `RecursiveMatcherStrategy`:
- Walks the rule body looking for the recursion shape.
- For each `$self` occurrence, classifies the surrounding context:
  - `$self` inside `Maybe` → the "exit" branch of `RecursiveMatcher` (combined with any sibling alternatives that *don't* include `$self`).
  - `$self` not inside `Maybe` → part of the "repeating" branch.
- Emits `RecursiveMatcher(repeating_matcher=<step>, exit_matcher=<exit>)`.
- If no decomposable shape is found, emits `C001_RECURSION_SHAPE_UNSUPPORTED`.

Stonfi V2's `swap_step` rule (spec §14.2) compiles via this strategy: the `maybe $swap_step` clauses become the exit (the absence of further recursion), the rest forms the repeating step.

When `RecursiveMatcher` is later replaced with a different mechanism, only `recursion.py` changes — the compiler's interface stays the same.

## 8. Codegen

`codegen.emit(resolved, out_dir)` writes one `.py` file per matcher into `out_dir/generated/`:

```python
# generated/jetton_transfer_match.py
# generated from indexer/events/mch/specs/jettons.mch  (do not edit)
from dataclasses import dataclass
from indexer.events.blocks.basic_blocks import CallContractBlock

@dataclass
class JettonTransferMatch:
    transfer: CallContractBlock
    internal: CallContractBlock
    notify:   CallContractBlock | None
```

Rules:
- One file per `matcher` decl.
- File names: `<matcher_name>_match.py`.
- Idempotent: if existing file content equals new content (modulo a stable timestamp-free header), no write.
- Header includes a content hash so reviewers can detect stale generated files at a glance.
- Imports limited to types actually referenced in the dataclass.

Types follow §7.1–7.3 of `mch-spec.md`:
- `op IDENT` → `CallContractBlock`
- `btype IDENT` → block class from `Registries.block_types[IDENT]`
- `pred IDENT` / `any` → `Block`
- `maybe`-wrapped → `T | None`
- recursive-rule captures → `list[T]`

## 9. Diagnostics

```python
class Severity(Enum):
    ERROR
    WARNING

@dataclass(frozen=True)
class Diagnostic:
    severity:   Severity
    code:       str        # e.g. "R001_UNKNOWN_OPCODE"
    message:    str
    span:       Span
    suggestion: str | None = None

class DiagnosticBag:
    items: list[Diagnostic]
    def error(self, code: str, msg: str, span: Span, suggestion: str | None = None) -> None: ...
    def warning(self, code: str, msg: str, span: Span, suggestion: str | None = None) -> None: ...
    @property
    def has_errors(self) -> bool: ...

class CompileError(Exception):
    bag: DiagnosticBag

def format_diagnostic(d: Diagnostic, source_text: str) -> str:
    """Returns the §13 multi-line representation:
        <path>:<line>:<col>: error: <message> (<code>)
                <source line>
                ^^^^^^
        (suggestion, if present)
    """
```

### 9.1 Diagnostic codes (reserved namespace)

| Prefix | Phase | Examples |
|---|---|---|
| `L###` | lexer | `L001_BAD_NUMERIC`, `L002_UNKNOWN_CHAR`, `L003_UNTERMINATED_STRING` |
| `P###` | parser | `P001_UNEXPECTED_TOKEN`, `P002_EMPTY_CHILDREN_ITEM`, `P003_MIXED_EDGE_DIRECTIONS` |
| `R###` | resolver | `R001_UNKNOWN_OPCODE`, `R002_OPCODE_VALUE_MISMATCH`, `R003_UNKNOWN_PREDICATE`, `R004_UNDEFINED_RULE`, `R005_DUP_CAPTURE`, `R006_UNBOUNDED_RECURSION`, `R007_ENTRY_VIOLATES_8_3`, `R008_ENTRY_REQUIRED` |
| `C###` | compiler | `C001_RECURSION_SHAPE_UNSUPPORTED`, `C002_UNKNOWN_BUILDER`, `C003_UNKNOWN_BLOCK_TYPE` |
| `G###` | codegen | `G001_STALE_GENERATED` (warning) |
| `W###` | warnings | `W001_UNUSED_RULE`, `W002_UNUSED_OPCODE`, `W003_INCLUDE_DEVIATION`, `W900_STITCH_DEFERRED`, `W201_NO_SELECTIVITY_STATS` (reserved, unused in v0) |

Tests assert by `code`, never by message text — wording stays editable.

## 10. why_not.py compatibility

`tools/why_not.py` and `tools/matcher_trace/` work by patching every `BlockMatcher` subclass's instrumented methods at import time. Because compiled matchers are real `BlockMatcher` subclasses (one synthesized per matcher decl), the patches apply transparently; no automated parity test is needed.

To get readable names in `why_not` output, a single one-line change goes into `tools/matcher_trace/_summary.py`: when summarising a matcher, prefer `getattr(matcher, "__mch_name__", None)` over `type(matcher).__name__` if present. This change lives in `tools/`, is one line plus a fallback, and is the only modification outside the new module.

The `why_not.py` CLI is treated as a *diagnostic affordance for the human developer or an agent investigating mismatched DSL behavior*, not as an automated test target.

## 11. Test strategy (TDD ladder)

Tests under `indexer/tests/mch/`, one file per module plus `integration/`.

Each TDD step: write a failing test for the smallest possible behavior, then make it pass.

| # | Layer | Smallest failing test → passing test |
|---|---|---|
| 1 | `diagnostics` | format a `Diagnostic` → matches §13 caret layout |
| 2 | `tokens` + `lexer` | tokenize `"opcode X = 0x1"` → 4 tokens with correct spans |
| 3 | `lexer` errors | each `L###` code with span |
| 4 | `ast_` | smoke: construct values |
| 5 | `parser` smallest | parse `opcode X = 0x1` → `File(opcodes=(OpcodeDecl(...),))` |
| 6 | `parser` shapes | linear chain; alternative; maybe; children block; rule ref; predicate clause (one micro-test each) |
| 7 | `parser` errors | each `P###` code with span |
| 8 | `registry` | decorator registers; duplicate raises; lookup errors |
| 9 | `resolver` success | resolve a complete file → all references bound |
| 10 | `resolver` errors | each `R###` code |
| 11 | `resolver` captures | each row of §7.1/7.2/7.3 produces the expected `CaptureInfo` |
| 12 | `recursion` | `step \| maybe $self` → `RecursiveMatcher`; `step -> (a \| $self \| b)` → `RecursiveMatcher`; unbounded → error |
| 13 | `compiler` structural | each node head → expected engine subclass instance; `entry` honored; `__mch_name__` set |
| 14 | `compiler` behavioral | feed synthetic `Block` tree → matcher fires; builder receives correct `Match` |
| 15 | `codegen` | each capture-type variant; idempotent re-emit; importable output |
| 16 | `integration/jetton_transfer` | full pipeline on §14.1 spec → matches existing `.lz4` trace as the hand-written matcher does |
| 17 | `integration/stonfi_v2_swap` | full pipeline on §14.2 spec, exercises recursion |

### 11.1 Synthetic block tree fixture

`conftest.py` exposes a `build_block_tree` helper that constructs a `Block` tree from a compact spec, so compiler tests don't depend on `.lz4` files. Real-trace coverage is the integration tests only.

### 11.2 Test isolation

Every test that needs predicates / builders / block types constructs a fresh `Registries()` and decorates against it. No module-level decorator side effects in tests.

### 11.3 Integration test assertion

For each worked example: compile the DSL, run the compiled matcher against an existing `.lz4` trace from `tests/traces/`, run the hand-written matcher against the same trace, assert structural equivalence of the resulting `Block` trees (`btype`, opcodes, sequence order, children set up to ordering).

## 12. File deliverables

After implementation:

```
indexer/events/mch/
  __init__.py            # exports compile_text / compile_file / compile_dir / Registries
  tokens.py
  lexer.py
  ast_.py
  parser.py
  registry.py
  resolver.py
  recursion.py
  compiler.py
  codegen.py
  diagnostics.py
  compile.py
  specs/
    jetton_transfer.mch
    stonfi_v2_swap.mch
  generated/             # codegen output (gitignored or committed; decision deferred)
    jetton_transfer_match.py
    stonfi_v2_swap_match.py

indexer/tests/mch/
  conftest.py
  test_lexer.py
  test_parser.py
  test_resolver.py
  test_recursion.py
  test_compiler_structural.py
  test_compiler_behavioral.py
  test_codegen.py
  test_diagnostics.py
  test_registry.py
  integration/
    test_jetton_transfer.py
    test_stonfi_v2_swap.py

tools/matcher_trace/_summary.py   # one-line change: honor __mch_name__
```

No changes to `indexer/events/event_processing.py`, `indexer/events/trace_processor.py`, the production `matchers` list, or any hand-written matcher.

## 13. Open items

- Whether `generated/*_match.py` should be committed or gitignored — defer to implementation (small impact either way; gitignored avoids review noise; committed gives PR reviewers immediate visibility into Match shapes).
- Exact format of the codegen file header content hash (BLAKE2 short hash on the source `.mch` is a reasonable default).
- Test coverage threshold target — defer to project norm.

## 14. Future work (not v0)

- Stitch lowering (§5.5, §9, §14.3 of `mch-spec.md`).
- Bounded-unrolling and per-rule-subclass `RecursionStrategy` alternatives.
- Selectivity-stats-based anchor selection (`opcode_freq.json` / `btype_freq.json`).
- Replacement of selected hand-written matchers in the production `matchers` list under a config flag.
- Codegen of `.pyi` stub files for IDE/mypy alongside the runtime files.
- C++ host contract and cross-language fidelity harness (§11.2, §11.3).
- Field-constraint clauses on captures, quantifiers on children-set items, multi-arity merge — the §16 future-extensions list.
