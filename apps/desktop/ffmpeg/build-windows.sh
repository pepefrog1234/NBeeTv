#!/usr/bin/env bash
# 給 Windows 的下載包建置 ffmpeg.exe（nbtv-gui 用它讀影片檔、把收到的畫面存成影片）。
#
#   apps/desktop/ffmpeg/build-windows.sh <x86_64|aarch64> <輸出目錄> [工作目錄]
#
# 從官方的原始碼交叉編譯（llvm-mingw），只放 nbtv-gui 用得到的格式（見 components.txt），加上 x264（存影片用的 H.264 編碼器）、
# dav1d（AV1 的解碼器）與 zlib，全部靜態連結：一個 ffmpeg.exe，不需要別的 DLL，Windows 10 以後直接能用。
# 原始碼與工具都固定版本、核對 SHA-256；x264 沒有正式的版本，固定在 stable 分支上的一個提交。
#
# 輸出目錄裡會有：
#   ffmpeg.exe                 放在 nbtv-gui.exe 旁邊（CMake 的 NBTV_DESKTOP_BUNDLED_FFMPEG 指向這個目錄）
#   FFMPEG-LICENSE.txt         這個 ffmpeg 是什麼、怎麼建置的、授權（GPL 2.0 以後的版本）、原始碼在哪裡
#   ffmpeg-windows-source.tar  建置用到的全部原始碼：四個原始碼包、這個腳本與 components.txt。ffmpeg.exe 以 GPL 發佈，
#                              CI 發佈新版時把它和下載包放在同一個地方（GitHub Releases）
#
# 在 Linux（x86_64 或 arm64）上執行。要有 curl、git（抓 x264 用）、make、gcc（ffmpeg 建置時在這台電腦上執行的小工具要用）、nasm、meson、ninja、
# pkg-config、python3、xz：
#   sudo apt install curl git make gcc libc6-dev nasm meson ninja-build pkg-config python3 xz-utils
# macOS 上請在 Docker 的 Linux 容器裡執行。llvm-mingw 自動下載到工作目錄；要用已經解開的，設環境變數 LLVM_MINGW=那個目錄。
# 工作目錄預設是目前目錄下的 ffmpeg-windows-work：下載過的檔案留在裡面，下一次不必再下載。
#
# 升級版本：改下面的版本與 SHA-256。原始碼包的 SHA-256 是核對過發佈者的簽章之後記下的
# （FFmpeg：FFmpeg release signing key FCF986EA15E6E293A5644F10B4322F04D67658D8；dav1d：VideoLAN Release Signing Key
# 65F7C6B4206BD057A7EB73787180713BE58D1ADC；zlib：Mark Adler 5ED46A6721D365587791E2AA783FCD8E58BCAFBA）。
# 改完之後在 Windows 上跑 check.sh（CI 會跑）。
set -euo pipefail

FFMPEG_VERSION=9.1
FFMPEG_SHA256=1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433
X264_COMMIT=b35605ace3ddf7c1a5d67a2eb553f034aef41d55   # stable 分支，2025-06-08
DAV1D_VERSION=1.5.5
DAV1D_SHA256=ca96b1d0ba3f01f55de7231d775892b64994ba061aaeeaaa4f662a390b3abf9e
ZLIB_VERSION=1.4
ZLIB_SHA256=4f11c512053d7bab8d9e9b89bde3575add3e4c50a383b42ef6cb837035fc5a7c
LLVM_MINGW_VERSION=20261117
LLVM_MINGW_SHA256_x86_64=e7a15a49a2ea95e9f3d8a9d0cfabe5052a4b10fb4777cc54de5a8c27b64bb701
LLVM_MINGW_SHA256_aarch64=6d9442ead9ace3b3e4c3389bfef69f93f873305e0705e6cc60978ac895320872

FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
X264_GIT="https://code.videolan.org/videolan/x264.git"
DAV1D_URL="https://downloads.videolan.org/pub/videolan/dav1d/$DAV1D_VERSION/dav1d-$DAV1D_VERSION.tar.xz"
ZLIB_URL="https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.xz"

usage() {
    echo "用法：$0 <x86_64|aarch64> <輸出目錄> [工作目錄]" >&2
    exit 2
}
[ $# -ge 2 ] && [ $# -le 3 ] || usage
arch="$1"
case "$arch" in x86_64 | aarch64) ;; *) usage ;; esac
[ "$(uname -s)" = Linux ] || { echo "這個腳本在 Linux 上執行（macOS 上請用 Docker 的 Linux 容器）" >&2; exit 2; }
for tool in curl make gcc meson ninja pkg-config python3 xz tar sha256sum; do
    command -v "$tool" > /dev/null || { echo "缺少 $tool" >&2; exit 2; }
