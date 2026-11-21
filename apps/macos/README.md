# macOS 外殼

SwiftUI 寫的 macOS app。音訊裝置、鏡頭、影片檔、PTT 與介面都在這裡；調變解調、影像編解碼全部交給核心，
而且只透過 `core/include/nbtv/nbtv.h` 這一個 C API 使用核心（經由 `CNBTV/` 的 module map 匯入 Swift）。
介面照 [macOS 介面版面](../../短波%20NBTV%20macOS%20介面版面.html) 的五張版面做。

## 建置

需要 Xcode（Swift 編譯器與 macOS SDK）、CMake、Ninja，以及核心用的 libavif（`brew install libavif`）。
找不到 libavif 時核心不含影像編解碼：類比模式照常，數位模式不能發射、收到的也顯示不出畫面，
`--snapshot` 會直接回報算不出來（這個組態沒有實際建置測試過）。

```sh
cmake -S . -B build-app -G Ninja -DNBTV_BUILD_MACOS_APP=ON -DNBTV_BUILD_TESTS=OFF -DNBTV_BUILD_TOOLS=OFF
cmake --build build-app
```

產物是 `build-app/apps/macos/NBTV.app`。建置的最後一步會把內附的測試圖複製進 bundle，並做 ad-hoc 簽章
（系統的相機與麥克風權限要 bundle 有簽章才認得）。沒有開沙盒。

- CMake 的 Swift 支援只有 Ninja 與 Xcode 兩種產生器能用，所以這個目錄要另外用 `-G Ninja` 設定。
- 最低系統版本：程式只用到 macOS 14 就有的 API。要做出能在 macOS 14 執行的版本，設定時加上
  `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0`（這裡會把它轉成 swiftc 的 `-target`；CMake 自己不會轉），
  而且 libavif 也要是為那個版本建置的（Homebrew 的是為安裝當時的系統版本建置的）。
  不指定時，以建置這台機器的系統版本為準。
- ad-hoc 簽章每次重新建置都會變，系統可能會再問一次相機與麥克風的權限。

## 執行

```sh
open build-app/apps/macos/NBTV.app
```

第一次使用：

1. 按右上角的齒輪（或 ⌘,）打開設定，填呼號、選音訊的輸入與輸出裝置、選 PTT 的控制方式。
   沒有填呼號不能發射。
2. 系統會問麥克風權限（接收要從音訊輸入裝置收音），選鏡頭當發射來源時會問相機權限。
3. 回到主視窗就已經在接收了。右側欄選發射來源（鏡頭或影片檔）、縮放方式與發射模式，按「開始發射」。

### 同一台電腦上同時開兩個

兩部電台接在同一台電腦上、要互相收發時，需要兩個設定各自獨立的 app。設定建置目錄時加上
`-DNBTV_MACOS_SECOND_APP=ON`，建置就會多做出 `NBTV 2.app`：

```sh
cmake -S . -B build-app -G Ninja -DNBTV_BUILD_MACOS_APP=ON -DNBTV_MACOS_SECOND_APP=ON -DNBTV_BUILD_TESTS=OFF -DNBTV_BUILD_TOOLS=OFF
cmake --build build-app
open "build-app/apps/macos/NBTV 2.app"
```

- `NBTV 2.app` 是 `NBTV.app` 的複本：程式完全相同，只有名稱與 bundle 識別碼不同（`tw.hf-nbtv.mac2`）。
  每次建置都會跟著重做，所以兩個一定是同一版。
- 識別碼不同，呼號、音訊裝置、PTT 等設定就各存各的；系統的麥克風與相機權限也要各自同意一次。
- 主視窗頂列的名稱是「短波 NBTV 2」，設定視窗的標題是「設定 — 短波 NBTV 2」，用來分辨眼前是哪一個。
- 兩個 app 各接一部電台：各選各的輸入裝置、輸出裝置與 PTT，兩邊不要選同一個序列埠。
- 第一次開啟時兩個視窗的位置相同，第二個會剛好蓋住第一個，拖開即可（之後各自記得位置）。

### 接上電台之前

還沒有實機測試過。下面是從訊號的設計推出來的注意事項：

- 訊號佔 352–2648 Hz。收發兩邊都用 USB（或資料模式），發射與接收的濾波器都要涵蓋這一段；
  比它窄的濾波器會切掉邊緣的子載波。
