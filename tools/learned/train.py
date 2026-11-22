# 訓練影像的聯合編碼模型（結構在 model.py）。資料是 prep.py 切好的 uint8 陣列 [N,72,96,3]（.npy）。
# 用法：python3 train.py --train data/train.npy --val data/val.npy --latent 72 --out run [--init 舊的權重檔.bin]
#       --latent 是潛在通道數，決定一張畫面變成幾個實數：72 → 7776（模式 11），24 → 2592（模式 12）。
# 在 Apple 晶片的 GPU（MPS）上跑；3 萬步約 50 分鐘（M4 Max）。
#
# 通道（結構 2）：每張畫面抽一個平均訊雜比；四分之一的畫面是白雜訊，其餘是衰落——每個值各自乘上一個隨機的功率增益
# （瑞利到接近不衰落之間），等化之後就是「各個值的雜訊不一樣大」。幀裡的值是打散過的（learned_frame.cpp），
# 所以一片衰落落在潛在張量上是散開的，這裡直接讓每個值各自獨立。另外有兩成的畫面再隨機抹掉一部分的值
# （干擾、音訊斷續），接收端估的雜訊也加一點誤差。接收端的處理（model.receive）和 C++ 的一樣。
# 結構 1 只有白雜訊，重現第一版的權重用。
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

from model import JSCC, psnr_uint8, receive, reference_noise

p = argparse.ArgumentParser()
p.add_argument("--train", required=True)
p.add_argument("--val", required=True)
p.add_argument("--latent", type=int, required=True)       # 潛在張量的通道數；實數個數 = latent × 9 × 12
p.add_argument("--arch", type=int, default=2)             # 模型的結構編號，見 model.py
p.add_argument("--init", default="")                      # 從這份權重檔（export.py 的 .bin）接著訓練；結構 1 的、潛在通道數不同的也可以
p.add_argument("--steps", type=int, default=30000)
p.add_argument("--batch", type=int, default=64)
p.add_argument("--lr", type=float, default=2e-4)
p.add_argument("--snr-lo", type=float, default=None)      # 預設：結構認得的下限
p.add_argument("--snr-hi", type=float, default=28.0)
p.add_argument("--out", required=True)
p.add_argument("--check-every", type=int, default=500)
p.add_argument("--drop-db", type=float, default=1.0)
p.add_argument("--warmup", type=int, default=1000)
p.add_argument("--seed", type=int, default=1)
p.add_argument("--compile", action="store_true")          # torch.compile（PyTorch 2.15 的 MPS 上每步快 5% 左右）
p.add_argument("--debug-break-at", type=int, default=-1)  # 測試用：在這一步故意把模型弄壞，看會不會退回
args = p.parse_args()

os.makedirs(args.out, exist_ok=True)
torch.manual_seed(args.seed)
gen = torch.Generator().manual_seed(args.seed)             # CPU 上的亂數：取哪一塊資料
dev = torch.device("mps")

CHUNK = 40000
train_cpu = torch.from_numpy(np.load(args.train))          # uint8 [N,72,96,3]
n = len(train_cpu)
chunks = [train_cpu[i:i + CHUNK].to(dev) for i in range(0, n, CHUNK)]
chunk_sizes = [len(c) for c in chunks]
chunk_probs = torch.tensor(chunk_sizes, dtype=torch.float)
del train_cpu
val = torch.from_numpy(np.load(args.val)).permute(0, 3, 1, 2).float().div(255.0).to(dev)

model = JSCC(args.latent, args.arch).to(dev)
snr_lo = model.min_snr_db if args.snr_lo is None else args.snr_lo
shape = (args.latent, 9, 12)
print(f"訓練 {n} 張、驗證 {len(val)} 張；潛在通道 {args.latent}（{args.latent * 108} 個實數）；結構 {args.arch}；"
      f"訊雜比 {snr_lo:g}–{args.snr_hi:g} dB；裝置 {dev}；學習率上限 {args.lr}；PyTorch {torch.__version__}", flush=True)

if args.init:
    from export import read_weights
    state, latent, _, _ = read_weights(args.init)
    own = model.state_dict()
    fresh = []
    for name, value in state.items():
        if own[name].shape == value.shape:
            own[name] = value
        elif latent != args.latent and name in ("enc.head.weight", "enc.head.bias", "dec.inp.weight"):
            fresh.append(name)                             # 潛在通道數不同：瓶頸兩側的這幾層接不上，留著剛初始化的值從頭學
        elif name == "dec.inp.weight":                     # 結構 1 → 2：多出來的「可靠程度」輸入先接上 0，一開始和舊模型算的一樣
            own[name] = torch.zeros_like(own[name])
            own[name][:, :value.shape[1]] = value
        else:
            raise SystemExit(f"{name} 的形狀接不上")
    model.load_state_dict(own)
    print(f"從 {args.init} 接著訓練" + (f"（潛在通道 {latent} → {args.latent}：{'、'.join(fresh)} 從頭學）" if fresh else ""), flush=True)

params = list(model.parameters())
print(f"參數 {sum(q.numel() for q in params) / 1e6:.2f} M", flush=True)
opt = torch.optim.AdamW(params, lr=args.lr, weight_decay=1e-5, fused=True)
enc_net = torch.compile(model.enc) if args.compile else model.enc
dec_net = torch.compile(model.dec) if args.compile else model.dec
REF_LO, REF_HI = 10.0 ** (-30.0 / 10.0), 10.0 ** (-model.min_snr_db / 10.0)   # 參考雜訊變異數的範圍


def lr_at(step):
    if step < args.warmup:
        return args.lr * (step + 1) / args.warmup
    t = (step - args.warmup) / max(1, args.steps - args.warmup)
    return args.lr * (0.02 + 0.98 * 0.5 * (1 + math.cos(math.pi * t)))


