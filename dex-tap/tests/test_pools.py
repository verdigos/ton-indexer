import pytest

from dex_tap import pools

USDT = "0:B113A994B5024A16719F69139328EB759596C38A25F59028B146FECDC3621DFE"


def parsed(fixture):
    return pools.parse_pool(fixture["dex"], fixture["stacks"])


def test_every_fixture_parses_to_a_positive_spot_price(fixtures):
    assert fixtures
    for fixture in fixtures:
        pool = parsed(fixture)
        assert pool.reserve0 > 0 and pool.reserve1 > 0, fixture["pool"]
        assert pool.spot_price() > 0, fixture["pool"]


def test_fixture_code_hashes_are_registered(fixtures):
    import base64

    for fixture in fixtures:
        code_hash = base64.b64decode(fixture["code_hash"]).hex().upper()
        assert pools.CODE_HASHES[code_hash] == fixture["dex"], fixture["pool"]


def test_spot_matches_stonfi_v1_quotes(fixtures):
    checked = 0
    for fixture in (f for f in fixtures if f["dex"] == "stonfi_v1"):
        pool = parsed(fixture)
        for quote in fixture["quotes"]:
            amount_in = int(quote["amount_in"])
            out = sum(int(e["number"]["number"]) for e in quote["stack"])  # out + protocol fee + ref fee
            spot = pool.spot_price() if quote["side"] == 0 else 1 / pool.spot_price()
            assert out / amount_in == pytest.approx(spot * (1 - pool.lp_fee), rel=1e-5)
            checked += 1
    assert checked


def test_spot_matches_dedust_v1_quotes(fixtures):
    checked = 0
    for fixture in (f for f in fixtures if f["dex"] == "dedust_v1"):
        pool = parsed(fixture)
        for quote in fixture["quotes"]:
            amount_in = int(quote["amount_in"])
            out = int(quote["stack"][1]["number"]["number"])
            fee = int(quote["stack"][2]["number"]["number"])
            spot = pool.spot_price() if quote["side"] == 0 else 1 / pool.spot_price()
            assert out / (amount_in - fee) == pytest.approx(spot, rel=1e-5)
            checked += 1
    assert checked


def test_pool_types_and_params(fixtures):
    by_pool = {f["pool"][:10]: parsed(f) for f in fixtures}
    assert by_pool["0:52518E14"].pool_type == "stableswap" and by_pool["0:52518E14"].params == {"amp": 100}
    assert by_pool["0:022CDBE4"].params == {"weight0": 0.25}
    assert by_pool["0:05EA635B"].params == pytest.approx({"amp": 50, "rate": 1.304, "weight0": 0.25})
    assert by_pool["0:01B7C61E"].pool_type == "solidly"
    assert by_pool["0:812FAE69"].side0 == pools.TON and by_pool["0:812FAE69"].side1 == USDT
    assert by_pool["0:3E5FFCA8"].side0 == pools.TON and by_pool["0:3E5FFCA8"].side1 == USDT
    assert by_pool["0:FC4C9F31"].sides_are_wallets


def test_anchor_pools_agree_on_ton_price(fixtures):
    by_pool = {f["pool"][:10]: parsed(f) for f in fixtures}
    # raw USDT per nanoTON from each anchor
    stonfi_v1 = 1 / by_pool["0:FC4C9F31"].spot_price()   # asset0 USDT, asset1 pTON
    stonfi_v2 = 1 / by_pool["0:8649CAD9"].spot_price()
    dedust_v1 = by_pool["0:3E5FFCA8"].spot_price()      # asset0 TON, asset1 USDT
    assert stonfi_v1 == pytest.approx(dedust_v1, rel=0.01)
    assert stonfi_v2 == pytest.approx(dedust_v1, rel=0.01)
