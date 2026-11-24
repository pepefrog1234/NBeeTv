"""整個網站從頭到尾：開工作、上傳、解碼、發卡、查驗，以及各種該被擋下來的情形。

錄音用專案的 nbtv-tx 與模擬通道合成，網站是真的在本機開起來的，Turnstile 的驗證服務是假的。
"""

from __future__ import annotations

import sqlite3
import time
import unittest
from pathlib import Path

import httpx
import numpy as np

from nbtvweb import provenance
from nbtvweb.cardfile import decode_png
from nbtvweb.signing import certificate_fingerprint, issued_by, load_certificate, load_certificates, verify

from . import util
from .server import GOOD_TOKEN, SECRET, TestServer


def good_recording() -> bytes:
    return util.recording(mode=14, frames=14, channel="awgn", snr=15.0)


class Site:
    """包一層：照頁面的做法操作網站。"""

    def __init__(self, server: TestServer):
        self.server = server
        self.http = httpx.Client(base_url=server.url, timeout=60)

    def create(self, token: str | None = GOOD_TOKEN, listener: dict | None = None, **kwargs) -> httpx.Response:
        body: dict = {}
        if token is not None:
            body["token"] = token
        if listener is not None:
            body["listener"] = listener
        return self.http.post("/api/jobs", json=body, **kwargs)

    def upload(self, job: dict, wav: bytes) -> httpx.Response:
        return self.http.put(job["upload"], content=wav, headers={"content-type": "audio/wav"})

    def wait(self, job_id: str, limit: float = 180.0) -> dict:
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            status = self.http.get(f"/api/jobs/{job_id}").json()
            if status["state"] in ("done", "failed"):
                return status
            time.sleep(0.1)
        raise AssertionError("解碼太久沒有結束")

    def run(self, wav: bytes, listener: dict | None = None) -> dict:
        job = self.create(listener=listener)
        assert job.status_code == 201, job.text
        accepted = self.upload(job.json(), wav)
        assert accepted.status_code == 202, accepted.text
        return self.wait(job.json()["id"])


