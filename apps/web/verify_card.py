#!/usr/bin/env python3
"""離線查驗 NBTV 收聽卡：不連網路，只看卡片檔案本身與電台公布的根憑證。

    python3 verify_card.py 卡片.png --cert 電台公布的根憑證.pem
    python3 verify_card.py 卡片.png --fingerprint 電台公布的根憑證指紋
    python3 verify_card.py 卡片.png --export 目錄      # 拆出紀錄、簽章、憑證，改用 openssl 驗

結束碼：0 通過；1 沒有通過；2 沒辦法下結論（沒有指定電台的根憑證、沒有驗簽章用的工具，
或是早期格式的卡而這台電腦沒有 Pillow）。只有 0 代表「這張卡是電台發的，而且沒有被改過」。

這個檔案可以單獨拿去用，不需要網站的其他程式，只要 Python 3.8 以上。簽章用 cryptography 套件驗
（pip install cryptography），沒有裝的話改用系統的 openssl 指令。另外有兩樣是有裝才會用到的：
    c2pa-python   卡片裡的 C2PA（Content Credentials）資料。沒裝不影響結論，只是少驗這一份
    Pillow        只有早期格式的卡（紀錄裡沒有 card.image_sha256）比對畫面時需要

卡片是一張 PNG，裡面有三個文字區塊（iTXt）：
    nbtv-card-record       簽過的紀錄（JSON 文字；簽的就是這段文字的 UTF-8 位元組）
    nbtv-card-signature    簽章（ECDSA P-256／SHA-256，DER 格式再轉 base64）
    nbtv-card-certificate  兩張憑證（PEM）：簽章憑證，接著簽發它的根憑證
以及一個 caBX 區塊：C2PA 的資料，裡面有同一份紀錄，由同一張簽章憑證簽署。

紀錄裡的 card.image_sha256 是整個圖檔的雜湊：除了上面這四個放簽章資料的區塊，檔案裡的每個區塊照順序，
各取「長度（4 位元組）、種類、內容」接在一起算 SHA-256。圖檔多一個、少一個或改一個區塊都對不上。

憑證都是電台自己簽發的，沒有經過憑證機構。任何人都做得出寫著同樣名字的憑證，所以
「簽章和卡片裡附的憑證相符」只代表檔案前後一致；要確定是電台發的，根憑證（或它的指紋）
必須來自電台公布的地方，用 --cert 或 --fingerprint 指定。沒有指定時，這個工具不會說「通過」。

卡片檔是別人給的，所以這裡把它當成可能是惡意的：憑證只用一種方式讀（讀出來的位元組同時拿去算指紋和
驗簽章）、卡片裡的文字印出來之前先把控制字元換掉、不會照卡片裡寫的網址去連任何地方。
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import struct
import sys
import unicodedata
import zlib
from pathlib import Path

KEY_RECORD = "nbtv-card-record"
KEY_SIGNATURE = "nbtv-card-signature"
KEY_CERTIFICATE = "nbtv-card-certificate"
SIGNED_TEXT_KEYS = (KEY_RECORD, KEY_SIGNATURE, KEY_CERTIFICATE)
C2PA_CHUNK = b"caBX"
C2PA_ASSERTION = "tw.hf-nbtv.reception-card"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

MAX_FILE_BYTES = 64 * 1024 * 1024
# 三個文字區塊最多多長（本站寫的遠小於這些數字）
MAX_TEXT_BYTES = {KEY_RECORD: 64 * 1024, KEY_SIGNATURE: 1024, KEY_CERTIFICATE: 16 * 1024}

PASSED, FAILED, UNDECIDED = 0, 1, 2


# ---------------------------------------------------------------- 印東西
#
# 卡片裡的文字（名字、地點……連憑證上的名稱）都是做這個檔案的人決定的。原樣印到終端機的話，
# 裡面的控制碼可以把游標移走、把後面的字藏起來，讓沒通過的結果看起來像通過。
# 所以每一行印出去之前都先過 clean()，卡片裡的值另外用 field() 排版。


def clean(text: str) -> str:
    """控制字元、看不見的排版字元、換行，都換成 \\uXXXX 的寫法；各種空白換成一般的空白。"""
    out = []
    for ch in text:
        category = unicodedata.category(ch)
        if ch == " " or (category[0] != "C" and category not in ("Zl", "Zp", "Zs")):
            out.append(ch)
        elif category == "Zs":
            out.append(" ")
        else:
            out.append("\\u%04x" % ord(ch) if ord(ch) < 0x10000 else "\\U%08x" % ord(ch))
    return "".join(out)


def say(text: str = "") -> None:
    print(clean(text))


def width(text: str) -> int:
    """在終端機上佔幾格（中日韓的字佔兩格）。"""
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1 for ch in text)


def as_text(value) -> str:
    if isinstance(value, str):
        return value
    if value is None:
        return "（沒有）"
    try:
        return json.dumps(value, ensure_ascii=False)
    except (TypeError, ValueError, RecursionError):
        return "（看不懂的值）"


def field(label: str, value, indent: int = 2) -> None:
    """印卡片裡的一個欄位。值太長時由這裡自己折行，每一行前面都留著縮排：
    卡片裡的文字不會出現在一行的開頭，也就排不出一行像是這個工具自己印的結論。"""
    text = " ".join(clean(as_text(value)).split())  # 連續的空白併成一個，不能拿一長串空白把字擠到下一行
    lines, current, used, total = [], [], 0, 0
    for ch in text:
        w = width(ch)
        if total + w > 300:  # 再長也只印這麼多
            current.append("…")
            break
        if used + w > 60:
            lines.append("".join(current))
            current, used = [], 0
        current.append(ch)
        used += w
        total += w
    lines.append("".join(current))
    head = " " * indent + label + " " * max(1, 14 - width(label))
    print(head + lines[0])
    for line in lines[1:]:
        print(" " * width(head) + line)


# ---------------------------------------------------------------- PNG


def read_png(data: bytes):
    """把 PNG 拆成 [(種類, 內容)]。只接受完整、檢查碼正確、結尾之後沒有東西的檔案。"""
    if not data.startswith(PNG_MAGIC):
        raise ValueError("不是 PNG 檔")
    chunks, pos = [], len(PNG_MAGIC)
    while True:
        if pos + 12 > len(data):
            raise ValueError("PNG 檔不完整")
        length, kind = struct.unpack_from(">I4s", data, pos)
        end = pos + 12 + length
        if end > len(data):
            raise ValueError("PNG 檔不完整")
        body = data[pos + 8:pos + 8 + length]
        if zlib.crc32(kind + body) != struct.unpack_from(">I", data, end - 4)[0]:
            raise ValueError("PNG 的區塊檢查碼不對，檔案損壞")
        chunks.append((kind, body))
        pos = end
        if kind == b"IEND":
            break
    if pos != len(data):
        raise ValueError("PNG 的結尾之後還有別的資料")
    if chunks[0][0] != b"IHDR":
        raise ValueError("PNG 的開頭不對")
    return chunks


def itxt_keyword(body: bytes) -> str:
    return body.split(b"\0", 1)[0].decode("latin-1")


def holds_signature_data(kind: bytes, body: bytes) -> bool:
    """放簽章資料的區塊（三個文字區塊與 C2PA）：不算進圖檔的雜湊。"""
    return kind == C2PA_CHUNK or (kind == b"iTXt" and itxt_keyword(body) in SIGNED_TEXT_KEYS)


def card_texts(chunks) -> dict:
    """三個放簽章資料的文字區塊 → {關鍵字: 文字}。同一個關鍵字只能出現一次；別的文字區塊不讀。"""
    texts = {}
    for kind, body in chunks:
        if kind != b"iTXt":
            continue
        keyword = itxt_keyword(body)
        if keyword not in SIGNED_TEXT_KEYS:
            continue
        if keyword in texts:
            raise ValueError(f"{keyword} 出現了不只一次")
        rest = body[len(keyword) + 1:]
        # 壓縮旗標、壓縮方法、語言標記、翻譯過的關鍵字：本站寫的都是 0 與空字串。壓縮過的不收，也就不必解壓縮
        if rest[:4] != b"\0\0\0\0":
            raise ValueError(f"{keyword} 的寫法不是本站的格式")
        if len(rest) - 4 > MAX_TEXT_BYTES[keyword]:
            raise ValueError(f"{keyword} 太長")
        texts[keyword] = rest[4:].decode("utf-8")
    return texts


def image_sha256(chunks) -> str:
    """整個圖檔的雜湊，算法見檔案開頭的說明。"""
    digest = hashlib.sha256()
    for kind, body in chunks:
        if not holds_signature_data(kind, body):
            digest.update(struct.pack(">I4s", len(body), kind))
            digest.update(body)
    return digest.hexdigest()


def early_format_problem(chunks):
    """早期的卡（紀錄裡沒有 card.image_sha256）只記了像素的雜湊，不涵蓋檔案裡其他的東西。
    所以要另外確認：檔案裡只有本站會寫的那幾種區塊，而且是不透明、沒有交錯的 8 位元 RGB——
    這樣看圖軟體顯示的才會就是那些像素，不會是另外加進去的動畫或透明度。回傳哪裡不對；沒問題回傳 None。"""
    for kind, body in chunks:
        if kind not in (b"IHDR", b"iCCP", b"IDAT", b"IEND", b"iTXt", C2PA_CHUNK):
            return "多了 " + kind.decode("latin-1") + " 區塊"
        if kind == b"iTXt" and itxt_keyword(body) not in SIGNED_TEXT_KEYS:
            return "多了別的文字區塊"
    header = chunks[0][1]
    if len(header) != 13 or header[8:13] != bytes([8, 2, 0, 0, 0]):
        return "不是不透明的 8 位元 RGB"
    columns, rows = struct.unpack(">II", header[:8])
    if columns * rows > 50_000_000:  # 本站的卡是 1680×1080；大得離譜的圖不解開
        return "圖的尺寸不對"
    return None


# ---------------------------------------------------------------- 憑證
#
# PEM 只在這裡拆一次，拆出來的 DER 位元組同時拿去算指紋、驗簽章、驗簽發關係。
# 不把卡片裡的 PEM 文字直接交給別的程式庫或 openssl：同一段刻意寫歪的文字，不同的程式會讀出不同的憑證，
# 於是「指紋是電台的根憑證」和「驗簽章用的是偽造者的憑證」可以同時成立。

_BASE64_LINE = re.compile(r"[A-Za-z0-9+/]+={0,2}")
PEM_BEGIN, PEM_END = "-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----"
# 憑證的簽章演算法：ecdsa-with-SHA256（1.2.840.10045.4.3.2）
ECDSA_SHA256 = bytes.fromhex("300a06082a8648ce3d040302")
# P-256 公鑰（SubjectPublicKeyInfo）的開頭：演算法是 id-ecPublicKey、曲線 prime256v1，接著 65 個位元組的點
P256_PUBLIC_KEY_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")


def _tlv(der: bytes, pos: int):
    """DER 的一個元素：回傳（標籤, 內容開始的位置, 內容結束的位置）。只收 DER 規定的寫法：長度確定、用最短的寫法。"""
    if pos + 2 > len(der):
        raise ValueError("憑證的編碼不完整")
    tag, first = der[pos], der[pos + 1]
    if tag & 0x1F == 0x1F:
        raise ValueError("憑證的編碼不對")
    if first < 0x80:
        start, length = pos + 2, first
    else:
        count = first & 0x7F
        if count == 0 or count > 4 or pos + 2 + count > len(der):
            raise ValueError("憑證的編碼不對")
        length = int.from_bytes(der[pos + 2:pos + 2 + count], "big")
        if length < 0x80 or der[pos + 2] == 0:
            raise ValueError("憑證的編碼不對")
        start = pos + 2 + count
    if start + length > len(der):
        raise ValueError("憑證的編碼不完整")
    return tag, start, start + length


def split_certificate(der: bytes):
    """憑證 = 被簽的內容（tbsCertificate）＋演算法＋簽章。回傳這三段。整份資料要剛好是一張憑證，後面不能多東西。"""
    tag, start, end = _tlv(der, 0)
    if tag != 0x30 or end != len(der):
        raise ValueError("憑證的編碼不對")
    tag, _, tbs_end = _tlv(der, start)
    if tag != 0x30:
        raise ValueError("憑證的編碼不對")
    tag, _, algorithm_end = _tlv(der, tbs_end)
    if tag != 0x30:
        raise ValueError("憑證的編碼不對")
    tag, signature_start, signature_end = _tlv(der, algorithm_end)
    # 簽章是位元字串，第一個位元組是「最後一個位元組有幾個位元沒用到」，一定是 0
    if tag != 0x03 or signature_end != end or signature_start >= signature_end or der[signature_start] != 0:
        raise ValueError("憑證的編碼不對")
    return der[start:tbs_end], der[tbs_end:algorithm_end], der[signature_start + 1:signature_end]


def parse_pem_certificates(text: str, most: int):
    """PEM 文字 → 每一張憑證的 DER 位元組。格式稍有出入就不收：
    除了 BEGIN／END 兩行與 base64 之外不能有別的字，base64 要是標準的寫法。"""
    ders, body, inside = [], [], False
    for raw in text.split("\n"):
        line = raw.rstrip(" \t\r")
        if not inside:
            if line == "":
                continue
            if line != PEM_BEGIN:
                raise ValueError("憑證的格式不對")
            inside, body = True, []
        elif line == PEM_END:
            joined = "".join(body)
            try:
                der = base64.b64decode(joined, validate=True)
            except ValueError:
                raise ValueError("憑證的格式不對") from None
            # 轉回去要和原來的字一模一樣：同一段位元組只有一種寫法
            if not der or base64.b64encode(der).decode("ascii") != joined:
                raise ValueError("憑證的格式不對")
            split_certificate(der)
            ders.append(der)
            inside = False
        else:
            if not _BASE64_LINE.fullmatch(line):
                raise ValueError("憑證的格式不對")
            body.append(line)
    if inside or not ders:
        raise ValueError("憑證的格式不對")
    if len(ders) > most:
        raise ValueError("憑證比預期的多")
    return ders


def to_pem(der: bytes) -> bytes:
    text = base64.b64encode(der).decode("ascii")
    lines = [text[i:i + 64] for i in range(0, len(text), 64)]
    return (PEM_BEGIN + "\n" + "\n".join(lines) + "\n" + PEM_END + "\n").encode("ascii")


def fingerprint_of(der: bytes) -> str:
    """憑證的 SHA-256 指紋：對 DER 編碼取雜湊，和 openssl x509 -fingerprint -sha256 一樣。"""
    return hashlib.sha256(der).hexdigest()


def normalize_fingerprint(text: str):
    """使用者給的指紋 → 64 個小寫的十六進位字元；不是指紋回傳 None。冒號、空白可以夾在中間，
    也可以整行貼上 openssl 印的「sha256 Fingerprint=…」。"""
    text = text.split("=", 1)[1] if "=" in text else text
    text = "".join(ch for ch in text.lower() if ch not in ": \t\r\n")
    return text if re.fullmatch(r"[0-9a-f]{64}", text) else None


# ---------------------------------------------------------------- 簽章


def verify_signature(cert_der: bytes, data: bytes, signature: bytes):
    """用憑證裡的公鑰（要是 P-256）驗 ECDSA／SHA-256 的簽章。回傳 True 或 False；兩種驗法都不能用時回傳 None。

    有 cryptography 套件就用它，沒有的話改呼叫系統的 openssl 指令。
    """
    try:
        from cryptography import x509
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError:
        return _verify_with_openssl(cert_der, data, signature)
    try:
        public_key = x509.load_der_x509_certificate(cert_der).public_key()
        if not isinstance(public_key, ec.EllipticCurvePublicKey) or public_key.curve.name != "secp256r1":
            return False
        public_key.verify(signature, data, ec.ECDSA(hashes.SHA256()))
        return True
    except (InvalidSignature, ValueError, TypeError):
        return False


def _verify_with_openssl(cert_der: bytes, data: bytes, signature: bytes):
    import shutil
    import subprocess
    import tempfile

    openssl = shutil.which("openssl")
    if openssl is None:
        return None
    with tempfile.TemporaryDirectory() as tmp:
        files = {name: Path(tmp) / name for name in ("cert.der", "pubkey.pem", "data.bin", "signature.der")}
        files["cert.der"].write_bytes(cert_der)
        files["data.bin"].write_bytes(data)
        files["signature.der"].write_bytes(signature)
        # 交給 openssl 的是 DER，不是卡片裡的 PEM 文字
        pubkey = subprocess.run([openssl, "x509", "-inform", "DER", "-in", str(files["cert.der"]), "-pubkey", "-noout"],
                                capture_output=True)
        if pubkey.returncode != 0:
            return False
        try:
            lines = [line.strip() for line in pubkey.stdout.decode("ascii").splitlines()]
            key_der = base64.b64decode("".join(line for line in lines if line and not line.startswith("-----")), validate=True)
        except ValueError:
            return False
        if len(key_der) != 91 or not key_der.startswith(P256_PUBLIC_KEY_PREFIX):
            return False  # 不是 P-256 的公鑰
        files["pubkey.pem"].write_bytes(pubkey.stdout)
        check = subprocess.run(
            [openssl, "dgst", "-sha256", "-verify", str(files["pubkey.pem"]), "-signature", str(files["signature.der"]),
             str(files["data.bin"])],
            capture_output=True,
        )
        return check.returncode == 0


def issued_by(cert_der: bytes, issuer_der: bytes):
    """cert 是不是 issuer 簽發的。回傳 True 或 False；沒辦法驗時回傳 None。"""
    try:
        from cryptography import x509
        from cryptography.exceptions import InvalidSignature
    except ImportError:
        signed_part, algorithm, signature = split_certificate(cert_der)
        if algorithm != ECDSA_SHA256:
            return False
        return _verify_with_openssl(issuer_der, signed_part, signature)
    try:
        x509.load_der_x509_certificate(cert_der).verify_directly_issued_by(x509.load_der_x509_certificate(issuer_der))
        return True
    except (InvalidSignature, ValueError, TypeError):
        return False


def certificate_validity(cert_der: bytes):
    """憑證的有效期限（兩個 ISO 格式的 UTC 時間）；沒有 cryptography 套件時回傳 None。"""
    try:
        from cryptography import x509
    except ImportError:
        return None
    try:
        cert = x509.load_der_x509_certificate(cert_der)
    except ValueError:
        return None
    return (f"{cert.not_valid_before_utc:%Y-%m-%dT%H:%M:%SZ}", f"{cert.not_valid_after_utc:%Y-%m-%dT%H:%M:%SZ}")


# ---------------------------------------------------------------- C2PA


def check_c2pa(data: bytes, root_der, record: dict):
    """驗卡片裡的 C2PA 資料。回傳（有沒有通過, 要印的說明）；沒辦法檢查時第一個值是 None。

    root_der 是確定屬於電台的根憑證，拿來當信任的起點；沒有的話只驗內容完整與簽章正確。
    """
    try:
        import c2pa
    except ImportError:
        return None, "沒有檢查（裝了 c2pa-python 才會驗：pip install c2pa-python）"
    import io

    # 不照檔案裡寫的網址去抓「放在別處的 manifest」，也不連憑證的撤銷查詢：這個工具不連網路
    options = {"verify": {"remote_manifest_fetch": False, "ocsp_fetch": False}}
    if root_der is not None:
        options["trust"] = {"user_anchors": to_pem(root_der).decode("ascii")}
    try:
        settings = c2pa.Settings.from_dict(options)
        with c2pa.Context(settings) as context:
            reader = c2pa.Reader.try_create("image/png", io.BytesIO(data), context=context)
            if reader is None:
                return False, "檔案裡的 C2PA 資料讀不出來"
            with reader:
                store = json.loads(reader.json())
                state = reader.get_validation_state()
    except (AttributeError, TypeError) as e:
        return None, f"沒有檢查（這一版的 c2pa-python 用不了，需要 0.39 以上：{e}）"
    except Exception as e:  # noqa: BLE001  c2pa 的各種錯誤都算驗不過
        return False, f"驗不過（{str(e)[:200]}）"

    if not isinstance(store, dict):
        return False, "檔案裡的 C2PA 資料讀不出來"
    manifests = store.get("manifests") if isinstance(store.get("manifests"), dict) else {}
    active = manifests.get(store.get("active_manifest")) if isinstance(store.get("active_manifest"), str) else None
    active = active if isinstance(active, dict) else {}
    results = (store.get("validation_results") or {}).get("activeManifest") or {}
    failures = [str(entry.get("code")) for entry in results.get("failure") or [] if isinstance(entry, dict)]
    serious = [code for code in failures if code != "signingCredential.untrusted"]
    assertions = [a for a in active.get("assertions") or [] if isinstance(a, dict)]
    inside = next((a.get("data") for a in assertions if a.get("label") == C2PA_ASSERTION), None)
    signer = str((active.get("signature_info") or {}).get("common_name") or "?")[:80]

    if state == "Invalid" or serious:
        return False, "驗不過：檔案在簽署之後被改過，或簽章不對（" + "、".join((serious or failures or ["Invalid"])[:5]) + "）"
    if inside != record:
        return False, "裡面的紀錄和簽過的紀錄不一樣"
    if state == "Trusted":
        return True, f"正確，簽署者（{signer}）的憑證是電台的根憑證簽發的"
    if root_der is not None:
        return False, f"內容完整，但簽署者（{signer}）的憑證不是電台的根憑證簽發的"
    return True, f"內容完整、簽章正確（簽署者 {signer}；沒有指定根憑證，所以沒有確認它是不是電台的）"


# ---------------------------------------------------------------- 主程式


def part(record: dict, key: str) -> dict:
    value = record.get(key)
    return value if isinstance(value, dict) else {}


def describe(record: dict) -> None:
    reception, listener = part(record, "reception"), part(record, "listener")
    field("序號", record.get("serial"))
    field("電台", record.get("station"))
    field("發行時間", record.get("issued_at"))
    field("發行網站", record.get("issuer"))
    modes = reception.get("modes")
    by_mode = "、".join(f"模式 {as_text(m)}：{as_text(n)} 幀" for m, n in modes.items()) if isinstance(modes, dict) else ""
    field("收到幀數", f"{as_text(reception.get('frames'))}（其中 {as_text(reception.get('frames_with_callsign'))} 幀解出呼號"
                      + (f"；{by_mode}" if by_mode else "") + "）")
    field("訊雜比", f"中位數 {as_text(reception.get('snr_db_median'))} dB、最高 {as_text(reception.get('snr_db_best'))} dB")
    field("錄音", f"{as_text(reception.get('audio_seconds'))} 秒，SHA-256 {as_text(reception.get('audio_sha256'))}")
    say("  以下是聽眾自己填的，電台沒有查核：")
    field("收聽者", listener.get("name") or "（沒填）", indent=4)
    field("日期時間", f"{as_text(listener.get('date') or '（沒填）')} {as_text(listener.get('time_utc') or '')} UTC", indent=4)
    frequency = listener.get("frequency_khz")
    field("頻率", f"{as_text(frequency)} kHz" if frequency is not None else "（沒填）", indent=4)
    field("地點", listener.get("location") or "（沒填）", indent=4)


def export(out: Path, record_text: str, signature: bytes, signing_der: bytes, root_der) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "record.json").write_bytes(record_text.encode("utf-8"))
    (out / "signature.der").write_bytes(signature)
    (out / "certificate.pem").write_bytes(to_pem(signing_der))
    names = "record.json、signature.der、certificate.pem（簽章憑證）"
    if root_der is not None:
        (out / "root.pem").write_bytes(to_pem(root_der))
        names += "、root.pem（卡片裡附的根憑證）"
    say(f"已存到 {out}：{names}")
    say("用 openssl 驗：")
    say("  openssl x509 -in certificate.pem -pubkey -noout > pubkey.pem")
    say("  openssl dgst -sha256 -verify pubkey.pem -signature signature.der record.json   # 紀錄的簽章")
    say("  openssl verify -CAfile root.pem certificate.pem                               # 簽章憑證是不是根憑證簽發的")
    say("  openssl x509 -in root.pem -noout -fingerprint -sha256                         # 根憑證的指紋")
    say("root.pem 要換成電台公布的根憑證（或比對指紋和電台公布的相同）才算數。")


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="backslashreplace")  # 終端機的編碼印不出中文時不要當掉
        except (AttributeError, ValueError):
            pass
    parser = argparse.ArgumentParser(description="離線查驗 NBTV 收聽卡")
    parser.add_argument("card", help="收聽卡的 PNG 檔（要是原檔；經過通訊軟體轉傳、截圖的通常已經沒有簽章資料）")
    anchor = parser.add_mutually_exclusive_group()
    anchor.add_argument("--cert", help="電台公布的根憑證（PEM）")
    anchor.add_argument("--fingerprint", help="電台公布的根憑證指紋（SHA-256，十六進位）")
    parser.add_argument("--export", metavar="目錄", help="把紀錄、簽章、憑證各存成一個檔案，可以改用 openssl 驗")
    args = parser.parse_args()

    # 先看使用者指定的信任起點。給了卻不能用（空字串、不是指紋、檔案讀不了）要明講，不能當成沒給
    wanted = given = given_fingerprint = None
    if args.fingerprint is not None:
        wanted = normalize_fingerprint(args.fingerprint)
        if wanted is None:
            parser.error("--fingerprint 要是 SHA-256 指紋：64 個十六進位字元，可以夾冒號或空白")
    if args.cert is not None:
        try:
            given = parse_pem_certificates(Path(args.cert).read_bytes().decode("ascii"), most=1)[0]
        except (OSError, ValueError):
            say(f"憑證：{args.cert} 讀不了。要是一個只放一張 PEM 憑證的檔案（電台公布的根憑證）")
            return UNDECIDED
        given_fingerprint = fingerprint_of(given)

    try:
        data = Path(args.card).read_bytes()
    except OSError as e:
        say(f"讀不了這個檔案：{e}")
        return UNDECIDED
    try:
        if len(data) > MAX_FILE_BYTES:
            raise ValueError("檔案太大，不是收聽卡")
        chunks = read_png(data)
        texts = card_texts(chunks)
        if sum(1 for kind, _ in chunks if kind == C2PA_CHUNK) > 1:
            raise ValueError("C2PA 的區塊不只一個")
    except ValueError as e:
        say(f"讀不了這個檔案：{e}")
        return FAILED
    if not all(key in texts for key in SIGNED_TEXT_KEYS):
        say("這張圖裡沒有收聽卡的簽章資料。")
        say("經過通訊軟體或社群網站轉傳、截圖的圖片通常會被拿掉這些資料；請改用卡片上的序號到網站查驗。")
        return FAILED
    record_text = texts[KEY_RECORD]
    try:
        record = json.loads(record_text)
        signature = base64.b64decode(texts[KEY_SIGNATURE], validate=True)
        embedded = parse_pem_certificates(texts[KEY_CERTIFICATE], most=2)
    except (ValueError, RecursionError) as e:
        say(f"簽章資料的格式不對：{e}")
        return FAILED
    if not isinstance(record, dict):
        say("簽章資料的格式不對")
        return FAILED
    signing = embedded[0]
    embedded_root = embedded[1] if len(embedded) > 1 else None
    signing_fingerprint = fingerprint_of(signing)
    embedded_root_fingerprint = fingerprint_of(embedded_root) if embedded_root else None

    if args.export:
        export(Path(args.export), record_text, signature, signing, embedded_root)
        return PASSED

    ok = True            # 到目前為止的檢查都沒有發現問題
    undecided = None     # 沒辦法下結論的原因

    # 一、紀錄有沒有被改過
    signed = verify_signature(signing, record_text.encode("utf-8"), signature)
    if signed is None:
        say("沒辦法驗簽章：需要 cryptography 套件（pip install cryptography），或系統裡有 openssl 指令")
        return UNDECIDED
    if signed:
        say("簽章：正確（紀錄沒有被改過）")
    else:
        say("簽章：不正確——紀錄被改過，或者不是卡片裡附的簽章憑證簽的")
        ok = False

    # 二、簽章憑證是不是電台的。trusted_root：確定屬於電台的根憑證
    trusted_root = None
    internal = issued_by(signing, embedded_root) if embedded_root else None  # 卡片裡附的兩張憑證接不接得起來
    if given is not None:
        if given_fingerprint == signing_fingerprint:
            say("憑證：卡片的簽章憑證就是你指定的那一張")
            trusted_root = embedded_root if internal else None
        elif issued_by(signing, given):
            say(f"憑證：簽章憑證是你指定的根憑證簽發的（根憑證指紋 {given_fingerprint}）")
            trusted_root = given
        else:
            say(f"憑證：簽章憑證不是 {args.cert} 簽發的")
            ok = False
    elif wanted is not None:
        if embedded_root and wanted == embedded_root_fingerprint and internal:
            say("憑證：卡片裡附的根憑證指紋和你給的相同，簽章憑證是它簽發的")
            trusted_root = embedded_root
        elif embedded_root and wanted == embedded_root_fingerprint:
            # 附了真的根憑證，簽的卻是別人的簽章憑證
            say("憑證：卡片裡附的根憑證指紋和你給的相同，但簽章憑證不是它簽發的")
            ok = False
        elif wanted == signing_fingerprint:
            say("憑證：簽章憑證的指紋和你給的相同")
            trusted_root = embedded_root if internal else None
        else:
            say(f"憑證：指紋不符——卡片裡附的根憑證是 {embedded_root_fingerprint}，簽章憑證是 {signing_fingerprint}")
            ok = False
    else:
        undecided = "沒有指定電台的根憑證。檔案本身前後一致，但任何人都做得出這樣的檔案"
        say("憑證：沒有指定要信任哪一張，所以沒辦法確認這張卡是不是電台發的")
        say(f"      卡片裡附的根憑證指紋 {embedded_root_fingerprint}")
        say(f"      簽章憑證指紋         {signing_fingerprint}")
        say("      用 --cert 或 --fingerprint 指定電台公布的根憑證之後再驗一次")
        if internal is False:
            say("      而且簽章憑證不是卡片裡附的根憑證簽發的")
            ok = False

    declared = part(record, "signature")
    if declared.get("certificate_sha256") not in (None, signing_fingerprint):
        say("      紀錄裡寫的簽章憑證指紋和實際簽的那一張不同")
        ok = False
    if trusted_root is not None and declared.get("root_sha256") not in (None, fingerprint_of(trusted_root)):
        say("      紀錄裡寫的根憑證指紋和驗證用的不同")
        ok = False
    validity = certificate_validity(signing)
    issued = record.get("issued_at")
    if validity and isinstance(issued, str) and issued and not (validity[0] <= issued <= validity[1]):
        say("      注意：卡片的發行時間不在簽章憑證的有效期限內")

    # 三、圖有沒有被改過
    card = part(record, "card")
    expected_image = card.get("image_sha256")
    if isinstance(expected_image, str):
        if image_sha256(chunks) == expected_image:
            say("畫面：和簽章時的一樣（整個圖檔都核對過）")
        else:
            say("畫面：和簽章時的不一樣——圖檔被改過")
            ok = False
    else:
        problem = early_format_problem(chunks)
        if problem is not None:
            say(f"畫面：這是早期格式的卡，但圖檔不是本站發出去的樣子（{problem}）——圖檔被改過")
            ok = False
        else:
            try:
                import io

                from PIL import Image

                image = Image.open(io.BytesIO(data)).convert("RGB")
                if hashlib.sha256(image.tobytes()).hexdigest() == card.get("pixels_sha256"):
                    say("畫面：和簽章時的一樣（早期格式的卡，核對的是像素）")
                else:
                    say("畫面：和簽章時的不一樣——圖被改過")
                    ok = False
            except ImportError:
                say("畫面：沒辦法核對——這是早期格式的卡，要裝 Pillow 才能比對像素（pip install pillow）")
                undecided = undecided or "畫面沒有核對（早期格式的卡，這台電腦沒有 Pillow）"
            except Exception:  # noqa: BLE001  圖解不開
                say("畫面：圖解不開——圖檔被改過或損壞")
                ok = False

    # 四、C2PA。前面已經沒通過的檔案就不再交給 C2PA 的程式庫讀了
    promised = bool(card.get("c2pa"))
    has_c2pa = any(kind == C2PA_CHUNK for kind, _ in chunks)
    if has_c2pa and not ok:
        say("C2PA：前面的檢查沒有通過，不再檢查")
    elif has_c2pa:
        passed, message = check_c2pa(data, trusted_root, record)
        say(f"C2PA：{message}")
        if passed is False:
            ok = False
    elif promised:
        say("C2PA：紀錄說這張卡附有 C2PA 的資料，但檔案裡沒有——被拿掉了")
        ok = False
    else:
        say("C2PA：這張卡沒有附 C2PA 的資料")

    say("紀錄內容（卡片裡寫的）：" if ok and undecided is None else "卡片裡寫的內容（沒有通過查驗的話，這些都不可信）：")
    describe(record)
    say("────────────────────────")
    if not ok:
        say("結果：沒有通過")
        return FAILED
    if undecided is not None:
        say(f"結果：沒有確認——{undecided}")
        return UNDECIDED
    say("結果：通過")
    return PASSED


if __name__ == "__main__":
    sys.exit(main())
