"""網站本身：頁面、上傳與查詢的網址。"""

from __future__ import annotations

import asyncio
import base64
import ipaddress
import json
import logging
import re
import subprocess
from contextlib import asynccontextmanager
from pathlib import Path
from urllib.parse import quote

from fastapi import FastAPI, Request
from fastapi.responses import FileResponse, JSONResponse, PlainTextResponse, RedirectResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import ValidationError
from starlette.requests import ClientDisconnect

from . import __version__
from .card import find_fonts
from .config import Config, ConfigError
from .config import warnings as config_warnings
from .issue import Issuer
from .jobs import JobError, JobManager
from .models import LOCATION_LIMIT, NAME_LIMIT, JobIn
from .provenance import C2paEmbedder, ProvenanceError
from .ratelimit import RateLimiter, client_key
from .serials import normalize_serial
from .signing import SIGNING_KEY, Signer, verify
from .store import Store
from .turnstile import ACTION as TURNSTILE_ACTION
from .turnstile import Turnstile

log = logging.getLogger(__name__)

STATIC_DIR = Path(__file__).resolve().parent.parent / "static"
JSON_BODY_LIMIT = 8 * 1024
JSON_BODY_SECONDS = 15
_JOB_ID = re.compile(r"^[0-9a-f]{32}$")

# 頁面只載入自己的檔案，外加 Cloudflare Turnstile 的程式與它的框架
CONTENT_SECURITY_POLICY = (
    "default-src 'self'; "
    "script-src 'self' https://challenges.cloudflare.com; "
    "frame-src https://challenges.cloudflare.com; "
    "img-src 'self' data: blob:; "
    "style-src 'self'; "
    "connect-src 'self'; "
    "object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'"
)
SECURITY_HEADERS = [
    (b"content-security-policy", CONTENT_SECURITY_POLICY.encode()),
    (b"x-content-type-options", b"nosniff"),
    (b"referrer-policy", b"same-origin"),
    (b"x-frame-options", b"DENY"),
]


class SecurityHeaders:
    """每個回應都加上安全相關的標頭。"""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        async def send_with_headers(message):
            if message["type"] == "http.response.start":
                headers = list(message.get("headers", []))
                present = {name.lower() for name, _ in headers}
                headers += [(name, value) for name, value in SECURITY_HEADERS if name not in present]
                if scope["path"].startswith("/api/") and b"cache-control" not in present:
                    headers.append((b"cache-control", b"no-store"))
                message = {**message, "headers": headers}
            await send(message)

        await self.app(scope, receive, send_with_headers)


# 頁面與靜態檔的網址都不吃參數。前面的 Cloudflare 是照整個網址（含 ? 後面的參數）快取的：隨便加一個參數，
# 每一次請求都會回到這台電腦重新讀一次檔案，等於不花力氣就能耗掉這裡的上傳頻寬。
# 所以帶了參數的請求只回一個轉址（幾百個位元組），轉到不帶參數的網址。卡片圖檔的網址在它自己的函式裡處理。
NO_QUERY_PATHS = ("/", "/verify", "/robots.txt", "/root.pem", "/verify_card.py")
NO_QUERY_PREFIXES = ("/static/", "/v/")
REDIRECT_CACHE = "public, max-age=86400"


class CanonicalUrls:
    """不吃參數的網址帶了參數：轉到不帶參數的那一個。"""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] == "http" and scope["method"] in ("GET", "HEAD") and scope.get("query_string"):
            path = scope["path"]
            # 這些路徑都是一個斜線開頭，轉址的目的地不會被當成別的網站
            if path in NO_QUERY_PATHS or path.startswith(NO_QUERY_PREFIXES):
                response = RedirectResponse(quote(path), status_code=301, headers={"cache-control": REDIRECT_CACHE})
                await response(scope, receive, send)
                return
        await self.app(scope, receive, send)


def error(status: int, code: str, **detail) -> JSONResponse:
    return JSONResponse({"error": code, **detail}, status_code=status)


