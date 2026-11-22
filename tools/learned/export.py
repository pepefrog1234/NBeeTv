# 把訓練好的檢查點匯出成 C++ 核心讀的權重檔（core/src/learned_codec.cpp），並產生測試用的參考答案。
#
# 權重檔的格式（位元組順序一律小端）：
#    0   8  魔術字 "NBTVLRN1"
#    8   4  格式版本（1）
#   12   4  結構編號（見 model.py：1 是只吃訊雜比的解碼器，2 是另外吃「各個值可不可靠」的解碼器）
#   16   4  潛在通道數（72 → 一張畫面是 72×9×12 = 7776 個實數）
#   20   4  張量個數
#   24   4  數值型別：0 = float32、1 = float16
#   28   4  保留（0）
#   之後每個張量：名稱長度（uint16）、名稱（ASCII，和 PyTorch 的 state_dict 相同）、維度數（uint8）、
#                 各維度（uint32）、資料（PyTorch 的排列）
#   最後 4 位元組：前面所有位元組的 CRC-32（和 zlib 相同）。這個值也是模型的識別碼，收發兩端必須相同。
#
# 預設存成 float16：第一版的權重在 800 張測試小圖、0／10／20 dB／無雜訊下的 PSNR 和 float32 到小數第三位都相同，
# 檔案小一半。（每個輸出通道各自量化成 int8 的話再小一半，但 PSNR 會掉 0.02–0.14 dB。）
#
# 參考答案檔（給 core/tests/test_learned.cpp）：
#    0   8  魔術字 "NBTVLRF2"
#    8   4  對應權重檔的識別碼
#   12   4  影像張數 N
#   16   4  每張的實數個數 L
#   之後每張影像：
#     通道值 float32[L]                編碼器的輸出
#     可靠程度 float32[L]              測試用的一組 0–1 之間的值（固定的亂數）
#     解碼結果 uint8[72×96×3]          輸入是通道值、可靠程度全部是 1、訊雜比 10 dB
#     解碼結果 uint8[72×96×3]          輸入是通道值 × 可靠程度、連同那組可靠程度、訊雜比 5 dB
#   全部在 CPU 上以 float32 計算，用的是從權重檔讀回來的權重。
#
# 用法：python3 export.py 檢查點.pt 權重檔.bin [--float32] [--reference 參考答案.bin 測試圖.ppm …]
#       檢查點也可以是另一份權重檔（.bin）：用來換存檔的型別，或把結構 1 的權重轉成結構 2（多出來的輸入接 0）。
import argparse
import os
import struct
import zlib

import numpy as np
import torch

from model import JSCC

MAGIC = b"NBTVLRN1"
REF_MAGIC = b"NBTVLRF2"
FORMAT_VERSION = 1


def write_weights(path, state, latent, half, architecture):
    body = bytearray(MAGIC)
    body += struct.pack("<6I", FORMAT_VERSION, architecture, latent, len(state), 1 if half else 0, 0)
    for name, value in state.items():
        raw = name.encode("ascii")
        arr = value.detach().cpu().float().numpy()
        body += struct.pack("<H", len(raw)) + raw + struct.pack("<B", arr.ndim) + struct.pack(f"<{arr.ndim}I", *arr.shape)
        body += arr.astype("<f2" if half else "<f4").tobytes()
    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
    with open(path, "wb") as f:
        f.write(bytes(body) + struct.pack("<I", crc))
    return crc


def read_weights(path):
    """權重檔 → (state_dict（float32）, 潛在通道數, 識別碼, 結構編號)。也可以用來把權重檔載回 PyTorch。"""
    data = open(path, "rb").read()
    if len(data) < 36 or data[:8] != MAGIC:
        raise ValueError("不是權重檔")
    crc = struct.unpack("<I", data[-4:])[0]
    if zlib.crc32(data[:-4]) & 0xFFFFFFFF != crc:
        raise ValueError("檢查碼不符")
    version, architecture, latent, count, dtype, _ = struct.unpack("<6I", data[8:32])
    if version != FORMAT_VERSION or architecture not in (1, 2) or dtype not in (0, 1):
        raise ValueError("不認得的版本、結構或數值型別")
    kind, width = ("<f2", 2) if dtype == 1 else ("<f4", 4)
    pos, state = 32, {}
    for _ in range(count):
        n = struct.unpack("<H", data[pos:pos + 2])[0]
        name = data[pos + 2:pos + 2 + n].decode("ascii")
        pos += 2 + n
        ndim = data[pos]
        shape = struct.unpack(f"<{ndim}I", data[pos + 1:pos + 1 + 4 * ndim])
        pos += 1 + 4 * ndim
        size = int(np.prod(shape))
        state[name] = torch.from_numpy(np.frombuffer(data, kind, size, pos).astype(np.float32).reshape(shape).copy())
        pos += size * width
    if pos != len(data) - 4:
        raise ValueError("長度不符")
    return state, latent, crc, architecture


