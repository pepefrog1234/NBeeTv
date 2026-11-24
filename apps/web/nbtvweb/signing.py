"""簽章用的金鑰與自己簽發的憑證，以及簽名、驗證。

憑證有兩層，都是自己簽發的，沒有經過任何憑證機構：

    根憑證（root）      自己簽自己的 CA。對外公布的就是它（或它的指紋），信不信一張卡看的是它。
      └ 簽章憑證        根憑證簽發的。網站用它的私鑰簽每一張卡：PNG 裡的紀錄，以及 C2PA 的資料。

分成兩層是因為 C2PA 不接受自己簽自己的簽章憑證；另一個好處是根憑證的私鑰平常用不到，
產生之後可以移到別的地方保管，網站被入侵時外流的只會是簽章憑證的私鑰。

任何人都做得出一張寫著同樣名字的自簽根憑證，所以要比對的是指紋，不是名字。
金鑰都是 ECDSA P-256。
"""

from __future__ import annotations

import datetime
import os
from pathlib import Path

from cryptography import x509
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

ALGORITHM = "ECDSA-P256-SHA256"

ROOT_KEY = "root-key.pem"
ROOT_CERT = "root-cert.pem"
SIGNING_KEY = "signing-key.pem"
SIGNING_CERT = "signing-cert.pem"

# 簽章憑證的用途。C2PA 要求簽章憑證至少有一個它認得的用途：這裡放「文件簽章」（RFC 9336）
# 與 C2PA 自己定義的「claim 簽章」
EKU_DOCUMENT_SIGNING = x509.ObjectIdentifier("1.3.6.1.5.5.7.3.36")
EKU_C2PA_CLAIM_SIGNING = x509.ObjectIdentifier("1.3.6.1.4.1.62558.2.1")


class SigningError(Exception):
    pass


def _write_private_key(path: Path, key: ec.EllipticCurvePrivateKey) -> None:
    # 私鑰只有擁有者讀得到；先以 0600 建檔再寫入，而且不覆蓋已經有的檔案
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))


def _key_usage(*, sign: bool, ca: bool) -> x509.KeyUsage:
    return x509.KeyUsage(
        digital_signature=sign, content_commitment=False, key_encipherment=False, data_encipherment=False,
        key_agreement=False, key_cert_sign=ca, crl_sign=ca, encipher_only=False, decipher_only=False,
    )


def issue_signing_certificate(
    root_key: ec.EllipticCurvePrivateKey, root_cert: x509.Certificate, public_key: ec.EllipticCurvePublicKey,
    callsign: str, years: int,
) -> x509.Certificate:
    """用根憑證簽發一張簽章憑證。"""
    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    return (
        x509.CertificateBuilder()
        .subject_name(x509.Name([
            x509.NameAttribute(NameOID.COMMON_NAME, f"{callsign} NBTV reception card"),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, callsign),
        ]))
        .issuer_name(root_cert.subject)
        .public_key(public_key)
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(min(now + datetime.timedelta(days=365 * years), root_cert.not_valid_after_utc))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(_key_usage(sign=True, ca=False), critical=True)
        .add_extension(x509.ExtendedKeyUsage([EKU_DOCUMENT_SIGNING, EKU_C2PA_CLAIM_SIGNING]), critical=False)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(public_key), critical=False)
        .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(root_cert.public_key()), critical=False)
        .sign(root_key, hashes.SHA256())
    )


def create_identity(keys_dir: Path, callsign: str, root_years: int = 20, signing_years: int = 10) -> tuple[x509.Certificate, x509.Certificate]:
    """產生根憑證與簽章憑證（各一把金鑰），回傳（根憑證, 簽章憑證）。

    已經有的話不會覆蓋：換了根憑證，以前發的卡就驗不過了。
    """
    paths = [keys_dir / name for name in (ROOT_KEY, ROOT_CERT, SIGNING_KEY, SIGNING_CERT)]
    if any(p.exists() for p in paths):
        raise SigningError(f"{keys_dir} 裡已經有金鑰或憑證，不覆蓋")

    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    root_key = ec.generate_private_key(ec.SECP256R1())
    root_name = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, f"{callsign} NBTV root CA"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, callsign),
    ])
    root_cert = (
        x509.CertificateBuilder()
        .subject_name(root_name)
        .issuer_name(root_name)
        .public_key(root_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=365 * root_years))
        # 只能直接簽發簽章憑證，底下不能再有別的 CA
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .add_extension(_key_usage(sign=False, ca=True), critical=True)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(root_key.public_key()), critical=False)
        .sign(root_key, hashes.SHA256())
    )
    signing_key = ec.generate_private_key(ec.SECP256R1())
    signing_cert = issue_signing_certificate(root_key, root_cert, signing_key.public_key(), callsign, signing_years)

    keys_dir.mkdir(parents=True, exist_ok=True)
    os.chmod(keys_dir, 0o700)
    _write_private_key(keys_dir / ROOT_KEY, root_key)
    _write_private_key(keys_dir / SIGNING_KEY, signing_key)
    (keys_dir / ROOT_CERT).write_bytes(root_cert.public_bytes(serialization.Encoding.PEM))
    (keys_dir / SIGNING_CERT).write_bytes(signing_cert.public_bytes(serialization.Encoding.PEM))
    return root_cert, signing_cert


