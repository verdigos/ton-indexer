"""Tests for the MCH IR emitter (docs/mch-ir-poc.md)."""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from indexer.events.mch import ast_, ir_emit
from indexer.events.mch.compile import compile_file, compile_text
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.expr_ir import emit_expr
from indexer.events.mch.ir_emit import (
    build_stub_registries,
    emit_default,
    emit_dir,
    emit_ir,
    render_json,
)
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse

SPECS_DIR = (
    Path(__file__).resolve().parents[2] / "indexer" / "events" / "mch" / "specs"
)
SPEC_PATHS = tuple(sorted(SPECS_DIR.glob("*.mch")))


def _emit() -> dict:
    return emit_dir(SPECS_DIR)


def test_manifest_emission_is_deterministic():
    """The built-in production list must emit byte-identical IR on repeated runs."""
    first = render_json(emit_default()).encode("utf-8")
    second = render_json(emit_default()).encode("utf-8")
    assert first == second


@pytest.mark.parametrize("output_option", ["-o", "--output"])
def test_cli_emits_default(tmp_path, monkeypatch, capsys, output_option):
    artifact = {"matchers": [], "nodes": []}
    monkeypatch.setattr(ir_emit, "emit_default", lambda: artifact)
    output = tmp_path / "out.json"
    ir_emit.main([output_option, str(output)])
    assert output.read_text() == render_json(artifact)
    captured = capsys.readouterr()
    assert captured.out == f"wrote {output}: 0 matchers, 0 nodes\n"
    assert captured.err == ""


@pytest.mark.parametrize("imported", [False, True])
def test_cli_compile_error_prints_each_diagnostic(tmp_path, monkeypatch, capsys, imported):
    source = tmp_path / "invalid.mch"
    pattern = "op Missing @a -> op Other"
    if imported:
        library = tmp_path / "lib" / "invalid.mch"
        library.parent.mkdir()
        library.write_text(f"export rule invalid = {pattern}\n")
        source.write_text('import "lib/invalid.mch"\n'
                          'matcher m { produces test_out\n op 1 @a\n out { v: 1 } }')
        diagnostic_source = library
    else:
        source.write_text(f"matcher m {{ produces test_out\n {pattern}\n out {{ v: 1 }} }}")
        diagnostic_source = source
    monkeypatch.setattr(ir_emit, "emit_default", lambda: emit_dir(tmp_path))
    output = tmp_path / "out.json"
    with pytest.raises(SystemExit) as exc:
        ir_emit.main(["-o", str(output)])
    assert exc.value.code == 1
    captured = capsys.readouterr()
    assert captured.out == ""
    assert "Traceback" not in captured.err
    assert captured.err.count("R001_UNKNOWN_OPCODE") == 2
    assert captured.err.count(f"{diagnostic_source}:") == 2
    assert captured.err.count(pattern) == 2
    assert sum("^" in line for line in captured.err.splitlines()) == 2
    assert not output.exists()


@pytest.mark.parametrize("source_args", [["specs"], ["--manifest", "manifest.yaml"]])
def test_cli_rejects_removed_source_options(tmp_path, capsys, source_args):
    output = tmp_path / "out.json"
    with pytest.raises(SystemExit) as exc:
        ir_emit.main([*source_args, "-o", str(output)])
    assert exc.value.code == 2
    captured = capsys.readouterr()
    assert "unrecognized arguments" in captured.err
    assert captured.out == ""
    assert not output.exists()


def test_emit_specs_dir_deterministic():
    first = render_json(_emit())
    second = render_json(_emit())
    assert first.encode("utf-8") == second.encode("utf-8")

    artifact = _emit()
    assert json.loads(first) == artifact
    assert artifact["mch_ir_version"] == "1.3"
    assert "frontend" not in artifact
    # Every node reference must be a valid index into the flat nodes array.
    n = len(artifact["nodes"])
    for node in artifact["nodes"]:
        refs = [node.get("child"), node.get("parent"), node.get("step"), node.get("exit")]
        refs += node.get("children", []) + node.get("branches", [])
        for r in refs:
            if r is not None:
                assert 0 <= r < n


def test_no_broken_specs():
    broken = []
    for path in SPEC_PATHS:
        try:
            source = path.read_text(encoding="utf-8")
            file = parse(tokenize(source, str(path)), str(path))
            emit_ir(compile_file(path, build_stub_registries([file])))
        except Exception as exc:  # pragma: no cover - defensive aggregation
            broken.append((path, f"{type(exc).__name__}: {exc}"))
    assert not broken, "specs failed to compile/emit offline:\n" + "\n".join(
        f"  {path}: {reason}" for path, reason in broken
    )