- 兩部電台的頻率差要在 ±150 Hz 以內，超過就同步不到。
- 關掉會改變波形的處理：發射端的語音壓縮與等化，接收端的降噪（NR）與自動陷波（導頻是固定的載波，會被當成干擾濾掉）。
- OFDM 的峰值比平均高很多。輸出音量調到 ALC 不動作為止，平均功率會比額定功率低不少，這是正常的。
- 兩部電台放在一起時訊號很強，接收端容易過載失真；用假負載、最小功率或衰減器。

收發的流程：

- **接收**：音訊一進來就交給核心；模式自動偵測。收到的畫面排在「最近收到」，點縮圖可以換到主畫面，
  「儲存圖片」把主畫面那一張存檔（數位模式存成標準的 `.avif`，就是收到的那份資料，不重新壓縮；類比模式存 PNG）。
- **發射**：按下後先按 PTT，等 0.3 秒才開始送音訊；每一張都在前一張快送完時才取來源當下的那一格，
  幀與幀之間沒有空隙。發射中接收暫停、不能換模式，但可以換來源與縮放方式（下一張生效）。
  按「停止發射」後送完目前這一張，補上尾巴，放開 PTT，回到接收。
- **PTT**：不控制（電台的 VOX）、序列埠的 RTS 或 DTR、Hamlib 的 rigctld（TCP，`T 1`／`T 0`）。
  PTT 出錯時訊息顯示在視窗底列，並回到接收。結束 app 時也會放開 PTT。
- **音訊**：app 與核心之間一律是 48 kHz 單聲道。輸入裝置不是 48 kHz 時在 app 裡轉換，輸出則交給系統轉換；
  輸入只取第一個聲道，立體聲的輸出裝置兩個聲道送一樣的訊號。
- **在背景執行**：app 執行期間會向系統宣告自己在做即時的工作（`ProcessInfo.beginActivity`），
  用意是視窗被蓋住、隱藏或縮進 Dock 時，不要被系統當成閒置而拖慢計時器（App Nap）、來不及取走收到的音訊。

## 開發用的模式

都不開視窗，直接執行 bundle 裡的執行檔 `build-app/apps/macos/NBTV.app/Contents/MacOS/NBTV`：

| 參數 | 作用 |
| --- | --- |
| `--snapshot <目錄>` | 不碰任何裝置、不要求權限，把五張版面各算成一張 PNG（`1-receiving`、`2-transmitting`、`3-idle-video`、`4-settings`、`5-mode-menu`），用來對照版面稿。畫面、縮圖、狀態、瀑布圖都是內附的測試圖真的經過核心收發算出來的。同一個目錄裡還會留下示範用的 `rocket_launch.mp4`。環境變數 `NBTV_SNAPSHOT_EXTRA_SIZE=寬x高` 會多算一張其他視窗大小的「接收中」；`NBTV_SNAPSHOT_GAP=秒數` 改變示範資料裡兩次發射之間的空檔（預設 0.8 秒；不論多長都應該解出 6 張），解出來的張數不是 6 張時會印出接收機依序交出了什麼。 |
| `--selftest [--rig 主機:連接埠]` | 逐項檢查外殼自己的零件：核心收發、存檔的內容、停止時的尾巴、取樣率轉換、來源畫面的縮放、影片檔來源與方向、鏡頭（已經有相機權限才測）、序列埠的錯誤處理、rigctld（給了 `--rig` 才測，對著真的 rigctld）、音訊輸出與自發自收（需要 BlackHole 虛擬音效卡與麥克風權限）。有失敗的項目時結束碼是 1。 |
| `--transmit-test [--device 名稱] [--mode 編號] [--frames 張數] [--callsign 呼號]` | 把內附的測試圖送到指定的輸出裝置（預設找名稱裡有 BlackHole 的）。 |
| `--receive-test [--device 名稱] [--seconds 秒數]` | 從指定的輸入裝置接收，印出解出來的每一幀，以及實際交給接收機的取樣率。 |

要在沒有人操作的情況下驗證真的 app（真的視窗、真的音訊裝置），用環境變數：

