"""讀使用者上傳的 WAV，整理成解碼器吃的樣子：12 kHz、單聲道、32 位元浮點。

上傳的檔案是陌生人給的，所以容器（RIFF 的區塊）在這裡用 Python 自己拆、每個欄位都檢查，
C++ 的工具只會讀到這個模組自己寫出去的檔案。
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from scipy import signal

MODEM_RATE = 12000

# 訊號佔 352–2648 Hz：取樣率低於 6 kHz 就裝不下；上限只是擋掉明顯不合理的檔頭
MIN_RATE = 6000
MAX_RATE = 192000
MAX_CHANNELS = 8


class AudioError(Exception):
    """上傳的檔案不能用。code 是給前端翻成訊息用的代號，detail 是訊息裡要帶的數字。"""

    def __init__(self, code: str, **detail):
        super().__init__(code)
        self.code = code
        self.detail = detail


@dataclass
class Audio:
    samples: np.ndarray   # float32、12 kHz、單聲道
    seconds: float        # 原始錄音的長度
    source_rate: int
    source_channels: int
    source_format: str    # 例如 "PCM 16"、"float 32"


def _parse_container(data: bytes) -> tuple[int, int, int, int, memoryview]:
    """拆 RIFF：回傳（格式代號、聲道數、取樣率、每個取樣的位元組數、取樣資料）。"""
    if len(data) < 12 or data[8:12] != b"WAVE" or data[0:4] != b"RIFF":
        raise AudioError("not_wav")
    fmt = None
    pos = 12
    while pos + 8 <= len(data):
        chunk_id = bytes(data[pos:pos + 4])
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if chunk_id == b"fmt ":
            if size < 16 or body + 16 > len(data):
                raise AudioError("not_wav")
            tag, channels, rate, _byte_rate, block_align, _bits = struct.unpack_from("<HHIIHH", data, body)
            if tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE：真正的格式在子格式 GUID 的前兩個位元組
                if size < 40 or body + 40 > len(data):
                    raise AudioError("not_wav")
                tag = struct.unpack_from("<H", data, body + 24)[0]
            fmt = (tag, channels, rate, block_align)
        elif chunk_id == b"data":
            if fmt is None:
                raise AudioError("not_wav")
            tag, channels, rate, block_align = fmt
            if not (1 <= channels <= MAX_CHANNELS):
                raise AudioError("unsupported", what="channels", value=channels)
            if not (MIN_RATE <= rate <= MAX_RATE):
                raise AudioError("bad_rate", rate=rate)
            if block_align == 0 or block_align % channels != 0:
                raise AudioError("not_wav")
            width = block_align // channels
            available = len(data) - body
            # 錄音程式沒有正常收尾時，長度會是 0 或全 1，或比檔案還長：有多少算多少
            if size == 0 or size == 0xFFFFFFFF or size > available:
                size = available
            return tag, channels, rate, width, memoryview(data)[body:body + size]
        pos = body + size + (size & 1)
    raise AudioError("not_wav")


_FORMATS = {
    # （格式代號, 每個取樣的位元組數）：名稱
    (1, 1): "PCM 8", (1, 2): "PCM 16", (1, 3): "PCM 24", (1, 4): "PCM 32", (3, 4): "float 32", (3, 8): "float 64",
}


def _channel(tag: int, width: int, channels: int, frames: int, raw: memoryview, c: int) -> np.ndarray:
    """取出第 c 個聲道，變成 float32（整數格式的滿刻度是 ±1），直流已經去掉。

    一次只轉一個聲道：八聲道的檔案如果整份轉成浮點，記憶體會是檔案的好幾倍。
    """
    count = frames * channels
    if tag == 1 and width == 1:
        x = (np.frombuffer(raw, dtype=np.uint8, count=count).reshape(frames, channels)[:, c].astype(np.float32) - 128.0) / 128.0
    elif tag == 1 and width == 2:
        x = np.frombuffer(raw, dtype="<i2", count=count).reshape(frames, channels)[:, c].astype(np.float32) / 32768.0
    elif tag == 1 and width == 3:
        b = np.frombuffer(raw, dtype=np.uint8, count=count * 3).reshape(frames, channels, 3)[:, c, :].astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = ((v ^ 0x800000) - 0x800000).astype(np.float32) / 8388608.0
    elif tag == 1 and width == 4:
        x = np.frombuffer(raw, dtype="<i4", count=count).reshape(frames, channels)[:, c].astype(np.float32) / 2147483648.0
    elif tag == 3 and width == 4:
        x = np.frombuffer(raw, dtype="<f4", count=count).reshape(frames, channels)[:, c].astype(np.float32)
    else:  # tag == 3 and width == 8
        with np.errstate(over="ignore"):  # 超出 float32 範圍的值會變成無限大，下面一併清掉
            x = np.frombuffer(raw, dtype="<f8", count=count).reshape(frames, channels)[:, c].astype(np.float32)
    # 浮點格式裡可能有 NaN 或無限大
    np.nan_to_num(x, copy=False, nan=0.0, posinf=0.0, neginf=0.0)
    x -= np.float32(x.mean(dtype=np.float64))
    return x


def load_wav(data: bytes, max_seconds: float) -> Audio:
    """把上傳的 WAV 內容變成 12 kHz 單聲道。不能用的檔案丟 AudioError。"""
    tag, channels, rate, width, raw = _parse_container(data)
    name = _FORMATS.get((tag, width))
    if name is None:
        raise AudioError("unsupported", what="encoding", value=tag, bits=width * 8)
    frames = len(raw) // (width * channels)
    if frames == 0:
        raise AudioError("empty")
    seconds = frames / rate
    if seconds > max_seconds + 1.0:  # 手動按停的錄音常常多出零點幾秒，留一秒的寬限
        raise AudioError("too_long", seconds=round(seconds, 1), limit=max_seconds)

    # 多聲道時取能量最大的那一個：有些錄音只有一邊有聲音，兩邊相加也可能互相抵消
    mono = _channel(tag, width, channels, frames, raw, 0)
    if channels > 1:
        loudest = float(np.dot(mono, mono))
        for c in range(1, channels):
            other = _channel(tag, width, channels, frames, raw, c)
            energy = float(np.dot(other, other))
            if energy > loudest:
                mono, loudest = other, energy
            del other

    if rate != MODEM_RATE:
        g = math.gcd(MODEM_RATE, rate)
        mono = signal.resample_poly(mono, MODEM_RATE // g, rate // g).astype(np.float32)

    # 音量整理到解碼器習慣的範圍：均方根 0.1，但峰值不超過 1
    peak = float(np.max(np.abs(mono))) if mono.size else 0.0
    if peak > 0.0 and math.isfinite(peak):
        rms = float(np.sqrt(np.mean(np.square(mono, dtype=np.float64))))
        mono = mono * np.float32(min(0.1 / rms, 1.0 / peak))
    else:
        mono = np.zeros_like(mono)

    return Audio(samples=mono, seconds=seconds, source_rate=rate, source_channels=channels, source_format=name)


def write_modem_wav(path: Path, samples: np.ndarray) -> None:
    """寫成 nbtv-rx 讀得懂的檔案：12 kHz、單聲道、32 位元浮點。"""
    body = np.asarray(samples, dtype="<f4").tobytes()
    header = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 3, 1, MODEM_RATE, MODEM_RATE * 4, 4, 32
    ) + b"data" + struct.pack("<I", len(body))
    with open(path, "wb") as f:
        f.write(header)
        f.write(body)
