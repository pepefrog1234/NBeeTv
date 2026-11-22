# 學出來的影像「訊源—通道聯合編碼」：96×72 的畫面 ↔ 通道上的實數值（每兩個組成一個格點的 I 與 Q）。
# 編碼器不知道對方的訊雜比（發射端無從得知）；解碼器知道（接收機本來就會估雜訊變異數）。
#
# 兩種結構（權重檔的「結構編號」）：
#   1  解碼器只吃收到的值與一個訊雜比。訓練時每個值的雜訊一樣大。
#      衰落通道下有一部分的值等於沒收到，這個解碼器沒見過缺值，會在畫面上畫出不存在的黑塊與點陣。
#   2  解碼器另外吃一張「各個值可不可靠」的圖（0 到 1）。訓練時各個值的雜訊不一樣大（衰落、干擾），
#      接收端先把太吵的值壓小（receive），解碼器知道哪些值被壓過，缺的地方就由周圍補，而不是當成 0 來畫。
import math

import torch
import torch.nn as nn
import torch.nn.functional as F

H, W = 72, 96
MAX_SNR_DB = 30.0
MIN_SNR_DB = {1: -2.0, 2: -6.0}   # 各結構的解碼器認得的最低訊雜比（訓練時涵蓋到這裡）
# 解析度每減半一次的通道數。models/ 裡的權重都是這一組，C++ 的推論（core/src/learned_codec.cpp）也只認得這一組；
# 別的寬度（train.py --widths）目前只能在 PyTorch 裡訓練與評估，用來試「模型加大有沒有幫助」。
DEFAULT_WIDTHS = (32, 64, 128, 192)


class ResBlock(nn.Module):
    def __init__(self, ch):
        super().__init__()
        self.c1 = nn.Conv2d(ch, ch, 3, padding=1)
        self.c2 = nn.Conv2d(ch, ch, 3, padding=1)

    def forward(self, x, film=None):
        y = self.c1(F.gelu(x))
        if film is not None:
            gamma, beta = film
            y = y * (1 + gamma[:, :, None, None]) + beta[:, :, None, None]
        y = self.c2(F.gelu(y))
        return x + y


class Encoder(nn.Module):
    """3×72×96 → latent_ch×9×12"""

    def __init__(self, latent_ch, widths=DEFAULT_WIDTHS, blocks=2):
        super().__init__()
        self.stem = nn.Conv2d(3, widths[0], 5, padding=2)
        stages = []
        for i in range(3):
            stages.append(nn.Conv2d(widths[i], widths[i + 1], 3, stride=2, padding=1))
            stages.extend(ResBlock(widths[i + 1]) for _ in range(blocks))
        self.stages = nn.Sequential(*stages)
        self.head = nn.Conv2d(widths[3], latent_ch, 1)

    def forward(self, x):
        return self.head(F.gelu(self.stages(self.stem(x))))


class Decoder(nn.Module):
    """latent_ch×9×12（收到的值）＋訊雜比 → 3×72×96。reliability 為真時，另外吃同樣形狀的「可靠程度」。"""

    def __init__(self, latent_ch, widths=DEFAULT_WIDTHS, blocks=2, reliability=False):
        super().__init__()
        self.reliability = reliability
        self.snr_embed = nn.Sequential(nn.Linear(1, 64), nn.GELU(), nn.Linear(64, 64), nn.GELU())
        self.inp = nn.Conv2d(latent_ch * (2 if reliability else 1), widths[3], 1)
        self.blocks = nn.ModuleList()
        self.films = nn.ModuleList()
        self.ups = nn.ModuleList()
        ch = widths[3]
        for i in range(3):
            stage = nn.ModuleList(ResBlock(ch) for _ in range(blocks))
            self.blocks.append(stage)
            self.films.append(nn.ModuleList(nn.Linear(64, 2 * ch) for _ in range(blocks)))
            nxt = widths[2 - i]
            self.ups.append(nn.Sequential(nn.Conv2d(ch, nxt * 4, 3, padding=1), nn.PixelShuffle(2)))
            ch = nxt
        self.tail = nn.ModuleList(ResBlock(ch) for _ in range(blocks))
        self.out = nn.Conv2d(ch, 3, 5, padding=2)

    def forward(self, z, snr_db, weight=None):
        e = self.snr_embed((snr_db[:, None] - 12.0) / 12.0)
        if self.reliability:
            if weight is None:
                weight = torch.ones_like(z)
            z = torch.cat([z, weight], dim=1)
        x = self.inp(z)
        for stage, films, up in zip(self.blocks, self.films, self.ups):
            for block, film in zip(stage, films):
                g, b = film(e).chunk(2, dim=1)
                x = block(x, (g, b))
            x = up(F.gelu(x))
        for block in self.tail:
            x = block(x)
        return self.out(F.gelu(x))


