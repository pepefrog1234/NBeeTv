# 訓練好的模型在各種通道下的 PSNR：白雜訊、瑞利衰落（每個值各自衰落），以及隨機少掉一部分的值。
# 模型可以是檢查點（.pt）或權重檔（.bin）；影像可以是 .npy（[N,72,96,3]）或幾張 96×72 的 PPM。
# 用法：python3 evaluate.py 模型 data/val.npy [測試圖.ppm …]
import sys

import numpy as np
import torch

from export import read_ppm, read_weights
from model import DEFAULT_WIDTHS, JSCC, psnr_uint8

path = sys.argv[1]
widths = DEFAULT_WIDTHS
if path.endswith(".bin"):
    state, latent, model_id, architecture = read_weights(path)
    label = f"權重檔 {model_id:08X}"
else:
    ckpt = torch.load(path, map_location="cpu")
    state, latent, architecture = ckpt["model"], ckpt["latent"], ckpt.get("architecture", 1)
    widths = tuple(ckpt.get("widths", DEFAULT_WIDTHS))
    label = f"檢查點，訓練 {ckpt['step']} 步"
    if widths != DEFAULT_WIDTHS:
        label += "，各層寬度 " + "／".join(str(w) for w in widths)
model = JSCC(latent, architecture, widths)
model.load_state_dict(state)
dev = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
model.to(dev).eval()

sets = {}
ppms = [a for a in sys.argv[2:] if a.endswith(".ppm")]
for a in sys.argv[2:]:
    if a.endswith(".npy"):
        sets[a.split("/")[-1]] = np.load(a)
if ppms:
    sets[f"{len(ppms)} 張 PPM"] = np.stack([read_ppm(p) for p in ppms])

SNRS = (-6, -3, 0, 5, 10, 15, 20, 30, 99)
print(f"模型：{label}；結構 {architecture}；{latent * 108} 個實數；解碼器認得的最低訊雜比 {model.min_snr_db:g} dB")


def measure(x_all, make_noise_var, g):
    """make_noise_var(張數) → 各個值的複數雜訊變異數（CPU）。回傳平均 PSNR。"""
    vals = []
    with torch.no_grad():
        for i in range(0, len(x_all), 200):
            x = x_all[i:i + 200].to(dev)
            z = model.encode(x)
            var = make_noise_var(len(x))
            noise = torch.randn(z.shape, generator=g)
            y = z + (noise * torch.sqrt(var / 2.0)).to(dev)
            vals.append(psnr_uint8(model.decode(y, var.to(dev)), x).cpu())
    return float(torch.cat(vals).mean())


for name, arr in sets.items():
    x_all = torch.from_numpy(arr).permute(0, 3, 1, 2).float().div(255.0).contiguous()
    shape = (latent, 9, 12)
    print(f"\n{name}（{len(arr)} 張）")
    print("平均訊雜比（dB）        " + "".join(f"{('無雜訊' if s >= 90 else s):>7}" for s in SNRS))
    g = torch.Generator().manual_seed(11)
    row = [measure(x_all, lambda c, s=s: torch.full((c,) + shape, 10.0 ** (-min(s, 60) / 10.0)), g) for s in SNRS]
    print("  白雜訊                " + "".join(f"{v:7.1f}" for v in row))
    row = [measure(x_all, lambda c, s=s: 10.0 ** (-min(s, 60) / 10.0) / torch.empty((c,) + shape).exponential_(1.0, generator=g).clamp_min(1e-4), g)
           for s in SNRS]
    print("  瑞利衰落              " + "".join(f"{v:7.1f}" for v in row))
    # 隨機少掉一部分的值（雜訊變異數 100，等於沒收到），其餘的是 10 dB
    fractions = (0.0, 0.1, 0.3, 0.5, 0.7)
    row = [measure(x_all, lambda c, f=f: torch.where(torch.rand((c,) + shape, generator=g) < f, torch.tensor(100.0), torch.tensor(0.1)), g)
           for f in fractions]
    print("  10 dB，少掉的比例     " + "  ".join(f"{int(f * 100)}%:{v:.1f}" for f, v in zip(fractions, row)))
