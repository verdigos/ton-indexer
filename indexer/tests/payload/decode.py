"""Decode action payload v2 rows for development tools."""

import base64
import json
from pathlib import Path
from typing import Any

import msgpack


_MAPPING = json.loads((Path(__file__).with_name("mapping.json")).read_text())
_KEY_KINDS = {
    key: {entry["kind"] for entry in value["uses"]}.pop()
    for key, value in _MAPPING["key_dictionary"].items()
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
