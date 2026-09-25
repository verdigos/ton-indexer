"""Registry and generated-file checks; no C++ build, source scan, or Go toolchain."""

import copy

import pytest
import yaml

from indexer.events.mch import payload_keys as pk


REGENERATE = 'cd indexer && venv/bin/python -m indexer.events.mch.payload_keys gen'


def test_registry_and_generated_files():
    rows = pk.checked()['keys']
    assert pk.GO_OUTPUT.read_bytes() == pk.go_text(rows).encode(), f'run: {REGENERATE}'
    assert pk.DOC_OUTPUT.read_bytes() == pk.doc_text(rows).encode(), f'run: {REGENERATE}'


@pytest.mark.parametrize('change', [
    lambda rows: rows[0].update(id=0),
    lambda rows: rows[1].update(id=rows[0]['id']),
    lambda rows: rows[1].update(id=3),
    lambda rows: rows[1].update(name=rows[0]['name']),
    lambda rows: rows[0].update(name='Bad'),
    lambda rows: rows[0].update(type='wrong'),
    lambda rows: rows[0].update(shape=123),
    lambda rows: rows[0].update(id=256),
])
def test_invalid_registry(tmp_path, change):
    rows = copy.deepcopy(pk.checked()['keys'][:2])
    change(rows)
    path = tmp_path / 'payload-keys.yaml'
    path.write_text(yaml.safe_dump({'version': 1, 'keys': rows}))
    with pytest.raises(ValueError):
        pk.checked(path)


def test_cli_gen(tmp_path, monkeypatch):
    path = tmp_path / 'payload-keys.yaml'
    path.write_text('version: 1\nkeys:\n- id: 1\n  name: first\n  type: string\n')
    monkeypatch.setattr(pk, 'GO_OUTPUT', tmp_path / 'payload_keys_gen.go')
    monkeypatch.setattr(pk, 'DOC_OUTPUT', tmp_path / 'KEYS.md')
    pk.main(['--registry', str(path), 'gen'])
    assert '{Name: "first", Type: payloadTypeString}' in pk.GO_OUTPUT.read_text()


def test_id_128_generation(tmp_path):
    rows = [{'id': i, 'name': f'key_{i}', 'type': 'uint'} for i in range(1, 129)]
    path = tmp_path / 'payload-keys.yaml'
    path.write_text(yaml.safe_dump({'version': 1, 'keys': rows}))
    assert len(pk.checked(path)['keys']) == 128
    assert '128: {Name: "key_128"' in pk.go_text(rows)
    assert '{"key_128", 128, PayloadKeyType::Uint}' in pk.cpp_text(rows)
