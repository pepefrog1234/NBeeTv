"""發卡的條件：有畫面的幀超過門檻，而且至少一幀的標頭解出了呼號。

卡片上印的電台就是從錄音解出來的那個呼號：不限定是哪一台，誰的發射都可以領卡。
一段錄音裡有好幾個呼號時，以標頭解出來次數最多的那一個為準（一樣多時取先出現的）。

這幾個模式的畫面沒有「解成功或失敗」的分界（訊號再弱也會有一張糊的畫面），
有對錯可言的只有標頭：呼號與幀序號有 CRC 保護。所以條件裡兩樣都要。
"""

from __future__ import annotations

import statistics
from dataclasses import dataclass, field

from .config import CALLSIGN_RE
from .decoder import Frame


@dataclass
class Verdict:
    ok: bool
    reason: str | None           # no_signal、no_callsign、too_few、model_mismatch
    callsign: str | None         # 這段錄音是哪一台送的（卡片上印的就是它）；一個呼號都沒解出來時是 None
    reported: int                # 接收機回報了幾幀
    pictures: int                # 其中幾幀有畫面
    counted: int                 # 算數的幀：有畫面，而且不是別的呼號送的
    callsign_frames: int         # 標頭解出這個呼號的幀
    other_callsigns: list[str]   # 錄音裡另外還解出了哪些呼號
    modes: dict[int, int]        # 算數的幀裡，各個模式各幾幀
    snr_median: float | None
    snr_best: float | None
    offset_hz: float | None      # 頻率偏移的中位數
    best: Frame | None = None    # 放在卡片正中的那一幀
    strip: list[Frame] = field(default_factory=list)  # 卡片上另外排的幾幀


def evaluate(frames: list[Frame], min_frames: int, strip_size: int = 5) -> Verdict:
    # 這段錄音是誰送的：標頭解出來的呼號裡出現最多次的那一個（一樣多時取先出現的）。
    # 寫法不像呼號的不算：它之後要印在卡片上、放進檔名。
    seen: dict[str, int] = {}
    for f in frames:
        if f.header and f.callsign and CALLSIGN_RE.match(f.callsign):
            seen[f.callsign] = seen.get(f.callsign, 0) + 1
    callsign = max(seen, key=seen.__getitem__) if seen else None
    ours = [f for f in frames if f.header and f.callsign == callsign] if callsign else []
    others = sorted(c for c in seen if c != callsign)
    # 標頭壞掉的幀不知道是誰送的，算進來；標頭明確寫著別的呼號的不算
    counted = [f for f in frames if f.picture and not (f.header and f.callsign != callsign)]
    pictures = sum(1 for f in frames if f.picture)

    modes: dict[int, int] = {}
    for f in counted:
        modes[f.mode] = modes.get(f.mode, 0) + 1
    snrs = [f.snr_db for f in counted if f.snr_db is not None]
    offsets = [f.offset_hz for f in counted if f.offset_hz is not None]

    if not frames:
        reason = "no_signal"
    elif not ours:
        reason = "no_callsign"
    elif len(counted) <= min_frames:
        missing_model = any(f.model_missing for f in frames)
        reason = "model_mismatch" if missing_model and not counted else "too_few"
    else:
        reason = None

    def quality(f: Frame) -> tuple[int, float]:
        # 先挑標頭解得出這個呼號的，再比訊雜比
        return (1 if f.header and f.callsign == callsign else 0, f.snr_db if f.snr_db is not None else -99.0)

    best = max(counted, key=quality) if counted else None
    rest = [f for f in counted if f is not best]
    if len(rest) > strip_size:
        # 在時間上平均取幾幀，看得出這段錄音從頭到尾的樣子
        step = (len(rest) - 1) / (strip_size - 1) if strip_size > 1 else 0
        rest = [rest[round(i * step)] for i in range(strip_size)]

    return Verdict(
        ok=reason is None,
        reason=reason,
        callsign=callsign,
        reported=len(frames),
        pictures=pictures,
        counted=len(counted),
        callsign_frames=len(ours),
        other_callsigns=others,
        modes=dict(sorted(modes.items())),
        snr_median=round(statistics.median(snrs), 1) if snrs else None,
        snr_best=round(max(snrs), 1) if snrs else None,
        offset_hz=round(statistics.median(offsets), 1) if offsets else None,
        best=best,
        strip=rest,
    )
