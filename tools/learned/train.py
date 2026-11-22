# 訓練影像的聯合編碼模型（結構在 model.py）。資料是 prep.py 切好的 uint8 陣列 [N,72,96,3]（.npy）。
# 用法：python3 train.py --train data/train.npy --val data/val.npy --latent 72 --out run
# 在 Apple 晶片的 GPU（MPS）上跑；3 萬步約 50 分鐘（M4 Max）。
#
#  - 每隔 --check-every 步驗證一次。畫質比最好的時候低 --drop-db 以上，或出現非數值，
#    就退回最好的那個狀態（模型與最佳化器一起），學習率係數乘 0.5 再繼續。
#    學習率上限 4e-4 時曾在第 8000–9000 步之間發散（31 dB 掉到 16 dB）；2e-4 跑完 3 萬步沒有發散。
#  - 訓練資料分塊放在 GPU 上，取樣也在 GPU 上做。分塊是為了讓每個張量小於 2^31 個元素：
#    之後的 PyTorch 版本修過一批 MPS 上大張量算錯的問題，這裡保守一點。
#  - 送進模型的張量要是連續的 NCHW 排列。PyTorch 2.8.0 的 MPS 遇到 permute 之後沒有整理過的輸入，
#    每步會從 93 ms 變成 200 ms 左右；下面的 torch.where 在 2.8.0 剛好會產生連續的張量。
#    2.15.0 沒有這個問題。
#  - 最佳化器用 fused 的實作（每步快 5 ms 左右）。
import argparse
import copy
import json
import math
import os
import time

import numpy as np
import torch

from model import JSCC, psnr_uint8

p = argparse.ArgumentParser()
p.add_argument("--train", required=True)
p.add_argument("--val", required=True)
p.add_argument("--latent", type=int, required=True)       # 潛在張量的通道數；實數個數 = latent × 9 × 12
p.add_argument("--steps", type=int, default=30000)
p.add_argument("--batch", type=int, default=64)
p.add_argument("--lr", type=float, default=2e-4)
p.add_argument("--snr-lo", type=float, default=-2.0)
p.add_argument("--snr-hi", type=float, default=28.0)
p.add_argument("--out", required=True)
p.add_argument("--check-every", type=int, default=500)
p.add_argument("--drop-db", type=float, default=1.0)
p.add_argument("--warmup", type=int, default=1000)
p.add_argument("--seed", type=int, default=1)
p.add_argument("--debug-break-at", type=int, default=-1)  # 測試用：在這一步故意把模型弄壞，看會不會退回
args = p.parse_args()

os.makedirs(args.out, exist_ok=True)
torch.manual_seed(args.seed)
gen = torch.Generator().manual_seed(args.seed)             # 取樣本用的亂數，在 CPU 上
dev = torch.device("mps")

CHUNK = 40000
train_cpu = torch.from_numpy(np.load(args.train))          # uint8 [N,72,96,3]
n = len(train_cpu)
chunks = [train_cpu[i:i + CHUNK].to(dev) for i in range(0, n, CHUNK)]
chunk_sizes = [len(c) for c in chunks]
chunk_probs = torch.tensor(chunk_sizes, dtype=torch.float)
del train_cpu
val = torch.from_numpy(np.load(args.val)).permute(0, 3, 1, 2).float().div(255.0).to(dev)
print(f"訓練 {n} 張、驗證 {len(val)} 張；潛在通道 {args.latent}（{args.latent * 108} 個實數）；"
      f"裝置 {dev}；學習率上限 {args.lr}", flush=True)

model = JSCC(args.latent).to(dev)
params = list(model.parameters())
print(f"參數 {sum(q.numel() for q in params) / 1e6:.2f} M", flush=True)
opt = torch.optim.AdamW(params, lr=args.lr, weight_decay=1e-5, fused=True)

CHECK_SNRS = (0, 10, 20, 99)
ALL_SNRS = (0, 5, 10, 15, 20, 25, 30, 99)


def lr_at(step):
    if step < args.warmup:
        return args.lr * (step + 1) / args.warmup
    t = (step - args.warmup) / max(1, args.steps - args.warmup)
    return args.lr * (0.02 + 0.98 * 0.5 * (1 + math.cos(math.pi * t)))


