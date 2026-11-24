# 訓練「接收端把最近幾幀合起來解」用的小網路（merge.py 的 Gate）。
# 發射端（編碼器）、空中格式、解碼器都不動，用的是現成的單張模型（--model，例如模式 14 的 261123-4）；只訓練 Gate。
# 兩個部分各學各的：
#   估計差距的那一部分，訓練時知道前後兩幀的值真正差多少，直接拿它當答案來學（不經過解碼器）；
#   決定閘的那一部分，用解出來的畫面和原圖的誤差來學。
# （只用畫面的誤差、讓一個網路直接輸出閘的做法試過：它會學成「都不合併」。）
#
# 用法：python3 train_merge.py --train data/train.npy --val data/val.npy --model ../../models/261123-4.bin --out run
#   --motion      合成短片的運動幅度：相鄰兩幀隔 0.68 秒是 1，照模式的幀長給（模式 14：1，模式 12：1.5，模式 11、13：4）。
#   --real-val    名字=路徑：96×72 的真實畫面（rgb24），每 --real-step 張取一張當作相鄰的幀；只拿來量，不參與訓練。
#
# 訓練用的短片由 clips.py 從靜態的小圖合成（靜止、鏡頭運動、前景移動、換鏡頭），相鄰兩張隔一幀的時間。
# 通道：同一段短片的幾幀訊雜比多半相近（±1.5 dB），三成各自獨立；衰落與缺值各幀各自獨立；
# 接收端以為的雜訊每個值各差一點、整幀再一起偏高或偏低；前面的幀有機會整幀沒收到，或是剛開始收、只有比較近的幾幀。
# 訓練完用 export_merge.py 匯出成核心讀的權重檔。
import argparse
import copy
import json
import math
import os
import signal
import time

import numpy as np
import torch

from clips import H, W, make_clips
from export import read_weights
from merge import STRUCTURE, Gate, fuse, pick_device
from model import JSCC, psnr_uint8, receive, widths_of


