"""呼叫 nbtv-rx 解一份錄音。

每一份錄音各開一個行程：解碼器當掉或卡住都不會拖垮網站，逾時就直接結束它。
結果從 nbtv-rx --json 的輸出讀（一幀一行），畫面從它存的 PPM 讀。
"""

from __future__ import annotations

import asyncio
import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

IMAGE_WIDTH = 96
IMAGE_HEIGHT = 72
IMAGE_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT * 3
# 畫面的大小跟著模式：模式 11–14 是上面的 96×72，模式 5–7 與 15（給 FM 用的大畫面）是 192×144。
# 位元組數各不相同，所以一張 RGB 的畫面由長度就看得出大小（picture_size）。
# 卡片與縮圖的版面是照 96×72 排的，比較大的畫面縮進同樣的位置（card.py 的 _picture）。
PICTURE_SIZES = ((IMAGE_WIDTH, IMAGE_HEIGHT), (192, 144))


def picture_size(rgb: bytes) -> tuple[int, int]:
    """這段 RGB 是多大的畫面（寬, 高）。長度不是認得的畫面大小時丟出 ValueError。"""
    for width, height in PICTURE_SIZES:
        if len(rgb) == width * height * 3:
            return width, height
    raise ValueError("不是認得的畫面大小")

# 五分鐘的錄音最多約 450 幀（模式 14 每幀 0.667 秒）；超過這個數就是哪裡不對了
MAX_FRAMES = 1000

_NICE = "/usr/bin/nice"


class DecodeError(Exception):
    def __init__(self, code: str, message: str = ""):
        super().__init__(message or code)
        self.code = code


@dataclass
class Frame:
    index: int
    time: float              # 這一幀在錄音裡的開始時間（秒）
    mode: int
    picture: bool            # 有沒有畫面
    header: bool             # 標頭（呼號、幀序號）有沒有通過檢查
    callsign: str | None
    sequence: int
    snr_db: float | None
    offset_hz: float | None
    model_missing: bool
    rgb: bytes | None = None  # RGB：96×72，模式 5–7 與 15 是 192×144（見 picture_size）


def _read_ppm(path: Path) -> bytes:
    """讀 nbtv-rx 存的二進位 PPM（96×72，或模式 5–7 與 15 的 192×144），回傳 RGB。"""
    data = path.read_bytes()
    for width, height in PICTURE_SIZES:
        header = b"P6\n%d %d\n255\n" % (width, height)   # nbtv-rx 寫的檔頭是固定的（tools/ppm.hpp 的 write_ppm）
        if data.startswith(header) and len(data) == len(header) + width * height * 3:
            return data[len(header):]
    raise DecodeError("decoder_failed", f"{path.name} 不是 96×72 或 192×144 的 PPM")


def _parse_frame(obj: dict) -> Frame:
    callsign = obj.get("callsign")
    return Frame(
        index=int(obj["frame"]),
        time=float(obj["time"] or 0.0),
        mode=int(obj["mode"]),
        picture=bool(obj["picture"]),
        header=bool(obj["header"]),
        callsign=callsign.strip() if isinstance(callsign, str) else None,
        sequence=int(obj["sequence"]),
        snr_db=None if obj.get("snr_db") is None else float(obj["snr_db"]),
        offset_hz=None if obj.get("offset_hz") is None else float(obj["offset_hz"]),
        model_missing=bool(obj.get("model_missing", False)),
    )


async def decode(
    nbtv_rx: Path,
    wav: Path,
    work_dir: Path,
    *,
    merge: bool,
    timeout: float,
    on_frame: Callable[[Frame], None] | None = None,
) -> list[Frame]:
    """解 wav（要是 12 或 48 kHz），回傳收到的每一幀。work_dir 是放畫面用的空目錄。"""
    command = [str(nbtv_rx), "--json", "--out-dir", str(work_dir)]
    if merge:
        command.append("--merge")
    command.append(str(wav))
    if os.access(_NICE, os.X_OK):
        # 降低優先權：同一台電腦上可能正在即時收發，解碼不該和它搶處理器
        command = [_NICE, "-n", "10", *command]

    proc = await asyncio.create_subprocess_exec(
        *command,
        stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    frames: list[Frame] = []
    ended = False

    async def read_stdout() -> None:
        nonlocal ended
        assert proc.stdout is not None
        async for line in proc.stdout:
            try:
                obj = json.loads(line)
            except ValueError:
                raise DecodeError("decoder_failed", "nbtv-rx 的輸出不是 JSON") from None
            if obj.get("end"):
                ended = True
                continue
            if len(frames) >= MAX_FRAMES:
                raise DecodeError("decoder_failed", "幀數多得不合理")
            frame = _parse_frame(obj)
            frames.append(frame)
            if on_frame is not None:
                on_frame(frame)

    async def read_stderr() -> bytes:
        assert proc.stderr is not None
        data = await proc.stderr.read()
        return data[-2000:]

    stderr_task = asyncio.ensure_future(read_stderr())
    try:
        await asyncio.wait_for(read_stdout(), timeout)
        await asyncio.wait_for(proc.wait(), 10.0)
    except asyncio.TimeoutError:
        raise DecodeError("timeout", "解碼逾時") from None
    finally:
        if proc.returncode is None:
            proc.kill()
            await proc.wait()
        stderr = (await stderr_task).decode("utf-8", "replace").strip()

    if proc.returncode != 0 or not ended:
        raise DecodeError("decoder_failed", f"nbtv-rx 結束碼 {proc.returncode}：{stderr}")

    for frame in frames:
        if frame.picture:
            frame.rgb = _read_ppm(work_dir / f"frame_{frame.index}.ppm")
    return frames
