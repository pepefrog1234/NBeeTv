#!/usr/bin/env bash
# 做出 check.sh 用的測試影片：每個 1 秒、96×72、每秒 10 格的測試圖，各用一種常見的格式。
# 要用功能齊全的 ffmpeg（例如 Homebrew 的）：附在下載包裡的那一個沒有這些編碼器。做好的檔案已經在這個目錄裡，平常不必再做。
#   apps/desktop/ffmpeg/samples/make.sh [ffmpeg]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
ffmpeg="${1:-ffmpeg}"
run() { SVT_LOG=1 "$ffmpeg" -y -nostdin -hide_banner -loglevel error "$@"; }
src=(-f lavfi -i testsrc2=size=96x72:rate=10:duration=1)
run "${src[@]}" -c:v libx264 -pix_fmt yuv420p rotated.tmp.mp4
run -display_rotation 90 -i rotated.tmp.mp4 -c copy h264-rotated.mp4   # 像手機直拍：畫面是橫的，檔案裡記著要轉 90 度
rm rotated.tmp.mp4
run "${src[@]}" -c:v libx265 -x265-params log-level=error -tag:v hvc1 -pix_fmt yuv420p hevc.mp4
run "${src[@]}" -c:v libx265 -x265-params log-level=error -tag:v hvc1 -pix_fmt yuv420p10le hevc-10bit.mp4   # 像 iPhone 的 HDR 影片
run "${src[@]}" -c:v libvpx-vp9 -b:v 0 -crf 40 vp9.webm
run "${src[@]}" -c:v libsvtav1 -pix_fmt yuv420p av1.mp4
run "${src[@]}" -c:v mpeg4 -vtag xvid -q:v 4 mpeg4.avi
run "${src[@]}" -c:v mpeg2video -q:v 4 -f mpegts mpeg2.ts
run "${src[@]}" -c:v wmv2 -q:v 4 wmv2.wmv
run "${src[@]}" -c:v prores_ks -profile:v proxy prores.mov
run -f lavfi -i testsrc2=size=320x240:rate=1 -frames:v 1 -q:v 3 photo.jpg
ls -l
