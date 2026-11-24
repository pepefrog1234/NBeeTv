"""測試共用：找專案裡建好的命令列工具、合成錄音、寫各種格式的 WAV。"""

from __future__ import annotations

import struct
import subprocess
import tempfile
from functools import lru_cache
from pathlib import Path

import numpy as np

WEB_DIR = Path(__file__).resolve().parent.parent
REPO = WEB_DIR.parent.parent
TOOLS = REPO / "build" / "tools"
IMAGES = [REPO / "core" / "tests" / "data" / name for name in ("coffee.ppm", "cat.ppm", "rocket.ppm")]


def have_tools() -> bool:
    return all((TOOLS / name).exists() for name in ("nbtv-tx", "nbtv-chan", "nbtv-rx"))


@lru_cache(maxsize=None)
def recording(mode: int = 14, frames: int = 14, callsign: str = "BX4ACP", channel: str = "poor",
              snr: float = 12.0, offset: float = 40.0) -> bytes:
    """用 nbtv-tx 與模擬通道合成一段 12 kHz 的錄音，回傳 WAV 檔的內容。"""
    with tempfile.TemporaryDirectory() as tmp:
        tx, rx = Path(tmp) / "tx.wav", Path(tmp) / "rx.wav"
        subprocess.run(
            [str(TOOLS / "nbtv-tx"), "--mode", str(mode), "--call", callsign, "--frames", str(frames), "--rate", "12000",
             "--out", str(tx), *map(str, IMAGES)],
            check=True, capture_output=True,
        )
        subprocess.run(
            [str(TOOLS / "nbtv-chan"), "--channel", channel, "--snr", str(snr), "--offset", str(offset), str(tx), str(rx)],
            check=True, capture_output=True,
        )
        return rx.read_bytes()


def wav_samples(data: bytes) -> tuple[np.ndarray, int]:
    """讀本專案工具寫的 WAV（16 位元單聲道）：回傳 float 取樣與取樣率。"""
    assert data[:4] == b"RIFF" and data[36:40] == b"data"
    rate = struct.unpack_from("<I", data, 24)[0]
    return np.frombuffer(data, dtype="<i2", offset=44).astype(np.float64) / 32768.0, rate


def make_wav(x: np.ndarray, rate: int, *, kind: str = "pcm16", channels: int = 1, extensible: bool = False,
             data_size: int | None = None) -> bytes:
    """把單聲道的取樣寫成指定格式的 WAV。channels 大於 1 時每個聲道放一樣的內容。"""
    x = np.asarray(x, dtype=np.float64)
    if kind == "pcm8":
        body, tag, width = (np.clip(np.round(x * 127.0) + 128, 0, 255)).astype(np.uint8), 1, 1
    elif kind == "pcm16":
        body, tag, width = np.clip(np.round(x * 32767.0), -32768, 32767).astype("<i2"), 1, 2
    elif kind == "pcm24":
        v = np.clip(np.round(x * 8388607.0), -8388608, 8388607).astype(np.int32)
        body = np.stack([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF], axis=-1).astype(np.uint8)
        tag, width = 1, 3
    elif kind == "pcm32":
        body, tag, width = np.clip(np.round(x * 2147483647.0), -2147483648, 2147483647).astype("<i4"), 1, 4
    elif kind == "float32":
        body, tag, width = x.astype("<f4"), 3, 4
    elif kind == "float64":
        body, tag, width = x.astype("<f8"), 3, 8
    else:
        raise ValueError(kind)
    per_sample = body.reshape(len(x), 3 if kind == "pcm24" else 1)  # 24 位元是每個取樣三個位元組
    interleaved = np.repeat(per_sample[:, None, :], channels, axis=1)
    raw = interleaved.tobytes()
    block = width * channels
    if extensible:
        guid_tail = bytes.fromhex("000000001000800000aa00389b71")
        fmt = struct.pack("<HHIIHH", 0xFFFE, channels, rate, rate * block, block, width * 8)
        fmt += struct.pack("<HHI", 22, width * 8, 0) + struct.pack("<H", tag) + guid_tail
    else:
        fmt = struct.pack("<HHIIHH", tag, channels, rate, rate * block, block, width * 8)
    size = len(raw) if data_size is None else data_size
    out = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
    out += b"LIST" + struct.pack("<I", 4) + b"INFO"          # 夾一個不相干的區塊
    out += b"data" + struct.pack("<I", size) + raw
    return b"RIFF" + struct.pack("<I", len(out)) + out
