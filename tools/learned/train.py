# 訓練影像的聯合編碼模型（結構在 model.py）。資料是 prep.py 切好的 uint8 陣列 [N,72,96,3]（.npy）。
# 用法：python3 train.py --train data/train.npy --val data/val.npy --latent 72 --out run [--init 舊的權重檔.bin]
#       --latent 是潛在通道數，決定一張畫面變成幾個實數：72 → 7776（模式 11、13），24 → 2592（模式 12），14 → 1512（模式 14）。
# 在 Apple 晶片的 GPU（MPS）上跑；3 萬步約 50 分鐘（M4 Max）。
#
# 通道（結構 2）：每張畫面抽一個平均訊雜比；四分之一的畫面是白雜訊，其餘是衰落——每個值各自乘上一個隨機的功率增益
# （瑞利到接近不衰落之間），等化之後就是「各個值的雜訊不一樣大」。幀裡的值是打散過的（learned_frame.cpp），
# 所以一片衰落落在潛在張量上是散開的，這裡直接讓每個值各自獨立。另外有兩成的畫面再隨機抹掉一部分的值
# （干擾、音訊斷續），接收端估的雜訊也加一點誤差。接收端的處理（model.receive）和 C++ 的一樣。
# 結構 1 只有白雜訊，重現第一版的權重用。
#
# 抹掉的值（缺值）預設是：兩成的畫面（--lost-share）、每張抹掉 0 到四成（--lost-max）、訊號比其餘的弱 20 dB（--lost-db）。
# 模式 11 的第二、三版都是這樣訓練的，結果是「一半以上的值沒收到」超出訓練的範圍：這時雜訊的中位數落在沒收到的值上，
# 告訴解碼器的訊雜比掉到下限，偶爾會整張壞掉（畫面上冒出帶點陣的亮斑）。要涵蓋這種情形，把範圍放寬，例如
# --lost-max 0.8 --lost-db 10,40（弱幾 dB 給兩個數時，每張畫面在這個範圍裡抽一個）。試過的結果在 README。
# 每次驗證另外列出「少掉六成」這一種情形（其餘的值 10 dB，六成的值雜訊是訊號的 100 倍）的平均與最差的 1%：
# 整張壞掉的畫面只有 10 dB 上下，會把最差的 1% 拉到 10 dB 以下。這一項不算進平均差，也不影響退回。
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
#
# 加大模型（實驗）：--widths 換一組各層的寬度，例如 64,128,256,384 是每一層都加倍（參數與運算量約四倍）。
# 配上 --init 的話，舊權重放在新模型前面的那些通道；新通道讀得到舊通道，舊通道與輸出一開始讀不到新通道（那些權重是 0），
# 所以起點算出來的和舊模型完全相同，多出來的容量從這裡開始學。--init 給的權重檔本來就是這種寬度的話，直接接著訓練。
# C++ 的推論由權重檔裡張量的形狀看出寬度；哪個模式用哪種寬度的模型寫在核心的模式表（模式 13、14 用 64,128,256,384）。
#
# 長時間的訓練：
#  - --ema 0.999：另外留一份「權重的移動平均」（每一步往當下的權重靠近千分之一）。學習率高的時候，當下的權重在好的位置
#    附近晃動，驗證的數字會比真正學到的程度低（先前接續訓練時，前三分之二都比起點低 1 dB 以上，學習率降下來才回升）；
#    平均過的權重沒有這個晃動，中途看得出進展，中途停下來也拿得到堪用的模型。驗證時兩份都量，平均的那一份存成 ema.pt。
#  - --progress-every N：每 N 步印一行進度與預估的剩餘時間。
#  - --compare 權重檔：先用同一組驗證條件量這份對照的模型，之後每次驗證都把和它的差距印在括號裡。
#  - --resume：--out 裡有 last.pt 就從那裡接著跑。last.pt 每次驗證後存一次；按 Ctrl-C、關掉終端機視窗或被 kill 時也會存，
#    所以同一行指令再執行一次就是接續（電腦當掉或被 kill -9 的話，最多重跑一個驗證間隔）。
#  - --keep-awake：訓練期間不讓電腦因為閒置而睡眠（macOS 的 caffeinate；螢幕照常可以關、可以鎖定）。
#  - 印在終端機上的訊息同時附加到 --out 裡的 train.log，可以在別的視窗用 tail -f 看。
import argparse
import copy
import json
import math
import os
import signal
import subprocess
import time

