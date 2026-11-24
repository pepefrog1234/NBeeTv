"""不需要解碼器的部分：發卡條件、序號、流量限制、欄位檢查、畫卡片。（憑證與簽章在 test_signing.py。）"""

from __future__ import annotations

import datetime
import tempfile
import unittest
from pathlib import Path

from PIL import Image
from pydantic import ValidationError

from nbtvweb import card as cardlib
from nbtvweb.cardfile import pixels_sha256
from nbtvweb.decoder import Frame
from nbtvweb.models import JobIn
from nbtvweb.ratelimit import RateLimiter, client_key
from nbtvweb.rules import evaluate
from nbtvweb.serials import new_serial, normalize_serial

RGB = bytes(96 * 72 * 3)


def frame(index, *, callsign="BX4ACP", header=True, picture=True, snr=10.0, mode=14, model_missing=False):
    return Frame(index=index, time=index * 0.667, mode=mode, picture=picture, header=header,
                 callsign=callsign if header else None, sequence=index if header else -1, snr_db=snr,
                 offset_hz=30.0, model_missing=model_missing, rgb=RGB if picture else None)


class RuleTests(unittest.TestCase):
    """條件：有畫面的幀「超過」10 幀，而且至少一幀解出本台的呼號。"""

    def check(self, frames):
        return evaluate(frames, "BX4ACP", 10)

    def test_eleven_frames_pass_ten_do_not(self):
        self.assertTrue(self.check([frame(i) for i in range(11)]).ok)
        verdict = self.check([frame(i) for i in range(10)])
        self.assertFalse(verdict.ok)
        self.assertEqual(verdict.reason, "too_few")

    def test_one_good_header_is_enough(self):
        frames = [frame(i, header=(i == 4)) for i in range(12)]
        verdict = self.check(frames)
        self.assertTrue(verdict.ok)
        self.assertEqual((verdict.counted, verdict.callsign_frames), (12, 1))
        self.assertIs(verdict.best, frames[4])  # 卡片正中放解得出呼號的那一幀

    def test_no_callsign(self):
        verdict = self.check([frame(i, header=False) for i in range(30)])
        self.assertEqual((verdict.ok, verdict.reason), (False, "no_callsign"))

    def test_other_station_does_not_count(self):
        verdict = self.check([frame(i, callsign="N0CALL") for i in range(30)])
        self.assertEqual((verdict.ok, verdict.reason, verdict.counted), (False, "no_callsign", 0))
        self.assertEqual(verdict.other_callsigns, ["N0CALL"])
        mixed = [frame(i, callsign="N0CALL") for i in range(20)] + [frame(20 + i) for i in range(10)]
        verdict = self.check(mixed)
        self.assertEqual((verdict.ok, verdict.reason, verdict.counted), (False, "too_few", 10))

    def test_nothing_received(self):
        verdict = self.check([])
        self.assertEqual((verdict.ok, verdict.reason), (False, "no_signal"))

    def test_frames_without_picture_do_not_count(self):
        frames = [frame(i, picture=(i < 10)) for i in range(20)]
        self.assertEqual(self.check(frames).reason, "too_few")
        missing = [frame(i, picture=False, model_missing=True) for i in range(20)]
        self.assertEqual(self.check(missing).reason, "model_mismatch")

    def test_statistics_and_strip(self):
        frames = [frame(i, snr=float(i), mode=14 if i < 20 else 11) for i in range(25)]
        verdict = self.check(frames)
        self.assertEqual(verdict.modes, {11: 5, 14: 20})
        self.assertEqual((verdict.snr_median, verdict.snr_best), (12.0, 24.0))
        self.assertIs(verdict.best, frames[24])
        self.assertEqual(len(verdict.strip), 5)
        self.assertEqual(len({f.index for f in verdict.strip}), 5)
        self.assertNotIn(verdict.best, verdict.strip)


class SerialTests(unittest.TestCase):
    def test_format_and_uniqueness(self):
        serials = {new_serial() for _ in range(2000)}
        self.assertEqual(len(serials), 2000)
        for s in serials:
            self.assertRegex(s, r"^[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}$")
            self.assertEqual(normalize_serial(s), s)

    def test_normalize(self):
        self.assertEqual(normalize_serial(" 7kq2 m9xd-4tfa "), "7KQ2-M9XD-4TFA")
        self.assertEqual(normalize_serial("OIL0-0000-0000"), "0110-0000-0000")  # 容易看錯的字母當成數字
        for bad in ("", "7KQ2-M9XD", "7KQ2-M9XD-4TFA-X", "../../etc/passwd", "7KQ2-M9XD-4TFU", "x" * 500, None):
            self.assertIsNone(normalize_serial(bad))