def test_jetton_transfer_graph_shape():
    artifact = _emit()
    matcher = next(m for m in artifact["matchers"] if m["name"] == "jetton_transfer")
    nodes = artifact["nodes"]

    # Anchor: unsigned opcode set for JettonTransfer 0x0f8a7ea5.
    assert matcher["anchor"] == {"kind": "opcode_set", "values": [0x0F8A7EA5]}
    # `produces JettonTransferBlock` (class name) normalizes to the btype string.
    assert matcher["produces"] == ["jetton_transfer"]

    root = nodes[matcher["root"]]
    assert root["kind"] == "contract"
    assert root["opcode"] == 0x0F8A7EA5
    assert root["capture"] == "transfer"
    assert not root.get("optional", False)

    internal = nodes[root["child"]]
    assert internal["kind"] == "contract"
    assert internal["opcode"] == 0x178D4519
    assert internal["capture"] == "internal"
    assert not internal.get("optional", False)

    notify = nodes[internal["child"]]
    assert notify["kind"] == "contract"
    assert notify["opcode"] == 0x7362D09C
    assert "capture" not in notify
    assert notify["optional"] is True


def test_dedust_swap_recursive_step_exit():
    artifact = _emit()
    matcher = next(m for m in artifact["matchers"] if m["name"] == "dedust_swap")
    nodes = artifact["nodes"]

    # Collect the matcher's reachable subgraph from its root.
    seen: set[int] = set()
    stack = [matcher["root"]]
    while stack:
        i = stack.pop()
        if i in seen:
            continue
        seen.add(i)
        node = nodes[i]
        for key in ("child", "parent", "step", "exit"):
            if key in node:
                stack.append(node[key])
        stack.extend(node.get("children", []))
        stack.extend(node.get("branches", []))

    recursive = [nodes[i] for i in seen if nodes[i]["kind"] == "recursive"]
    assert len(recursive) == 1
    rec = recursive[0]
    assert "step" in rec and "exit" in rec

    # $peer_chain step: or over the two swap-hop variants (peer / external).
    step = nodes[rec["step"]]
    assert step["kind"] == "or"
    assert len(step["branches"]) == 2
    step_nodes = [nodes[b] for b in step["branches"]]
    step_opcodes = {node["opcode"] for node in step_nodes}
    assert step_opcodes == {0x72ACA8AA, 0x61EE542D}
    assert all("child" not in node for node in step_nodes)
    for node in step_nodes:
        notification, = (nodes[i] for i in node["children"])
        assert notification["opcode"] == 0x9C610DE3
        assert notification["optional"] is True

    # Exit: the terminal DedustPayoutFromPool payout chain.
    exit_node = nodes[rec["exit"]]
    assert exit_node["kind"] == "contract"
    assert exit_node["opcode"] == 0xAD4EB6F5


def test_recursive_relation_child_preserves_frontier_step_ir():
    src = """
    opcode Anchor = 1
    opcode Step = 2
    opcode Prefix = 3
    opcode Exit = 4
    opcode Note = 5

    rule chain = or {
        op Step @head_step {
            -> { maybe op Note @head_note }
            -> maybe $chain
        }
        op Prefix @prefix -> op Step @tail_step {
            -> { maybe op Note @tail_note }
            -> maybe $chain
        }
        op Exit @exit
    }
    matcher m { produces test_out
        op Anchor @anchor -> $chain
        out { value: 1 }
    }
    """
    path = "recursive_relations.mch"
    file_ast = parse(tokenize(src, path), path)
    artifact = emit_ir(compile_text(src, path, build_stub_registries([file_ast])))
    matcher = artifact["matchers"][0]
    nodes = artifact["nodes"]

    root = nodes[matcher["root"]]
    recursive = nodes[root["child"]]
    step = nodes[recursive["step"]]
    head_step = nodes[step["branches"][0]]
    prefix = nodes[step["branches"][1]]
    tail_step = nodes[prefix["child"]]

    assert recursive["optional"] is True
    assert step["kind"] == "or"
    assert head_step["opcode"] == 2
    assert prefix["opcode"] == 3
    assert tail_step["opcode"] == 2
    assert "child" not in head_step
    assert "child" not in tail_step
    for branch, capture in ((head_step, "head_note"), (tail_step, "tail_note")):
        note, = (nodes[i] for i in branch["children"])
        assert note == {
            "kind": "contract",
            "opcode": 5,
            "optional": True,
            "capture": capture,
        }

    assert nodes[recursive["exit"]] == {
        "kind": "contract",
        "opcode": 4,
        "capture": "exit",
    }
    assert matcher["captures"] == [
        {"name": "anchor", "card": "one"},
        {"name": "head_step", "card": "many"},
        {"name": "head_note", "card": "many"},
        {"name": "prefix", "card": "many"},
        {"name": "tail_step", "card": "many"},
        {"name": "tail_note", "card": "many"},
        {"name": "exit", "card": "many"},
    ]


def test_per_branch_where_emits_mixed_anchor():
    source = """
    opcode A = 0x1
    opcode B = 0x2
    predicate is_marked
    matcher m { produces test_out
        xor { op A @x ; op B @x where is_marked }
        out { value: 1 }
    }
    """
    path = "mixed_anchor.mch"
    file = parse(tokenize(source, path), path)
    artifact = emit_ir(compile_text(source, path, build_stub_registries([file])))
    assert artifact["matchers"][0]["anchor"] == {
        "kind": "mixed",
        "branches": [{"op": 0x1}, {"op": 0x2, "where": "is_marked"}],
    }