import numpy as np
import torch

from model import DEFAULT_WIDTHS, JSCC, psnr_uint8, receive, reference_noise, widths_of

p = argparse.ArgumentParser()
p.add_argument("--train", required=True)
p.add_argument("--val", required=True)
p.add_argument("--latent", type=int, required=True)       # 潛在張量的通道數；實數個數 = latent × 9 × 12
p.add_argument("--arch", type=int, default=2)             # 模型的結構編號，見 model.py
p.add_argument("--widths", default="")                    # 各層的寬度：四個數用逗號隔開；不給就是 model.DEFAULT_WIDTHS
p.add_argument("--init", default="")                      # 從這份權重檔（export.py 的 .bin）接著訓練；結構 1 的、潛在通道數不同的、比較窄的都可以
p.add_argument("--steps", type=int, default=30000)
p.add_argument("--batch", type=int, default=64)
p.add_argument("--lr", type=float, default=2e-4)
p.add_argument("--snr-lo", type=float, default=None)      # 預設：結構認得的下限
p.add_argument("--snr-hi", type=float, default=28.0)
p.add_argument("--lost-share", type=float, default=0.2)   # 幾成的畫面另外有一部分的值幾乎沒收到（干擾、音訊斷續）
p.add_argument("--lost-max", type=float, default=0.4)     # 那些畫面裡沒收到的值最多佔幾成（每張在 0 到這個數之間抽）
p.add_argument("--lost-db", default="20")                 # 沒收到的值訊號比其餘的弱幾 dB：一個數，或「下限,上限」（每張抽一個）
p.add_argument("--out", required=True)
p.add_argument("--check-every", type=int, default=500)
p.add_argument("--progress-every", type=int, default=0)   # 每隔幾步印一行進度；0 表示只在驗證時印
p.add_argument("--compare", default="")                   # 對照用的權重檔：驗證時印出和它的差距
p.add_argument("--resume", action="store_true")           # --out 裡有 last.pt 就接著跑
p.add_argument("--ema", type=float, default=0.0)          # 權重移動平均的係數（例如 0.999）；0 表示不做
p.add_argument("--keep-awake", action="store_true")       # 訓練期間不讓電腦閒置睡眠
p.add_argument("--drop-db", type=float, default=1.0)
p.add_argument("--warmup", type=int, default=1000)
p.add_argument("--seed", type=int, default=1)
p.add_argument("--compile", action="store_true")          # torch.compile（PyTorch 2.15 的 MPS 上每步快 5% 左右）
p.add_argument("--debug-break-at", type=int, default=-1)  # 測試用：在這一步故意把模型弄壞，看會不會退回
args = p.parse_args()

widths = tuple(int(w) for w in args.widths.split(",")) if args.widths else DEFAULT_WIDTHS
if len(widths) != 4 or min(widths) <= 0:
    raise SystemExit("--widths 要四個正整數，例如 64,128,256,384")
try:
    lost_db = tuple(float(v) for v in args.lost_db.split(","))
except ValueError:
    lost_db = ()
if len(lost_db) == 1:
    lost_db = (lost_db[0], lost_db[0])
if len(lost_db) != 2 or not 0.0 < lost_db[0] <= lost_db[1]:
    raise SystemExit("--lost-db 要一個正數，或「下限,上限」，例如 20 或 10,40")
if not (0.0 <= args.lost_share <= 1.0 and 0.0 <= args.lost_max <= 1.0):
    raise SystemExit("--lost-share 與 --lost-max 要在 0 和 1 之間")
# 接續時要和原來的設定相同，不然學習率的排程、模型的形狀、訓練用的通道都對不上
config = {"latent": args.latent, "arch": args.arch, "widths": list(widths), "steps": args.steps, "batch": args.batch,
          "lr": args.lr, "warmup": args.warmup, "ema": args.ema,
          "lost_share": args.lost_share, "lost_max": args.lost_max, "lost_db": list(lost_db)}
