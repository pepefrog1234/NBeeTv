#!/usr/bin/env bash
# 檢查一個 ffmpeg 做不做得到 nbtv-gui 要它做的事。CI 在 Windows 上對著下載包裡的 ffmpeg.exe 跑；別的 ffmpeg 也可以拿來試。
#   1. samples/ 裡各種格式的影片：用和 nbtv-gui 播放影片時一樣的參數（apps/desktop/src/sources.cpp 的 decode_loop）解出第一格，
#      大小要對（手機直拍的要轉正），而且是 8 位元的 PPM（10 位元的影片也是）
#   2. 說明文件裡的用法：把照片轉成 96×72 的 PPM
#   3. 存影片：和 nbtv-gui 一樣（apps/desktop/src/app.cpp 的 export_sequence）從管線送 RGB 的畫面，放大、貼上印字的那一塊，
#      編成 H.264 的 .mp4；再讀回來。不給輸出檔時 ffmpeg 印的資訊裡要有長度與大小（沒有 ffprobe 時 nbtv-gui 靠這個）
#
#   apps/desktop/ffmpeg/check.sh <ffmpeg 執行檔> [工作目錄]
set -euo pipefail

[ $# -ge 1 ] && [ $# -le 2 ] || { echo "用法：$0 <ffmpeg 執行檔> [工作目錄]" >&2; exit 2; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ffmpeg="$1"
work="${2:-$(mktemp -d)}"
mkdir -p "$work"
failed=0
ok() { echo "可以：$*"; }
bad() {
    echo "不行：$*" >&2
    failed=1
}

"$ffmpeg" -hide_banner -version | head -1

# PPM 的檔頭「P6\n寬 高\n最大值\n」→「寬x高 最大值」
ppm_header() { head -n 3 "$1" 2> /dev/null | tr '\n' ' ' | awk '$1 == "P6" { print $2 "x" $3, $4 }'; }
# nbtv-gui 播放影片時的參數（從 0 秒開始，只取第一格）
first_frame() { "$ffmpeg" -nostdin -hide_banner -loglevel error -ss 0.000 -i "$1" -an -sn -dn \
    -vf fps=10,scale=512:512:force_original_aspect_ratio=decrease -frames:v 1 -pix_fmt rgb24 -f image2pipe -c:v ppm - > "$2"; }

# 1. 各種格式的影片
while read -r file size _; do
    case "$file" in "" | \#*) continue ;; esac
    out="$work/${file%.*}.ppm"
    rm -f "$out"
    first_frame "$here/samples/$file" "$out" || true
    got="$(ppm_header "$out")"
    if [ "$got" = "$size 255" ]; then ok "讀 $file（$size）"; else bad "讀 $file：要 $size 的 8 位元 PPM，得到「$got」"; fi
done < "$here/samples/expected.txt"

# 2. 把照片轉成 PPM（說明文件裡的寫法）
rm -f "$work/photo-96x72.ppm"
"$ffmpeg" -nostdin -hide_banner -loglevel error -i "$here/samples/photo.jpg" -vf scale=96:72 -pix_fmt rgb24 "$work/photo-96x72.ppm" || true
got="$(ppm_header "$work/photo-96x72.ppm")"
if [ "$got" = "96x72 255" ]; then ok "把照片轉成 96×72 的 PPM"; else bad "把照片轉成 PPM：得到「$got」"; fi

# 3. 存影片：送過去的一格是「96×72 的畫面 ｜ 64×32 的那一塊」，每秒 24 格送 1 秒
movie="$work/movie.mp4"
rm -f "$movie"
graph="[0:v]split[picture][corner];[picture]crop=96:72:0:0,scale=960:720:flags=neighbor[large];[corner]crop=64:32:96:0[stamp];[large][stamp]overlay=8:8[out]"
head -c $((160 * 72 * 3 * 24)) /dev/zero | "$ffmpeg" -y -nostdin -hide_banner -loglevel error -f rawvideo -pix_fmt rgb24 -s 160x72 -r 24 -i pipe:0 \
    -filter_complex "$graph" -map "[out]" -pix_fmt yuv420p -movflags +faststart "$movie" || true
info="$("$ffmpeg" -hide_banner -nostdin -i "$movie" 2>&1 || true)"
if echo "$info" | grep -q "Duration: 00:00:01.00" && echo "$info" | grep -E -q "Video: h264 .*960x720"; then
    ok "存成 H.264 的 .mp4（960×720、1 秒）"
else
    bad "存影片：ffmpeg 印的資訊是「$info」"
fi
rm -f "$work/movie.ppm"
first_frame "$movie" "$work/movie.ppm" || true
got="$(ppm_header "$work/movie.ppm")"
if [ "$got" = "512x384 255" ]; then ok "讀回存好的影片"; else bad "讀回存好的影片：得到「$got」"; fi

if [ $failed -ne 0 ]; then
    echo "有不行的項目（工作目錄：$work）" >&2
    exit 1
fi
echo "全部可以"