def reference_noise(noise_var, min_snr_db, max_snr_db=MAX_SNR_DB):
    """告訴解碼器的雜訊變異數（每張畫面一個）：各個值的雜訊變異數的中位數，夾在解碼器認得的範圍裡。
    和 core/src/learned_frame.cpp 的 learned_picture 一樣取排序後的第 n/2 個。在 CPU 上算。"""
    flat = noise_var.flatten(1).cpu()
    ref = torch.kthvalue(flat, flat.shape[1] // 2 + 1, dim=1).values
    return ref.clamp(10.0 ** (-max_snr_db / 10.0), 10.0 ** (-min_snr_db / 10.0))


def receive(y, noise_var, ref):
    """接收端交給解碼器之前的處理（和 learned_picture 相同）。
    y、noise_var 是 [B,C,9,12]：收到的值與它所在格點的複數雜訊變異數；ref 是 [B]：reference_noise 的結果。
    每個實數的訊號變異數是 1/2、雜訊變異數是 σ²/2，最小均方誤差的權重是 1/(1+σ²)。比參考值吵的值壓到相對於
    參考值的權重，其餘不動。回傳（壓過的值，權重，告訴解碼器的訊雜比 dB）。"""
    ref = ref.to(y.device)[:, None, None, None]
    weight = ((1.0 + ref) / (1.0 + noise_var)).clamp(max=1.0)
    return y * weight, weight, -10.0 * torch.log10(ref[:, 0, 0, 0])


class JSCC(nn.Module):
    def __init__(self, latent_ch, architecture=1, widths=DEFAULT_WIDTHS):
        super().__init__()
        self.latent_ch = latent_ch
        self.architecture = architecture
        self.widths = tuple(int(w) for w in widths)
        self.min_snr_db = MIN_SNR_DB[architecture]
        self.enc = Encoder(latent_ch, self.widths)
        self.dec = Decoder(latent_ch, self.widths, reliability=architecture >= 2)

    @property
    def real_dims(self):
        return self.latent_ch * (H // 8) * (W // 8)

    def encode(self, x):
        """回傳功率正規化後的通道值：每個實數的均方值是 1/2，也就是每個複數格點的平均功率是 1。"""
        z = self.enc(x)
        power = z.pow(2).mean(dim=(1, 2, 3), keepdim=True)
        return z * torch.sqrt(0.5 / (power + 1e-9))

    def decode(self, y, noise_var, ref=None):
        """y：收到的值；noise_var：接收端認為各個值的雜訊變異數（形狀同 y）。"""
        if ref is None:
            ref = reference_noise(noise_var, self.min_snr_db)
        v, weight, snr_db = receive(y, noise_var, ref)
        return self.dec(v, snr_db, weight)

    def forward(self, x, snr_db, noise=None):
        """每個值的雜訊一樣大（白雜訊）時的整條路：snr_db 是 [B]。量測用。"""
        z = self.encode(x)
        sigma2 = torch.pow(10.0, -snr_db / 10.0)[:, None, None, None].expand_as(z)   # 複數格點的雜訊變異數
        if noise is None:
            noise = torch.randn_like(z)
        return self.decode(z + noise * torch.sqrt(sigma2 / 2.0), sigma2)


def psnr_uint8(x, y):
    """兩批 [0,1] 的影像量化成 8 位元後的 PSNR（每張一個值）。"""
    a = torch.round(x.clamp(0, 1) * 255.0)
    b = torch.round(y.clamp(0, 1) * 255.0)
    mse = (a - b).pow(2).mean(dim=(1, 2, 3))
    return 10.0 * torch.log10(255.0 * 255.0 / mse.clamp_min(1e-9))
