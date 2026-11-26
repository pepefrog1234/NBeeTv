"""發卡：畫卡片、寫紀錄、簽章、存檔。只有這裡會用到私鑰。"""

from __future__ import annotations

import base64
import datetime
import hashlib
import logging
from dataclasses import dataclass
from urllib.parse import urlsplit

from . import card as cardlib
from .cardfile import RECORD_TYPE, RECORD_VERSION, attach, canonical_json, encode_image, image_sha256, pixels_sha256
from .config import Config
from .provenance import C2paEmbedder, ProvenanceError
from .rules import Verdict
from .serials import new_serial
from .signing import ALGORITHM, Signer
from .store import Store, StoredCard

log = logging.getLogger(__name__)


class IssueError(Exception):
    def __init__(self, code: str):
        super().__init__(code)
        self.code = code


@dataclass
class Listener:
    """聽眾自己填的資料，全部可以留空。本站不查核。"""

    name: str = ""
    date: str = ""                 # YYYY-MM-DD（UTC）
    time: str = ""                 # HH:MM（UTC）
    freq_khz: float | None = None
    location: str = ""


@dataclass
class Issued:
    serial: str
    record: dict


class Issuer:
    def __init__(self, cfg: Config, signer: Signer, store: Store, fonts: cardlib.Fonts, embedder: C2paEmbedder | None = None):
        self.cfg = cfg
        self.signer = signer
        self.store = store
        self.fonts = fonts
        self.embedder = embedder  # None：卡片裡不放 C2PA 的資料

    def issue(self, verdict: Verdict, listener: Listener, audio_sha256: str, audio_seconds: float) -> Issued:
        """發一張卡（會花零點幾秒畫圖，要在執行緒裡呼叫）。verdict 必須是通過的。"""
        if not verdict.ok or not verdict.callsign or verdict.best is None or verdict.best.rgb is None:
            raise IssueError("not_eligible")
        # 卡片上的電台是從這份錄音解出來的呼號，不是設定檔裡本站自己的呼號
        station = verdict.callsign
        now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
        day_start = now.replace(hour=0, minute=0, second=0).strftime("%Y-%m-%dT%H:%M:%SZ")
        if self.store.count_since(day_start) >= self.cfg.cards_per_day:
            raise IssueError("daily_limit")

        best = verdict.best
        where = f"幀序號 {best.sequence}" if best.header else f"第 {best.index + 1} 幀"
        caption = f"{where} · 模式 {best.mode}"
        if best.snr_db is not None:
            caption += f" · 訊雜比 {best.snr_db:.1f} dB"
        host = urlsplit(self.cfg.public_url)
        hint = f"{host.netloc}{host.path.rstrip('/')}/verify"

        for _ in range(5):
            serial = new_serial()
            content = cardlib.CardContent(
                callsign=station,
                serial=serial,
                verify_url=f"{self.cfg.public_url}/v/{serial}",
                verify_hint=hint,
                issued_at=now.strftime("%Y-%m-%d %H:%M UTC"),
                fingerprint=self.signer.root_fingerprint,
                c2pa=self.embedder is not None,
                listener_name=listener.name,
                listener_date=listener.date,
                listener_time=listener.time,
                listener_freq=cardlib.format_frequency(listener.freq_khz),
                listener_location=listener.location,
                modes=", ".join(str(m) for m in verdict.modes),
                frames=verdict.counted,
                snr=f"{verdict.snr_median:.1f} dB" if verdict.snr_median is not None else "",
                picture=best.rgb,
                picture_caption=caption,
                strip=[f.rgb for f in verdict.strip if f.rgb is not None],
            )
            background = cardlib.load_background(cardlib.pick_background(self.cfg.backgrounds, serial))
            image = cardlib.render(content, background, self.fonts)
            # 先把圖壓成 PNG：紀錄裡要記的是這個圖檔本身的雜湊，簽完之後只加文字區塊，不再重新壓一次
            picture = encode_image(image)
            record = {
                "v": RECORD_VERSION,
                "type": RECORD_TYPE,
                "serial": serial,
                "issued_at": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
                "station": station,
                "issuer": self.cfg.public_url,
                # 解碼器量到的
                "reception": {
                    "frames": verdict.counted,
                    "frames_with_callsign": verdict.callsign_frames,
                    "modes": {str(m): n for m, n in verdict.modes.items()},
                    "snr_db_median": verdict.snr_median,
                    "snr_db_best": verdict.snr_best,
                    "audio_seconds": round(audio_seconds, 1),
                    "audio_sha256": audio_sha256,
                    "picture_sha256": hashlib.sha256(best.rgb).hexdigest(),
                },
                # 聽眾自己填的，本站沒有查核
                "listener": {
                    "self_reported": True,
                    "name": listener.name,
                    "date": listener.date,
                    "time_utc": listener.time,
                    "frequency_khz": listener.freq_khz,
                    "location": listener.location,
                },
                # image_sha256：整個圖檔（簽章資料以外的每個區塊）；pixels_sha256：解開之後的像素。
                # c2pa：這個檔案發出去的時候有沒有附 C2PA 的資料（有的話被拿掉也看得出來）
                "card": {
                    "width": image.width, "height": image.height,
                    "image_sha256": image_sha256(picture), "pixels_sha256": pixels_sha256(image),
                    "c2pa": self.embedder is not None,
                },
                "signature": {
                    "algorithm": ALGORITHM,
                    "certificate_sha256": self.signer.fingerprint,
                    "root_sha256": self.signer.root_fingerprint,
                },
            }
            text = canonical_json(record)
            signature = self.signer.sign(text.encode("utf-8"))
            png = attach(picture, text, signature, self.signer.chain_pem)
            if self.embedder is not None:
                # C2PA 記的是整個檔案的雜湊，所以放在最後：上面的三個文字區塊都寫好了才加
                try:
                    png = self.embedder.embed(png, record, f"{station} NBTV reception card {serial}")
                except ProvenanceError as e:
                    log.error("%s", e)
                    raise IssueError("internal") from None
            # 發出去之前照查驗的方式再算一次：後面的步驟如果動到了圖檔的區塊，這張卡到別人手上會驗不過
            if image_sha256(png) != record["card"]["image_sha256"]:
                log.error("卡片 %s：加上簽章資料之後，圖檔的雜湊和紀錄裡的不一樣", serial)
                raise IssueError("internal")
            stored = StoredCard(
                serial=serial, issued_at=record["issued_at"], record_text=text,
                signature_b64=base64.b64encode(signature).decode("ascii"), audio_sha256=audio_sha256,
            )
            if self.store.add(stored, png):
                return Issued(serial=serial, record=record)
        raise IssueError("serial_collision")
