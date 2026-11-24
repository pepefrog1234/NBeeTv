# 量「接收端把最近幾幀合起來解」在真實畫面上的效果。不經過數據機（經過數據機的量法：nbtv-sim --merge）。
# 畫面的來源是原始的 rgb24 檔（96×72，一張接一張），例如：
#   ffmpeg -i 影片 -vf "crop=…,fps=6,scale=96:72:flags=area" -f rawvideo -pix_fmt rgb24 frames.rgb
# 每 --step 張取一張當作相鄰的幀（每秒 6 張的檔案：模式 14 給 4，模式 12 給 6，模式 11、13 給 16）。
# 每張畫面各自編碼、各自收到一次（固定的雜訊），和實際的單張模式一樣；差別只在接收端：
#   base   現在的做法：只用這一幀收到的值
#   fuse   用訓練出來的閘，把前面幾幀收到的值合進來
#   all    不判斷、一律合併（對照用，看閘有沒有必要）
# 數字是螢幕上那一張和當時的原圖比的 PSNR（dB）。
# 分組：全部；同一個鏡頭內（這一幀和前面幾幀之間沒有換鏡頭）；剛換鏡頭；同一個鏡頭內動作最大的兩成、最小的兩成。
# --loss 給掉幀率時另外算一次：隨機掉幀，沒收到的那一幀螢幕停在上一張，合併時把它當成沒有。
import argparse

import numpy as np
import torch

from export import read_weights
from merge import fuse, load_gate, pick_device
from model import JSCC, psnr_uint8, widths_of

p = argparse.ArgumentParser()
p.add_argument("videos", nargs="+")                        # 名字=路徑
p.add_argument("--model", required=True)
p.add_argument("--gate", required=True)
p.add_argument("--step", type=int, default=4)              # 每隔幾張取一張當作相鄰的幀
p.add_argument("--snr", default="0,5,10,20,99")
p.add_argument("--fade", default="yes")
p.add_argument("--loss", type=float, default=0.0)          # 掉幀率（0 表示不算）
p.add_argument("--device", default="")
p.add_argument("--csv", default="")
args = p.parse_args()

H, W = 72, 96
dev = pick_device(args.device)
state, C, _, arch = read_weights(args.model)
model = JSCC(C, arch, widths_of(state))
model.load_state_dict(state)
model = model.to(dev).eval()
gate, info = load_gate(args.gate, dev)
K = info["frames"]
snrs = [float(v) for v in args.snr.split(",")]


def decode(y, var):
    out = []
    with torch.no_grad():
        for i in range(0, len(y), 128):
            out.append(model.decode(y[i:i + 128].to(dev), var[i:i + 128].to(dev)).cpu())
    return torch.cat(out)


def fused(y_win, var_win, mode, have_win=None):
    """y_win、var_win：[N,K,C,9,12]（在 CPU 上）；have_win：[N,K-1]，前面的幀有沒有收到（不給表示都有）。
    回傳畫面 [N,3,H,W] 與閘的平均開度 [N]。"""
    out, opened = [], []
    with torch.no_grad():
        for i in range(0, len(y_win), 128):
            y, v = y_win[i:i + 128].to(dev), var_win[i:i + 128].to(dev)
            have = None if have_win is None else have_win[i:i + 128].to(dev)
            g = gate(y, v, have) if mode == "fuse" else torch.ones_like(y[:, 1:])
            f, fv = fuse(y, v, g)
            out.append(model.decode(f, fv).cpu())
            opened.append(g.mean(dim=(1, 2, 3, 4)).cpu())
    return torch.cat(out), torch.cat(opened)


