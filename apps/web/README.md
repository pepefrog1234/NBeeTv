# 收聽卡網站

讓聽眾上傳從無線電錄下來的 WAV 檔。網站用專案的解碼器解出畫面，符合條件就產生一張收聽卡（PNG）給他下載；
卡片有數位簽章，也可以用序號或 QR code 回到網站查驗。不需要登入。

Python 寫的（FastAPI），解碼交給專案建出來的 `nbtv-rx`。設計成架在電台主人自己的 Mac 上，經由 Cloudflare Tunnel 對外。

## 流程

1. 訪客在首頁選一個 WAV 檔，可以填要印在卡片上的資料（稱呼、收聽的日期時間、頻率、地點，都是選填、自己填的）。
2. 頁面上的 Cloudflare Turnstile 確認不是機器人。按下上傳時，網站先拿 Turnstile 的權杖去問 Cloudflare，
   通過了才開一個「工作」，訪客的瀏覽器接著把錄音傳上來。
3. 錄音排隊等解碼。網站先自己拆 WAV 的檔頭、檢查長度，轉成 12 kHz 單聲道，再交給 `nbtv-rx --json --merge`。
   頁面每秒問一次進度（解到第幾秒、幾幀）。
4. 解完之後照條件判定。通過的話畫卡片、簽章、存檔，頁面上顯示卡片與下載按鈕；
   沒通過的話說明原因，並把解出來的畫面列給訪客看。
5. 錄音與暫存檔在工作結束時刪掉，只留下錄音的 SHA-256。

## 發卡的條件

設定檔的 `[rules]` 與 `[station]`：

- 有畫面的幀**超過** `min_frames`（預設 10，也就是至少 11 幀），而且
- 至少一幀的標頭解出了電台的呼號（`callsign`）。

這幾個模式的畫面沒有「解成功或失敗」的分界，訊號再弱也會有一張糊的畫面；有對錯可言的是標頭，呼號與幀序號有 CRC 保護。
（模式 5–7 與 15 的畫面是 192×144，其餘是 96×72；卡片與縮圖的版面是照 96×72 排的，比較大的畫面縮進同樣的位置。）
所以兩樣都要。標頭壞掉的幀不知道是誰送的，算進幀數；標頭明確寫著別的呼號的幀不算。

錄音最長 `max_seconds`（預設 300 秒，另留一秒的寬限），超過的直接拒絕。

## 收聽卡與簽章

卡片是 1680×1080 的 PNG（140×90 mm 的 QSL 卡比例）：底圖是電台主人放在 `assets/backgrounds/` 的照片，
上面有呼號、聽眾填的資料、解碼器量到的數字（模式、幀數、訊雜比）、解出來的畫面與一個 QR code。

**卡片證明什麼**：這個網站收到一份錄音，裡面有帶著本台呼號、解得出畫面的訊號，所以發了這張卡。
**不證明什麼**：上傳的人是不是自己從空中收到的、什麼時候、在哪裡、用哪個頻率——這些欄位是聽眾自己填的，
卡片上與紀錄裡都標明「自填、未查核」。採榮譽制。

查驗有四條路：

| 方式 | 需要什麼 | 轉傳、截圖之後還能用嗎 |
| --- | --- | --- |
| 掃卡片上的 QR code，或到 `/verify` 輸入序號 | 網站還在 | 可以。網站會顯示它保存的那一張，拿來和手上的比對 |
| `verify_card.py 卡片.png --cert 根憑證.pem` | 卡片的原檔、電台公布的根憑證或它的指紋 | 不行。通訊軟體與社群網站會重新壓縮圖片、拿掉簽章資料 |
| `verify_card.py 卡片.png --export 目錄` 之後用 `openssl` | 同上 | 不行 |
| 支援 C2PA（Content Credentials）的工具 | 卡片的原檔 | 不行。而且簽署者會顯示為「不認得」，除非把本台的根憑證加進那個工具的信任錨點 |

### 憑證

金鑰都是 ECDSA P-256，憑證是**自己簽發**的 X.509，沒有經過憑證機構。`python -m nbtvweb init-keys` 產生兩層：