# 有缺值的選項之前存下來的狀態沒有這幾項，當時用的就是這組預設值
OLDER_CONFIG = {"lost_share": 0.2, "lost_max": 0.4, "lost_db": [20.0, 20.0]}
if not 0.0 <= args.ema < 1.0:
    raise SystemExit("--ema 要在 0 和 1 之間，例如 0.999")

os.makedirs(args.out, exist_ok=True)
state_path = os.path.join(args.out, "last.pt")
log_path = os.path.join(args.out, "train.log")
dev = torch.device("mps")


def say(text):
    """印到終端機，同時附加到 train.log。終端機已經關掉的話只寫檔。"""
    try:
        print(text, flush=True)
    except OSError:
        pass
    with open(log_path, "a", encoding="utf-8") as f:
        f.write(text + "\n")


def interrupted(signum, frame):
    raise KeyboardInterrupt


# Ctrl-C、kill、關掉終端機視窗都當成「中斷」，下面的迴圈會先把狀態存好再結束。
# （SIGINT 也明確設一次：從背景啟動時，它可能一開始就被設成忽略。）
STOP_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
for stop_signal in STOP_SIGNALS:
    signal.signal(stop_signal, interrupted)
if args.keep_awake:
    try:
        subprocess.Popen(["caffeinate", "-is", "-w", str(os.getpid())])   # 這個行程結束時它會跟著結束
    except OSError:
        pass
say("──── " + time.strftime("%Y-%m-%d %H:%M:%S") + " ────")
resumed = torch.load(state_path, map_location=dev) if args.resume and os.path.exists(state_path) else None
if resumed is not None:
    changed = [k for k in config if resumed["config"].get(k, OLDER_CONFIG.get(k)) != config[k]]
    if changed:
        raise SystemExit(f"{state_path} 是用不同的設定（{'、'.join(changed)}）訓練的，不能接著跑；換一個 --out，或把設定改回來")
start_step = resumed["step"] if resumed is not None else 0
torch.manual_seed(args.seed + start_step)                  # 接續時換一組亂數，不重複開頭那一段
gen = torch.Generator().manual_seed(args.seed + start_step)  # CPU 上的亂數：取哪一塊資料

CHUNK = 40000
train_cpu = torch.from_numpy(np.load(args.train))          # uint8 [N,72,96,3]
n = len(train_cpu)
chunks = [train_cpu[i:i + CHUNK].to(dev) for i in range(0, n, CHUNK)]
chunk_sizes = [len(c) for c in chunks]
chunk_probs = torch.tensor(chunk_sizes, dtype=torch.float)
del train_cpu
val = torch.from_numpy(np.load(args.val)).permute(0, 3, 1, 2).float().div(255.0).to(dev)

model = JSCC(args.latent, args.arch, widths)
snr_lo = model.min_snr_db if args.snr_lo is None else args.snr_lo
shape = (args.latent, 9, 12)
say(f"訓練 {n} 張、驗證 {len(val)} 張；潛在通道 {args.latent}（{args.latent * 108} 個實數）；結構 {args.arch}；"
      f"各層寬度 {'／'.join(str(w) for w in widths)}；訊雜比 {snr_lo:g}–{args.snr_hi:g} dB；"
      f"缺值：{args.lost_share:g} 的畫面、每張 0–{args.lost_max:g}、弱 "
      + (f"{lost_db[0]:g}" if lost_db[0] == lost_db[1] else f"{lost_db[0]:g}–{lost_db[1]:g}")
      + f" dB；裝置 {dev}；學習率上限 {args.lr}；PyTorch {torch.__version__}")


def embed(name, old, new):
    """各層加寬時，把舊權重 old 放進形狀較大的 new（就地修改）：舊的通道排在前面。放不進去回傳 False。
    舊的輸出通道對新的輸入通道的權重設成 0，舊通道算出來的值就和原來一樣；新的輸出通道留著剛初始化的值。"""
    if old.dim() != new.dim() or any(a > b for a, b in zip(old.shape, new.shape)):
        return False
    if ".films." in name:
        # FiLM 的輸出是 [γ…, β…] 兩半：舊的 γ 放在前半的開頭，舊的 β 放在後半的開頭
        half_old, half_new = old.shape[0] // 2, new.shape[0] // 2
        new[:half_old] = old[:half_old]
        new[half_new:half_new + half_old] = old[half_old:]
        return True
    if old.dim() >= 2:
        new[:old.shape[0]] = 0.0
    new[tuple(slice(0, size) for size in old.shape)] = old
    return True


