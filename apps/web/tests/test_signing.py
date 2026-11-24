"""憑證（根憑證＋簽章憑證）、紀錄的簽章、卡片檔、離線查驗工具，以及 C2PA。"""

from __future__ import annotations

import base64
import datetime
import http.server
import io
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import serialization
from PIL import Image

from nbtvweb import provenance
from nbtvweb.cardfile import (
    attach, canonical_json, decode_png, encode_image, encode_png, image_sha256, join_chunks, pixels_sha256, split_chunks,
)
from nbtvweb.signing import (
    EKU_C2PA_CLAIM_SIGNING, EKU_DOCUMENT_SIGNING, ROOT_CERT, ROOT_KEY, SIGNING_CERT, SIGNING_KEY, Signer, SigningError,
    certificate_fingerprint, create_identity, issued_by, load_certificates, verify,
)

from . import util

try:
    import c2pa  # noqa: F401
    HAVE_C2PA = True
except ImportError:
    HAVE_C2PA = False


def rewrite_chunks(png: bytes, change) -> bytes:
    """走一遍 PNG 的區塊：change(種類, 內容) 回傳新的內容，回傳 None 表示拿掉這個區塊。檢查碼會重算。"""
    out, pos = bytearray(png[:8]), 8
    while pos < len(png):
        length, kind = struct.unpack_from(">I4s", png, pos)
        body = change(kind, png[pos + 8:pos + 8 + length])
        if body is not None:
            out += struct.pack(">I4s", len(body), kind) + body + struct.pack(">I", zlib.crc32(kind + body))
        pos += 12 + length
    return bytes(out)


def rename_listener(png: bytes) -> bytes:
    """把紀錄裡的名字換掉（只動那一個文字區塊）。"""
    changed = rewrite_chunks(png, lambda kind, body: body.replace("山田".encode(), "佐藤".encode()) if kind == b"iTXt" else body)
    assert changed != png
    return changed


def add_chunks(png: bytes, before_pixels=(), at_end=()) -> bytes:
    """在像素資料之前、以及檔案結尾之前，各插入幾個區塊。"""
    out, placed = [], False
    for kind, body in split_chunks(png):
        if kind == b"IDAT" and not placed:
            out += list(before_pixels)
            placed = True
        if kind == b"IEND":
            out += list(at_end)
        out.append((kind, body))
    return join_chunks(out)


def animation_frame(size: tuple[int, int], color: tuple[int, int, int]) -> tuple[list, list]:
    """一格動畫要用的區塊：（放在像素資料之前的, 放在結尾之前的）。
    加進一張 PNG 之後，原本的畫面變成「不屬於動畫的預設畫面」，支援動畫的看圖軟體顯示的是這一格。"""
    out = io.BytesIO()
    Image.new("RGB", size).save(out, format="PNG", save_all=True, append_images=[Image.new("RGB", size, color)], default_image=True)
    chunks = split_chunks(out.getvalue())
    return [c for c in chunks if c[0] == b"acTL"], [c for c in chunks if c[0] in (b"fcTL", b"fdAT")]


def cert_lines(pem: bytes) -> list[str]:
    """一張 PEM 憑證中間的 base64 那幾行。"""
    return pem.decode("ascii").strip().splitlines()[1:-1]


def der_of(pem: bytes) -> bytes:
    return x509.load_pem_x509_certificate(pem).public_bytes(serialization.Encoding.DER)


def pem_of(der: bytes) -> str:
    text = base64.b64encode(der).decode("ascii")
    lines = [text[i:i + 64] for i in range(0, len(text), 64)]
    return "\n".join(["-----BEGIN CERTIFICATE-----", *lines, "-----END CERTIFICATE-----"]) + "\n"


