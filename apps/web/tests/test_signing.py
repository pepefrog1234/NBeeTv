"""憑證（根憑證＋簽章憑證）、紀錄的簽章、卡片檔、離線查驗工具，以及 C2PA。"""

from __future__ import annotations

import datetime
import io
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

from cryptography import x509
from PIL import Image

from nbtvweb import provenance
from nbtvweb.cardfile import canonical_json, decode_png, encode_png, pixels_sha256
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

    def make_card(self, signer: Signer | None = None, *, chain: bytes | None = None, says_c2pa: bool = False,
                  embedder=None) -> tuple[bytes, dict]:
        signer = signer or self.signer
        image = Image.new("RGB", (64, 48), (10, 20, 30))
        record = {
            "v": 1, "serial": "7KQ2-M9XD-4TFA", "station": "BX4ACP", "listener": {"name": "山田", "frequency_khz": 7165.0},
            "issued_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "card": {"pixels_sha256": pixels_sha256(image), "c2pa": says_c2pa or embedder is not None},
            "signature": {"certificate_sha256": signer.fingerprint, "root_sha256": signer.root_fingerprint},
        }
        text = canonical_json(record)
        png = encode_png(image, text, signer.sign(text.encode()), chain or signer.chain_pem)
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
        self.assertVerifier(0, good, "--cert", self.root_file, says="簽章憑證是你指定的根憑證簽發的")
        self.assertVerifier(0, good, "--cert", str(self.keys / SIGNING_CERT), says="就是你指定的那一張")
        self.assertVerifier(0, good, "--fingerprint", self.signer.root_fingerprint.upper(), says="根憑證指紋和你給的相同")
        spaced = " ".join(self.signer.root_fingerprint[i:i + 4] for i in range(0, 64, 4))
        self.assertVerifier(0, good, "--fingerprint", spaced)             # 照查驗頁上的寫法，四個字一組
        self.assertVerifier(0, good, "--fingerprint", self.signer.fingerprint)
        result = self.assertVerifier(0, good, says="只代表檔案前後一致")   # 沒說要信任誰：過，但講清楚
        self.assertIn(self.signer.root_fingerprint, result.stdout)
        self.assertIn("C2PA：這張卡沒有附 C2PA 的資料", result.stdout)

    def test_wrong_trust_anchor(self):
        good = self.save("good2.png", self.make_card()[0])
        self.assertVerifier(1, good, "--fingerprint", "00" * 32, says="指紋不符")
        self.assertVerifier(1, good, "--cert", self.impostor_root_file, says="不是")
        self.assertVerifier(1, good, "--cert", str(self.dir / "no-such-file.pem"))

    def test_card_made_by_someone_else(self):
        # 別人用自己做的憑證（名字和我們的一模一樣）發的卡
        forged = self.save("forged.png", self.make_card(self.impostor)[0])
        self.assertVerifier(0, forged)                                     # 檔案本身前後一致
        self.assertVerifier(1, forged, "--cert", self.root_file)
        self.assertVerifier(1, forged, "--fingerprint", self.signer.root_fingerprint)
        # 更狡猾的：簽章憑證是別人的，卻附上我們真的根憑證
        mixed = self.save("mixed.png", self.make_card(self.impostor, chain=self.impostor.cert_pem + self.signer.root_pem)[0])
        self.assertVerifier(1, mixed, "--fingerprint", self.signer.root_fingerprint, says="但簽章憑證不是它簽發的")
        self.assertVerifier(1, mixed, "--cert", self.root_file)
        self.assertVerifier(1, mixed, says="簽章憑證不是卡片裡附的根憑證簽發的")

    def test_altered_cards(self):
        png, _ = self.make_card()
        # 改了紀錄（把名字換掉）：簽章不對
        self.assertVerifier(1, self.save("renamed.png", rename_listener(png)), "--cert", self.root_file, says="簽章：不正確")
        # 只改圖、不動紀錄：畫面的雜湊對不上
        card = decode_png(png)
        repainted = encode_png(Image.new("RGB", (64, 48), (200, 0, 0)), card.record_text, card.signature, card.cert_pem)
        self.assertVerifier(1, self.save("repainted.png", repainted), "--cert", self.root_file, says="圖被改過")
        # 沒有簽章資料的圖（例如轉傳之後被重新壓縮的）
        plain = io.BytesIO()
        Image.new("RGB", (64, 48)).save(plain, format="PNG")
        self.assertVerifier(1, self.save("plain.png", plain.getvalue()), says="沒有收聽卡的簽章資料")
        self.assertVerifier(1, self.save("garbage.png", b"not a png at all"))
        self.assertVerifier(1, self.save("cut.png", png[: len(png) // 2]))

    def test_with_only_python_and_openssl(self):
        png, _ = self.make_card()
        good = self.save("bare.png", png)
        result = self.assertVerifier(0, good, "--cert", self.root_file, bare=True, says="簽章：正確")
        self.assertIn("簽章憑證是你指定的根憑證簽發的", result.stdout)
        self.assertIn("畫面：沒有檢查", result.stdout)
        self.assertVerifier(0, good, "--fingerprint", self.signer.root_fingerprint, bare=True)
        self.assertVerifier(1, good, "--cert", self.impostor_root_file, bare=True)
        self.assertVerifier(1, self.save("bare-renamed.png", rename_listener(png)), "--cert", self.root_file, bare=True)
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
        self.assertVerifier(0, good, says="C2PA：內容完整、簽章正確")
        self.assertVerifier(1, good, "--cert", self.impostor_root_file)
        # 只有 Python 與 openssl 的電腦：簽章照驗，C2PA 沒辦法驗但不算失敗
        self.assertVerifier(0, good, "--cert", self.root_file, bare=True, says="C2PA：沒有檢查")

        # 把 C2PA 的區塊整個拿掉：紀錄裡寫著「有附」，所以看得出來
        stripped = rewrite_chunks(png, lambda kind, body: None if kind == b"caBX" else body)
        self.assertVerifier(1, self.save("c2pa-stripped.png", stripped), "--cert", self.root_file, says="被拿掉了")

        # 簽署之後改了檔案：兩種簽章都驗不過
        result = self.assertVerifier(1, self.save("c2pa-renamed.png", rename_listener(png)), "--cert", self.root_file)
        self.assertIn("簽章：不正確", result.stdout)
        self.assertIn("C2PA：驗不過", result.stdout)

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


if __name__ == "__main__":
    unittest.main()
