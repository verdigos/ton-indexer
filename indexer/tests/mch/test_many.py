import pytest

from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.ir_emit import build_stub_registries, emit_ir
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse


def emit(pattern, rules="", build="out { v: 1 }"):
    src = f"""{rules}
    matcher m {{
        produces fb
        {pattern}
        {build}
    }}"""
    reg = build_stub_registries([parse(tokenize(src, "test.mch"), "test.mch")])
    return emit_ir(compile_text(src, "test.mch", reg))


@pytest.mark.parametrize("pattern,rules,message", [
    ("many op 1 @x", "", "children-set item"),
    ("op 1 @x -> many op 2", "", "children-set item"),
    ("op 1 @x <- many op 2", "", "children-set item"),
    ("op 1 @x -> { or { many op 2; op 3 } }", "", "children-set item"),
    ("op 1 @x -> { op 2 -> many op 3 }", "", "children-set item"),
    ("op 1 @x -> { many op 2 -> op 3 @ys }", "", "repeated block"),
    ("op 1 @x -> { many op 2 { -> op 3 @ys } }", "", "repeated block"),
    ("op 1 @x -> { many op 2 -> { many op 3 @ys } }", "", "repeated block"),
    ("op 1 @x -> { many maybe op 2 }", "", "write many[0..] / many[0..N]"),
    ("op 1 @x -> { many $r }", "rule r = maybe op 2", "write many[0..] / many[0..N]"),
    ("op 1 @x -> { many or { maybe op 2 @ys; op 3 } }", "", "repeated block"),
    ("op 1 @x -> { many $r }", "rule r = op 2 -> maybe $r", "recursive rule"),
    ("op 1 @x -> $r -> { many op 3 }", "rule r = op 2 -> maybe $r", "recursive rule"),
    ("op 1 @x -> $r", "rule r = op 2 -> { many op 3; maybe $r }", "recursive rule"),
    ("op 1 @x -> { many $s }", "rule s = $r\nrule r = op 2 -> maybe $r", "recursive rule"),
    ("op 1 @x -> { many[3] op 2 }", "", "write many[0..] / many[0..N]"),
    ("op 1 @x -> { maybe many op 2 }", "", "write many[0..] / many[0..N]"),
    ("op 1 @x -> { maybe peek many op 2 }", "", "write many[0..] / many[0..N]"),
    ("op 1 @x -> { many[2..3] op 2 }", "", "lower bound above 1 is not supported yet; it arrives with its first consumer"),
    ("op 1 @x -> { many[3..1] op 2 }", "", "n <= k"),
])
def test_many_diagnostics(pattern, rules, message):
    with pytest.raises(CompileError) as exc:
        emit(pattern, rules)
    assert any(d.code == "C017_MANY" and message in d.message for d in exc.value.bag.items), exc.value.bag.items


@pytest.mark.parametrize("pattern", [
    "op 1 @x -> { peek many op 2 }",
    "op 1 @x -> { many peek op 2 }",
])
def test_peek_many_lowers_to_one_node(pattern):
    doc = emit(pattern)
    root = doc["nodes"][doc["matchers"][0]["root"]]
    repeated = doc["nodes"][root["children"][0]]
    assert repeated["many"] is True and repeated["peek"] is True


def test_peek_many_cap_and_optional():
    doc = emit("op 1 @x -> { peek many[1..2] op 2 }")
    root = doc["nodes"][doc["matchers"][0]["root"]]
    repeated = doc["nodes"][root["children"][0]]
    assert repeated["many"] is True and repeated["peek"] is True and repeated["max"] == 2
    assert "optional" not in repeated
    doc = emit("op 1 @x -> { peek many[0..] op 2 }")
    root = doc["nodes"][doc["matchers"][0]["root"]]
    repeated = doc["nodes"][root["children"][0]]
    assert repeated["many"] is True and repeated["peek"] is True and repeated["optional"] is True
    assert "max" not in repeated


def test_peek_many_consuming_descendant():
    with pytest.raises(CompileError) as exc:
        emit("op 1 @x -> { peek many op 2 -> { op 3 } }")
    assert any(d.code == "C014_PEEK_SUBTREE_CONSUMES" for d in exc.value.bag.items)


def test_many_capture_context_agreement():
    with pytest.raises(CompileError) as exc:
        emit("op 1 @x -> or { op 2 -> { many op 3 @ys }; op 4 @ys }")
    assert any(d.code == "R005_DUP_CAPTURE" for d in exc.value.bag.items)


@pytest.mark.parametrize("item,rules", [
    ("many[\n0..2\n] op [2, 3] @ys", ""),
    ("many[0..2] $r", "rule r = xor { op 2 @ys; op 3 @ys }"),
])
def test_many_shared_cap_and_list_capture(item, rules):
    doc = emit(f"op 1 @x {{ -> {{ {item} }} }}", rules)
    root = doc["nodes"][doc["matchers"][0]["root"]]
    repeated = doc["nodes"][root["children"][0]]
    assert repeated["many"] is True and repeated["max"] == 2 and repeated["optional"] is True
    assert repeated["kind"] == "or" and repeated["exclusive"] is True
    assert all("many" not in doc["nodes"][b] for b in repeated["branches"])
    assert {c["name"]: c["card"] for c in doc["matchers"][0]["captures"]}["ys"] == "many"


def test_many_range_lowering():
    doc = emit("op 1 @x -> { many[0..2] op 2 }")
    root = doc["nodes"][doc["matchers"][0]["root"]]
    repeated = doc["nodes"][root["children"][0]]
    assert repeated["many"] is True and repeated["optional"] is True and repeated["max"] == 2
    bare = emit("op 1 @x -> { many op 2 }")
    open_hi = emit("op 1 @x -> { many[1..] op 2 }")
    assert bare == open_hi
    root = open_hi["nodes"][open_hi["matchers"][0]["root"]]
    repeated = open_hi["nodes"][root["children"][0]]
    assert repeated["many"] is True
    assert "optional" not in repeated and "max" not in repeated


def test_many_contextual_identifier_and_uncapped_emission():
    doc = emit("op 1 @x -> { many op 2 @many }", build="out { xs: many }")
    assert any(n.get("many") and "max" not in n for n in doc["nodes"])
    doc = emit("op 1 @x", build="let many = 1\nout { n: many }")
    assert all("many" not in n and "max" not in n for n in doc["nodes"])


def test_many_annotation_and_uncaptured_nested_items():
    doc = emit("op 1 @x -> { many op 2 @ys as Msg -> { many op 3; peek op 4 } }")
    assert sum(n.get("many", False) for n in doc["nodes"]) == 2
    assert doc["matchers"][0]["build_program"][0]["types"] == ["Msg"]