if args.init and resumed is None:
    from export import read_weights
    state, latent, _, _ = read_weights(args.init)
    own = model.state_dict()                               # 和模型的參數共用記憶體
    fresh, widened = [], False
    with torch.no_grad():
        for name, value in state.items():
            if own[name].shape == value.shape:
                own[name].copy_(value)
            elif latent != args.latent and name in ("enc.head.weight", "enc.head.bias", "dec.inp.weight"):
                fresh.append(name)                         # 潛在通道數不同：瓶頸兩側的這幾層接不上，留著剛初始化的值從頭學
            elif embed(name, value, own[name]):
                # 各層加寬；或是結構 1 → 2，多出來的「可靠程度」輸入先接上 0。兩種情形一開始都和舊模型算的一樣
                widened = widths != DEFAULT_WIDTHS
            else:
                raise SystemExit(f"{name} 的形狀接不上")
    notes = []
    if fresh:
        notes.append(f"潛在通道 {latent} → {args.latent}：{'、'.join(fresh)} 從頭學")
    if widened:
        notes.append("各層加寬：舊權重放在前面的通道，新通道一開始不影響輸出")
    say(f"從 {args.init} 接著訓練" + (f"（{'；'.join(notes)}）" if notes else ""))

model.to(dev)
params = list(model.parameters())
say(f"參數 {sum(q.numel() for q in params) / 1e6:.2f} M")
opt = torch.optim.AdamW(params, lr=args.lr, weight_decay=1e-5, fused=True)
if resumed is not None:
    model.load_state_dict(resumed["model"])
    opt.load_state_dict(resumed["opt"])
# 權重的移動平均：另一份同樣的模型，從同一組權重出發。每 EMA_EVERY 步更新一次（係數換算成這麼多步的份量）
EMA_EVERY = 10
ema = copy.deepcopy(model).requires_grad_(False) if args.ema > 0.0 else None
if ema is not None and resumed is not None:
    ema.load_state_dict(resumed["ema"])
ema_params = list(ema.parameters()) if ema is not None else []
ema_weight = 1.0 - args.ema ** EMA_EVERY
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
    # 一部分的畫面（預設兩成）另外有一部分（預設最多四成）的值幾乎沒收到：干擾、音訊斷續。
    # 沒收到的值訊號弱 --lost-db（預設 20 dB，也就是功率增益再乘 0.01；給的是範圍時每張畫面各抽一個）
    burst = (torch.rand(count, device=dev) < args.lost_share)[:, None, None, None]
    fraction = torch.empty(count, device=dev).uniform_(0.0, args.lost_max)[:, None, None, None]
    lost = torch.rand(full, device=dev) < fraction
    if lost_db[0] == lost_db[1]:
        weaker = 10.0 ** (-lost_db[0] / 10.0)
    else:
        weaker = torch.pow(10.0, -torch.empty(count, device=dev).uniform_(lost_db[0], lost_db[1]) / 10.0)[:, None, None, None]
    gain = torch.where(burst & lost, gain * weaker, gain)
    true_var = sigma2 / gain.clamp_min(1e-4)
    # 接收端估的雜訊有誤差：標準差 1 dB
    believed = true_var * torch.pow(10.0, torch.randn(full, device=dev) * 0.1)
    return true_var, believed


def run(x, true_var, believed, noise=None, ref=None, enc=None, dec=None):
    """畫面 → 通道 → 接收端的處理 → 解碼。enc、dec 不給就是正在訓練的模型。"""
    if ref is None:
        # 訓練時在 GPU 上取中位數（和 reference_noise 差在偶數個時取中間兩個的哪一個，可以忽略）
        ref = believed.flatten(1).median(dim=1).values.clamp(REF_LO, REF_HI)
    z = (enc_net if enc is None else enc)(x)
    z = z * torch.sqrt(0.5 / (z.pow(2).mean(dim=(1, 2, 3), keepdim=True) + 1e-9))
    if noise is None:
        noise = torch.randn_like(z)
    v, weight, snr_db = receive(z + noise * torch.sqrt(true_var / 2.0), believed, ref)
    return (dec_net if dec is None else dec)(v, snr_db, weight)


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
    # 另外一種：「少掉六成」。其餘的值 10 dB，六成的值雜訊是訊號的 100 倍（見檔案開頭）
    g = torch.Generator().manual_seed(8)
    full = (len(val),) + shape
    true_var = torch.where(torch.rand(full, generator=g) < 0.6, torch.tensor(100.0), torch.tensor(0.1))
    lost_check = (true_var.to(dev), reference_noise(true_var, model.min_snr_db).to(dev), torch.randn(full, generator=g).to(dev))