def test_mixed_opcode_and_btype_with_where_emits_anchor():
    source = """
    opcode A = 0x1
    predicate is_marked
    matcher m { produces test_out
        xor { op A @x where is_marked ; btype jetton_transfer @x }
        out { value: 1 }
    }
    """
    path = "mixed_anchor.mch"
    file = parse(tokenize(source, path), path)
    artifact = emit_ir(compile_text(source, path, build_stub_registries([file])))
    assert artifact["matchers"][0]["anchor"] == {
        "kind": "mixed",
        "branches": [{"op": 0x1, "where": "is_marked"}, {"btype": "jetton_transfer"}],
    }


def test_pure_single_kind_group_emits_opcode_set_anchor():
    source = """
    opcode A = 0x1
    opcode B = 0x2
    matcher m { produces test_out
        xor { op A @x ; op B @x }
        out { value: 1 }
    }
    """
    path = "opcode_set_anchor.mch"
    file = parse(tokenize(source, path), path)
    artifact = emit_ir(compile_text(source, path, build_stub_registries([file])))
    assert artifact["matchers"][0]["anchor"] == {
        "kind": "opcode_set",
        "values": [0x1, 0x2],
    }


def test_predicate_anchor_emits_pred_record():
    source = """
    predicate is_marked
    matcher m { produces test_out
        pred is_marked @x
        out { value: 1 }
    }
    """
    path = "predicate_anchor.mch"
    file = parse(tokenize(source, path), path)
    artifact = emit_ir(compile_text(source, path, build_stub_registries([file])))
    assert artifact["matchers"][0]["anchor"] == {
        "kind": "pred",
        "pred": "is_marked",
    }



_BUILD_PROGRAM_SRC = """\
opcode TestBuildOp  = 0x1001
opcode TestBuildOp2 = 0x1002

matcher test_build_program {
    produces test_build_out

    op TestBuildOp @a -> op TestBuildOp2 @b where (.value > 0)

    parse a as TestMsg
    let w = lookup jetton_wallet(a.msg.destination)
    reject when w == null
    out {
        sender: a.msg.source if a.msg.source != null else b.msg.source
        amount: amount(a.msg.value)
    }
}
"""


def _emit_build_program() -> dict:
    path = "test_build_program.mch"
    file_ast = parse(list(tokenize(_BUILD_PROGRAM_SRC, path)), path)
    registries = build_stub_registries([file_ast])
    compiled = compile_text(_BUILD_PROGRAM_SRC, path, registries)
    return emit_ir(compiled)


def test_build_program_encoding():
    artifact = _emit_build_program()
    matcher = artifact["matchers"][0]
    nodes = artifact["nodes"]

    # Node-level `where_expr` on the non-anchor node (`b`), alongside the
    # existing named-predicate `where` field (mutually exclusive per node).
    anchor = nodes[matcher["root"]]
    assert anchor == {"kind": "contract", "opcode": 0x1001, "capture": "a", "child": 1}
    assert nodes[1] == {
        "kind": "contract",
        "opcode": 0x1002,
        "capture": "b",
        "where_expr": {
            "k": "bin", "op": ">",
            "l": {"k": "dotfield", "name": "value"},
            "r": {"k": "int", "v": 0},
        },
    }

    def attr(base: dict, *names: str) -> dict:
        e = base
        for n in names:
            e = {"k": "attr", "of": e, "name": n}
        return e

    a_msg_source = attr({"k": "name", "id": "a"}, "msg", "source")
    a_msg_value = attr({"k": "name", "id": "a"}, "msg", "value")
    a_msg_destination = attr({"k": "name", "id": "a"}, "msg", "destination")
    b_msg_source = attr({"k": "name", "id": "b"}, "msg", "source")
    a_msg_source, a_msg_destination, b_msg_source = (
        {"k": "call", "fn": "account", "args": [path]}
        for path in (a_msg_source, a_msg_destination, b_msg_source)
    )

    assert matcher["build_program"] == [
        {"s": "parse", "target": "a", "types": ["TestMsg"]},
        {
            "s": "let", "name": "w",
            "expr": {"k": "lookup", "name": "jetton_wallet", "args": [a_msg_destination]},
        },
        {
            "s": "reject",
            "when": {"k": "bin", "op": "==", "l": {"k": "name", "id": "w"}, "r": {"k": "null"}},
        },
        {
            "s": "out",
            "fields": [
                {
                    "name": "sender",
                    "expr": {
                        "k": "ternary",
                        "cond": {"k": "bin", "op": "!=", "l": a_msg_source, "r": {"k": "null"}},
                        "then": a_msg_source,
                        "else": b_msg_source,
                    },
                },
                {
                    "name": "amount",
                    "expr": {"k": "call", "fn": "amount", "args": [a_msg_value]},
                },
            ],
        },
    ]