| 環境變數 | 作用 |
| --- | --- |
| `NBTV_DEBUG_SETTINGS="鍵=值;…"` | 用暫時的設定啟動，不讀也不寫使用者的設定。鍵：`callsign`、`input`、`output`（裝置名稱的一部分）、`ptt`（`none`／`serial`／`rigctld`）、`rig`（主機:連接埠）、`mode`、`source`（`camera`／`video`）。 |
| `NBTV_DEBUG_VIDEO=<影片檔>` | 啟動後開啟這個影片檔當發射來源。 |
| `NBTV_DEBUG_SCRIPT="秒:動作;…"` | 主視窗出現後照時間表執行動作，並印出收發流程的轉折。動作有 `tx`、`stop`、`toggle`、`input=`、`output=`、`source=`、`mode=`、`scale=`、`settings`、`modes`、`select=`、`resize=`、`click=x,y[,視窗]`、`drag=x1,y1,x2,y2[,視窗]`（把滑鼠事件放進 app 的事件佇列）、`savepanel`、`dump`、`quit`，說明在 `Sources/NBTVApp.swift` 的 `DebugOptions`。 |
| `NBTV_DEBUG_DUMP=<目錄>` | `dump` 動作把每個視窗（連同標題列的控制鈕）存成 PNG 的地方；沒有設就只印狀態。 |
| `NBTV_NO_DEVICES=1` | 啟動時不開音訊輸入、也不開發射來源。 |

例：兩個行程經 BlackHole 對傳，一個發射、一個接收。

```sh
APP=build-app/apps/macos/NBTV.app/Contents/MacOS/NBTV
$APP --receive-test --device BlackHole --seconds 20 &
NBTV_DEBUG_SETTINGS="callsign=N0CALL;input=BlackHole;output=BlackHole;source=video;mode=5" \
NBTV_DEBUG_VIDEO=影片.mp4 NBTV_DEBUG_SCRIPT="2:tx;9:stop;14:quit" $APP
```

注意：`output=` 找不到符合的裝置時會用系統預設的輸出裝置；測試時不要把訊號送進真的電台。

## 原始碼

| 檔案 | 內容 |
| --- | --- |
| `CMakeLists.txt`、`Info.plist.in` | 建置設定與 bundle 的 Info.plist（相機、麥克風的用途說明）。 |
| `CNBTV/` | 讓 Swift 匯入 `nbtv.h` 的 module map，以及幾個 Swift 不方便直接做的小函式（存取大陣列、序列埠的控制線）。 |
| `Sources/main.swift` | 進入點：依參數分到一般啟動或上面幾個開發用的模式。 |
| `Sources/NBTVApp.swift` | 視窗（主視窗、設定）、功能表，以及開發用的 `DebugOptions`。 |
| `Sources/AppModel.swift` | 介面的狀態與收發流程（接收的顯示、發射的排程、PTT 的時機、發射來源）。 |
| `Sources/RadioEngine.swift` | 音訊裝置與核心之間的橋樑；核心的接收機與頻譜分析只在它的佇列上使用。 |
| `Sources/CoreBridge.swift` | 核心 C API 的 Swift 包裝（模式表、接收機、發射機、頻譜分析）。 |
| `Sources/AudioDevices.swift`、`Sources/AudioIO.swift` | 列出音訊裝置；用 AUHAL 從指定的裝置收音、送音，環狀緩衝與取樣率轉換。 |
| `Sources/PTT.swift` | PTT：不控制、序列埠 RTS／DTR、rigctld。 |
| `Sources/VideoSources.swift` | 鏡頭、影片檔，以及把來源畫面縮成 96×72（填滿或完整、影片的方向）。 |
| `Sources/Settings.swift` | 使用者設定（存在 UserDefaults）。 |
| `Sources/Waterfall.swift` | 把核心算出來的頻譜列畫成瀑布圖。 |
| `Sources/Theme.swift`、`Sources/Controls.swift` | 版面稿的顏色、字型、圖示，以及自己畫的控制元件。 |
| `Sources/MainWindow.swift`、`Sources/ReceivePanels.swift`、`Sources/SidePanel.swift`、`Sources/SettingsWindow.swift`、`Sources/ModeMenu.swift` | 五張版面。發射模式選單的每一列都取自核心的模式表（`nbtv_mode_at`）。 |
| `Sources/WindowChrome.swift` | 把視窗的紅黃綠控制鈕排到頂列的垂直中央。 |
| `Sources/Demo.swift`、`Sources/Snapshot.swift`、`Sources/SelfTest.swift` | 開發用：示範資料、`--snapshot`、`--selftest` 與收發測試。 |