@unittest.skipUnless(util.have_tools(), "要先建好 build/tools 裡的命令列工具")
class EndToEndTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = TestServer()
        cls.site = Site(cls.server)

    @classmethod
    def tearDownClass(cls):
        cls.site.http.close()
        cls.server.close()

    def test_public_config(self):
        config = self.site.http.get("/api/config").json()
        self.assertEqual(config["station"], "BX4ACP")
        self.assertEqual((config["min_frames"], config["max_seconds"]), (10, 300))
        self.assertEqual(config["turnstile"], {"enabled": True, "site_key": "site-key", "action": "upload"})
        self.assertNotIn(SECRET, self.site.http.get("/api/config").text)  # secret key 不會跑到頁面上

    def test_card_is_issued_signed_and_verifiable(self):
        listener = {"name": "JA1XYZ 山田", "date": "2026-11-23", "time": "12:30", "freq_khz": 7165, "location": "Yokohama"}
        status = self.site.run(good_recording(), listener)
        self.assertEqual(status["state"], "done", status)
        result = status["result"]
        self.assertTrue(result["ok"], result)
        stats = result["stats"]
        self.assertEqual(stats["frames"], 14)
        self.assertGreaterEqual(stats["callsign_frames"], 1)
        self.assertEqual(stats["modes"], {"14": 14})
        serial = result["card"]["serial"]

        # 解出來的畫面總覽
        sheet = self.site.http.get(result["sheet"])
        self.assertEqual(sheet.headers["content-type"], "image/png")

        # 卡片本身：簽章憑證是網站公布的根憑證簽發的，紀錄的簽章驗得過，畫面的雜湊也對
        png = self.site.http.get(result["card"]["png"])
        self.assertEqual(png.status_code, 200)
        self.assertEqual(png.headers["content-type"], "image/png")
        card = decode_png(png.content)
        published = self.site.http.get("/root.pem")
        self.assertIn('filename="BX4ACP-nbtv-root.pem"', published.headers["content-disposition"])
        root = load_certificate(published.content)
        leaf, embedded_root = load_certificates(card.cert_pem)
        self.assertEqual(certificate_fingerprint(embedded_root), certificate_fingerprint(root))
        self.assertTrue(issued_by(leaf, root))
        self.assertTrue(verify(leaf, card.record_text.encode(), card.signature))
        record = card.record
        self.assertEqual(card.size, (1680, 1080))
        self.assertEqual(record["card"], {"width": 1680, "height": 1080, "pixels_sha256": card.pixels_sha256, "c2pa": True})
        config = self.site.http.get("/api/config").json()
        self.assertEqual(config["root_sha256"], certificate_fingerprint(root))
        self.assertEqual(record["signature"], {"algorithm": "ECDSA-P256-SHA256", "certificate_sha256": certificate_fingerprint(leaf),
                                               "root_sha256": certificate_fingerprint(root)})
        self.assertNotIn(b"PRIVATE KEY", published.content)

        # C2PA：以公布的根憑證為信任的起點時是「受信任」，裡面的紀錄和簽過的是同一份
        report = provenance.inspect(png.content, published.content)
        self.assertEqual((report.state, report.failures), ("Trusted", []))
        self.assertEqual(report.record, record)
        self.assertEqual(provenance.inspect(png.content).state, "Valid")   # 不認得我們根憑證的工具：內容完整，簽署者不明

        # 用離線查驗工具從頭驗一次
        import subprocess
        import sys
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "card.png").write_bytes(png.content)
            (Path(tmp) / "root.pem").write_bytes(published.content)
            checked = subprocess.run([sys.executable, str(util.WEB_DIR / "verify_card.py"), str(Path(tmp) / "card.png"),
                                      "--cert", str(Path(tmp) / "root.pem")], capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        for line in ("簽章：正確", "畫面：和簽章時的一樣", "C2PA：正確", "結果：通過"):
            self.assertIn(line, checked.stdout)
        self.assertEqual((record["serial"], record["station"], record["type"]), (serial, "BX4ACP", "nbtv-reception-card"))
        self.assertEqual(record["reception"]["frames"], 14)
        import hashlib
        self.assertEqual(record["reception"]["audio_sha256"], hashlib.sha256(good_recording()).hexdigest())
        self.assertEqual(record["listener"], {"self_reported": True, "name": "JA1XYZ 山田", "date": "2026-11-23",
                                              "time_utc": "12:30", "frequency_khz": 7165.0, "location": "Yokohama"})
        self.assertEqual(record["issuer"], "https://card.example.org")
        self.assertEqual(record["signature"]["certificate_sha256"], config["certificate_sha256"])
        self.assertTrue(config["c2pa"])

        # 用序號查：大小寫、有沒有連字號都可以
        for spelling in (serial, serial.lower(), serial.replace("-", "")):
            found = self.site.http.get(f"/api/cards/{spelling}")
            self.assertEqual(found.status_code, 200, spelling)
            self.assertTrue(found.json()["signature_valid"])
            self.assertEqual(found.json()["record"], record)
        download = self.site.http.get(result["card"]["png"], params={"download": 1})
        self.assertIn(f'filename="BX4ACP-NBTV-{serial}.png"', download.headers["content-disposition"])
        self.assertEqual(self.site.http.get(f"/v/{serial}").status_code, 200)

        # 錄音不留：暫存目錄是空的，資料庫裡只有雜湊
        self.assertEqual(list(self.server.cfg.tmp_dir.iterdir()), [])
        with sqlite3.connect(self.server.cfg.db_path) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM cards WHERE serial = ?", (serial,)).fetchone()[0], 1)

    def test_ten_frames_is_not_enough(self):
        # 門檻是「超過 10 幀」：剛好 10 幀不發，11 幀才發
        ten = self.site.run(util.recording(mode=14, frames=10, channel="awgn", snr=15.0))["result"]
        self.assertEqual((ten["ok"], ten["reason"], ten["stats"]["frames"], ten["card"]), (False, "too_few", 10, None))
        eleven = self.site.run(util.recording(mode=14, frames=11, channel="awgn", snr=15.0))["result"]
        self.assertEqual((eleven["ok"], eleven["stats"]["frames"]), (True, 11))

    def test_other_callsign_gets_no_card(self):
        result = self.site.run(util.recording(mode=14, frames=14, callsign="N0CALL", channel="awgn", snr=15.0))["result"]
        self.assertEqual((result["ok"], result["reason"], result["card"]), (False, "no_callsign", None))
        self.assertEqual(result["stats"]["other_callsigns"], ["N0CALL"])

    def test_noise_gets_no_card(self):
        rng = np.random.default_rng(5)
        result = self.site.run(util.make_wav(rng.normal(0, 0.1, 12000 * 15), 12000))["result"]
        self.assertEqual((result["ok"], result["reason"], result["sheet"]), (False, "no_signal", None))

    def test_bad_files(self):
        self.assertEqual(self.site.run(b"this is not audio" * 100)["error"]["code"], "not_wav")
        too_long = self.site.run(util.make_wav(np.zeros(8000 * 302), 8000))
        self.assertEqual((too_long["state"], too_long["error"]["code"], too_long["error"]["seconds"]), ("failed", "too_long", 302.0))
        self.assertEqual(self.site.run(util.make_wav(np.zeros(4000), 4000))["error"]["code"], "bad_rate")
        job = self.site.create().json()
        self.assertEqual(self.site.upload(job, b"").status_code, 400)

    def test_other_sample_rate_through_the_site(self):
        from scipy import signal
        x, rate = util.wav_samples(good_recording())
        result = self.site.run(util.make_wav(signal.resample_poly(x, 147, 40), 44100, kind="pcm24", channels=2))["result"]
        self.assertTrue(result["ok"], result)

    def test_turnstile_is_enforced(self):
        self.assertEqual(self.site.create(token="forged").status_code, 403)
        self.assertEqual(self.site.create(token=None).json(), {"error": "turnstile_failed"})
        self.assertEqual(self.site.create(token="x" * 5000).status_code, 400)
        before = len(self.server.fake.requests)
        job = self.site.create()
        self.assertEqual(job.status_code, 201)
        sent = self.server.fake.requests[before]
        self.assertEqual(sent, {"secret": SECRET, "response": GOOD_TOKEN})  # 只送這兩樣，沒有訪客的位址
        self.site.upload(job.json(), b"x")  # 把這個工作結束掉，不佔名額
        self.site.wait(job.json()["id"])

    def test_upload_needs_a_job_and_only_once(self):
        wav = good_recording()
        self.assertEqual(self.site.http.put("/api/jobs/" + "0" * 32 + "/audio", content=wav).status_code, 404)
        self.assertEqual(self.site.http.put("/api/jobs/not-a-job/audio", content=wav).status_code, 404)
        job = self.site.create().json()
        self.assertEqual(self.site.upload(job, wav).status_code, 202)
        self.assertEqual(self.site.upload(job, wav).status_code, 409)
        self.site.wait(job["id"])

    def test_bad_listener_fields(self):
        for bad in ({"date": "23/11/2026"}, {"time": "noon"}, {"freq_khz": -5}, {"unknown": 1}):
            response = self.site.create(listener=bad)
            self.assertEqual(response.status_code, 400, bad)
            self.assertEqual(response.json()["error"], "bad_request")
        self.assertEqual(self.site.http.post("/api/jobs", content=b"{" * 20000).status_code, 413)
        self.assertEqual(self.site.http.post("/api/jobs", content=b"not json").status_code, 400)

    def test_unknown_things_are_404(self):
        for path in ("/api/jobs/" + "f" * 32, "/api/cards/0000-0000-0000", "/api/cards/..%2F..%2Fetc", "/card/0000-0000-0000.png",
                     "/card/..%2F..%2Fkeys%2Fsigning-key.pem", "/card/signing-key.pem", "/static/../config.toml", "/docs", "/openapi.json",
                     "/root-key.pem", "/signing-key.pem", "/cert.pem"):
            self.assertEqual(self.site.http.get(path).status_code, 404, path)

    def test_pages_and_headers(self):
        for path in ("/", "/verify", "/v/7KQ2-M9XD-4TFA"):
            page = self.site.http.get(path)
            self.assertEqual(page.status_code, 200, path)
            self.assertIn("text/html", page.headers["content-type"])
            self.assertIn("challenges.cloudflare.com", page.headers["content-security-policy"])
            self.assertEqual(page.headers["x-content-type-options"], "nosniff")
            self.assertEqual(page.headers["x-frame-options"], "DENY")
        self.assertEqual(self.site.http.get("/api/config").headers["cache-control"], "no-store")
        self.assertIn("Disallow: /card/", self.site.http.get("/robots.txt").text)
        self.assertNotIn("server", {k.lower() for k in self.site.http.get("/").headers})  # 不報自己是什麼軟體
        tool = self.site.http.get("/verify_card.py")
        self.assertEqual(tool.status_code, 200)
        self.assertIn("離線查驗", tool.text)


@unittest.skipUnless(util.have_tools(), "要先建好 build/tools 裡的命令列工具")
class LimitTests(unittest.TestCase):
    def serve(self, **settings) -> Site:
        server = TestServer(**settings)
        self.addCleanup(server.close)
        site = Site(server)
        self.addCleanup(site.http.close)
        return site

    def test_upload_size_limit(self):
        site = self.serve(**{"limits.max_upload_mb": 1})
        big = util.make_wav(np.zeros(12000 * 60), 12000)  # 1.4 MB
        job = site.create().json()
        refused = site.upload(job, big)                    # 檔頭寫明了大小：還沒收就拒絕
        self.assertEqual((refused.status_code, refused.json()["error"], refused.json()["limit_mb"]), (413, "too_large", 1))

        def pieces():
            for i in range(0, len(big), 65536):
                yield big[i:i + 65536]

        job = site.create().json()
        streamed = site.http.put(job["upload"], content=pieces())  # 沒有寫明大小：收到超過為止
        self.assertEqual(streamed.status_code, 413)
        self.assertEqual(site.http.get(f"/api/jobs/{job['id']}").json()["error"]["code"], "too_large")
        self.assertEqual(list(site.server.cfg.tmp_dir.iterdir()), [])  # 收到一半的檔案也刪掉了

    def test_queue_limit(self):
        site = self.serve(**{"limits.queue": 1})
        self.assertEqual(site.create().status_code, 201)   # 開了工作但還沒上傳，也佔一個位置
        full = site.create()
        self.assertEqual((full.status_code, full.json()["error"]), (503, "busy"))

    def test_rate_limit_per_address(self):
        site = self.serve(**{"limits.jobs_per_ip": 2, "server.behind_cloudflare": True})
        one, two = {"cf-connecting-ip": "203.0.113.5"}, {"cf-connecting-ip": "2001:db8::1"}
        self.assertEqual(site.create(headers=one).status_code, 201)
        self.assertEqual(site.create(token="forged", headers=one).status_code, 403)  # 沒通過驗證的不佔額度
        self.assertEqual(site.create(headers=one).status_code, 201)
        limited = site.create(headers=one)
        self.assertEqual((limited.status_code, limited.json()["error"]), (429, "rate_limited"))
        self.assertGreater(int(limited.headers["retry-after"]), 0)
        self.assertEqual(site.create(headers=two).status_code, 201)                    # 別的位址不受影響
        self.assertEqual(site.create(headers={"cf-connecting-ip": "2001:db8::2"}).status_code, 201)
        self.assertEqual(site.create(headers={"cf-connecting-ip": "2001:db8::3"}).status_code, 429)  # 同一個 /64

    def test_simultaneous_requests_cannot_beat_the_limit(self):
        import threading
        site = self.serve(**{"limits.jobs_per_ip": 2})
        codes: list[int] = []

        def attempt():
            with httpx.Client(base_url=site.server.url, timeout=30) as http:
                codes.append(http.post("/api/jobs", json={"token": GOOD_TOKEN}).status_code)

        threads = [threading.Thread(target=attempt) for _ in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(sorted(codes), [201, 201] + [429] * 6)

    def test_failed_attempts_are_limited_too(self):
        site = self.serve(**{"limits.jobs_per_ip": 2})
        asked = len(site.server.fake.requests)
        codes = [site.create(token="forged").status_code for _ in range(25)]
        self.assertEqual(codes, [403] * 20 + [429] * 5)                 # 一直亂送權杖：第 21 次起直接擋掉
        self.assertEqual(len(site.server.fake.requests) - asked, 20)    # 擋掉的不再去問 Cloudflare

    def test_one_address_cannot_hold_all_the_slots(self):
        site = self.serve()
        first, second = site.create(), site.create()
        self.assertEqual((first.status_code, second.status_code), (201, 201))
        third = site.create()                                            # 兩份還沒結束，第三份先不收
        self.assertEqual((third.status_code, third.json()["error"]), (429, "already_active"))
        site.upload(first.json(), b"not audio")
        site.wait(first.json()["id"])
        self.assertEqual(site.create().status_code, 201)                # 結束一份就可以再來

    def test_daily_card_limit(self):
        site = self.serve(**{"limits.cards_per_day": 1})
        self.assertTrue(site.run(good_recording())["result"]["ok"])
        second = site.run(good_recording())
        self.assertEqual((second["state"], second["error"]["code"]), ("failed", "daily_limit"))

    def test_turnstile_off_and_unreachable(self):
        off = self.serve(turnstile="off")
        self.assertEqual(off.create(token=None).status_code, 201)
        self.assertEqual(off.http.get("/api/config").json()["turnstile"], {"enabled": False, "site_key": "", "action": "upload"})
        down = self.serve(turnstile="unreachable")
        response = down.create()
        self.assertEqual((response.status_code, response.json()["error"]), (503, "turnstile_unavailable"))

    def test_token_must_be_for_this_site_and_this_action(self):
        # Cloudflare 說權杖有效還不夠：它必須是在我們的網域、為了「上傳」這個動作取得的
        site = self.serve()
        fake = site.server.fake

        def accepted() -> bool:
            job = site.create()
            if job.status_code != 201:
                return False
            site.upload(job.json(), b"x")                  # 把工作結束掉，不佔「同時兩份」的名額
            site.wait(job.json()["id"])
            return True

        self.assertTrue(accepted())
        fake.hostname = "evil.example.net"                 # 同一個 widget 被放在別人的網站上取得的權杖
        refused = site.create()
        self.assertEqual((refused.status_code, refused.json()), (403, {"error": "turnstile_failed"}))
        fake.hostname = "CARD.example.org"                 # 大小寫不算不同
        self.assertTrue(accepted())

        fake.hostname, fake.action = "card.example.org", "login"   # 為了別的動作取得的權杖
        self.assertEqual(site.create().status_code, 403)
        fake.action = ""
        self.assertEqual(site.create().status_code, 403)

    def test_a_token_cannot_be_used_twice(self):
        site = self.serve()
        site.server.fake.single_use = True                 # 像真的 Cloudflare：同一個權杖第二次查證就不認
        self.assertEqual(site.create().status_code, 201)
        replayed = site.create()
        self.assertEqual((replayed.status_code, replayed.json()), (403, {"error": "turnstile_failed"}))

    def test_allowed_hostnames_can_be_listed(self):
        site = self.serve(**{"turnstile.hostnames": ["card.example.org", "www.example.org"]})
        site.server.fake.hostname = "www.example.org"
        self.assertEqual(site.create().status_code, 201)
        site.server.fake.hostname = "example.org"
        self.assertEqual(site.create().status_code, 403)

    def test_visitor_address_goes_to_cloudflare_only_when_cloudflare_supplied_it(self):
        direct = self.serve()
        direct.create(headers={"cf-connecting-ip": "198.51.100.7"})   # 沒有說經過 Cloudflare：不信這個標頭
        self.assertNotIn("remoteip", direct.server.fake.requests[-1])
        behind = self.serve(**{"server.behind_cloudflare": True})
        behind.create(headers={"cf-connecting-ip": "198.51.100.7"})
        self.assertEqual(behind.server.fake.requests[-1]["remoteip"], "198.51.100.7")
        behind.create(headers={"cf-connecting-ip": "not-an-address"})
        self.assertNotIn("remoteip", behind.server.fake.requests[-1])
        behind.create()
        self.assertNotIn("remoteip", behind.server.fake.requests[-1])

    def test_test_keys_skip_the_extra_checks(self):
        # Cloudflare 的測試金鑰回的主機名稱固定是 example.com、沒有 action：只看成不成功
        site = self.serve(**{"turnstile.site_key": "1x00000000000000000000AA"})
        site.server.fake.hostname, site.server.fake.action = "example.com", ""
        self.assertEqual(site.create().status_code, 201)

    def test_c2pa_can_be_turned_off(self):
        site = self.serve(**{"c2pa.enabled": False})
        self.assertFalse(site.http.get("/api/config").json()["c2pa"])
        result = site.run(good_recording())["result"]
        png = site.http.get(result["card"]["png"]).content
        self.assertFalse(decode_png(png).record["card"]["c2pa"])
        self.assertFalse(provenance.inspect(png).present)
        self.assertNotIn(b"caBX", png)

    def test_wrong_secret_is_reported_as_unavailable(self):
        site = self.serve(**{"turnstile.secret_key": "wrong"})
        self.assertEqual(site.create().status_code, 503)

    def test_decoder_timeout(self):
        site = self.serve(**{"limits.decode_timeout_seconds": 0.05})
        status = site.run(good_recording())
        self.assertEqual((status["state"], status["error"]["code"]), ("failed", "internal"))
        self.assertEqual(list(site.server.cfg.tmp_dir.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