done
[ "$arch" != x86_64 ] || command -v nasm > /dev/null || { echo "缺少 nasm（x86_64 的組合語言要用）" >&2; exit 2; }

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$2" "${3:-ffmpeg-windows-work}/downloads"
out="$(cd "$2" && pwd)"
work="$(cd "${3:-ffmpeg-windows-work}" && pwd)"
downloads="$work/downloads"
jobs="$(nproc)"
triple="$arch-w64-mingw32"

say() { printf '\n==== %s\n' "$*"; }

# 下載一個檔案並核對 SHA-256。已經下載過、而且對得上的就不再下載。
fetch() {   # fetch <網址> <SHA-256>
    local url="$1" sum="$2" file="$downloads/${1##*/}"
    if [ -f "$file" ] && echo "$sum  $file" | sha256sum -c --status; then return; fi
    curl -fL --retry 3 --retry-delay 5 -o "$file.part" "$url"
    if ! echo "$sum  $file.part" | sha256sum -c --status; then
        echo "SHA-256 對不上：$url" >&2
        exit 1
    fi
    mv "$file.part" "$file"
}

say "下載原始碼"
fetch "$FFMPEG_URL" "$FFMPEG_SHA256"
fetch "$DAV1D_URL" "$DAV1D_SHA256"
fetch "$ZLIB_URL" "$ZLIB_SHA256"
# x264：只抓那一個提交（提交的編號本身就核對了內容），做成一個原始碼包，之後和另外三個一樣用。
# 已經有這個包（例如從發佈的原始碼那一份拿來的）就直接用，不必連到 VideoLAN。
x264_tar="$downloads/x264-$X264_COMMIT.tar.xz"
if [ ! -f "$x264_tar" ]; then
    command -v git > /dev/null || { echo "缺少 git（要從 VideoLAN 抓 x264；或把 x264-$X264_COMMIT.tar.xz 放進 $downloads）" >&2; exit 2; }
    git_dir="$work/x264-git"
    rm -rf "$git_dir"
    git init -q "$git_dir"
    git -C "$git_dir" fetch -q --depth 1 "$X264_GIT" "$X264_COMMIT"
    [ "$(git -C "$git_dir" rev-parse FETCH_HEAD)" = "$X264_COMMIT" ] || { echo "x264 拿到的不是 $X264_COMMIT" >&2; exit 1; }
    git -C "$git_dir" archive --format=tar --prefix="x264-$X264_COMMIT/" FETCH_HEAD | xz -9 > "$x264_tar.part"
    mv "$x264_tar.part" "$x264_tar"
    rm -rf "$git_dir"
fi

# llvm-mingw：給 Windows 的 Clang、LLD 與 mingw-w64（UCRT），在 Linux 上執行
if [ -n "${LLVM_MINGW:-}" ]; then
    toolchain="$LLVM_MINGW"
else
    case "$(uname -m)" in
    x86_64) host=x86_64 ;;
    aarch64 | arm64) host=aarch64 ;;
    *) echo "llvm-mingw 沒有給這種電腦的版本：$(uname -m)" >&2; exit 2 ;;
    esac
    sum_name="LLVM_MINGW_SHA256_$host"
    name="llvm-mingw-$LLVM_MINGW_VERSION-ucrt-ubuntu-22.04-$host"
    fetch "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VERSION/$name.tar.xz" "${!sum_name}"
    toolchain="$work/$name"
    [ -x "$toolchain/bin/$triple-clang" ] || tar -xJf "$downloads/$name.tar.xz" -C "$work"
fi
export PATH="$toolchain/bin:$PATH"
command -v "$triple-clang" > /dev/null || { echo "找不到 $triple-clang（llvm-mingw：$toolchain）" >&2; exit 1; }

# 每次都從頭建置：舊的建置目錄整個刪掉
build="$work/build-$arch"
prefix="$build/prefix"
rm -rf "$build"
mkdir -p "$build" "$prefix/include" "$prefix/lib/pkgconfig"
# pkg-config 只找這裡建置的函式庫，不碰執行這個腳本的電腦上的
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR

say "zlib $ZLIB_VERSION"
tar -xJf "$downloads/zlib-$ZLIB_VERSION.tar.xz" -C "$build"
make -C "$build/zlib-$ZLIB_VERSION" -f win32/Makefile.gcc -j"$jobs" PREFIX="$triple-" CC="$triple-clang" libz.a
cp "$build/zlib-$ZLIB_VERSION/zlib.h" "$build/zlib-$ZLIB_VERSION/zconf.h" "$prefix/include/"
cp "$build/zlib-$ZLIB_VERSION/libz.a" "$prefix/lib/"

