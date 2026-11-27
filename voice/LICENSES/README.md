# 語音模組用到的第三方程式

`-DNBTV_VOICE=ON` 的建置會把下面兩份別人的程式連進執行檔（預設的建置沒有；GitHub 上 0.2.1 之後的下載包有）。它們的授權和這個專案的
GPL-3.0 相容；散佈含有它們的執行檔時要附上這裡的條文。建置已經照做了：macOS 的 app 放在 app 的 Resources 裡，
桌面 app 與命令列工具的安裝（下載包）放在 `THIRD-PARTY-LICENSES.txt` 裡。

| 程式 | 來源 | 授權 | 這裡的檔案 |
| --- | --- | --- | --- |
| rade_c：RADE（Radio Autoencoder）的 C 實作與它的權重 | <https://github.com/freedv/rade_c> | BSD 兩條款 | `rade_c-LICENSE.txt`（儲存庫的授權檔）、`rade_c-source-notice.txt`（各個原始檔開頭的聲明，年份各檔不同） |
| Opus：語音的特徵（LPCNet）與合成（FARGAN） | <https://github.com/xiph/opus> | BSD 三條款 | `opus-COPYING.txt` |

用的是哪一個提交寫在 `voice/cmake/sources.cmake`。這個專案只多了接它們的一層（`voice/src/`）。建置時對它們的原始碼
有兩處修改（都在 `voice/CMakeLists.txt` 裡，每一處的理由寫在那裡）：Opus 的 `dnn/nnet.c` 把類神經網路層的輸入上限加大
（rade_c 自己要求的，它的儲存庫裡有同樣內容的補丁）；Windows 上 rade_c 的 `rade_api.h` 拿掉給動態函式庫用的匯出標記。
