"""命令列：python -m nbtvweb serve | init-keys | check"""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path

from .config import Config, ConfigError, load_config
from .config import warnings as config_warnings

DEFAULT_CONFIG = Path(__file__).resolve().parent.parent / "config.toml"


def cmd_init_keys(cfg: Config) -> int:
    from .signing import ROOT_CERT, ROOT_KEY, SIGNING_CERT, SIGNING_KEY, SigningError, certificate_fingerprint, create_identity

    try:
        root, signing = create_identity(cfg.keys_dir, cfg.callsign)
    except SigningError as e:
        print(e, file=sys.stderr)
        return 1
    print(f"產生在 {cfg.keys_dir}：")
    print(f"  {ROOT_CERT}     根憑證（公開的，網站會放在 /root.pem）。有效到 {root.not_valid_after_utc:%Y-%m-%d}")
    print(f"  {ROOT_KEY}      根憑證的私鑰。網站用不到它，只有以後要簽發新的簽章憑證時才需要：")
    print("                    建議現在就移到別的地方保管（例如隨身碟），不要留在這台電腦上，也不能弄丟")
    print(f"  {SIGNING_CERT}  簽章憑證（根憑證簽發的）。有效到 {signing.not_valid_after_utc:%Y-%m-%d}")
    print(f"  {SIGNING_KEY}   簽章憑證的私鑰。網站每發一張卡都要用，不能外流，要備份")
    print(f"根憑證指紋 SHA-256：{certificate_fingerprint(root)}")
    print("  對外公布的是這個指紋（查驗頁上會顯示）；別人驗收聽卡時比對的就是它")
    return 0


def cmd_check(cfg: Config) -> int:
    """把會用到的東西都找一遍，印出找到什麼。"""
    from .app import check_decoder
    from .card import find_fonts, list_backgrounds
    from .provenance import C2paEmbedder, ProvenanceError
    from .signing import Signer, SigningError

    ok = True
    print(f"電台呼號：{cfg.callsign}；發卡條件：有畫面的幀超過 {cfg.min_frames} 幀，並解出呼號；錄音最長 {cfg.max_seconds:.0f} 秒")
    print(f"對外網址：{cfg.public_url}；監聽 {cfg.host}:{cfg.port}")
    for warning in config_warnings(cfg):
        print(f"  注意：{warning}")
    try:
        check_decoder(cfg)
        print(f"解碼器：{cfg.nbtv_rx}")
    except ConfigError as e:
        ok = False
        print(f"解碼器：{e}")
    signer = None
    try:
        signer = Signer(cfg.keys_dir)
        print(f"根憑證：指紋 {signer.root_fingerprint}，有效到 {signer.root.not_valid_after_utc:%Y-%m-%d}")
        print(f"簽章憑證：指紋 {signer.fingerprint}，有效到 {signer.cert.not_valid_after_utc:%Y-%m-%d}（都在 {cfg.keys_dir}）")
        if signer.key_file_exposed:
            print("  注意：簽章憑證的私鑰別的使用者也讀得到")
        if signer.root_key_present:
            print("  根憑證的私鑰還放在這裡：網站用不到它，可以移到別的地方保管")
    except SigningError as e:
        ok = False
        print(f"憑證：{e}")
    if not cfg.c2pa_enabled:
        print("C2PA：沒有啟用（卡片裡不放 C2PA 的資料）")
    elif signer is not None:
        try:
            import c2pa

            C2paEmbedder(signer).self_test()
            print(f"C2PA：啟用，c2pa-python {c2pa.__version__}；用現在的憑證試簽、試驗都通過")
        except ProvenanceError as e:
            ok = False
            print(f"C2PA：{e}")
    try:
        fonts = find_fonts(cfg.font_cjk, cfg.font_latin, cfg.font_mono)
        print(f"字型：中文 {Path(fonts.regular.path).name}、數字 {Path(fonts.display.path).name}、序號 {Path(fonts.mono.path).name}")
    except RuntimeError as e:
        ok = False
        print(f"字型：{e}")
    photos = list_backgrounds(cfg.backgrounds)
    print(f"卡片底圖：{cfg.backgrounds} 裡有 {len(photos)} 張" + ("" if photos else "（沒有照片時用內建的底圖）"))
    if not cfg.turnstile_enabled:
        print("Turnstile：沒有啟用（只適合本機試用）")
    elif cfg.turnstile_uses_test_keys:
        print("Turnstile：用的是 Cloudflare 的測試金鑰，驗證一律通過（公開之前要換成自己的）")
    else:
        print("Turnstile：已啟用")
    return 0 if ok else 1


def cmd_serve(cfg: Config) -> int:
    import uvicorn

    from .app import create_app, server_config
    from .signing import SigningError

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    try:
        app = create_app(cfg)
    except (ConfigError, SigningError, RuntimeError) as e:
        print(e, file=sys.stderr)
        return 1
    # 一個行程就好：工作的狀態放在記憶體裡
    try:
        uvicorn.Server(server_config(app, cfg)).run()
    except KeyboardInterrupt:
        pass  # 按 Ctrl-C 停下來是正常的結束方式，不必印出一長串錯誤
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m nbtvweb", description="NBTV 收聽卡網站")
    parser.add_argument("command", choices=["serve", "init-keys", "check"],
                        help="serve：啟動網站；init-keys：產生根憑證、簽章憑證與它們的私鑰；check：檢查設定")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG), help="設定檔（預設是 apps/web/config.toml）")
    args = parser.parse_args(argv)
    try:
        cfg = load_config(args.config)
    except ConfigError as e:
        print(e, file=sys.stderr)
        return 1
    return {"serve": cmd_serve, "init-keys": cmd_init_keys, "check": cmd_check}[args.command](cfg)


if __name__ == "__main__":
    sys.exit(main())
