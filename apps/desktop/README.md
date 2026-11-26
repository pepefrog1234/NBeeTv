# Windows 與 Linux 的桌面 app（nbtv-gui）

把 [macOS 的 app](../macos/README.md) 搬到 Windows 與 Linux：同樣的版面、同樣的收發流程，一個執行檔。
視窗、音訊裝置、鏡頭、開檔存檔的對話框用 [SDL 3](https://libsdl.org)，介面上的元件用 [Dear ImGui](https://github.com/ocornut/imgui) 自己畫
（顏色、尺寸照 macOS 版，不是系統原生的元件）。調變解調、畫面的編解碼全部交給核心，而且和 macOS 版一樣只透過
`core/include/nbtv/nbtv.h` 這一個 C API 使用核心。

![nbtv-gui 的主視窗，正在接收](../../docs/images/desktop-receiving.zh-Hant.png)

> 這個 app 是新的。它的每一步都有自動檢查（見[檢查](#檢查)），但還沒有人在真的 Windows、Linux 電腦上接著電台用過：
> 真的音效卡、鏡頭、序列埠的 PTT 都還沒有實機驗證。哪些驗證過、哪些沒有，列在[驗證過與還沒驗證的](#驗證過與還沒驗證的)。

## 執行

下載包解開之後，`nbtv-gui`（Windows 是 `nbtv-gui.exe`）和 `models/` 在同一個資料夾，直接執行。

- **Windows**：執行檔沒有程式碼簽章，第一次執行時 SmartScreen 可能會擋下來；確認檔案是從這個 repo 下載的之後，按「其他資訊 → 仍要執行」。
  不需要另外安裝 Visual C++ 的可轉散發套件。
- **Linux**：需要 glibc 2.35 以上（Ubuntu 22.04、Debian 12、Raspberry Pi OS bookworm 以後）。視窗用 X11 或 Wayland，
  聲音用 PipeWire、PulseAudio 或 ALSA，系統上有哪一個就用哪一個，不必另外裝東西。
  介面的中文、日文要靠系統的字型（例如 Noto Sans CJK：`sudo apt install fonts-noto-cjk`）；找不到的話介面會改用英文顯示。

第一次使用：

1. 按右上角的設定鈕，填呼號、選音訊的輸入與輸出裝置、選 PTT 的控制方式。
2. 回到主視窗就已經在接收了。右側欄選發射來源（鏡頭、影片檔或圖片序列）與發射模式，按「開始發射」。
   影片檔、圖片，或裝著圖片的資料夾也可以直接拖進視窗。

設定存在系統給這個 app 的目錄裡：Windows 是 `%APPDATA%\NBeeTv\nbtv-gui\settings.txt`，Linux 是 `~/.local/share/NBeeTv/nbtv-gui/settings.txt`。
接上電台之前要注意的事（濾波器、ALC、頻率差）和 macOS 版相同，見 [macOS 版的說明](../macos/README.md#接上電台之前)。

### 要另外安裝才有的功能

| 功能 | 需要 | 沒有裝的時候 |
| --- | --- | --- |
| 用影片檔當發射來源、把收到的畫面存成影片 | [ffmpeg](https://ffmpeg.org)（在 `PATH` 裡，或和 `nbtv-gui` 放在一起；有 `ffprobe` 更好） | 圖片檔（JPEG、PNG、BMP、GIF）與圖片序列照樣能當來源；存影片的按鈕不能按 |
| 用電台的 CAT 指令控制 PTT，或列出電台型號 | [Hamlib](https://github.com/Hamlib/Hamlib) 的 `rigctld`、`rigctl`（Windows 裝官方的安裝程式；Linux 例如 `sudo apt install libhamlib-utils`） | 另外三種 PTT（不控制、序列埠的 RTS／DTR、連到已經開著的 rigctld）不受影響 |

這個程式自己不帶影片的解碼器，也不帶 Hamlib。

### 命令列的參數

```text
nbtv-gui                             開視窗
nbtv-gui --demo                      開視窗，裡面是示範資料，不碰任何裝置（看版面用）
nbtv-gui --selftest [--only 名稱]      不開視窗、不碰任何真的裝置，逐項檢查；有失敗的項目時結束碼是 1
nbtv-gui --snapshot 目錄 [--lang 語言]   不開視窗，把六張版面各算成一張 PNG（語言：zh-Hant、zh-Hans、ja、es、en、all）
nbtv-gui --version
  --settings 檔案      設定存在這個檔案，不用預設的位置
  --lang 語言          這一次用這個語言（開視窗時也會記進設定）
  --report 檔案        --selftest、--snapshot 印的內容也寫一份進這個檔案
  --quit-after 秒數    開視窗之後過這麼久自己結束
```

Windows 上這是視窗程式，從檔案總管啟動時沒有地方印字；`--selftest` 的結果要看，請從命令提示字元執行，或加 `--report`。

環境變數：`NBTV_LANGUAGE`（當成系統的語言，例如 `ja`）、`NBTV_MODEL_DIR`（權重檔放在哪裡；預設找執行檔旁邊的 `models/`）、
`NBTV_NO_DEVICES`（設了就不開音訊輸入與鏡頭）。

## 和 macOS 版不一樣的地方

收發的流程（什麼時候按下 PTT、怎麼接下一張、怎麼收尾、定時發射）是照 macOS 版一步一步搬過來的，下面是不一樣的地方：

- **第一次啟動時發射來源是「影片檔」**（還沒選檔），不是鏡頭：不會一打開就把鏡頭開起來。
- **圖片也能當來源**：「影片檔」可以選圖片，那一張會一直重送。定時發射的來源是圖片時沒有「播完」，改成每次連續送 30 秒。
  要照順序送很多張、每張送一次的話，用「圖片序列」。
- **圖片序列**的行為照 macOS 版（選一批圖片或一個資料夾、照檔名排、每張送一次、送完自動停止、沒送出去的那一張下次再送，
  見 [macOS 版的說明](../macos/README.md)裡的「用圖片序列發射」），不一樣的只有這幾處：
  - 讀得進來的圖片是 JPEG、PNG、BMP、GIF（程式自己讀，不靠系統）。HEIC、TIFF、WebP、相機的 RAW 讀不了，選到了會被當成不是圖片而略過。
    照檔案裡記載的方向轉正只認 JPEG 的 EXIF（手機直拍的相片就是這一種）。「影片檔」那裡選的圖片也一樣會轉正。
  - 系統的開檔對話框一次只能選檔案或只能選資料夾，所以按「選擇…」會先問是「選擇圖片檔…」（可以多選）還是「選擇資料夾…」。
    也可以把好幾張圖片或一個資料夾直接拖進視窗；只拖一張圖片的話，來源已經是圖片序列時才算換一批，不然照舊當成在「影片檔」那裡選了它。
  - 檔名的順序是程式自己排的：數字照大小（`2.jpg` 在 `10.jpg` 前面）、英文字母不分大小寫，其他的字照字碼。
    中文、日文或帶重音的檔名，排出來的順序和檔案總管照語言排的不一定一樣。
  - 相片是整張解開之後再縮小的（macOS 版在解碼時就縮），大相片讀起來比較久：4032×3024 的測試圖在開發用的 Mac 上讀一張約 0.05 秒，
    真的相片（檔案比較大）與比較慢的電腦會更久，沒有量過。預覽的下一張會先讀好，發射中輪到它時不必等。
- **存成影片時印上訊雜比的選項**在按了「儲存影片」之後跳出來的一小塊裡，在那裡按「儲存…」才開存檔的對話框
  （macOS 版把它放在系統的存檔對話框裡；這裡的存檔對話框也是系統的，但沒有辦法往裡面加東西）。勾不勾一樣會記住，預設不印。
  字用系統的等寬粗體字型、由 stb_truetype 畫，筆畫和 macOS 版略有不同；找不到字型時影片照存，底列會寫沒有印上。
- **影片靠使用者電腦上的 ffmpeg**：播放時開一個 ffmpeg 把畫面照實際的速度送過來（每秒 10 格，先縮小到 512 點以內），
  暫停就把它結束、記住位置。macOS 版用的是系統的 AVFoundation。
- **介面的語言可以在設定裡選**（macOS 版只跟著系統）。預設跟著系統的語言。
- **設定是蓋在主視窗上的對話框**，不是另一個視窗；選單也是 app 自己畫的。沒有選單列與鍵盤快速鍵（Esc 可以關掉選單與設定）。
- **畫布的字**用系統的粗體字型、由 stb_truetype 畫，所以同一句字的筆畫和 macOS 版、和別台電腦會有一點不同（排法與字級的規則相同）。
- **放開 PTT 之前的靜音是 0.25 秒**（macOS 版 0.1 秒）：不同系統的音效卡手上沒播完的聲音長短不一，留寬一點。
- **同一台電腦同時開兩個**（兩部電台互相收發）：macOS 版要另外建一個 NBTV 2.app；這裡第二個加上 `--settings 另一個檔案` 就有自己的一份設定。
- **沒有搬過來的**：開發用的除錯紀錄（DebugTap）、電腦進入睡眠時自動停止發射。
  定時發射開著的時候會請系統不要因為閒置而睡眠，但使用者自己讓電腦睡眠的話，發射中的那一張不會先收尾。
- **鏡頭的方向**不做修正：畫面照鏡頭給的樣子。
- **沒有模式 15，也沒有模式 5、6、7**（給 VHF／UHF 的 FM 語音通道用的 192×144 大畫面，macOS 版有）：這裡的畫面還是固定 96×72。
  選單裡不列這幾個模式；收到它們的幀時顯示「模型不符」，沒有畫面。

## 建置

需要 CMake 3.20 以上、支援 C++20 的編譯器。設定（configure）時要連得上網路：SDL 3.4.20、Dear ImGui 1.93.0、stb 的兩個標頭檔會在這一步下載，
版本固定，並核對 SHA-256；之後都是靜態連結進執行檔。

```sh
cmake -S . -B build-desktop -DNBTV_BUILD_DESKTOP_APP=ON -DNBTV_BUILD_TESTS=OFF
cmake --build build-desktop --config Release --target nbtv-gui
```

產物是 `build-desktop/apps/desktop/nbtv-gui`（Visual Studio 是 `build-desktop/apps/desktop/Release/nbtv-gui.exe`）。
從建置目錄直接執行時，權重檔用的是專案的 `models/`。要做成可以整包搬走的資料夾：設定時加 `-DNBTV_INSTALL_FLAT=ON`，再
`cmake --install build-desktop --config Release --prefix <目錄>`。

- **Linux**：SDL 要有各種視窗、音訊系統的標頭檔才會把對應的功能編進去。Debian／Ubuntu：

  ```sh
  sudo apt install g++ cmake ninja-build pkg-config \
    libasound2-dev libpulse-dev libpipewire-0.3-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
    libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev libgles2-mesa-dev libgl1-mesa-dev libdrm-dev libgbm-dev \
    libdbus-1-dev libudev-dev libibus-1.0-dev
  ```

  這些只有建置時要；做出來的執行檔在執行時才載入系統上有的那幾個，直接相依的只有 glibc
  （`-DNBTV_DESKTOP_STATIC_RUNTIME=ON`，預設就是：把 libstdc++ 連進執行檔）。
- **Windows**：Visual Studio 2022 以上（MSVC）。要讓使用者不必裝 Visual C++ 的可轉散發套件，設定時加 `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`。
- **macOS** 上也編得起來（開發時就是在那裡試的），但發佈給 macOS 的是 [apps/macos](../macos/README.md) 那一個。
- 已經裝了 SDL 3.2 以上、不想再編一次：`-DNBTV_DESKTOP_SYSTEM_SDL3=ON`。

GitHub 上的下載包就是 [`.github/workflows/build.yml`](../../.github/workflows/build.yml) 照這個方式在各個平台上建出來的，`nbtv-gui` 和命令列工具放在同一包。

## 介面的文字

和 macOS 版一樣：原始碼裡給使用者看的文字是繁體中文（同時是每一句的索引），其他四種語言的翻譯表在建置時編進執行檔。
大部分的句子直接沿用 macOS 版的 [`Localizable.strings`](../macos/Localization)；桌面版自己多出來、或 macOS 版的說法裡寫著「Mac」的那幾句放在
[`Localization/`](Localization)。建置時 [`tools/gen_strings.cpp`](tools/gen_strings.cpp) 會檢查程式裡的每一句在四種語言都有翻譯、
要填的值（`%@`、`%d`）對得上，少一句就建不過。`--selftest`、`--snapshot` 這些開發用的輸出不翻譯，一律是繁體中文。

## 原始碼

| 檔案（`src/`） | 內容 | 對應 macOS 版的 |
| --- | --- | --- |
| `main.cpp` | 進入點：參數、視窗、事件迴圈、放大倍率、語言 | `main.swift`、`NBTVApp.swift` |
| `app.hpp/.cpp` | 介面的狀態與收發流程：接收、發射的排程、來源、定時發射 | `AppModel.swift`、`RadioEngine.swift` |
| `ui.hpp/.cpp` | 整個介面：主視窗、設定、發射模式選單、定時發射的面板 | `MainWindow.swift`、`ReceivePanels.swift`、`SidePanel.swift`、`SettingsWindow.swift`、`ModeMenu.swift`、`ScheduleMenu.swift`、`Theme.swift` |
| `core_bridge.hpp/.cpp` | 核心 C API 的 C++ 包裝、模式與模型的清單 | `CoreBridge.swift`、`Modes.swift` |
| `audio.hpp/.cpp` | 音訊裝置的清單、收音、發射用的輸出佇列 | `AudioDevices.swift`、`AudioIO.swift` |
| `sources.hpp/.cpp` | 發射來源：鏡頭、影片檔（ffmpeg）、圖片、圖片序列的檔案（哪些是圖片、怎麼排、照方向轉正、縮小）；縮成 96×72 | `VideoSources.swift`、`ImageSequence.swift` |
| `canvas.hpp/.cpp` | 畫布：印在送出畫面上的字；存成影片時印在每一張左上角的訊雜比 | `Canvas.swift`、`SequenceVideo.swift` |
| `ptt.hpp/.cpp`、`serial.hpp/.cpp`、`net.hpp/.cpp` | PTT 的四種控制方式、序列埠的控制線、對 rigctld 的 TCP 連線 | `PTT.swift` |
| `settings.hpp/.cpp` | 設定檔、定時發射的時刻 | `Settings.swift`、`Schedule.swift` |
| `waterfall.hpp/.cpp` | 瀑布圖 | `Waterfall.swift` |
| `i18n.hpp/.cpp` | 介面的語言 | `Localization.swift` |
| `fonts.hpp/.cpp` | 找系統上的字型檔 | （系統處理） |
| `process.hpp/.cpp`、`tasks.hpp`、`png.hpp/.cpp`、`log.hpp` | 啟動別的程式、執行緒之間交接工作、寫 PNG、開發用的輸出 | |
| `demo.hpp/.cpp`、`snapshot.hpp/.cpp`、`offscreen.hpp/.cpp`、`selftest.hpp/.cpp` | 示範資料、把版面算成圖、不開視窗畫介面、自我測試 | `Demo.swift`、`Snapshot.swift`、`SelfTest.swift` |

執行緒的安排和 macOS 版相同：介面的狀態（`App`）只在主執行緒上讀寫；解調在自己的一條執行緒上（不在音訊的回呼裡、也不在主執行緒上），
影像編碼、PTT、縮圖、開檔各有一條；做完的結果排回主執行緒。主迴圈每一圈呼叫一次 `App::tick()`，發射的排程、定時發射都在裡面照時間做。
視窗被拖著移動、改大小的時候 Windows 與 macOS 會把主執行緒卡在系統自己的迴圈裡，這段時間由 SDL 的事件監看照樣推進（發射中的排程不能停）。

用 CAT 指令按下的 PTT 不會因為序列埠關了就放開，所以按下時另外啟動一個看門的程序（就是 `nbtv-gui` 自己，加上 `--ptt-watchdog`）：
app 不管怎麼結束（當掉、被強制結束），它都會替我們對 rigctld 送「T 0」、把 rigctld 結束。

## 檢查

`nbtv-gui --selftest` 不開視窗、不碰任何真的裝置，目前有 33 項：

- 核心：每個模式送兩幀收兩幀、經過有雜訊有頻偏的模擬通道、只有雜訊時不誤解出畫面；
- 發射的流程（不開音訊裝置，由模擬的輸出照實際的時間把取樣取走，再把取走的聲音解回來）：開頭的靜音、PTT 先按下才送聲音、
  送完目前這張才停、立刻停止、還沒開始送就取消、PTT 失敗、輸出裝置送到一半不回應、畫布的字、沒有呼號時不能發射；
- 來源：圖片、影片檔（用 ffmpeg 現做一段）、播完自動停止、循環播放、移動位置；存圖片、存影片；
- 圖片序列：哪些檔案算圖片、檔名的順序（數字照大小、不分大小寫；隱藏的、子資料夾裡的、名字像圖片的資料夾不算；同一個檔案只算一次）、
  照記載的方向轉正（八種方向各做一張 JPEG，讀回來看四個角落落在哪裡）、大圖縮小；以及用它當來源的整個發射流程：
  依序送完自動停止、送到一半按停止（取走卻沒送出去的那一張退回去，再按一次接著送，沒有哪一張被跳過或重複）、立刻停止、
  還沒開始送就取消、循環播放、只有一張、讀不到的檔案、啟動時帶回上次那一批、把檔案與資料夾拖進視窗；
- 存成影片時印上訊雜比：要印的是那一張自己的訊雜比、那一塊的位置與樣子；影片實際寫出來再用 ffmpeg 讀回，每一張的左上角
  和預期的樣子比（而且比「印成別張的訊雜比」像得多），沒勾的時候左上角是原來的畫面，那一塊以外勾不勾都一樣；找不到字型時影片照存並說明；
- 定時發射：時刻的計算，以及用假的時鐘實際跑一次（包含「正在收別人的畫面時不發射」，以及來源是圖片序列時先換成影片檔、送完換回來）；
- PTT：對假的 rigctld、用 Hamlib 內建的假電台（型號 1）實際按下放開、app 突然不在時看門的程序有沒有收拾；
- 介面：不開視窗把每個面板、五種語言、三種視窗大小各畫一次；假裝滑鼠去按每一個按鈕、選單、輸入框，確認真的有作用
  （包含圖片序列的上一張、下一張、滑桿、循環播放的勾，以及存影片的那個選項）；從「開始發射」鈕按出一次完整的發射；
- 其他：翻譯表、設定檔、字型、縮放、PNG、瀑布圖、輸出佇列、TCP、啟動別的程式。
- `SDL_AUDIODRIVER=dummy` 時多跑一項：用 SDL 的「假」音訊驅動，照平常的方式啟動整個 app、開始接收、發射兩張——
  走的是和真的音效裝置一樣的程式，只是聲音沒有去任何地方。沒有設這個環境變數時這一項略過（自我測試不碰真的音效裝置）。

缺 ffmpeg、Hamlib 或字型時，相關的項目會略過並說明原因；`NBTV_SELFTEST_REQUIRE=ffmpeg,hamlib,cjk,fonts,audio` 可以指定哪些不准略過（CI 用）。

CI（[`build.yml`](../../.github/workflows/build.yml)）在 Windows 與 Linux 的 x86_64、arm64 四種機器上各自建置，然後對著「下載包解開之後」的那一份：
跑完上面的自我測試（含假音訊驅動的那一項）、把六張版面五種語言各算成一張圖（留在那一次執行的 Artifacts，`screens-<平台>`）、
真的開視窗幾秒鐘（示範資料一次、照平常的方式啟動一次；Linux 的機器沒有螢幕，用 xvfb）。Linux 另外檢查執行檔直接相依的只有 glibc。

開發時另外用開著 AddressSanitizer 與 UndefinedBehaviorSanitizer 的版本把整套自我測試跑過一次，沒有報告任何問題。

開發用的（平常用不到）：

- `nbtv-gui --read-image 圖片檔 [輸出.png]`：照「圖片序列」讀圖的方式把一張圖讀進來（照記載的方向轉正、縮小），印出原圖轉正之後的大小
  與讀進來留著的大小；有給輸出檔就把讀進來的樣子存成 PNG。不開視窗、不碰裝置，用來看某一張真的相片這個程式讀成什麼樣子。
- `--snapshot` 時的環境變數：`NBTV_SNAPSHOT_IMAGES=1` 多算一張來源是圖片序列的版面（`extra-images.png`；值是一個資料夾或圖片檔的路徑時，
  用那裡的圖片；再設 `NBTV_SNAPSHOT_EXTRA_SIZE=寬x高` 多算一張那個視窗大小的）、`NBTV_SNAPSHOT_SAVE_VIDEO=1` 多算一張存影片的選項開著的樣子、
  `NBTV_SNAPSHOT_SNR=1` 另外把印了訊雜比的影片的一格存成 `video-frame-snr.png`。
- `--selftest` 時設 `NBTV_SELFTEST_SCREENS=目錄`：把測試中途的幾個畫面（跳出來的選單、滑鼠停著時的說明、圖片序列發射中）存成 PNG。

## 驗證過與還沒驗證的

驗證過的（自動檢查，見上面）：

- 四種機器上建得起來，下載包解開就能執行，找得到自己旁邊的權重檔；
- 收發的整個流程、PTT 的順序、各種出錯時的收尾（都用模擬的輸出與只做紀錄的 PTT）；
- 經過 SDL 的音訊串流收音與送出（假的音訊驅動）；
- 影片檔來源與存影片（Linux：ffmpeg；Windows：CI 的機器上有裝到 ffmpeg 的時候）；
- 用 Hamlib 的假電台走完 CAT 的 PTT，包含看門的程序（Linux、macOS；Windows 沒有）；
- 介面在三種視窗大小、五種語言下畫得出來，每個按鈕有作用。
- 圖片序列與存成影片時印上訊雜比（2026-11-26 加的）。推上 GitHub 之後 CI 在四種機器上都跑過：Linux 的 x86_64、arm64 是 33 項全部通過；
  Windows 的 x86_64、arm64 是 32 項通過、1 項略過（用 Hamlib 假電台的那一項：機器上沒有裝 Hamlib，原本就略過），
  MSVC 沒有編譯警告，存影片印訊雜比的那一項用的是機器上的 ffmpeg 9.1。推上去之前，在開發的那台電腦上另外做了這些：
  - 整套自我測試 33 項在 macOS（Clang，開發用的 Mac）與 Docker 裡的 Ubuntu 22.04（arm64、GCC 12、ffmpeg 4.4）上全部通過，
    Linux 那一份照 CI 的方式建置（沒有編譯警告、直接相依的只有 glibc），另外用 xvfb 真的開視窗兩次；
    開著 AddressSanitizer 與 UndefinedBehaviorSanitizer 的版本也跑過一次。
  - 圖片序列的發射流程那一項裡，按停止時下一張「已經排進佇列」與「還在編碼」兩種情形都實際發生過，也都退得回去。
  - 把程式故意改壞 19 處（按停止時不退回去、發射結束時不退回去、送完不自己停、不管循環播放的勾、位置不往後移、讀不到的檔案不拿掉、
    不照方向轉正、方向 6 與 8 弄反、檔名照字元排、隱藏的檔案也算、大圖不縮小、每一張都印成第一張的訊雜比、勾了不印、沒勾也印、
    那一塊貼錯位置、啟動時不帶回上次那一批、循環播放的勾改到影片檔的設定、存影片的選項按了沒有作用、那一批路徑沒有寫進設定檔），
    對應的檢查每一處都會失敗。
  - 版面：五種語言各算了一般大小與最小視窗（1040×700）的圖片序列畫面、存影片的選項開著的樣子，macOS 與 Linux 的都逐張看過。
  - 方向的讀法另外和 macOS 的系統函式庫對過一次：用 ImageIO 把同一張圖寫成八種方向的 JPEG（EXIF 是它寫的，不是這個程式的測試寫的），
    這個程式讀進來的結果（`--read-image`）和 ImageIO 自己轉正的結果八種都一致（平均差 0.5／255，和別的方向比差 21 以上）。
    讀 EXIF 的那一段（輸入是使用者選的檔案，內容不可信）另外用 libFuzzer 加上 AddressSanitizer、UndefinedBehaviorSanitizer
    餵了 8400 萬筆隨機變造的輸入，沒有當掉，也沒有讀到資料以外的地方。

**還沒驗證的**：

- **真的音效裝置**：Windows（WASAPI）、Linux（PipeWire、PulseAudio、ALSA）上實際從電台收音、把聲音送給電台，都還沒有人試過。
  取樣率的轉換交給 SDL；延遲多少、會不會斷音，不知道。
- **真的電台**：沒有在任何一部電台上發射或接收過。
- **序列埠的 RTS／DTR**：Windows（COM 埠）與 Linux（`/dev/ttyUSB*`、`/dev/ttyACM*`）的程式只編譯過、測過「開不起來要回報原因」，沒有接過真的介面。
- **鏡頭**：從來沒有開過（CI 的機器沒有鏡頭；開發用的 Mac 上也沒有開）。
- **Windows 上的 Hamlib**：找 `rigctld.exe` 的位置（`Program Files\hamlib-*\bin`）與啟動它，沒有執行過。
- **Wayland**：CI 用的是 X11（xvfb）。
- **人實際操作**：開檔與存檔的對話框、輸入法（在「畫布」裡打中文）、高解析度與多螢幕、拖動視窗時的行為，都沒有人在 Windows、Linux 上看過。
  版面只看過不開視窗算出來的圖。
- **圖片序列**：沒有經過真的音訊裝置、真的 PTT 與電台。選圖片的兩個對話框（多選圖片檔、選資料夾）與把檔案拖進視窗，
  都只測了「拿到路徑之後」的處理，沒有人實際選過、拖過；一次拖很多個時系統是不是照程式以為的方式一個一個送來，沒有在任何系統上看過。
  真的相片沒有試過：方向與大小只用程式自己做的測試圖（最大 4032×3024）驗證，手機、相機實際寫出來的 JPEG（EXIF 的寫法各家不同）沒有拿來讀過。
  Windows 上真的標成「隱藏」的檔案會不會被略過沒有測過：檢查屬性的那幾行在 CI 上執行過，但測試裡沒有做出標成隱藏的檔案。
- **存成影片時印上訊雜比**：印了字的影片只用 ffmpeg 讀回來比對過（ffmpeg 4.4、9.0 與 9.1），沒有拿到別的播放器或社群網站上看；
  Windows 上印出來的樣子沒有人看過：CI 只比對了影片和預期的一不一樣（那一塊在 Windows 上是 206×44，Linux 與 macOS 是 223×49，字型不同）。
  找不到字型時的處理只用「故意不給字型」的方式測過。
- 長時間執行（幾個小時的接收、定時發射）。

## 已知的限制

- 介面是 app 自己畫的：沒有系統的選單列、不支援螢幕閱讀器，文字不能用系統的方式選取。
- 影片的畫面更新率固定是每秒 10 格；影片檔不讀聲音。
- 圖片只讀 JPEG、PNG、BMP、GIF；照記載的方向轉正只認 JPEG 的 EXIF。
- 圖片序列的順序只能照檔名，不能在 app 裡拖動調整；每一張固定送一次，不能指定某一張重複幾次或停留多久；不找子資料夾；
  看不到整批的縮圖，只看得到下一張要送的那一張。定時發射不能用圖片序列（到時間時來源是圖片序列的話，和鏡頭一樣先換成影片檔，播完再換回來）。
- 音訊裝置是用名稱記的：兩個裝置同名時分不出來。裝置被拔掉時不會自動改用別的（發射的聲音送錯地方比送不出去糟）。
- 沒有自動更新；新的版本要自己到 Releases 下載。
