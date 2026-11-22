# 把一個資料夾裡的照片切成 96×72 的小圖，存成訓練與測試用的陣列。
# 發射的畫面是整個場景縮小後的樣子，所以大部分的小圖取自涵蓋畫面三成以上的範圍；另外取少數較緊的局部增加變化。
# 測試用的小圖來自另外保留的照片，訓練時完全沒看過。
# 用法：python3 prep.py 照片資料夾 輸出資料夾 [保留幾張照片當測試（預設 400）]
import os, sys, random
from multiprocessing.pool import ThreadPool as Pool
import numpy as np
from PIL import Image

W, H = 96, 72

def crops(args):
    path, count, seed = args
    rnd = random.Random(seed)
    try:
        im = Image.open(path).convert("RGB")
    except Exception:
        return np.zeros((0, H, W, 3), np.uint8)
    w, h = im.size
    bw = min(w, h * 4 / 3); bh = bw * 3 / 4                      # 能放進去的最大 4:3 範圍
    if bw < W * 1.5: return np.zeros((0, H, W, 3), np.uint8)
    out = []
    for k in range(count):
        if k == 0: s = 1.0
        elif k % 6 == 5: s = rnd.uniform(0.12, 0.3)
        else: s = rnd.uniform(0.3, 1.0)
        cw, ch = bw * s, bh * s
        if cw < W: cw, ch = W, H
        left = (w - cw) / 2 if k == 0 else rnd.uniform(0, w - cw)
        top = (h - ch) / 2 if k == 0 else rnd.uniform(0, h - ch)
        patch = im.resize((W, H), Image.LANCZOS, box=(left, top, left + cw, top + ch), reducing_gap=3.0)
        out.append(np.asarray(patch, np.uint8))
    return np.stack(out)

if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    held = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    os.makedirs(dst, exist_ok=True)
    files = sorted(os.path.join(src, f) for f in os.listdir(src) if f.lower().endswith((".jpg", ".jpeg", ".png")))
    random.Random(1).shuffle(files)
    test_files, train_files = files[:held], files[held:]
    with Pool(12) as pool:
        tr = pool.map(crops, [(f, 30, i) for i, f in enumerate(train_files)], chunksize=16)
        te = pool.map(crops, [(f, 2, 10_000_000 + i) for i, f in enumerate(test_files)], chunksize=16)
    train = np.concatenate(tr); test = np.concatenate(te)
    np.random.default_rng(1).shuffle(train)
    np.save(os.path.join(dst, "train.npy"), train)
    np.save(os.path.join(dst, "val.npy"), test)
    test.tofile(os.path.join(dst, "val.raw"))
    print(f"照片 {len(files)} 張：訓練用 {len(train_files)} 張 → {len(train)} 張小圖；測試用 {len(test_files)} 張 → {len(test)} 張小圖")