def check_decoder(cfg: Config) -> None:
    """確認 nbtv-rx 在，而且是有 --json 的版本。"""
    if not cfg.nbtv_rx.is_file():
        raise ConfigError(f"找不到解碼器 {cfg.nbtv_rx}；先在專案根目錄執行 cmake -S . -B build && cmake --build build")
    try:
        out = subprocess.run([str(cfg.nbtv_rx), "--help"], capture_output=True, timeout=20).stdout.decode("utf-8", "replace")
    except (OSError, subprocess.SubprocessError) as e:
        raise ConfigError(f"解碼器 {cfg.nbtv_rx} 執行不了：{e}") from None
    if "--json" not in out:
        raise ConfigError(f"解碼器 {cfg.nbtv_rx} 是舊的（沒有 --json）；重新執行 cmake --build build")


def server_config(app: FastAPI, cfg: Config, **overrides):
    """uvicorn 的設定。正式執行與測試都從這裡拿，兩邊才會一樣。"""
    import uvicorn

    options: dict = {
        "host": cfg.host,
        "port": cfg.port,
        "log_level": "info",
        "access_log": False,      # 每秒一次的進度查詢會洗版
        "server_header": False,   # 不報自己是什麼軟體
        "timeout_keep_alive": 20,
    }
    options.update(overrides)
    return uvicorn.Config(app, **options)


