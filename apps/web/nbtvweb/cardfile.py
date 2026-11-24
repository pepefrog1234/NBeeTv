"""收聽卡檔案的格式：一張 PNG，簽過的紀錄、簽章與憑證放在三個文字區塊（iTXt）裡。

紀錄是一段 JSON 文字，簽章簽的就是這段文字的 UTF-8 位元組，原樣存進檔案，
所以驗證時不必重新排版 JSON。憑證的區塊裡是兩張 PEM：簽章憑證在前，簽發它的根憑證在後。

紀錄把圖檔本身也綁進來，兩種方式都記：
    card.image_sha256   整個圖檔的雜湊（見 image_sha256）：除了放簽章資料的區塊以外，檔案裡的每個區塊
                        （大小、色彩設定、壓縮過的像素……）都算在內。圖檔只要多一個、少一個或改一個區塊，
                        雜湊就對不上——包括不動原本的像素、另外加一格動畫或透明度這類手法。
                        驗的時候只要會拆 PNG 的區塊，不必解開圖。
    card.pixels_sha256  解開之後的像素的雜湊。早期的卡只有這一項；它不涵蓋像素以外的東西，
                        所以驗早期的卡時還要另外檢查檔案裡沒有多出別種區塊。

另外還可以有一個 caBX 區塊，裡面是 C2PA 的資料（見 provenance.py）。它不影響上面這些，
但它記了整個檔案的雜湊，所以要在三個文字區塊都寫好之後才加。

離線驗證的獨立工具是 verify_card.py，它不依賴這個套件；這裡的格式改了，那邊要跟著改。
"""

from __future__ import annotations

import base64
import hashlib
import io
import json
import struct
import zlib
from dataclasses import dataclass

from PIL import Image

KEY_RECORD = "nbtv-card-record"
KEY_SIGNATURE = "nbtv-card-signature"
KEY_CERTIFICATE = "nbtv-card-certificate"
SIGNED_TEXT_KEYS = (KEY_RECORD, KEY_SIGNATURE, KEY_CERTIFICATE)
C2PA_CHUNK = b"caBX"

RECORD_TYPE = "nbtv-reception-card"
RECORD_VERSION = 1

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def pixels_sha256(image: Image.Image) -> str:
    """卡片畫面的雜湊：8 位元 RGB、由左上角逐列排列的像素。"""
    return hashlib.sha256(image.convert("RGB").tobytes()).hexdigest()


def canonical_json(record: dict) -> str:
    """紀錄 → 要簽的文字：鍵照字母排序、沒有多餘的空白、中文直接寫。"""
    return json.dumps(record, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


# ---------------------------------------------------------------- PNG 的區塊


def split_chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    """把 PNG 拆成 [(種類, 內容)]。檔頭、長度、檢查碼不對，或 IEND 之後還有東西，丟 ValueError。"""
    if not data.startswith(PNG_MAGIC):
        raise ValueError("不是 PNG")
    chunks, pos = [], len(PNG_MAGIC)
    while True:
        if pos + 12 > len(data):
            raise ValueError("PNG 檔不完整")
        length, kind = struct.unpack_from(">I4s", data, pos)
        end = pos + 12 + length
        if end > len(data):
            raise ValueError("PNG 檔不完整")
        body = data[pos + 8:pos + 8 + length]
        if zlib.crc32(kind + body) != struct.unpack_from(">I", data, pos + 8 + length)[0]:
            raise ValueError("PNG 的區塊檢查碼不對")
        chunks.append((kind, body))
        pos = end
        if kind == b"IEND":
            break
    if pos != len(data):
        raise ValueError("PNG 的結尾之後還有資料")
    return chunks


def join_chunks(chunks: list[tuple[bytes, bytes]]) -> bytes:
    out = bytearray(PNG_MAGIC)
    for kind, body in chunks:
        out += struct.pack(">I4s", len(body), kind) + body + struct.pack(">I", zlib.crc32(kind + body))
    return bytes(out)


def _holds_signature_data(kind: bytes, body: bytes) -> bool:
    """這個區塊是不是放簽章資料的（三個文字區塊與 C2PA）：它們不算進圖檔的雜湊。"""
    if kind == C2PA_CHUNK:
        return True
    return kind == b"iTXt" and body.split(b"\0", 1)[0].decode("latin-1") in SIGNED_TEXT_KEYS


def image_sha256(data: bytes) -> str:
    """整個圖檔的雜湊：除了放簽章資料的區塊，每個區塊照檔案裡的順序，各取「長度（4 位元組）、種類、內容」
    接在一起算 SHA-256。"""
    digest = hashlib.sha256()
    for kind, body in split_chunks(data):
        if not _holds_signature_data(kind, body):
            digest.update(struct.pack(">I4s", len(body), kind))
            digest.update(body)
    return digest.hexdigest()


def encode_image(image: Image.Image) -> bytes:
    """只有圖的 PNG（還沒有簽章資料）。"""
    out = io.BytesIO()
    image.convert("RGB").save(out, format="PNG", compress_level=6)
    return out.getvalue()


def _itxt(keyword: str, text: str) -> tuple[bytes, bytes]:
    # 關鍵字、不壓縮（旗標與方法都是 0）、沒有語言標記、沒有翻譯過的關鍵字，最後是 UTF-8 的文字
    return b"iTXt", keyword.encode("latin-1") + b"\0\0\0\0\0" + text.encode("utf-8")


def attach(png: bytes, record_text: str, signature: bytes, chain_pem: bytes) -> bytes:
    """把紀錄、簽章、憑證三個文字區塊放進 png（放在像素資料之前），其他區塊原封不動。

    chain_pem：簽章憑證接著根憑證（兩張 PEM 連在一起）。
    """
    texts = [
        _itxt(KEY_RECORD, record_text),
        _itxt(KEY_SIGNATURE, base64.b64encode(signature).decode("ascii")),
        _itxt(KEY_CERTIFICATE, chain_pem.decode("ascii")),
    ]
    out, placed = [], False
    for kind, body in split_chunks(png):
        if _holds_signature_data(kind, body):
            raise ValueError("這張圖裡已經有簽章資料")
        if kind == b"IDAT" and not placed:
            out += texts
            placed = True
        out.append((kind, body))
    if not placed:
        raise ValueError("PNG 裡沒有像素資料")
    return join_chunks(out)


def encode_png(image: Image.Image, record_text: str, signature: bytes, chain_pem: bytes) -> bytes:
    """畫面加上三個文字區塊。紀錄裡要記圖檔的雜湊時，先 encode_image、算好雜湊、簽完再 attach。"""
    return attach(encode_image(image), record_text, signature, chain_pem)


# ---------------------------------------------------------------- 讀回來


@dataclass
class CardFile:
    record_text: str
    record: dict
    signature: bytes
    cert_pem: bytes          # 簽章憑證接著根憑證
    pixels_sha256: str
    image_sha256: str
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
        pixels_sha256=pixels_sha256(image), image_sha256=image_sha256(data), size=image.size,
    )
