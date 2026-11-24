"""測試用：在本機開一個真的網站（uvicorn），以及一個假的 Turnstile 驗證服務。"""

from __future__ import annotations

import json
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs

import uvicorn

from nbtvweb.app import create_app, server_config
from nbtvweb.config import load_config
from nbtvweb.signing import create_identity

from . import util

GOOD_TOKEN = "good-token"
SECRET = "test-secret"


class FakeTurnstile:
    """假的 siteverify：權杖是 good-token、secret 也對才算通過；每個權杖只能用一次。"""

    def __init__(self):
        self.requests: list[dict] = []
        self.single_use = False
        # 成功時回報的：權杖是在哪個主機名稱、為了哪個動作取得的（測試可以改掉其中一個）
        self.hostname = "card.example.org"
        self.action = "upload"
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802
                body = self.rfile.read(int(self.headers.get("content-length", "0"))).decode()
                form = {k: v[0] for k, v in parse_qs(body).items()}
                outer.requests.append(form)
                if form.get("secret") != SECRET:
                    reply = {"success": False, "error-codes": ["invalid-input-secret"]}
                elif form.get("response") != GOOD_TOKEN:
                    reply = {"success": False, "error-codes": ["invalid-input-response"]}
                elif outer.single_use and sum(1 for r in outer.requests if r.get("response") == GOOD_TOKEN) > 1:
                    reply = {"success": False, "error-codes": ["timeout-or-duplicate"]}
                else:
                    reply = {"success": True, "error-codes": [], "hostname": outer.hostname, "action": outer.action}
                data = json.dumps(reply).encode()
                self.send_response(200)
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}/siteverify"
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class TestServer:
    __test__ = False

    def __init__(self, *, turnstile: str = "fake", **overrides):
        """overrides 是要改的設定，寫成「段落.項目」＝值，例如 {"limits.queue": 1}。"""
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.fake = FakeTurnstile() if turnstile == "fake" else None
        settings: dict[str, dict] = {
            "station": {"callsign": "BX4ACP"},
            "rules": {"min_frames": 10, "max_seconds": 300},
            "server": {"public_url": "https://card.example.org"},
            "limits": {"jobs_per_ip": 1000, "workers": 2},
            "decoder": {"nbtv_rx": str(util.TOOLS / "nbtv-rx")},
            "turnstile": {"enabled": turnstile != "off", "site_key": "site-key", "secret_key": SECRET,
                          "verify_url": self.fake.url if self.fake else "http://127.0.0.1:9/siteverify"},
            "card": {"backgrounds": str(self.dir / "backgrounds")},
            "c2pa": {"enabled": True},
            "paths": {"data": str(self.dir / "data")},
        }
        for key, value in overrides.items():
            section, _, name = key.partition(".")
            settings[section][name] = value
        lines = []
        for section, values in settings.items():
            lines.append(f"[{section}]")
            lines += [f"{name} = {json.dumps(value)}" for name, value in values.items()]
        (self.dir / "config.toml").write_text("\n".join(lines) + "\n", encoding="utf-8")
        self.cfg = load_config(self.dir / "config.toml")
        create_identity(self.cfg.keys_dir, self.cfg.callsign)
        self.app = create_app(self.cfg)
        self.server = uvicorn.Server(server_config(self.app, self.cfg, port=0, log_level="warning"))
        self.thread = threading.Thread(target=self.server.run, daemon=True)
        self.thread.start()
        deadline = time.monotonic() + 20
        while not self.server.started:
            if time.monotonic() > deadline or not self.thread.is_alive():
                raise RuntimeError("測試用的網站沒有起來")
            time.sleep(0.02)
        port = self.server.servers[0].sockets[0].getsockname()[1]
        self.url = f"http://127.0.0.1:{port}"

    def close(self):
        self.server.should_exit = True
        self.thread.join(timeout=20)
        if self.fake:
            self.fake.close()
        self.tmp.cleanup()
