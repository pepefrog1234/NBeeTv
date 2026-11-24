# 把訓練好的「合併最近幾幀」的小網路（train_merge.py 的 ema.pt）匯出成 C++ 核心讀的權重檔
# （core/src/learned_merge.cpp），並產生測試用的參考答案。
#
# 權重檔的格式和畫面模型的（export.py）一樣，只有魔術字與兩個欄位的意義不同（位元組順序一律小端）：
#    0   8  魔術字 "NBTVMRG1"
#    8   4  格式版本（1）
#   12   4  結構編號（merge.py 的 STRUCTURE）
#   16   4  潛在通道數（和它配的畫面模型相同）
#   20   4  張量個數
#   24   4  數值型別：0 = float32、1 = float16
#   28   4  它配的畫面模型的識別碼（不同模型送出來的值互不相通，接收機用這個數對應）
#   之後每個張量：名稱長度（uint16）、名稱（ASCII，和 PyTorch 的 state_dict 相同）、維度數（uint8）、
#                 各維度（uint32）、資料（PyTorch 的排列）
#   最後 4 位元組：前面所有位元組的 CRC-32。
#   一共看幾幀、網路的寬度與層數不另外記：讀的一方由張量的形狀看出來。
#
# 檔名是模型的名稱（「訓練完成的年月日-當天第幾個完成的」），和畫面的模型放在一起（models/）。
#
# 參考答案檔（給 core/tests/test_learned.cpp）：
#    0   8  魔術字 "NBTVMRF1"
#    8   4  這個權重檔的識別碼
#   12   4  它配的畫面模型的識別碼
#   16   4  情形的個數 N
#   20   4  每一幀的實數個數 L
#   24   4  一共看幾幀 K
#   之後每個情形：
#     uint32                       前面的幀有沒有收到：第 j−1 個位元是 1 表示 j 幀之前的那一幀有
#     float32[K][L]                各幀收到的值（第 0 個是現在這一幀，第 j 個是 j 幀之前的；沒收到的填 0）
#     float32[K][L]                各個值的雜訊變異數（沒收到的填 1）
#     float32[L]                   合併後的值
#     float32[L]                   合併後的雜訊變異數
#   全部在 CPU 上以 float32 計算，用的是從權重檔讀回來的權重。
#
# 用法：python3 export_merge.py 檢查點.pt 權重檔.bin --model 畫面模型.bin [--float32] [--reference 參考答案.bin 測試圖.ppm …]
import argparse
import os
import struct
import zlib

import numpy as np
import torch

from export import read_ppm, read_weights
from merge import STRUCTURE, Gate, fuse
from model import JSCC, psnr_uint8, widths_of

MAGIC = b"NBTVMRG1"
REF_MAGIC = b"NBTVMRF1"
FORMAT_VERSION = 1


def write_merge(path, state, latent, half, model_id):
    body = bytearray(MAGIC)
    body += struct.pack("<6I", FORMAT_VERSION, STRUCTURE, latent, len(state), 1 if half else 0, model_id)
    for name, value in state.items():
        raw = name.encode("ascii")
        arr = value.detach().cpu().float().numpy()
        body += struct.pack("<H", len(raw)) + raw + struct.pack("<B", arr.ndim) + struct.pack(f"<{arr.ndim}I", *arr.shape)
        body += arr.astype("<f2" if half else "<f4").tobytes()
    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
    with open(path, "wb") as f:
        f.write(bytes(body) + struct.pack("<I", crc))
    return crc