def create_app(cfg: Config) -> FastAPI:
    check_decoder(cfg)
    signer = Signer(cfg.keys_dir)
    store = Store(cfg.db_path, cfg.cards_dir)
    try:
        embedder = C2paEmbedder(signer) if cfg.c2pa_enabled else None
        if embedder is not None:
            embedder.self_test()
    except ProvenanceError as e:
        raise ConfigError(str(e)) from None
    issuer = Issuer(cfg, signer, store, find_fonts(cfg.font_cjk, cfg.font_latin, cfg.font_mono), embedder)
    jobs = JobManager(cfg, issuer)
    turnstile = Turnstile(cfg)
    limiter = RateLimiter(cfg.jobs_per_ip, cfg.window_seconds)
    # 沒通過驗證的請求不佔上面的額度，但每一次都要去問 Cloudflare，所以另外限制嘗試的次數
    attempts = RateLimiter(max(20, cfg.jobs_per_ip * 5), cfg.window_seconds)

    if not cfg.turnstile_enabled:
        log.warning("Turnstile 沒有啟用：任何程式都可以直接上傳。只適合在本機試用")
    elif cfg.turnstile_uses_test_keys:
        log.warning("Turnstile 用的是 Cloudflare 的測試金鑰：驗證一律通過，等於沒有防護。公開之前要換成自己的金鑰")
    for warning in config_warnings(cfg):
        log.warning("%s", warning)
    if signer.key_file_exposed:
        log.warning("簽章憑證的私鑰（%s 裡的 %s）別的使用者也讀得到；建議 chmod 600", cfg.keys_dir, SIGNING_KEY)
    if signer.root_key_present:
        log.info("根憑證的私鑰還放在 %s：網站用不到它，可以移到別的地方保管", cfg.keys_dir)

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        await jobs.start()
        try:
            yield
        finally:
            await jobs.stop()
            await turnstile.close()

    app = FastAPI(title="NBTV 收聽卡", version=__version__, lifespan=lifespan,
                  docs_url=None, redoc_url=None, openapi_url=None)
    app.add_middleware(CanonicalUrls)
    app.add_middleware(SecurityHeaders)  # 最後加的在最外層：轉址的回應也會有安全標頭
    app.state.jobs = jobs
    app.state.store = store

    noticed_cloudflare = False

    def client_address(request: Request) -> str:
        nonlocal noticed_cloudflare
        forwarded = request.headers.get("cf-connecting-ip")
        if cfg.behind_cloudflare:
            # 只有 cloudflared 連得到這個埠（綁在 127.0.0.1），所以這個標頭可以信
            if forwarded:
                return forwarded
        elif forwarded and not noticed_cloudflare:
            noticed_cloudflare = True
            log.warning("收到經過 Cloudflare 的請求，但設定檔的 behind_cloudflare 是 false：所有訪客正被當成同一個來源來限制用量")
        return request.client.host if request.client else "unknown"

    def visitor_address(request: Request) -> str | None:
        """交給 Turnstile 一併核對的訪客位址：只有經過 Cloudflare、它告訴我們訪客是誰的時候才有。"""
        if not cfg.behind_cloudflare:
            return None
        try:
            return str(ipaddress.ip_address((request.headers.get("cf-connecting-ip") or "").strip()))
        except ValueError:
            return None

    def find_job(job_id: str):
        return jobs.get(job_id) if _JOB_ID.match(job_id) else None

    # ---------------------------------------------------------------- 頁面

    def page(name: str) -> FileResponse:
        return FileResponse(STATIC_DIR / name, media_type="text/html; charset=utf-8", headers={"cache-control": "no-cache"})

    @app.get("/", include_in_schema=False)
    async def index():
        return page("index.html")

    @app.get("/verify", include_in_schema=False)
    async def verify_page():
        return page("verify.html")

    @app.get("/v/{serial}", include_in_schema=False)
    async def verify_serial_page(serial: str):
        return page("verify.html")

    @app.get("/robots.txt", include_in_schema=False)
    async def robots():
        # 查驗頁與卡片是給拿到序號的人看的，不讓搜尋引擎收錄
        return PlainTextResponse("User-agent: *\nDisallow: /v/\nDisallow: /card/\nDisallow: /api/\n")

    @app.get("/root.pem", include_in_schema=False)
    async def root_certificate():
        # 本台的根憑證：查驗時拿來當信任的起點
        return Response(signer.root_pem, media_type="application/x-pem-file",
                        headers={"content-disposition": f'attachment; filename="{cfg.callsign}-nbtv-root.pem"'})

    @app.get("/verify_card.py", include_in_schema=False)
    async def verifier_tool():
        # 離線查驗的工具，單獨一個檔案就能用
        return FileResponse(STATIC_DIR.parent / "verify_card.py", media_type="text/x-python; charset=utf-8",
                            headers={"content-disposition": 'attachment; filename="verify_card.py"'})

    # ---------------------------------------------------------------- 設定

    @app.get("/api/config")
    async def public_config():
        return {
            "station": cfg.callsign,
            "notice": cfg.notice,
            "min_frames": cfg.min_frames,
            "max_seconds": cfg.max_seconds,
            "max_bytes": cfg.max_upload_bytes,
            "name_limit": NAME_LIMIT,
            "location_limit": LOCATION_LIMIT,
            "turnstile": {
                "enabled": cfg.turnstile_enabled,
                "site_key": cfg.turnstile_site_key if cfg.turnstile_enabled else "",
                "action": TURNSTILE_ACTION,  # 頁面上的 widget 與伺服器端的檢查用同一個名稱
            },
            "root_sha256": signer.root_fingerprint,       # 對外公布的根憑證
            "certificate_sha256": signer.fingerprint,     # 現在用的簽章憑證
            "c2pa": embedder is not None,
        }

    # ---------------------------------------------------------------- 上傳與解碼

    @app.post("/api/jobs")
    async def create_job(request: Request):
        """第一步：驗 Turnstile 的權杖，通過了才給一個可以上傳的工作。"""
        declared = request.headers.get("content-length", "")
        if declared.isdigit() and int(declared) > JSON_BODY_LIMIT:
            return error(413, "too_large")
        body = b""
        try:
            async with asyncio.timeout(JSON_BODY_SECONDS):  # 這麼小的內容不該傳很久
                async for chunk in request.stream():
                    body += chunk
                    if len(body) > JSON_BODY_LIMIT:
                        return error(413, "too_large")
        except (ClientDisconnect, TimeoutError):
            return error(400, "bad_request")
        try:
            data = JobIn.model_validate_json(body or b"{}")
        except ValidationError as e:
            fields = sorted({str(err["loc"][-1]) for err in e.errors() if err.get("loc")})
            return error(400, "bad_request", fields=fields)

        def too_many(wait: float) -> JSONResponse:
            seconds = int(wait) + 1
            return JSONResponse({"error": "rate_limited", "retry_after": seconds}, status_code=429,
                                headers={"retry-after": str(seconds)})

        key = client_key(client_address(request))
        wait = max(limiter.check(key), attempts.check(key))
        if wait > 0:
            return too_many(wait)
        refused = jobs.refusal(key)
        if refused is not None:
            return error(refused.status, refused.code)

        # 先把這一次記下來再去驗證：同時送來的好幾個請求才不會都趕在記帳之前通過
        attempts.record(key)
        stamp = limiter.record(key)
        checked = await turnstile.verify(data.token, visitor_address(request))
        if not checked.ok:
            limiter.forget(key, stamp)  # 沒通過驗證的不佔發卡的額度
            return error(503, "turnstile_unavailable") if checked.unavailable else error(403, "turnstile_failed")
        try:
            job = jobs.create(data.listener.to_listener(), owner=key)
        except JobError as e:
            limiter.forget(key, stamp)
            return error(e.status, e.code)
        return JSONResponse({"id": job.id, "upload": f"/api/jobs/{job.id}/audio"}, status_code=201)

    @app.put("/api/jobs/{job_id}/audio")
    async def upload_audio(job_id: str, request: Request):
        """第二步：錄音直接當成請求的內容送上來（不是表單），邊收邊算大小，超過就中斷。"""
        job = find_job(job_id)
        if job is None:
            return error(404, "not_found")
        declared = request.headers.get("content-length", "")
        if declared.isdigit() and int(declared) > cfg.max_upload_bytes:
            return error(413, "too_large", limit_mb=cfg.max_upload_bytes // (1024 * 1024))
        try:
            await jobs.receive(job, request.stream())
        except JobError as e:
            return error(e.status, e.code, limit_mb=cfg.max_upload_bytes // (1024 * 1024))
        except (ClientDisconnect, TimeoutError):
            return error(400, "upload_interrupted")
        return JSONResponse(jobs.public(job), status_code=202)

    @app.get("/api/jobs/{job_id}")
    async def job_status(job_id: str):
        job = find_job(job_id)
        if job is None:
            return error(404, "not_found")
        return jobs.public(job)

    @app.get("/api/jobs/{job_id}/frames.png")
    async def job_frames(job_id: str):
        job = find_job(job_id)
        if job is None or job.sheet_png is None:
            return error(404, "not_found")
        return Response(job.sheet_png, media_type="image/png", headers={"cache-control": "private, max-age=600"})

    # ---------------------------------------------------------------- 查驗

    @app.get("/api/cards/{serial}")
    async def card_record(serial: str):
        normal = normalize_serial(serial)
        stored = store.get(normal) if normal else None
        if stored is None:
            return error(404, "not_found")
        valid = verify(signer.cert, stored.record_text.encode("utf-8"), base64.b64decode(stored.signature_b64))
        return {
            "serial": stored.serial,
            "record": json.loads(stored.record_text),
            "record_text": stored.record_text,
            "signature": stored.signature_b64,
            "signature_valid": valid,
            "certificate_sha256": signer.fingerprint,
            "root_sha256": signer.root_fingerprint,
            "png": f"/card/{stored.serial}.png",
        }

    @app.get("/card/{name}", include_in_schema=False)
    async def card_png(name: str, request: Request):
        normal = normalize_serial(name[:-4]) if name.endswith(".png") else None
        if normal is None or store.get(normal) is None:
            return error(404, "not_found")
        path = store.card_path(normal)
        if not path.is_file():
            return error(404, "not_found")
        # 一張卡的圖檔只有兩個網址：序號照標準寫法的那一個，以及它加上 ?download=1（下載用）。
        # 序號的其他寫法（小寫、沒有連字號）與其他的參數都只回一個轉址：圖檔有幾百 KB 到幾 MB，
        # 如果每一種寫法都直接給圖，換個寫法就能讓 Cloudflare 的快取失效、每次都從這台電腦重新送一次。
        query = request.url.query
        download = query == "download=1"
        if name != f"{normal}.png" or (query and not download):
            wanted = "?download=1" if "download=1" in query.split("&") else ""
            return RedirectResponse(f"/card/{normal}.png{wanted}", status_code=301, headers={"cache-control": REDIRECT_CACHE})
        headers = {"cache-control": "public, max-age=31536000, immutable"}
        if download:
            headers["content-disposition"] = f'attachment; filename="{cfg.callsign}-NBTV-{normal}.png"'
        return FileResponse(path, media_type="image/png", headers=headers)

    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")
    return app