say "x264 $X264_COMMIT"
# 只要 8 位元、4:2:0（存影片用的是 yuv420p）；不要命令列程式與它讀寫檔案用的函式庫
tar -xJf "$x264_tar" -C "$build"
x264_src="$build/x264-$X264_COMMIT"
mkdir -p "$build/x264"
(cd "$build/x264" && CC="$triple-clang" "$x264_src/configure" --host="$triple" --cross-prefix="$triple-" --prefix="$prefix" \
    --enable-static --disable-cli --disable-opencl --bit-depth=8 --chroma-format=420 \
    --disable-avs --disable-swscale --disable-lavf --disable-ffms --disable-gpac --disable-lsmash)
make -C "$build/x264" -j"$jobs"
make -C "$build/x264" install

say "dav1d $DAV1D_VERSION"
tar -xJf "$downloads/dav1d-$DAV1D_VERSION.tar.xz" -C "$build"
cat > "$build/cross.meson" << EOF
[binaries]
c = '$triple-clang'
cpp = '$triple-clang++'
ar = '$triple-ar'
strip = '$triple-strip'
windres = '$triple-windres'

[host_machine]
system = 'windows'
cpu_family = '$arch'
cpu = '$arch'
endian = 'little'
EOF
meson setup "$build/dav1d" "$build/dav1d-$DAV1D_VERSION" --cross-file "$build/cross.meson" --prefix "$prefix" --libdir lib \
    --buildtype release --default-library static -Denable_tools=false -Denable_tests=false -Denable_examples=false -Denable_docs=false
ninja -C "$build/dav1d" install

say "FFmpeg $FFMPEG_VERSION"
tar -xJf "$downloads/ffmpeg-$FFMPEG_VERSION.tar.xz" -C "$build"
# components.txt：一行一個選項，# 之後是說明
components=()
while IFS= read -r line; do
    line="${line%%#*}"
    line="${line//[[:space:]]/}"
    [ -n "$line" ] && components+=("$line")
done < "$here/components.txt"
# --no-insert-timestamp：執行檔裡不記連結的時間，同樣的原始碼在同樣的路徑下建置，做出來的 ffmpeg.exe 每個位元組都一樣
mkdir -p "$build/ffmpeg"
(cd "$build/ffmpeg" && "$build/ffmpeg-$FFMPEG_VERSION/configure" --prefix="$prefix" \
    --target-os=mingw32 --arch="$arch" --enable-cross-compile --cross-prefix="$triple-" --cc="$triple-clang" --cxx="$triple-clang++" \
    --pkg-config=pkg-config --pkg-config-flags=--static --extra-cflags="-I$prefix/include" --extra-ldflags="-L$prefix/lib -static -Wl,--no-insert-timestamp" \
    --extra-version=nbeetv "${components[@]}") | tee "$build/configure.log"
# 名字打錯的元件 configure 只會警告；這裡當成錯誤
if grep -i "did not match anything\|unknown option" "$build/configure.log"; then
    echo "components.txt 裡有 configure 不認得的選項" >&2
    exit 1
fi
make -C "$build/ffmpeg" -j"$jobs"

say "檢查 ffmpeg.exe"
exe="$build/ffmpeg/ffmpeg.exe"
"$triple-strip" "$exe"
# PE 檔頭裡記的處理器（Machine 欄位）：x86_64 是 0x8664，arm64 是 0xaa64。
# 不看 llvm-objdump 印的格式名稱：它把 arm64 的執行檔叫做「pe-arm-wince」
pe_offset="$(od -An -t u4 -j 60 -N 4 "$exe" | tr -d ' ')"
machine="$(od -An -t x2 -j $((pe_offset + 4)) -N 2 "$exe" | tr -d ' ')"
case "$arch" in x86_64) wanted=8664 ;; aarch64) wanted=aa64 ;; esac
[ "$machine" = "$wanted" ] || { echo "ffmpeg.exe 的處理器是 0x$machine，不是 $arch（0x$wanted）" >&2; exit 1; }
# 只能用到 Windows 本身就有的 DLL（C 執行階段是 Windows 10 以後內建的 UCRT）：別的 DLL 使用者的電腦上沒有
dlls="$("$triple-objdump" -p "$exe" | sed -n 's/^[[:space:]]*DLL Name: //p' | sort -u)"
echo "用到的 DLL：" $dlls
if echo "$dlls" | grep -v -i -E '^(kernel32|user32|advapi32|bcrypt|ole32|oleaut32|shell32|shlwapi|psapi|ntdll|ucrtbase|api-ms-win-[a-z0-9-]+)\.dll$'; then
    echo "ffmpeg.exe 用到了 Windows 本身沒有的 DLL（上面那幾個）" >&2
    exit 1
fi
ls -l "$exe"

