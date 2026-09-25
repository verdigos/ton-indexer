import pytest

from tests.utils import pool_fixture


def _numbers(count):
    return [{"@type": "tvm.stackEntryNumber", "number": {"number": "0"}}] * count


@pytest.mark.parametrize("methods, dex", [
    ({"get_pool_type": [{}], "get_pool_data": _numbers(12)}, "stonfi_v2"),
    ({"get_assets": [{}, {}], "get_reserves": _numbers(2), "get_pool_data": None}, "dedust_v1"),
    ({"get_pool_data": _numbers(10)}, "stonfi_v1"),
    ({"get_pool_data": _numbers(19)}, "dedust_v2"),
    ({"get_pool_data": _numbers(7)}, None),
    ({}, None),
])
def test_detect_dex(methods, dex):
    assert pool_fixture.detect_dex(methods) == dex


def test_fixture_path_uses_dex_short_address_and_seqno(tmp_path):
    fixture = {"pool": "0:FC4C9F311160754A99D113877CF583B78E5D16D048819CD4B820168769499D7E",
               "dex": "stonfi_v1", "mc_seqno": 95040131}

    assert pool_fixture.fixture_path(tmp_path, fixture) == tmp_path / "stonfi_v1-fc4c9f31-95040131.json"
