# 從靜態的小圖合成幾張連續的畫面（短片），訓練「接收端合併最近幾幀」用（train_merge.py）。
# 專案裡沒有影片的訓練資料；這裡用 COCO 切出來的小圖做出幾種常見的情形：
#   靜止（只加一點鏡頭雜訊）、鏡頭平移／推近／轉動、前景的一塊（取自另一張圖）在畫面上移動、中途換鏡頭，以及這些的組合。
# 超出小圖邊界的地方用鏡射補。全部在訓練用的裝置上做（MPS 沒有雙三次取樣，那一步放到 CPU）。
# 這只是對真實畫面的猜測；真實影片上的效果要另外量（做法見 README.md）。
import math

import torch
import torch.nn.functional as F

H, W = 72, 96


def affine_sample(img, shift_x, shift_y, zoom, angle):
    """img [N,3,H,W]。把內容向右移 shift_x、向下移 shift_y 個像素，放大 zoom 倍，轉 angle 弧度（各是 [N]）。
    取樣落到邊界外時用鏡射補。位移是整數、不縮放也不轉的時候，取到的正好是原來的像素，不會變糊。"""
    n = img.shape[0]
    cos, sin = torch.cos(angle) / zoom, torch.sin(angle) / zoom
    theta = torch.zeros(n, 2, 3, device=img.device)
    theta[:, 0, 0] = cos
    theta[:, 0, 1] = -sin * (H / W)
    theta[:, 1, 0] = sin * (W / H)
    theta[:, 1, 1] = cos
    sx, sy = 2.0 * shift_x / W, 2.0 * shift_y / H            # 正規化座標裡一個像素是 2/W、2/H
    theta[:, 0, 2] = -(theta[:, 0, 0] * sx + theta[:, 0, 1] * sy)
    theta[:, 1, 2] = -(theta[:, 1, 0] * sx + theta[:, 1, 1] * sy)
    grid = F.affine_grid(theta, list(img.shape), align_corners=False)
    if img.device.type == "mps":
        # MPS 沒有雙三次的取樣：這一步放到 CPU 上做（影像很小，每步只多幾毫秒）
        return F.grid_sample(img.cpu(), grid.cpu(), mode="bicubic", padding_mode="reflection",
                             align_corners=False).clamp(0.0, 1.0).to(img.device)
    return F.grid_sample(img, grid, mode="bicubic", padding_mode="reflection", align_corners=False).clamp(0.0, 1.0)


def make_clips(x, pictures, gen=None, static_only=False, motion=1.0, cut_share=0.08):
    """x [B,3,H,W]（0–1）→ 短片 [B,K,3,H,W]。gen 是亂數產生器（驗證用的短片要固定）；static_only 只做靜止的。
    motion 是運動的幅度：1 是相鄰兩張隔 0.68 秒時的設定，隔得愈近給得愈小（隔 0.17 秒給 0.25）。
    cut_share 是中途換鏡頭的短片佔幾成。"""
    b, dev = x.shape[0], x.device

    def rand(*shape):
        return torch.rand(*shape, device=dev, generator=gen)

    def uniform(lo, hi, *shape):
        return lo + (hi - lo) * rand(*shape)

    steps = torch.arange(pictures, device=dev, dtype=torch.float32) - float((pictures - 1) // 2)   # K=4：−1、0、1、2
    zero = torch.zeros(b, device=dev)
    # 鏡頭的運動：三成五的短片有。其中一半是整數像素的平移（不變糊），一半另外帶縮放、轉動與一點晃動
    moving = (rand(b) < 0.35) & (not static_only)
    sharp = rand(b) < 0.5
    small = rand(b) < 0.6                                       # 六成是慢的（每張 3 個像素以內），其餘到 10 個像素
    speed = torch.where(small, uniform(0.0, 3.0, b), uniform(3.0, 10.0, b)) * motion
    direction = uniform(0.0, 2.0 * math.pi, b)
    vx, vy = speed * torch.cos(direction), speed * torch.sin(direction) * 0.75
    vx = torch.where(sharp, vx.round(), vx) * moving
    vy = torch.where(sharp, vy.round(), vy) * moving
    zoom_rate = torch.where(moving & ~sharp, uniform(-0.03, 0.03, b), zero) * motion
    turn_rate = torch.where(moving & ~sharp, uniform(-0.026, 0.026, b), zero) * motion   # motion 是 1 時每張最多 1.5 度
    shake = (moving & ~sharp).float()
    # 前景：五成五的短片有一塊取自另一張圖的橢圓形區域在動
    has_object = (rand(b) < 0.55) & (not static_only)
    other = torch.randperm(b, device=dev, generator=gen)
    cx, cy = uniform(0.0, W, b), uniform(0.0, H, b)
    rx, ry = uniform(8.0, 30.0, b), uniform(8.0, 30.0, b)
    tilt = uniform(0.0, math.pi, b)
    ox, oy = uniform(-12.0, 12.0, b) * motion, uniform(-9.0, 9.0, b) * motion
    object_zoom = uniform(-0.03, 0.03, b) * motion
    ys = torch.arange(H, device=dev, dtype=torch.float32)[None, :, None] + 0.5
    xs = torch.arange(W, device=dev, dtype=torch.float32)[None, None, :] + 0.5

    frames = []
    for k in range(pictures):
        t = steps[k]
        jitter_x = uniform(-0.5, 0.5, b) * shake * motion ** 0.5
        jitter_y = uniform(-0.5, 0.5, b) * shake * motion ** 0.5
        back = affine_sample(x, vx * t + jitter_x, vy * t + jitter_y, torch.exp(zoom_rate * t), turn_rate * t)
        fore = affine_sample(x[other], ox * t, oy * t, torch.exp(object_zoom * t), zero)
        dx = xs - (cx + ox * t)[:, None, None]
        dy = ys - (cy + oy * t)[:, None, None]
        cos, sin = torch.cos(tilt)[:, None, None], torch.sin(tilt)[:, None, None]
        size = torch.exp(object_zoom * t)[:, None, None]
        u = (cos * dx + sin * dy) / (rx[:, None, None] * size)
        v = (-sin * dx + cos * dy) / (ry[:, None, None] * size)
        edge = (1.0 - torch.sqrt(u * u + v * v)) * torch.minimum(rx, ry)[:, None, None] / 1.5 + 0.5   # 邊緣約 1.5 個像素寬
        alpha = edge.clamp(0.0, 1.0)[:, None] * has_object[:, None, None, None]
        frames.append(alpha * fore + (1.0 - alpha) * back)
    clips = torch.stack(frames, dim=1)

    # 換鏡頭：一部分的短片（cut_share，預設百分之八）從某一張起換成另一段短片
    if not static_only:
        cut = rand(b) < cut_share
        cut_at = 1 + (rand(b) * (pictures - 1)).long().clamp(max=pictures - 2)
        partner = torch.randperm(b, device=dev, generator=gen)
        later = torch.arange(pictures, device=dev)[None, :] >= cut_at[:, None]
        swap = (cut[:, None] & later)[:, :, None, None, None]
        clips = torch.where(swap, clips[partner], clips)
    # 鏡頭雜訊：七成的短片每張各加一份，標準差 0 到 2 個灰階
    noisy = (rand(b) < 0.7).float()[:, None, None, None, None]
    sigma = uniform(0.0, 2.0 / 255.0, b)[:, None, None, None, None]
    noise = torch.randn(clips.shape, device=dev, generator=gen)
    return (clips + noisy * sigma * noise).clamp(0.0, 1.0)
