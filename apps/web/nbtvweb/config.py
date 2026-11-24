"""設定：從 TOML 檔讀入。檔案裡的路徑一律相對於設定檔所在的目錄。"""

from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

# 呼號能用的字元和核心的一樣（core/src/callsign.hpp）：數字、大寫字母與 / - .，最多 9 個
CALLSIGN_RE = re.compile(r"^[0-9A-Z/\-.]{1,9}$")

# Cloudflare 公開的 Turnstile 測試金鑰都是「1x、2x、3x 加一串 0」開頭；用它們等於沒有防護
_TEST_KEY_RE = re.compile(r"^[123]x0{10,}")
_UUID_RE = re.compile(r"^[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$")

TURNSTILE_VERIFY_URL = "https://challenges.cloudflare.com/turnstile/v0/siteverify"


class ConfigError(Exception):
    pass


@dataclass(frozen=True)
class Config:
    base_dir: Path

    # 電台
    callsign: str
    notice: str

    # 發卡的條件
    min_frames: int      # 有畫面的幀要「超過」這個數目
    max_seconds: float   # 錄音最長幾秒

    # 伺服器
    host: str
    port: int
    public_url: str          # 對外的網址，印在收聽卡的 QR code 裡
    behind_cloudflare: bool  # True：訪客的位址取自 CF-Connecting-IP

    # 用量限制
    max_upload_bytes: int
    workers: int
    queue: int
    jobs_per_ip: int
    window_seconds: float
    cards_per_day: int
    decode_timeout: float

    # 解碼器
    nbtv_rx: Path
    merge: bool

    # Cloudflare Turnstile
    turnstile_enabled: bool
    turnstile_site_key: str
    turnstile_secret_key: str
    turnstile_verify_url: str
    turnstile_hostnames: tuple[str, ...]  # 權杖必須是在這些主機名稱的頁面上取得的

    # 收聽卡
    backgrounds: Path
    font_cjk: str
    font_latin: str
    font_mono: str

    # 要不要在卡片裡嵌入 C2PA（Content Credentials）的資料
    c2pa_enabled: bool

    data_dir: Path

    @property
    def keys_dir(self) -> Path:
        """根憑證、簽章憑證與它們的私鑰（檔名見 signing.py）。"""
        return self.data_dir / "keys"

    @property
    def cards_dir(self) -> Path:
        return self.data_dir / "cards"

    @property
    def db_path(self) -> Path:
        return self.data_dir / "cards.sqlite3"

    @property
    def tmp_dir(self) -> Path:
        return self.data_dir / "tmp"

    @property
    def turnstile_uses_test_keys(self) -> bool:
        return self.turnstile_enabled and bool(
            _TEST_KEY_RE.match(self.turnstile_site_key) or _TEST_KEY_RE.match(self.turnstile_secret_key)
        )


def warnings(cfg: Config) -> list[str]:
    """讀得進來、但上線之後會出問題的設定。check 與 serve 都會把它們列出來。"""
    out = []
    host = urlsplit(cfg.public_url).hostname or ""
    local = host in ("127.0.0.1", "localhost", "::1") or host.endswith(".localhost")
    live_keys = cfg.turnstile_enabled and not cfg.turnstile_uses_test_keys
    if local and cfg.public_url.startswith("https://"):
        out.append(
            f"public_url 是 {cfg.public_url}，但網站自己只講 http：照這個網址印出來的 QR code 連不上。"
            "只在本機試用填 http://127.0.0.1:埠號，正式上線填 https://你的網域"
        )
    elif local and live_keys:
        out.append("Turnstile 填的是正式的金鑰，但 public_url 還是本機的網址。它會印在每張卡的 QR code 裡，發出去就改不了，上線前要改成對外的網址")
    if not local and not cfg.behind_cloudflare:
        out.append("public_url 是對外的網址，但 behind_cloudflare 是 false：經過 Cloudflare 進來的訪客會被當成同一個來源，一起受「每個來源」的用量限制")
    port = urlsplit(cfg.public_url).port
    if not local and port is not None:
        out.append(
            f"public_url 帶著埠號（:{port}）。經過 Cloudflare Tunnel 對外的網址是 https://{host}，沒有埠號；"
            f"{cfg.port} 只是網站在這台電腦上聽的埠。照現在的網址印出來的 QR code 多半連不上"
        )
    if cfg.turnstile_enabled and not local and {"localhost", "127.0.0.1"} & set(cfg.turnstile_hostnames):
        out.append("[turnstile] hostnames 裡有 localhost 或 127.0.0.1：正式上線的清單只該有對外的主機名稱")
    return out


