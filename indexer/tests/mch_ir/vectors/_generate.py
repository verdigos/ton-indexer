"""Generator for expr_vectors.json (T42 expression conformance vectors).

Run:  python -m tests.mch_ir.vectors._generate     (from the indexer app root)

Each vector's `expr` is produced by the REAL frontend (parse `let e = <src>` ->
IR encoding), so the vectors carry exactly the IR the emitter emits. `env` and
`expect` are authored here in the tagged value-encoding convention documented in
docs/mch-ir-spec.md §12. The committed JSON is the language-neutral contract;
this script only regenerates it.
"""
from __future__ import annotations

import base64
import json
from pathlib import Path

from pytoniq_core import begin_cell

from indexer.events.mch import ast_
from indexer.events.mch.ir_emit import _emit_expr
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse

RAW = "0:" + "ab" * 32          # has hex letters -> exercises case-insensitive compares
RAW2 = "0:" + "cd" * 32
_SOME_BYTES = b"\x01\x02\x03hello"
_SOME_BYTES_B64 = base64.b64encode(_SOME_BYTES).decode()
# A small cell; b64(Cell) must BOC-encode it first.
_CELL = begin_cell().store_uint(0xDEADBEEF, 32).store_uint(7, 8).end_cell()
_CELL_BOC_B64 = base64.b64encode(_CELL.to_boc()).decode()


def _ir(src: str) -> dict:
    file_src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n  build b\n  entry @x\n\n  op Foo @x\n"
        f"  let e = {src}\n}}"
    )
    f = parse(list(tokenize(file_src, "t.mch")), "t.mch")
    let = f.matchers[0].build_stmts[-1]
    assert isinstance(let, ast_.LetStmt), src
    return _emit_expr(let.value)


FAULT = {"$fault": True}

