"""Unpack an LZ4 trace fixture to editable JSON and pack it back.

The JSON form is the fixture map 1:1, except for the two msgpack shapes JSON
cannot spell: bin values become ``{"$b64": "..."}`` and a map with any
non-string key becomes ``{"$map": [[key, value], ...]}``. Those two tags are
reserved: a fixture map holding exactly one of them under that name would be
read back as the tagged value.
"""

from __future__ import annotations

import argparse
import base64
import json
from pathlib import Path
from typing import Any

import lz4.frame
import msgpack

from tests.utils import api_fixture

B64_TAG = "$b64"
MAP_TAG = "$map"


def _to_json(value: Any) -> Any:
    if isinstance(value, bytes):
        return {B64_TAG: base64.b64encode(value).decode()}
    if isinstance(value, dict):
        if all(isinstance(key, str) for key in value):
            return {key: _to_json(item) for key, item in value.items()}
        return {MAP_TAG: [[_to_json(key), _to_json(item)] for key, item in value.items()]}
    if isinstance(value, list):
        return [_to_json(item) for item in value]
    return value


def _from_json(value: Any) -> Any:
    if isinstance(value, dict):
        if set(value) == {B64_TAG}:
            return base64.b64decode(value[B64_TAG])
        if set(value) == {MAP_TAG}:
            return {_from_json(key): _from_json(item) for key, item in value[MAP_TAG]}
        return {key: _from_json(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_from_json(item) for item in value]
    return value


def unpack(path: str | Path) -> Any:
    """Read an LZ4 fixture and return its JSON-safe form."""
    raw = lz4.frame.decompress(Path(path).read_bytes())
    # strict_map_key=False: the $map tag exists precisely for non-string keys.
    return _to_json(msgpack.unpackb(raw, raw=False, strict_map_key=False))


def pack(obj: Any, path: str | Path) -> None:
    """Write a JSON-form fixture back to an LZ4 fixture."""
    api_fixture.write_fixture(_from_json(obj), path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Unpack an LZ4 trace fixture to JSON, or pack JSON back")
    commands = parser.add_subparsers(dest="command", required=True)
    unpack_parser = commands.add_parser("unpack", help="write the fixture's JSON form")
    unpack_parser.add_argument("fixture", type=Path, help="input .lz4 path")
    unpack_parser.add_argument("-o", "--output", required=True, type=Path, help="output .json path")
    pack_parser = commands.add_parser("pack", help="write the JSON form back as a fixture")
    pack_parser.add_argument("json_file", type=Path, help="input .json path")
    pack_parser.add_argument("-o", "--output", required=True, type=Path, help="output .lz4 path")
    args = parser.parse_args(argv)
    if args.command == "unpack":
        args.output.write_text(json.dumps(unpack(args.fixture), indent=2) + "\n", encoding="utf-8")
    else:
        pack(json.loads(args.json_file.read_text(encoding="utf-8")), args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