| 檔案（`data/keys/`） | 是什麼 | 誰用 |
| --- | --- | --- |
| `root-cert.pem` | 根憑證：自己簽自己的 CA，有效二十年。**對外公布的就是它的指紋** | 查驗的人；網站放在 `/root.pem` |
| `root-key.pem` | 根憑證的私鑰 | 只有以後要簽發新的簽章憑證時才用。網站用不到，建議產生之後移到別的地方保管 |
| `signing-cert.pem` | 簽章憑證：根憑證簽發的，有效十年 | 附在每張卡裡 |
| `signing-key.pem` | 簽章憑證的私鑰 | 網站每發一張卡都用它簽 |

分成兩層有兩個原因：C2PA 不接受自己簽自己的簽章憑證；根憑證的私鑰平常用不到，可以不放在連著網路的電腦上。
「信不信一張卡」看的是根憑證的 SHA-256 指紋是不是電台公布的那一個（查驗頁上有）——任何人都做得出寫著同樣名字的憑證。

### 卡片檔裡有什麼

- 每張卡有一筆紀錄（JSON）：序號、發行時間、電台、解碼的統計、錄音的雜湊、聽眾自填的資料、圖檔的雜湊、兩張憑證的指紋。
  簽章憑證簽的是這段 JSON 文字的 UTF-8 位元組（ECDSA／SHA-256）。
- 紀錄、簽章、憑證放在 PNG 的三個文字區塊（iTXt）：`nbtv-card-record`、`nbtv-card-signature`、
  `nbtv-card-certificate`（兩張 PEM：簽章憑證，接著根憑證）。
- 紀錄把圖檔本身也綁進來：`card.image_sha256` 是**整個圖檔**的雜湊——除了放簽章資料的區塊（上面三個文字區塊與
  C2PA 的 `caBX`），PNG 裡的每個區塊照順序各取「長度（4 位元組）、種類、內容」接在一起算 SHA-256。
  圖檔多一個、少一個或改一個區塊都對不上，包括「原本的像素一個都不動，另外加一格動畫或透明度，
  讓看圖軟體顯示別的東西」這類手法。驗的時候只要會拆 PNG 的區塊，不必解開圖，所以不需要 Pillow。
  另外還留著 `card.pixels_sha256`（解開之後的像素的雜湊）：2026-11-24 以前發的卡只有這一項。
  驗那種早期的卡時，除了比對像素（要有 Pillow），還要確認檔案裡沒有本站不會寫的區塊。
- **C2PA**（`[c2pa] enabled`，預設開）：另外在 `caBX` 區塊放一份 C2PA 的 manifest，用官方的 `c2pa-python` 產生，
  由同一張簽章憑證簽署。內容是：產生它的程式、`c2pa.created` 動作（來源類型是 `compositeSynthetic`：程式把照片、
  文字與解出來的畫面合成的）、以及一則自訂的聲明 `tw.hf-nbtv.reception-card`，裡面就是上面那一筆紀錄。
  manifest 還記了整個檔案的雜湊，所以它是最後才加的，加了之後檔案改一個位元組都驗不過。
  紀錄裡的 `card.c2pa` 寫著這張卡有沒有附 C2PA，所以 `caBX` 區塊被拿掉也看得出來。

C2PA 這一層有兩件事要知道：

- 本台的根憑證不在 C2PA 的信任清單上。別人的工具驗得出「檔案沒有被改過、簽章正確」（狀態 `Valid`），
  但簽署者是 `signingCredential.untrusted`；把本台的根憑證設成信任錨點之後才是 `Trusted`。
- 沒有向時間戳記服務（TSA）要時間戳記。簽章憑證到期之後，C2PA 的工具會把簽章判定為過期。

`verify_card.py` 是獨立的一個檔案，網站上也可以下載（`/verify_card.py`），只要 Python 3.8 以上。它檢查四件事：
紀錄的簽章、簽章憑證是不是指定的根憑證簽發的、圖檔有沒有被改、C2PA 的資料（並確認裡面的紀錄和簽過的是同一份）。
簽章用 `cryptography` 套件驗，沒有裝的話改用系統的 `openssl`。C2PA 要有 `c2pa-python` 才會檢查，沒裝不影響結論。

