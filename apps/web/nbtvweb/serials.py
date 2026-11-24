"""收聽卡的序號：12 個字元、分成三組，例如 7KQ2-M9XD-4TFA。

序號是亂數（60 位元），猜不到也排不出順序：知道序號的人才查得到那一張卡。
字母表是 Crockford 的 base32，沒有容易和數字混淆的 I、L、O、U。
"""

from __future__ import annotations

import re
import secrets

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
_PLAIN = re.compile(f"^[{ALPHABET}]{{12}}$")


def new_serial() -> str:
    raw = "".join(secrets.choice(ALPHABET) for _ in range(12))
    return f"{raw[0:4]}-{raw[4:8]}-{raw[8:12]}"


def normalize_serial(text: str) -> str | None:
    """把使用者打的序號整理成標準寫法；不是序號就回傳 None。"""
    if not isinstance(text, str) or len(text) > 40:
        return None
    raw = re.sub(r"[\s\-]", "", text.upper()).replace("O", "0").replace("I", "1").replace("L", "1")
    if not _PLAIN.match(raw):
        return None
    return f"{raw[0:4]}-{raw[4:8]}-{raw[8:12]}"
