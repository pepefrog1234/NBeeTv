// 頁面上的文字：繁體中文、簡體中文、日文、西班牙文、英文（美式）。兩個頁面共用。
// 用法：元素加 data-i18n="鍵"，或在程式裡呼叫 NBTV.t("鍵", {變數})。文字一律用 textContent 放進頁面。
// 每一種語言的鍵與 {變數} 都要一樣多（tests/test_static.py 會檢查）；用詞跟著 README 與桌面 app 的翻譯。
(() => {
  "use strict";

  const STRINGS = {
    "zh-Hant": {
      "title": "NBTV 收聽卡",
      "doc.index": "NBTV 收聽卡",
      "doc.verify": "查驗 NBTV 收聽卡",
      "nav.verify": "查驗收聽卡",
      "nav.home": "領取收聽卡",
      "lang.label": "語言",

      "intro.heading": "上傳錄到的訊號，領一張收聽卡",
      "intro.body": "把你從無線電錄下來的窄頻電視（NBTV）訊號傳上來。解出超過 {min} 幀畫面，而且從訊號裡解出發射電台的呼號，就會產生一張印著那個呼號、有數位簽章的收聽卡。",
      "tips.usb": "用上邊帶（USB）接收，頻率和公告的相差 200 Hz 以內。",
      "tips.format": "存成 WAV 檔，長度 {minutes} 分鐘以內、檔案 {mb} MB 以內。取樣率 8 kHz 以上都可以，單聲道或立體聲皆可。",
      "tips.direct": "直接從電台的音訊輸出或 SDR 軟體錄音，效果最好。",

      "form.file.legend": "錄音檔",
      "form.file.drop": "選擇 WAV 檔，或把檔案拖到這裡",
      "form.file.change": "換一個檔案",
      "form.listener.legend": "卡片上要印的資料（選填）",
      "form.listener.hint": "這些由你自己填寫，本台不查核。它們會印在卡片上並註明是自填，也會顯示在這張卡的查驗頁。全部都可以留空。",
      "form.send.legend": "上傳",
      "form.submit": "上傳並解碼",
      "field.name": "稱呼或呼號",
      "field.date": "收聽日期（UTC）",
      "field.time": "時間（UTC）",
      "field.freq": "頻率（kHz）",
      "field.location": "收聽地點",
      "privacy": "錄音解完就刪除，只留下它的 SHA-256 雜湊。發出的收聽卡（包含你填的資料）會保存下來，供日後查驗。",

      "file.meta": "{duration} · {rate} · {bits} 位元 · {channels} · {size}",
      "file.meta.unknown": "{size}",
      "file.mono": "單聲道",
      "file.stereo": "立體聲",
      "file.channels": "{n} 聲道",

      "progress.starting": "正在確認…",
      "progress.uploading": "上傳中",
      "progress.uploading.detail": "已上傳 {pct}%",
      "progress.queued": "排隊中",
      "progress.queued.next": "下一個就輪到你。",
      "progress.queued.detail": "前面還有 {n} 份錄音。",
      "progress.decoding": "解碼中",
      "progress.decoding.detail": "已經解出 {frames} 幀。",
      "progress.decoding.none": "正在找訊號…",

      "result.ok.heading": "解碼成功，這是你的收聽卡",
      "result.ok.body": "從你的錄音解出 {frames} 幀畫面，其中 {callsign} 幀解出了呼號 {station}。",
      "result.download": "下載收聽卡（PNG）",
      "result.verify": "查驗頁",
      "result.again": "再傳一份錄音",
      "result.keep": "請保存下載的原檔，數位簽章就嵌在裡面。經過通訊軟體或社群網站轉傳的圖片會失去簽章，不過用卡片上的序號或 QR code 仍然可以在本站查驗。",
      "result.card.alt": "收聽卡，序號 {serial}",
      "sheet.heading": "從你的錄音解出來的畫面",
      "sheet.alt": "解出來的 {n} 幀畫面",

      "fail.no_signal.heading": "沒有找到 NBTV 訊號",
      "fail.no_signal.body": "這份錄音裡沒有偵測到任何一幀。",
      "fail.no_signal.tip1": "確認接收模式是上邊帶（USB），頻率和公告的相差 200 Hz 以內。",
      "fail.no_signal.tip2": "確認錄音的時段有發射，而且訊號有被錄進去（不是靜音）。",
      "fail.no_signal.tip3": "錄音音量不要過大而破音。",
      "fail.no_callsign.heading": "收到了畫面，但沒有解出呼號",
      "fail.no_callsign.body": "收到 {pictures} 幀畫面，但沒有一幀解出發射電台的呼號。呼號在每一幀的開頭，訊號太弱或衰落太深時會解不出來。",
      "fail.too_few.heading": "收到的幀數還不夠",
      "fail.too_few.body": "收到 {frames} 幀畫面，要超過 {min} 幀才發卡。把整段發射都錄進來，再試一次。",
      "fail.model_mismatch.heading": "收到了訊號，但解不出畫面",
      "fail.model_mismatch.body": "伺服器的解碼模型和發射端的版本不同。這是本站的問題，請通知電台。",

      "stats.station": "呼號",
      "stats.frames": "有畫面的幀",
      "stats.callsign": "解出呼號的幀",
      "stats.modes": "模式",
      "stats.modes.item": "模式 {mode}：{n} 幀",
      "stats.snr": "訊雜比",
      "stats.snr.value": "中位數 {median} dB，最高 {best} dB",
      "stats.offset": "頻率偏移",
      "stats.seconds": "錄音長度",

      "error.not_wav": "這不是 WAV 檔。請把錄音存成或轉成 WAV 再上傳。",
      "error.unsupported": "這個 WAV 的編碼不支援。請轉成 16 位元 PCM 的 WAV 再上傳。",
      "error.bad_rate": "取樣率 {rate} Hz 不能用，請用 8 kHz 以上的取樣率。",
      "error.empty": "檔案裡沒有聲音資料。",
      "error.too_long": "錄音長 {got}，上限是 {max}。請剪短後再上傳。",
      "error.too_large": "檔案超過 {mb} MB 的上限。可以轉成單聲道、16 位元或較低的取樣率再上傳。",
      "error.upload_interrupted": "上傳中斷了，請再試一次。",
      "error.internal": "伺服器解碼時出了問題，請稍後再試。",
      "error.daily_limit": "你的錄音符合條件，但今天發卡的數量已經到上限。請明天再傳一次。",
      "error.expired": "等太久沒有完成上傳，請重新開始。",
      "error.busy": "現在排隊的錄音太多，請過幾分鐘再試。",
      "error.rate_limited": "你上傳得太頻繁了，請 {minutes} 分鐘後再試。",
      "error.already_active": "你已經有錄音正在處理，等它完成之後再傳下一份。",
      "error.turnstile_failed": "沒有通過「不是機器人」的驗證。請等驗證完成後再按一次。",
      "error.turnstile_unavailable": "驗證服務暫時連不上，請稍後再試。",
      "error.turnstile_load": "載入不了驗證元件（challenges.cloudflare.com）。請關掉會擋它的擴充功能，或換個網路再試。",
      "error.turnstile_widget": "驗證沒有完成，請重新整理頁面再試。",
      "error.bad_request": "有欄位的格式不對，請檢查後再試。",
      "error.network": "連不上伺服器，請檢查網路後再試。",
      "error.failed.heading": "這份錄音不能用",
      "error.server.heading": "沒有完成",

      "duration.ms": "{m} 分 {s} 秒",
      "duration.s": "{s} 秒",

      "verify.heading": "查驗收聽卡",
      "verify.lead": "輸入卡片上的序號，或掃描卡片上的 QR code。查得到，就表示這張卡是本站發的。",
      "verify.serial": "序號",
      "verify.submit": "查驗",
      "verify.checking": "查詢中…",
      "verify.found.heading": "這是本站發行的收聽卡",
      "verify.found.body": "序號 {serial} 的紀錄存在，簽章正確。請比對下面這一張和你手上的卡片是不是一樣。",
      "verify.badsig.heading": "紀錄存在，但簽章不正確",
      "verify.badsig.body": "本站的資料可能損壞了，請通知電台。",
      "verify.missing.heading": "查不到這個序號",
      "verify.missing.body": "本站沒有發過這個序號的卡。請確認序號有沒有打錯；沒有打錯的話，這張卡不是本站發的。",
      "verify.format.heading": "序號的格式不對",
      "verify.format.body": "序號是 12 個英文字母或數字，分成三組，例如 7KQ2-M9XD-4TFA。",
      "verify.self": "以下由聽眾自己填寫，本站沒有查核",
      "verify.card.alt": "本站保存的收聽卡，序號 {serial}",
      "record.serial": "序號",
      "record.station": "電台",
      "record.issued": "發行時間（UTC）",
      "record.frames": "收到幀數",
      "record.frames.value": "{frames} 幀，其中 {callsign} 幀解出呼號",
      "record.audio": "錄音",
      "record.audio.value": "{duration}，SHA-256 {hash}",
      "record.name": "收聽者",
      "record.when": "收聽時間（UTC）",
      "record.freq": "頻率",
      "record.location": "收聽地點",
      "record.blank": "（沒填）",

      "offline.heading": "離線查驗",
      "offline.body": "下載的收聽卡原檔（PNG）裡嵌了紀錄、簽章與憑證，不連本站也能驗。憑證是本台自己簽發的，沒有經過憑證機構：一張根憑證，以及由它簽發、實際用來簽卡片的簽章憑證。所以要以這裡公布的根憑證指紋為準。",
      "offline.fingerprint": "根憑證指紋（SHA-256）",
      "offline.cert": "下載根憑證（PEM）",
      "offline.tool": "下載查驗工具（Python）",
      "offline.note": "查驗工具需要 Python；有 cryptography 套件或系統的 openssl 指令就能驗簽章。經過轉傳、截圖或重新壓縮的圖片已經沒有簽章資料，只能用序號在本站查驗。",
      "offline.c2pa": "原檔另外帶有 C2PA（Content Credentials）的資料，支援 C2PA 的工具讀得到。本台的根憑證不在 C2PA 的信任清單上，所以那些工具驗得出檔案沒有被改過，但會把簽署者列為不認得；把上面的根憑證加進工具的信任錨點之後，才會顯示為受信任。",

      "footer.signed": "收聽卡以本台自己簽發的憑證做數位簽章。",
      "footer.how": "怎麼查驗",
    },

    "zh-Hans": {
      "title": "NBTV 收听卡",
      "doc.index": "NBTV 收听卡",
      "doc.verify": "查验 NBTV 收听卡",
      "nav.verify": "查验收听卡",
      "nav.home": "领取收听卡",
      "lang.label": "语言",

      "intro.heading": "上传录到的信号，领一张收听卡",
      "intro.body": "把你从无线电录下来的窄带电视（NBTV）信号传上来。解出超过 {min} 帧画面，并且从信号里解出发射电台的呼号，就会生成一张印着那个呼号、带数字签名的收听卡。",
      "tips.usb": "用上边带（USB）接收，频率与公告的相差 200 Hz 以内。",
      "tips.format": "保存为 WAV 文件，长度 {minutes} 分钟以内、文件 {mb} MB 以内。采样率 8 kHz 以上都可以，单声道或立体声均可。",
      "tips.direct": "直接从电台的音频输出或 SDR 软件录音，效果最好。",

      "form.file.legend": "录音文件",
      "form.file.drop": "选择 WAV 文件，或把文件拖到这里",
      "form.file.change": "换一个文件",
      "form.listener.legend": "要印在卡片上的资料（选填）",
      "form.listener.hint": "这些由你自己填写，本台不核实。它们会印在卡片上并注明是自填，也会显示在这张卡的查验页。全部都可以留空。",
      "form.send.legend": "上传",
      "form.submit": "上传并解码",
      "field.name": "称呼或呼号",
      "field.date": "收听日期（UTC）",
      "field.time": "时间（UTC）",
      "field.freq": "频率（kHz）",
      "field.location": "收听地点",
      "privacy": "录音解完就删除，只留下它的 SHA-256 哈希值。发出的收听卡（包括你填的资料）会保存下来，供日后查验。",

      "file.meta": "{duration} · {rate} · {bits} 位 · {channels} · {size}",
      "file.meta.unknown": "{size}",
      "file.mono": "单声道",
      "file.stereo": "立体声",
      "file.channels": "{n} 声道",

      "progress.starting": "正在确认…",
      "progress.uploading": "上传中",
      "progress.uploading.detail": "已上传 {pct}%",
      "progress.queued": "排队中",
      "progress.queued.next": "下一个就轮到你。",
      "progress.queued.detail": "前面还有 {n} 份录音。",
      "progress.decoding": "解码中",
      "progress.decoding.detail": "已经解出 {frames} 帧。",
      "progress.decoding.none": "正在找信号…",

      "result.ok.heading": "解码成功，这是你的收听卡",
      "result.ok.body": "从你的录音解出 {frames} 帧画面，其中 {callsign} 帧解出了呼号 {station}。",
      "result.download": "下载收听卡（PNG）",
      "result.verify": "查验页",
      "result.again": "再传一份录音",
      "result.keep": "请保存下载的原文件，数字签名就嵌在里面。经过聊天软件或社交网站转发的图片会失去签名，不过用卡片上的序号或二维码仍然可以在本站查验。",
      "result.card.alt": "收听卡，序号 {serial}",
      "sheet.heading": "从你的录音解出来的画面",
      "sheet.alt": "解出来的 {n} 帧画面",

      "fail.no_signal.heading": "没有找到 NBTV 信号",
      "fail.no_signal.body": "这份录音里没有检测到任何一帧。",
      "fail.no_signal.tip1": "确认接收模式是上边带（USB），频率与公告的相差 200 Hz 以内。",
      "fail.no_signal.tip2": "确认录音的时段有发射，而且信号确实录进去了（不是静音）。",
      "fail.no_signal.tip3": "录音音量不要过大而失真。",
      "fail.no_callsign.heading": "收到了画面，但没有解出呼号",
      "fail.no_callsign.body": "收到 {pictures} 帧画面，但没有一帧解出发射电台的呼号。呼号在每一帧的开头，信号太弱或衰落太深时会解不出来。",
      "fail.too_few.heading": "收到的帧数还不够",
      "fail.too_few.body": "收到 {frames} 帧画面，要超过 {min} 帧才发卡。把整段发射都录下来，再试一次。",
      "fail.model_mismatch.heading": "收到了信号，但解不出画面",
      "fail.model_mismatch.body": "服务器的解码模型和发射端的版本不同。这是本站的问题，请通知电台。",

      "stats.station": "呼号",
      "stats.frames": "有画面的帧",
      "stats.callsign": "解出呼号的帧",
      "stats.modes": "模式",
      "stats.modes.item": "模式 {mode}：{n} 帧",
      "stats.snr": "信噪比",
      "stats.snr.value": "中位数 {median} dB，最高 {best} dB",
      "stats.offset": "频率偏移",
      "stats.seconds": "录音长度",

      "error.not_wav": "这不是 WAV 文件。请把录音保存为或转换成 WAV 再上传。",
      "error.unsupported": "不支持这种 WAV 编码。请转换成 16 位 PCM 的 WAV 再上传。",
      "error.bad_rate": "采样率 {rate} Hz 不能用，请使用 8 kHz 以上的采样率。",
      "error.empty": "文件里没有声音数据。",
      "error.too_long": "录音长 {got}，上限是 {max}。请剪短后再上传。",
      "error.too_large": "文件超过 {mb} MB 的上限。可以转换成单声道、16 位或较低的采样率再上传。",
      "error.upload_interrupted": "上传中断了，请再试一次。",
      "error.internal": "服务器解码时出了问题，请稍后再试。",
      "error.daily_limit": "你的录音符合条件，但今天发卡的数量已经到上限。请明天再传一次。",
      "error.expired": "等待太久没有完成上传，请重新开始。",
      "error.busy": "现在排队的录音太多，请过几分钟再试。",
      "error.rate_limited": "你上传得太频繁了，请 {minutes} 分钟后再试。",
      "error.already_active": "你已经有录音正在处理，等它完成之后再传下一份。",
      "error.turnstile_failed": "没有通过“不是机器人”的验证。请等验证完成后再按一次。",
      "error.turnstile_unavailable": "验证服务暂时连不上，请稍后再试。",
      "error.turnstile_load": "无法加载验证组件（challenges.cloudflare.com）。请关掉会拦截它的扩展程序，或换个网络再试。",
      "error.turnstile_widget": "验证没有完成，请刷新页面再试。",
      "error.bad_request": "有填写项的格式不对，请检查后再试。",
      "error.network": "连不上服务器，请检查网络后再试。",
      "error.failed.heading": "这份录音不能用",
      "error.server.heading": "没有完成",

      "duration.ms": "{m} 分 {s} 秒",
      "duration.s": "{s} 秒",

      "verify.heading": "查验收听卡",
      "verify.lead": "输入卡片上的序号，或扫描卡片上的二维码。查得到，就表示这张卡是本站发的。",
      "verify.serial": "序号",
      "verify.submit": "查验",
      "verify.checking": "查询中…",
      "verify.found.heading": "这是本站发行的收听卡",
      "verify.found.body": "序号 {serial} 的记录存在，签名正确。请比对下面这一张和你手上的卡片是否一样。",
      "verify.badsig.heading": "记录存在，但签名不正确",
      "verify.badsig.body": "本站的数据可能损坏了，请通知电台。",
      "verify.missing.heading": "查不到这个序号",
      "verify.missing.body": "本站没有发过这个序号的卡。请确认序号有没有输错；没有输错的话，这张卡不是本站发的。",
      "verify.format.heading": "序号的格式不对",
      "verify.format.body": "序号是 12 个英文字母或数字，分成三组，例如 7KQ2-M9XD-4TFA。",
      "verify.self": "以下由听众自己填写，本站没有核实",
      "verify.card.alt": "本站保存的收听卡，序号 {serial}",
      "record.serial": "序号",
      "record.station": "电台",
      "record.issued": "发行时间（UTC）",
      "record.frames": "收到帧数",
      "record.frames.value": "{frames} 帧，其中 {callsign} 帧解出呼号",
      "record.audio": "录音",
      "record.audio.value": "{duration}，SHA-256 {hash}",
      "record.name": "收听者",
      "record.when": "收听时间（UTC）",
      "record.freq": "频率",
      "record.location": "收听地点",
      "record.blank": "（未填）",

      "offline.heading": "离线查验",
      "offline.body": "下载的收听卡原文件（PNG）里嵌入了记录、签名与证书，不连接本站也能验证。证书是本台自己签发的，没有经过证书颁发机构：一张根证书，以及由它签发、实际用来给卡片签名的签名证书。所以要以这里公布的根证书指纹为准。",
      "offline.fingerprint": "根证书指纹（SHA-256）",
      "offline.cert": "下载根证书（PEM）",
      "offline.tool": "下载查验工具（Python）",
      "offline.note": "查验工具需要 Python；有 cryptography 包或系统的 openssl 命令就能验证签名。经过转发、截图或重新压缩的图片已经没有签名数据，只能用序号在本站查验。",
      "offline.c2pa": "原文件还带有 C2PA（Content Credentials）的数据，支持 C2PA 的工具可以读取。本台的根证书不在 C2PA 的信任列表上，所以那些工具能验证文件没有被改动，但会把签名者列为未知；把上面的根证书加入工具的信任锚之后，才会显示为受信任。",

      "footer.signed": "收听卡用本台自己签发的证书做数字签名。",
      "footer.how": "如何查验",
    },

    "ja": {
      "title": "NBTV 受信カード",
      "doc.index": "NBTV 受信カード",
      "doc.verify": "NBTV 受信カードの確認",
      "nav.verify": "カードを確認",
      "nav.home": "カードを受け取る",
      "lang.label": "言語",

      "intro.heading": "録音した信号をアップロードして、受信カードを受け取りましょう",
      "intro.body": "無線機で録音した狭帯域テレビ（NBTV）信号をアップロードしてください。{min} フレームを超える画像がデコードされ、信号から送信局のコールサインも読み取れれば、そのコールサインを記載した電子署名付きの受信カードを発行します。",
      "tips.usb": "USB（上側波帯）で受信し、告知された周波数との差を 200 Hz 以内にしてください。",
      "tips.format": "WAV ファイルで保存してください。長さは {minutes} 分以内、サイズは {mb} MB 以内です。サンプリングレートは 8 kHz 以上であれば何でもよく、モノラルでもステレオでも構いません。",
      "tips.direct": "無線機の音声出力や SDR ソフトウェアから直接録音すると、最も良い結果が得られます。",

      "form.file.legend": "録音ファイル",
      "form.file.drop": "WAV ファイルを選択するか、ここにドラッグしてください",
      "form.file.change": "別のファイルを選ぶ",
      "form.listener.legend": "カードに印字する内容（任意）",
      "form.listener.hint": "これらはご自身で記入するもので、当局は内容を確認しません。自己申告と明記したうえでカードに印字され、このカードの確認ページにも表示されます。すべて空欄でも構いません。",
      "form.send.legend": "アップロード",
      "form.submit": "アップロードしてデコード",
      "field.name": "お名前またはコールサイン",
      "field.date": "受信日（UTC）",
      "field.time": "時刻（UTC）",
      "field.freq": "周波数（kHz）",
      "field.location": "受信場所",
      "privacy": "録音はデコードが終わりしだい削除し、SHA-256 ハッシュだけを残します。発行した受信カード（記入された内容を含む）は、あとで確認できるように保存されます。",

      "file.meta": "{duration} · {rate} · {bits} ビット · {channels} · {size}",
      "file.meta.unknown": "{size}",
      "file.mono": "モノラル",
      "file.stereo": "ステレオ",
      "file.channels": "{n} チャンネル",

      "progress.starting": "確認中…",
      "progress.uploading": "アップロード中",
      "progress.uploading.detail": "{pct}% アップロード済み",
      "progress.queued": "順番待ち",
      "progress.queued.next": "次があなたの番です。",
      "progress.queued.detail": "あなたの前に {n} 件の録音があります。",
      "progress.decoding": "デコード中",
      "progress.decoding.detail": "これまでに {frames} フレームをデコードしました。",
      "progress.decoding.none": "信号を探しています…",

      "result.ok.heading": "デコード成功。こちらがあなたの受信カードです",
      "result.ok.body": "録音から {frames} フレームの画像をデコードし、そのうち {callsign} フレームでコールサイン {station} を読み取りました。",
      "result.download": "受信カードをダウンロード（PNG）",
      "result.verify": "確認ページ",
      "result.again": "別の録音をアップロード",
      "result.keep": "ダウンロードした元のファイルを保存してください。電子署名はその中に埋め込まれています。メッセージアプリや SNS で転送された画像からは署名が失われますが、カードのシリアル番号や QR コードを使えば、このサイトで引き続き確認できます。",
      "result.card.alt": "受信カード、シリアル番号 {serial}",
      "sheet.heading": "録音からデコードされた画像",
      "sheet.alt": "デコードされた {n} フレームの画像",

      "fail.no_signal.heading": "NBTV 信号が見つかりませんでした",
      "fail.no_signal.body": "この録音からは 1 フレームも検出されませんでした。",
      "fail.no_signal.tip1": "受信モードが USB（上側波帯）で、告知された周波数との差が 200 Hz 以内であることを確認してください。",
      "fail.no_signal.tip2": "録音した時間帯に送信があり、信号が実際に録音されている（無音ではない）ことを確認してください。",
      "fail.no_signal.tip3": "録音レベルが大きすぎて歪まないようにしてください。",
      "fail.no_callsign.heading": "画像は受信できましたが、コールサインを読み取れませんでした",
      "fail.no_callsign.body": "{pictures} フレームの画像を受信しましたが、送信局のコールサインを読み取れたフレームはありませんでした。コールサインは各フレームの先頭にあり、信号が弱すぎるときやフェージングが深いときは読み取れません。",
      "fail.too_few.heading": "受信したフレーム数がまだ足りません",
      "fail.too_few.body": "{frames} フレームの画像を受信しました。カードの発行には {min} フレームを超える必要があります。送信全体を録音して、もう一度お試しください。",
      "fail.model_mismatch.heading": "信号は受信できましたが、画像をデコードできません",
      "fail.model_mismatch.body": "サーバーのデコードモデルが送信側のバージョンと異なっています。これは当サイト側の問題ですので、局にお知らせください。",

      "stats.station": "コールサイン",
      "stats.frames": "画像のあるフレーム",
      "stats.callsign": "コールサインを読み取れたフレーム",
      "stats.modes": "モード",
      "stats.modes.item": "モード {mode}：{n} フレーム",
      "stats.snr": "SN 比",
      "stats.snr.value": "中央値 {median} dB、最高 {best} dB",
      "stats.offset": "周波数オフセット",
      "stats.seconds": "録音の長さ",

      "error.not_wav": "これは WAV ファイルではありません。録音を WAV で保存するか、WAV に変換してからアップロードしてください。",
      "error.unsupported": "この WAV のエンコード形式には対応していません。16 ビット PCM の WAV に変換してからアップロードしてください。",
      "error.bad_rate": "サンプリングレート {rate} Hz は使用できません。8 kHz 以上のサンプリングレートを使用してください。",
      "error.empty": "ファイルに音声データがありません。",
      "error.too_long": "録音の長さは {got} で、上限は {max} です。短くしてからアップロードしてください。",
      "error.too_large": "ファイルが上限の {mb} MB を超えています。モノラル、16 ビット、または低いサンプリングレートに変換すると小さくなります。",
      "error.upload_interrupted": "アップロードが中断されました。もう一度お試しください。",
      "error.internal": "サーバーでのデコード中に問題が発生しました。しばらくしてからもう一度お試しください。",
      "error.daily_limit": "録音は条件を満たしていますが、本日のカード発行数が上限に達しました。明日もう一度アップロードしてください。",
      "error.expired": "時間内にアップロードが完了しませんでした。最初からやり直してください。",
      "error.busy": "現在、順番待ちの録音が多すぎます。数分後にもう一度お試しください。",
      "error.rate_limited": "アップロードの頻度が高すぎます。{minutes} 分後にもう一度お試しください。",
      "error.already_active": "処理中の録音がすでにあります。完了してから次の録音をアップロードしてください。",
      "error.turnstile_failed": "「ロボットではない」ことの確認を通過できませんでした。確認が終わるのを待ってから、もう一度ボタンを押してください。",
      "error.turnstile_unavailable": "確認サービスに一時的に接続できません。しばらくしてからもう一度お試しください。",
      "error.turnstile_load": "確認用のウィジェット（challenges.cloudflare.com）を読み込めませんでした。これをブロックする拡張機能を無効にするか、別のネットワークでお試しください。",
      "error.turnstile_widget": "確認が完了しませんでした。ページを再読み込みして、もう一度お試しください。",
      "error.bad_request": "形式が正しくない項目があります。確認してからもう一度お試しください。",
      "error.network": "サーバーに接続できません。ネットワークを確認してからもう一度お試しください。",
      "error.failed.heading": "この録音は使用できません",
      "error.server.heading": "完了できませんでした",

      "duration.ms": "{m} 分 {s} 秒",
      "duration.s": "{s} 秒",

      "verify.heading": "受信カードの確認",
      "verify.lead": "カードに印字されたシリアル番号を入力するか、カードの QR コードを読み取ってください。見つかれば、そのカードはこのサイトが発行したものです。",
      "verify.serial": "シリアル番号",
      "verify.submit": "確認",
      "verify.checking": "照会中…",
      "verify.found.heading": "このサイトが発行した受信カードです",
      "verify.found.body": "シリアル番号 {serial} の記録があり、署名も正しいものです。下のカードとお手元のカードが同じかどうか見比べてください。",
      "verify.badsig.heading": "記録はありますが、署名が正しくありません",
      "verify.badsig.body": "このサイトのデータが破損している可能性があります。局にお知らせください。",
      "verify.missing.heading": "このシリアル番号は見つかりません",
      "verify.missing.body": "このサイトはこのシリアル番号のカードを発行していません。入力に誤りがないか確認してください。誤りがなければ、そのカードはこのサイトが発行したものではありません。",
      "verify.format.heading": "シリアル番号の形式が正しくありません",
      "verify.format.body": "シリアル番号は英数字 12 文字で、3 つのグループに分かれています。例：7KQ2-M9XD-4TFA",
      "verify.self": "以下は受信者による自己申告で、このサイトは確認していません",
      "verify.card.alt": "このサイトが保存している受信カード、シリアル番号 {serial}",
      "record.serial": "シリアル番号",
      "record.station": "局",
      "record.issued": "発行日時（UTC）",
      "record.frames": "受信フレーム数",
      "record.frames.value": "{frames} フレーム（うち {callsign} フレームでコールサインを読み取り）",
      "record.audio": "録音",
      "record.audio.value": "{duration}、SHA-256 {hash}",
      "record.name": "受信者",
      "record.when": "受信日時（UTC）",
      "record.freq": "周波数",
      "record.location": "受信場所",
      "record.blank": "（未記入）",

      "offline.heading": "オフラインでの確認",
      "offline.body": "ダウンロードした受信カードの元ファイル（PNG）には、記録、署名、証明書が埋め込まれているため、このサイトに接続しなくても検証できます。証明書は当局が自ら発行したもので、認証局は関与していません。ルート証明書が 1 つと、それによって発行され、実際にカードへの署名に使われる署名用証明書があります。そのため、ここに掲載しているルート証明書のフィンガープリントを基準にしてください。",
      "offline.fingerprint": "ルート証明書のフィンガープリント（SHA-256）",
      "offline.cert": "ルート証明書をダウンロード（PEM）",
      "offline.tool": "検証ツールをダウンロード（Python）",
      "offline.note": "検証ツールには Python が必要です。cryptography パッケージまたはシステムの openssl コマンドがあれば署名を検証できます。転送、スクリーンショット、再圧縮された画像には署名データが残っていないため、シリアル番号を使ってこのサイトで確認するしかありません。",
      "offline.c2pa": "元ファイルには C2PA（Content Credentials）のデータも含まれており、C2PA に対応したツールで読み取れます。当局のルート証明書は C2PA の信頼リストに載っていないため、それらのツールはファイルが改変されていないことは確認できますが、署名者を不明として表示します。上のルート証明書をツールのトラストアンカーに追加すると、信頼済みとして表示されます。",

      "footer.signed": "受信カードは、当局が自ら発行した証明書で電子署名されています。",
      "footer.how": "確認方法",
    },

    "es": {
      "title": "Tarjeta de recepción NBTV",
      "doc.index": "Tarjeta de recepción NBTV",
      "doc.verify": "Verificar una tarjeta de recepción NBTV",
      "nav.verify": "Verificar una tarjeta",
      "nav.home": "Obtener una tarjeta",
      "lang.label": "Idioma",

      "intro.heading": "Sube tu grabación y obtén una tarjeta de recepción",
      "intro.body": "Sube una grabación de una señal de televisión de banda estrecha (NBTV) hecha con tu receptor. Si se decodifican más de {min} imágenes y se lee en la señal el indicativo de la estación emisora, se genera una tarjeta de recepción con ese indicativo y con firma digital.",
      "tips.usb": "Recibe en USB (banda lateral superior), a menos de 200 Hz de la frecuencia anunciada.",
      "tips.format": "Guárdala como archivo WAV, de hasta {minutes} minutos y {mb} MB. Sirve cualquier frecuencia de muestreo a partir de 8 kHz, en mono o en estéreo.",
      "tips.direct": "Se obtienen los mejores resultados grabando directamente de la salida de audio del receptor o desde un programa SDR.",

      "form.file.legend": "Grabación",
      "form.file.drop": "Elige un archivo WAV o arrástralo aquí",
      "form.file.change": "Elegir otro archivo",
      "form.listener.legend": "Datos que se imprimirán en la tarjeta (opcional)",
      "form.listener.hint": "Estos datos los escribes tú; la estación no los comprueba. Se imprimen en la tarjeta, indicando que son declarados por el oyente, y se muestran en la página de verificación de la tarjeta. Puedes dejarlos todos en blanco.",
      "form.send.legend": "Subir",
      "form.submit": "Subir y decodificar",
      "field.name": "Nombre o indicativo",
      "field.date": "Fecha de recepción (UTC)",
      "field.time": "Hora (UTC)",
      "field.freq": "Frecuencia (kHz)",
      "field.location": "Lugar de recepción",
      "privacy": "La grabación se borra en cuanto termina la decodificación; solo se conserva su hash SHA-256. Las tarjetas emitidas, incluidos los datos que hayas escrito, se guardan para poder verificarlas más adelante.",

      "file.meta": "{duration} · {rate} · {bits} bits · {channels} · {size}",
      "file.meta.unknown": "{size}",
      "file.mono": "mono",
      "file.stereo": "estéreo",
      "file.channels": "{n} canales",

      "progress.starting": "Comprobando…",
      "progress.uploading": "Subiendo",
      "progress.uploading.detail": "{pct} % subido",
      "progress.queued": "En cola",
      "progress.queued.next": "Tu grabación es la siguiente.",
      "progress.queued.detail": "Hay {n} grabaciones antes que la tuya.",
      "progress.decoding": "Decodificando",
      "progress.decoding.detail": "{frames} tramas decodificadas hasta ahora.",
      "progress.decoding.none": "Buscando la señal…",

      "result.ok.heading": "Decodificación correcta: esta es tu tarjeta de recepción",
      "result.ok.body": "Se decodificaron {frames} imágenes de tu grabación y en {callsign} de ellas se leyó el indicativo {station}.",
      "result.download": "Descargar la tarjeta (PNG)",
      "result.verify": "Página de verificación",
      "result.again": "Subir otra grabación",
      "result.keep": "Conserva el archivo descargado: la firma digital va incrustada en él. Las imágenes reenviadas por aplicaciones de mensajería o redes sociales pierden la firma, pero el número de serie y el código QR de la tarjeta se pueden seguir comprobando en este sitio.",
      "result.card.alt": "Tarjeta de recepción, número de serie {serial}",
      "sheet.heading": "Imágenes decodificadas de tu grabación",
      "sheet.alt": "{n} imágenes decodificadas",

      "fail.no_signal.heading": "No se encontró ninguna señal NBTV",
      "fail.no_signal.body": "No se detectó ni una sola trama en esta grabación.",
      "fail.no_signal.tip1": "Comprueba que el receptor estaba en USB (banda lateral superior) y a menos de 200 Hz de la frecuencia anunciada.",
      "fail.no_signal.tip2": "Comprueba que la grabación coincide con una transmisión y que la señal quedó grabada de verdad (no es silencio).",
      "fail.no_signal.tip3": "Mantén el nivel de grabación lo bastante bajo para que no sature.",
      "fail.no_callsign.heading": "Se recibieron imágenes, pero no se pudo leer ningún indicativo",
      "fail.no_callsign.body": "Se recibieron {pictures} imágenes, pero en ninguna se pudo leer el indicativo de la estación emisora. El indicativo va al principio de cada trama y se pierde cuando la señal es demasiado débil o el desvanecimiento es profundo.",
      "fail.too_few.heading": "Todavía no hay suficientes tramas",
      "fail.too_few.body": "Se recibieron {frames} imágenes; para emitir una tarjeta hacen falta más de {min}. Graba la transmisión completa e inténtalo de nuevo.",
      "fail.model_mismatch.heading": "Se recibió la señal, pero no se pudieron decodificar las imágenes",
      "fail.model_mismatch.body": "El modelo de decodificación de este servidor es distinto del que usa el transmisor. Es un problema nuestro: por favor, avisa a la estación.",

      "stats.station": "Indicativo",
      "stats.frames": "Tramas con imagen",
      "stats.callsign": "Tramas con el indicativo",
      "stats.modes": "Modos",
      "stats.modes.item": "modo {mode}: {n} tramas",
      "stats.snr": "Relación señal/ruido",
      "stats.snr.value": "mediana {median} dB, máxima {best} dB",
      "stats.offset": "Desviación de frecuencia",
      "stats.seconds": "Duración de la grabación",

      "error.not_wav": "Esto no es un archivo WAV. Guarda o convierte la grabación a WAV antes de subirla.",
      "error.unsupported": "Esta codificación de WAV no es compatible. Conviértela a WAV PCM de 16 bits antes de subirla.",
      "error.bad_rate": "No se puede usar una frecuencia de muestreo de {rate} Hz. Graba a 8 kHz o más.",
      "error.empty": "El archivo no contiene audio.",
      "error.too_long": "La grabación dura {got} y el límite es {max}. Recórtala y vuelve a subirla.",
      "error.too_large": "El archivo supera el límite de {mb} MB. Puedes reducirlo convirtiéndolo a mono, a 16 bits o a una frecuencia de muestreo menor.",
      "error.upload_interrupted": "La subida se interrumpió. Inténtalo de nuevo.",
      "error.internal": "Algo falló en el servidor durante la decodificación. Inténtalo de nuevo más tarde.",
      "error.daily_limit": "Tu grabación cumple los requisitos, pero ya se alcanzó el límite de tarjetas emitidas por hoy. Vuelve a subirla mañana.",
      "error.expired": "La subida no se completó a tiempo. Empieza de nuevo.",
      "error.busy": "Ahora mismo hay demasiadas grabaciones en cola. Inténtalo de nuevo en unos minutos.",
      "error.rate_limited": "Estás subiendo grabaciones con demasiada frecuencia. Inténtalo de nuevo dentro de {minutes} minutos.",
      "error.already_active": "Ya tienes grabaciones en proceso. Espera a que terminen antes de subir otra.",
      "error.turnstile_failed": "No se superó la comprobación de «no soy un robot». Espera a que termine y vuelve a pulsar el botón.",
      "error.turnstile_unavailable": "No se puede contactar con el servicio de verificación en este momento. Inténtalo de nuevo más tarde.",
      "error.turnstile_load": "No se pudo cargar el componente de verificación (challenges.cloudflare.com). Desactiva cualquier extensión que lo bloquee o prueba con otra red.",
      "error.turnstile_widget": "La comprobación no se completó. Recarga la página e inténtalo de nuevo.",
      "error.bad_request": "Alguno de los campos tiene un formato incorrecto. Revísalo e inténtalo de nuevo.",
      "error.network": "No se puede contactar con el servidor. Comprueba tu conexión e inténtalo de nuevo.",
      "error.failed.heading": "Esta grabación no se puede usar",
      "error.server.heading": "No se completó",

      "duration.ms": "{m} min {s} s",
      "duration.s": "{s} s",

      "verify.heading": "Verificar una tarjeta de recepción",
      "verify.lead": "Escribe el número de serie impreso en la tarjeta o escanea su código QR. Si aparece aquí, la tarjeta fue emitida por este sitio.",
      "verify.serial": "Número de serie",
      "verify.submit": "Verificar",
      "verify.checking": "Consultando…",
      "verify.found.heading": "Esta tarjeta fue emitida por este sitio",
      "verify.found.body": "Existe un registro con el número de serie {serial} y su firma es válida. Compara la tarjeta de abajo con la que tienes.",
      "verify.badsig.heading": "Existe un registro, pero su firma no es válida",
      "verify.badsig.body": "Es posible que los datos de este sitio estén dañados. Por favor, avisa a la estación.",
      "verify.missing.heading": "No se encontró ese número de serie",
      "verify.missing.body": "Este sitio no ha emitido ninguna tarjeta con ese número de serie. Comprueba que esté bien escrito; si lo está, la tarjeta no fue emitida aquí.",
      "verify.format.heading": "El formato del número de serie no es correcto",
      "verify.format.body": "Un número de serie tiene 12 letras o cifras en tres grupos, por ejemplo 7KQ2-M9XD-4TFA.",
      "verify.self": "Datos declarados por el oyente; este sitio no los ha comprobado",
      "verify.card.alt": "La tarjeta tal como la guarda este sitio, número de serie {serial}",
      "record.serial": "Número de serie",
      "record.station": "Estación",
      "record.issued": "Emitida (UTC)",
      "record.frames": "Tramas recibidas",
      "record.frames.value": "{frames} tramas, {callsign} de ellas con el indicativo",
      "record.audio": "Grabación",
      "record.audio.value": "{duration}, SHA-256 {hash}",
      "record.name": "Oyente",
      "record.when": "Recibida (UTC)",
      "record.freq": "Frecuencia",
      "record.location": "Lugar de recepción",
      "record.blank": "(sin indicar)",

      "offline.heading": "Verificación sin conexión",
      "offline.body": "El archivo original de la tarjeta (PNG) lleva incrustados su registro, la firma y los certificados, así que se puede comprobar sin este sitio. Los certificados los emite la propia estación, sin ninguna autoridad de certificación: un certificado raíz y un certificado de firma emitido por él, que es el que firma realmente las tarjetas. Por eso, la referencia es la huella del certificado raíz publicada aquí.",
      "offline.fingerprint": "Huella del certificado raíz (SHA-256)",
      "offline.cert": "Descargar el certificado raíz (PEM)",
      "offline.tool": "Descargar la herramienta de verificación (Python)",
      "offline.note": "La herramienta de verificación necesita Python; las firmas se comprueban con el paquete cryptography o con el comando openssl del sistema. Una imagen reenviada, capturada de pantalla o recomprimida ya no conserva la firma; en ese caso solo se puede verificar con su número de serie en este sitio.",
      "offline.c2pa": "El archivo original incluye además datos C2PA (Content Credentials) que pueden leer las herramientas compatibles con C2PA. El certificado raíz de la estación no está en la lista de confianza de C2PA, así que esas herramientas pueden confirmar que el archivo no se ha modificado, pero mostrarán al firmante como no reconocido; solo aparecerá como de confianza después de añadir el certificado raíz de arriba a las anclas de confianza de la herramienta.",

      "footer.signed": "Las tarjetas se firman digitalmente con un certificado emitido por la propia estación.",
      "footer.how": "Cómo verificar",
    },

    "en": {
      "title": "NBTV Reception Card",
      "doc.index": "NBTV Reception Card",
      "doc.verify": "Verify an NBTV reception card",
      "nav.verify": "Verify a card",
      "nav.home": "Get a card",
      "lang.label": "Language",

      "intro.heading": "Upload your recording, get a reception card",
      "intro.body": "Upload a recording of a narrow-band television (NBTV) signal made from your radio. If more than {min} pictures decode and the transmitting station's callsign is read from the signal, you get a digitally signed reception card showing that callsign.",
      "tips.usb": "Receive in USB, within 200 Hz of the announced frequency.",
      "tips.format": "Save as a WAV file, up to {minutes} minutes and {mb} MB. Any sample rate from 8 kHz up works, mono or stereo.",
      "tips.direct": "For best results record straight from the radio's audio output or from SDR software.",

      "form.file.legend": "Recording",
      "form.file.drop": "Choose a WAV file, or drop it here",
      "form.file.change": "Choose a different file",
      "form.listener.legend": "Details to print on the card (optional)",
      "form.listener.hint": "You fill these in yourself; the station does not check them. They are printed on the card, marked as self-reported, and shown on the card's verification page. You may leave any of them blank.",
      "form.send.legend": "Upload",
      "form.submit": "Upload and decode",
      "field.name": "Name or callsign",
      "field.date": "Date received (UTC)",
      "field.time": "Time (UTC)",
      "field.freq": "Frequency (kHz)",
      "field.location": "Location",
      "privacy": "Your recording is deleted as soon as it has been decoded; only its SHA-256 hash is kept. Issued cards, including the details you entered, are stored so they can be verified later.",

      "file.meta": "{duration} · {rate} · {bits}-bit · {channels} · {size}",
      "file.meta.unknown": "{size}",
      "file.mono": "mono",
      "file.stereo": "stereo",
      "file.channels": "{n} channels",

      "progress.starting": "Checking…",
      "progress.uploading": "Uploading",
      "progress.uploading.detail": "{pct}% uploaded",
      "progress.queued": "Waiting in line",
      "progress.queued.next": "You are next.",
      "progress.queued.detail": "{n} recordings ahead of yours.",
      "progress.decoding": "Decoding",
      "progress.decoding.detail": "{frames} frames decoded so far.",
      "progress.decoding.none": "Looking for the signal…",

      "result.ok.heading": "Decoded — here is your reception card",
      "result.ok.body": "{frames} pictures were decoded from your recording, and the callsign {station} was read from {callsign} of them.",
      "result.download": "Download card (PNG)",
      "result.verify": "Verification page",
      "result.again": "Upload another recording",
      "result.keep": "Keep the downloaded file: the digital signature is embedded in it. Images passed on through messaging apps or social media lose the signature, but the serial number and QR code on the card can still be checked on this site.",
      "result.card.alt": "Reception card, serial {serial}",
      "sheet.heading": "Pictures decoded from your recording",
      "sheet.alt": "{n} decoded pictures",

      "fail.no_signal.heading": "No NBTV signal found",
      "fail.no_signal.body": "Not a single frame was detected in this recording.",
      "fail.no_signal.tip1": "Check that the receiver was in USB and within 200 Hz of the announced frequency.",
      "fail.no_signal.tip2": "Check that the recording covers a transmission and actually contains the signal (not silence).",
      "fail.no_signal.tip3": "Keep the recording level low enough that it does not clip.",
      "fail.no_callsign.heading": "Pictures were received, but no callsign",
      "fail.no_callsign.body": "{pictures} pictures were received, but a callsign could not be read from any of them. The callsign is sent at the start of every frame and is lost when the signal is too weak or fading is deep.",
      "fail.too_few.heading": "Not enough frames yet",
      "fail.too_few.body": "{frames} pictures were received; a card needs more than {min}. Record the whole transmission and try again.",
      "fail.model_mismatch.heading": "The signal was received, but the pictures could not be decoded",
      "fail.model_mismatch.body": "The decoder model on this server differs from the transmitter's. That is a problem on our side — please tell the station.",

      "stats.station": "Callsign",
      "stats.frames": "Frames with a picture",
      "stats.callsign": "Frames with the callsign",
      "stats.modes": "Modes",
      "stats.modes.item": "mode {mode}: {n} frames",
      "stats.snr": "Signal-to-noise ratio",
      "stats.snr.value": "median {median} dB, best {best} dB",
      "stats.offset": "Frequency offset",
      "stats.seconds": "Recording length",

      "error.not_wav": "This is not a WAV file. Please save or convert the recording to WAV first.",
      "error.unsupported": "This WAV encoding is not supported. Please convert it to 16-bit PCM WAV.",
      "error.bad_rate": "A sample rate of {rate} Hz cannot be used. Please record at 8 kHz or higher.",
      "error.empty": "The file contains no audio.",
      "error.too_long": "The recording is {got} long; the limit is {max}. Please trim it and upload again.",
      "error.too_large": "The file is over the {mb} MB limit. Converting to mono, 16-bit or a lower sample rate will make it smaller.",
      "error.upload_interrupted": "The upload was interrupted. Please try again.",
      "error.internal": "Something went wrong while decoding on the server. Please try again later.",
      "error.daily_limit": "Your recording qualifies, but today's limit on issued cards has been reached. Please upload it again tomorrow.",
      "error.expired": "The upload was not completed in time. Please start again.",
      "error.busy": "Too many recordings are waiting right now. Please try again in a few minutes.",
      "error.rate_limited": "You are uploading too often. Please try again in {minutes} minutes.",
      "error.already_active": "You already have recordings being processed. Wait for them to finish before uploading another.",
      "error.turnstile_failed": "The \"not a robot\" check did not pass. Wait for it to finish, then press the button again.",
      "error.turnstile_unavailable": "The verification service cannot be reached right now. Please try again later.",
      "error.turnstile_load": "The verification widget (challenges.cloudflare.com) could not be loaded. Turn off any extension that blocks it, or try another network.",
      "error.turnstile_widget": "The check did not complete. Please reload the page and try again.",
      "error.bad_request": "One of the fields has an invalid value. Please check and try again.",
      "error.network": "The server cannot be reached. Please check your connection and try again.",
      "error.failed.heading": "This recording cannot be used",
      "error.server.heading": "Not completed",

      "duration.ms": "{m} min {s} s",
      "duration.s": "{s} s",

      "verify.heading": "Verify a reception card",
      "verify.lead": "Enter the serial number printed on the card, or scan its QR code. If it is found here, the card was issued by this site.",
      "verify.serial": "Serial number",
      "verify.submit": "Verify",
      "verify.checking": "Looking it up…",
      "verify.found.heading": "This card was issued by this site",
      "verify.found.body": "A record for serial {serial} exists and its signature is valid. Compare the card below with the one you have.",
      "verify.badsig.heading": "A record exists, but its signature is not valid",
      "verify.badsig.body": "The data on this site may be damaged. Please tell the station.",
      "verify.missing.heading": "Serial number not found",
      "verify.missing.body": "This site has not issued a card with that serial. Check it for typing mistakes; if it is correct, the card was not issued here.",
      "verify.format.heading": "That does not look like a serial number",
      "verify.format.body": "A serial is 12 letters or digits in three groups, for example 7KQ2-M9XD-4TFA.",
      "verify.self": "Reported by the listener; not checked by this site",
      "verify.card.alt": "The card as stored by this site, serial {serial}",
      "record.serial": "Serial number",
      "record.station": "Station",
      "record.issued": "Issued (UTC)",
      "record.frames": "Frames received",
      "record.frames.value": "{frames} frames, {callsign} of them with the callsign",
      "record.audio": "Recording",
      "record.audio.value": "{duration}, SHA-256 {hash}",
      "record.name": "Listener",
      "record.when": "Received (UTC)",
      "record.freq": "Frequency",
      "record.location": "Location",
      "record.blank": "(not given)",

      "offline.heading": "Verifying offline",
      "offline.body": "The original card file (PNG) carries its record, signature and certificates, so it can be checked without this site. The certificates are issued by the station itself, with no certificate authority involved: a root certificate, and a signing certificate issued by it that actually signs the cards. Trust therefore rests on the root fingerprint published here.",
      "offline.fingerprint": "Root certificate fingerprint (SHA-256)",
      "offline.cert": "Download root certificate (PEM)",
      "offline.tool": "Download the verifier (Python)",
      "offline.note": "The verifier needs Python; signatures are checked with the cryptography package or the system's openssl command. An image that has been re-sent, screenshotted or recompressed no longer carries the signature; use its serial number on this site instead.",
      "offline.c2pa": "The original file also carries C2PA (Content Credentials) data that C2PA-aware tools can read. The station's root certificate is not on the C2PA trust list, so those tools can confirm the file is unaltered but will show the signer as unrecognized; it is shown as trusted only after the root certificate above is added to the tool's trust anchors.",

      "footer.signed": "Cards are digitally signed with the station's own self-issued certificate.",
      "footer.how": "How to verify",
    },
  };

  // 本站有的語言，照這個順序列在右上角的選單裡；名稱用各個語言自己的寫法。
  const LANGUAGES = [
    ["zh-Hant", "繁體中文"],
    ["zh-Hans", "简体中文"],
    ["ja", "日本語"],
    ["es", "Español"],
    ["en", "English"],
  ];

  // 照瀏覽器列的偏好語言，依序找第一個本站有的；都沒有就用英文。
  // 中文：寫明簡體（Hans）的用簡體；寫明繁體（Hant）或地區是台灣、香港、澳門的用繁體；
  // 其餘的（中國、新加坡、馬來西亞，或只寫 zh）用簡體。
  function preferred(tags) {
    for (const raw of tags) {
      const tag = String(raw || "").toLowerCase();
      if (tag === "zh" || tag.startsWith("zh-")) {
        if (tag.includes("hans")) return "zh-Hans";
        return tag.includes("hant") || /-(tw|hk|mo)\b/.test(tag) ? "zh-Hant" : "zh-Hans";
      }
      for (const code of ["ja", "es", "en"]) {
        if (tag === code || tag.startsWith(code + "-")) return code;
      }
    }
    return "en";
  }

  // 選過的語言記在 localStorage；沒選過（或不能用 localStorage）就照瀏覽器的語言
  let saved = null;
  try { saved = localStorage.getItem("nbtv-lang"); } catch (_) { /* 照瀏覽器的語言 */ }
  const wanted = navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language];
  let lang = saved && STRINGS[saved] ? saved : preferred(wanted);

  const vars = {};       // 靜態文字裡要代入的值（電台呼號、門檻…），拿到設定之後填
  const listeners = [];  // 換語言時要重畫的東西

  function t(key, extra) {
    const text = STRINGS[lang][key] ?? STRINGS["zh-Hant"][key] ?? key;
    const all = Object.assign({}, vars, extra || {});
    return text.replace(/\{(\w+)\}/g, (_, name) => (name in all ? String(all[name]) : ""));
  }

  // 文字裡要代入的值（電台呼號、門檻…）還沒拿到時先不顯示，免得出現缺字的句子
  function ready(key) {
    const text = STRINGS[lang][key] ?? STRINGS["zh-Hant"][key] ?? "";
    return [...text.matchAll(/\{(\w+)\}/g)].every((match) => match[1] in vars);
  }

  function apply() {
    document.documentElement.lang = lang;
    document.querySelectorAll("[data-i18n]").forEach((el) => {
      if (ready(el.dataset.i18n)) el.textContent = t(el.dataset.i18n);
    });
    const menu = document.getElementById("language");
    if (menu) {
      menu.value = lang;
      menu.setAttribute("aria-label", t("lang.label"));
    }
    listeners.forEach((fn) => fn());
  }

  function setLanguage(next) {
    if (!STRINGS[next]) return;
    lang = next;
    try { localStorage.setItem("nbtv-lang", next); } catch (_) { /* 沒關係 */ }
    apply();
  }

  function duration(seconds) {
    const total = Math.round(seconds);
    const m = Math.floor(total / 60), s = total % 60;
    return m > 0 ? t("duration.ms", { m, s: String(s).padStart(2, "0") }) : t("duration.s", { s });
  }

  document.addEventListener("DOMContentLoaded", () => {
    // 語言選單的項目由這裡填：頁面裡只放一個空的、先藏起來的 <select id="language">
    const menu = document.getElementById("language");
    if (menu) {
      for (const [code, name] of LANGUAGES) {
        const option = document.createElement("option");
        option.value = code;
        option.lang = code;  // 各個語言的名稱用它自己的字型顯示
        option.textContent = name;
        menu.appendChild(option);
      }
      menu.addEventListener("change", () => setLanguage(menu.value));
      menu.hidden = false;
    }
    apply();
  });

  window.NBTV = {
    t, apply, duration, vars,
    get lang() { return lang; },
    languages: LANGUAGES.map(([code]) => code),
    onLanguage(fn) { listeners.push(fn); },
  };
})();