_TABLES: dict[str, set[str]] = {
    "station": {"callsign", "notice"},
    "rules": {"min_frames", "max_seconds"},
    "server": {"host", "port", "public_url", "behind_cloudflare"},
    "limits": {
        "max_upload_mb", "workers", "queue", "jobs_per_ip", "window_minutes", "cards_per_day", "decode_timeout_seconds",
    },
    "decoder": {"nbtv_rx", "merge"},
    "turnstile": {"enabled", "site_key", "secret_key", "verify_url", "hostnames"},
    "card": {"backgrounds", "font_cjk", "font_latin", "font_mono"},
    "c2pa": {"enabled"},
    "paths": {"data"},
}


def load_config(path: str | Path) -> Config:
    path = Path(path).expanduser().resolve()
    try:
        with open(path, "rb") as f:
            doc = tomllib.load(f)
    except FileNotFoundError:
        raise ConfigError(f"找不到設定檔 {path}（可以從 config.example.toml 複製一份）") from None
    except tomllib.TOMLDecodeError as e:
        hint = "（那一行的字串多半少了結尾的引號）" if "Illegal character '\\n'" in str(e) else ""
        raise ConfigError(f"設定檔 {path} 的格式不對：{e}{hint}") from None

    unknown = set(doc) - set(_TABLES)
    if unknown:
        raise ConfigError(f"設定檔裡有不認得的段落：{', '.join(sorted(unknown))}")

    def table(name: str) -> dict:
        t = doc.get(name, {})
        if not isinstance(t, dict):
            raise ConfigError(f"[{name}] 應該是一個段落")
        extra = set(t) - _TABLES[name]
        if extra:
            raise ConfigError(f"[{name}] 裡有不認得的項目：{', '.join(sorted(extra))}")
        return t

    def typed(t: dict, section: str, key: str, kind, default):
        value = t.get(key, default)
        if kind is float and isinstance(value, int) and not isinstance(value, bool):
            value = float(value)
        if not isinstance(value, kind) or (kind in (int, float) and isinstance(value, bool)):
            raise ConfigError(f"[{section}] {key} 的型別不對")
        return value

    base = path.parent

    def rel(p: str) -> Path:
        return (base / Path(p).expanduser()).resolve()

    station, rules, server = table("station"), table("rules"), table("server")
    limits, decoder, turnstile = table("limits"), table("decoder"), table("turnstile")
    card, paths, c2pa = table("card"), table("paths"), table("c2pa")

    callsign = typed(station, "station", "callsign", str, "").strip().upper()
    if not CALLSIGN_RE.match(callsign):
        raise ConfigError("[station] callsign 要填電台的呼號（數字、大寫字母與 / - .，最多 9 個字元）")

    public_url = typed(server, "server", "public_url", str, "http://127.0.0.1:8088").strip().rstrip("/")
    if not re.match(r"^https?://[^/\s]+(/[^\s]*)?$", public_url):
        raise ConfigError("[server] public_url 要是 http:// 或 https:// 開頭的網址")

    # Turnstile 的權杖要是在這些主機名稱的頁面上取得的才算數。沒有寫就用 public_url 的主機名稱
    hostnames = turnstile.get("hostnames", [])
    if not isinstance(hostnames, list) or not all(isinstance(h, str) and re.fullmatch(r"[0-9A-Za-z.-]+", h.strip()) for h in hostnames):
        raise ConfigError('[turnstile] hostnames 要是一串主機名稱，例如 ["card.example.com"]（不含 https:// 與埠號）')
    hostnames = tuple(dict.fromkeys(h.strip().lower() for h in hostnames)) or ((urlsplit(public_url).hostname or "").lower(),)

    cfg = Config(
        base_dir=base,
        callsign=callsign,
        notice=typed(station, "station", "notice", str, "").strip(),
        min_frames=typed(rules, "rules", "min_frames", int, 10),
        max_seconds=typed(rules, "rules", "max_seconds", float, 300.0),
        host=typed(server, "server", "host", str, "127.0.0.1"),
        port=typed(server, "server", "port", int, 8088),
        public_url=public_url,
        behind_cloudflare=typed(server, "server", "behind_cloudflare", bool, False),
        max_upload_bytes=int(typed(limits, "limits", "max_upload_mb", float, 64.0) * 1024 * 1024),
        workers=typed(limits, "limits", "workers", int, 2),
        queue=typed(limits, "limits", "queue", int, 8),
        jobs_per_ip=typed(limits, "limits", "jobs_per_ip", int, 6),
        window_seconds=typed(limits, "limits", "window_minutes", float, 10.0) * 60.0,
        cards_per_day=typed(limits, "limits", "cards_per_day", int, 200),
        decode_timeout=typed(limits, "limits", "decode_timeout_seconds", float, 300.0),
        nbtv_rx=rel(typed(decoder, "decoder", "nbtv_rx", str, "../../build/tools/nbtv-rx")),
        merge=typed(decoder, "decoder", "merge", bool, True),
        turnstile_enabled=typed(turnstile, "turnstile", "enabled", bool, True),
        turnstile_site_key=typed(turnstile, "turnstile", "site_key", str, "").strip(),
        turnstile_secret_key=typed(turnstile, "turnstile", "secret_key", str, "").strip(),
        turnstile_verify_url=typed(turnstile, "turnstile", "verify_url", str, TURNSTILE_VERIFY_URL).strip(),
        turnstile_hostnames=hostnames,
        backgrounds=rel(typed(card, "card", "backgrounds", str, "assets/backgrounds")),
        font_cjk=typed(card, "card", "font_cjk", str, "").strip(),
        font_latin=typed(card, "card", "font_latin", str, "").strip(),
        font_mono=typed(card, "card", "font_mono", str, "").strip(),
        c2pa_enabled=typed(c2pa, "c2pa", "enabled", bool, True),
        data_dir=rel(typed(paths, "paths", "data", str, "data")),
    )

    if cfg.min_frames < 0 or cfg.max_seconds <= 0:
        raise ConfigError("[rules] 的數值要是正的")
    if not (1 <= cfg.port <= 65535):
        raise ConfigError("[server] port 要在 1 到 65535 之間")
    if cfg.workers < 1 or cfg.queue < 1 or cfg.jobs_per_ip < 1 or cfg.cards_per_day < 1:
        raise ConfigError("[limits] 的數值至少要是 1")
    if cfg.max_upload_bytes < 1024 * 1024 or cfg.window_seconds <= 0 or cfg.decode_timeout <= 0:
        raise ConfigError("[limits] 的數值太小")
    if cfg.turnstile_enabled and not (cfg.turnstile_site_key and cfg.turnstile_secret_key):
        raise ConfigError(
            "[turnstile] 已啟用，但沒有填 site_key 與 secret_key。"
            "到 Cloudflare 的 Turnstile 建一個 widget 取得；只在本機試用的話可以把 enabled 設成 false，"
            "或填 config.example.toml 裡列的測試金鑰"
        )
    if cfg.turnstile_enabled:
        # 常見的誤會：把 Cloudflare Tunnel 的東西填到這裡。訊息裡不印出填的值
        where = "Turnstile 的金鑰在 Cloudflare 控制台的 Turnstile 頁建 widget 取得，和通道（Tunnel）是兩回事"
        if cfg.turnstile_secret_key.startswith("eyJ") or len(cfg.turnstile_secret_key) > 100:
            raise ConfigError(
                "[turnstile] secret_key 看起來是 Cloudflare Tunnel 的權杖，不是 Turnstile 的 secret key。"
                f"通道的權杖是給 cloudflared 用的，不要放在這個檔案裡。{where}"
            )
        if _UUID_RE.match(cfg.turnstile_site_key):
            raise ConfigError(f"[turnstile] site_key 看起來是通道或帳號的 ID，不是 Turnstile 的 site key。{where}")
    return cfg
