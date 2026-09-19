"""Decode action payload v2 rows for development tools."""

import base64
import json
import re
from pathlib import Path
from typing import Any

import msgpack


_MAPPING = json.loads((Path(__file__).with_name("mapping.json")).read_text())
_KEY_KINDS = {
    key: {entry["kind"] for entry in value["uses"]}.pop()
    for key, value in _MAPPING["key_dictionary"].items()
}


def _transform(value, spec, producer=False, path=""):
    if value is None:
        return None
    kind = spec["kind"]
    if kind == "object":
        assert isinstance(value, dict), (path, "expected object")
        fields = spec["fields"]
        lookup = {child.get("legacy_name", key): (key, child) for key, child in fields.items()}
        normalized = dict(value)
        if producer and path == "ton_transfer_data" and "comment" in normalized and "content" in lookup:
            if "content" in normalized and normalized["content"] != normalized["comment"]:
                raise AssertionError("Conflicting content/comment alias")
            normalized["content"] = normalized.pop("comment")
        assert not set(normalized) - set(lookup), (
            path,
            "unknown fields",
            sorted(set(normalized) - set(lookup)),
        )
        out = {}
        for key, child_value in normalized.items():
            name, child = lookup[key]
            assert name not in out, (path, name, "target collision")
            out[name] = _transform(child_value, child, producer, path + "." + key)
        return out
    if kind == "array":
        assert isinstance(value, list), (path, "expected array")
        return [_transform(child, spec["items"], producer, path + "[]") for child in value]
    if kind == "integer_string":
        assert isinstance(value, (int, str)) and not isinstance(value, bool), (
            path,
            "integer without float hop",
            value,
        )
        text = str(value)
        assert re.fullmatch(r"0|-?[1-9][0-9]*", text), (path, "noncanonical integer", text)
        return text
    if kind == "boolean":
        assert isinstance(value, bool), (path, "expected bool", value)
    else:
        assert isinstance(value, str), (path, "expected string", value)
    return value


def convert(action, legacy, producer=False):
    groups = _MAPPING["actions"][action]["groups"]
    allowed = {group["legacy_column"] for group in groups.values()}
    unexpected = {
        key
        for key in set(legacy) - allowed
        if key not in _MAPPING["legacy_detail_columns"] or legacy[key] is not None
    }
    assert not unexpected, (action, "unexpected populated column", unexpected)
    return {
        name: _transform(
            legacy[group["legacy_column"]],
            group["schema"],
            producer=producer and action == "auction_outbid",
            path=group["legacy_column"],
        )
        for name, group in groups.items()
        if group["legacy_column"] in legacy
    }


def _decode_value(value: Any, kind: str | None = None) -> Any:
    if value is None:
        return None
    if isinstance(value, dict):
        return {
            key: _decode_value(child, _KEY_KINDS[key])
            for key, child in value.items()
            if isinstance(key, str) and key in _KEY_KINDS
        }
    if isinstance(value, list):
        return [_decode_value(child) for child in value]
    if kind == "bytes_base64" and isinstance(value, bytes):
        return base64.b64encode(value).decode("ascii")
    # The writer emits canonical decimal and NUL-free strings already.
    return value


def decode_payload(b: bytes) -> dict:
    """Decode a codec-prefixed action payload into its logical JSON shape."""
    if not isinstance(b, (bytes, bytearray, memoryview)) or not b:
        raise ValueError("empty action payload")
    codec = b[0]
    if codec != 1:
        raise ValueError(f"unsupported payload codec {codec}")
    value = msgpack.unpackb(bytes(b[1:]), raw=False, strict_map_key=False)
    if not isinstance(value, dict):
        raise ValueError("action payload is not a msgpack map")
    return _decode_value(value)
