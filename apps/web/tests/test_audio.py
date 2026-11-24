"""音檔的解析與轉檔：各種格式都要轉得出解碼器解得開的 12 kHz 單聲道，壞檔案要被擋下來。"""

from __future__ import annotations

import asyncio
import tempfile
import unittest
from pathlib import Path

import numpy as np
from scipy import signal

from nbtvweb.audio import AudioError, load_wav, write_modem_wav
from nbtvweb.decoder import decode

from . import util


def run_decoder(samples: np.ndarray):
    with tempfile.TemporaryDirectory() as tmp:
        wav = Path(tmp) / "in.wav"
        out = Path(tmp) / "out"
        out.mkdir()
        write_modem_wav(wav, samples)
        return asyncio.run(decode(util.TOOLS / "nbtv-rx", wav, out, merge=True, timeout=120))


class ContainerTests(unittest.TestCase):
    def tone(self, rate: int, seconds: float = 0.5) -> np.ndarray:
        t = np.arange(int(rate * seconds)) / rate
        return 0.5 * np.sin(2 * np.pi * 1000.0 * t)

    def test_rejects_non_wav(self):
        for data in (b"", b"RIFF", b"ID3\x04" + b"\0" * 64, b"RIFF\x24\0\0\0AVI " + b"\0" * 64, b"fLaC" + b"\0" * 64):
            with self.assertRaises(AudioError) as ctx:
                load_wav(data, 300)
            self.assertEqual(ctx.exception.code, "not_wav")

    def test_rejects_too_long(self):
        data = util.make_wav(np.zeros(8000 * 12), 8000)
        with self.assertRaises(AudioError) as ctx:
            load_wav(data, 10)
        self.assertEqual(ctx.exception.code, "too_long")
        self.assertEqual(ctx.exception.detail["seconds"], 12.0)
        load_wav(util.make_wav(np.zeros(8000 * 10 + 4000), 8000), 10)  # 多出半秒在寬限內

    def test_rejects_bad_rate_and_encoding(self):
        with self.assertRaises(AudioError) as ctx:
            load_wav(util.make_wav(self.tone(4000), 4000), 300)
        self.assertEqual(ctx.exception.code, "bad_rate")
        data = bytearray(util.make_wav(self.tone(8000), 8000))
        data[20:22] = (6).to_bytes(2, "little")  # A-law
        with self.assertRaises(AudioError) as ctx:
            load_wav(bytes(data), 300)
        self.assertEqual(ctx.exception.code, "unsupported")

    def test_rejects_empty(self):
        with self.assertRaises(AudioError) as ctx:
            load_wav(util.make_wav(np.zeros(0), 12000), 300)
        self.assertEqual(ctx.exception.code, "empty")

    def test_truncated_and_garbage_never_crash(self):
        good = util.make_wav(self.tone(12000), 12000, kind="pcm24", channels=2, extensible=True)
        rng = np.random.default_rng(1)
        for cut in range(0, len(good), 97):
            try:
                load_wav(good[:cut], 300)
            except AudioError:
                pass
        for _ in range(300):
            data = bytearray(good[:400])
            for _ in range(8):
                data[int(rng.integers(len(data)))] = int(rng.integers(256))
            try:
                load_wav(bytes(data), 300)
            except AudioError:
                pass

    def test_unfinished_recording_size_fields(self):
        x = self.tone(12000)
        for size in (0, 0xFFFFFFFF, 10**9):
            audio = load_wav(util.make_wav(x, 12000, data_size=size), 300)
            self.assertEqual(len(audio.samples), len(x))

    def test_formats_give_the_same_signal(self):
        x = self.tone(12000)
        reference = load_wav(util.make_wav(x, 12000, kind="float64"), 300).samples
        for kind in ("pcm8", "pcm16", "pcm24", "pcm32", "float32"):
            for extensible in (False, True):
                audio = load_wav(util.make_wav(x, 12000, kind=kind, channels=2, extensible=extensible), 300)
                self.assertEqual(audio.source_channels, 2)
                self.assertEqual(len(audio.samples), len(x))
                tolerance = 0.02 if kind == "pcm8" else 1e-3
                self.assertLess(float(np.max(np.abs(audio.samples - reference))), tolerance, kind)

    def test_level_is_normalised_and_finite(self):
        x = self.tone(12000)
        for gain in (1e-4, 1.0, 40.0):
            audio = load_wav(util.make_wav(x * gain, 12000, kind="float32"), 300)
            rms = float(np.sqrt(np.mean(audio.samples.astype(np.float64) ** 2)))
            self.assertAlmostEqual(rms, 0.1, places=3)
        bad = x.copy()
        bad[100], bad[200], bad[300] = np.nan, np.inf, -np.inf
        audio = load_wav(util.make_wav(bad, 12000, kind="float32"), 300)
        self.assertTrue(np.all(np.isfinite(audio.samples)))
        silent = load_wav(util.make_wav(np.zeros(12000), 12000), 300)
        self.assertEqual(float(np.max(np.abs(silent.samples))), 0.0)

    def test_picks_the_channel_with_sound(self):
        x = self.tone(12000)
        stereo = np.zeros((len(x), 2), dtype="<i2")
        stereo[:, 1] = np.round(x * 32767)
        import struct
        raw = stereo.tobytes()
        data = b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, 12000, 48000, 4, 16) + b"data" + struct.pack("<I", len(raw)) + raw
        audio = load_wav(b"RIFF" + struct.pack("<I", len(data)) + data, 300)
        self.assertGreater(float(np.max(np.abs(audio.samples))), 0.1)


@unittest.skipUnless(util.have_tools(), "要先建好 build/tools 裡的命令列工具")
class DecodeTests(unittest.TestCase):
    """聽眾的錄音會是各種取樣率與格式：轉檔之後都要解得出一樣多的幀。"""

    @classmethod
    def setUpClass(cls):
        cls.wav = util.recording(mode=12, frames=8, snr=10.0)
        cls.x, cls.rate = util.wav_samples(cls.wav)

    def check(self, data: bytes):
        frames = run_decoder(load_wav(data, 300).samples)
        self.assertEqual(len(frames), 8)
        self.assertTrue(all(f.picture and f.header and f.callsign == "BX4ACP" for f in frames))
        self.assertTrue(all(f.rgb is not None and len(f.rgb) == 96 * 72 * 3 for f in frames))
        self.assertEqual([f.mode for f in frames], [12] * 8)
        return frames

    def test_original(self):
        frames = self.check(self.wav)
        self.assertEqual([f.sequence for f in frames], list(range(8)))
        self.assertAlmostEqual(frames[0].offset_hz, 40.0, delta=3.0)

    def test_other_sample_rates_and_formats(self):
        cases = [(44100, "pcm16", 2), (48000, "pcm24", 1), (8000, "pcm16", 1), (11025, "pcm8", 1),
                 (22050, "float32", 2), (96000, "pcm32", 1), (7119, "pcm16", 1)]
        for rate, kind, channels in cases:
            with self.subTest(rate=rate, kind=kind):
                import math
                g = math.gcd(rate, self.rate)
                y = signal.resample_poly(self.x, rate // g, self.rate // g)
                self.check(util.make_wav(y, rate, kind=kind, channels=channels, extensible=channels > 1))

    def test_noise_gives_nothing(self):
        rng = np.random.default_rng(7)
        frames = run_decoder(load_wav(util.make_wav(rng.normal(0, 0.1, 12000 * 20), 12000), 300).samples)
        self.assertEqual(frames, [])


if __name__ == "__main__":
    unittest.main()
