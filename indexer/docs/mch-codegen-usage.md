# `.mch` Codegen Usage Guide

The codegen module emits one Python dataclass file per `matcher` declaration in your `.mch` source. Builders import these dataclasses so they get IDE autocompletion and type checking on capture access.

**Module:** `indexer.events.mch.codegen`
**Public API:** one function — `emit(resolved, out_dir)`.

## What it does

For each `matcher M { ... }` in a resolved `.mch` file, codegen writes `<out_dir>/<M>_match.py` containing:

```python
# Generated from <source.mch>  (do not edit)
# hash: <6-byte blake2s hash>
from __future__ import annotations
from dataclasses import dataclass
from indexer.events.blocks.basic_blocks import CallContractBlock   # (only what's used)

@dataclass
class MMatch:
    cap_name1: CallContractBlock
    cap_name2: CallContractBlock | None       # under `maybe`
    cap_name3: list[CallContractBlock]        # under a recursive rule
```

Field name = capture name. Field type derives from the capture's node head and modifiers (see "Type mapping" below).

## API

```python
def emit(resolved: ResolvedFile, out_dir: pathlib.Path) -> list[pathlib.Path]: ...
```

- `resolved`: output of `indexer.events.mch.resolver.resolve(...)`. You can't go directly from `.mch` text — you need lex + parse + resolve first.
- `out_dir`: target directory. Created if missing.
- Returns: list of paths written (or skipped — see Idempotency).
- Writes one `.py` per `matcher` declaration in `resolved.matchers`.

## End-to-end example

```python
from pathlib import Path

from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.resolver import resolve
from indexer.events.mch.codegen import emit
from indexer.events.mch.registry import Registries

# 1. Build a Registries with the host bindings the resolver needs.
reg = Registries()
reg.block_types["JettonTransferBlock"] = JettonTransferBlock   # your block class

@reg.register_builder("build_jetton_transfer")
async def build_jetton_transfer(m):
    ...

# 2. Pipeline: text -> tokens -> AST -> ResolvedFile.
src = Path("indexer/events/mch/specs/jetton_transfer.mch").read_text(encoding="utf-8")
tokens   = list(tokenize(src, "jetton_transfer.mch"))
file_ast = parse(tokens, "jetton_transfer.mch")
resolved = resolve(file_ast, reg)

# 3. Emit.
out_paths = emit(resolved, Path("indexer/events/mch/generated"))
# → ['indexer/events/mch/generated/jetton_transfer_match.py']
```

## CLI / batch script

A one-liner you can put in a `dev_utils/` script:

```python
# dev_utils/mch_codegen.py
from pathlib import Path
from indexer.events.mch import compile_dir   # implicit lex+parse+resolve+compile
from indexer.events.mch.codegen import emit
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.resolver import resolve
from indexer.events.mch.registry import Registries

SPEC_DIR = Path("indexer/events/mch/specs")
OUT_DIR  = Path("indexer/events/mch/generated")

reg = Registries()   # populate with your host bindings here

for spec in sorted(SPEC_DIR.glob("*.mch")):
    text = spec.read_text(encoding="utf-8")
    tokens   = list(tokenize(text, str(spec)))
    file_ast = parse(tokens, str(spec))
    resolved = resolve(file_ast, reg)
    written  = emit(resolved, OUT_DIR)
    print(f"{spec.name}: wrote {[p.name for p in written]}")
```

Run via: `"S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe" dev_utils/mch_codegen.py`.

## Using the generated dataclass in a builder

Once `<matcher>_match.py` exists, the builder imports it for typed access:

```python
from indexer.events.mch.generated.jetton_transfer_match import JettonTransferMatch
from indexer.events.mch.registry import Registries

reg = Registries()

@reg.register_builder("build_jetton_transfer")
async def build_jetton_transfer(m: JettonTransferMatch) -> JettonTransferBlock:
    # m.transfer is typed as CallContractBlock
    # m.internal is typed as CallContractBlock
    # m.notify   is typed as CallContractBlock | None
    transfer_body = m.transfer.get_body()
    ...
```

At runtime the compiler builds its own dataclass via `dataclasses.make_dataclass` and passes that to the builder — the *file* exists purely for the type checker / IDE. Both classes have the same fields in the same order, so the builder signature is honored.

## Type mapping (capture → field type)

| Capture lives under… | Node head | Generated field type |
|---|---|---|
| (none) | `op IDENT` / `op 0x…` | `CallContractBlock` |
| (none) | `btype X` | `Block` |
| (none) | `pred X` / `any` | `Block` |
| `maybe X` | any of above | `T \| None` |
| inside a recursive rule | any of above | `list[T]` |

`list[T]` takes precedence over `T \| None` — a capture inside a recursive rule that's also under `maybe` still emits `list[T]` (the empty list represents "rule didn't fire").

## Idempotency

`emit()` reads the existing file (if any) and writes only when content differs. Mtime is preserved when content is unchanged, so:

- Running `emit()` repeatedly during dev does NOT touch `git status` if specs haven't changed.
- `make` / `ninja` rules treating the file as a target won't trigger downstream rebuilds.

To force a rewrite, delete the file first.

## Header hash

The first lines of every generated file look like:

```python
# Generated from <source-path>  (do not edit)
# hash: 7f3d8c2a1b4e
```

The hash is `blake2s(f"{matcher_name}\n{body}", digest_size=6).hexdigest()` — it covers the matcher name and the field block. If you hand-edit the file (don't), the hash won't match a regenerated one, so reviewers can spot tampering with `grep -L "hash: " indexer/events/mch/generated/`.

## Common gotchas

- **`btype` captures import `Block`, not the protocol-specific class.** Codegen doesn't know which `Block` subclass corresponds to a btype string — that mapping lives in the host's runtime block-type registry. If you want the precise type in your builder, cast at the call site or annotate explicitly.
- **Predicates declared but not used in a capture don't appear in the dataclass.** `pred X` and `where X` don't produce fields — only `@capture` does.
- **The dataclass has no defaults.** Every field is required. Captures under `maybe` have type `T | None` but the field is still positional-required; the compiler always passes a value (which may be `None`).
- **Module name does NOT have the `_match` suffix.** The file is `jetton_transfer_match.py` but the dataclass is `JettonTransferMatch` (PascalCase of the matcher name + `Match`). Import: `from indexer.events.mch.generated.jetton_transfer_match import JettonTransferMatch`.
- **`generated/` is a real package only if it has `__init__.py`.** If you want `from indexer.events.mch.generated.* import …` to work, add an empty `__init__.py` there (the current setup uses `.gitkeep` only). Or import via `importlib.util.spec_from_file_location` for ad-hoc use.

## Relationship to the compiler

Codegen and the compiler share the resolver output but otherwise don't depend on each other:

- The **compiler** builds runtime `BlockMatcher` instances + an in-memory Match dataclass via `make_dataclass`. It works without codegen having ever run.
- **Codegen** writes `.py` files for IDE and type-checker support. It works without the compiler having ever run.

You can call them in any order. In practice: run codegen once when the spec changes, then keep using the compiler at runtime.

## What codegen does NOT do (v0)

- Does not emit `.pyi` stubs — only `.py`.
- Does not emit imports for protocol-specific block classes used by `btype` captures.
- Does not generate the matcher itself (that's the compiler's job).
- Does not emit C++ headers (deferred future work per the spec).

See `docs/mch-spec.md` §8 and `docs/superpowers/specs/2026-05-19-mch-dsl-design.md` §8 for the design rationale.