rows = []
for item in args.videos:
    video, path = item.split("=", 1)
    frames = np.fromfile(path, dtype=np.uint8).reshape(-1, H, W, 3)[::args.step]
    x = torch.from_numpy(frames.copy()).permute(0, 3, 1, 2).contiguous().float().div(255.0)
    N = len(x)
    with torch.no_grad():
        z = torch.cat([model.encode(x[i:i + 256].to(dev)).cpu() for i in range(0, N, 256)])
    adjacent = psnr_uint8(x[1:], x[:-1])                                       # 第 i 張與第 i+1 張
    change = (x[1:] - x[:-1]).pow(2).mean(dim=(1, 2, 3))
    T = torch.arange(K - 1, N)                                                  # 有完整的前 K−1 幀可以用的那些張
    cut = torch.stack([adjacent[T - 1 - j] < 12.0 for j in range(K - 1)], dim=1).any(dim=1)
    blank = x[T].flatten(1).std(dim=1) < 0.02
    same = ~cut & ~blank
    motion = change[T - 1]
    busy = torch.zeros(len(T), dtype=torch.bool)
    calm = torch.zeros(len(T), dtype=torch.bool)
    if int(same.sum()) >= 5:
        busy = same & (motion >= torch.quantile(motion[same], 0.8))
        calm = same & (motion <= torch.quantile(motion[same], 0.2))
    sets = (("全部", ~blank), ("同一個鏡頭內", same), ("剛換鏡頭", cut & ~blank), ("動作最大的兩成", busy), ("動作最小的兩成", calm))
    print(f"\n{video}：{N} 張（相隔 {args.step * 5 / 29.97:.2f} 秒），其中 {len(T)} 張有前面 {K - 1} 幀；同一個鏡頭內 {int(same.sum())} 張、"
          f"剛換鏡頭 {int((cut & ~blank).sum())} 張")
    for fade in ((False, True) if args.fade == "yes" else (False,)):
        for snr in snrs:
            if fade and snr > 60:
                continue
            gen = torch.Generator().manual_seed(int(snr * 10) + (1000 if fade else 0) + 21)
            var = torch.full(z.shape, 10.0 ** (-min(snr, 60.0) / 10.0))
            if fade:
                var = var / torch.empty(z.shape).exponential_(1.0, generator=gen).clamp_min(1e-4)
            y = z + torch.randn(z.shape, generator=gen) * torch.sqrt(var / 2.0)
            y_win = torch.stack([y[K - 1 - j:N - j] for j in range(K)], dim=1)
            var_win = torch.stack([var[K - 1 - j:N - j] for j in range(K)], dim=1)
            shown = {"base": decode(y[K - 1:], var[K - 1:])}
            shown["fuse"], opened = fused(y_win, var_win, "fuse")
            shown["all"], _ = fused(y_win, var_win, "all")
            quality = {n: psnr_uint8(shown[n], x[K - 1:]) for n in shown}
            tag = ("衰落 " if fade else "白雜訊 ") + ("沒有雜訊" if snr > 60 else f"{snr:g} dB")
            for set_name, sel in sets:
                if sel.sum() == 0:
                    continue
                b, f, a = (float(quality[n][sel].mean()) for n in ("base", "fuse", "all"))
                print(f"  {tag:<13}{set_name:<8}現在 {b:5.2f}  合併 {f:5.2f}（{f - b:+.2f}）  一律合併 {a:5.2f}（{a - b:+.2f}）  閘 {float(opened[sel].mean()):.2f}")
                rows.append((video, "fade" if fade else "awgn", snr, set_name, int(sel.sum()), b, f, a, float(opened[sel].mean())))
            if args.loss > 0.0:
                # 隨機掉幀：沒收到的那一幀螢幕停在上一張；合併時那一幀標成沒收到
                lost = torch.rand(N, generator=gen) < args.loss
                lost[:K - 1] = False
                have_win = torch.stack([~lost[K - 1 - j:N - j] for j in range(1, K)], dim=1)
                pictures = {"base": shown["base"], "fuse": fused(y_win, var_win, "fuse", have_win)[0]}
                line = f"  {tag:<13}掉幀 {args.loss:.0%}（螢幕上看到的，全部）"
                vals = []
                for n in ("base", "fuse"):
                    screen = pictures[n].clone()
                    for t in range(1, len(screen)):
                        if lost[K - 1 + t]:
                            screen[t] = screen[t - 1]
                    vals.append(float(psnr_uint8(screen, x[K - 1:])[~blank].mean()))
                print(line + f"  現在 {vals[0]:5.2f}  合併 {vals[1]:5.2f}（{vals[1] - vals[0]:+.2f}）")
                rows.append((video, "fade" if fade else "awgn", snr, f"掉幀{args.loss:g}", int((~blank).sum()), vals[0], vals[1], float("nan"), float("nan")))
if args.csv:
    with open(args.csv, "w", encoding="utf-8") as f:
        f.write("video,channel,snr,set,count,base,fuse,all,gate\n")
        for r in rows:
            f.write("%s,%s,%g,%s,%d,%.3f,%.3f,%.3f,%.3f\n" % r)
