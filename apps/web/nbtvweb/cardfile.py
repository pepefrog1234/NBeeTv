"""收聽卡檔案的格式：一張 PNG，簽過的紀錄、簽章與憑證放在三個文字區塊（iTXt）裡。

紀錄是一段 JSON 文字，簽章簽的就是這段文字的 UTF-8 位元組，原樣存進檔案，
所以驗證時不必重新排版 JSON。紀錄裡有卡片像素的雜湊（card.pixels_sha256），
把畫面和紀錄綁在一起：只改圖、不改紀錄，雜湊就對不上。
憑證的區塊裡是兩張 PEM：簽章憑證在前，簽發它的根憑證在後。

另外還可以有一個 caBX 區塊，裡面是 C2PA 的資料（見 provenance.py）。它不影響這裡的三個區塊，
但它記了整個檔案的雜湊，所以要在這三個區塊都寫好之後才加。

離線驗證的獨立工具是 verify_card.py，它不依賴這個套件；這裡的常數改了，那邊要跟著改。
"""

from __future__ import annotations

import base64
import hashlib
import io
import json
from dataclasses import dataclass

from PIL import Image
from PIL.PngImagePlugin import PngInfo

KEY_RECORD = "nbtv-card-record"
KEY_SIGNATURE = "nbtv-card-signature"
KEY_CERTIFICATE = "nbtv-card-certificate"

RECORD_TYPE = "nbtv-reception-card"
RECORD_VERSION = 1


def pixels_sha256(image: Image.Image) -> str:
    """卡片畫面的雜湊：8 位元 RGB、由左上角逐列排列的像素。"""
    return hashlib.sha256(image.convert("RGB").tobytes()).hexdigest()


def canonical_json(record: dict) -> str:
    """紀錄 → 要簽的文字：鍵照字母排序、沒有多餘的空白、中文直接寫。"""
    return json.dumps(record, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def encode_png(image: Image.Image, record_text: str, signature: bytes, chain_pem: bytes) -> bytes:
    """chain_pem：簽章憑證接著根憑證（兩張 PEM 連在一起）。"""
    info = PngInfo()
    info.add_itxt(KEY_RECORD, record_text)
    info.add_itxt(KEY_SIGNATURE, base64.b64encode(signature).decode("ascii"))
    info.add_itxt(KEY_CERTIFICATE, chain_pem.decode("ascii"))
    out = io.BytesIO()
    image.convert("RGB").save(out, format="PNG", pnginfo=info, compress_level=6)
    return out.getvalue()


@dataclass
class CardFile:
    record_text: str
    record: dict
    signature: bytes
    cert_pem: bytes          # 簽章憑證接著根憑證
    pixels_sha256: str
    size: tuple[int, int]


def decode_png(data: bytes) -> CardFile:
    """讀一張收聽卡。不是收聽卡（沒有那三個區塊）時丟 ValueError。"""
    image = Image.open(io.BytesIO(data))
    if image.format != "PNG":
        raise ValueError("不是 PNG")
    image.load()
    text = getattr(image, "text", {})
    try:
        record_text = text[KEY_RECORD]
        signature = base64.b64decode(text[KEY_SIGNATURE], validate=True)
        cert_pem = text[KEY_CERTIFICATE].encode("ascii")
    except (KeyError, ValueError, UnicodeEncodeError):
        raise ValueError("這張 PNG 裡沒有收聽卡的簽章資料") from None
    record = json.loads(record_text)
    if not isinstance(record, dict):
        raise ValueError("紀錄的格式不對")
    return CardFile(
        record_text=record_text, record=record, signature=signature, cert_pem=cert_pem,
        pixels_sha256=pixels_sha256(image), size=image.size,
    )
