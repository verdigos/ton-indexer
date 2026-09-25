"""Minimal toncenter client for the few facts the tap does not carry.

Only immutable facts are fetched, each once and cached on disk: library cells (STON.fi v2 pool
code is a library reference), the jetton master behind a router jetton wallet, and jetton
decimals. The API key comes from the API_KEY environment variable and is sent as a header.
"""
from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

BASE_URL = "https://toncenter.com/api/"
USER_AGENT = "ton-indexer-dex-tap/1.0"


class ToncenterError(RuntimeError):
    pass


class Toncenter:
    def __init__(self, api_key: str | None = None, base_url: str = BASE_URL, retries: int = 5):
        self.api_key = api_key if api_key is not None else os.environ.get("API_KEY", "")
        self.base_url = base_url.rstrip("/") + "/"
        self.retries = retries

    def _request(self, path: str, params: dict | None = None, body: dict | None = None) -> Any:
        url = self.base_url + path
        if params:
            url += "?" + urllib.parse.urlencode(params)
        headers = {"User-Agent": USER_AGENT, "Accept": "application/json"}
        if self.api_key:
            headers["X-API-Key"] = self.api_key
        payload = None
        if body is not None:
            payload = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        for attempt in range(self.retries + 1):
            try:
                with urllib.request.urlopen(urllib.request.Request(url, payload, headers), timeout=30) as response:
                    return json.loads(response.read())
            except urllib.error.HTTPError as exc:
                if exc.code != 429 and exc.code < 500:
                    raise ToncenterError(f"toncenter {path} failed with status {exc.code}") from None
            except (urllib.error.URLError, TimeoutError):
                pass
            time.sleep(min(0.5 * 2 ** attempt, 8.0))
        # The URL may carry query parameters but never the key, which travels in a header.
        raise ToncenterError(f"toncenter {path} failed after retries")

    def library(self, lib_hash_hex: str) -> str:
        """Base64 BoC of a library cell."""
        result = self._request("v2/getLibraries", {"libraries": lib_hash_hex.lower()})
        entries = (result.get("result") or {}).get("result") or []
        if not entries or "data" not in entries[0]:
            raise ToncenterError(f"no library {lib_hash_hex}")
        return entries[0]["data"]

    def jetton_wallet_master(self, wallet: str) -> str | None:
        result = self._request("v3/jetton/wallets", {"address": wallet, "limit": 1})
        wallets = result.get("jetton_wallets") or []
        return wallets[0]["jetton"].upper() if wallets else None

    def jetton_master(self, master: str) -> dict | None:
        result = self._request("v3/jetton/masters", {"address": master, "limit": 1})
        masters = result.get("jetton_masters") or []
        if not masters:
            return None
        content = masters[0].get("jetton_content") or {}
        symbol = content.get("symbol")
        metadata = (result.get("metadata") or {}).get(masters[0]["address"], {})
        for info in metadata.get("token_info") or []:
            symbol = symbol or info.get("symbol")
        return {"decimals": int(content.get("decimals", 9)), "symbol": symbol or ""}


class DiskCache:
    """A JSON file of key -> value, written through on every new key."""

    def __init__(self, path: Path):
        self.path = path
        self.values: dict[str, Any] = json.loads(path.read_text()) if path.exists() else {}

    def get(self, key: str, default=None):
        return self.values.get(key, default)

    def __contains__(self, key: str) -> bool:
        return key in self.values

    def put(self, key: str, value) -> None:
        self.values[key] = value
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.values, sort_keys=True))
        tmp.replace(self.path)
