import base64
import json
import sys
from pathlib import Path

import pytest
from pytoniq_core import Cell

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

FIXTURES = Path(__file__).resolve().parents[2] / "indexer" / "tests" / "pools"


def stack_value(entry: dict):
    """API-shaped stack entry (as pool fixtures store them) -> pytoniq value."""
    kind = entry["@type"]
    if kind == "tvm.stackEntryNumber":
        number = entry["number"]
        return int(number["number"] if isinstance(number, dict) else number)
    if kind == "tvm.stackEntryNull":
        return None
    field = "slice" if kind == "tvm.stackEntrySlice" else "cell"
    cell = Cell.one_from_boc(base64.b64decode(entry[field]["bytes"]))
    return cell.begin_parse() if field == "slice" else cell


def load_fixture(path: Path) -> dict:
    fixture = json.loads(path.read_text())
    fixture["stacks"] = {name: [stack_value(e) for e in stack] for name, stack in fixture["methods"].items()}
    return fixture


@pytest.fixture(scope="session")
def fixtures() -> list[dict]:
    return [load_fixture(p) for p in sorted(FIXTURES.glob("*.json"))]
