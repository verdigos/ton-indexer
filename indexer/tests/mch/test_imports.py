from pathlib import Path

import pytest

from indexer.events.mch.compile import compile_file, compile_text
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch.tokens import TokenKind


def _registries():
    reg = Registries()
    reg.predicates["p"] = lambda block: True
    return reg


def _write(directory, name, text):
    path = directory / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def _matcher(pattern="op 1 @root", value="1"):
    return (
        "matcher m {\n produces test_out\n"
        f" {pattern}\n out {{ v: {value} }}\n}}\n"
    )


def _error(path, code):
    with pytest.raises(CompileError) as exc:
        compile_file(path, _registries())
    diagnostic = next(d for d in exc.value.bag.items if d.code == code)
    return diagnostic


@pytest.mark.parametrize("declaration, pattern, value", [
    ("opcode Shared = 0x42", "op Shared @root", "1"),
    ("predicate p", "op 1 @root where p", "1"),
    ("rule leg = op 2 @leg", "op 1 @root -> $leg", "leg.msg.value"),
    ("rule walk = op 2 @step -> maybe $walk", "op 1 @root -> $walk", "len(step)"),
    ("rule walk cyclic = or { op 2 -> $walk; op 3 }", "op 1 @root -> $walk", "1"),
    ("fn add_one(x) = x + 1", "op 1 @root", "add_one(root.msg.value)"),
    ("fn pair(x) = { a: x, b: 1 }", "op 1 @root", "pair(root.msg.value)"),
    ("fn positive(x) = x > 0", "op 1 @root where (positive(root.msg.value))", "1"),
])
def test_export_matches_local_ir(tmp_path, declaration, pattern, value):
    _write(tmp_path, "lib/shared.mch", "export " + declaration + "\n")
    matcher = _matcher(pattern, value)
    path = _write(tmp_path, "main.mch", 'import "lib/shared.mch"\n' + matcher)
    reg = _registries()
    actual = emit_ir(compile_file(path, reg))
    expected = emit_ir(compile_text(declaration + "\n" + matcher, str(path), reg))
    assert actual["matchers"] == expected["matchers"]
    assert actual["nodes"] == expected["nodes"]
    if declaration.startswith("fn add_one"):
        expanded = emit_ir(compile_text(_matcher(pattern, "root.msg.value + 1"), str(path), reg))
        assert actual["matchers"] == expanded["matchers"]


def test_private_opcode_is_invisible(tmp_path):
    _write(tmp_path, "lib.mch", "opcode Hidden = 2")
    path = _write(tmp_path, "main.mch", 'import "lib.mch"\n' + _matcher("op Hidden @root"))
    _error(path, "R001_UNKNOWN_OPCODE")


def test_exported_predicate_satisfies_named_where(tmp_path):
    _write(tmp_path, "lib.mch", "export predicate visible")
    path = _write(
        tmp_path, "main.mch",
        'import "lib.mch"\n' + _matcher("op 1 @root where visible"),
    )
    assert len(compile_file(path, _registries())) == 1


def test_private_predicate_does_not_satisfy_named_where(tmp_path):
    _write(tmp_path, "lib.mch", "predicate hidden")
    path = _write(
        tmp_path, "main.mch",
        'import "lib.mch"\n' + _matcher("op 1 @root where hidden"),
    )
    diagnostic = _error(path, "R044_UNDECLARED_PREDICATE")
    assert "hidden" in diagnostic.message


def test_private_fn_is_not_inlined(tmp_path):
    _write(tmp_path, "lib.mch", "fn hidden(x) = x + 1")
    matcher = _matcher(value="hidden(1)")
    path = _write(tmp_path, "main.mch", 'import "lib.mch"\n' + matcher)
    reg = _registries()
    actual = emit_ir(compile_file(path, reg))
    expected = emit_ir(compile_text(matcher, str(path), reg))
    assert actual["matchers"] == expected["matchers"]


@pytest.mark.parametrize("local_helper", ["", "fn helper(x) = x + 100\n"])
def test_exported_fn_inlines_private_helper(tmp_path, local_helper):
    _write(tmp_path, "lib.mch", "fn helper(x) = x + 1\nexport fn public(x) = helper(x)")
    matcher = _matcher(value="public(root.msg.value)")
    path = _write(tmp_path, "main.mch", 'import "lib.mch"\n' + local_helper + matcher)
    reg = _registries()

    actual = emit_ir(compile_file(path, reg))
    expected = emit_ir(compile_text(_matcher(value="root.msg.value + 1"), str(path), reg))

    assert actual == expected
    assert "helper" not in str(actual)


