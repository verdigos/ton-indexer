"""Cross-check the compiler's builtin signatures against the C++ evaluator.

The two tables are read from different languages and used by different stages
(the compiler resolves calls at build time, the engine dispatches them at run
time), so a name or arity that drifts on one side compiles fine and faults only
at run time: the name and arity sets must be equal, in both directions.
"""

import re
from pathlib import Path

from indexer.events.mch.builtin_signatures import BUILTIN_SIGNATURES

ENGINE_EXPR_RUNTIME = (
    Path(__file__).resolve().parents[3] / "ton-index-worker/ton-mch-engine/src/ExprRuntime.cpp"
)

# The arity map `rt_call_builtin` looks the name up in before dispatching.
_ARITY_MAP = re.compile(r"rt_call_builtin\(.*?arity\s*=\s*\{(.*?)\};", re.S)
_ARITY_ENTRY = re.compile(r'\{\s*"(\w+)"\s*,\s*(\d+)\s*\}')


def engine_builtin_arities(path=ENGINE_EXPR_RUNTIME):
    """Parse the builtin arity map out of the engine's ExprRuntime.cpp."""
    text = Path(path).read_text()
    match = _ARITY_MAP.search(text)
    if match is None:
        raise AssertionError(f"rt_call_builtin arity map not found in {path}")
    body = match.group(1)
    arities = {name: int(arity) for name, arity in _ARITY_ENTRY.findall(body)}
    # Every brace in the map body must belong to a parsed entry, so a shape this
    # parser does not understand fails loudly instead of silently dropping rows.
    if not arities or body.count("{") != len(arities):
        raise AssertionError(f"arity map in {path} did not parse as a list of entries")
    return arities


def builtin_signature_differences(python_signatures, engine_arities):
    """Return (only in python, only in engine, arity differs) name sets."""
    only_python = sorted(set(python_signatures) - set(engine_arities))
    only_engine = sorted(set(engine_arities) - set(python_signatures))
    differs = {name: (python_signatures[name], engine_arities[name])
               for name in sorted(set(python_signatures) & set(engine_arities))
               if python_signatures[name] != engine_arities[name]}
    return only_python, only_engine, differs


def check_builtin_signatures(python_signatures=BUILTIN_SIGNATURES, engine_path=ENGINE_EXPR_RUNTIME):
    engine_arities = engine_builtin_arities(engine_path)
    only_python, only_engine, differs = builtin_signature_differences(
        python_signatures, engine_arities
    )
    assert not only_python, f"builtins missing from the engine arity map: {only_python}"
    assert not only_engine, f"builtins missing from BUILTIN_SIGNATURES: {only_engine}"
    assert not differs, f"builtin arity differs as (python, engine): {differs}"


def test_builtin_signatures_match_engine_arity_map():
    check_builtin_signatures()