def read_merge(path):
    """權重檔 →（Gate、識別碼、它配的畫面模型的識別碼）。權重是從檔案讀回來的（float32）。"""
    data = open(path, "rb").read()
    if len(data) < 36 or data[:8] != MAGIC:
        raise ValueError("不是合併最近幾幀用的權重檔")
    crc = struct.unpack("<I", data[-4:])[0]
    if zlib.crc32(data[:-4]) & 0xFFFFFFFF != crc:
        raise ValueError("檢查碼不符")
    version, structure, latent, count, dtype, model_id = struct.unpack("<6I", data[8:32])
    if version != FORMAT_VERSION or structure != STRUCTURE or dtype not in (0, 1):
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
    # 幀數、寬度、層數由張量的形狀看出來（和 C++ 的 learned_merge.cpp 相同的算法）
    first = state["estimator.0.weight"]
    width_, inputs = int(first.shape[0]), int(first.shape[1])
    frames = (inputs - latent) // (2 * latent + 7) + 1
    depth = sum(1 for k, v in state.items() if k.startswith("estimator.") and k.endswith(".weight") and v.shape[2] == 3)
    gate = Gate(latent, frames, width_, depth, int(state["rule.0.weight"].shape[0]))
    gate.load_state_dict(state)
    return gate.eval(), crc, model_id


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("checkpoint")
    p.add_argument("weights")
    p.add_argument("--model", required=True)                 # 它配的畫面模型（.bin）
    p.add_argument("--float32", action="store_true")
    p.add_argument("--reference", nargs="+", metavar="檔名")
    args = p.parse_args()

    model_state, model_latent, model_id, model_arch = read_weights(args.model)
    ckpt = torch.load(args.checkpoint, map_location="cpu")
    if ckpt.get("structure") != STRUCTURE:
        raise SystemExit("檢查點不是這個版本的閘（結構編號不符）")
    if ckpt["latent"] != model_latent or ckpt.get("model_id") != model_id:
        raise SystemExit(f"這個檢查點是配另一個畫面模型訓練的（識別碼 {ckpt.get('model_id', 0):08X}，--model 的是 {model_id:08X}）")
    source = {k: v.detach().cpu().float() for k, v in ckpt["gate"].items()}
    half = not args.float32
    merge_id = write_merge(args.weights, source, model_latent, half, model_id)
    gate, crc, tagged = read_merge(args.weights)             # 讀回來用，之後算的和 C++ 看到的是同一份數值
    assert crc == merge_id and tagged == model_id and gate.frames == ckpt["frames"] and gate.width == ckpt["width"]
    state = gate.state_dict()
    worst = max(float((state[k] - v).abs().max()) for k, v in source.items())
    K = gate.frames
    print(f"已寫入 {args.weights}：一共看 {K} 幀、{len(state)} 個張量、{sum(v.numel() for v in state.values())} 個參數、"
          f"{'float16' if half else 'float32'}、{os.path.getsize(args.weights)} 位元組；識別碼 {merge_id:08X}，配畫面模型 {model_id:08X}")
    print(f"存檔捨入造成的最大權重誤差 {worst:.2e}")

    if args.reference:
        out_path, images = args.reference[0], args.reference[1:]
        if len(images) < 3:
            raise SystemExit("--reference 要給至少三張測試圖")
        model = JSCC(model_latent, model_arch, widths_of(model_state))
        model.load_state_dict(model_state)
        model.eval()
        L = model.real_dims
        gen = torch.Generator().manual_seed(1)
        with torch.no_grad():
            pictures = [torch.from_numpy(read_ppm(path).copy()).permute(2, 0, 1)[None].float().div(255.0) for path in images]
            z = [model.encode(x) for x in pictures]                                    # 每張圖的通道值 [1,C,9,12]
        a, b, c = 0, 1, 2

        def white(snr_db):
            return lambda: torch.full(z[0].shape, 10.0 ** (-snr_db / 10.0))

        def faded(snr_db):
            return lambda: 10.0 ** (-snr_db / 10.0) / torch.empty(z[0].shape).exponential_(1.0, generator=gen).clamp_min(1e-4)

        # 每個情形：說明、各幀是哪一張圖（第 0 個是現在這一幀；None 表示沒收到）、各幀的雜訊
        cases = [
            ("同一張圖連續 %d 幀，白雜訊 0 dB" % K, [a] * K, [white(0.0)] * K),
            ("同一張圖，白雜訊 10 dB，兩幀之前的那一幀沒收到", [a, a, None] + [a] * (K - 3), [white(10.0)] * K),
            ("每一幀都是不同的圖，白雜訊 5 dB", [a, b, c] + [a] * (K - 3), [white(5.0)] * K),
            ("兩幀之前換過圖，白雜訊 10 dB", [b, b] + [c] * (K - 2), [white(10.0)] * K),
            ("同一張圖，瑞利衰落平均 5 dB", [c] * K, [faded(5.0)] * K),
            ("同一張圖，這一幀 0 dB、前面幾幀 15 dB", [b] * K, [white(0.0)] + [white(15.0)] * (K - 1)),
            ("前面的幀都沒收到", [c] + [None] * (K - 1), [white(5.0)] * K),
            ("同一張圖，白雜訊 25 dB", [a] * K, [white(25.0)] * K),
        ]
        blob = bytearray(REF_MAGIC + struct.pack("<5I", merge_id, model_id, len(cases), L, K))
        with torch.no_grad():
            for text, which, noises in cases:
                y = torch.zeros(1, K, *z[0].shape[1:])
                var = torch.ones_like(y)
                have = torch.zeros(1, K - 1, dtype=torch.bool)
                for j in range(K):
                    if which[j] is None:
                        continue
                    v = noises[j]()
                    y[:, j] = z[which[j]] + torch.randn(z[0].shape, generator=gen) * (v / 2).sqrt()
                    var[:, j] = v
                    if j > 0:
                        have[0, j - 1] = True
                g = gate(y, var, have)
                merged, merged_var = fuse(y, var, g)
                mask = sum(1 << j for j in range(K - 1) if bool(have[0, j]))
                blob += struct.pack("<I", mask)
                blob += y.flatten().numpy().astype("<f4").tobytes() + var.flatten().numpy().astype("<f4").tobytes()
                blob += merged.flatten().numpy().astype("<f4").tobytes() + merged_var.flatten().numpy().astype("<f4").tobytes()
                before = float(psnr_uint8(model.decode(y[:, 0], var[:, 0]), pictures[which[0]]))
                after = float(psnr_uint8(model.decode(merged, merged_var), pictures[which[0]]))
                print(f"  {text}：閘平均 {float(g.mean()):.3f}；不合併 {before:.2f} dB，合併 {after:.2f} dB（{after - before:+.2f}）")
        with open(out_path, "wb") as f:
            f.write(bytes(blob))
        print(f"已寫入 {out_path}：{len(cases)} 個情形、{len(blob)} 位元組")