def test_build_program_deterministic():
    first = render_json(_emit_build_program())
    second = render_json(_emit_build_program())
    assert first.encode("utf-8") == second.encode("utf-8")


def test_declared_named_where_preserves_ir():
    matcher = (
        "matcher m { produces test_out\n"
        " op 1 @a -> op 2 @b where declared\n"
        " out { value: 1 } }\n"
    )
    artifacts = []
    for source in ("predicate declared\n" + matcher, matcher + "predicate declared\n"):
        path = "declared_where.mch"
        file = parse(tokenize(source, path), path)
        artifacts.append(emit_ir(compile_text(source, path, build_stub_registries([file]))))
    assert artifacts[0] == artifacts[1]
    matcher_ir = artifacts[0]["matchers"][0]
    assert matcher_ir["anchor"] == {"kind": "opcode_set", "values": [1]}
    assert artifacts[0]["nodes"] == [
        {"kind": "contract", "opcode": 1, "capture": "a", "child": 1},
        {"kind": "contract", "opcode": 2, "capture": "b", "where": "declared"},
    ]


@pytest.mark.parametrize("old, new", [
    ("let w = lookup jetton_wallet(a.msg.destination)\nreject when w == null",
     "require w = lookup jetton_wallet(a.msg.destination)"),
    ("parse a as TestMsg", "parse? a as TestMsg"),
    ("reject when a.value == 1 or a.value == 2 or a.value == 3",
     "reject when a.value in [1, 2, 3]"),
    ("reject when a.value != 1 and a.value != 2 and a.value != 3",
     "reject when a.value not in [1, 2, 3]"),
    ("reject when false", "reject when a in []"),
    ("reject when true", "reject when a not in []"),
])
def test_build_spelling_emits_same_program(old, new):
    programs = []
    for stmts in (old, new):
        src = (
            "matcher m { produces test_build_out\n op 0x1001 @a\n"
            + stmts + "\nout { value: 1 } }"
        )
        path = "test_build_program.mch"
        file_ast = parse(tokenize(src, path), path)
        compiled = compile_text(src, path, build_stub_registries([file_ast]))
        artifact = emit_ir(compiled)
        programs.append(artifact["matchers"][0]["build_program"])
    assert programs[0] == programs[1]


@pytest.mark.parametrize("spelling, nullable, types", [
    ("parse!", False, ("TestMsg",)),
    ("parse?", True, ("TestMsg", "OtherMsg")),
])
def test_parse_expression_nullability_emitted(spelling, nullable, types):
    type_names = " | ".join(types)
    src = (
        "matcher m { produces test_build_out\n op 0x1001 @a\n"
        f"let v = {spelling} a as {type_names}\nout {{ value: 1 }} }}"
    )
    path = "test_build_program.mch"
    file_ast = parse(tokenize(src, path), path)
    let = file_ast.matchers[0].build_stmts[0]
    assert isinstance(let, ast_.LetStmt)
    assert isinstance(let.value, ast_.ParseExpr)
    assert let.value.nullable is nullable
    assert let.value.msg_types == types
    registries = build_stub_registries([file_ast])
    compiled = compile_text(src, path, registries)
    artifact = emit_ir(compiled)
    expected = {
        "k": "parse", "x": {"k": "name", "id": "a"},
        "types": list(types), "nullable": nullable,
    }
    assert emit_expr(let.value) == expected
    assert artifact["matchers"][0]["build_program"][0]["expr"] == expected


@pytest.mark.parametrize("name, literal", [("Excess", "0xd53276db"), ("Bounce", "0xffffffff")])
def test_builtin_opcode_emits_same_anchor(name, literal):
    anchors = []
    for opcode in (name, literal):
        src = _BUILD_PROGRAM_SRC.replace("op TestBuildOp @a", f"op {opcode} @a")
        path = "test_build_program.mch"
        file_ast = parse(tokenize(src, path), path)
        compiled = compile_text(src, path, build_stub_registries([file_ast]))
        artifact = emit_ir(compiled)
        anchors.append(artifact["matchers"][0]["anchor"])
    assert anchors[0] == anchors[1] == {"kind": "opcode_set", "values": [int(literal, 16)]}