def channel_state(count):
    """count 張畫面各自的通道狀態（都在 GPU 上算，不必等 CPU）：真正的雜訊變異數、接收端以為的雜訊變異數，
    形狀都是 [count,C,9,12]。"""
    full = (count,) + shape
    snr_db = torch.empty(count, device=dev).uniform_(snr_lo, args.snr_hi)
    sigma2 = torch.pow(10.0, -snr_db / 10.0)[:, None, None, None]
    if model.architecture < 2:
        true_var = sigma2.expand(full)
        return true_var, true_var
    # 四分之三的畫面有衰落：每個值的功率增益是 (1−d) + d·指數分布，d 在 0.3 到 1 之間（1 是瑞利衰落）
    fading = (torch.rand(count, device=dev) < 0.75)[:, None, None, None]
    depth = torch.empty(count, device=dev).uniform_(0.3, 1.0)[:, None, None, None]
    exponential = -torch.log(torch.rand(full, device=dev).clamp_min(1e-6))
    gain = torch.where(fading, (1.0 - depth) + depth * exponential, torch.ones(full, device=dev))
    # 兩成的畫面另外有一部分（最多四成）的值幾乎沒收到：干擾、音訊斷續
    burst = (torch.rand(count, device=dev) < 0.2)[:, None, None, None]
    fraction = torch.empty(count, device=dev).uniform_(0.0, 0.4)[:, None, None, None]
    lost = torch.rand(full, device=dev) < fraction
    gain = torch.where(burst & lost, gain * 0.01, gain)
    true_var = sigma2 / gain.clamp_min(1e-4)
    # 接收端估的雜訊有誤差：標準差 1 dB
    believed = true_var * torch.pow(10.0, torch.randn(full, device=dev) * 0.1)
    return true_var, believed


def run(x, true_var, believed, noise=None, ref=None):
    """畫面 → 通道 → 接收端的處理 → 解碼。"""
    if ref is None:
        # 訓練時在 GPU 上取中位數（和 reference_noise 差在偶數個時取中間兩個的哪一個，可以忽略）
        ref = believed.flatten(1).median(dim=1).values.clamp(REF_LO, REF_HI)
    z = enc_net(x)
    z = z * torch.sqrt(0.5 / (z.pow(2).mean(dim=(1, 2, 3), keepdim=True) + 1e-9))
    if noise is None:
        noise = torch.randn_like(z)
    v, weight, snr_db = receive(z + noise * torch.sqrt(true_var / 2.0), believed, ref)
    return dec_net(v, snr_db, weight)


# 驗證用的情形：白雜訊 0／10／20 dB 與沒有雜訊，瑞利衰落（平均）5／15 dB。通道狀態與雜訊先算好，每次都用同一組。
CHECKS = (("awgn", 0.0), ("awgn", 10.0), ("awgn", 20.0), ("awgn", 99.0), ("fade", 5.0), ("fade", 15.0))
check_data = {}
with torch.no_grad():
    g = torch.Generator().manual_seed(7)
    for kind, snr in CHECKS:
        full = (len(val),) + shape
        sigma2 = 10.0 ** (-min(snr, 60.0) / 10.0)
        if kind == "awgn":
            true_var = torch.full(full, sigma2)
        else:
            true_var = sigma2 / torch.empty(full).exponential_(1.0, generator=g).clamp_min(1e-4)
        check_data[f"{kind}{snr:g}"] = (true_var.to(dev), reference_noise(true_var, model.min_snr_db).to(dev),
                                         torch.randn(full, generator=g).to(dev))


def evaluate():
    model.eval()
    out = {}
    with torch.no_grad():
        for name, (true_var, ref, noise) in check_data.items():
            vals = []
            for i in range(0, len(val), 200):
                sl = slice(i, i + 200)
                vals.append(psnr_uint8(run(val[sl], true_var[sl], true_var[sl], noise[sl], ref[sl]), val[sl]).cpu())
            out[name] = float(torch.cat(vals).mean())
    model.train()
    return out


def snapshot():
    return copy.deepcopy(model.state_dict()), copy.deepcopy(opt.state_dict())


def save(name, step):
    torch.save({"model": model.state_dict(), "latent": args.latent, "architecture": args.arch, "step": step},
               os.path.join(args.out, name))


first = evaluate()
print("開始之前  " + "  ".join(f"{k}:{v:.2f}" for k, v in first.items()), flush=True)

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
    true_var, believed = channel_state(args.batch)
    loss = torch.mean((run(x, true_var, believed) - x) ** 2)
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
        ev = evaluate()
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
        log.append({"step": step + 1, "loss": mean_loss, "seconds": elapsed, "lr": lr, "psnr": ev, "rolled_back": bool(note)})
        print(f"步 {step + 1:6d}  損失 {mean_loss:.5f}  {elapsed / 60:5.1f} 分  每步 {ms_per_step:5.1f} ms  "
              f"學習率 {lr:.2e}  " + "  ".join(f"{k}:{v:.2f}" for k, v in ev.items()) + note, flush=True)
        json.dump(log, open(os.path.join(args.out, "log.json"), "w"))
        t_last = time.time()

final = evaluate()
save("model.pt", args.steps)
json.dump({"final": final, "rollbacks": rollbacks, "best_step": best_step, "minutes": (time.time() - t0) / 60},
          open(os.path.join(args.out, "final.json"), "w"))
print("最後的模型  " + "  ".join(f"{k}:{v:.2f}" for k, v in final.items()), flush=True)
print(f"完成；退回 {rollbacks} 次；共 {(time.time() - t0) / 60:.1f} 分", flush=True)
