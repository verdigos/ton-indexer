import io
import json

import pytest

from tests.utils import api_fixture


LIBRARY_BOC = "te6ccgEBAQEAAgAAAA=="


def _client(response):
    payload = json.dumps({"ok": True, "result": response}).encode()
    return api_fixture.ApiClient(api_key="test", opener=lambda request: io.BytesIO(payload))


def test_get_library_parses_toncenter_v2_library_result():
    response = {
        "@type": "smc.libraryResult",
        "result": [{
            "@type": "smc.libraryEntry",
            "data": LIBRARY_BOC,
            "hash": "j0Utek39dAZraCNlF3JZ7QVzRDW+drX9S9XYryt8PWg=",
        }],
    }

    library = _client(response).get_library(bytes.fromhex("8f452d7a4dfd74066b682365177259ed05734435be76b5fd4bd5d8af2b7c3d68"))

    assert library == api_fixture.Cell.one_from_boc(api_fixture.base64.b64decode(LIBRARY_BOC))


@pytest.mark.parametrize("response", [
    {"@type": "smc.libraryResult", "result": []},
    {"@type": "smc.libraryResult", "result": [{}]},
])
def test_get_library_reports_missing_data(response):
    client = _client(response)

    with pytest.raises(api_fixture.ApiError, match="library for 00"):
        client.get_library(bytes(32))


def test_method_propagates_library_api_error():
    class Client:
        def run_get_method(self, address, method, seqno, stack):
            raise api_fixture.ApiError("library fetch failed")

    with pytest.raises(api_fixture.ApiError, match="library fetch failed"):
        api_fixture._method(Client(), "0:00", "get_wallet_data", 1)
