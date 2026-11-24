"""Cloudflare Turnstile：伺服器端的驗證。

頁面上的 widget 只負責讓訪客的瀏覽器拿到一個權杖；權杖是真是假、有沒有用過，
要由伺服器拿著 secret key 去問 Cloudflare 才算數。權杖五分鐘內有效、只能用一次。

照 Cloudflare 的做法，光是「驗證成功」還不夠，還要核對回應裡的兩樣東西：

- action：權杖是為了哪一個動作取得的。這個網站只有一個受保護的動作（開始一次上傳），
  頁面上的 widget 會帶著同一個名稱。
- hostname：權杖是在哪一個網域的頁面上取得的。只收設定檔列出的主機名稱，
  別人把同一個 widget 放在別的網站上取得的權杖不能拿來這裡用。
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field

import httpx

from .config import Config

log = logging.getLogger(__name__)

MAX_TOKEN_LENGTH = 2048

# 受保護的動作的名稱（1 到 32 個字元，只能有英數字、底線與連字號）。頁面從 /api/config 拿到同一個值
ACTION = "upload"


@dataclass
class TurnstileResult:
    ok: bool
    unavailable: bool = False            # 問不到 Cloudflare（網路或對方出錯），不是訪客的問題
    codes: list[str] = field(default_factory=list)
    hostname: str | None = None          # Cloudflare 回報的：權杖在哪個主機名稱取得
    action: str | None = None            # Cloudflare 回報的：權杖是為哪個動作取得


class Turnstile:
    def __init__(self, cfg: Config):
        self.enabled = cfg.turnstile_enabled
        self._secret = cfg.turnstile_secret_key
        self._url = cfg.turnstile_verify_url
        self._hostnames = frozenset(cfg.turnstile_hostnames)
        # Cloudflare 的測試金鑰回的主機名稱固定是 example.com、也沒有 action。
        # 測試金鑰本來就沒有防護（驗證一律通過），所以用它們的時候只看成不成功
        self._test_keys = cfg.turnstile_uses_test_keys
        self._client = httpx.AsyncClient(timeout=httpx.Timeout(10.0)) if self.enabled else None

    async def close(self) -> None:
        if self._client is not None:
            await self._client.aclose()

    async def verify(self, token: str, remote_ip: str | None = None) -> TurnstileResult:
        """查證一個權杖。remote_ip 是訪客的位址（有的話一併交給 Cloudflare 核對）。"""
        if not self.enabled:
            return TurnstileResult(ok=True)
        if not isinstance(token, str) or not token or len(token) > MAX_TOKEN_LENGTH:
            return TurnstileResult(ok=False, codes=["missing-input-response"])
        assert self._client is not None
        data = {"secret": self._secret, "response": token}
        if remote_ip:
            data["remoteip"] = remote_ip
        try:
            response = await self._client.post(self._url, data=data)
            response.raise_for_status()
            body = response.json()
        except (httpx.HTTPError, ValueError) as e:
            log.warning("Turnstile 驗證服務沒有回應：%s", type(e).__name__)
            return TurnstileResult(ok=False, unavailable=True)
        if not isinstance(body, dict):
            return TurnstileResult(ok=False, unavailable=True)
        codes = [str(c) for c in body.get("error-codes", []) if isinstance(c, str)]
        hostname = body.get("hostname") if isinstance(body.get("hostname"), str) else None
        action = body.get("action") if isinstance(body.get("action"), str) else None
        if body.get("success") is not True:
            # secret key 填錯是站方的設定問題，不是訪客的錯：記下來，回報成服務無法使用
            if any(c in ("missing-input-secret", "invalid-input-secret") for c in codes):
                log.error("Turnstile 的 secret key 不對（%s）", ", ".join(codes))
                return TurnstileResult(ok=False, unavailable=True, codes=codes)
            return TurnstileResult(ok=False, codes=codes, hostname=hostname, action=action)
        if self._test_keys:
            return TurnstileResult(ok=True, hostname=hostname, action=action)
        if action != ACTION:
            log.warning("Turnstile：權杖的動作是 %r，不是 %r", (action or "")[:40], ACTION)
            return TurnstileResult(ok=False, codes=["action-mismatch"], hostname=hostname, action=action)
        if hostname is None or hostname.lower() not in self._hostnames:
            # 多半是設定的問題（網站換了網域，或 public_url／hostnames 沒有跟著改），所以把名稱記下來
            log.warning("Turnstile：權杖是在 %r 取得的，不在允許的主機名稱裡（%s）", (hostname or "")[:100],
                        "、".join(sorted(self._hostnames)))
            return TurnstileResult(ok=False, codes=["hostname-mismatch"], hostname=hostname, action=action)
        return TurnstileResult(ok=True, hostname=hostname, action=action)
