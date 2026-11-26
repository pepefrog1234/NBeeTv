"""把收聽卡畫出來：底圖（電台主人提供的照片）、解出來的畫面、文字與查驗用的 QR code。

卡片是 1680×1080，照 140×90 mm 的 QSL 卡比例，印出來約 300 dpi。
解出來的畫面一律用最近點放大（整數倍），和 app 裡看到的一樣是一格一格的。
"""

from __future__ import annotations

import glob
import hashlib
import logging
import unicodedata
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path

import numpy as np
import qrcode
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageOps

from .decoder import IMAGE_HEIGHT, IMAGE_WIDTH, picture_size

log = logging.getLogger(__name__)

CARD_WIDTH = 1680
CARD_HEIGHT = 1080

# 顏色照 app 的主題（apps/macos/Sources/Theme.swift）
INK = (232, 236, 240)
SECONDARY = (173, 182, 191)
TERTIARY = (144, 154, 165)
ACCENT = (68, 214, 214)     # 接收用的青色
WELL = (12, 14, 17)

MARGIN = 56
GAP = 32
LEFT_X1 = 1016              # 左邊文字面板的右緣
PICTURE_SCALE = 5

BACKGROUND_SUFFIXES = {".jpg", ".jpeg", ".png", ".webp"}


# ------------------------------------------------------------------ 字型


@dataclass(frozen=True)
class FontFace:
    path: str
    index: int = 0
    variation: str = ""     # 可變字型用哪一個具名的粗細


@dataclass(frozen=True)
class Fonts:
    regular: FontFace       # 中文與一般文字
    medium: FontFace
    bold: FontFace
    display: FontFace       # 呼號與大的數字
    mono: FontFace          # 序號

    def get(self, role: str, size: int) -> ImageFont.FreeTypeFont:
        return _load_font(getattr(self, role), int(size))


@lru_cache(maxsize=256)
def _load_font(face: FontFace, size: int) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(face.path, size, index=face.index)
    if face.variation:
        try:
            font.set_variation_by_name(face.variation)
        except (OSError, ValueError):
            pass
    return font


def _find_face(path: str, family: str, styles: tuple[str, ...]) -> FontFace | None:
    """在字型檔（可能是合集）裡找指定的家族與樣式。"""
    found: dict[str, int] = {}
    for index in range(64):
        try:
            name = ImageFont.truetype(path, 20, index=index).getname()
        except OSError:
            break
        if family in ("", name[0]) and name[1] not in found:
            found[name[1] or ""] = index
    for style in styles:
        if style in found:
            return FontFace(path, found[style])
    return FontFace(path, next(iter(found.values()))) if found else None


