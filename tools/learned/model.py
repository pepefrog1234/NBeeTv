# 學出來的影像「訊源—通道聯合編碼」：96×72 的畫面 ↔ 通道上的實數值（每兩個組成一個格點的 I 與 Q）。
# 編碼器不知道對方的訊雜比（發射端無從得知）；解碼器知道（接收機本來就會估雜訊變異數）。
import math

import torch
import torch.nn as nn
import torch.nn.functional as F

H, W = 72, 96


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

    def __init__(self, latent_ch, widths=(32, 64, 128, 192), blocks=2):
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
    """latent_ch×9×12（加了雜訊）＋訊雜比 → 3×72×96"""

    def __init__(self, latent_ch, widths=(32, 64, 128, 192), blocks=2):
        super().__init__()
        self.snr_embed = nn.Sequential(nn.Linear(1, 64), nn.GELU(), nn.Linear(64, 64), nn.GELU())
        self.inp = nn.Conv2d(latent_ch, widths[3], 1)
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

    def forward(self, z, snr_db):
        e = self.snr_embed((snr_db[:, None] - 12.0) / 12.0)
        x = self.inp(z)
        for stage, films, up in zip(self.blocks, self.films, self.ups):
            for block, film in zip(stage, films):
                g, b = film(e).chunk(2, dim=1)
                x = block(x, (g, b))
            x = up(F.gelu(x))
        for block in self.tail:
            x = block(x)
        return self.out(F.gelu(x))


class JSCC(nn.Module):
    def __init__(self, latent_ch):
        super().__init__()
        self.latent_ch = latent_ch
        self.enc = Encoder(latent_ch)
        self.dec = Decoder(latent_ch)

    @property
    def real_dims(self):
        return self.latent_ch * (H // 8) * (W // 8)

    def encode(self, x):
        """回傳功率正規化後的通道值：每個實數的均方值是 1/2，也就是每個複數格點的平均功率是 1。"""
        z = self.enc(x)
        power = z.pow(2).mean(dim=(1, 2, 3), keepdim=True)
        return z * torch.sqrt(0.5 / (power + 1e-9))

    def forward(self, x, snr_db, noise=None):
        z = self.encode(x)
        sigma2 = torch.pow(10.0, -snr_db / 10.0)                      # 複數格點的雜訊變異數
        if noise is None:
            noise = torch.randn_like(z)
        z_noisy = z + noise * torch.sqrt(sigma2 / 2.0)[:, None, None, None]
        return self.dec(z_noisy, snr_db)


def psnr_uint8(x, y):
    """兩批 [0,1] 的影像量化成 8 位元後的 PSNR（每張一個值）。"""
    a = torch.round(x.clamp(0, 1) * 255.0)
    b = torch.round(y.clamp(0, 1) * 255.0)
    mse = (a - b).pow(2).mean(dim=(1, 2, 3))
    return 10.0 * torch.log10(255.0 * 255.0 / mse.clamp_min(1e-9))