class IdentityCase(unittest.TestCase):
    """共用：我們的憑證，以及另一個人照樣做的一套（名字一模一樣）。"""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.keys = cls.dir / "keys"
        create_identity(cls.keys, "BX4ACP")
        cls.signer = Signer(cls.keys)
        cls.root_file = str(cls.keys / ROOT_CERT)
        create_identity(cls.dir / "impostor", "BX4ACP")
        cls.impostor = Signer(cls.dir / "impostor")
        cls.impostor_root_file = str(cls.dir / "impostor" / ROOT_CERT)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def make_card(self, signer: Signer | None = None, *, chain: bytes | str | None = None, says_c2pa: bool = False,
                  embedder=None, early: bool = False, listener: dict | None = None, picture: bytes | None = None,
                  claims: dict | None = None) -> tuple[bytes, dict]:
        """做一張卡。early：早期的格式，紀錄裡只有像素的雜湊；picture：自己準備的圖檔（PNG）；
        chain：憑證區塊裡要放的文字（預設是簽章憑證接著根憑證）；claims：紀錄的 signature 欄位要寫的指紋。"""
        signer = signer or self.signer
        image = Image.new("RGB", (64, 48), (10, 20, 30))
        picture = picture if picture is not None else encode_image(image)
        card = {"pixels_sha256": pixels_sha256(Image.open(io.BytesIO(picture))), "c2pa": says_c2pa or embedder is not None}
        if not early:
            card["image_sha256"] = image_sha256(picture)
        record = {
            "v": 1, "serial": "7KQ2-M9XD-4TFA", "station": "BX4ACP",
            "listener": listener if listener is not None else {"name": "山田", "frequency_khz": 7165.0},
            "issued_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "card": card,
            "signature": claims or {"certificate_sha256": signer.fingerprint, "root_sha256": signer.root_fingerprint},
        }
        text = canonical_json(record)
        chain = chain if chain is not None else signer.chain_pem
        chain = chain.encode("ascii") if isinstance(chain, str) else chain
        png = attach(picture, text, signer.sign(text.encode()), chain)
        if embedder is not None:
            png = embedder.embed(png, record, "test card")
        return png, record

    def save(self, name: str, png: bytes) -> str:
        path = self.dir / name
        path.write_bytes(png)
        return str(path)

    def run_verifier(self, *args: str, bare: bool = False) -> subprocess.CompletedProcess:
        # bare：用 -S 啟動 Python，裝在虛擬環境裡的套件都載不到，等於一台只有 Python 與 openssl 的電腦
        python = [sys.executable, "-S"] if bare else [sys.executable]
        return subprocess.run([*python, str(util.WEB_DIR / "verify_card.py"), *args], capture_output=True, text=True)

    def assertVerifier(self, expected: int, *args: str, bare: bool = False, says: str | None = None):
        result = self.run_verifier(*args, bare=bare)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        if says is not None:
            self.assertIn(says, result.stdout)
        return result


class SigningTests(IdentityCase):
    def test_identity_files(self):
        for name in (ROOT_KEY, SIGNING_KEY):
            self.assertEqual((self.keys / name).stat().st_mode & 0o777, 0o600, name)
        self.assertEqual(self.keys.stat().st_mode & 0o777, 0o700)
        self.assertFalse(self.signer.key_file_exposed)
        self.assertTrue(self.signer.root_key_present)

        root, leaf = self.signer.root, self.signer.cert
        self.assertEqual(root.subject, root.issuer)                      # 根憑證自己簽自己
        constraints = root.extensions.get_extension_for_class(x509.BasicConstraints).value
        self.assertEqual((constraints.ca, constraints.path_length), (True, 0))
        self.assertTrue(root.extensions.get_extension_for_class(x509.KeyUsage).value.key_cert_sign)
        self.assertGreater(root.not_valid_after_utc - root.not_valid_before_utc, datetime.timedelta(days=7000))

        self.assertEqual(leaf.issuer, root.subject)                      # 簽章憑證是根憑證簽發的
        self.assertNotEqual(leaf.subject, leaf.issuer)
        self.assertFalse(leaf.extensions.get_extension_for_class(x509.BasicConstraints).value.ca)
        usage = leaf.extensions.get_extension_for_class(x509.KeyUsage).value
        self.assertTrue(usage.digital_signature)
        self.assertFalse(usage.key_cert_sign)
        purposes = list(leaf.extensions.get_extension_for_class(x509.ExtendedKeyUsage).value)
        self.assertEqual(purposes, [EKU_DOCUMENT_SIGNING, EKU_C2PA_CLAIM_SIGNING])
        self.assertGreater(leaf.not_valid_after_utc - leaf.not_valid_before_utc, datetime.timedelta(days=3600))
        self.assertLessEqual(leaf.not_valid_after_utc, root.not_valid_after_utc)
        self.assertIn("BX4ACP", leaf.subject.rfc4514_string())

        self.assertTrue(issued_by(leaf, root))
        self.assertFalse(issued_by(leaf, self.impostor.root))            # 名字一樣的另一張根憑證
        self.assertFalse(issued_by(root, leaf))
        self.assertNotEqual(self.signer.fingerprint, self.signer.root_fingerprint)
        self.assertEqual([certificate_fingerprint(c) for c in load_certificates(self.signer.chain_pem)],
                         [self.signer.fingerprint, self.signer.root_fingerprint])
        with self.assertRaises(SigningError):                             # 不覆蓋已經有的金鑰
            create_identity(self.keys, "BX4ACP")

    def test_site_does_not_need_the_root_key(self):
        copy = self.dir / "online"
        shutil.copytree(self.keys, copy)
        (copy / ROOT_KEY).unlink()                                         # 根憑證的私鑰移到別的地方保管了
        signer = Signer(copy)
        self.assertFalse(signer.root_key_present)
        self.assertTrue(verify(signer.cert, b"x", signer.sign(b"x")))

    def test_mismatched_files_are_refused(self):
        wrong_root = self.dir / "wrong-root"
        shutil.copytree(self.keys, wrong_root)
        shutil.copy(self.impostor_root_file, wrong_root / ROOT_CERT)
        with self.assertRaises(SigningError):
            Signer(wrong_root)
        wrong_key = self.dir / "wrong-key"
        shutil.copytree(self.keys, wrong_key)
        shutil.copy(self.dir / "impostor" / SIGNING_KEY, wrong_key / SIGNING_KEY)
        with self.assertRaises(SigningError):
            Signer(wrong_key)
        with self.assertRaises(SigningError):
            Signer(self.dir / "nothing-here")

    def test_sign_and_verify(self):
        data = "收聽卡".encode()
        signature = self.signer.sign(data)
        self.assertTrue(verify(self.signer.cert, data, signature))
        self.assertFalse(verify(self.signer.cert, data + b"x", signature))
        self.assertFalse(verify(self.signer.cert, data, signature[:-1] + bytes([signature[-1] ^ 1])))
        self.assertFalse(verify(self.impostor.cert, data, signature))
        self.assertFalse(verify(self.signer.root, data, signature))      # 簽的是簽章憑證，不是根憑證

    def test_card_file_round_trip(self):
        png, record = self.make_card()
        card = decode_png(png)
        self.assertEqual(card.record, record)
        self.assertIn("山田", card.record_text)  # 中文直接寫，不轉成 \u
        leaf, root = load_certificates(card.cert_pem)
        self.assertTrue(verify(leaf, card.record_text.encode(), card.signature))
        self.assertTrue(issued_by(leaf, root))
        self.assertEqual(certificate_fingerprint(root), self.signer.root_fingerprint)
        self.assertEqual(card.pixels_sha256, record["card"]["pixels_sha256"])
        plain = io.BytesIO()
        Image.new("RGB", (4, 4)).save(plain, format="PNG")
        with self.assertRaises(ValueError):
            decode_png(plain.getvalue())


