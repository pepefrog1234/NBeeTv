# 接收端把最近幾幀合起來解：決定「前面幾幀要合進來多少」的小網路（Gate），以及合併本身（fuse）。
# 發射端（編碼器）、空中格式、解碼器都不動；用的是現成的單張模型。C++ 的實作在 core/src/learned_merge.cpp，
# 兩邊要一一對應：改了這裡，那邊和權重檔的結構編號（export_merge.py）都要跟著改。
#
# 合併的算法是固定的：照各幀的雜訊變異數加權平均（愈吵的權重愈小），前面幾幀的權重再乘上一個 0 到 1 的「閘」。
# 畫面沒變的地方，合 4 幀等於雜訊少了 6 dB；畫面變了的地方，閘關起來，只用這一幀。閘全關時和不合併完全一樣。
# 沒收到的幀用旗標（have）表示：它的輸入全部是 0、不參與合併。
import math

import torch
import torch.nn as nn
import torch.nn.functional as F

STRUCTURE = 1         # 權重檔的結構編號；Gate 的輸入或算法改了就換一個數字


class Gate(nn.Module):
    """決定前面幾幀要合進來多少。兩個部分：
    1. 估計「前面第 j 幀和這一幀的值差多少」（每個位置一個數：差距平方的平均，取以 10 為底的對數）。
       接收端看得到的只有收到的值和各自的雜訊：兩幀的值如果一樣，差的平方平均起來就是兩邊雜訊的和，多出來的就是畫面變了。
       訓練時知道真正的差距，直接拿它當答案來學（不經過解碼器）。
    2. 由估計的差距和雜訊的大小決定每個值的閘（0 到 1）。起點是簡單的規則：閘 = 雜訊 ÷（雜訊 + 8 × 差距），
       再由一個很小的網路修正；這一部分用畫面的誤差來學。"""

    def __init__(self, latent, frames, width=128, depth=2, rule_width=32):
        super().__init__()
        self.latent, self.frames, self.width, self.depth, self.rule_width = latent, frames, width, depth, rule_width
        inputs = (2 * latent + 7) * (frames - 1) + latent      # 每個前面的幀：2C 個逐值的量、6 個合起來的量、1 個「有沒有這一幀」
        layers = [nn.Conv2d(inputs, width, 3, padding=1), nn.GELU()]
        for _ in range(depth - 1):                              # depth 是 3×3 卷積的層數（預設 2）
            layers += [nn.Conv2d(width, width, 3, padding=1), nn.GELU()]
        layers.append(nn.Conv2d(width, frames - 1, 1))
        self.estimator = nn.Sequential(*layers)
        nn.init.constant_(self.estimator[-1].bias, -1.0)
        self.rule = nn.Sequential(nn.Linear(4, rule_width), nn.GELU(), nn.Linear(rule_width, rule_width), nn.GELU(), nn.Linear(rule_width, 1))
        nn.init.zeros_(self.rule[-1].weight)
        nn.init.zeros_(self.rule[-1].bias)

    def estimate(self, y, var, have=None):
        """y、var：[B,K,C,9,12]，第 0 個是現在這一幀，第 j 個是 j 幀之前的。have：[B,K-1]，前面第 j 幀有沒有收到
        （沒收到的那一幀，y 與 var 填什麼都可以，這裡不會用到）。回傳 [B,K-1,9,12]：log10（差距平方的平均）。"""
        y0, v0 = y[:, 0], var[:, 0]
        yp, vp = y[:, 1:], var[:, 1:]
        if have is None:
            have = torch.ones(yp.shape[:2], dtype=torch.bool, device=y.device)
        here = have[:, :, None, None, None]
        yp = torch.where(here, yp, torch.zeros_like(yp))
        vp = torch.where(here, vp, torch.ones_like(vp))
        d2 = (y0[:, None] - yp).pow(2)
        noise = 0.5 * (v0[:, None] + vp)                       # 兩邊雜訊變異數的和（每個實數的雜訊變異數是 σ²/2）
        m = d2.mean(dim=2)                                     # [B,J,9,12]
        nz = noise.mean(dim=2)
        e = m - nz                                             # 這個位置差距平方的估計：平均起來是對的，但很吵
        e_box = F.avg_pool2d(e, 3, stride=1, padding=1, count_include_pad=False)
        e_all = e.mean(dim=(2, 3), keepdim=True).expand_as(e)
        n_all = nz.mean(dim=(2, 3), keepdim=True).expand_as(e)

        def squash(v):                                         # 把大小差很多的數壓到差不多的範圍，保留正負
            return torch.asinh(v / 0.02) / 4.0

        pooled = torch.stack([torch.log10(m + 1e-4), torch.log10(nz), squash(e), squash(e_box), squash(e_all), torch.log10(n_all)], dim=2)
        value = torch.cat([torch.log1p(d2 / noise), -torch.log10(vp) / 3.0], dim=2)
        # 沒收到的那一幀：它的每一個輸入都是 0，最後一個旗標也是 0
        flag = here.float().expand(-1, -1, 1, 9, 12)
        slot = torch.cat([value, pooled], dim=2) * here.float()
        feats = torch.cat([torch.cat([slot, flag], dim=2).flatten(1, 2), -torch.log10(v0) / 3.0], dim=1)
        return self.estimator(feats).clamp(-4.0, 0.7)

    def gates(self, raw, var, have=None):
        """raw：estimate 的結果。回傳閘 [B,K-1,C,9,12]；沒收到的幀是 0。"""
        v0, vp = var[:, 0], var[:, 1:]
        if have is None:
            have = torch.ones(vp.shape[:2], dtype=torch.bool, device=var.device)
        here = have[:, :, None, None, None]
        vp = torch.where(here, vp, torch.ones_like(vp))
        ls = torch.log10(vp / 2.0)                             # 前面那一幀每個值的雜訊（log10）
        l0 = torch.log10(v0 / 2.0)[:, None].expand_as(ls)
        ld = raw[:, :, None].expand_as(ls)
        ld_all = raw.mean(dim=(2, 3), keepdim=True)[:, :, None].expand_as(ls)
        simple = math.log(10.0) * (ls - ld - math.log10(8.0))  # 簡單規則：雜訊 ÷（雜訊 + 8 × 差距）
        feats = torch.stack([ld, ls, l0, ld_all], dim=-1) / 2.0
        return torch.sigmoid(simple + self.rule(feats).squeeze(-1)) * here.float()

    def forward(self, y, var, have=None):
        return self.gates(self.estimate(y, var, have), var, have)