@pytest.mark.parametrize("pattern, anchor", [
    ("op Zero @a", {"kind": "opcode_set", "values": [0]}),
    ("op A @a -> op Zero", {"kind": "opcode_set", "values": [1]}),
    ("op A @a -> xor { op Zero; op A }", {"kind": "opcode_set", "values": [1]}),
    ("op A @a -> or { op Zero; op A }", {"kind": "opcode_set", "values": [1]}),
    ("or { op A @a; op Zero @a }", {"kind": "opcode_set", "values": [0, 1]}),
    ("or { op Zero @a; btype call_contract @a }", {
        "kind": "mixed", "branches": [{"op": 0}, {"btype": "call_contract"}],
    }),
])
@pytest.mark.parametrize("host_opcode", [None, 99])
def test_declared_zero_opcode_emits_zero(pattern, anchor, host_opcode):
    artifacts = []
    for spelling in (pattern, pattern.replace("Zero", "0")):
        source = ("opcode Zero = 0\nopcode A = 1\n"
                  "matcher m { produces test_out\n"
                  + spelling + "\n out { v: 1 } }")
        path = "zero.mch"
        file = parse(tokenize(source, path), path)
        registries = build_stub_registries([file])
        if host_opcode is not None:
            registries.opcodes["Zero"] = host_opcode
        artifacts.append(emit_ir(compile_text(source, path, registries)))
    assert artifacts[0] == artifacts[1]
    assert artifacts[0]["matchers"][0]["anchor"] == anchor
    if "->" in pattern or pattern == "op Zero @a":
        assert any(node.get("opcode") == 0 for node in artifacts[0]["nodes"])
    for node in artifacts[0]["nodes"]:
        if node["kind"] == "or":
            assert node["exclusive"] is True


@pytest.mark.parametrize("pattern, expanded", [
    ("op [A, B] @x", "xor { op A @x ; op B @x }"),
    ("op [A, B] @x -> op T", "xor { op A @x ; op B @x } -> op T"),
    ("op R @r -> op [A, B, C, D] @x",
     "op R @r -> xor { op A @x ; op B @x ; op C @x ; op D @x }"),
    ("op R @r -> op [A, B] @x -> { op T }",
     "op R @r -> xor { op A @x -> { op T } ; op B @x -> { op T } }"),
])
def test_opcode_set_emits_same_matchers_and_nodes(pattern, expanded):
    artifacts = []
    for spelling in (pattern, expanded):
        src = (
            "opcode A = 1\nopcode B = 2\nopcode C = 3\nopcode D = 4\n"
            "opcode R = 5\nopcode T = 6\n"
            "matcher m { produces test_out\n"
            + spelling + "\n out { value: 1 } }"
        )
        path = "opcode_set.mch"
        file_ast = parse(tokenize(src, path), path)
        compiled = compile_text(src, path, build_stub_registries([file_ast]))
        artifacts.append(emit_ir(compiled))
    for key in ("matchers", "nodes"):
        assert artifacts[0][key] == artifacts[1][key]


@pytest.mark.parametrize("head, separator", [("or", "|"), ("xor", "^")])
def test_alternative_block_replaces_operator(head, separator):
    old = f"(op 1 @x {separator} op 2 @x)"
    src = (
        "matcher m { produces test_out\n op 4 @r -> "
        + old + "\n out { value: 1 } }"
    )
    path = "alternative.mch"
    src = src.replace(old, head + " { op 1 @x ; op 2 @x }")
    file_ast = parse(tokenize(src, path), path)
    compiled = compile_text(src, path, build_stub_registries([file_ast]))
    artifact = emit_ir(compiled)
    nodes = artifact["nodes"]
    child = nodes[nodes[artifact["matchers"][0]["root"]]["child"]]
    assert child["kind"] == "or"
    assert child["exclusive"] is True


def test_peek_true_is_emitted_and_false_is_omitted():
    src = """
    opcode PeekRoot = 0x2001
    opcode PeekChild = 0x2002
    matcher peek_emit {
        produces peek_out

        op PeekRoot @root -> peek op PeekChild @seen
        out { v: 1 }
    }
    """
    path = "peek_emit.mch"
    file_ast = parse(list(tokenize(src, path)), path)
    artifact = emit_ir(compile_text(src, path, build_stub_registries([file_ast])))
    matcher = artifact["matchers"][0]
    root = artifact["nodes"][matcher["root"]]
    child = artifact["nodes"][root["child"]]

    assert artifact["mch_ir_version"] == "1.3"
    assert "peek" not in root
    assert child["peek"] is True


def _relation_artifact(pattern, rule="op A -> { op X }"):
    src = (
        "opcode A = 1\nopcode B = 2\nopcode X = 3\nopcode Y = 4\n"
        "opcode P = 5\nopcode R = 6\nopcode Q = 7\n"
        "rule leg = op Q\nrule r = " + rule + "\n"
        "matcher m { produces test_out\n"
        + pattern + "\n out { value: 1 } }"
    )
    path = "relations.mch"
    file_ast = parse(tokenize(src, path), path)
    compiled = compile_text(src, path, build_stub_registries([file_ast]))
    artifact = emit_ir(compiled)
    return {key: artifact[key] for key in ("matchers", "nodes")}


