"""Keep compiler and evaluator builtin metadata on one authoritative table."""

from indexer.events.mch import compiler
from indexer.events.mch.builtin_signatures import BUILTIN_SIGNATURES
from indexer.events.mch.expr_eval import BUILTINS


def test_compiler_and_evaluator_builtin_signatures_agree():
    assert compiler.BUILTIN_SIGNATURES is BUILTIN_SIGNATURES
    assert {name: arity for name, (_, arity) in BUILTINS.items()} == BUILTIN_SIGNATURES
    assert {name: BUILTIN_SIGNATURES[name] for name in ("get", "tx_aborted", "exit_code", "at")} == {
        "get": 2, "tx_aborted": 1, "exit_code": 1, "at": 2,
    }
    assert {name: BUILTIN_SIGNATURES[name] for name in ("uint_hex", "ext_addr_str", "txid_le_hex", "addr_hash_hex")} == {
        "uint_hex": 1, "ext_addr_str": 1, "txid_le_hex": 1, "addr_hash_hex": 1,
    }