class RateLimitTests(unittest.TestCase):
    def test_window(self):
        now = [0.0]
        limiter = RateLimiter(3, 60.0, clock=lambda: now[0])
        for _ in range(3):
            self.assertEqual(limiter.check("a"), 0.0)
            limiter.record("a")
        self.assertAlmostEqual(limiter.check("a"), 60.0)
        self.assertEqual(limiter.check("b"), 0.0)  # 別的位址不受影響
        now[0] = 59.0
        self.assertAlmostEqual(limiter.check("a"), 1.0)
        now[0] = 60.5
        self.assertEqual(limiter.check("a"), 0.0)
        limiter.prune()
        self.assertEqual(limiter._hits, {})

    def test_forget_gives_the_slot_back(self):
        limiter = RateLimiter(1, 60.0, clock=lambda: 5.0)
        stamp = limiter.record("a")
        self.assertGreater(limiter.check("a"), 0.0)
        limiter.forget("a", stamp)
        self.assertEqual(limiter.check("a"), 0.0)
        limiter.forget("a", stamp)      # 再取消一次、或取消沒記過的，都沒事
        limiter.forget("nobody", 1.0)

    def test_client_key(self):
        self.assertEqual(client_key("203.0.113.9"), "203.0.113.9")
        self.assertEqual(client_key("2001:db8:1:2:aaaa::1"), client_key("2001:db8:1:2:bbbb::2"))  # 同一個 /64
        self.assertNotEqual(client_key("2001:db8:1:2::1"), client_key("2001:db8:1:3::1"))
        self.assertEqual(client_key("::ffff:203.0.113.9"), "203.0.113.9")
        self.assertEqual(client_key("not an address"), "unknown")


class ListenerFieldTests(unittest.TestCase):
    def parse(self, **listener):
        import json
        return JobIn.model_validate_json(json.dumps({"token": "t", "listener": listener})).listener

    def test_everything_optional(self):
        listener = JobIn.model_validate_json(b"{}").listener
        self.assertEqual((listener.name, listener.date, listener.time, listener.freq_khz, listener.location), ("", "", "", None, ""))

    def test_text_is_cleaned(self):
        listener = self.parse(name="  JA1XYZ‮\n\t山田\u0000  太郎 ", location="A" * 100)
        self.assertEqual(listener.name, "JA1XYZ 山田 太郎")
        self.assertEqual(len(listener.location), 60)

    def test_bad_values_are_rejected(self):
        tomorrow = (datetime.datetime.now(datetime.timezone.utc).date() + datetime.timedelta(days=3)).isoformat()
        for bad in ({"date": "2026/11/23"}, {"date": tomorrow}, {"date": "2026-13-40"}, {"time": "25:00"}, {"time": "9:5"},
                    {"freq_khz": 1}, {"freq_khz": 1e12}, {"freq_khz": "abc"}, {"name": "x" * 1000}, {"extra": 1}):
            with self.assertRaises(ValidationError, msg=str(bad)):
                self.parse(**bad)
        with self.assertRaises(ValidationError):
            JobIn.model_validate_json(b'{"token": "' + b"x" * 3000 + b'"}')

    def test_good_values(self):
        listener = self.parse(name="BV2XX", date="2026-11-23", time="12:30", freq_khz=7165.5, location="Taipei")
        self.assertEqual((listener.date, listener.time, listener.freq_khz), ("2026-11-23", "12:30", 7165.5))


class CardRenderTests(unittest.TestCase):
    def test_render_is_deterministic_and_sized(self):
        fonts = cardlib.find_fonts()
        content = cardlib.CardContent(
            callsign="BX4ACP", serial="7KQ2-M9XD-4TFA", verify_url="https://example.org/v/7KQ2-M9XD-4TFA",
            verify_hint="example.org/verify", issued_at="2026-11-24 11:02 UTC", fingerprint="ab" * 32,
            listener_name="山田", modes="14", frames=12, snr="8.0 dB", picture=RGB, picture_caption="x", strip=[RGB] * 9,
        )
        background = cardlib.default_background()
        first = cardlib.render(content, background, fonts)
        self.assertEqual(first.size, (cardlib.CARD_WIDTH, cardlib.CARD_HEIGHT))
        self.assertEqual(pixels_sha256(first), pixels_sha256(cardlib.render(content, background, fonts)))
        empty = cardlib.CardContent(callsign="BX4ACP", serial="0000-0000-0000", verify_url="http://x/v/0", verify_hint="x",
                                    issued_at="", fingerprint="")
        cardlib.render(empty, Image.new("RGB", (300, 200), (255, 255, 255)), fonts)  # 什麼都沒填、底圖大小不對也畫得出來

    def test_backgrounds_are_picked_by_serial(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.assertIsNone(cardlib.pick_background(directory, "A"))
            for name in ("a.jpg", "b.png", "notes.txt", ".hidden.jpg"):
                (directory / name).write_bytes(b"")
            picks = {cardlib.pick_background(directory, new_serial()).name for _ in range(200)}
            self.assertEqual(picks, {"a.jpg", "b.png"})
            self.assertEqual(cardlib.pick_background(directory, "S"), cardlib.pick_background(directory, "S"))
            # 壞掉的照片不會讓發卡失敗：改用內建的底圖
            self.assertEqual(cardlib.load_background(directory / "a.jpg").size, (cardlib.CARD_WIDTH, cardlib.CARD_HEIGHT))

    def test_qr_code_reads_back(self):
        image = cardlib.qr_image("https://example.org/v/7KQ2-M9XD-4TFA", 240)
        self.assertEqual(image.size, (240, 240))
        self.assertEqual(image.getpixel((2, 2)), (255, 255, 255))  # 四周留白

    def test_frequency_and_text(self):
        self.assertEqual(cardlib.format_frequency(7165.0), "7165 kHz")
        self.assertEqual(cardlib.format_frequency(7165.5), "7165.5 kHz")
        self.assertEqual(cardlib.format_frequency(430300.0), "430.300 MHz")
        self.assertEqual(cardlib.format_frequency(None), "")
        self.assertEqual(cardlib.clean_text(" a​\u0007 b\n\nc ", 10), "a b c")


if __name__ == "__main__":
    unittest.main()
