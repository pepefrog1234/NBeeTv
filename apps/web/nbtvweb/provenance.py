"""C2PA（Content Credentials）：把一份 manifest 嵌進收聽卡的 PNG，以及讀回來檢查。

C2PA 是記錄「這個檔案是誰、用什麼做出來的」的業界標準。manifest 放在 PNG 的 caBX 區塊裡，
內容是：誰產生的（claim generator）、怎麼產生的（c2pa.created 動作）、我們自己的一則聲明
（收聽卡的紀錄，和 PNG 文字區塊裡簽過的那一份相同），再由簽章憑證簽起來。
它還記了整個檔案（manifest 自己除外）的雜湊，所以嵌進去之後檔案改一個位元組都驗不過——
C2PA 必須是最後一步。

簽章憑證是自己的根憑證簽發的，不在 C2PA 的信任清單上：別人的工具驗得出「內容完整、簽章正確」，
但會把簽署者列為不認得。把本台的根憑證設成信任錨點之後才會是「受信任」。

沒有向時間戳記服務（TSA）要時間戳記，所以簽章憑證到期之後，C2PA 的工具會判定簽章過期。

用的是官方的 c2pa-python。這個模組在真的用到時才載入它，沒有裝也不影響網站的其他部分。
"""

from __future__ import annotations

import io
import json
import threading
from dataclasses import dataclass, field

from . import __version__
from .signing import Signer

# 我們自己的聲明在 manifest 裡的標籤
ASSERTION_LABEL = "tw.hf-nbtv.reception-card"
GENERATOR_NAME = "hf-nbtv reception card site"
PNG = "image/png"


class ProvenanceError(Exception):
    pass


def _library():
    try:
        import c2pa
    except ImportError:
        raise ProvenanceError(
            "沒有裝 c2pa-python（pip install -r requirements.txt）；不要 C2PA 的話把設定檔的 [c2pa] enabled 設成 false"
        ) from None
    return c2pa


class C2paEmbedder:
    def __init__(self, signer: Signer):
        self._c2pa = _library()
        self._signer = signer
        self._lock = threading.Lock()  # 一次只簽一張；簽一張只要幾毫秒

    def embed(self, png: bytes, record: dict, title: str) -> bytes:
        """把 manifest 嵌進 png，回傳新的檔案內容。png 裡的其他東西（像素、文字區塊）不動。"""
        c2pa = self._c2pa
        manifest = {
            "claim_generator_info": [{"name": GENERATOR_NAME, "version": __version__}],
            "title": title,
            "assertions": [{"label": ASSERTION_LABEL, "data": record}],
        }
        info = c2pa.C2paSignerInfo(
            alg=c2pa.C2paSigningAlg.ES256,
            sign_cert=self._signer.cert_pem,  # 憑證鏈從簽章憑證開始，不含根憑證
            private_key=self._signer.private_key_pem(),
            ta_url=None,
        )
        # 縮圖用不到，關掉之後 manifest 只有幾 KB
        settings = c2pa.Settings.from_dict({"builder": {"thumbnail": {"enabled": False}}})
        with self._lock:
            try:
                with c2pa.Context(settings) as context, c2pa.Signer.from_info(info) as signer, \
                        c2pa.Builder(manifest, context) as builder:
                    # 卡片是程式把照片、文字與解出來的畫面合成的
                    builder.set_intent(c2pa.C2paBuilderIntent.CREATE, c2pa.C2paDigitalSourceType.COMPOSITE_SYNTHETIC)
                    out = io.BytesIO()
                    builder.sign(signer, PNG, io.BytesIO(png), out)
                    return out.getvalue()
            except c2pa.C2paError as e:
                raise ProvenanceError(f"C2PA 簽章失敗：{e}") from None


    def self_test(self) -> None:
        """用現在的憑證實際簽一張小圖再驗回來。

        憑證不合 C2PA 的要求時（例如用途不對、是自己簽自己的），網站一啟動就會發現，不必等到發第一張卡。
        """
        from PIL import Image

        blank = io.BytesIO()
        Image.new("RGB", (8, 8)).save(blank, format="PNG")
        report = inspect(self.embed(blank.getvalue(), {"self_test": True}, "self test"), self._signer.root_pem)
        if report.state != "Trusted":
            raise ProvenanceError(f"用現在的憑證簽出來的 C2PA 資料驗不過（{report.state}：{'、'.join(report.failures)}）")


@dataclass
class C2paReport:
    present: bool                    # 檔案裡有沒有 C2PA 的資料
    state: str | None = None         # Trusted：內容完整而且簽署者受信任；Valid：內容完整但簽署者不認得；Invalid：驗不過
    failures: list[str] = field(default_factory=list)   # 沒通過的檢查（C2PA 的狀態代碼）
    record: dict | None = None       # manifest 裡我們那則聲明的內容
    signer: str | None = None        # 簽章憑證的名稱
    issuer: str | None = None        # 簽發者（憑證的 O 欄位）
    generator: str | None = None


def inspect(png: bytes, root_pem: bytes | None = None) -> C2paReport:
    """讀出並驗證 png 裡的 C2PA 資料。root_pem 是要當成信任錨點的根憑證；不給就不信任任何簽署者。"""
    c2pa = _library()
    # 不照檔案裡寫的網址去抓「放在別處的 manifest」，也不連憑證的撤銷查詢：驗一個檔案不該讓這台電腦連到任何地方
    options: dict = {"verify": {"remote_manifest_fetch": False, "ocsp_fetch": False}}
    if root_pem:
        options["trust"] = {"user_anchors": root_pem.decode("ascii")}
    settings = c2pa.Settings.from_dict(options)
    try:
        with c2pa.Context(settings) as context:
            reader = c2pa.Reader.try_create(PNG, io.BytesIO(png), context=context)
            if reader is None:
                return C2paReport(present=False)
            with reader:
                store = json.loads(reader.json())
                state = reader.get_validation_state()
    except c2pa.C2paError as e:
        return C2paReport(present=True, state="Invalid", failures=[str(e)])
    active = (store.get("manifests") or {}).get(store.get("active_manifest") or "", {})
    results = (store.get("validation_results") or {}).get("activeManifest") or {}
    failures = [str(entry.get("code")) for entry in results.get("failure") or []]
    record = next((a.get("data") for a in active.get("assertions") or [] if a.get("label") == ASSERTION_LABEL), None)
    signature = active.get("signature_info") or {}
    generators = active.get("claim_generator_info") or [{}]
    return C2paReport(
        present=True, state=state, failures=failures, record=record if isinstance(record, dict) else None,
        signer=signature.get("common_name"), issuer=signature.get("issuer"), generator=generators[0].get("name"),
    )