# (name, source-expr, env, expect [, lookups])
_V: list[tuple] = [
    # --- literals ---
    ("int_lit", "42", {}, 42),
    ("hex_lit", "0xff", {}, 255),
    ("str_lit", '"hi"', {}, "hi"),
    ("bool_true", "true", {}, True),
    ("bool_false", "false", {}, False),
    ("null_lit", "null", {}, None),
    # --- name ---
    ("name_bound", "a", {"a": 7}, 7),
    ("name_unbound_fault", "a", {}, FAULT),
    # --- attr / accessors ---
    ("attr_obj_field", "o.x", {"o": {"$obj": {"x": 9}}}, 9),
    ("attr_chain", "o.a.b", {"o": {"$obj": {"a": {"$obj": {"b": 3}}}}}, 3),
    ("attr_on_null", "o.x", {"o": None}, None),
    ("attr_dict_key", "d.k", {"d": {"$dict": {"k": 5}}}, 5),
    ("attr_dict_missing_fault", "d.k", {"d": {"$dict": {"other": 1}}}, FAULT),
    ("exit_code", "m.exit_code",
     {"m": {"$obj": {"transaction": {"$obj": {"compute_exit_code": 123}}}}}, 123),
    ("exit_code_absent_field", "m.exit_code",
     {"m": {"$obj": {"transaction": {"$obj": {}}}}}, None),
    ("exit_code_no_tx_fault", "m.exit_code", {"m": {"$obj": {}}}, FAULT),
    # leading-dot `.field` is where-clause-only; in a build expression it faults.
    ("dotfield_in_build_fault", ".value", {}, FAULT),
    # --- constructors ---
    ("account_str", "account(a)", {"a": RAW}, {"$account": RAW}),
    ("account_null", "account(a)", {"a": None}, None),
    ("addr_none", "addr_none()", {}, {"$account": None}),
    ("amount_int", "amount(a)", {"a": 50}, {"$amount": 50}),
    ("amount_amount", "amount(a)", {"a": {"$amount": 50}}, {"$amount": 50}),
    # `.value` unwrap (F2): the inverse of amount(x). Amount(None).value -> null.
    ("amount_value", "a.value", {"a": {"$amount": 50}}, 50),
    ("amount_value_null", "a.value", {"a": {"$amount": None}}, None),
    ("amount_value_bad_accessor_fault", "a.foo", {"a": {"$amount": 50}}, FAULT),
    ("asset_jetton", "asset(a)", {"a": RAW}, {"$asset_jetton": RAW}),
    ("ton_asset", "ton_asset()", {}, {"$asset_ton": True}),
    ("b64_bytes", "b64(a)", {"a": {"$bytes_b64": _SOME_BYTES_B64}}, _SOME_BYTES_B64),
    ("b64_cell", "b64(a)", {"a": {"$cell_b64": _CELL_BOC_B64}}, _CELL_BOC_B64),
    # --- list combinators ---
    ("first_nonempty", "first(xs)", {"xs": {"$list": [10, 20]}}, 10),
    ("first_empty", "first(xs)", {"xs": {"$list": []}}, None),
    ("first_null", "first(xs)", {"xs": None}, None),
    ("last_nonempty", "last(xs)", {"xs": {"$list": [10, 20]}}, 20),
    ("len_list", "len(xs)", {"xs": {"$list": [1, 2, 3]}}, 3),
    ("len_empty", "len(xs)", {"xs": {"$list": []}}, 0),
    ("len_null", "len(xs)", {"xs": None}, None),
    ("len_nonlist_fault", "len(xs)", {"xs": 5}, FAULT),
    ("sum_ints", "sum(xs)", {"xs": {"$list": [1, 2, 3]}}, 6),
    ("sum_amounts", "sum(xs)", {"xs": {"$list": [{"$amount": 10}, {"$amount": 5}]}}, 15),
    ("sum_empty", "sum(xs)", {"xs": {"$list": []}}, 0),
    ("sum_mixed_fault", "sum(xs)", {"xs": {"$list": [1, "x"]}}, FAULT),
    ("zip_equal", "zip(xs, ys)", {"xs": {"$list": [1, 2]}, "ys": {"$list": [3, 4]}},
     {"$list": [{"$list": [1, 3]}, {"$list": [2, 4]}]}),
    ("zip_mismatch", "zip(xs, ys)", {"xs": {"$list": [1, 2, 3]}, "ys": {"$list": [9]}},
     {"$list": [{"$list": [1, 9]}]}),
    ("map_field", 'map(xs, "a")', {"xs": {"$list": [{"a": 1}, {"a": 2}]}}, {"$list": [1, 2]}),
    ("map_missing_fault", 'map(xs, "z")', {"xs": {"$list": [{"a": 1}]}}, FAULT),
    ("map_empty", 'map(xs, "a")', {"xs": {"$list": []}}, {"$list": []}),
    ("sum_of_map", 'sum(map(xs, "a"))', {"xs": {"$list": [{"a": 3}, {"a": 4}]}}, 7),
    # --- unary ---
    ("not_true", "not a", {"a": True}, False),
    ("not_false", "not a", {"a": False}, True),
    ("not_nonbool_fault", "not a", {"a": 5}, FAULT),
    ("neg_int", "-a", {"a": 5}, -5),
    ("neg_null", "-a", {"a": None}, None),
    # --- binary comparisons ---
    ("eq_true", "a == b", {"a": 3, "b": 3}, True),
    ("eq_false", "a == b", {"a": 3, "b": 4}, False),
    ("eq_null_null", "a == b", {"a": None, "b": None}, True),
    ("eq_null_val", "a == b", {"a": None, "b": 3}, False),
    ("ne_true", "a != b", {"a": 3, "b": 4}, True),
    ("asset_eq_string_ci", "a == b", {"a": {"$asset_jetton": RAW}, "b": RAW.upper()}, True),
    ("lt_true", "a < b", {"a": 3, "b": 4}, True),
    ("le_eq", "a <= b", {"a": 4, "b": 4}, True),
    ("gt_false", "a > b", {"a": 3, "b": 4}, False),
    ("ge_true", "a >= b", {"a": 4, "b": 4}, True),
    ("ord_null_false", "a < b", {"a": None, "b": 4}, False),
    ("ord_nonorder_fault", "a < b", {"a": "x", "b": 3}, FAULT),
    # --- logical + coalesce (short-circuit) ---
    ("and_true", "a and b", {"a": True, "b": True}, True),
    ("and_short_circuit", "a and b", {"a": False, "b": 5}, False),
    ("or_true_short_circuit", "a or b", {"a": True, "b": 5}, True),
    ("coalesce_left", "a ?? b", {"a": 7, "b": 9}, 7),
    ("coalesce_right", "a ?? b", {"a": None, "b": 9}, 9),
    ("coalesce_chain", "a ?? b ?? c", {"a": None, "b": None, "c": 3}, 3),
    # --- binary arithmetic (G-ARITH) ---
    ("add", "a + b", {"a": 3, "b": 4}, 7),
    ("sub", "a - b", {"a": 10, "b": 3}, 7),
    ("mul", "a * b", {"a": 6, "b": 7}, 42),
    ("precedence_mul_over_add", "a + b * c", {"a": 2, "b": 3, "c": 4}, 14),
    ("precedence_arith_over_cmp", "a + b == c", {"a": 2, "b": 3, "c": 5}, True),
    ("sub_left_assoc", "a - b - c", {"a": 10, "b": 3, "c": 2}, 5),
    ("mul_then_sub", "a * b - c", {"a": 2, "b": 3, "c": 1}, 5),
    ("neg_operand_in_sub", "a - -b", {"a": 5, "b": 3}, 8),
    ("arith_null_prop", "a + b", {"a": None, "b": 3}, None),
    ("arith_amount_coerce", "a - b", {"a": {"$amount": 10}, "b": {"$amount": 3}}, 7),
    ("arith_amount_mixed", "a + b", {"a": {"$amount": 10}, "b": 5}, 15),
    ("arith_noninteger_fault", "a + b", {"a": "x", "b": 3}, FAULT),
    ("arith_bool_fault", "a + b", {"a": True, "b": 3}, FAULT),
    # --- ternary ---
    ("ternary_then", "x if c else y", {"c": True, "x": 1, "y": 2}, 1),
    ("ternary_else", "x if c else y", {"c": False, "x": 1, "y": 2}, 2),
    ("ternary_cond_nonbool_fault", "x if c else y", {"c": 5, "x": 1, "y": 2}, FAULT),
    # --- literals: list / record ---
    ("list_empty", "[]", {}, {"$list": []}),
    ("list_vals", "[1, 2, 3]", {}, {"$list": [1, 2, 3]}),
    ("list_env", "[a, b]", {"a": 7, "b": 8}, {"$list": [7, 8]}),
    ("record_empty", "{}", {}, {}),
    ("record_vals", "{x: 1, y: 2}", {}, {"x": 1, "y": 2}),
    ("record_nested", "{legs: [1, 2], meta: {n: 3}}", {},
     {"legs": {"$list": [1, 2]}, "meta": {"n": 3}}),
    # --- lookup ---
    ("lookup_field", "(lookup jetton_wallet(a)).owner", {"a": RAW}, RAW2,
     {"jetton_wallet": {"$obj": {"owner": RAW2, "jetton": RAW}}}),
    ("lookup_null_arg", "lookup jetton_wallet(a)", {"a": None}, None,
     {"jetton_wallet": {"$obj": {"owner": RAW2}}}),
]


def build() -> dict:
    vectors = []
    for row in _V:
        name, src, env, expect = row[0], row[1], row[2], row[3]
        rec = {"name": name, "src": src, "expr": _ir(src), "env": env, "expect": expect}
        if len(row) > 4:
            rec["lookups"] = row[4]
        vectors.append(rec)
    return {
        "_readme": "Expression conformance vectors (T42). Value-encoding convention: "
                   "docs/mch-ir-spec.md §12. `expr` is IR (docs/mch-ir-spec.md §7). "
                   "Regenerate with tests/mch_ir/vectors/_generate.py.",
        "vectors": vectors,
    }


if __name__ == "__main__":
    out = Path(__file__).parent / "expr_vectors.json"
    out.write_text(json.dumps(build(), indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {len(build()['vectors'])} vectors -> {out}")
