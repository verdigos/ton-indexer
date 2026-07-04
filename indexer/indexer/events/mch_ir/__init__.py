"""MCH IR engine — runs matchers from an mch-ir.json artifact (docs/mch-ir-poc.md).

Host-side counterpart of the Go POC loader (mch-go-poc/ir/loader.go), including
full build-program execution. Consumes ONLY the artifact plus host registries
(indexer.events.mch.registry.Registries) — never the DSL frontend. This package
is the reference blueprint for the C++ implementation.

Public API::

    from indexer.events.mch_ir import load_artifact, build_matchers
    art = load_artifact(path_or_dict)          # parsed + version-checked
    matchers = build_matchers(art, registries) # registration order

Each returned matcher exposes `.name`, `.produces`, and the full BlockMatcher
interface (`test_self` / `try_build`); `try_build` returns `[produced_block]`
on success, `[]` on a builder/program rejection, `None` on structural mismatch
— exactly like the compiler-generated matchers.
"""
from indexer.events.mch_ir.engine import build_matchers
from indexer.events.mch_ir.expr import EvalError
from indexer.events.mch_ir.loader import Artifact, IrLoadError, load_artifact

__all__ = [
    "Artifact",
    "EvalError",
    "IrLoadError",
    "build_matchers",
    "load_artifact",
]