| 結束碼 | 意思 |
| --- | --- |
| 0 | 通過：這張卡是指定的電台發的，紀錄與圖檔都沒有被改過 |
| 1 | 沒有通過 |
| 2 | 沒辦法下結論：沒有用 `--cert` 或 `--fingerprint` 指定電台的根憑證、沒有驗簽章用的工具，或是早期格式的卡而沒有 Pillow |

沒有指定根憑證時，它只能確認檔案前後一致——任何人都做得出這樣的檔案——所以不會說「通過」。

卡片檔是別人給的，工具把它當成可能是惡意的來處理：

- **憑證只讀一次**。PEM 文字由工具自己拆成 DER 位元組，格式稍有出入就不收；算指紋、驗簽章、驗簽發關係用的都是
  同一份位元組，交給 `openssl` 的也是 DER。（同一段刻意寫歪的 PEM 文字，不同的程式會讀出不同的憑證：
  指紋算出來是電台的根憑證，驗簽章用的卻是偽造者的憑證。）
- **卡片裡的文字先處理過才印**：控制字元、換行、看不見的排版字元都換成 `\uXXXX`，太長的值由工具自己折行，
  每一行前面都留著縮排，所以卡片裡的字排不出一行像是工具自己印的結論。結論固定在最後一行。
- **不連網路**：不照卡片裡寫的網址去抓 C2PA 的「放在別處的 manifest」，也不做憑證的撤銷查詢。
- 前面的檢查沒通過的檔案，不再交給 C2PA 的程式庫讀；壓縮過的文字區塊不解壓縮。

## 在這台 Mac 上架起來

以下的指令都從專案根目錄執行。需要 Python 3.11 以上（開發時用的是 Homebrew 的 3.12）。

```sh
# 1. 建解碼器（已經建過的話，這次是為了 nbtv-rx 新增的 --json）
cmake -S . -B build && cmake --build build

# 2. 網站自己的 Python 環境，不動系統的 Python
python3.12 -m venv apps/web/.venv
apps/web/.venv/bin/python -m pip install -r apps/web/requirements.txt

# 3. 設定檔
cp apps/web/config.example.toml apps/web/config.toml

# 4. 根憑證、簽章憑證與它們的私鑰（只做一次；做完把 data/keys/root-key.pem 移到別的地方保管）
cd apps/web && .venv/bin/python -m nbtvweb init-keys

# 5. 檢查找不找得到解碼器、憑證、字型、底圖
.venv/bin/python -m nbtvweb check

# 6. 啟動（caffeinate 讓電腦接著電源時不睡眠）
caffeinate -s .venv/bin/python -m nbtvweb serve
```

`config.toml` 裡至少要填的：

- `[turnstile]` 的 `site_key` 與 `secret_key`。到 Cloudflare 控制台的 Turnstile 建一個 widget，主機名稱填網站的網域。
  只在本機試用時，可以填範例檔裡列的測試金鑰，或把 `enabled` 設成 `false`。
- `[server]` 的 `public_url`：對外的網址（例如 `https://card.example.com`，**不要加埠號**）。它會印在每張卡的 QR code 與紀錄裡，
  **卡片發出去之後改不了**，上線前要先填對。Turnstile 的查證也用它的主機名稱（見下面）。
- `[server]` 的 `behind_cloudflare = true`：訪客的位址才會取自 Cloudflare 給的標頭，而不是通通算成 `127.0.0.1`。

照片放進 `apps/web/assets/backgrounds/`（見那裡的說明）。

### Turnstile 是怎麼接的

照 Cloudflare 的做法：瀏覽器 → 本站的伺服器 → Cloudflare 的 siteverify，瀏覽器不直接問 Cloudflare。

- 頁面（`static/app.js`）用 `turnstile.render` 明確地畫 widget，帶著動作名稱 `upload`。按下上傳時把權杖連同表單送到
  `POST /api/jobs`；權杖只能用一次，所以每送一次就 `turnstile.reset` 要一個新的。
