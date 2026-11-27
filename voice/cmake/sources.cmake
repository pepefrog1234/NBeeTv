# 語音的編解碼器（-DNBTV_VOICE=ON）要另外取得的三份檔案：哪一個版本、從哪裡抓、SHA-256 是多少。
# voice/CMakeLists.txt（建置時）與 voice/cmake/fetch_sources.cmake（先抓好放著）都讀這個檔，版本只寫在這一個地方。
#
# RADE V2 在 2026 年底還是測試版，波形與權重之後可能再改。換版本時改這裡的提交編號與雜湊值，並重跑 voice/tests；
# Opus 要用 rade_c 指定的那一個提交（它的 cmake/BuildOpus.cmake），模型資料要用那個提交的 autogen.sh 指定的那一份。

# rade_c：RADE（Radio Autoencoder）的 C 實作，權重編在程式裡，原始碼一百多 MB（BSD 兩條款）
set(NBTV_RADE_COMMIT 23c3746e488dd4825178b4abb857106a5f61a4e6)  # 2026-11-20
set(NBTV_RADE_SHA256 eae99fbdba40ee9f51dfff4afe97006bf27782489ca76ee5c0dcc36c1eeed8c9)
set(NBTV_RADE_URL "https://github.com/freedv/rade_c/archive/${NBTV_RADE_COMMIT}.zip")
set(NBTV_RADE_FILE "rade_c-${NBTV_RADE_COMMIT}.zip")

# Opus：語音的特徵（LPCNet）與合成（FARGAN）在它裡面（BSD 三條款）
set(NBTV_OPUS_COMMIT 940d4e5af64351ca8ba8390df3f555484c567fbb)
set(NBTV_OPUS_SHA256 20e37f9079ac2b80e3235cd8ce2547829e147bf44a2f5dd28a888fa3e9c24341)
set(NBTV_OPUS_URL "https://github.com/xiph/opus/archive/${NBTV_OPUS_COMMIT}.zip")
set(NBTV_OPUS_FILE "opus-${NBTV_OPUS_COMMIT}.zip")

# Opus 的模型資料（權重不在它的原始碼儲存庫裡，約 184 MB）；檔名裡的就是它的 SHA-256
set(NBTV_OPUS_MODEL_SHA256 4ed9445b96698bad25d852e912b41495ddfa30c8dbc8a55f9cde5826ed793453)
set(NBTV_OPUS_MODEL_URL "https://media.xiph.org/opus/models/opus_data-${NBTV_OPUS_MODEL_SHA256}.tar.gz")
set(NBTV_OPUS_MODEL_FILE "opus_data-${NBTV_OPUS_MODEL_SHA256}.tar.gz")