## 執行緒

核心的物件不是執行緒安全的，同一個物件只在同一條佇列上使用：

- 即時音訊執行緒只做一件事：在環狀緩衝與裝置之間搬取樣。
- 接收機與頻譜分析在 `RadioEngine` 的序列佇列上（`nbtv_rx_push` 在一幀結束時可能要幾十毫秒，不能放在即時執行緒或主執行緒）。
- 發射機在 `AppModel` 的發射佇列上，PTT 在另一條序列佇列上（開序列埠、連線都可能卡一下）。
- 介面的狀態只在主執行緒上改，而且集中在每 0.1 秒一次的更新裡，整個視窗一秒只重排 10 次。

## 驗證過與還沒驗證的

驗證過（都在開發機上，音訊只經過 BlackHole 虛擬音效卡，沒有接電台）：

- `--snapshot` 的五張圖逐張對照過版面稿；`--selftest` 全部通過（鏡頭與 rigctld 兩項略過）。
- 兩個行程經 BlackHole 對傳：九個模式各送一張都解得出來；真的 app 以影片檔來源連續發射、另一個行程接收，
  張數與幀序號都對得上，輸出佇列沒有空掉過。
- 停止發射的幾種時機：下一張已經排進佇列、下一張剛編好、還在開頭的靜音、開始後立刻取消。接收端收到的張數都正確，
  被取消的那一張的幀序號會再用一次（核心的 `nbtv_tx_discard_last`）。
- rigctld 的 PTT：對著另外開的 `rigctld -m 1` 測過按下、放開，以及正常結束、`kill`、Ctrl-C 時會放開。
- 輸入裝置不是 48 kHz（24 kHz、44.1 kHz）時的取樣率轉換；換輸入裝置後重新開始接收。
- 第二個 app（`NBTV 2.app`）：簽章有效；`--snapshot` 與真的視窗的標題都是「短波 NBTV 2」；
  用 `open` 在背景啟動（不開任何裝置）後正常結束，設定存在自己的 `tw.hf-nbtv.mac2`，第一個 app 的設定沒有被動到。

還沒驗證的：

- **兩個 app 各接一部電台實際收發**：第二個 app 只確認過能啟動、設定獨立；沒有經過音訊裝置與另一個 app 對傳
  （程式與第一個相同，第一個的對傳是驗證過的）。
- **在背景執行時不被系統拖慢**：只加了宣告，沒有實際比較過視窗被蓋住時有沒有漏掉音訊。

- **鏡頭擷取**：相機權限沒有授權過，鏡頭這個預設來源完全沒跑過。
- **序列埠的 RTS／DTR**：只測了開不了的埠與虛擬終端機的錯誤處理，沒有接真的控制線。
- 用 `open`（LaunchServices）啟動時 app 自己的權限詢問，以及拒絕權限之後的訊息。
- 存檔對話框按下「儲存」之後實際寫檔、全螢幕、裝置熱插拔與系統預設裝置改變。
- 在 macOS 14 上實際執行（只確認過以 `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0` 編得過）。

## 已知的限制

- 下拉選單（音訊裝置、序列埠、鏡頭）出現在游標的位置，不是貼齊按鈕。
- 連結的是 Homebrew 的 libavif 動態函式庫（路徑寫死在執行檔裡），還不能直接拿到別台機器上用。
- 用 rigctld 控制 PTT 時，如果 app 當掉或被強制結束（`kill -9`），沒有人去送「放開」的指令，電台會停在發射狀態。
  正常結束、`kill`、Ctrl-C 都會先放開 PTT。
  （序列埠的話，行程結束時系統會關閉序列埠，控制線通常會跟著放掉；這一點沒有接硬體實測。）
- 影片檔記載的左右鏡像不處理（只處理 90 度的倍數的旋轉）。
- 收到的畫面只留在記憶體裡（最多 60 張），結束 app 就沒有了。
- 用到幾個在 macOS 27 SDK 標為過時的 AVFoundation API（`AVPlayerItemVideoOutput`、`AVAssetWriterInput`），建置時會有警告。