def evaluate(snrs):
    """每個訊雜比下，驗證圖的平均 PSNR。每次用同一組雜訊，前後才能比。"""
    model.eval()
    out = {}
    g = torch.Generator().manual_seed(7)
    with torch.no_grad():
        for snr in snrs:
            vals = []
            for i in range(0, len(val), 200):
                x = val[i:i + 200]
                s = torch.full((len(x),), float(min(snr, 30)), device=dev)   # 沒有雜訊時告訴解碼器的是訓練過的最高值附近
                noise = torch.randn((len(x), args.latent, 9, 12), generator=g).to(dev)
                if snr >= 90:
                    noise = torch.zeros_like(noise)
                vals.append(psnr_uint8(model(x, s, noise), x).cpu())
            out[snr] = float(torch.cat(vals).mean())
    model.train()
    return out


def snapshot():
    return copy.deepcopy(model.state_dict()), copy.deepcopy(opt.state_dict())


def save(name, step):
    torch.save({"model": model.state_dict(), "latent": args.latent, "step": step}, os.path.join(args.out, name))


best_metric = -math.inf
best_state = snapshot()
best_step = 0
lr_scale = 1.0
rollbacks = 0
log = []
loss_acc = torch.zeros((), device=dev)
acc_steps = 0
t0 = time.time()
t_last = t0

for step in range(args.steps):
    lr = lr_at(step) * lr_scale
    for group in opt.param_groups:
        group["lr"] = lr
    k = int(torch.multinomial(chunk_probs, 1, generator=gen))       # 每批取自同一塊；各塊依大小輪到
    idx = torch.randint(0, chunk_sizes[k], (args.batch,), device=dev)
    x = torch.index_select(chunks[k], 0, idx).permute(0, 3, 1, 2).float().div(255.0)
    flip = torch.rand(args.batch, device=dev) < 0.5                 # 一半的圖左右翻轉
    x = torch.where(flip[:, None, None, None], x.flip(3), x)
    snr = torch.empty(args.batch, device=dev).uniform_(args.snr_lo, args.snr_hi)
    loss = torch.mean((model(x, snr) - x) ** 2)
    opt.zero_grad(set_to_none=True)
    loss.backward()
    torch.nn.utils.clip_grad_norm_(params, 1.0)
    opt.step()
    loss_acc += loss.detach()
    acc_steps += 1

    if step == args.debug_break_at:
        with torch.no_grad():
            model.dec.out.weight.mul_(40.0)
        print(f"（測試）第 {step + 1} 步故意把輸出層放大 40 倍", flush=True)

    if (step + 1) % args.check_every == 0 or step + 1 == args.steps:
        mean_loss = loss_acc.item() / acc_steps
        ms_per_step = (time.time() - t_last) / acc_steps * 1000
        loss_acc.zero_()
        acc_steps = 0
        ev = evaluate(CHECK_SNRS)
        metric = sum(ev.values()) / len(ev)
        note = ""
        if not (math.isfinite(metric) and math.isfinite(mean_loss)) or metric < best_metric - args.drop_db:
            model.load_state_dict(best_state[0])
            opt.load_state_dict(copy.deepcopy(best_state[1]))   # 載入時不複製的話，之後的更新會改到存下來的那一份
            lr_scale *= 0.5
            rollbacks += 1
            note = f"  ← 發散，退回第 {best_step} 步，學習率係數 {lr_scale:g}"
        elif metric > best_metric:
            best_metric = metric
            best_state = snapshot()
            best_step = step + 1
            save("best.pt", best_step)
        elapsed = time.time() - t0
        log.append({"step": step + 1, "loss": mean_loss, "seconds": elapsed, "lr": lr, "psnr": ev,
                    "rolled_back": bool(note)})
        print(f"步 {step + 1:6d}  損失 {mean_loss:.5f}  {elapsed / 60:5.1f} 分  每步 {ms_per_step:5.1f} ms  "
              f"學習率 {lr:.2e}  " + "  ".join(f"{k}:{v:.2f}" for k, v in ev.items()) + note, flush=True)
        json.dump(log, open(os.path.join(args.out, "log.json"), "w"))
        t_last = time.time()

final = evaluate(ALL_SNRS)
save("model.pt", args.steps)
json.dump({"final": final, "rollbacks": rollbacks, "best_step": best_step, "minutes": (time.time() - t0) / 60},
          open(os.path.join(args.out, "final.json"), "w"))
print("最後的模型  " + "  ".join(f"{k}:{v:.2f}" for k, v in final.items()), flush=True)
print(f"完成；退回 {rollbacks} 次；共 {(time.time() - t0) / 60:.1f} 分", flush=True)
