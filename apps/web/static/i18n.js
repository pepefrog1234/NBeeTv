// 頁面上的文字：繁體中文與英文。兩個頁面共用。
// 用法：元素加 data-i18n="鍵"，或在程式裡呼叫 NBTV.t("鍵", {變數})。文字一律用 textContent 放進頁面。
(() => {
  "use strict";

  const STRINGS = {
    "zh-Hant": {
      "title": "NBTV 收聽卡",
      "doc.index": "NBTV 收聽卡",
      "doc.verify": "查驗 NBTV 收聽卡",
      "nav.verify": "查驗收聽卡",
      "nav.home": "領取收聽卡",
      "lang.switch": "EN",
      "lang.label": "Switch to English",

      "intro.heading": "上傳錄到的訊號，領一張收聽卡",
      "intro.body": "把你從無線電錄下來的 {station} 窄頻電視（NBTV）訊號傳上來。解出超過 {min} 幀畫面，而且從訊號裡解出本台的呼號，就會產生一張有數位簽章的收聽卡。",
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
      "result.ok.body": "從你的錄音解出 {frames} 幀畫面，其中 {callsign} 幀解出了 {station} 的呼號。",
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
      "fail.no_callsign.heading": "收到了畫面，但沒有解出 {station} 的呼號",
      "fail.no_callsign.body": "收到 {pictures} 幀畫面，但沒有一幀解出本台的呼號。呼號在每一幀的開頭，訊號太弱或衰落太深時會解不出來。",
      "fail.no_callsign.other": "收到 {pictures} 幀畫面，但解出來的呼號是 {others}，不是本台（{station}）的發射。",
      "fail.too_few.heading": "收到的幀數還不夠",
      "fail.too_few.body": "收到 {frames} 幀畫面，要超過 {min} 幀才發卡。把整段發射都錄進來，再試一次。",
      "fail.model_mismatch.heading": "收到了訊號，但解不出畫面",
      "fail.model_mismatch.body": "伺服器的解碼模型和發射端的版本不同。這是本站的問題，請通知電台。",

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

    "en": {
      "title": "NBTV Reception Card",
      "doc.index": "NBTV Reception Card",
      "doc.verify": "Verify an NBTV reception card",
      "nav.verify": "Verify a card",
      "nav.home": "Get a card",
      "lang.switch": "中文",
      "lang.label": "切換到中文",

      "intro.heading": "Upload your recording, get a reception card",
      "intro.body": "Upload a recording of {station}'s narrow-band television (NBTV) signal made from your radio. If more than {min} pictures decode and our callsign is read from the signal, you get a digitally signed reception card.",
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
      "fail.no_callsign.heading": "Pictures were received, but not the callsign {station}",
      "fail.no_callsign.body": "{pictures} pictures were received, but our callsign could not be read from any of them. The callsign is sent at the start of every frame and is lost when the signal is too weak or fading is deep.",
      "fail.no_callsign.other": "{pictures} pictures were received, but the callsign read from them is {others}, so this is not a transmission from {station}.",
      "fail.too_few.heading": "Not enough frames yet",
      "fail.too_few.body": "{frames} pictures were received; a card needs more than {min}. Record the whole transmission and try again.",
      "fail.model_mismatch.heading": "The signal was received, but the pictures could not be decoded",
      "fail.model_mismatch.body": "The decoder model on this server differs from the transmitter's. That is a problem on our side — please tell the station.",

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
      "offline.c2pa": "The original file also carries C2PA (Content Credentials) data that C2PA-aware tools can read. The station's root certificate is not on the C2PA trust list, so those tools can confirm the file is unaltered but will show the signer as unrecognised; it is shown as trusted only after the root certificate above is added to the tool's trust anchors.",

      "footer.signed": "Cards are digitally signed with the station's own self-issued certificate.",
      "footer.how": "How to verify",
    },
  };

  let lang = "zh-Hant";
  try {
    const saved = localStorage.getItem("nbtv-lang");
    if (saved && STRINGS[saved]) lang = saved;
    else if (!(navigator.language || "").toLowerCase().startsWith("zh")) lang = "en";
  } catch (_) { /* 不能用 localStorage 就照瀏覽器的語言 */ }

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
    const button = document.getElementById("lang");
    if (button) {
      button.textContent = t("lang.switch");
      button.setAttribute("aria-label", t("lang.label"));
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
    const button = document.getElementById("lang");
    if (button) button.addEventListener("click", () => setLanguage(lang === "en" ? "zh-Hant" : "en"));
    apply();
  });

  window.NBTV = {
    t, apply, duration, vars,
    get lang() { return lang; },
    onLanguage(fn) { listeners.push(fn); },
  };
})();