@pytest.mark.parametrize("pattern, expanded", [
    ("op A @a { -> op X -> op Y }", "op A @a -> op X -> op Y"),
    ("op A @a { -> { op X ; maybe op Y } }",
     "op A @a -> { op X ; maybe op Y }"),
    ("op R @r -> op A { -> { op X } }", "op R @r -> op A -> { op X }"),
    ("op R @r -> { op A { -> op X } ; op B }",
     "op R @r -> { op A -> op X ; op B }"),
    ("op A @a { <- op P <- op Q }", "op A @a <- op P <- op Q"),
    ("op A @a { -> { maybe op X ; maybe op Y } }",
     "op A @a -> { maybe op X ; maybe op Y }"),
    ("op R @r -> maybe op A { -> { op X } }",
     "op R @r -> maybe (op A -> { op X })"),
    ("op R @r -> peek op A { -> { peek op X } }",
     "op R @r -> peek (op A -> { peek op X })"),
    ("op R @r -> xor { op A { -> op X } ; op B }",
     "op R @r -> xor { op A -> op X ; op B }"),
    ("op R @r -> or { op A { -> op X } ; op B }",
     "op R @r -> or { op A -> op X ; op B }"),
    ("op R @r -> op [A, B] @x { -> { op X @y } }",
     "op R @r -> op [A, B] @x -> { op X @y }"),
    ("op [A, B] @a { -> { op X @x } }",
     "op [A, B] @a -> { op X @x }"),
])
def test_relation_blocks_emit_same_matchers_and_nodes(pattern, expanded):
    assert _relation_artifact(pattern) == _relation_artifact(expanded)


def test_rule_body_relation_block_emits_same_matchers_and_nodes():
    pattern = "op R @r -> $r"
    assert _relation_artifact(pattern, "op A { -> { op X } }") == (
        _relation_artifact(pattern, "op A -> { op X }")
    )


def test_relation_item_order_does_not_change_artifact():
    clauses = ["-> op X @x", "-> { op Y @y }", "<- op P @p"]
    assert _relation_artifact("op A @a { " + "; ".join(clauses) + " }") == (
        _relation_artifact("op A @a { " + "; ".join(reversed(clauses)) + " }")
    )


@pytest.mark.parametrize("pattern, expanded, parses", [
    ("op A @x as T -> op B @y as U", "op A @x -> op B @y",
     [("x", ["T"]), ("y", ["U"])]),
    ("op A @x -> or { op B @y as T | U ; op C @y as T | U }",
     "op A @x -> or { op B @y ; op C @y }", [("y", ["T", "U"])]),
    ("op A @x -> or { op B @y as T -> op C ; op B @y as T }",
     "op A @x -> or { op B @y -> op C ; op B @y }", [("y", ["T"])]),
    ("op [A, B] @x as T", "op [A, B] @x", [("x", ["T"])]),
])
def test_parse_annotations_emit_same_build_program(pattern, expanded, parses):
    artifacts = []
    written = "\n".join(f"parse {name} as {' | '.join(types)}" for name, types in parses)
    for spelling, statements in ((pattern, ""), (expanded, written)):
        src = (
            "opcode A = 1\nopcode B = 2\nopcode C = 3\n"
            "matcher m { produces test_out\n"
            + spelling + "\n" + statements + "\nlet v = 1\nout { value: v } }"
        )
        path = "parse_annotation.mch"
        file_ast = parse(tokenize(src, path), path)
        registries = build_stub_registries([file_ast])
        registries.message_types.update(T=object, U=object)
        artifacts.append(emit_ir(compile_text(src, path, registries)))
    assert artifacts[0] == artifacts[1]
    assert artifacts[0]["matchers"][0]["build_program"] == [
        *({"s": "parse", "target": name, "types": types} for name, types in parses),
        {"s": "let", "name": "v", "expr": {"k": "int", "v": 1}},
        {"s": "out", "fields": [{"name": "value", "expr": {"k": "name", "id": "v"}}]},
    ]


def test_rule_parse_annotations_emit_in_each_referencing_matcher():
    artifacts = []
    for annotation, statements in (
        (" as T | U", ""),
        ("", "parse x as T | U\nparse y as T | U\nparse z as T | U\n"),
    ):
        src = f"rule leg = op 2 @y{annotation}\n"
        for name in ("first", "second"):
            src += (
                f"matcher {name} {{ produces test_out\n"
                f"op 1 @x{annotation} -> $leg -> op 3 @z{annotation}\n"
                + statements + "let v = 1\nout { value: v } }\n"
            )
        path = "rule_parse_annotation.mch"
        file_ast = parse(tokenize(src, path), path)
        registries = build_stub_registries([file_ast])
        registries.message_types.update(T=object, U=object)
        artifacts.append(emit_ir(compile_text(src, path, registries)))
    assert artifacts[0] == artifacts[1]
    for matcher in artifacts[0]["matchers"]:
        assert matcher["build_program"][:3] == [
            {"s": "parse", "target": name, "types": ["T", "U"]}
            for name in ("x", "y", "z")
        ]


def test_stub_registry_collects_parse_types_from_fn_body():
    src = "fn f(x) = (parse? x as T).v"
    file = parse(tokenize(src, "fn.mch"), "fn.mch")
    assert set(build_stub_registries([file]).message_types) == {"T"}