def validation_psnr(case, enc, dec):
    """一種驗證情形（雜訊變異數、參考值、雜訊）下每張圖的 PSNR。"""
    true_var, ref, noise = case
    vals = []
    for i in range(0, len(val), 200):
        sl = slice(i, i + 200)
        vals.append(psnr_uint8(run(val[sl], true_var[sl], true_var[sl], noise[sl], ref[sl], enc, dec), val[sl]).cpu())
    return torch.cat(vals)


def evaluate(enc=None, dec=None):
    """六種驗證情形的平均 PSNR。enc、dec 不給就是正在訓練的模型。"""
    model.eval()
    with torch.no_grad():
        out = {name: float(validation_psnr(case, enc, dec).mean()) for name, case in check_data.items()}
    model.train()
    return out


def evaluate_lost(enc=None, dec=None):
    """「少掉六成」那一種情形：平均 PSNR 與最差的 1%。"""
    model.eval()
    with torch.no_grad():
        q = validation_psnr(lost_check, enc, dec)
    model.train()
    return {"mean": float(q.mean()), "worst": float(torch.quantile(q, 0.01))}


# 對照的模型：同一組驗證條件下的數字，之後每次驗證都拿來比
compare = {}
compare_lost = None
if args.compare:
    from export import read_weights
    c_state, c_latent, c_id, c_arch = read_weights(args.compare)
    if c_latent != args.latent:
        raise SystemExit(f"--compare 的模型是 {c_latent} 個潛在通道，和 --latent {args.latent} 不同，不能比")
    reference = JSCC(c_latent, c_arch, widths_of(c_state))   # 對照的模型可以是別的寬度
    reference.load_state_dict(c_state)
    reference.to(dev).eval()
    compare = evaluate(reference.enc, reference.dec)
    compare_lost = evaluate_lost(reference.enc, reference.dec)
    del reference, c_state
    say(f"驗證的六種情形：awgn0／10／20 是白雜訊 0／10／20 dB，awgn99 是沒有雜訊，fade5／15 是瑞利衰落平均 5／15 dB（PSNR，dB）")
    say(f"對照的模型 {c_id:08X}  " + "  ".join(f"{k}:{v:.2f}" for k, v in compare.items()) + "　之後括號裡是和它的差距")
    say(f"另外列出「少掉六成」（其餘的值 10 dB、六成的值沒收到）的平均與最差的 1%，不算進平均差；"
        f"對照的模型是 {compare_lost['mean']:.2f}、{compare_lost['worst']:.1f}。整張壞掉的畫面會把最差的 1% 拉到 10 dB 以下")
if ema is not None:
    say("每次驗證列出的是「權重的移動平均」那一份模型（比當下的權重穩定，存成 ema.pt）；行尾另外列出當下的權重的平均值")


def psnr_text(ev, raw=None, lost=None):
    """一次驗證的結果。raw 是「當下的權重」的結果（ev 是移動平均的那一份時才給），只在行尾列出平均值。
    lost 是 evaluate_lost 的結果（和 ev 同一份模型）。"""
    if compare:
        text = "  ".join(f"{k}:{v:.2f}({v - compare[k]:+.2f})" for k, v in ev.items())
        text += f"  平均差 {sum(ev[k] - compare[k] for k in ev) / len(ev):+.2f}"
        if raw is not None:
            text += f"｜當下的權重 平均差 {sum(raw[k] - compare[k] for k in raw) / len(raw):+.2f}"
    else:
        text = "  ".join(f"{k}:{v:.2f}" for k, v in ev.items())
        if raw is not None:
            text += f"｜當下的權重 平均 {sum(raw.values()) / len(raw):.2f}"
    if lost is not None:
        text += f"｜少掉六成 {lost['mean']:.2f}"
        if compare_lost is not None:
            text += f"({lost['mean'] - compare_lost['mean']:+.2f})"
        text += f"，最差的 1% {lost['worst']:.1f}"
    return text