def fuse(y, var, g):
    """照雜訊變異數加權平均，前面幾幀的權重乘上閘 g。回傳（合併後的值，它的雜訊變異數），都是 [B,C,9,12]。
    閘是 0 的地方（包括沒收到的幀）完全不影響結果，那裡的 y、var 填什麼都可以（var 要大於 0）。"""
    y0, v0 = y[:, 0], var[:, 0]
    yp, vp = y[:, 1:], var[:, 1:]
    w0 = 1.0 / v0
    live = g > 0
    yp = torch.where(live, yp, torch.zeros_like(yp))
    wp = torch.where(live, g / vp.clamp_min(1e-12), torch.zeros_like(g))
    total = w0 + wp.sum(dim=1)
    fused = (w0 * y0 + (wp * yp).sum(dim=1)) / total
    fused_var = (w0 + (g * wp).sum(dim=1)) / total.pow(2)       # Σ 權重²×變異數 ÷（Σ 權重）²
    return fused, fused_var


def pick_device(name=""):
    if name:
        return torch.device(name)
    if torch.cuda.is_available():
        return torch.device("cuda")
    if torch.backends.mps.is_available():
        return torch.device("mps")
    return torch.device("cpu")


def load_gate(path, device="cpu"):
    """讀訓練存下來的檢查點（train_merge.py 的 ema.pt／gate.pt）。回傳（Gate，檢查點的內容）。"""
    ckpt = torch.load(path, map_location="cpu")
    if ckpt.get("structure") != STRUCTURE:
        raise SystemExit(f"{path} 不是這個版本的閘（結構編號不符）")
    gate = Gate(ckpt["latent"], ckpt["frames"], ckpt["width"], ckpt.get("depth", 2), ckpt.get("rule_width", 32))
    gate.load_state_dict(ckpt["gate"])
    return gate.to(device).eval(), ckpt