def load_single(path, dev):
    state, latent, model_id, arch = read_weights(path)
    model = JSCC(latent, arch, widths_of(state))
    model.load_state_dict(state)
    model = model.to(dev).eval().requires_grad_(False)
    return model, latent, model_id


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--train", required=True)
    p.add_argument("--val", required=True)
    p.add_argument("--model", required=True)                  # 單張模型的權重檔（.bin）：編碼器與解碼器都用它，不訓練
    p.add_argument("--frames", type=int, default=4)           # 現在這一幀＋前面幾幀，一共幾幀
    p.add_argument("--width", type=int, default=128)
    p.add_argument("--depth", type=int, default=2)            # 估計差距的網路有幾層 3×3 卷積
    p.add_argument("--rule-width", type=int, default=32)      # 決定閘的小網路的寬度
    p.add_argument("--steps", type=int, default=6000)
    p.add_argument("--batch", type=int, default=64)
    p.add_argument("--lr", type=float, default=1e-3)
    p.add_argument("--warmup", type=int, default=200)
    p.add_argument("--snr-hi", type=float, default=28.0)
    p.add_argument("--lost-share", type=float, default=0.2)
    p.add_argument("--lost-max", type=float, default=0.8)
    p.add_argument("--lost-db", default="10,40")
    p.add_argument("--gone-share", type=float, default=0.08)  # 前面的幀整幀沒收到的機率
    p.add_argument("--motion", type=float, default=1.0)       # 相鄰兩幀隔 0.68 秒是 1
    p.add_argument("--cut-share", type=float, default=0.08)
    p.add_argument("--out", required=True)
    p.add_argument("--check-every", type=int, default=500)
    p.add_argument("--progress-every", type=int, default=50)
    p.add_argument("--ema", type=float, default=0.999)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--val-clips", type=int, default=200)
    p.add_argument("--device", default="")
    p.add_argument("--real-val", action="append", default=[])
    p.add_argument("--real-step", type=int, default=1)
    args = p.parse_args()

    lost_db = tuple(float(v) for v in args.lost_db.split(","))
    K = args.frames
    os.makedirs(args.out, exist_ok=True)
    log_path = os.path.join(args.out, "train.log")
    dev = pick_device(args.device)

    def say(text):
        try:
            print(text, flush=True)
        except OSError:
            pass
        with open(log_path, "a", encoding="utf-8") as f:
            f.write(text + "\n")

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    for stop_signal in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(stop_signal, interrupted)
    say("──── " + time.strftime("%Y-%m-%d %H:%M:%S") + " ────")
    torch.manual_seed(args.seed)
    gen = torch.Generator().manual_seed(args.seed)

    CHUNK = 40000
    train_cpu = torch.from_numpy(np.load(args.train))          # uint8 [N,72,96,3]
    n = len(train_cpu)
    chunks = [train_cpu[i:i + CHUNK].to(dev) for i in range(0, n, CHUNK)]
    chunk_sizes = [len(c) for c in chunks]
    chunk_probs = torch.tensor(chunk_sizes, dtype=torch.float)
    del train_cpu
    val = torch.from_numpy(np.load(args.val)).permute(0, 3, 1, 2).contiguous().float().div(255.0).to(dev)

    model, C, model_id = load_single(args.model, dev)
    snr_lo = model.min_snr_db
    shape = (C, 9, 12)
    REF_LO, REF_HI = 10.0 ** (-30.0 / 10.0), 10.0 ** (-model.min_snr_db / 10.0)
    gate = Gate(C, K, args.width, args.depth, args.rule_width).to(dev)
    params = list(gate.parameters())
    opt = torch.optim.AdamW(params, lr=args.lr, weight_decay=1e-5)
    EMA_EVERY = 10
    ema = copy.deepcopy(gate).requires_grad_(False)
    ema_params = list(ema.parameters())
    ema_weight = 1.0 - args.ema ** EMA_EVERY
    say(f"訓練 {n} 張小圖、驗證 {len(val)} 張；單張模型 {model_id:08X}（{C * 108} 個實數，不訓練）；合併 {K} 幀（現在這一幀＋前面 {K - 1} 幀）；"
        f"閘的參數 {sum(q.numel() for q in params) / 1e3:.0f} 千個；訊雜比 {snr_lo:g}–{args.snr_hi:g} dB；"
        f"缺值：{args.lost_share:g} 的幀、每幀 0–{args.lost_max:g}、弱 {lost_db[0]:g}–{lost_db[1]:g} dB；前面的幀整幀沒收到 {args.gone_share:g}；"
        f"合成短片的運動幅度 {args.motion:g}、換鏡頭 {args.cut_share:g}；裝置 {dev}；PyTorch {torch.__version__}")

    def lr_at(step):
        if step < args.warmup:
            return args.lr * (step + 1) / args.warmup
        t = (step - args.warmup) / max(1, args.steps - args.warmup)
        return args.lr * (0.02 + 0.98 * 0.5 * (1 + math.cos(math.pi * t)))

    def channel_state(count):
        """K 幀各自的雜訊變異數 [count,K,C,9,12]：真正的、接收端以為的。"""
        full = (count, K) + shape

        def rand(*size):
            return torch.rand(*size, device=dev)

        base = snr_lo + (args.snr_hi - snr_lo) * rand(count, 1)
        near = base + 3.0 * rand(count, K) - 1.5
        apart = snr_lo + (args.snr_hi - snr_lo) * rand(count, K)
        snr_db = torch.where(rand(count, 1) < 0.7, near, apart)
        sigma2 = torch.pow(10.0, -snr_db / 10.0)[:, :, None, None, None]
        fading = (rand(count) < 0.75)[:, None, None, None, None]
        depth = (0.3 + 0.7 * rand(count))[:, None, None, None, None]
        exponential = -torch.log(rand(full).clamp_min(1e-6))
        gain = torch.where(fading, (1.0 - depth) + depth * exponential, torch.ones(full, device=dev))
        burst = (rand(count, K) < args.lost_share)[:, :, None, None, None]
        fraction = (args.lost_max * rand(count, K))[:, :, None, None, None]
        lost = rand(full) < fraction
        weaker = torch.pow(10.0, -(lost_db[0] + (lost_db[1] - lost_db[0]) * rand(count, K)) / 10.0)[:, :, None, None, None]
        gain = torch.where(burst & lost, gain * weaker, gain)
        true_var = sigma2 / gain.clamp_min(1e-4)
        # 接收端以為的雜訊：每個值各差一點（標準差 1 dB），整幀再一起偏高或偏低（±1.5 dB 以內）
        believed = true_var * torch.pow(10.0, torch.randn(full, device=dev) * 0.1 + (0.3 * rand(count, K, 1, 1, 1) - 0.15))
        # 前面的幀有沒有收到：各有 gone_share 的機會整幀沒收到；另外一成五的短片是剛開始收，只有比較近的幾幀
        back = torch.arange(1, K, device=dev)[None, :]
        available = torch.where(rand(count, 1) < 0.15, torch.randint(0, K - 1, (count, 1), device=dev), torch.full((count, 1), K - 1, device=dev))
        have = (rand(count, K - 1) >= args.gone_share) & (back <= available)
        return true_var, believed, have

    def decode(fused, fused_var):
        """和 model.decode 一樣的處理，參考值在同一個裝置上取中位數（訓練用；量測另外用 model.decode）。"""
        ref = fused_var.detach().flatten(1).median(dim=1).values.clamp(REF_LO, REF_HI)
        v, weight, snr_db = receive(fused, fused_var, ref)
        return model.dec(v, snr_db, weight)

    # ── 驗證：合成的短片（靜止、會動）──
    CHECKS = (("awgn", 0.0), ("awgn", 10.0), ("awgn", 20.0), ("awgn", 99.0), ("fade", 5.0), ("fade", 15.0))

    def conditions(count, frames, seed):
        g = torch.Generator().manual_seed(seed)
        out = {}
        for kind, snr in CHECKS:
            full = (count, frames) + shape if frames else (count,) + shape
            sigma2 = 10.0 ** (-min(snr, 60.0) / 10.0)
            if kind == "awgn":
                true_var = torch.full(full, sigma2)
            else:
                true_var = sigma2 / torch.empty(full).exponential_(1.0, generator=g).clamp_min(1e-4)
            out[f"{kind}{snr:g}"] = (true_var.to(dev), torch.randn(full, generator=g).to(dev))
        return out

    with torch.no_grad():
        g_dev = torch.Generator(device=dev).manual_seed(11)
        count = min(args.val_clips, len(val))
        val_sets = {}
        for set_name, clips in (("靜止", make_clips(val[:count], K, g_dev, static_only=True)),
                                ("會動", make_clips(val[len(val) - count:], K, g_dev, motion=args.motion, cut_share=args.cut_share))):
            z = model.encode(clips.reshape(count * K, 3, H, W)).reshape(count, K, C, 9, 12).flip(1).contiguous()
            val_sets[set_name] = (clips[:, K - 1].contiguous(), z, conditions(count, K, 7))

        # 真實畫面：每張各自收到一次（固定的雜訊），第 t 張用第 t、t−1、…、t−K+1 張收到的值
        REAL_CHECKS = ("awgn0", "awgn10")
        real_sets = {}
        for item in args.real_val:
            real_name, real_path = item.split("=", 1)
            frames_u8 = np.fromfile(real_path, dtype=np.uint8).reshape(-1, H, W, 3)[::args.real_step]
            x = torch.from_numpy(frames_u8.copy()).permute(0, 3, 1, 2).contiguous().float().div(255.0).to(dev)
            z = torch.cat([model.encode(x[i:i + 256]) for i in range(0, len(x), 256)])
            conds = conditions(len(x), 0, 13)
            received = {}
            for cond in REAL_CHECKS:
                true_var, noise = conds[cond]
                y = z + noise * torch.sqrt(true_var / 2.0)
                windows = torch.stack([y[K - 1 - j:len(y) - j] for j in range(K)], dim=1)       # [N-K+1,K,C,9,12]
                var = true_var[:len(windows), None].expand(-1, K, -1, -1, -1).contiguous()
                received[cond] = (windows, var)
            real_sets[real_name] = (x[K - 1:].contiguous(), received)
            del conds

    def run_eval(net, target, z_or_y, var, noise=None, mode="gate"):
        """量一組：mode 是 gate（訓練出來的閘）、none（閘全關＝現在的單張模式）、all（閘全開＝一律合併）。"""
        vals, opened = [], []
        with torch.no_grad():
            for i in range(0, len(target), 100):
                sl = slice(i, i + 100)
                v = var[sl]
                y = z_or_y[sl] if noise is None else z_or_y[sl] + noise[sl] * torch.sqrt(v / 2.0)
                if mode == "gate":
                    g = net(y, v)
                elif mode == "none":
                    g = torch.zeros_like(y[:, 1:])
                else:
                    g = torch.ones_like(y[:, 1:])
                fused, fused_var = fuse(y, v, g)
                vals.append(psnr_uint8(model.decode(fused, fused_var), target[sl]).cpu())
                opened.append(g.mean(dim=(1, 2, 3, 4)).cpu())
        return float(torch.cat(vals).mean()), float(torch.cat(opened).mean())

    def evaluate(net, mode="gate"):
        net.eval()
        out, opened = {}, {}
        for set_name, (target, z, conds) in val_sets.items():
            for name, (true_var, noise) in conds.items():
                out[f"{set_name}/{name}"], opened[f"{set_name}/{name}"] = run_eval(net, target, z, true_var, noise, mode)
        for set_name, (target, received) in real_sets.items():
            for name, (windows, var) in received.items():
                out[f"{set_name}/{name}"], opened[f"{set_name}/{name}"] = run_eval(net, target, windows, var, None, mode)
        net.train()
        return out, opened

    base, _ = evaluate(gate, "none")
    merged, _ = evaluate(gate, "all")
    say("現在的單張模式（閘全關）：" + "  ".join(f"{k}:{v:.2f}" for k, v in base.items()))
    say("一律合併（閘全開），和上面的差距：" + "  ".join(f"{k}:{merged[k] - base[k]:+.2f}" for k in base))
    say("驗證的情形：awgn0／10／20 是白雜訊 0／10／20 dB，awgn99 是沒有雜訊，fade5／15 是瑞利衰落平均 5／15 dB；"
        "數字是這一幀畫面的 PSNR（dB），括號裡是和現在的單張模式的差距；「閘」是閘平均開了多少")

    def text(ev, opened):
        parts = []
        for set_name in list(val_sets) + list(real_sets):
            keys = [k for k in ev if k.startswith(set_name + "/")]
            parts.append(set_name + " " + " ".join(f"{k.split('/')[1]}:{ev[k]:.2f}({ev[k] - base[k]:+.2f})" for k in keys)
                         + f" 閘 {sum(opened[k] for k in keys) / len(keys):.2f}")
        return "｜".join(parts)

    def hms(seconds):
        seconds = int(seconds + 0.5)
        return f"{seconds // 3600}:{seconds % 3600 // 60:02d}:{seconds % 60:02d}"

    def write(payload, path):
        torch.save(payload, path + ".tmp")
        os.replace(path + ".tmp", path)

    def save(name, net, step):
        write({"gate": net.state_dict(), "structure": STRUCTURE, "latent": C, "frames": K, "width": args.width, "depth": args.depth, "rule_width": args.rule_width,
               "model_id": model_id, "step": step},
              os.path.join(args.out, name))

    first, first_open = evaluate(gate)
    say("開始之前  " + text(first, first_open))
    log = []
    loss_acc = torch.zeros((), device=dev)
    progress_acc = torch.zeros((), device=dev)
    guess_acc = torch.zeros((), device=dev)                    # 「估計差距」那一部分的損失（只用來印進度）
    acc_steps = progress_steps = 0
    done = 0
    t0 = time.time()
    t_last = t0
    try:
        for step in range(args.steps):
            lr = lr_at(step)
            for group in opt.param_groups:
                group["lr"] = lr
            k = int(torch.multinomial(chunk_probs, 1, generator=gen))
            idx = torch.randint(0, chunk_sizes[k], (args.batch,), device=dev)
            x = torch.index_select(chunks[k], 0, idx).permute(0, 3, 1, 2).float().div(255.0)
            flip = torch.rand(args.batch, device=dev) < 0.5
            x = torch.where(flip[:, None, None, None], x.flip(3), x)
            with torch.no_grad():
                clips = make_clips(x, K, motion=args.motion, cut_share=args.cut_share)                  # [B,K,3,H,W]，照時間排
                z = model.encode(clips.reshape(args.batch * K, 3, H, W)).reshape(args.batch, K, C, 9, 12).flip(1).contiguous()
                true_var, believed, have = channel_state(args.batch)
                y = z + torch.randn_like(z) * torch.sqrt(true_var / 2.0)
                target = clips[:, K - 1].contiguous()
                changed = (z[:, :1] - z[:, 1:]).pow(2).mean(dim=2)                                      # 真正的差距 [B,K-1,9,12]
            raw = gate.estimate(y, believed, have)
            # 第一部分：估計的差距直接和真正的差距比（變異數估計常用的計分方式，答案是條件平均時最小）
            spread = torch.pow(10.0, raw)
            score = torch.log(spread) + (changed + 1e-4) / spread
            guess_loss = (score * have[:, :, None, None]).sum() / (have.sum() * 108).clamp_min(1)       # 只算有收到的幀
            # 第二部分：閘用畫面的誤差來學；估計值在這裡當成固定的輸入，兩部分各學各的
            fused, fused_var = fuse(y, believed, gate.gates(raw.detach(), believed, have))
            loss = torch.mean((decode(fused, fused_var) - target) ** 2)
            opt.zero_grad(set_to_none=True)
            (guess_loss + loss).backward()
            torch.nn.utils.clip_grad_norm_(gate.estimator.parameters(), 5.0)
            torch.nn.utils.clip_grad_norm_(gate.rule.parameters(), 1.0)
            opt.step()
            guess_acc += guess_loss.detach()
            done = step + 1
            if done % EMA_EVERY == 0:
                with torch.no_grad():
                    for averaged, current in zip(ema_params, params):
                        averaged.lerp_(current, ema_weight)
            loss_acc += loss.detach()
            progress_acc += loss.detach()
            acc_steps += 1
            progress_steps += 1

            checking = done % args.check_every == 0 or done == args.steps
            if args.progress_every > 0 and done % args.progress_every == 0 and not checking:
                spent = time.time() - t0
                say(f"  步 {done:6d}/{args.steps}（{100.0 * done / args.steps:4.1f}%）  損失 {progress_acc.item() / progress_steps:.5f}"
                    f"（估計差距 {guess_acc.item() / progress_steps:.3f}）  "
                    f"每步 {(time.time() - t_last) / acc_steps * 1000:5.1f} ms  已經跑了 {hms(spent)}  還要約 {hms(spent / done * (args.steps - done))}")
                progress_acc.zero_()
                guess_acc.zero_()
                progress_steps = 0
            if checking:
                mean_loss = loss_acc.item() / acc_steps
                ms_per_step = (time.time() - t_last) / acc_steps * 1000
                loss_acc.zero_()
                progress_acc.zero_()
                guess_acc.zero_()
                acc_steps = progress_steps = 0
                if not math.isfinite(mean_loss):
                    raise SystemExit("損失不是有限的數，停下來")
                ev, opened = evaluate(ema)
                elapsed = time.time() - t0
                log.append({"step": done, "loss": mean_loss, "seconds": elapsed, "lr": lr, "psnr": ev, "open": opened})
                say(f"步 {done:6d}  損失 {mean_loss:.5f}  {elapsed / 60:5.1f} 分  每步 {ms_per_step:5.1f} ms  學習率 {lr:.2e}  " + text(ev, opened))
                json.dump({"base": base, "all": merged, "log": log}, open(os.path.join(args.out, "log.json"), "w"))
                save("gate.pt", gate, done)
                save("ema.pt", ema, done)
                t_last = time.time()
    except KeyboardInterrupt:
        save("gate.pt", gate, done)
        save("ema.pt", ema, done)
        say(f"\n在第 {done} 步中斷，已經存檔。")
        raise SystemExit(130)

    final, final_open = evaluate(ema)
    minutes = (time.time() - t0) / 60
    json.dump({"final_ema": final, "open": final_open, "base": base, "all": merged, "minutes": minutes},
              open(os.path.join(args.out, "final.json"), "w"))
    say("權重的移動平均  " + text(final, final_open))
    say(f"完成；共 {minutes:.1f} 分。")