- 伺服器（`nbtvweb/turnstile.py`）拿 secret key 去查證，下面幾樣都對才開工作，否則回 403：
  `success` 是 true、`action` 是 `upload`、`hostname` 在允許的清單裡。
  清單預設是 `public_url` 的主機名稱，另有別的網域時列在 `[turnstile] hostnames`。
  這樣別人把同一個 widget 放到別的網站取得的權杖，拿來這裡不能用。
- 經過 Cloudflare 時（`behind_cloudflare = true`），訪客的位址會一併交給 siteverify 核對。
- secret key 只放在 `config.toml`（不進版本控制），不會出現在頁面、紀錄檔或錯誤訊息裡。
- 用 Cloudflare 的測試金鑰時只看 `success`：測試金鑰回的主機名稱固定是 `example.com`，也沒有動作名稱。

### 對外：Cloudflare Tunnel

網站只聽 `127.0.0.1`。對外用 Cloudflare Tunnel：`cloudflared` 從這台 Mac 連出去，不必有固定 IP、也不用在路由器開埠。
需要一個放在 Cloudflare 上的網域。

```sh
cloudflared tunnel login                               # 會開瀏覽器，用你的 Cloudflare 帳號授權
cloudflared tunnel create nbtv-card                    # 印出通道的 UUID，憑證檔在 ~/.cloudflared/
cloudflared tunnel route dns nbtv-card card.example.com
# 把 apps/web/deploy/cloudflared-config.example.yml 照著改，存成 ~/.cloudflared/config.yml
cloudflared tunnel run nbtv-card
```

**通道是在 Cloudflare 控制台建的話**（拿到的是一長串權杖），不需要上面的 `login`、`create` 與 `config.yml`：

1. 把權杖交給 `cloudflared`。`sudo cloudflared service install 權杖` 會裝成開機就啟動的服務，權杖存在它自己的設定目錄裡；
   只想臨時跑的話用 `cloudflared tunnel run --token 權杖`。
2. 在控制台加一條路由，把網域指到這個網站（照 2026 年 6 月的 Cloudflare 文件）：
   **Networking → Tunnels** → 選那條通道 → **Routes** 分頁 → **Add route** → **Published application** →
   填子網域、選網域 → **Service URL** 填 `http://127.0.0.1:8088` → **Add route**。
   一條通道可以有很多條路由，所以這台電腦上已經有通道在跑的話，加在那一條就好，不必另建。
3. DNS：不要自己加指到 `127.0.0.1` 的 A 紀錄（那樣每個訪客連到的是他自己的電腦）。
   控制台如果沒有自動建紀錄、而是提示你自己加，就在 DNS 頁加一筆 CNAME：名稱是子網域，
   目標是 `通道的ID.cfargotunnel.com`，Proxy 要開（橘色雲）。通道的 ID 要是**加了路由的那一條**；
   指到別條通道的話，連進來會是 Cloudflare 的錯誤 1033。存檔後清單裡會顯示成類型「通道」。

通道的權杖只給 `cloudflared` 用，**不要放進 `config.toml`**。`[turnstile]` 要填的是另一樣東西：
Turnstile 的 site key 與 secret key，在控制台的 Turnstile 頁建 widget 取得（通常是 `0x4` 開頭的英數字）。
填錯的話 `python -m nbtvweb check` 會直接說。

`cloudflared service install` 可以把它裝成登入後自動啟動的服務。沒有網域、只想先試試看對外通不通，
可以用 `cloudflared tunnel --url http://127.0.0.1:8088`：它會給一個隨機的 `trycloudflare.com` 網址，
每次都不一樣，所以不能拿來正式發卡（QR code 會失效）。

免費方案經過 Cloudflare 的請求上限是 100 MB，比這裡的上傳上限（預設 64 MB）大。

### 讓它一直開著

最簡單的是開一個終端機視窗跑第 6 步。要登入後自動啟動的話，`deploy/tw.hf-nbtv.card.plist` 是 launchd 的範本
（**沒有實際裝起來試過**；專案放在「文件」底下時可能會被 macOS 的隱私權保護擋住，範本裡有說明）。

### 上線前要確認的

- `check` 沒有印出「測試金鑰」或「沒有啟用」。
- `public_url` 是正式的網址，`behind_cloudflare` 是 `true`。
- 用手機掃一張測試卡的 QR code，確定連得回查驗頁。
- `data/` 有備份的安排（見下面）。