def certificate_fingerprint(cert: x509.Certificate) -> str:
    """憑證的 SHA-256 指紋（小寫十六進位），和 openssl x509 -fingerprint -sha256 算的一樣。"""
    return cert.fingerprint(hashes.SHA256()).hex()


def load_certificate(pem: bytes) -> x509.Certificate:
    """讀 PEM 裡的第一張憑證。"""
    try:
        return x509.load_pem_x509_certificate(pem)
    except ValueError as e:
        raise SigningError(f"憑證讀不了：{e}") from None


def load_certificates(pem: bytes) -> list[x509.Certificate]:
    """讀 PEM 裡所有的憑證（收聽卡裡附的是簽章憑證接著根憑證）。"""
    try:
        return x509.load_pem_x509_certificates(pem)
    except ValueError as e:
        raise SigningError(f"憑證讀不了：{e}") from None


def issued_by(cert: x509.Certificate, issuer: x509.Certificate) -> bool:
    """cert 是不是 issuer 簽發的（名稱對得上，而且簽章正確）。"""
    try:
        cert.verify_directly_issued_by(issuer)
        return True
    except (ValueError, TypeError, InvalidSignature):
        return False


def verify(cert: x509.Certificate, data: bytes, signature: bytes) -> bool:
    """用憑證裡的公鑰驗一段資料的簽章。"""
    public_key = cert.public_key()
    if not isinstance(public_key, ec.EllipticCurvePublicKey):
        return False
    try:
        public_key.verify(signature, data, ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


class Signer:
    """持有簽章憑證私鑰的物件。只有發卡的那一步會用到它。根憑證的私鑰它用不到，也不讀。"""

    def __init__(self, keys_dir: Path):
        try:
            self._key_pem = (keys_dir / SIGNING_KEY).read_bytes()
            self.cert_pem = (keys_dir / SIGNING_CERT).read_bytes()
            self.root_pem = (keys_dir / ROOT_CERT).read_bytes()
        except FileNotFoundError:
            raise SigningError("還沒有簽章用的金鑰與憑證，先執行 python -m nbtvweb init-keys") from None
        try:
            key = serialization.load_pem_private_key(self._key_pem, password=None)
        except (ValueError, TypeError) as e:
            raise SigningError(f"簽章金鑰讀不了：{e}") from None
        if not isinstance(key, ec.EllipticCurvePrivateKey):
            raise SigningError("簽章金鑰要是 ECDSA 的")
        self._key = key
        self.cert = load_certificate(self.cert_pem)
        self.root = load_certificate(self.root_pem)
        public_key = self.cert.public_key()
        if not isinstance(public_key, ec.EllipticCurvePublicKey) or public_key.public_numbers() != key.public_key().public_numbers():
            raise SigningError("簽章憑證和私鑰不是一對")
        if not issued_by(self.cert, self.root):
            raise SigningError("簽章憑證不是這張根憑證簽發的")
        self.fingerprint = certificate_fingerprint(self.cert)
        self.root_fingerprint = certificate_fingerprint(self.root)
        # 放進收聽卡的憑證：簽章憑證在前，根憑證在後
        self.chain_pem = self.cert_pem.rstrip(b"\n") + b"\n" + self.root_pem.rstrip(b"\n") + b"\n"
        self.key_file_exposed = bool((keys_dir / SIGNING_KEY).stat().st_mode & 0o077)  # 別的使用者讀得到私鑰
        self.root_key_present = (keys_dir / ROOT_KEY).exists()

    def sign(self, data: bytes) -> bytes:
        """簽一段資料（ECDSA／SHA-256，DER 格式）。"""
        return self._key.sign(data, ec.ECDSA(hashes.SHA256()))

    def private_key_pem(self) -> bytes:
        """簽章憑證的私鑰（PEM）。只給 C2PA 的函式庫簽 claim 用。"""
        return self._key_pem