def hms(seconds):
    seconds = int(seconds + 0.5)
    return f"{seconds // 3600}:{seconds % 3600 // 60:02d}:{seconds % 60:02d}"


def snapshot():
    return (copy.deepcopy(model.state_dict()), copy.deepcopy(opt.state_dict()),
            copy.deepcopy(ema.state_dict()) if ema is not None else None)


def evaluate_ema():
    return evaluate(ema.enc, ema.dec) if ema is not None else None


def evaluate_lost_listed():
    """「少掉六成」：量驗證時列出來的那一份模型（有移動平均就是它，否則是當下的權重）。"""
    return evaluate_lost(ema.enc, ema.dec) if ema is not None else evaluate_lost()


def write(payload, path):
    """先寫到暫存檔再換名字：寫到一半被關掉，也不會留下壞掉的檔案。"""
    torch.save(payload, path + ".tmp")
    os.replace(path + ".tmp", path)


def save(name, step, source=None):
    """存成 evaluate.py、export.py 讀的檢查點。source 不給就是正在訓練的模型。"""
    write({"model": (model if source is None else source).state_dict(), "latent": args.latent, "architecture": args.arch,
           "widths": list(widths), "step": step}, os.path.join(args.out, name))


first = evaluate()
if resumed is None:
    say("開始之前  " + psnr_text(first, lost=evaluate_lost()))   # 移動平均的那一份此時和它完全相同
    best_metric = -math.inf          # 退回用的基準：這次執行裡最好的驗證結果
    disk_best = -math.inf            # best.pt 裡那個模型的驗證結果
    best_step, lr_scale, rollbacks, log, elapsed_before = 0, 1.0, 0, [], 0.0
else:
    first_ema = evaluate_ema()
    say(f"從第 {start_step} 步接著跑（已經跑了 {hms(resumed['seconds'])}）  "
          + (psnr_text(first, lost=evaluate_lost_listed()) if first_ema is None
             else psnr_text(first_ema, first, evaluate_lost_listed())))
    best_metric = sum(first.values()) / len(first)
    disk_best = resumed["best_metric"]
    best_step, lr_scale, rollbacks = resumed["best_step"], resumed["lr_scale"], resumed["rollbacks"]
    log, elapsed_before = resumed["log"], resumed["seconds"]
    resumed = None                   # 不再需要，讓記憶體可以釋放
best_state = snapshot()
best_state_step = start_step         # best_state 是第幾步的狀態
loss_acc = torch.zeros((), device=dev)
progress_acc = torch.zeros((), device=dev)
acc_steps = 0
progress_steps = 0
done = start_step                    # 已經完成的步數
t0 = time.time()
t_last = t0


def save_state():
    """接續用的狀態。"""
    write({"model": model.state_dict(), "opt": opt.state_dict(), "ema": ema.state_dict() if ema is not None else None,
           "step": done, "lr_scale": lr_scale, "best_metric": disk_best, "best_step": best_step, "rollbacks": rollbacks,
           "log": log, "seconds": elapsed_before + time.time() - t0, "config": config}, state_path)
    if ema is not None:
        save("ema.pt", done, ema)