## 防濫用

| 措施 | 預設 | 在哪裡 |
| --- | --- | --- |
| Turnstile：每開始一次解碼要一個通過驗證的權杖，伺服器端向 Cloudflare 查證，並核對動作與主機名稱 | 啟用 | `[turnstile]` |
| 每個來源位址每 10 分鐘開始幾次解碼（IPv6 以 /64 計） | 6 次 | `jobs_per_ip`、`window_minutes` |
| 驗證沒過的請求另外計數，超過就不再去問 Cloudflare | 30 次／10 分鐘 | 由 `jobs_per_ip` 推得（5 倍，最少 20） |
| 同一個來源同時還沒結束的工作 | 2 個 | `jobs.py` 的 `ACTIVE_PER_OWNER` |
| 上傳完、等著解碼的工作 | 8 個 | `queue` |
| 還在上傳的工作（含開了工作還沒開始傳的；90 秒內沒開始就作廢） | `queue` 的 4 倍 | — |
| 上傳到一半停住不傳 | 30 秒就中斷 | — |
| 上傳的平均速度（開始 30 秒之後才算） | 低於每秒 8 KB 就中斷 | — |
| 上傳的大小：邊收邊算，超過就中斷；一次上傳最多傳 10 分鐘 | 64 MB | `max_upload_mb` |
| 錄音的長度 | 300 秒 | `max_seconds` |
| 同時解碼的份數；解碼以較低的優先權執行（`nice`），不和即時收發搶處理器 | 2 份 | `workers` |
| 一份錄音的解碼時間 | 300 秒 | `decode_timeout_seconds` |
| 一天發幾張卡 | 200 張 | `cards_per_day` |
| 頁面、靜態檔、卡片圖檔的網址帶了多餘的參數，或卡片的序號不是標準寫法 | 只回一個轉址，不送檔案 | `app.py` 的 `CanonicalUrls` 與 `card_png` |

上傳的檔案是陌生人給的：WAV 的檔頭由 Python 自己拆、每個欄位都檢查，`nbtv-rx` 只讀得到網站自己寫出去的檔案；
每一份錄音各開一個 `nbtv-rx` 行程，當掉或卡住不會拖垮網站。頁面有 Content-Security-Policy，只載入自己的檔案與 Turnstile。

最後一列的理由：前面的 Cloudflare 是照整個網址（含 `?` 後面的參數）快取的。如果每一種寫法都直接給檔案，
隨便加一個參數就能讓每一次請求都回到這台電腦重新送一次——卡片圖檔有幾百 KB 到幾 MB，等於不花力氣就能耗掉這裡的上傳頻寬。
所以一張卡的圖檔只有兩個網址（`/card/序號.png` 與它加上 `?download=1`），其他寫法都轉回這兩個之一。

這些擋得住隨手的濫用，擋不住有心人用很多位址慢慢耗。真的遇到時要靠 Cloudflare 那一邊的規則（速率限制、封鎖）。

## 資料與隱私

都在 `data/`（不進版本控制）：

| 路徑 | 內容 | 要備份嗎 |
| --- | --- | --- |
| `data/keys/root-key.pem` | 根憑證的私鑰（權限 600） | **要**，不能外流也不能弄丟。網站用不到它，最好移到別的地方保管 |
| `data/keys/signing-key.pem` | 簽章憑證的私鑰（權限 600） | **要**，而且不能外流 |
| `data/keys/root-cert.pem`、`signing-cert.pem` | 兩張憑證（公開的） | 要。根憑證遺失的話，以前發的卡就沒有可以比對的指紋了 |
| `data/cards.sqlite3` | 發過的卡的紀錄與簽章 | 要，查驗頁靠它 |
| `data/cards/` | 發過的卡的 PNG | 要 |
| `data/tmp/` | 上傳中、解碼中的暫存檔，工作結束就刪 | 不用 |

- 錄音不保存，只留 SHA-256。
- 聽眾填的資料會印在卡上、存在紀錄裡，知道序號的人在查驗頁看得到。序號是亂數（60 位元），猜不到也排不出順序；
  `robots.txt` 請搜尋引擎不要收錄查驗頁與卡片。