def read_ppm(path):
    parts = open(path, "rb").read().split(b"\n", 3)       # "P6"、"96 72"、"255"、像素
    if parts[0] != b"P6" or parts[1].split() != [b"96", b"72"] or parts[2] != b"255":
        raise ValueError(f"{path} 不是 96×72 的 PPM")
    return np.frombuffer(parts[3], np.uint8, 72 * 96 * 3).reshape(72, 96, 3)


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse <= 0 else 10.0 * np.log10(255.0 * 255.0 / mse)


def to_rgb(y):
    return (y.clamp(0, 1) * 255.0).round().byte()[0].permute(1, 2, 0).contiguous().numpy()


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("checkpoint")
    p.add_argument("weights")
    p.add_argument("--float32", action="store_true")
    p.add_argument("--architecture", type=int, default=0)   # 0：照檢查點的；2：把結構 1 的權重轉成結構 2
    p.add_argument("--reference", nargs="+", metavar="檔名")
    args = p.parse_args()

    if args.checkpoint.endswith(".bin"):
        source, source_latent, _, source_arch = read_weights(args.checkpoint)
    else:
        ckpt = torch.load(args.checkpoint, map_location="cpu")
        source, source_latent, source_arch = ckpt["model"], ckpt["latent"], ckpt.get("architecture", 1)
    architecture = args.architecture or source_arch
    source = {k: v.detach().cpu().float() for k, v in source.items()}
    if architecture == 2 and source_arch == 1:
        old = source["dec.inp.weight"]
        source["dec.inp.weight"] = torch.cat([old, torch.zeros_like(old)], dim=1)   # 「可靠程度」那一半接 0

    half = not args.float32
    model_id = write_weights(args.weights, source, source_latent, half, architecture)
    state, latent, crc, arch = read_weights(args.weights)   # 讀回來用，之後算的和 C++ 看到的是同一份數值
    assert crc == model_id and latent == source_latent and arch == architecture and list(state) == list(source)
    worst = max(float((state[k] - v).abs().max()) for k, v in source.items())
    print(f"已寫入 {args.weights}：結構 {architecture}、{len(state)} 個張量、{sum(v.numel() for v in state.values())} 個參數、"
          f"{'float16' if half else 'float32'}、{os.path.getsize(args.weights)} 位元組；識別碼 {model_id:08X}")
    print(f"存檔捨入造成的最大權重誤差 {worst:.2e}")

    if args.reference:
        out_path, images = args.reference[0], args.reference[1:]
        model = JSCC(latent, architecture)
        model.load_state_dict(state)
        model.eval()
        count = model.real_dims
        blob = bytearray(REF_MAGIC + struct.pack("<3I", model_id, len(images), count))
        gen = torch.Generator().manual_seed(1)
        awgn_all, fade_all = [], []
        with torch.no_grad():
            for path in images:
                rgb = read_ppm(path)
                x = torch.from_numpy(rgb.copy()).permute(2, 0, 1)[None].float().div(255.0)
                z = model.encode(x)
                weight = torch.rand(z.shape, generator=gen).pow(0.5)                    # 0–1 之間，偏向 1
                blob += z.flatten().numpy().astype("<f4").tobytes()
                blob += weight.flatten().numpy().astype("<f4").tobytes()
                plain = to_rgb(model.dec(z, torch.tensor([10.0]), torch.ones_like(z)))
                weighted = to_rgb(model.dec(z * weight, torch.tensor([5.0]), weight))
                blob += plain.tobytes() + weighted.tobytes()
                clean = psnr(to_rgb(model.dec(z, torch.tensor([30.0]), torch.ones_like(z))), rgb)
                # 測試拿來當統計對照的兩個數字（各 32 次）：10 dB 白雜訊，以及平均 10 dB 的瑞利衰落
                awgn, fade = [], []
                for _ in range(32):
                    sigma2 = torch.full(z.shape, 10.0 ** (-10.0 / 10.0))
                    awgn.append(psnr(to_rgb(model.decode(z + torch.randn(z.shape, generator=gen) * (sigma2 / 2).sqrt(), sigma2)), rgb))
                    faded = sigma2 / torch.empty(z.shape).exponential_(1.0, generator=gen).clamp_min(1e-4)
                    fade.append(psnr(to_rgb(model.decode(z + torch.randn(z.shape, generator=gen) * (faded / 2).sqrt(), faded)), rgb))
                awgn_all.append(float(np.mean(awgn)))
                fade_all.append(float(np.mean(fade)))
                print(f"  {os.path.basename(path):<12s} 通道值均方 {float(z.pow(2).mean()):.6f}；不加雜訊（訊雜比輸入 30 dB）{clean:.3f} dB；"
                      f"10 dB 白雜訊平均 {awgn_all[-1]:.3f} dB；平均 10 dB 的瑞利衰落 {fade_all[-1]:.3f} dB")
        with open(out_path, "wb") as f:
            f.write(bytes(blob))
        print(f"已寫入 {out_path}：{len(images)} 張、{len(blob)} 位元組；10 dB 白雜訊總平均 {np.mean(awgn_all):.3f} dB、"
              f"瑞利衰落總平均 {np.mean(fade_all):.3f} dB")