class OfflineVerifierTests(IdentityCase):
    def test_genuine_card(self):
        good = self.save("good.png", self.make_card()[0])
        result = self.assertVerifier(0, good, "--cert", self.root_file, says="簽章憑證是你指定的根憑證簽發的")
        self.assertIn("畫面：和簽章時的一樣", result.stdout)
        self.assertIn("C2PA：這張卡沒有附 C2PA 的資料", result.stdout)
        self.assertEqual(result.stdout.splitlines()[-1], "結果：通過")
        self.assertVerifier(0, good, "--cert", str(self.keys / SIGNING_CERT), says="就是你指定的那一張")
        self.assertVerifier(0, good, "--fingerprint", self.signer.root_fingerprint.upper(), says="根憑證指紋和你給的相同")
        spaced = " ".join(self.signer.root_fingerprint[i:i + 4] for i in range(0, 64, 4))
        self.assertVerifier(0, good, "--fingerprint", spaced)             # 照查驗頁上的寫法，四個字一組
        colons = ":".join(self.signer.root_fingerprint[i:i + 2] for i in range(0, 64, 2)).upper()
        self.assertVerifier(0, good, "--fingerprint", "sha256 Fingerprint=" + colons)  # 整行貼上 openssl 印的
        self.assertVerifier(0, good, "--fingerprint", self.signer.fingerprint)
        crlf = self.dir / "root-crlf.pem"                                  # 根憑證檔用 Windows 的換行也可以
        crlf.write_bytes(self.signer.root_pem.replace(b"\n", b"\r\n"))
        self.assertVerifier(0, good, "--cert", str(crlf))

    def test_no_trust_anchor_is_not_a_pass(self):
        # 沒說要信任誰：檔案前後一致不代表是電台發的，不能說通過
        good = self.save("good-no-anchor.png", self.make_card()[0])
        result = self.assertVerifier(2, good, says="沒辦法確認這張卡是不是電台發的")
        self.assertIn(self.signer.root_fingerprint, result.stdout)
        self.assertTrue(result.stdout.splitlines()[-1].startswith("結果：沒有確認"))
        self.assertNotIn("結果：通過", result.stdout)
        # 別人用自己做的憑證（名字和我們的一模一樣）發的卡也是前後一致的
        forged = self.save("forged-no-anchor.png", self.make_card(self.impostor)[0])
        self.assertNotIn("結果：通過", self.assertVerifier(2, forged).stdout)
        # 給了卻不能用的：空字串（例如指令稿裡的變數沒有值）、不是指紋、檔案不存在、兩個都給。不能當成沒給
        two = self.dir / "two.pem"
        two.write_bytes(self.signer.chain_pem)                             # 檔案裡不只一張憑證
        noisy = self.dir / "noisy.pem"
        noisy.write_bytes(b"Certificate:\n" + self.signer.root_pem)        # 夾了別的字
        for args in (("--fingerprint", ""), ("--fingerprint", "1234"), ("--fingerprint", "zz" * 32), ("--cert", ""),
                     ("--cert", str(self.dir / "no-such-file.pem")), ("--cert", str(two)), ("--cert", str(noisy)),
                     ("--cert", self.root_file, "--fingerprint", self.signer.root_fingerprint)):
            for card in (good, forged):
                result = self.assertVerifier(2, card, *args)
                self.assertNotIn("結果：通過", result.stdout)

    def test_wrong_trust_anchor(self):
        good = self.save("good2.png", self.make_card()[0])
        self.assertVerifier(1, good, "--fingerprint", "00" * 32, says="指紋不符")
        self.assertVerifier(1, good, "--cert", self.impostor_root_file, says="不是")

    def test_card_made_by_someone_else(self):
        # 別人用自己做的憑證（名字和我們的一模一樣）發的卡
        forged = self.save("forged.png", self.make_card(self.impostor)[0])
        self.assertVerifier(1, forged, "--cert", self.root_file)
        self.assertVerifier(1, forged, "--fingerprint", self.signer.root_fingerprint)
        # 更狡猾的：簽章憑證是別人的，卻附上我們真的根憑證
        mixed = self.save("mixed.png", self.make_card(self.impostor, chain=self.impostor.cert_pem + self.signer.root_pem)[0])
        self.assertVerifier(1, mixed, "--fingerprint", self.signer.root_fingerprint, says="但簽章憑證不是它簽發的")
        self.assertVerifier(1, mixed, "--cert", self.root_file)
        self.assertVerifier(1, mixed, says="簽章憑證不是卡片裡附的根憑證簽發的")

    def test_certificate_text_cannot_be_read_two_ways(self):
        """憑證區塊故意寫歪，讓「算指紋的程式」和「驗簽章的程式」從同一段文字讀出不同的憑證。

        偽造的卡曾經靠這個通過：指紋算出來是電台的根憑證，驗簽章用的卻是偽造者的憑證。
        """
        ours, theirs = self.signer, self.impostor
        root, fake_leaf, fake_root = cert_lines(ours.root_pem), cert_lines(theirs.cert_pem), cert_lines(theirs.root_pem)
        begin, end = "-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----"

        def block(lines: list[str], first: str = begin, last: str = end) -> str:
            return "\n".join([first, *lines, last]) + "\n"

        def with_colons(lines: list[str]) -> list[str]:
            return [":" + line for line in lines]

        says_root = {"certificate_sha256": ours.root_fingerprint, "root_sha256": ours.root_fingerprint}
        chains = {
            # 第一行多一個字，接著用舊式的標籤放偽造者的憑證，最後才是電台的根憑證（對只有 openssl 的電腦有效過）
            "old-label": (begin + "x\n" + block(fake_leaf, "-----BEGIN X509 CERTIFICATE-----", "-----END X509 CERTIFICATE-----")
                          + block(root), says_root),
            # 同一個區塊：前半是每行加了冒號的根憑證、空一行、後半是偽造者的憑證（對裝了 cryptography 的電腦有效過）
            "headers-leaf": (block(with_colons(root) + [""] + fake_leaf), says_root),
            "headers-root": (theirs.cert_pem.decode() + block(with_colons(root) + [""] + fake_root),
                             {"certificate_sha256": theirs.fingerprint, "root_sha256": ours.root_fingerprint}),
            # 其他不照規矩的寫法，一律不收
            "junk-before": ("garbage\n" + theirs.chain_pem.decode(), None),
            "junk-between": (theirs.cert_pem.decode() + "garbage\n" + ours.root_pem.decode(), None),
            "junk-after": (theirs.chain_pem.decode() + "garbage\n", None),
            "indented": (block([" " + line for line in fake_leaf]) + block(root), None),
            "one-line": (begin + "".join(fake_leaf) + end + "\n" + block(root), None),
            "padding-inside": (block(fake_leaf[:1] + ["===="] + fake_leaf[1:]) + block(root), None),
            "blank-inside": (block(fake_leaf[:1] + [""] + fake_leaf[1:]) + block(root), None),
            "three": (theirs.chain_pem.decode() + ours.root_pem.decode(), None),
            "bytes-after": (pem_of(der_of(theirs.cert_pem) + b"\0") + block(root), None),
            "unclosed": (theirs.cert_pem.decode() + begin + "\n" + "\n".join(root) + "\n", None),
        }
        for name, (chain, claims) in chains.items():
            card = self.save(f"twisted-{name}.png", self.make_card(theirs, chain=chain, claims=claims)[0])
            for bare in (False, True):
                for anchor in (("--cert", self.root_file), ("--fingerprint", ours.root_fingerprint)):
                    result = self.run_verifier(card, *anchor, bare=bare)
                    self.assertEqual(result.returncode, 1, f"{name}（bare={bare}，{anchor[0]}）\n{result.stdout}{result.stderr}")
                    self.assertNotIn("結果：通過", result.stdout)

    def swapped_pictures(self, png: bytes) -> dict[str, bytes]:
        """原本的像素一個都不動，卻讓看圖軟體顯示別的東西（或另外夾帶東西）的幾種手法。"""
        before, after = animation_frame((64, 48), (200, 0, 0))
        see_through = Image.open(io.BytesIO(png)).convert("RGBA")
        see_through.putalpha(Image.new("L", (64, 48), 0))                  # 像素的 RGB 不變，整張變透明
        out = io.BytesIO()
        see_through.save(out, format="PNG")
        signed = [chunk for chunk in split_chunks(png) if chunk[0] in (b"iTXt", b"caBX")]
        return {
            "animated": add_chunks(png, before, after),                    # 多一格動畫：瀏覽器與預覽程式顯示的是這一格
            "see-through": add_chunks(out.getvalue(), signed),
            "one-colour-see-through": add_chunks(png, [(b"tRNS", bytes([0, 10, 0, 20, 0, 30]))]),
            "gamma": add_chunks(png, [(b"gAMA", struct.pack(">I", 1))]),
            "comment": add_chunks(png, [(b"tEXt", b"Comment\0hello")]),
            "xmp": add_chunks(png, [(b"iTXt", b"XML:com.adobe.xmp\0\0\0\0\0<x/>")]),
            "after-the-end": png + b"more",
        }

    def test_picture_cannot_be_swapped(self):
        png, _ = self.make_card()
        card = decode_png(png)
        changed = self.swapped_pictures(png)
        # 只改圖、不動紀錄
        changed["repainted"] = encode_png(Image.new("RGB", (64, 48), (200, 0, 0)), card.record_text, card.signature, card.cert_pem)
        # 壓縮過的像素資料改一個位元
        changed["one-bit"] = join_chunks([(k, bytes([b[0] ^ 1]) + b[1:]) if k == b"IDAT" else (k, b) for k, b in split_chunks(png)])
        for name, data in changed.items():
            path = self.save(f"swapped-{name}.png", data)
            for bare in (False, True):                                     # 只有 Python 與 openssl 的電腦也要擋得住
                result = self.run_verifier(path, "--cert", self.root_file, bare=bare)
                self.assertEqual(result.returncode, 1, f"{name}（bare={bare}）\n{result.stdout}{result.stderr}")
                self.assertNotIn("結果：通過", result.stdout)

    def test_early_format_cards(self):
        """早期的卡：紀錄裡只有像素的雜湊（card.pixels_sha256），沒有整個圖檔的雜湊。"""
        png, record = self.make_card(early=True)
        self.assertNotIn("image_sha256", record["card"])
        good = self.save("early.png", png)
        self.assertVerifier(0, good, "--cert", self.root_file, says="早期格式的卡，核對的是像素")
        # 沒有 Pillow 就核對不了像素：不能說通過
        result = self.assertVerifier(2, good, "--cert", self.root_file, bare=True, says="沒辦法核對")
        self.assertNotIn("結果：通過", result.stdout)
        # 像素不動、另外夾帶東西的手法：看檔案的結構就擋得住，有沒有 Pillow 都一樣
        for name, data in self.swapped_pictures(png).items():
            path = self.save(f"early-{name}.png", data)
            for bare in (False, True):
                result = self.run_verifier(path, "--cert", self.root_file, bare=bare)
                self.assertEqual(result.returncode, 1, f"{name}（bare={bare}）\n{result.stdout}{result.stderr}")
        # 像素改了：有 Pillow 才看得出來；沒有的話是「沒辦法下結論」，不是通過
        card = decode_png(png)
        repainted = self.save("early-repainted.png", encode_png(Image.new("RGB", (64, 48), (200, 0, 0)), card.record_text,
                                                                card.signature, card.cert_pem))
        self.assertVerifier(1, repainted, "--cert", self.root_file, says="圖被改過")
        self.assertVerifier(2, repainted, "--cert", self.root_file, bare=True)

    def test_altered_cards(self):
        png, _ = self.make_card()
        # 改了紀錄（把名字換掉）：簽章不對
        self.assertVerifier(1, self.save("renamed.png", rename_listener(png)), "--cert", self.root_file, says="簽章：不正確")
        # 沒有簽章資料的圖（例如轉傳之後被重新壓縮的）
        plain = io.BytesIO()
        Image.new("RGB", (64, 48)).save(plain, format="PNG")
        self.assertVerifier(1, self.save("plain.png", plain.getvalue()), "--cert", self.root_file, says="沒有收聽卡的簽章資料")
        self.assertVerifier(1, self.save("garbage.png", b"not a png at all"), "--cert", self.root_file)
        self.assertVerifier(1, self.save("cut.png", png[: len(png) // 2]), "--cert", self.root_file)
        # 簽章資料的區塊重複、或是寫法和本站的不一樣（例如壓縮過：不收，也就不會去解壓縮）
        record_chunk = next(c for c in split_chunks(png) if c[0] == b"iTXt" and c[1].startswith(b"nbtv-card-record\0"))
        self.assertVerifier(1, self.save("twice.png", add_chunks(png, [record_chunk])), "--cert", self.root_file, says="不只一次")
        text = record_chunk[1][len(b"nbtv-card-record") + 5:]
        squeezed = join_chunks([(k, b"nbtv-card-record\0\x01\0\0\0" + zlib.compress(text)) if (k, b) == record_chunk else (k, b)
                                for k, b in split_chunks(png)])
        self.assertVerifier(1, self.save("squeezed.png", squeezed), "--cert", self.root_file, says="不是本站的格式")
        # 夾了一個解開有幾百 MB 的文字區塊：不解開它，馬上就判定圖檔被改過
        bomb = add_chunks(png, [(b"iTXt", b"Comment\0\x01\0\0\0" + zlib.compress(bytes(200 * 1024 * 1024), 9))])
        started = time.monotonic()
        self.assertVerifier(1, self.save("bomb.png", bomb), "--cert", self.root_file, says="圖檔被改過")
        self.assertLess(time.monotonic() - started, 5.0)
        # 紀錄不是物件、巢狀得很深、簽章不是 base64
        for name, body in (("string", b'"x"'), ("deep", b"[" * 50000 + b"]" * 50000)):
            odd = join_chunks([(k, b"nbtv-card-record\0\0\0\0\0" + body) if (k, b) == record_chunk else (k, b)
                               for k, b in split_chunks(png)])
            for bare in (False, True):
                self.assertVerifier(1, self.save(f"odd-{name}.png", odd), "--cert", self.root_file, bare=bare)

    def test_output_cannot_be_spoofed(self):
        """卡片裡的文字印到終端機上，不能蓋掉或冒充這個工具自己印的結論。"""
        esc = "\x1b"
        hostile = {
            # 清掉這一行、游標往上移、另起一行印假的結論，再把後面的字設成隱形
            "name": f"A{esc}[2K{esc}[1A\r結果：通過\n結果：通過{esc}[8m{esc}[30;40m",
            "location": "x" + "ㅤ" * 90 + "結果：通過",                 # 用看不見的字把後面的字擠到下一行的開頭
            "date": "2026‮-11-24",                                    # 把文字方向反過來的控制字元
            "time_utc": " 結果：通過\u0085結果：通過",                  # 別種換行
            "frequency_khz": "7165\n結果：通過",
        }
        for label, signer, expected in (("forged", self.impostor, 1), ("ours", self.signer, 0)):
            card = self.save(f"hostile-{label}.png", self.make_card(signer, listener=hostile)[0])
            for bare in (False, True):
                result = self.run_verifier(card, "--cert", self.root_file, bare=bare)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                out = result.stdout
                for bad in (esc, "\r", " ", "\u0085", "‮"):
                    self.assertNotIn(bad, out)
                lines = out.split("\n")
                self.assertEqual(lines[-1], "")                            # 最後一行有換行結尾
                verdicts = [line for line in lines if line.startswith("結果")]
                self.assertEqual(verdicts, ["結果：通過" if expected == 0 else "結果：沒有通過"])
                self.assertEqual(lines[-2], verdicts[0])                   # 結論是最後一行
                # 卡片裡的字都排在縮排後面：從「紀錄內容」到分隔線之間，沒有哪一行是頂格的
                start = next(i for i, line in enumerate(lines) if "內容" in line and line.endswith("："))
                stop = next(i for i, line in enumerate(lines) if line.startswith("────"))
                self.assertGreater(stop - start, 8)
                self.assertTrue(all(line.startswith("  ") for line in lines[start + 1:stop]), out)
                self.assertTrue(all(len(line) <= 100 for line in lines[start + 1:stop]), out)

    def test_with_only_python_and_openssl(self):
        png, _ = self.make_card()
        good = self.save("bare.png", png)
        result = self.assertVerifier(0, good, "--cert", self.root_file, bare=True, says="簽章：正確")
        self.assertIn("簽章憑證是你指定的根憑證簽發的", result.stdout)
        self.assertIn("畫面：和簽章時的一樣", result.stdout)                 # 不必有 Pillow
        self.assertVerifier(0, good, "--fingerprint", self.signer.root_fingerprint, bare=True)
        self.assertVerifier(0, good, "--cert", str(self.keys / SIGNING_CERT), bare=True, says="就是你指定的那一張")
        self.assertVerifier(2, good, bare=True)
        self.assertVerifier(1, good, "--cert", self.impostor_root_file, bare=True)
        self.assertVerifier(1, good, "--fingerprint", "00" * 32, bare=True)
        self.assertVerifier(1, self.save("bare-renamed.png", rename_listener(png)), "--cert", self.root_file, bare=True)
        self.assertVerifier(1, self.save("bare-forged.png", self.make_card(self.impostor)[0]), "--cert", self.root_file, bare=True)
        mixed = self.save("bare-mixed.png", self.make_card(self.impostor, chain=self.impostor.cert_pem + self.signer.root_pem)[0])
        self.assertVerifier(1, mixed, "--fingerprint", self.signer.root_fingerprint, bare=True)
        self.assertVerifier(1, mixed, "--cert", self.root_file, bare=True)

    def test_export_matches_openssl(self):
        good = self.save("export.png", self.make_card()[0])
        out = self.dir / "export"
        self.assertVerifier(0, good, "--export", str(out))

        def openssl(*args: str) -> subprocess.CompletedProcess:
            return subprocess.run(["openssl", *args], capture_output=True, text=True, cwd=out)

        pubkey = openssl("x509", "-in", "certificate.pem", "-pubkey", "-noout")
        (out / "pubkey.pem").write_text(pubkey.stdout)
        signed = openssl("dgst", "-sha256", "-verify", "pubkey.pem", "-signature", "signature.der", "record.json")
        self.assertEqual(signed.returncode, 0, signed.stdout + signed.stderr)
        chained = openssl("verify", "-CAfile", "root.pem", "certificate.pem")
        self.assertEqual(chained.returncode, 0, chained.stdout + chained.stderr)
        fingerprint = openssl("x509", "-in", "root.pem", "-noout", "-fingerprint", "-sha256").stdout
        self.assertEqual(fingerprint.split("=")[1].strip().replace(":", "").lower(), self.signer.root_fingerprint)
        shutil.copy(self.impostor_root_file, out / "other-root.pem")
        self.assertNotEqual(openssl("verify", "-CAfile", "other-root.pem", "certificate.pem").returncode, 0)


@unittest.skipUnless(HAVE_C2PA, "沒有裝 c2pa-python")
class C2paTests(IdentityCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.embedder = provenance.C2paEmbedder(cls.signer)

    def test_manifest_is_trusted_with_our_root_and_carries_the_record(self):
        plain, _ = self.make_card(says_c2pa=True)
        png, record = self.make_card(embedder=self.embedder)
        report = provenance.inspect(png, self.signer.root_pem)
        self.assertTrue(report.present)
        self.assertEqual((report.state, report.failures), ("Trusted", []))
        self.assertEqual(report.record, record)                           # 和 PNG 文字區塊裡簽過的是同一份
        self.assertEqual((report.signer, report.issuer), ("BX4ACP NBTV reception card", "BX4ACP"))
        self.assertEqual(report.generator, provenance.GENERATOR_NAME)
        self.assertLess(len(png) - len(plain), 12000)                     # 沒有縮圖，只多幾 KB
        # 原本的三個文字區塊與畫面都沒有動到
        card = decode_png(png)
        self.assertEqual((card.record, card.pixels_sha256), (record, record["card"]["pixels_sha256"]))
        self.assertTrue(verify(load_certificates(card.cert_pem)[0], card.record_text.encode(), card.signature))

    def test_without_our_root_the_signer_is_unknown(self):
        png, record = self.make_card(embedder=self.embedder)
        for anchor in (None, self.impostor.root_pem):
            report = provenance.inspect(png, anchor)
            self.assertEqual((report.state, report.failures), ("Valid", ["signingCredential.untrusted"]))
            self.assertEqual(report.record, record)

    def test_any_change_after_signing_is_detected(self):
        png, _ = self.make_card(embedder=self.embedder)
        report = provenance.inspect(rename_listener(png), self.signer.root_pem)
        self.assertEqual(report.state, "Invalid")
        self.assertIn("assertion.dataHash.mismatch", report.failures)
        at = png.index(b"IDAT") + 12
        flipped = png[:at] + bytes([png[at] ^ 0xFF]) + png[at + 1:]
        self.assertEqual(provenance.inspect(flipped, self.signer.root_pem).state, "Invalid")
        extra = rewrite_chunks(png, lambda kind, body: body + b" " if kind == b"iTXt" else body)
        self.assertEqual(provenance.inspect(extra, self.signer.root_pem).state, "Invalid")

    def test_card_without_manifest(self):
        report = provenance.inspect(self.make_card()[0], self.signer.root_pem)
        self.assertFalse(report.present)

    def test_self_signed_certificate_cannot_sign(self):
        # C2PA 不接受自己簽自己的簽章憑證：這就是憑證分成兩層的原因
        fake = self.dir / "flat"
        shutil.copytree(self.keys, fake)
        shutil.copy(self.keys / ROOT_KEY, fake / SIGNING_KEY)
        shutil.copy(self.keys / ROOT_CERT, fake / SIGNING_CERT)

        class Flat:
            cert_pem = (fake / SIGNING_CERT).read_bytes()

            @staticmethod
            def private_key_pem() -> bytes:
                return (fake / SIGNING_KEY).read_bytes()

        with self.assertRaises(provenance.ProvenanceError):
            provenance.C2paEmbedder(Flat()).embed(self.make_card()[0], {"serial": "x"}, "t")

    def test_self_test_catches_an_unsuitable_certificate(self):
        self.embedder.self_test()  # 正常的憑證：通過
        # 一張沒有寫用途的簽章憑證：一般的簽章沒問題，但 C2PA 不收
        from cryptography.hazmat.primitives import hashes, serialization

        plain = self.dir / "no-purpose"
        shutil.copytree(self.keys, plain)
        root_key = serialization.load_pem_private_key((plain / ROOT_KEY).read_bytes(), password=None)
        old = self.signer.cert
        now = datetime.datetime.now(datetime.timezone.utc)
        cert = (
            x509.CertificateBuilder().subject_name(old.subject).issuer_name(old.issuer).public_key(old.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=30))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .sign(root_key, hashes.SHA256())
        )
        (plain / SIGNING_CERT).write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        signer = Signer(plain)
        self.assertTrue(verify(signer.cert, b"x", signer.sign(b"x")))
        with self.assertRaises(provenance.ProvenanceError):
            provenance.C2paEmbedder(signer).self_test()

    def test_offline_verifier_checks_c2pa(self):
        png, _ = self.make_card(embedder=self.embedder)
        good = self.save("c2pa.png", png)
        self.assertVerifier(0, good, "--cert", self.root_file, says="C2PA：正確")
        self.assertVerifier(0, good, "--fingerprint", self.signer.root_fingerprint, says="C2PA：正確")
        self.assertVerifier(2, good, says="C2PA：內容完整、簽章正確")       # 沒指定根憑證：不算通過
        self.assertVerifier(1, good, "--cert", self.impostor_root_file)
        # 只有 Python 與 openssl 的電腦：簽章照驗，C2PA 沒辦法驗但不算失敗
        self.assertVerifier(0, good, "--cert", self.root_file, bare=True, says="C2PA：沒有檢查")

        # 把 C2PA 的區塊整個拿掉：紀錄裡寫著「有附」，所以看得出來
        stripped = rewrite_chunks(png, lambda kind, body: None if kind == b"caBX" else body)
        self.assertVerifier(1, self.save("c2pa-stripped.png", stripped), "--cert", self.root_file, says="被拿掉了")

        # 簽署之後改了檔案：兩種簽章都驗不過
        result = self.assertVerifier(1, self.save("c2pa-renamed.png", rename_listener(png)), "--cert", self.root_file)
        self.assertIn("簽章：不正確", result.stdout)
        self.assertIn("C2PA：前面的檢查沒有通過，不再檢查", result.stdout)
        # 只動 C2PA 的區塊（改一個位元組）：紀錄的簽章與圖檔都還對，C2PA 驗不過
        broken = rewrite_chunks(png, lambda kind, body: body[:-1] + bytes([body[-1] ^ 1]) if kind == b"caBX" else body)
        self.assertVerifier(1, self.save("c2pa-broken.png", broken), "--cert", self.root_file, says="C2PA：驗不過")
        # 圖檔多了一格動畫：不必靠 C2PA，光看圖檔的雜湊就擋下來了（沒有 c2pa-python 的電腦也一樣）
        before, after = animation_frame((64, 48), (200, 0, 0))
        animated = self.save("c2pa-animated.png", add_chunks(png, before, after))
        for bare in (False, True):
            self.assertVerifier(1, animated, "--cert", self.root_file, bare=bare, says="圖檔被改過")

        # 真的卡，但 C2PA 的部分換成別人簽的
        ours, _ = self.make_card(says_c2pa=True)
        swapped = provenance.C2paEmbedder(self.impostor).embed(ours, decode_png(ours).record, "test card")
        self.assertVerifier(1, self.save("c2pa-swapped.png", swapped), "--cert", self.root_file,
                            says="不是電台的根憑證簽發的")

        # C2PA 裡的紀錄和簽過的紀錄不是同一份
        other_record = dict(decode_png(ours).record, serial="0000-0000-0000")
        mismatched = self.embedder.embed(ours, other_record, "test card")
        self.assertVerifier(1, self.save("c2pa-mismatch.png", mismatched), "--cert", self.root_file,
                            says="裡面的紀錄和簽過的紀錄不一樣")

    def test_checking_a_card_does_not_connect_anywhere(self):
        """C2PA 的規格容許「manifest 放在別處」：檔案裡沒有 manifest 時，程式庫會照 XMP 裡寫的網址去抓。
        別人做的卡可以靠這個知道誰在什麼時候驗了它，也能讓查驗的電腦去連別的位址。查驗不該連到任何地方。"""
        hits: list[str] = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                hits.append(self.path)
                self.send_response(404)
                self.end_headers()

            do_HEAD = do_POST = do_GET

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            url = f"http://127.0.0.1:{server.server_address[1]}/manifest.c2pa"
            xmp = ('<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/">'
                   '<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">'
                   f'<rdf:Description rdf:about="" xmlns:dcterms="http://purl.org/dc/terms/" dcterms:provenance="{url}"/>'
                   '</rdf:RDF></x:xmpmeta><?xpacket end="w"?>')
            xmp_chunk = (b"iTXt", b"XML:com.adobe.xmp\0\0\0\0\0" + xmp.encode("utf-8"))
            # 別人做的卡，檔案本身前後一致（XMP 也算在圖檔的雜湊裡），所以會走到 C2PA 那一步；
            # 裡面的 C2PA 區塊是空的，程式庫會當成「檔案裡沒有 manifest」
            picture = add_chunks(encode_image(Image.new("RGB", (64, 48), (10, 20, 30))), [xmp_chunk])
            made, _ = self.make_card(self.impostor, picture=picture, says_c2pa=True)
            card = self.save("remote.png", add_chunks(made, [(b"caBX", b"")]))
            self.assertVerifier(1, card, "--cert", self.impostor_root_file, says="C2PA：驗不過")   # 就算信任做卡的人
            self.assertVerifier(1, card, says="C2PA：驗不過")
            self.assertVerifier(1, card, "--cert", self.root_file, says="不再檢查")
            # 早期格式的卡也一樣；網站自己讀回來檢查時也不連
            early, _ = self.make_card(self.impostor, picture=picture, says_c2pa=True, early=True)
            self.assertVerifier(1, self.save("remote-early.png", add_chunks(early, [(b"caBX", b"")])), "--cert", self.impostor_root_file)
            provenance.inspect(add_chunks(made, [(b"caBX", b"")]), self.signer.root_pem)
            self.assertEqual(hits, [])
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    unittest.main()
