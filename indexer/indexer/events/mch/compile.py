from __future__ import annotations

from dataclasses import replace
from pathlib import Path

from indexer.events.mch import ast_, diagnostics
from indexer.events.mch.compiler import CompiledMatcher, compile_
from indexer.events.mch.diagnostics import CompileError, DiagnosticBag
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.recursion import RecursionStrategy
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import resolve


def _load_imports(file: ast_.File) -> ast_.File:
    cache: dict[Path, tuple[ast_.OpcodeDecl | ast_.PredicateDecl | ast_.RuleDecl | ast_.FnDecl, ...]] = {}
    active: set[Path] = set()
    bag = DiagnosticBag()
    kinds = ("opcodes", "predicates", "rules", "fns")

    def load(file: ast_.File) -> ast_.File:
        path = Path(file.path).resolve()
        active.add(path)
        declarations = (*file.opcodes, *file.predicates, *file.rules, *file.fns)
        names = {d.name for d in (*declarations, *file.matchers)}
        seen: set[Path] = set()
        imported = []
        for imp in file.imports:
            target = (path.parent / imp.path).resolve()
            if target in seen:
                bag.error(diagnostics.R040_DUPLICATE_IMPORT,
                          f"file {imp.path!r} is imported more than once", imp.span)
                raise CompileError(bag)
            seen.add(target)
            if target in active:
                bag.error(diagnostics.R039_IMPORT_CYCLE,
                          f"import of {imp.path!r} forms a cycle", imp.span)
                raise CompileError(bag)
            if target not in cache:
                try:
                    text = target.read_text(encoding="utf-8")
                except (FileNotFoundError, NotADirectoryError, IsADirectoryError):
                    bag.error(diagnostics.R038_IMPORT_NOT_FOUND,
                              f"import {imp.path!r} is not a file", imp.span)
                    raise CompileError(bag) from None
                load(parse(tokenize(text, str(target)), str(target)))
            for decl in cache[target]:
                if decl.name in names:
                    bag.error(diagnostics.R041_IMPORT_NAME_COLLISION,
                              f"imported name {decl.name!r} is already declared", imp.span)
                    raise CompileError(bag)
                names.add(decl.name)
                imported.append(decl)
        active.remove(path)
        # Cache only this file's exports; its imports stay private.
        cache[path] = tuple(d for d in declarations if d.exported)
        return replace(file, **{
            kind: getattr(file, kind) + tuple(d for d in imported if isinstance(d, cls))
            for kind, cls in zip(kinds, (ast_.OpcodeDecl, ast_.PredicateDecl, ast_.RuleDecl, ast_.FnDecl))
        })

    return load(file)


def compile_text(
    text: str,
    path: str,
    registries: Registries,
    *,
    recursion: RecursionStrategy | None = None,
) -> list[CompiledMatcher]:
    tokens = list(tokenize(text, path))
    file_ast = parse(tokens, path)
    resolved = resolve(_load_imports(file_ast), registries)
    return compile_(resolved, registries, recursion=recursion)


def compile_file(
    path: Path,
    registries: Registries,
    *,
    recursion: RecursionStrategy | None = None,
) -> list[CompiledMatcher]:
    text = Path(path).read_text(encoding="utf-8")
    return compile_text(text, str(path), registries, recursion=recursion)


def compile_dir(
    dir_path: Path,
    registries: Registries,
    *,
    recursion: RecursionStrategy | None = None,
) -> list[CompiledMatcher]:
    out: list[CompiledMatcher] = []
    for p in sorted(Path(dir_path).glob("*.mch")):
        out.extend(compile_file(p, registries, recursion=recursion))
    return out
