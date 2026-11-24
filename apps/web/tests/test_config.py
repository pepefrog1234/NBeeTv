"""設定檔：範例讀得進來，填錯的地方要有看得懂的訊息。"""

from __future__ import annotations

import base64
import tempfile
import unittest
from pathlib import Path

from nbtvweb.config import ConfigError, load_config

from . import util

EXAMPLE = (util.WEB_DIR / "config.example.toml").read_text(encoding="utf-8")


class ConfigTests(unittest.TestCase):
    def load(self, text: str):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config.toml"
            path.write_text(text, encoding="utf-8")
            return load_config(path)

    def with_keys(self, site: str, secret: str) -> str:
        return EXAMPLE.replace('site_key = ""', f'site_key = "{site}"').replace('secret_key = ""', f'secret_key = "{secret}"')

    def test_example_needs_only_the_two_keys(self):
        with self.assertRaises(ConfigError) as ctx:      # 範例裡金鑰是空的：要說清楚缺什麼
            self.load(EXAMPLE)
        self.assertIn("site_key", str(ctx.exception))
        cfg = self.load(self.with_keys("0x4AAAAAAAexampleSiteKey", "0x4AAAAAAAexampleSecretKeyexample00"))
        self.assertEqual((cfg.callsign, cfg.min_frames, cfg.max_seconds, cfg.port), ("BX4ACP", 10, 300.0, 8088))
        self.assertTrue(cfg.c2pa_enabled)
        self.assertFalse(cfg.turnstile_uses_test_keys)
        self.assertEqual(cfg.keys_dir.name, "keys")

    def test_test_keys_are_recognised(self):
        cfg = self.load(self.with_keys("1x00000000000000000000AA", "1x0000000000000000000000000000000AA"))
        self.assertTrue(cfg.turnstile_uses_test_keys)

    def test_missing_closing_quote_gets_a_hint(self):
        broken = EXAMPLE.replace('secret_key = ""', 'secret_key = "0x4AAAAAAAsomething')
        with self.assertRaises(ConfigError) as ctx:
            self.load(broken)
        self.assertIn("少了結尾的引號", str(ctx.exception))

    def test_tunnel_credentials_are_not_turnstile_keys(self):
        # Cloudflare Tunnel 的權杖是一段 base64 的 JSON；通道的 ID 是 UUID。兩個都不是 Turnstile 的金鑰
        token = base64.b64encode(b'{"a":"account","t":"tunnel","s":"' + b"x" * 90 + b'"}').decode()
        tunnel_id = "3f2b9c1e-7a4d-4e8f-9b1a-5c6d7e8f9a0b"
        for site, secret, word in ((tunnel_id, token, "secret_key"), ("0x4AAAAAAAexampleSiteKey", token, "secret_key"),
                                   (tunnel_id, "0x4AAAAAAAexampleSecretKeyexample00", "site_key")):
            with self.assertRaises(ConfigError) as ctx:
                self.load(self.with_keys(site, secret))
            message = str(ctx.exception)
            self.assertIn(word, message)
            self.assertNotIn(token, message)        # 訊息裡不會把填的值印出來
            self.assertNotIn(tunnel_id, message)

    def test_warnings_for_settings_that_would_break_once_live(self):
        from nbtvweb.config import warnings

        real = self.with_keys("0x4AAAAAAAexampleSiteKey", "0x4AAAAAAAexampleSecretKeyexample00")
        test = self.with_keys("1x00000000000000000000AA", "1x0000000000000000000000000000000AA")
        url = 'public_url = "http://127.0.0.1:8088"'
        self.assertEqual(warnings(self.load(test)), [])                                   # 本機試用：沒事
        self.assertEqual(len(warnings(self.load(real))), 1)                               # 正式金鑰配本機網址
        https_local = warnings(self.load(test.replace(url, 'public_url = "https://127.0.0.1:8088"')))
        self.assertEqual(len(https_local), 1)
        self.assertIn("只講 http", https_local[0])
        public = real.replace(url, 'public_url = "https://card.example.org"')
        direct = warnings(self.load(public))                                              # 對外了卻沒開 behind_cloudflare
        self.assertEqual(len(direct), 1)
        self.assertIn("behind_cloudflare", direct[0])
        self.assertEqual(warnings(self.load(public.replace("behind_cloudflare = false", "behind_cloudflare = true"))), [])

    def test_turnstile_hostnames(self):
        from nbtvweb.config import warnings

        real = self.with_keys("0x4AAAAAAAexampleSiteKey", "0x4AAAAAAAexampleSecretKeyexample00")
        url = 'public_url = "http://127.0.0.1:8088"'
        self.assertEqual(self.load(real).turnstile_hostnames, ("127.0.0.1",))            # 沒有寫：用 public_url 的主機名稱
        live = real.replace(url, 'public_url = "https://Card.Example.org"').replace("behind_cloudflare = false", "behind_cloudflare = true")
        self.assertEqual(self.load(live).turnstile_hostnames, ("card.example.org",))
        listed = live.replace('secret_key = "', 'hostnames = ["card.example.org", "WWW.example.org", "card.example.org"]\nsecret_key = "', 1)
        self.assertEqual(self.load(listed).turnstile_hostnames, ("card.example.org", "www.example.org"))
        self.assertEqual(warnings(self.load(listed)), [])
        # 對外的網址帶著本機的埠號：多半是把網站聽的埠寫進去了
        with_port = warnings(self.load(live.replace("https://Card.Example.org", "https://card.example.org:8088")))
        self.assertEqual(len(with_port), 1)
        self.assertIn("埠號", with_port[0])
        self.assertEqual(self.load(live.replace("https://Card.Example.org", "https://card.example.org:8088")).turnstile_hostnames,
                         ("card.example.org",))
        # 正式上線的清單裡不該有本機
        mixed = warnings(self.load(live.replace('secret_key = "', 'hostnames = ["card.example.org", "localhost"]\nsecret_key = "', 1)))
        self.assertEqual(len(mixed), 1)
        self.assertIn("localhost", mixed[0])
        for bad in ('hostnames = "card.example.org"', 'hostnames = ["https://card.example.org"]', 'hostnames = ["card.example.org:443"]',
                    "hostnames = [1]", 'hostnames = [""]'):
            with self.assertRaises(ConfigError, msg=bad):
                self.load(live.replace('secret_key = "', bad + '\nsecret_key = "', 1))

    def test_typos_are_caught(self):
        base = self.with_keys("0x4AAAAAAAexampleSiteKey", "0x4AAAAAAAexampleSecretKeyexample00")
        for broken in (base.replace("min_frames", "min_frame"), base + "\n[unknown]\nx = 1\n",
                       base.replace('callsign = "BX4ACP"', 'callsign = "not a callsign!"'),
                       base.replace("port = 8088", 'port = "8088"'), base.replace("workers = 2", "workers = 0"),
                       base.replace('public_url = "http://127.0.0.1:8088"', 'public_url = "127.0.0.1:8088"')):
            with self.assertRaises(ConfigError):
                self.load(broken)
        with self.assertRaises(ConfigError):
            load_config("/nonexistent/config.toml")


if __name__ == "__main__":
    unittest.main()