def test_stub_registry_collects_parse_types_from_where_expression():
    source = (
        "matcher m { produces test_out\n"
        " btype call_contract @a where ((parse? . as UnknownMsg).value == 42)\n"
        " out { value: 1 } }"
    )
    path = "where_parse.mch"
    file = parse(tokenize(source, path), path)
    registries = build_stub_registries([file])
    assert "UnknownMsg" in registries.message_types
    artifact = emit_ir(compile_text(source, path, registries))
    assert artifact["nodes"][0]["where_expr"]["l"]["of"]["types"] == ["UnknownMsg"]


def _fn_artifact(declarations, statements, pattern="op 1 @a", rules=""):
    src = (
        declarations + "\n" + rules + "\n"
        "matcher m { produces test_out\n"
        + pattern + "\n" + statements + "\n}"
    )
    path = "fn.mch"
    file = parse(tokenize(src, path), path)
    registries = build_stub_registries([file])
    registries.message_types["T"] = object
    return emit_ir(compile_text(src, path, registries))


@pytest.mark.parametrize("root", ["X", "alias"])
@pytest.mark.parametrize("field", ["source", "destination", "transaction.account"])
def test_envelope_address_emits_account_once(root, field):
    path = f"{root}.msg.{field}"
    prefix = "let alias = X\n" if root == "alias" else ""
    artifacts = [
        _fn_artifact("", prefix + f"let v = {expr}\nout {{ v: v }}", "op 1 @a -> op 2 @X")
        for expr in (path, f"account({path})")
    ]
    assert render_json(artifacts[0]) == render_json(artifacts[1])
    raw = {"k": "name", "id": root}
    for name in ["msg", *field.split(".")]:
        raw = {"k": "attr", "of": raw, "name": name}
    assert artifacts[0]["matchers"][0]["build_program"][-2]["expr"] == {
        "k": "call", "fn": "account", "args": [raw],
    }


@pytest.mark.parametrize("root", ["a", ""])
@pytest.mark.parametrize("field", ["source", "destination", "transaction.account"])
def test_where_envelope_address_emits_account_once(root, field):
    path = f"{root}.msg.{field}"
    artifacts = [
        _fn_artifact("", "out { v: 1 }", f"op 1 @a where ({expr} != null)")
        for expr in (path, f"account({path})")
    ]
    assert artifacts[0] == artifacts[1]
    expr = artifacts[0]["nodes"][0]["where_expr"]["l"]
    assert expr["k"] == "call" and expr["fn"] == "account"
    assert expr["args"][0]["k"] == "attr"


def test_where_dot_fields_emit_expected_ir():
    expected = {
        ".": {"k": "dotself"},
        ".msg": {"k": "attr", "of": {"k": "dotself"}, "name": "msg"},
        ".other": {"k": "dotfield", "name": "other"},
        ".failed": {"k": "dotfield", "name": "failed"},
    }
    for expression, expression_ir in expected.items():
        artifact = _fn_artifact("", "out { v: 1 }", f"op 1 @a where ({expression})")
        assert artifact["nodes"][0]["where_expr"] == expression_ir


def test_parse_root_membership_emits_expanded_expression():
    left = "(parse? . as T).value"
    membership = _fn_artifact("", "out { v: 1 }", f"op 1 @a where ({left} in [1, 42])")
    expanded = _fn_artifact(
        "", "out { v: 1 }", f"op 1 @a where ({left} == 1 or {left} == 42)",
    )
    assert membership == expanded


def test_strict_parse_is_rejected_in_where_expression():
    with pytest.raises(CompileError) as exc:
        _fn_artifact(
            "", "out { v: 1 }", "op 1 @a where ((parse! . as T).value == 42)",
        )
    assert any(
        diagnostic.code == "C013_WHERE_EXPR_NOT_SYNC"
        and diagnostic.message == "`where (expr)` allows only `parse?`"
        for diagnostic in exc.value.bag.items
    )


@pytest.mark.parametrize(
    "btype", ["ton_transfer", "call_contract", "tick_tock", "jetton_transfer"],
)
def test_parse_statement_accepts_only_base_block_targets(btype):
    if btype != "jetton_transfer":
        assert _fn_artifact("", "parse a as T\nout { v: 1 }", f"btype {btype} @a")
        return
    with pytest.raises(CompileError) as exc:
        _fn_artifact("", "parse a as T\nout { v: 1 }", "btype jetton_transfer @a")
    diagnostic = next(
        diagnostic
        for diagnostic in exc.value.bag.items
        if diagnostic.code == "R023_PARSE_TARGET_NOT_BASE_BLOCK"
    )
    assert "produced block" in diagnostic.message
    assert "only base blocks (ton_transfer, call_contract, tick_tock) can be parsed" in (
        diagnostic.message
    )


