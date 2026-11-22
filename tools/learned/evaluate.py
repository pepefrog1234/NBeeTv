# 訓練好的模型在測試小圖與專案的三張測試圖上，各個格點訊雜比下的 PSNR。
# 用法：python3 evaluate.py run/model.pt val.npy 測試圖.ppm …
import sys
import numpy as np
import torch
from model import JSCC, psnr_uint8

ckpt = torch.load(sys.argv[1], map_location="cpu")
model = JSCC(ckpt["latent"]); model.load_state_dict(ckpt["model"]); model.eval()
dev = torch.device("mps" if torch.backends.mps.is_available() else "cpu"); model.to(dev)
def ppm(path):
    d = open(path, "rb").read().split(b"\n", 3)
    return np.frombuffer(d[3], np.uint8).reshape(72, 96, 3)
sets = {"測試小圖": np.load(sys.argv[2])}
if len(sys.argv) > 3: sets["專案的三張測試圖"] = np.stack([ppm(p) for p in sys.argv[3:]])
snrs = (0, 5, 10, 15, 20, 25, 30, 99)
print(f"模型：{ckpt['latent'] * 108} 個實數、訓練 {ckpt['step']} 步")
print("格點訊雜比（dB）            " + "".join(f"{('無雜訊' if s >= 90 else s):>7}" for s in snrs))
for name, arr in sets.items():
    x_all = torch.from_numpy(arr).permute(0, 3, 1, 2).float().div(255.0)
    row = []
    g = torch.Generator().manual_seed(11)
    with torch.no_grad():
        for snr in snrs:
            vals = []
            for i in range(0, len(x_all), 256):
                x = x_all[i:i + 256].to(dev)
                noise = torch.randn((len(x), ckpt["latent"], 9, 12), generator=g).to(dev)
                if snr >= 90: noise = torch.zeros_like(noise)
                y = model(x, torch.full((len(x),), float(min(snr, 30)), device=dev), noise)
                vals.append(psnr_uint8(y, x).cpu())
            row.append(float(torch.cat(vals).mean()))
    print(f"  {name}（{len(arr)} 張）".ljust(26) + "".join(f"{v:7.1f}" for v in row))
