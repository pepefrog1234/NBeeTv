# 模式 15（給 VHF／UHF 的 FM 語音通道用的大畫面）的訓練資料：把一個資料夾裡的照片切成 192×144 的小圖。
# 用法：python3 prep_hi.py 照片資料夾 輸出資料夾 [保留幾張照片當驗證（預設 400）] [每張訓練照片切幾張（預設 16）]
#
#  - 照片的排序、打散的亂數、保留當驗證的那 400 張，都和 prep.py 相同，所以驗證用的照片和 data/val.npy 是同一批，
#    切的範圍也一樣（每張照片兩張：整張、其中一部分），只是輸出的解析度加倍：val_192.npy。
#    另外照同樣的範圍存一份 96×72 的 val_96.npy（和 prep.py 的 val.npy 相同，用來核對），以及每張小圖在原照片上的寬度
#    val_boxw.npy（比 192 像素窄的，192×144 那一份是放大出來的，比清晰度時要排除）。
#  - 訓練用的小圖：每張照片第一張是整張（置中的最大 4:3 範圍），其餘取畫面的三成到全部；範圍不小於 192×144，不放大。
#    輸出 train_192.npy。COCO val2017 的 5000 張照片切出 73,600 張（6.1 GB）。
import os
import random
import sys
from multiprocessing.pool import ThreadPool as Pool

import numpy as np
from PIL import Image

W, H = 192, 144
OLD_W, OLD_H = 96, 72


def val_crops(args):
    """和 prep.py 的 crops(path, 2, seed) 取同樣的兩個範圍。回傳（192×144 兩張、96×72 兩張、兩個範圍的寬度）。"""
    path, seed = args
    rnd = random.Random(seed)
    try:
        im = Image.open(path).convert("RGB")
    except Exception:
        return None
    w, h = im.size
    bw = min(w, h * 4 / 3)
    bh = bw * 3 / 4
    if bw < OLD_W * 1.5:
        return None
    hi, lo, widths = [], [], []
    for k in range(2):
        s = 1.0 if k == 0 else rnd.uniform(0.3, 1.0)
        cw, ch = bw * s, bh * s
        if cw < OLD_W:
            cw, ch = OLD_W, OLD_H
        left = (w - cw) / 2 if k == 0 else rnd.uniform(0, w - cw)
        top = (h - ch) / 2 if k == 0 else rnd.uniform(0, h - ch)
        box = (left, top, left + cw, top + ch)
        hi.append(np.asarray(im.resize((W, H), Image.LANCZOS, box=box, reducing_gap=3.0), np.uint8))
        lo.append(np.asarray(im.resize((OLD_W, OLD_H), Image.LANCZOS, box=box, reducing_gap=3.0), np.uint8))
        widths.append(cw)
    return np.stack(hi), np.stack(lo), np.array(widths, np.float32)


def train_crops(args):
    path, count, seed = args
    rnd = random.Random(seed)
    try:
        im = Image.open(path).convert("RGB")
    except Exception:
        return np.zeros((0, H, W, 3), np.uint8)
    w, h = im.size
    bw = min(w, h * 4 / 3)
    bh = bw * 3 / 4
    if bw < W:
        return np.zeros((0, H, W, 3), np.uint8)
    out = []
    for k in range(count):
        s = 1.0 if k == 0 else rnd.uniform(0.3, 1.0)
        cw, ch = max(bw * s, W), max(bh * s, H)
        left = (w - cw) / 2 if k == 0 else rnd.uniform(0, w - cw)
        top = (h - ch) / 2 if k == 0 else rnd.uniform(0, h - ch)
        out.append(np.asarray(im.resize((W, H), Image.LANCZOS, box=(left, top, left + cw, top + ch), reducing_gap=3.0), np.uint8))
    return np.stack(out)


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    held = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    per_photo = int(sys.argv[4]) if len(sys.argv) > 4 else 16
    os.makedirs(dst, exist_ok=True)
    files = sorted(os.path.join(src, f) for f in os.listdir(src) if f.lower().endswith((".jpg", ".jpeg", ".png")))
    random.Random(1).shuffle(files)
    test_files, train_files = files[:held], files[held:]
    with Pool(12) as pool:
        te = [r for r in pool.map(val_crops, [(f, 10_000_000 + i) for i, f in enumerate(test_files)], chunksize=16) if r is not None]
        tr = pool.map(train_crops, [(f, per_photo, 20_000_000 + i) for i, f in enumerate(train_files)], chunksize=16)
    val_hi = np.concatenate([r[0] for r in te])
    val_lo = np.concatenate([r[1] for r in te])
    widths = np.concatenate([r[2] for r in te])
    train = np.concatenate(tr)
    np.random.default_rng(1).shuffle(train)
    np.save(os.path.join(dst, "train_192.npy"), train)
    np.save(os.path.join(dst, "val_192.npy"), val_hi)
    np.save(os.path.join(dst, "val_96.npy"), val_lo)
    np.save(os.path.join(dst, "val_boxw.npy"), widths)
    print(f"照片 {len(files)} 張：訓練用 {len(train_files)} 張 → {len(train)} 張 192×144 的小圖；驗證用 {len(test_files)} 張 → {len(val_hi)} 張"
          f"（其中 {int((widths < W).sum())} 張的範圍比 192 像素窄，是放大的）")
