#!/usr/bin/env python3
"""離線查驗 NBTV 收聽卡：不連網站，只看卡片檔案本身與電台公布的根憑證。

    python3 verify_card.py 卡片.png --cert 電台公布的根憑證.pem
    python3 verify_card.py 卡片.png --fingerprint 電台公布的根憑證指紋
    python3 verify_card.py 卡片.png --export 目錄      # 拆出紀錄、簽章、憑證，改用 openssl 驗

這個檔案可以單獨拿去用，不需要網站的其他程式。簽章用 cryptography 套件驗（pip install cryptography），
沒有裝的話改用系統的 openssl 指令。另外兩項檢查要有對應的套件才會做：
    Pillow        卡片的畫面有沒有被改過
    c2pa-python   卡片裡的 C2PA（Content Credentials）資料

卡片是一張 PNG，裡面有三個文字區塊（iTXt）：
    nbtv-card-record       簽過的紀錄（JSON 文字；簽的就是這段文字的 UTF-8 位元組）
    nbtv-card-signature    簽章（ECDSA P-256／SHA-256，DER 格式再轉 base64）
    nbtv-card-certificate  兩張憑證（PEM）：簽章憑證，接著簽發它的根憑證
以及一個 caBX 區塊：C2PA 的資料，裡面有同一份紀錄，由同一張簽章憑證簽署。

憑證都是電台自己簽發的，沒有經過憑證機構。任何人都做得出寫著同樣名字的憑證，所以
「簽章和卡片裡附的憑證相符」只代表檔案前後一致；要確定是電台發的，根憑證（或它的指紋）
必須來自電台公布的地方，用 --cert 或 --fingerprint 指定。
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import struct
import sys
import zlib
from pathlib import Path

KEY_RECORD = "nbtv-card-record"
KEY_SIGNATURE = "nbtv-card-signature"
KEY_CERTIFICATE = "nbtv-card-certificate"
C2PA_ASSERTION = "tw.hf-nbtv.reception-card"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

_PEM_BLOCK = re.compile(rb"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", re.S)


def read_chunks(data: bytes):
    """走一遍 PNG 的區塊：回傳（所有 iTXt 區塊的 {關鍵字: 文字}, 出現過的區塊種類）。"""
    if not data.startswith(PNG_MAGIC):
        raise ValueError("不是 PNG 檔")
    texts, kinds = {}, set()
    pos = len(PNG_MAGIC)
    while pos + 12 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + length]
        if len(body) != length or pos + 12 + length > len(data):
            raise ValueError("PNG 檔不完整")
        (crc,) = struct.unpack_from(">I", data, pos + 8 + length)
        if zlib.crc32(kind + body) != crc:
            raise ValueError("PNG 的區塊檢查碼不對，檔案損壞")
        kinds.add(kind.decode("latin-1"))
        if kind == b"iTXt":
            keyword, _, rest = body.partition(b"\0")
            if len(rest) >= 2:
                compressed = rest[0]
                _language, _, rest = rest[2:].partition(b"\0")
                _translated, _, text = rest.partition(b"\0")
                if compressed:
                    text = zlib.decompress(text)
                texts[keyword.decode("latin-1")] = text.decode("utf-8")
        elif kind == b"IEND":
            break
        pos += 12 + length
    return texts, kinds


def split_pem(pem: bytes):
    """PEM 文字裡的每一張憑證，各自一段。"""
    return [block + b"\n" for block in _PEM_BLOCK.findall(pem)]


def der_of(cert_pem: bytes) -> bytes:
    lines = [line.strip() for line in cert_pem.decode("ascii").splitlines()]
    begin, end = lines.index("-----BEGIN CERTIFICATE-----"), lines.index("-----END CERTIFICATE-----")
    return base64.b64decode("".join(lines[begin + 1:end]))


def fingerprint_of(cert_pem: bytes) -> str:
    """憑證的 SHA-256 指紋：對 DER 編碼取雜湊，和 openssl x509 -fingerprint -sha256 一樣。"""
    return hashlib.sha256(der_of(cert_pem)).hexdigest()


def normalize_fingerprint(text: str) -> str:
    return "".join(ch for ch in text.lower() if ch in "0123456789abcdef")


# ---------------------------------------------------------------- 簽章


def verify_signature(cert_pem: bytes, data: bytes, signature: bytes):
    """用憑證裡的公鑰驗 ECDSA／SHA-256 的簽章。回傳 True 或 False；兩種驗法都不能用時回傳 None。

    有 cryptography 套件就用它，沒有的話改呼叫系統的 openssl 指令。
    """
    try:
        from cryptography import x509
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError:
        return _verify_with_openssl(cert_pem, data, signature)
    try:
        public_key = x509.load_pem_x509_certificate(cert_pem).public_key()
        if not isinstance(public_key, ec.EllipticCurvePublicKey):
            return False
        public_key.verify(signature, data, ec.ECDSA(hashes.SHA256()))
        return True
    except (InvalidSignature, ValueError):
        return False


def _verify_with_openssl(cert_pem: bytes, data: bytes, signature: bytes):
    import shutil
    import subprocess
    import tempfile

    openssl = shutil.which("openssl")
    if openssl is None:
        return None
    with tempfile.TemporaryDirectory() as tmp:
        files = {name: Path(tmp) / name for name in ("cert.pem", "pubkey.pem", "data.bin", "signature.der")}
        files["cert.pem"].write_bytes(cert_pem)
        files["data.bin"].write_bytes(data)
        files["signature.der"].write_bytes(signature)
        pubkey = subprocess.run([openssl, "x509", "-in", str(files["cert.pem"]), "-pubkey", "-noout"], capture_output=True)
        if pubkey.returncode != 0:
            return False
        files["pubkey.pem"].write_bytes(pubkey.stdout)
        check = subprocess.run(
            [openssl, "dgst", "-sha256", "-verify", str(files["pubkey.pem"]), "-signature", str(files["signature.der"]),
             str(files["data.bin"])],
            capture_output=True,
        )
        return check.returncode == 0


def _tlv(der: bytes, pos: int):
    """DER 的一個元素：回傳（標籤, 內容開始的位置, 內容結束的位置）。"""
    tag, first = der[pos], der[pos + 1]
    if first < 0x80:
        start, length = pos + 2, first
    else:
        count = first & 0x7F
        start, length = pos + 2 + count, int.from_bytes(der[pos + 2:pos + 2 + count], "big")
    if start + length > len(der):
        raise ValueError("憑證的編碼不完整")
    return tag, start, start + length


def _split_certificate(der: bytes):
    """憑證 = 被簽的內容（tbsCertificate）＋演算法＋簽章。回傳（被簽的內容, 簽章）。"""
    _, start, _ = _tlv(der, 0)
    _, _, tbs_end = _tlv(der, start)
    _, _, algorithm_end = _tlv(der, tbs_end)
    tag, signature_start, signature_end = _tlv(der, algorithm_end)
    if tag != 0x03:
        raise ValueError("憑證的編碼不對")
    return der[start:tbs_end], der[signature_start + 1:signature_end]  # 位元字串的第一個位元組是「沒用到幾個位元」


def issued_by(cert_pem: bytes, issuer_pem: bytes):
    """cert 是不是 issuer 簽發的。回傳 True 或 False；沒辦法驗時回傳 None。"""
    try:
        from cryptography import x509
        from cryptography.exceptions import InvalidSignature
    except ImportError:
        try:
            signed_part, signature = _split_certificate(der_of(cert_pem))
        except (ValueError, IndexError):
            return False
        return _verify_with_openssl(issuer_pem, signed_part, signature)
    try:
        x509.load_pem_x509_certificate(cert_pem).verify_directly_issued_by(x509.load_pem_x509_certificate(issuer_pem))
        return True
    except (InvalidSignature, ValueError, TypeError):
        return False


def certificate_validity(cert_pem: bytes):
    """憑證的有效期限（兩個 ISO 格式的 UTC 時間）；沒有 cryptography 套件時回傳 None。"""
    try:
        from cryptography import x509
    except ImportError:
        return None
    try:
        cert = x509.load_pem_x509_certificate(cert_pem)
    except ValueError:
        return None
    return (f"{cert.not_valid_before_utc:%Y-%m-%dT%H:%M:%SZ}", f"{cert.not_valid_after_utc:%Y-%m-%dT%H:%M:%SZ}")


# ---------------------------------------------------------------- C2PA


def check_c2pa(data: bytes, root_pem, record: dict):
    """驗卡片裡的 C2PA 資料。回傳（有沒有通過, 要印的說明）；沒辦法檢查時第一個值是 None。

    root_pem 是確定屬於電台的根憑證，拿來當信任的起點；沒有的話只驗內容完整與簽章正確。
    """
    try:
        import c2pa
    except ImportError:
        return None, "沒有檢查（裝了 c2pa-python 才會驗：pip install c2pa-python）"
    import io

    try:
        settings = c2pa.Settings.from_dict({"trust": {"user_anchors": root_pem.decode("ascii")}} if root_pem else {})
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
        return False, f"驗不過（{e}）"

    active = (store.get("manifests") or {}).get(store.get("active_manifest") or "", {})
    results = (store.get("validation_results") or {}).get("activeManifest") or {}
    failures = [str(entry.get("code")) for entry in results.get("failure") or []]
    serious = [code for code in failures if code != "signingCredential.untrusted"]
    inside = next((a.get("data") for a in active.get("assertions") or [] if a.get("label") == C2PA_ASSERTION), None)
    signer = (active.get("signature_info") or {}).get("common_name") or "?"

    if state == "Invalid" or serious:
        return False, "驗不過：檔案在簽署之後被改過，或簽章不對（" + "、".join(serious or failures or ["Invalid"]) + "）"
    if inside != record:
        return False, "裡面的紀錄和簽過的紀錄不一樣"
    if state == "Trusted":
        return True, f"正確，簽署者（{signer}）的憑證是電台的根憑證簽發的"
    if root_pem is not None:
        return False, f"內容完整，但簽署者（{signer}）的憑證不是電台的根憑證簽發的"
    return True, f"內容完整、簽章正確（簽署者 {signer}；沒有指定根憑證，所以沒有確認它是不是電台的）"


# ---------------------------------------------------------------- 主程式


def describe(record: dict) -> None:
    reception = record.get("reception", {})
    listener = record.get("listener", {})
    print(f"  序號          {record.get('serial')}")
    print(f"  電台          {record.get('station')}")
    print(f"  發行時間      {record.get('issued_at')}")
    print(f"  發行網站      {record.get('issuer')}")
    modes = "、".join(f"模式 {m}：{n} 幀" for m, n in (reception.get("modes") or {}).items())
    print(f"  收到幀數      {reception.get('frames')}（其中 {reception.get('frames_with_callsign')} 幀解出呼號；{modes}）")
    print(f"  訊雜比        中位數 {reception.get('snr_db_median')} dB、最高 {reception.get('snr_db_best')} dB")
    print(f"  錄音          {reception.get('audio_seconds')} 秒，SHA-256 {reception.get('audio_sha256')}")
    print("  以下是聽眾自己填的，電台沒有查核：")
    print(f"    收聽者      {listener.get('name') or '（沒填）'}")
    print(f"    日期時間    {listener.get('date') or '（沒填）'} {listener.get('time_utc') or ''} UTC")
    frequency = listener.get("frequency_khz")
    print(f"    頻率        {str(frequency) + ' kHz' if frequency is not None else '（沒填）'}")
    print(f"    地點        {listener.get('location') or '（沒填）'}")


def export(out: Path, record_text: str, signature: bytes, signing_pem: bytes, root_pem) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "record.json").write_bytes(record_text.encode("utf-8"))
    (out / "signature.der").write_bytes(signature)
    (out / "certificate.pem").write_bytes(signing_pem)
    names = "record.json、signature.der、certificate.pem（簽章憑證）"
    if root_pem is not None:
        (out / "root.pem").write_bytes(root_pem)
        names += "、root.pem（卡片裡附的根憑證）"
    print(f"已存到 {out}：{names}")
    print("用 openssl 驗：")
    print("  openssl x509 -in certificate.pem -pubkey -noout > pubkey.pem")
    print("  openssl dgst -sha256 -verify pubkey.pem -signature signature.der record.json   # 紀錄的簽章")
    print("  openssl verify -CAfile root.pem certificate.pem                               # 簽章憑證是不是根憑證簽發的")
    print("  openssl x509 -in root.pem -noout -fingerprint -sha256                         # 根憑證的指紋")
    print("root.pem 要換成電台公布的根憑證（或比對指紋和電台公布的相同）才算數。")


def main() -> int:
    parser = argparse.ArgumentParser(description="離線查驗 NBTV 收聽卡")
    parser.add_argument("card", help="收聽卡的 PNG 檔（要是原檔；經過通訊軟體轉傳、截圖的通常已經沒有簽章資料）")
    parser.add_argument("--cert", help="電台公布的根憑證（PEM）")
    parser.add_argument("--fingerprint", help="電台公布的根憑證指紋（SHA-256，十六進位）")
    parser.add_argument("--export", metavar="目錄", help="把紀錄、簽章、憑證各存成一個檔案，可以改用 openssl 驗")
    args = parser.parse_args()

    data = Path(args.card).read_bytes()
    try:
        chunks, kinds = read_chunks(data)
    except ValueError as e:
        print(f"讀不了這個檔案：{e}")
        return 1
    if not all(key in chunks for key in (KEY_RECORD, KEY_SIGNATURE, KEY_CERTIFICATE)):
        print("這張圖裡沒有收聽卡的簽章資料。")
        print("經過通訊軟體或社群網站轉傳、截圖的圖片通常會被拿掉這些資料；請改用卡片上的序號到網站查驗。")
        return 1
    record_text = chunks[KEY_RECORD]
    try:
        record = json.loads(record_text)
        signature = base64.b64decode(chunks[KEY_SIGNATURE], validate=True)
        embedded = split_pem(chunks[KEY_CERTIFICATE].encode("ascii"))
        signing_pem = embedded[0]
        embedded_root = embedded[1] if len(embedded) > 1 else None
        signing_fingerprint = fingerprint_of(signing_pem)
        embedded_root_fingerprint = fingerprint_of(embedded_root) if embedded_root else None
    except (ValueError, IndexError, UnicodeEncodeError) as e:
        print(f"簽章資料的格式不對：{e}")
        return 1
    if not isinstance(record, dict):
        print("簽章資料的格式不對")
        return 1

    if args.export:
        export(Path(args.export), record_text, signature, signing_pem, embedded_root)
        return 0

    ok = True

    # 一、紀錄有沒有被改過
    signed = verify_signature(signing_pem, record_text.encode("utf-8"), signature)
    if signed is None:
        print("沒辦法驗簽章：需要 cryptography 套件（pip install cryptography），或系統裡有 openssl 指令")
        return 2
    if signed:
        print("簽章：正確（紀錄沒有被改過）")
    else:
        print("簽章：不正確——紀錄被改過，或者不是卡片裡附的簽章憑證簽的")
        ok = False

    # 二、簽章憑證是不是電台的。trusted_root：確定屬於電台的根憑證
    trusted_root = None
    internal = issued_by(signing_pem, embedded_root) if embedded_root else None  # 卡片裡附的兩張憑證接不接得起來
    if args.cert:
        try:
            given = split_pem(Path(args.cert).read_bytes())[0]
            given_fingerprint = fingerprint_of(given)
        except (OSError, ValueError, IndexError):
            print(f"憑證：{args.cert} 讀不了")
            return 1
        if given_fingerprint == signing_fingerprint:
            print("憑證：卡片的簽章憑證就是你指定的那一張")
            trusted_root = embedded_root if internal else None
        else:
            chained = issued_by(signing_pem, given)
            if chained:
                print(f"憑證：簽章憑證是你指定的根憑證簽發的（根憑證指紋 {given_fingerprint}）")
                trusted_root = given
            else:
                print(f"憑證：簽章憑證不是 {args.cert} 簽發的")
                ok = False
    elif args.fingerprint:
        wanted = normalize_fingerprint(args.fingerprint)
        if embedded_root and wanted == embedded_root_fingerprint and internal:
            print("憑證：卡片裡附的根憑證指紋和你給的相同，簽章憑證是它簽發的")
            trusted_root = embedded_root
        elif embedded_root and wanted == embedded_root_fingerprint:
            # 附了真的根憑證，簽的卻是別人的簽章憑證
            print("憑證：卡片裡附的根憑證指紋和你給的相同，但簽章憑證不是它簽發的")
            ok = False
        elif wanted == signing_fingerprint:
            print("憑證：簽章憑證的指紋和你給的相同")
            trusted_root = embedded_root if internal else None
        else:
            print(f"憑證：指紋不符——卡片裡附的根憑證是 {embedded_root_fingerprint}，簽章憑證是 {signing_fingerprint}")
            ok = False
    else:
        print("憑證：沒有指定要信任哪一張，所以下面的結果只代表檔案前後一致")
        print(f"      卡片裡附的根憑證指紋 {embedded_root_fingerprint}")
        print(f"      簽章憑證指紋         {signing_fingerprint}")
        print("      請比對根憑證的指紋是不是電台公布的，或用 --cert／--fingerprint 指定")
        if internal is False:
            print("      而且簽章憑證不是卡片裡附的根憑證簽發的")
            ok = False

    declared = record.get("signature") or {}
    if declared.get("certificate_sha256") not in (None, signing_fingerprint):
        print("      紀錄裡寫的簽章憑證指紋和實際簽的那一張不同")
        ok = False
    if trusted_root is not None and declared.get("root_sha256") not in (None, fingerprint_of(trusted_root)):
        print("      紀錄裡寫的根憑證指紋和驗證用的不同")
        ok = False
    validity = certificate_validity(signing_pem)
    issued = str(record.get("issued_at", ""))
    if validity and issued and not (validity[0] <= issued <= validity[1]):
        print("      注意：卡片的發行時間不在簽章憑證的有效期限內")

    # 三、畫面有沒有被改過
    expected_pixels = (record.get("card") or {}).get("pixels_sha256")
    try:
        import io

        from PIL import Image

        image = Image.open(io.BytesIO(data)).convert("RGB")
        if hashlib.sha256(image.tobytes()).hexdigest() == expected_pixels:
            print("畫面：和簽章時的一樣")
        else:
            print("畫面：和簽章時的不一樣——圖被改過")
            ok = False
    except ImportError:
        print("畫面：沒有檢查（裝了 Pillow 才會比對：pip install pillow）")

    # 四、C2PA
    promised = bool((record.get("card") or {}).get("c2pa"))
    if "caBX" in kinds:
        passed, message = check_c2pa(data, trusted_root, record)
        print(f"C2PA：{message}")
        if passed is False:
            ok = False
    elif promised:
        print("C2PA：紀錄說這張卡附有 C2PA 的資料，但檔案裡沒有——被拿掉了")
        ok = False
    else:
        print("C2PA：這張卡沒有附 C2PA 的資料")

    print("紀錄內容：")
    describe(record)
    print("結果：" + ("通過" if ok else "沒有通過"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