- 訪客的位址只放在記憶體裡計數，不寫進紀錄檔，程式重開就清空。查證 Turnstile 的權杖時，
  會把 Cloudflare 告訴我們的訪客位址一併交回給 Cloudflare 核對。
- 簽章憑證的私鑰和網站在同一個使用者帳號底下，靠的只是檔案權限：這台 Mac 被入侵的話，它也保不住。
  根憑證的私鑰移走之後不受影響。

## 測試

```sh
cd apps/web && .venv/bin/python -m unittest discover -s tests -t .
```

約一分鐘。`test_units.py` 與 `test_signing.py`（憑證、簽章、離線查驗、C2PA）不需要解碼器；`test_audio.py` 與 `test_server.py` 會用 `build/tools/` 裡的 `nbtv-tx`、`nbtv-chan`
合成錄音，在本機開一個真的網站從頭走到尾（Turnstile 的查證服務是假的）。沒有建工具時這兩組會跳過。

量過的速度（M4 Max）：1 分 41 秒、150 幀的模式 14 錄音，從上傳到出卡約 30 秒。

## 檔案

| 路徑 | 內容 |
| --- | --- |
| `nbtvweb/app.py` | 網址：頁面、開工作、上傳、查進度、查驗 |
| `nbtvweb/jobs.py` | 工作的排隊與處理 |
| `nbtvweb/audio.py` | 拆 WAV、挑聲道、轉成 12 kHz |
| `nbtvweb/decoder.py` | 呼叫 `nbtv-rx --json` |
| `nbtvweb/rules.py` | 發卡的條件 |
| `nbtvweb/card.py` | 畫卡片 |
| `nbtvweb/issue.py`、`signing.py`、`cardfile.py`、`store.py` | 發卡、憑證與簽章、卡片檔的格式、保存 |
| `nbtvweb/provenance.py` | C2PA：把 manifest 嵌進卡片、讀回來檢查 |
| `nbtvweb/turnstile.py`、`ratelimit.py`、`models.py` | Turnstile 查證、用量限制、欄位檢查 |
| `static/` | 頁面（繁體中文與英文，右上角切換） |
| `verify_card.py` | 離線查驗的獨立工具 |
| `deploy/` | Cloudflare Tunnel 與 launchd 的設定範本 |

## 還沒做、沒驗證過的

- **正式環境走過一次**（2026-11-24）：經過 Cloudflare Tunnel 的正式網域上，由真人通過 Turnstile 的 widget、上傳、出卡，
  卡片再從外面下載回來，用公布的根憑證離線驗過（簽章、畫面、C2PA 都對）。沒有權杖或假權杖的上傳都被擋下。
  還沒驗的是「同一個真的權杖重送會被拒絕」：這一點只在測試裡用假的查證服務驗過。
- Cloudflare Tunnel 實際接的是「控制台建的通道」那一種做法；用 `cloudflared` 指令建通道的那一段是照說明寫的，沒有實際跑過。
- **空中錄下來的真錄音沒有試過**，只用 `nbtv-tx` 加模擬通道合成的錄音，以及把它轉成各種取樣率與位元深度的版本。
- 頁面只在 Claude 桌面版內建的瀏覽器（Chromium）看過，Safari、Firefox 與真的手機沒有試。
- launchd 的範本沒有裝起來試過。
- **C2PA 只用官方的 SDK（`c2pa-python` 0.39.0，也就是 c2pa-rs 0.92）驗過**：設了本台的根憑證是 `Trusted`，沒設是 `Valid`，
  檔案被改是 `Invalid`。沒有拿 `c2patool` 或 Content Credentials 的網站實際看過這些卡。
- 還不能換簽章憑證：沒有「用根憑證簽發一張新的簽章憑證」的指令，網站查驗舊卡時也只認現在這一張。
  簽章憑證十年後到期前要補上。
- 同一份錄音可以重複領卡（榮譽制，沒有擋）。
- Linux 上沒有跑過；字型的搜尋路徑是照 macOS 寫的，其他系統要在設定檔指定字型檔。