@pytest.mark.parametrize("path", [
    "a.msg.value", "a.msg.opcode", "a.body.source", "a.data.source",
    "a.source", "a.transaction.account", "a.data.msg.source",
    "(lookup nft_item(a.msg.source)).owner_address",
])
def test_other_fields_emit_bare_attrs(path):
    artifact = _fn_artifact("", f"parse a as T\nlet v = {path}\nout {{ v: v }}")
    assert artifact["matchers"][0]["build_program"][1]["expr"]["k"] == "attr"


@pytest.mark.parametrize("declarations, call, expanded", [
    ("fn f(x) = x + 1", "f(a.msg.value)", "a.msg.value + 1"),
    ("fn k(x) = f(f(x))\nfn f(x) = x + 1", "k(a.msg.value)", "(a.msg.value + 1) + 1"),
    ("fn f(x) = x + 1", "f(f(a.msg.value))", "(a.msg.value + 1) + 1"),
    ("fn g() = 1", "g()", "1"),
    ("fn twice(x) = x + x", "twice(a.msg.value)", "a.msg.value + a.msg.value"),
    ("fn f(x) = 1", "f(missing)", "1"),
    ("fn f(x) = x", "f(lookup wallet(a.msg.source))", "lookup wallet(a.msg.source)"),
    ("fn f(x) = x", "f(map([1] as e => e + 1))", "map([1] as e => e + 1)"),
    ("fn f(x) = x", "map([1] as e => f(e))", "map([1] as e => e)"),
    ("fn f(x) = parse? x as T", "f(a)", "parse? a as T"),
    ("fn f(x) = parse! x as T", "f(a)", "parse! a as T"),
    ("fn f(x) = host_fn(x)", "f(a.msg.value)", "host_fn(a.msg.value)"),
    ("fn f(len) = len(len)", "f([1, 2])", "len([1, 2])"),
])
def test_fn_emits_expanded_build_program(declarations, call, expanded):
    actual = _fn_artifact(declarations, f"let v = {call}\nout {{ v: v }}")
    expected = _fn_artifact("", f"let v = {expanded}\nout {{ v: v }}")
    assert actual["matchers"][0]["build_program"] == expected["matchers"][0]["build_program"]
    assert render_json(actual) == render_json(expected)


def test_fn_ternary_field_arguments_in_out():
    actual = _fn_artifact(
        "fn h(a, b) = a if a > b else b",
        "out { m: h(x.msg.value, y.msg.value) }",
        "op 1 @a -> op 2 @x -> op 3 @y",
    )
    expected = _fn_artifact(
        "", "out { m: x.msg.value if x.msg.value > y.msg.value else y.msg.value }",
        "op 1 @a -> op 2 @x -> op 3 @y",
    )
    assert actual == expected


def test_fn_expands_require_guards_and_switch():
    statements = """
        require v = f(a.msg.value)
        reject when f(v) == 0
        failed when f(v) == 1
        produces switch {
            when f(v) > 3 => test_out out { v: f(v) }
            else => test_out out { v: f(0) }
        }
    """
    expanded = statements.replace("f(a.msg.value)", "(a.msg.value + 1)")
    expanded = expanded.replace("f(v)", "(v + 1)").replace("f(0)", "(0 + 1)")
    assert _fn_artifact("fn f(x) = x + 1", statements) == _fn_artifact("", expanded)


@pytest.mark.parametrize("pattern, rules", [
    ("op 1 @a where (EXPR)", ""),
    ("op 1 @a -> op 2 where (EXPR)", ""),
    ("op 1 @a -> $r", "rule r = op 2 where (EXPR)"),
    ("op 1 @a { -> { op 2 where (EXPR) } }", ""),
    ("op 1 @a { -> op 2 where (EXPR) }", ""),
    ("op 1 @a { <- op 2 where (EXPR) }", ""),
])
def test_fn_in_where_emits_expanded_expression(pattern, rules):
    artifacts = []
    for declarations, expr in (
        ("fn f(x) = len(x) > 0", "f(a.msg.value)"),
        ("", "len(a.msg.value) > 0"),
    ):
        artifacts.append(_fn_artifact(
            declarations, "out { v: 1 }", pattern.replace("EXPR", expr), rules.replace("EXPR", expr),
        ))
    assert artifacts[0] == artifacts[1]


@pytest.mark.parametrize("body, arg, code", [
    ("len(x) > 0", "b.msg.value", "C015_WHERE_EXPR_CAPTURE_NOT_ENTRY"),
    ("host_fn(x)", "a", "C013_WHERE_EXPR_NOT_SYNC"),
    ("parse! x as T", "a", "C013_WHERE_EXPR_NOT_SYNC"),
    ("x", "", "R036_FN_ARITY"),
])
def test_fn_in_where_keeps_existing_checks(body, arg, code):
    with pytest.raises(CompileError) as exc:
        _fn_artifact(
            f"fn f(x) = {body}", "out { v: 1 }",
            f"op 1 @a -> op 2 @b where (f({arg}))",
        )
    assert [d.code for d in exc.value.bag.items] == [code]