say "輸出到 $out"
configuration="$(sed -n 's/^#define FFMPEG_CONFIGURATION "\(.*\)"$/\1/p' "$build/ffmpeg/config.h")"
cp "$exe" "$out/ffmpeg.exe"
{
    cat << EOF
FFmpeg (ffmpeg.exe) in the NBeeTv package for Windows
=====================================================

ffmpeg.exe is FFmpeg $FFMPEG_VERSION (https://ffmpeg.org) for Windows on $arch, built from the official
source code by apps/desktop/ffmpeg/build-windows.sh in the NBeeTv repository
(https://github.com/pepefrog1234/NBeeTv). It contains only the formats nbtv-gui needs
(apps/desktop/ffmpeg/components.txt) and these libraries, all statically linked:

  - x264, commit $X264_COMMIT: the H.264 encoder, used to save received pictures as video
  - dav1d $DAV1D_VERSION: the AV1 decoder
  - zlib $ZLIB_VERSION

It was compiled with llvm-mingw $LLVM_MINGW_VERSION (Clang, mingw-w64, UCRT).

nbtv-gui.exe runs it as a separate program: to read video files used as a transmit source and to save
received pictures as an .mp4 video. It is not part of NBeeTv and is not linked into NBeeTv. nbtv-gui uses
the ffmpeg.exe next to itself first; replace this file to use another FFmpeg build, or delete it to use
one from PATH.

License
-------
Because it includes x264, this ffmpeg.exe as a whole is licensed under the GNU General Public License,
version 2 or (at your option) any later version. The license texts of FFmpeg, x264, dav1d and zlib
follow below.

Source code
-----------
The complete corresponding source code (the unmodified source packages of FFmpeg, x264, dav1d and zlib,
and the build script with its configuration) is published with every NBeeTv release that contains this
program, as NBeeTv-<version>-windows-ffmpeg-source.tar next to the packages on
https://github.com/pepefrog1234/NBeeTv/releases . The source packages come from:

  $FFMPEG_URL
      SHA-256 $FFMPEG_SHA256
  $X264_GIT
      commit $X264_COMMIT
  $DAV1D_URL
      SHA-256 $DAV1D_SHA256
  $ZLIB_URL
      SHA-256 $ZLIB_SHA256

FFmpeg configuration
--------------------
$configuration

EOF
    for part in "FFmpeg - LICENSE.md:ffmpeg-$FFMPEG_VERSION/LICENSE.md" "FFmpeg - COPYING.GPLv2:ffmpeg-$FFMPEG_VERSION/COPYING.GPLv2" \
        "x264 - COPYING:x264-$X264_COMMIT/COPYING" "dav1d - COPYING:dav1d-$DAV1D_VERSION/COPYING" "zlib - LICENSE:zlib-$ZLIB_VERSION/LICENSE"; do
        title="${part%%:*}"
        file="$build/${part#*:}"
        printf '================================================================\n%s\n================================================================\n' "$title"
        cat "$file"
        printf '\n'
    done
} > "$out/FFMPEG-LICENSE.txt"

# 原始碼的那一份：四個原始碼包、這個腳本與 components.txt，外加一段說明
source_dir="$build/ffmpeg-windows-source"
mkdir -p "$source_dir"
cp "$downloads/ffmpeg-$FFMPEG_VERSION.tar.xz" "$downloads/x264-$X264_COMMIT.tar.xz" "$downloads/dav1d-$DAV1D_VERSION.tar.xz" \
    "$downloads/zlib-$ZLIB_VERSION.tar.xz" "$here/build-windows.sh" "$here/components.txt" "$source_dir/"
cat > "$source_dir/README.txt" << EOF
Source code of ffmpeg.exe in the NBeeTv packages for Windows (x86_64 and arm64).

  ffmpeg-$FFMPEG_VERSION.tar.xz     FFmpeg, unmodified ($FFMPEG_URL)
  x264-$X264_COMMIT.tar.xz
                            x264, unmodified ("git archive" of commit $X264_COMMIT
                            of $X264_GIT)
  dav1d-$DAV1D_VERSION.tar.xz       dav1d, unmodified ($DAV1D_URL)
  zlib-$ZLIB_VERSION.tar.xz          zlib, unmodified ($ZLIB_URL)
  build-windows.sh          the build script (apps/desktop/ffmpeg/ in https://github.com/pepefrog1234/NBeeTv)
  components.txt            the FFmpeg configuration it reads

To rebuild on Linux: put build-windows.sh and components.txt in one directory, copy the four packages into
<work>/downloads/, and run  ./build-windows.sh x86_64 <output> <work>  (or aarch64).
EOF
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -cf "$out/ffmpeg-windows-source.tar" -C "$build" ffmpeg-windows-source
ls -l "$out"