try:
    for step in range(start_step, args.steps):
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
        done = step + 1
        if ema is not None and done % EMA_EVERY == 0:
            with torch.no_grad():
                for averaged, current in zip(ema_params, params):
                    averaged.lerp_(current, ema_weight)
        loss_acc += loss.detach()
        progress_acc += loss.detach()
        acc_steps += 1
        progress_steps += 1

        if step == args.debug_break_at:
            with torch.no_grad():
                model.dec.out.weight.mul_(40.0)
            say(f"（測試）第 {done} 步故意把輸出層放大 40 倍")

        checking = done % args.check_every == 0 or done == args.steps
        if args.progress_every > 0 and done % args.progress_every == 0 and not checking:
            recent = progress_acc.item() / progress_steps        # 先取值：GPU 的工作是排隊做的，取了值才算做完，之後量的時間才準
            spent = time.time() - t0
            remaining = spent / (done - start_step) * (args.steps - done)
            say(f"  步 {done:6d}/{args.steps}（{100.0 * done / args.steps:4.1f}%）  損失 {recent:.5f}  "
                  f"每步 {(time.time() - t_last) / acc_steps * 1000:5.1f} ms  已經跑了 {hms(elapsed_before + spent)}  "
                  f"還要約 {hms(remaining)}")
            progress_acc.zero_()
            progress_steps = 0

        if checking:
            mean_loss = loss_acc.item() / acc_steps
            ms_per_step = (time.time() - t_last) / acc_steps * 1000
            loss_acc.zero_()
            progress_acc.zero_()
            acc_steps = 0
            progress_steps = 0
            ev = evaluate()
            ev_ema = evaluate_ema()                                 # 在可能的退回之前量：這一行印的是這個時間點的狀態
            ev_lost = evaluate_lost_listed()
            metric = sum(ev.values()) / len(ev)
            note = ""
            if not (math.isfinite(metric) and math.isfinite(mean_loss)) or metric < best_metric - args.drop_db:
                model.load_state_dict(best_state[0])
                opt.load_state_dict(copy.deepcopy(best_state[1]))   # 載入時不複製的話，之後的更新會改到存下來的那一份
                if ema is not None:
                    ema.load_state_dict(best_state[2])              # 平均裡已經混進發散的權重，一起退回
                lr_scale *= 0.5
                rollbacks += 1
                note = f"  ← 發散，退回第 {best_state_step} 步，學習率係數 {lr_scale:g}"
            elif metric > best_metric:
                best_metric = metric
                best_state = snapshot()
                best_state_step = done
                if metric > disk_best:
                    disk_best = metric
                    best_step = done
                    save("best.pt", best_step)
            elapsed = elapsed_before + time.time() - t0
            entry = {"step": done, "loss": mean_loss, "seconds": elapsed, "lr": lr, "psnr": ev, "rolled_back": bool(note),
                     "lost60": ev_lost}
            if ev_ema is not None:
                entry["psnr_ema"] = ev_ema
            log.append(entry)
            say(f"步 {done:6d}  損失 {mean_loss:.5f}  {elapsed / 60:5.1f} 分  每步 {ms_per_step:5.1f} ms  "
                  f"學習率 {lr:.2e}  " + (psnr_text(ev, lost=ev_lost) if ev_ema is None else psnr_text(ev_ema, ev, ev_lost)) + note)
            json.dump(log, open(os.path.join(args.out, "log.json"), "w"))
            save_state()
            t_last = time.time()
except KeyboardInterrupt:
    for stop_signal in STOP_SIGNALS:
        signal.signal(stop_signal, signal.SIG_IGN)       # 存檔要一兩秒，這段時間不再理會中斷
    save_state()
    say(f"\n在第 {done} 步中斷，狀態存在 {state_path}。加上 --resume 再執行同一行指令，就會從這裡接著跑。")
    raise SystemExit(130)

final = evaluate()
final_lost = evaluate_lost()
final_ema = evaluate_ema()
final_ema_lost = evaluate_lost(ema.enc, ema.dec) if ema is not None else None
save("model.pt", args.steps)
if ema is not None:
    save("ema.pt", args.steps, ema)
minutes = (elapsed_before + time.time() - t0) / 60
json.dump({"final": final, "final_ema": final_ema, "compare": compare, "rollbacks": rollbacks, "best_step": best_step,
           "minutes": minutes, "lost60": final_lost, "lost60_ema": final_ema_lost, "lost60_compare": compare_lost},
          open(os.path.join(args.out, "final.json"), "w"))
say("最後的模型  " + psnr_text(final, lost=final_lost))
if final_ema is not None:
    say("權重的移動平均  " + psnr_text(final_ema, lost=final_ema_lost))
say(f"完成；退回 {rollbacks} 次；共 {minutes:.1f} 分。模型存在 {os.path.join(args.out, 'model.pt')}"
      + (f"，移動平均的那一份是 {os.path.join(args.out, 'ema.pt')}" if ema is not None else ""))