def find_fonts(cjk: str = "", latin: str = "", mono: str = "") -> Fonts:
    """找卡片用的字型。三個參數是設定檔指定的字型檔，空字串表示自己找系統裡的。"""
    def first_existing(patterns: list[str]) -> str | None:
        for pattern in patterns:
            matches = sorted(glob.glob(pattern))
            if matches:
                return matches[0]
        return None

    regular = medium = bold = None
    if cjk:
        regular = medium = bold = _find_face(cjk, "", ("Regular", "Medium", "Bold", ""))
    else:
        # macOS 的蘋方在系統資產裡，路徑含一段會變的雜湊，所以用萬用字元找
        pingfang = first_existing([
            "/System/Library/AssetsV2/com_apple_MobileAsset_Font*/*/AssetData/PingFang.ttc",
            "/System/Library/Fonts/PingFang.ttc",
        ])
        if pingfang:
            regular = _find_face(pingfang, "PingFang TC", ("Regular",))
            medium = _find_face(pingfang, "PingFang TC", ("Medium", "Regular"))
            bold = _find_face(pingfang, "PingFang TC", ("Semibold", "Medium", "Regular"))
        if regular is None:
            for pattern, family in [
                ("/System/Library/Fonts/STHeiti Medium.ttc", "Heiti TC"),
                ("/usr/share/fonts/**/NotoSansCJK-Regular.ttc", "Noto Sans CJK TC"),
                ("/usr/share/fonts/**/NotoSansTC-Regular.otf", ""),
                ("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", ""),
            ]:
                matches = sorted(glob.glob(pattern, recursive=True))
                if matches:
                    regular = medium = bold = _find_face(matches[0], family, ("Regular", "Medium", "")) \
                        or _find_face(matches[0], "", ("Regular", "Medium", ""))
                    if regular:
                        break
    if regular is None:
        raise RuntimeError("找不到可以顯示中文的字型；請在設定檔的 [card] font_cjk 指定一個字型檔")
    medium, bold = medium or regular, bold or medium or regular

    display = None
    for path in ([latin] if latin else [
        "/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf",
        "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf",
        "/System/Library/Fonts/HelveticaNeue.ttc",
    ]):
        if Path(path).exists():
            display = _find_face(path, "", ("Bold", "Regular", ""))
            if display:
                break
    mono_face = None
    if mono:
        mono_face = _find_face(mono, "", ("Regular", "Medium", ""))
    elif Path("/System/Library/Fonts/SFNSMono.ttf").exists():
        mono_face = FontFace("/System/Library/Fonts/SFNSMono.ttf", 0, "Medium")
    elif Path("/System/Library/Fonts/Menlo.ttc").exists():
        mono_face = _find_face("/System/Library/Fonts/Menlo.ttc", "", ("Regular",))
    return Fonts(regular=regular, medium=medium, bold=bold, display=display or bold, mono=mono_face or medium)


# ------------------------------------------------------------------ 內容


@dataclass
class CardContent:
    callsign: str
    serial: str
    verify_url: str            # QR code 的內容
    verify_hint: str           # 印在 QR code 旁邊的網址（不含 https://）
    issued_at: str             # 例如 "2026-11-24 11:02 UTC"
    fingerprint: str           # 根憑證的指紋（十六進位）：對外公布、拿來比對的那一個
    c2pa: bool = False         # 檔案裡會不會嵌入 C2PA 的資料
    # 聽眾自己填的；空字串表示沒填
    listener_name: str = ""
    listener_date: str = ""
    listener_time: str = ""
    listener_freq: str = ""
    listener_location: str = ""
    # 解碼器量到的
    modes: str = ""
    frames: int = 0
    snr: str = ""
    picture: bytes = b""       # 正中那一幀的 RGB：96×72，模式 5–7 與 15 是 192×144
    picture_caption: str = ""
    strip: list[bytes] = field(default_factory=list)


def clean_text(text: str, limit: int) -> str:
    """整理使用者填的文字：去掉控制字元與不可見的格式字元、把連續的空白併成一個、截到 limit 個字。"""
    kept = []
    for ch in unicodedata.normalize("NFC", text or ""):
        category = unicodedata.category(ch)
        if category.startswith("Z") or ch in "\t\n\r":
            kept.append(" ")
        elif category[0] != "C":
            kept.append(ch)
    return " ".join("".join(kept).split())[:limit].strip()


def format_frequency(khz: float | None) -> str:
    if khz is None:
        return ""
    if khz >= 100000:
        return f"{khz / 1000:.3f} MHz"
    return f"{khz:.1f}".rstrip("0").rstrip(".") + " kHz"


# ------------------------------------------------------------------ 底圖


def list_backgrounds(directory: Path) -> list[Path]:
    """底圖目錄裡的照片，照檔名排序。"""
    if not directory.is_dir():
        return []
    return sorted(p for p in directory.iterdir()
                  if p.is_file() and p.suffix.lower() in BACKGROUND_SUFFIXES and not p.name.startswith("."))


def pick_background(directory: Path, serial: str) -> Path | None:
    """從底圖目錄裡挑一張：同一個序號每次挑到同一張，不同的序號大致平均分到各張。"""
    files = list_backgrounds(directory)
    if not files:
        return None
    return files[int.from_bytes(hashlib.sha256(serial.encode()).digest()[:8], "big") % len(files)]


