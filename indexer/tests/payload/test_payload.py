import base64
import json
import re
from pathlib import Path

import msgpack
import pytest

from decode import convert, decode_payload


CODECS = [1]
ROOT = Path(__file__).resolve().parents[3]
DUMP = ROOT / "ton-index-go/index/parse/testdata/action_payload.jsonl"
KEYS = ROOT / "ton-index-worker/ton-mch-engine/docs/KEYS.md"
MAPPING = Path(__file__).resolve().parent / "mapping.json"


def _dump_rows():
    assert DUMP.is_file(), f"missing payload fixture dump: {DUMP}"
    rows = [json.loads(line) for line in DUMP.read_text().splitlines() if line.strip()]
    assert rows, f"empty payload fixture dump: {DUMP}"
    return rows


def _pack(value, codec=1):
    return bytes([codec]) + msgpack.packb(value, use_bin_type=True)


@pytest.mark.parametrize("codec", CODECS)
def test_fixture_payloads_round_trip(codec):
    for row in _dump_rows():
        payload = bytes.fromhex(row["payload_hex"])
        assert payload[0] == codec, row["fixture_id"]
        expected = convert(row["type"], row["legacy"])
        if expected.get("extra") == {}:  # v2 omits an empty extra group.
            expected.pop("extra")
        assert decode_payload(payload) == expected


def test_unknown_codec_is_rejected():
    with pytest.raises(ValueError, match=r"^unsupported payload codec 2$"):
        decode_payload(_pack({}, codec=2))


def test_empty_map_and_null_values():
    assert decode_payload(_pack({})) == {}
    assert decode_payload(_pack({"data": {"comment": None, "query_id": None}})) == {
        "data": {"comment": None, "query_id": None}
    }


def test_nested_nil_array_elements_are_preserved():
    value = {"data": {"peer_swaps": [None, {"query_id": "7"}, None]}}
    assert decode_payload(_pack(value)) == value


def test_decimal_strings_and_writer_stripped_text_are_preserved():
    value = {
        "data": {
            "query_id": str(2**80),
            "tick_lower": "-887272",
            "comment": "beforeafter",
        }
    }
    assert decode_payload(_pack(value)) == {
        "data": {"query_id": str(2**80), "tick_lower": "-887272", "comment": "beforeafter"}
    }


def test_decoder_does_not_rewrite_wire_text():
    value = {"data": {"comment": "before\x00after", "record_value": "001"}}
    assert decode_payload(_pack(value)) == value


def test_binary_values_become_contract_base64():
    raw = b"\x00\xffpayload"
    assert decode_payload(_pack({"data": {"custom_payload": raw}})) == {
        "data": {"custom_payload": base64.b64encode(raw).decode("ascii")}
    }


def test_unknown_keys_are_ignored():
    assert decode_payload(_pack({"future_group": {"future_field": 1}, "data": {"future_field": 2}})) == {
        "data": {}
    }


def _registry_ids():
    pattern = re.compile(r"^\| `([^`]+)` \|")
    return {match.group(1) for line in KEYS.read_text().splitlines() if (match := pattern.match(line))}


def test_registry_matches_mapping_and_fixture_keys():
    registry = _registry_ids()
    mapping = json.loads(MAPPING.read_text())["key_dictionary"]
    # The legacy mapping and fixtures still carry marketplace, removed from the registry.
    assert registry == set(mapping) - {"marketplace"}

    present = set()
    for row in _dump_rows():
        def visit(value):
            if isinstance(value, dict):
                present.update(value)
                for child in value.values():
                    visit(child)
            elif isinstance(value, list):
                for child in value:
                    visit(child)

        visit(decode_payload(bytes.fromhex(row["payload_hex"])))
    assert present - {"marketplace"} <= registry
