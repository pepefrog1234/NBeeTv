"""每個來源位址在一段時間內能開始幾次解碼。紀錄只放在記憶體裡，程式重開就清空。"""

from __future__ import annotations

import ipaddress
import time
from collections import deque


def client_key(address: str) -> str:
    """把位址變成計數用的鍵。IPv6 以 /64 計：同一條線路可以任意換後半段的位址。"""
    try:
        ip = ipaddress.ip_address(address.strip())
    except ValueError:
        return "unknown"
    if isinstance(ip, ipaddress.IPv6Address):
        if ip.ipv4_mapped is not None:
            return str(ip.ipv4_mapped)
        return str(ipaddress.ip_network((int(ip) >> 64 << 64, 64)).network_address) + "/64"
    return str(ip)


class RateLimiter:
    def __init__(self, limit: int, window_seconds: float, clock=time.monotonic):
        self.limit = limit
        self.window = window_seconds
        self._clock = clock
        self._hits: dict[str, deque[float]] = {}
        self._records = 0

    def _trim(self, key: str, now: float) -> deque[float]:
        hits = self._hits.setdefault(key, deque())
        while hits and now - hits[0] >= self.window:
            hits.popleft()
        return hits

    def check(self, key: str) -> float:
        """還能不能再來一次：可以就回傳 0，不行就回傳還要等幾秒。不會記一次。"""
        now = self._clock()
        hits = self._trim(key, now)
        if len(hits) < self.limit:
            return 0.0
        return max(0.0, self.window - (now - hits[0]))

    def record(self, key: str) -> float:
        """記一次，回傳這一次的時間（之後要取消的話交給 forget）。"""
        now = self._clock()
        self._trim(key, now).append(now)
        self._records += 1
        if self._records % 200 == 0:
            self.prune()
        return now

    def forget(self, key: str, stamp: float) -> None:
        """取消剛才記的那一次（例如後面的驗證沒過，這一次不該算）。"""
        hits = self._hits.get(key)
        if hits is None:
            return
        try:
            hits.remove(stamp)
        except ValueError:
            pass

    def prune(self) -> None:
        """丟掉已經過期的紀錄，免得位址愈積愈多。"""
        now = self._clock()
        for key in list(self._hits):
            if not self._trim(key, now):
                del self._hits[key]