def _cover(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    image = ImageOps.exif_transpose(image).convert("RGB")
    return ImageOps.fit(image, size, Image.Resampling.LANCZOS, centering=(0.5, 0.5))


def default_background() -> Image.Image:
    """沒有照片時用的底圖：深色的漸層，加上從右下角散開的幾圈電波。"""
    w, h = CARD_WIDTH, CARD_HEIGHT
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    cx, cy = w * 0.80, h * 0.86
    distance = np.sqrt((x - cx) ** 2 + (y - cy) ** 2) / w
    glow = np.exp(-(distance / 0.42) ** 2)
    top = np.array([9, 20, 30], dtype=np.float32)
    bottom = np.array([18, 21, 26], dtype=np.float32)
    t = (y / h)[..., None]
    base = top * (1 - t) + bottom * t
    base += glow[..., None] * np.array([10, 62, 70], dtype=np.float32)
    image = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), "RGB")

    scale = 2
    rings = Image.new("RGBA", (w * scale, h * scale), (0, 0, 0, 0))
    draw = ImageDraw.Draw(rings)
    for i in range(1, 15):
        r = i * 86 * scale
        alpha = int(max(0.0, 0.34 - i * 0.021) * 255)
        box = [cx * scale - r, cy * scale - r, cx * scale + r, cy * scale + r]
        draw.ellipse(box, outline=(*ACCENT, alpha), width=2 * scale)
    rings = rings.resize((w, h), Image.Resampling.LANCZOS)
    return Image.alpha_composite(image.convert("RGBA"), rings).convert("RGB")


def load_background(path: Path | None) -> Image.Image:
    if path is not None:
        try:
            with Image.open(path) as image:
                return _cover(image, (CARD_WIDTH, CARD_HEIGHT))
        except (OSError, ValueError) as e:
            log.warning("底圖 %s 讀不了（%s），改用內建的底圖", path, e)
    return default_background()


# ------------------------------------------------------------------ 畫圖的小工具


@lru_cache(maxsize=32)
def _rounded_mask(size: tuple[int, int], radius: int) -> Image.Image:
    """圓角矩形的遮罩。先畫四倍大再縮小，邊緣才平滑。"""
    scale = 4
    mask = Image.new("L", (size[0] * scale, size[1] * scale), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size[0] * scale - 1, size[1] * scale - 1], radius * scale, fill=255)
    return mask.resize(size, Image.Resampling.LANCZOS)