def test_private_rule_reference_is_undefined(tmp_path):
    _write(tmp_path, "lib.mch", "rule hidden = op 2")
    path = _write(tmp_path, "main.mch", 'import "lib.mch"\nrule visible = $hidden\n')
    _error(path, "R004_UNDEFINED_RULE")


def test_private_rule_capture_is_invisible(tmp_path):
    _write(tmp_path, "lib.mch", "rule hidden = op 2 @leg")
    path = _write(tmp_path, "main.mch", 'import "lib.mch"\n' +
                  _matcher("op 1 @root -> $hidden", "leg.msg.value"))
    _error(path, "R010_UNRESOLVED_REFERENCE")


@pytest.mark.parametrize("declaration", [
    "opcode Shared = 1", "predicate Shared", "rule Shared = op 1", "fn Shared(x) = x",
])
@pytest.mark.parametrize("other_import", [False, True])
def test_import_name_collision(tmp_path, declaration, other_import):
    _write(tmp_path, "lib.mch", "export " + declaration)
    if other_import:
        _write(tmp_path, "other.mch", "export " + declaration)
        source = 'import "lib.mch"\nimport "other.mch"\n'
    else:
        source = 'import "lib.mch"\n' + declaration
    path = _write(tmp_path, "main.mch", source)
    diagnostic = _error(path, "R041_IMPORT_NAME_COLLISION")
    assert diagnostic.span.path == str(path)


@pytest.mark.parametrize("local", ["fn Shared(x) = x", _matcher().replace("matcher m", "matcher Shared")])
def test_import_collides_across_declaration_kinds(tmp_path, local):
    _write(tmp_path, "lib.mch", "export opcode Shared = 1")
    path = _write(tmp_path, "main.mch", 'import "lib.mch"\n' + local)
    _error(path, "R041_IMPORT_NAME_COLLISION")


def test_different_opcode_names_may_share_value(tmp_path):
    _write(tmp_path, "lib.mch", "export opcode Shared = 1")
    path = _write(tmp_path, "main.mch", 'import "lib.mch"\nopcode Local = 1\n' +
                  _matcher("op Shared @root -> op Local"))
    assert len(compile_file(path, _registries())) == 1


def test_imports_are_not_reexported(tmp_path):
    _write(tmp_path, "lib/base.mch", "export opcode Hidden = 2")
    _write(tmp_path, "lib/shared.mch", 'import "base.mch"\nexport opcode Shared = 1')
    path = _write(tmp_path, "main.mch", 'import "lib/shared.mch"\n' + _matcher("op Hidden @root"))
    _error(path, "R001_UNKNOWN_OPCODE")


def test_shared_dependency_is_read_once(tmp_path, monkeypatch):
    base = _write(tmp_path, "lib/base.mch", "export opcode Base = 2")
    _write(tmp_path, "lib/shared.mch", 'import "base.mch"\nexport opcode Shared = 1')
    path = _write(tmp_path, "main.mch", 'import "lib/shared.mch"\nimport "lib/base.mch"\n' +
                  _matcher("op Shared @root -> op Base"))
    reads = []
    read_text = Path.read_text

    def read(file, *args, **kwargs):
        reads.append(file)
        return read_text(file, *args, **kwargs)

    monkeypatch.setattr(Path, "read_text", read)
    assert len(compile_file(path, _registries())) == 1
    assert reads.count(base) == 1


def test_matchers_are_not_imported(tmp_path):
    _write(tmp_path, "lib.mch", "export opcode Shared = 2\n" + _matcher())
    path = _write(tmp_path, "main.mch", 'import "lib.mch"\n' + _matcher("op Shared @root"))
    assert [m.name for m in compile_file(path, _registries())] == ["m"]


def test_compile_text_resolves_imports_relative_to_path(tmp_path):
    _write(tmp_path, "lib.mch", "export opcode Shared = 2")
    source = 'import "lib.mch"\n' + _matcher("op Shared @root")
    assert len(compile_text(source, str(tmp_path / "memory.mch"), _registries())) == 1


def test_import_export_ast_and_contextual_names():
    source = ('import "lib/shared.mch"\nexport opcode import = 1\n'
              'export predicate p\nexport rule r = op import\n'
              'export fn export(x) = x\nopcode Private = 2')
    tokens = list(tokenize(source, "main.mch"))
    assert all(t.kind is TokenKind.IDENT for t in tokens if t.text in {"import", "export"})
    file = parse(tokens, "main.mch")
    assert file.imports[0].path == "lib/shared.mch"
    assert file.imports[0].span.start_line == 1
    assert all(d.exported for d in (file.opcodes[0], *file.predicates, *file.rules, *file.fns))
    assert not file.opcodes[1].exported
