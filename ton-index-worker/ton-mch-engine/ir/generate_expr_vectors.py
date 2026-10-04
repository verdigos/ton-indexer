"""Generator for expr_vectors.json (T42 expression conformance vectors).

Run:  PYTHONPATH=indexer python ton-index-worker/ton-mch-engine/ir/generate_expr_vectors.py <out.json>

Run from the repository root; PYTHONPATH points at the directory holding the
`indexer` package. The output path is the first argument, so the build-tree
sync gate can regenerate the corpus anywhere.

Each vector's `expr` is produced by the REAL frontend (parse `let e = <src>` ->
IR encoding), so the vectors carry exactly the IR the emitter emits. `env` and
`expect` are authored here in the tagged value-encoding convention documented in
docs/mch-ir-spec.md §12. The committed JSON is the language-neutral contract;
this script only regenerates it.
"""
from __future__ import annotations

import base64
import json
import sys
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
_CELL_BYTES_B64 = base64.b64encode(b"\xDE\xAD\xBE\xEF\x07").decode()
_EMPTY_CELL = begin_cell().end_cell()
_EMPTY_CELL_BOC_B64 = base64.b64encode(_EMPTY_CELL.to_boc()).decode()
_REF_ONLY_CELL = begin_cell().store_ref(_EMPTY_CELL).end_cell()
_REF_ONLY_CELL_BOC_B64 = base64.b64encode(_REF_ONLY_CELL.to_boc()).decode()
_BIT_ONLY_CELL = begin_cell().store_uint(1, 1).end_cell()
_BIT_ONLY_CELL_BOC_B64 = base64.b64encode(_BIT_ONLY_CELL.to_boc()).decode()
_HASH_CELL = begin_cell().store_bytes(bytes.fromhex("ab" * 32)).end_cell()
_HASH_CELL_BOC_B64 = base64.b64encode(_HASH_CELL.to_boc()).decode()
_SHORT_HASH_CELL = begin_cell().store_uint(0, 255).end_cell()
_SHORT_HASH_CELL_BOC_B64 = base64.b64encode(_SHORT_HASH_CELL.to_boc()).decode()
_REF_HASH_CELL = begin_cell().store_bytes(bytes.fromhex("ab" * 32)).store_ref(_EMPTY_CELL).end_cell()
_REF_HASH_CELL_BOC_B64 = base64.b64encode(_REF_HASH_CELL.to_boc()).decode()
_ALIGNED_REF_CELL = begin_cell().store_uint(0xAB, 8).store_ref(_EMPTY_CELL).end_cell()
_ALIGNED_REF_CELL_BOC_B64 = base64.b64encode(_ALIGNED_REF_CELL.to_boc()).decode()
_JETTON_RAW = "-1:" + "AB" * 32