def _glass_panel(card: Image.Image, box: tuple[int, int, int, int], radius: int = 28, opacity: float = 0.80) -> None:
    """在底圖上放一塊深色的毛玻璃：後面的照片先模糊再壓暗，文字放上去才清楚。"""
    x0, y0, x1, y1 = box
    size = (x1 - x0, y1 - y0)
    region = card.crop(box).filter(ImageFilter.GaussianBlur(26))
    region = Image.blend(region, Image.new("RGB", size, WELL), opacity)
    mask = _rounded_mask(size, radius)
    card.paste(region, (x0, y0), mask)
    # 一圈很淡的亮邊
    ring = Image.new("L", size, 0)
    inner = _rounded_mask((size[0] - 2, size[1] - 2), radius - 1)
    ring.paste(mask, (0, 0))
    ring.paste(0, (1, 1), inner)
    card.paste(Image.new("RGB", size, (255, 255, 255)), (x0, y0), ring.point(lambda v: v * 30 // 255))


def _tracked(draw: ImageDraw.ImageDraw, xy: tuple[float, float], text: str, font, fill, tracking: float) -> float:
    """畫一行加了字距的文字（基線對齊），回傳右緣的 x。"""
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill, anchor="ls")
        x += font.getlength(ch) + tracking
    return x - tracking


def _fit(fonts: Fonts, role: str, text: str, width: float, size: int, smallest: int) -> tuple[ImageFont.FreeTypeFont, str]:
    """把文字塞進 width：先縮小字，縮到 smallest 還是太長就截斷加上刪節號。"""
    for s in range(size, smallest - 1, -2):
        font = fonts.get(role, s)
        if font.getlength(text) <= width:
            return font, text
    font = fonts.get(role, smallest)
    while len(text) > 1 and font.getlength(text + "…") > width:
        text = text[:-1]
    return font, text + "…"


def _picture(rgb: bytes, scale: int) -> Image.Image:
    """一張畫面畫成 96×72 的 scale 倍。畫面本身比較大（模式 5–7 與 15 的 192×144）時也畫成同樣的大小，版面不必跟著變：
    剛好是整數倍就一格一格放大（不內插），否則重新取樣。"""
    width, height = picture_size(rgb)
    image = Image.frombytes("RGB", (width, height), rgb)
    target = (IMAGE_WIDTH * scale, IMAGE_HEIGHT * scale)
    whole = target[0] % width == 0 and target[1] % height == 0
    return image.resize(target, Image.Resampling.NEAREST if whole else Image.Resampling.LANCZOS)


def qr_image(text: str, size: int) -> Image.Image:
    """QR code，白底黑點，邊長 size。點的大小是整數，四周留白至少三格。"""
    code = qrcode.QRCode(version=None, error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=1, border=0)
    code.add_data(text)
    code.make(fit=True)
    matrix = code.get_matrix()
    n = len(matrix)
    module = max(1, size // (n + 6))
    tile = Image.new("L", (n, n), 255)
    tile.putdata([0 if cell else 255 for row in matrix for cell in row])
    tile = tile.resize((n * module, n * module), Image.Resampling.NEAREST)
    out = Image.new("L", (size, size), 255)
    out.paste(tile, ((size - n * module) // 2, (size - n * module) // 2))
    return out.convert("RGB")


# ------------------------------------------------------------------ 整張卡


def render(content: CardContent, background: Image.Image, fonts: Fonts) -> Image.Image:
    card = background.copy()
    if card.size != (CARD_WIDTH, CARD_HEIGHT):
        card = _cover(card, (CARD_WIDTH, CARD_HEIGHT))

    left = (MARGIN, MARGIN, LEFT_X1, CARD_HEIGHT - MARGIN)
    right_x0, right_x1 = LEFT_X1 + GAP, CARD_WIDTH - MARGIN
    picture_w, picture_h = IMAGE_WIDTH * PICTURE_SCALE, IMAGE_HEIGHT * PICTURE_SCALE
    picture_panel = (right_x0, MARGIN, right_x1, MARGIN + 40 + 26 + 18 + picture_h + 16 + 28 + 22 + IMAGE_HEIGHT + 40)
    qr_panel = (right_x0, picture_panel[3] + GAP, right_x1, CARD_HEIGHT - MARGIN)
    for box in (left, picture_panel, qr_panel):
        _glass_panel(card, box)

    draw = ImageDraw.Draw(card)

    # ---------------- 左邊：文字
    x0, x1 = left[0] + 64, left[2] - 64
    width = x1 - x0
    y = left[1] + 80
    _tracked(draw, (x0, y), "NBTV RECEPTION CARD", fonts.get("display", 27), ACCENT, 3.2)
    draw.text((x1, y), "窄頻電視收聽卡", font=fonts.get("medium", 27), fill=ACCENT, anchor="rs")

    y += 186
    font, text = _fit(fonts, "display", content.callsign, width, 204, 120)
    draw.text((x0 - 6, y), text, font=font, fill=INK, anchor="ls")

    y += 62
    draw.text((x0, y), "確認收到本台的窄頻電視（NBTV）訊號", font=fonts.get("medium", 33), fill=INK, anchor="ls")
    y += 38
    draw.text((x0, y), "Confirming reception of our narrow-band television (NBTV) signal",
              font=fonts.get("regular", 22), fill=SECONDARY, anchor="ls")

    y += 34
    card.paste(Image.new("RGB", (width, 1), (58, 66, 76)), (x0, y))

    def label(x: float, y: float, zh: str, en: str, self_reported: bool = False) -> None:
        f_zh, f_en = fonts.get("regular", 19), fonts.get("display", 17)
        draw.text((x, y), zh, font=f_zh, fill=SECONDARY, anchor="ls")
        end = _tracked(draw, (x + f_zh.getlength(zh) + 10, y), en, f_en, TERTIARY, 1.6)
        if self_reported:
            draw.text((end + 5, y - 1), "＊", font=fonts.get("regular", 15), fill=ACCENT, anchor="ls")

    y += 46
    label(x0, y, "收聽者", "LISTENER", True)
    y += 66
    font, text = _fit(fonts, "bold", content.listener_name or "聽友  Listener", width, 56, 30)
    draw.text((x0, y), text, font=font, fill=INK, anchor="ls")
    y += 42
    if content.listener_location:  # 收聽地點寫在名字底下，像地址那樣
        font, text = _fit(fonts, "regular", content.listener_location, width, 26, 18)
        draw.text((x0, y), text, font=font, fill=SECONDARY, anchor="ls")

    def value(x: float, y: float, text: str, unit: str = "", cell: float = 0.0) -> None:
        if not text:
            draw.text((x, y), "—", font=fonts.get("regular", 40), fill=TERTIARY, anchor="ls")
            return
        unit_font = fonts.get("medium", 22)
        room = cell - (unit_font.getlength(unit) + 8 if unit else 0)
        font, text = _fit(fonts, "display", text, room, 46, 28)
        draw.text((x, y), text, font=font, fill=INK, anchor="ls")
        if unit:
            draw.text((x + font.getlength(text) + 8, y), unit, font=unit_font, fill=SECONDARY, anchor="ls")

    column = width / 3
    cell = column - 24

    def split_unit(text: str) -> tuple[str, str]:
        head, _, tail = text.partition(" ")
        return head, tail

    y += 58
    for i, (zh, en) in enumerate([("日期", "DATE"), ("時間", "UTC"), ("頻率", "FREQUENCY")]):
        label(x0 + i * column, y, zh, en, True)
    y += 56
    value(x0, y, content.listener_date, cell=cell)
    value(x0 + column, y, content.listener_time, cell=cell)
    value(x0 + 2 * column, y, *split_unit(content.listener_freq), cell=cell)

    y += 58
    for i, (zh, en) in enumerate([("模式", "MODE"), ("收到幀數", "FRAMES"), ("訊雜比", "SNR")]):
        label(x0 + i * column, y, zh, en)
    y += 56
    value(x0, y, content.modes, cell=cell)
    value(x0 + column, y, str(content.frames), cell=cell)
    value(x0 + 2 * column, y, *split_unit(content.snr), cell=cell)

    y = left[3] - 132
    card.paste(Image.new("RGB", (width, 1), (58, 66, 76)), (x0, y))
    small, small_latin = fonts.get("regular", 17), fonts.get("mono", 16)
    y += 36
    end = x0 + draw.textlength("序號 No. ", font=small)
    draw.text((x0, y), "序號 No. ", font=small, fill=TERTIARY, anchor="ls")
    draw.text((end, y), content.serial, font=small_latin, fill=SECONDARY, anchor="ls")
    draw.text((x1, y), f"發行 Issued {content.issued_at}", font=small, fill=TERTIARY, anchor="rs")
    y += 28
    draw.text((x0, y), "＊", font=fonts.get("regular", 15), fill=ACCENT, anchor="ls")
    draw.text((x0 + 20, y), "聽眾自填，本台沒有查核  Reported by the listener, not verified by the station",
              font=small, fill=TERTIARY, anchor="ls")
    y += 28
    fingerprint = " ".join(content.fingerprint[i:i + 4] for i in range(0, 16, 4))
    signed = "數位簽章 Digitally signed · ECDSA P-256" + (" · C2PA" if content.c2pa else "")
    font, text = _fit(fonts, "regular", f"{signed} · 根憑證指紋 Root SHA-256 {fingerprint} …", width, 17, 13)
    draw.text((x0, y), text, font=font, fill=TERTIARY, anchor="ls")

    # ---------------- 右上：收到的畫面
    px = picture_panel[0] + (picture_panel[2] - picture_panel[0] - picture_w) // 2
    y = picture_panel[1] + 40 + 20
    draw.text((px, y), "收到的畫面", font=fonts.get("regular", 19), fill=SECONDARY, anchor="ls")
    _tracked(draw, (px + fonts.get("regular", 19).getlength("收到的畫面") + 10, y), "RECEIVED PICTURE",
             fonts.get("display", 17), TERTIARY, 1.6)
    y += 6 + 18
    if content.picture:
        card.paste(Image.new("RGB", (picture_w + 4, picture_h + 4), (70, 78, 88)), (px - 2, y - 2))
        card.paste(_picture(content.picture, PICTURE_SCALE), (px, y))
    y += picture_h + 16 + 22
    font, text = _fit(fonts, "regular", content.picture_caption, picture_w, 19, 14)
    draw.text((px, y), text, font=font, fill=SECONDARY, anchor="ls")
    y += 6 + 22
    # 其他幾幀排成一條底片：原始大小，不縮放
    for i, rgb in enumerate(content.strip[: picture_w // IMAGE_WIDTH]):
        card.paste(_picture(rgb, 1), (px + i * IMAGE_WIDTH, y))
    if content.strip:
        count = min(len(content.strip), picture_w // IMAGE_WIDTH)
        for i in range(1, count):
            card.paste(Image.new("RGB", (2, IMAGE_HEIGHT), WELL), (px + i * IMAGE_WIDTH - 1, y))

    # ---------------- 右下：查驗
    qr_size = qr_panel[3] - qr_panel[1] - 80
    qx, qy = qr_panel[0] + 40, qr_panel[1] + 40
    code = qr_image(content.verify_url, qr_size)
    card.paste(code, (qx, qy), _rounded_mask((qr_size, qr_size), 14))
    tx = qx + qr_size + 32
    room = qr_panel[2] - 40 - tx
    y = qy + 34
    draw.text((tx, y), "線上查驗", font=fonts.get("bold", 30), fill=INK, anchor="ls")
    y += 30
    _tracked(draw, (tx, y), "VERIFY ONLINE", fonts.get("display", 17), ACCENT, 1.6)
    y += 52
    font, text = _fit(fonts, "mono", content.serial, room, 25, 16)
    draw.text((tx, y), text, font=font, fill=INK, anchor="ls")
    y += 40
    for line in ("掃描 QR code，或到", content.verify_hint, "輸入序號"):
        font, text = _fit(fonts, "regular", line, room, 18, 12)
        draw.text((tx, y), text, font=font, fill=SECONDARY if line != content.verify_hint else INK, anchor="ls")
        y += 27
    return card


def contact_sheet(pictures: list[bytes], columns: int = 7, limit: int = 154) -> Image.Image | None:
    """把解出來的畫面排成一張總覽（原始大小）。太多幀時平均抽 limit 幀。"""
    if not pictures:
        return None
    if len(pictures) > limit:
        step = (len(pictures) - 1) / (limit - 1)
        pictures = [pictures[round(i * step)] for i in range(limit)]
    columns = min(columns, len(pictures))
    rows = (len(pictures) + columns - 1) // columns
    gap = 2
    sheet = Image.new("RGB", (columns * IMAGE_WIDTH + (columns - 1) * gap, rows * IMAGE_HEIGHT + (rows - 1) * gap), WELL)
    for i, rgb in enumerate(pictures):
        sheet.paste(_picture(rgb, 1), ((i % columns) * (IMAGE_WIDTH + gap), (i // columns) * (IMAGE_HEIGHT + gap)))
    return sheet