def _ir(src: str) -> dict:
    file_src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n\n  op Foo @x\n"
        f"  let e = {src}\n  out {{ v: 1 }}\n}}"
    )
    f = parse(list(tokenize(file_src, "t.mch")), "t.mch")
    let = f.matchers[0].build_stmts[0]
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
    ("msg_exit_code_gone", "m.exit_code",
     {"m": {"$obj": {"transaction": {"$obj": {"compute_exit_code": 123}}}}}, FAULT),
    # --- optional record fields ---
    ("get_dict_hit", 'get(d, "k")', {"d": {"$dict": {"k": 5}}}, 5),
    ("get_dict_miss", 'get(d, "k")', {"d": {"$dict": {}}}, None),
    ("get_obj_hit", 'get(o, "x")', {"o": {"$obj": {"x": 9}}}, 9),
    ("get_obj_miss", 'get(o, "x")', {"o": {"$obj": {}}}, None),
    ("get_null_record", 'get(null, "k")', {}, None),
    ("get_null_key", 'get({k: 5}, null)', {}, None),
    ("get_key_not_str_fault", "get({}, 1)", {}, FAULT),
    ("get_on_list_fault", 'get([], "k")', {}, FAULT),
    ("get_on_int_fault", 'get(1, "k")', {}, FAULT),
    ("get_on_amount_fault", 'get(a, "k")', {"a": {"$amount": 1}}, FAULT),
    ("get_present_false", 'get(d, "k")', {"d": {"$dict": {"k": False}}}, False),
    ("get_present_null", 'get(d, "k")', {"d": {"$dict": {"k": None}}}, None),
    ("get_chain_coalesce", 'get(get(o, "content"), "uri") ?? ""',
     {"o": {"$obj": {}}}, ""),
    ("get_in_contains", 'contains(get(get(o, "content"), "uri") ?? "", "https://nft.fragment.com")',
     {"o": {"$obj": {"content": {"$dict": {"uri": "https://nft.fragment.com/x"}}}}}, True),
    # --- transaction values ---
    ("tx_aborted_true", "tx_aborted(m)",
     {"m": {"$obj": {"transaction": {"$obj": {"aborted": True}}}}}, True),
    ("tx_aborted_false", "tx_aborted(m)",
     {"m": {"$obj": {"transaction": {"$obj": {"aborted": False}}}}}, False),
    ("tx_aborted_tx_null", "tx_aborted(m)", {"m": {"$obj": {"transaction": None}}}, None),
    ("tx_aborted_tx_absent", "tx_aborted(m)", {"m": {"$obj": {}}}, None),
    ("tx_aborted_null", "tx_aborted(null)", {}, None),
    ("tx_aborted_on_int_fault", "tx_aborted(1)", {}, FAULT),
    ("exit_code_val", "exit_code(m)",
     {"m": {"$obj": {"transaction": {"$obj": {"compute_exit_code": 123}}}}}, 123),
    ("exit_code_tx_null", "exit_code(m)", {"m": {"$obj": {"transaction": None}}}, None),
    ("exit_code_absent_ec", "exit_code(m)",
     {"m": {"$obj": {"transaction": {"$obj": {}}}}}, None),
    ("exit_code_on_str_fault", 'exit_code("x")', {}, FAULT),
    # leading-dot `.field` is where-clause-only; in a build expression it faults.
    ("dotfield_in_build_fault", ".value", {}, FAULT),
    # --- constructors ---
    ("account_str", "account(a)", {"a": RAW}, {"$account": RAW}),
    ("account_null", "account(a)", {"a": None}, None),
    ("account_eq_str_neg_wc", f'account("-1:{"33" * 32}") == "-1:{"33" * 32}"', {}, True),
    ("account_idempotent", "account(account(a)) == account(a)", {"a": RAW}, True),
    ("addr_none", "addr_none()", {}, {"$account": None}),
    ("amount_int", "amount(a)", {"a": 50}, {"$amount": 50}),
    ("amount_amount", "amount(a)", {"a": {"$amount": 50}}, {"$amount": 50}),
    ("amount_str_fault", 'amount("not-a-number")', {}, FAULT),
    # `.value` unwrap (F2): the inverse of amount(x). Amount(None).value -> null.
    ("amount_value", "a.value", {"a": {"$amount": 50}}, 50),
    ("amount_value_null", "a.value", {"a": {"$amount": None}}, None),
    ("amount_value_bad_accessor_fault", "a.foo", {"a": {"$amount": 50}}, FAULT),
    # Float-backed Amount (render-only, getgems price): the language has no
    # floats, so any USE faults — `.value` unwrap, `==`, arithmetic (§4.2).
    ("amount_float_value_fault", "a.value", {"a": {"$amount_float": 1.5}}, FAULT),
    ("amount_float_arith_fault", "a + b",
     {"a": {"$amount_float": 1.5}, "b": 1}, FAULT),
    ("asset_jetton", "asset(a)", {"a": RAW}, {"$asset_jetton": RAW}),
    ("ton_asset", "ton_asset()", {}, {"$asset_ton": True}),
    ("b64_bytes", "b64(a)", {"a": {"$bytes_b64": _SOME_BYTES_B64}}, _SOME_BYTES_B64),
    ("b64_cell", "b64(a)", {"a": {"$cell_b64": _CELL_BOC_B64}}, _CELL_BOC_B64),
    # --- ABI-faithful conversion builtins (F0; production specs do not use these yet) ---
    ("asset_of_ton", "asset_of(x)",
     {"x": {"$obj": {"$": "AssetTon"}}}, {"$asset_ton": True}),
    ("asset_of_jetton", "asset_of(x)",
     {"x": {"$obj": {"$": "AssetJetton", "workchain": -1,
                       "hash": {"$cell_b64": _HASH_CELL_BOC_B64}}}},
     {"$asset_jetton": _JETTON_RAW}),
    ("asset_of_null", "asset_of(x)", {"x": None}, None),
    ("asset_of_bad_tag_fault", "asset_of(x)",
     {"x": {"$obj": {"$": "UnknownAsset"}}}, FAULT),
    ("asset_of_nonobj_fault", "asset_of(x)", {"x": 7}, FAULT),
    ("asset_of_short_hash_fault", "asset_of(x)",
     {"x": {"$obj": {"$": "AssetJetton", "workchain": 0,
                       "hash": {"$cell_b64": _SHORT_HASH_CELL_BOC_B64}}}}, FAULT),
    ("asset_of_ref_hash_fault", "asset_of(x)",
     {"x": {"$obj": {"$": "AssetJetton", "workchain": 0,
                       "hash": {"$cell_b64": _REF_HASH_CELL_BOC_B64}}}}, FAULT),
    ("tail_unwrap_jetton_ref", "tail_unwrap(x)",
     {"x": {"$obj": {"$": "JettonForwardPayloadRef",
                       "value": {"$cell_b64": _EMPTY_CELL_BOC_B64}}}},
     {"$cell_b64": _EMPTY_CELL_BOC_B64}),
    ("tail_unwrap_jetton_inline_nonempty", "tail_unwrap(x)",
     {"x": {"$obj": {"$": "JettonForwardPayloadInline",
                       "value": {"$cell_b64": _BIT_ONLY_CELL_BOC_B64}}}},
     {"$cell_b64": _BIT_ONLY_CELL_BOC_B64}),
    ("tail_unwrap_jetton_inline_empty", "tail_unwrap(x)",
     {"x": {"$obj": {"$": "JettonForwardPayloadInline",
                       "value": {"$cell_b64": _REF_ONLY_CELL_BOC_B64}}}}, None),
    ("tail_unwrap_pton_ref", "tail_unwrap(x)",
     {"x": {"$obj": {"$": "PTonForwardPayloadRef",
                       "value": {"$cell_b64": _EMPTY_CELL_BOC_B64}}}},
     {"$cell_b64": _EMPTY_CELL_BOC_B64}),
    ("tail_unwrap_pton_inline_nonempty", "tail_unwrap(x)",
     {"x": {"$obj": {"$": "PTonForwardPayloadInline",
                       "value": {"$cell_b64": _REF_ONLY_CELL_BOC_B64}}}},
     {"$cell_b64": _REF_ONLY_CELL_BOC_B64}),
    ("tail_unwrap_pton_inline_empty", "tail_unwrap(x)",
     {"x": {"$obj": {"$": "PTonForwardPayloadInline",
                       "value": {"$cell_b64": _BIT_ONLY_CELL_BOC_B64}}}}, None),
    ("tail_unwrap_null", "tail_unwrap(x)", {"x": None}, None),
    ("tail_unwrap_bad_tag_fault", "tail_unwrap(x)",
     {"x": {"$obj": {"$": "UnknownTail", "value": {"$cell_b64": _CELL_BOC_B64}}}},
     FAULT),
    ("tail_unwrap_nonobj_fault", "tail_unwrap(x)", {"x": 7}, FAULT),
    ("tail_unwrap_missing_value_fault", "tail_unwrap(x)",
     {"x": {"$obj": {"$": "JettonForwardPayloadRef"}}}, FAULT),
    ("tail_unwrap_noncell_value_fault", "tail_unwrap(x)",
     {"x": {"$obj": {"$": "PTonForwardPayloadInline", "value": 7}}}, FAULT),
    ("bytes_of_bytes", "bytes_of(x)",
     {"x": {"$bytes_b64": _SOME_BYTES_B64}}, {"$bytes_b64": _SOME_BYTES_B64}),
    ("bytes_of_aligned_cell", "bytes_of(x)",
     {"x": {"$cell_b64": _CELL_BOC_B64}}, {"$bytes_b64": _CELL_BYTES_B64}),
    ("bytes_of_null", "bytes_of(x)", {"x": None}, None),
    ("bytes_of_wrong_type_fault", "bytes_of(x)", {"x": 7}, FAULT),
    ("bytes_of_unaligned_cell_fault", "bytes_of(x)",
     {"x": {"$cell_b64": _BIT_ONLY_CELL_BOC_B64}}, FAULT),
    ("bytes_of_ref_cell_fault", "bytes_of(x)",
     {"x": {"$cell_b64": _ALIGNED_REF_CELL_BOC_B64}}, FAULT),
    # --- list combinators ---
    ("first_nonempty", "first(xs)", {"xs": {"$list": [10, 20]}}, 10),
    ("first_empty", "first(xs)", {"xs": {"$list": []}}, None),
    ("first_null", "first(xs)", {"xs": None}, None),
    ("last_nonempty", "last(xs)", {"xs": {"$list": [10, 20]}}, 20),
    ("len_list", "len(xs)", {"xs": {"$list": [1, 2, 3]}}, 3),
    ("len_empty", "len(xs)", {"xs": {"$list": []}}, 0),
    ("len_null", "len(xs)", {"xs": None}, None),
    ("len_nonlist_fault", "len(xs)", {"xs": 5}, FAULT),
    ("zip_equal", "zip(xs, ys)", {"xs": {"$list": [1, 2]}, "ys": {"$list": [3, 4]}},
     {"$list": [{"$list": [1, 3]}, {"$list": [2, 4]}]}),
    ("zip_mismatch", "zip(xs, ys)", {"xs": {"$list": [1, 2, 3]}, "ys": {"$list": [9]}},
     {"$list": [{"$list": [1, 9]}]}),
    ("concat_lists", "concat(xs, ys)", {"xs": {"$list": [1, 2]}, "ys": {"$list": [3]}},
     {"$list": [1, 2, 3]}),
    ("concat_empty_left", "concat(xs, ys)", {"xs": {"$list": []}, "ys": {"$list": [7]}},
     {"$list": [7]}),
    ("concat_null_arg", "concat(xs, ys)", {"xs": {"$list": [1]}, "ys": None}, None),
    ("concat_nonlist_fault", "concat(xs, ys)", {"xs": {"$list": [1]}, "ys": 5}, FAULT),
    # --- string test (the language's ONLY string op) ---
    ("contains_true", "contains(s, t)", {"s": "https://nft.fragment.com/x", "t": "fragment.com"},
     True),
    ("contains_false", "contains(s, t)", {"s": "https://getgems.io/x", "t": "fragment.com"}, False),
    ("contains_empty_needle", "contains(s, t)", {"s": "abc", "t": ""}, True),
    ("contains_case_sensitive", "contains(s, t)", {"s": "ABC", "t": "abc"}, False),
    ("contains_null_arg", "contains(s, t)", {"s": None, "t": "a"}, None),
    ("contains_null_needle", "contains(s, t)", {"s": "abc", "t": None}, None),
    ("contains_nonstr_fault", "contains(s, t)", {"s": 5, "t": "a"}, FAULT),
    ("contains_nonstr_needle_fault", "contains(s, t)", {"s": "abc", "t": 5}, FAULT),
    ("map_field", 'map(xs, "a")', {"xs": {"$list": [{"a": 1}, {"a": 2}]}}, {"$list": [1, 2]}),
    ("map_missing_fault", 'map(xs, "z")', {"xs": {"$list": [{"a": 1}]}}, FAULT),
    ("map_empty", 'map(xs, "a")', {"xs": {"$list": []}}, {"$list": []}),
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
    ("asset_eq_str_ton", "a == b", {"a": {"$asset_ton": True}, "b": RAW}, False),
    ("asset_ne_str_ton", "a != b", {"a": {"$asset_ton": True}, "b": RAW}, True),
    ("asset_eq_str_ton_reversed", "a == b", {"a": RAW, "b": {"$asset_ton": True}}, False),
    ("asset_eq_str_ton_literal", "a == b", {"a": {"$asset_ton": True}, "b": "TON"}, False),
    ("asset_eq_str_jetton_miss", "a == b", {"a": {"$asset_jetton": RAW}, "b": RAW2}, False),
    ("asset_eq_same_kind", "a == b", {"a": {"$asset_ton": True}, "b": {"$asset_ton": True}}, True),
    ("asset_eq_diff_kind", "a == b", {"a": {"$asset_ton": True}, "b": {"$asset_jetton": RAW}}, False),
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
    # --- G-LC map comprehension (§16.1) ---
    ("comp_map_captured_list", "map(xs as s => s + 1)",
     {"xs": {"$list": [10, 20]}}, {"$list": [11, 21]}),
    ("comp_map_record_body", "map(xs as s => {v: s.a})",
     {"xs": {"$list": [{"a": 1}, {"a": 2}]}},
     {"$list": [{"v": 1}, {"v": 2}]}),
    ("comp_map_account", "map(xs as a => account(a))",
     {"xs": {"$list": [RAW, RAW2]}},
     {"$list": [{"$account": RAW}, {"$account": RAW2}]}),
    ("comp_map_empty", "map(xs as s => s + 1)", {"xs": {"$list": []}}, {"$list": []}),
    ("comp_map_null", "map(xs as s => s + 1)", {"xs": None}, None),
    ("comp_map_nonlist_fault", "map(xs as s => s)", {"xs": 5}, FAULT),
    # binder shadowing: `s` inside the body is the element, and the outer `s`
    # binding is restored afterward (list evaluates the comprehension first).
    ("comp_binder_shadow", "[map(xs as s => s), s]",
     {"s": 99, "xs": {"$list": [1, 2]}}, {"$list": [{"$list": [1, 2]}, 99]}),
    # --- G-Q any/all quantifiers (§16.1) ---
    ("comp_any_true", "any(xs as s => s > 2)", {"xs": {"$list": [1, 3]}}, True),
    ("comp_any_false", "any(xs as s => s > 5)", {"xs": {"$list": [1, 3]}}, False),
    ("comp_any_empty", "any(xs as s => s > 0)", {"xs": {"$list": []}}, False),
    ("comp_any_null", "any(xs as s => s > 0)", {"xs": None}, None),
    ("comp_all_true", "all(xs as s => s > 0)", {"xs": {"$list": [1, 3]}}, True),
    ("comp_all_false", "all(xs as s => s > 2)", {"xs": {"$list": [1, 3]}}, False),
    ("comp_all_empty", "all(xs as s => s > 0)", {"xs": {"$list": []}}, True),
    ("comp_all_null", "all(xs as s => s > 0)", {"xs": None}, None),
    ("comp_quant_nonlist_fault", "any(xs as s => s > 0)", {"xs": 5}, FAULT),
    ("comp_quant_body_nonbool_fault", "any(xs as s => s)", {"xs": {"$list": [1]}}, FAULT),
    # --- find ---
    ("find_empty", "find(xs as s => s > 0)", {"xs": {"$list": []}}, None),
    ("find_miss", "find(xs as s => s > 5)", {"xs": {"$list": [1, 3]}}, None),
    ("find_null_xs", "find(xs as s => s > 0)", {"xs": None}, None),
    ("find_nonlist_fault", "find(xs as s => s > 0)", {"xs": 5}, FAULT),
    ("find_pred_nonbool_fault", "find(xs as s => s)", {"xs": {"$list": [1]}}, FAULT),
    ("find_first_match", "find(xs as s => s > 2)", {"xs": {"$list": [1, 3, 4]}}, 3),
    ("find_early_exit", "find(xs as s => s > 0)", {"xs": {"$list": [1, "x"]}}, 1),
    # --- stable_sort_by ---
    ("sort_empty", "stable_sort_by(xs as x => x)", {"xs": {"$list": []}}, {"$list": []}),
    ("sort_single", "stable_sort_by(xs as x => x)", {"xs": {"$list": [7]}}, {"$list": [7]}),
    ("sort_ties_stable", "stable_sort_by(xs as x => 0)", {"xs": {"$list": [3, 1, 2]}},
     {"$list": [3, 1, 2]}),
    ("sort_null_xs", "stable_sort_by(xs as x => x)", {"xs": None}, None),
    ("sort_nonlist_fault", "stable_sort_by(xs as x => x)", {"xs": 5}, FAULT),
    ("sort_ints", "stable_sort_by(xs as x => x)", {"xs": {"$list": [3, 1, 2]}},
     {"$list": [1, 2, 3]}),
    ("sort_amount_keys", "stable_sort_by(xs as x => x)",
     {"xs": {"$list": [{"$amount": 2}, {"$amount": 1}]}},
     {"$list": [{"$amount": 1}, {"$amount": 2}]}),
    ("sort_null_key_fault", "stable_sort_by(xs as x => x)", {"xs": {"$list": [1, None]}}, FAULT),
    ("sort_nonint_key_fault", "stable_sort_by(xs as x => x)", {"xs": {"$list": ["a"]}}, FAULT),
    ("sort_mixed_key_fault", "stable_sort_by(xs as x => x)",
     {"xs": {"$list": [1, "a"]}}, FAULT),
    # --- at ---
    ("at_in_range", "at(xs, 1)", {"xs": {"$list": [10, 20, 30]}}, 20),
    ("at_oob", "at(xs, 3)", {"xs": {"$list": [10, 20, 30]}}, None),
    ("at_negative", "at(xs, -1)", {"xs": {"$list": [10, 20]}}, None),
    ("at_empty", "at(xs, 0)", {"xs": {"$list": []}}, None),
    ("at_null_xs", "at(xs, 0)", {"xs": None}, None),
    ("at_null_xs_1", "at(xs, 1)", {"xs": None}, None),
    ("at_null_index", "at(xs, i)", {"xs": {"$list": [1]}, "i": None}, None),
    ("at_index_not_int_fault", 'at(xs, "0")', {"xs": {"$list": [1]}}, FAULT),
    ("at_nonlist_fault", "at(xs, 0)", {"xs": 5}, FAULT),
    ("at_of_sorted", "at(stable_sort_by(xs as x => x), 0)",
     {"xs": {"$list": [3, 1, 2]}}, 1),
    ("uint_hex_zero", "uint_hex(0)", {}, "0"),
    ("uint_hex_small", "uint_hex(255)", {}, "ff"),
    ("uint_hex_256bit", f'uint_hex(0x0{"f" * 63})', {}, "f" * 63),
    ("uint_hex_null", "uint_hex(null)", {}, None),
    ("uint_hex_neg_fault", "uint_hex(-1)", {}, FAULT),
    ("uint_hex_str_fault", 'uint_hex("ff")', {}, FAULT),
    ("uint_hex_bool_fault", "uint_hex(true)", {}, FAULT),
    ("ext_addr_str_std", f'ext_addr_str(account("{RAW}"))', {}, RAW.upper()),
    ("ext_addr_str_masterchain", f'ext_addr_str(account("-1:{"ab" * 32}"))', {}, _JETTON_RAW),
    ("ext_addr_str_none", "ext_addr_str(addr_none())", {}, None),
    ("ext_addr_str_null", "ext_addr_str(null)", {}, None),
    ("ext_addr_str_int_fault", "ext_addr_str(1)", {}, FAULT),
    ("txid_le_one", "txid_le_hex(1)", {}, "01" + "00" * 31),
    ("txid_le_max", f"txid_le_hex({2**256 - 1})", {}, "ff" * 32),
    ("txid_le_null", "txid_le_hex(null)", {}, None),
    ("txid_le_overflow_fault", f"txid_le_hex({2**256})", {}, FAULT),
    ("txid_le_neg_fault", "txid_le_hex(-1)", {}, FAULT),
    ("txid_le_str_fault", 'txid_le_hex("ff")', {}, FAULT),
    ("hex_bytes", "hex(a, 5)", {"a": {"$bytes_b64": _CELL_BYTES_B64}}, "deadbeef07"),
    ("hex_int_padded", "hex(1, 4)", {}, "00000001"),
    ("hex_null", "hex(null, 32)", {}, None),
    ("hex_bytes_width_fault", "hex(a, 4)", {"a": {"$bytes_b64": _CELL_BYTES_B64}}, FAULT),
    ("hex_int_overflow_fault", "hex(256, 1)", {}, FAULT),
    ("hex_neg_fault", "hex(-1, 1)", {}, FAULT),
    ("hex_str_fault", 'hex("ff", 1)', {}, FAULT),
    ("addr_hash_hex_basic", f'addr_hash_hex(account("{RAW.upper()}"))', {}, "0x" + "ab" * 32),
    ("addr_hash_hex_masterchain", f'addr_hash_hex(account("{_JETTON_RAW}"))', {}, "0x" + "ab" * 32),
    ("addr_hash_hex_none", "addr_hash_hex(addr_none())", {}, None),
    ("addr_hash_hex_null", "addr_hash_hex(null)", {}, None),
    ("addr_hash_hex_str_fault", f'addr_hash_hex("{RAW}")', {}, FAULT),
    ("addr_hash_hex_leading_zero", f'addr_hash_hex(account("0:0F3A{"AB" * 30}"))', {}, "0xf3a" + "ab" * 30),
    ("addr_hash_hex_zero", f'addr_hash_hex(account("0:{"0" * 64}"))', {}, "0x0"),
    ("dotself_in_build_fault", ".", {}, FAULT),
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
                   "Regenerate with ton-index-worker/ton-mch-engine/ir/generate_expr_vectors.py.",
        "vectors": vectors,
    }


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: generate_expr_vectors.py <out.json>")
    out = Path(sys.argv[1])
    out.write_text(json.dumps(build(), indent=1) + "\n", encoding="utf-8")
    print(f"wrote {len(build()['vectors'])} vectors -> {out}")
