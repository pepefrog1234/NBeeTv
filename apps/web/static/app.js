// 上傳頁：選檔 → 驗證不是機器人 → 上傳 → 看解碼進度 → 顯示收聽卡或說明原因。
// 伺服器回來的字串與使用者填的字一律用 textContent 放進頁面，不組 HTML。
(() => {
  "use strict";

  const { t, duration } = window.NBTV;
  const $ = (id) => document.getElementById(id);
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

  const FILE_ERRORS = new Set(["not_wav", "unsupported", "bad_rate", "empty", "too_long", "too_large"]);
  const FIELD_IDS = { name: "name", date: "date", time: "time", freq_khz: "freq", location: "location" };

  const state = {
    config: null,
    file: null,
    check: null,         // 檔頭看起來怎樣：{kind: "wav", …}、{kind: "unknown"} 或 {kind: "error", code, vars}
    turnstile: "off",    // off、loading、ready、failed
    widget: null,
    token: null,
    phase: "idle",       // idle、starting、uploading、queued、decoding、done
    percent: 0,
    jobId: null,
    status: null,        // 伺服器回報的工作狀態
    formError: null,     // {code, vars}：顯示在按鈕底下
    outcome: null,       // {kind: "result", result} 或 {kind: "error", code, vars}
  };

  // ------------------------------------------------------------ 檔案

  // 只讀檔頭，在上傳之前先擋掉明顯不行的檔案（伺服器還會再檢查一次）
  async function inspect(file) {
    const head = new DataView(await file.slice(0, 262144).arrayBuffer());
    const tag = (at) => String.fromCharCode(head.getUint8(at), head.getUint8(at + 1), head.getUint8(at + 2), head.getUint8(at + 3));
    if (head.byteLength < 12 || tag(0) !== "RIFF" || tag(8) !== "WAVE") return { kind: "error", code: "not_wav" };
    let pos = 12, format = null;
    while (pos + 8 <= head.byteLength) {
      const id = tag(pos), size = head.getUint32(pos + 4, true), body = pos + 8;
      if (id === "fmt " && body + 16 <= head.byteLength) {
        format = {
          channels: head.getUint16(body + 2, true), rate: head.getUint32(body + 4, true),
          align: head.getUint16(body + 12, true), bits: head.getUint16(body + 14, true),
        };
      } else if (id === "data") {
        if (!format || !format.align || !format.rate) return { kind: "unknown" };
        let bytes = file.size - body;
        if (size !== 0 && size !== 0xFFFFFFFF && size < bytes) bytes = size;
        return Object.assign({ kind: "wav", seconds: bytes / format.align / format.rate }, format);
      }
      pos = body + size + (size & 1);
    }
    return { kind: "unknown" };
  }

  async function chooseFile(file) {
    state.file = file || null;
    state.check = null;
    state.formError = null;
    if (file) {
      const limit = state.config ? state.config.max_bytes : Infinity;
      if (file.size > limit) {
        state.check = { kind: "error", code: "too_large", vars: { mb: Math.floor(limit / 1048576) } };
      } else {
        try {
          state.check = await inspect(file);
        } catch (_) {
          state.check = { kind: "unknown" };
        }
        if (state.file !== file) return;  // 讀檔頭的時候又換了檔案
        const c = state.check;
        if (c.kind === "wav" && state.config) {
          if (c.rate < 6000) state.check = { kind: "error", code: "bad_rate", vars: { rate: c.rate } };
          else if (c.seconds > state.config.max_seconds + 1) {
            state.check = { kind: "error", code: "too_long", vars: { seconds: c.seconds, limit: state.config.max_seconds } };
          }
        }
      }
    }
    render();
  }

  function megabytes(bytes) {
    return (bytes / 1048576).toFixed(bytes < 10485760 ? 1 : 0) + " MB";
  }

  function errorText(code, vars) {
    const v = Object.assign({}, vars || {});
    if (code === "too_long") {
      v.got = duration(v.seconds || 0);
      v.max = duration(v.limit || (state.config ? state.config.max_seconds : 300));
    }
    if (code === "too_large") v.mb = v.mb || v.limit_mb || (state.config ? Math.floor(state.config.max_bytes / 1048576) : "");
    if (code === "rate_limited") v.minutes = Math.max(1, Math.ceil((v.retry_after || 60) / 60));
    const key = "error." + code;
    const text = t(key, v);
    return text === key ? t("error.internal") : text;
  }

  // ------------------------------------------------------------ Turnstile

  function loadTurnstile() {
    state.turnstile = "loading";
    window.nbtvTurnstileReady = () => {
      state.turnstile = "ready";
      state.widget = window.turnstile.render("#turnstile", {
        sitekey: state.config.turnstile.site_key,
        action: state.config.turnstile.action,  // 伺服器查證時會核對這個名稱
        theme: "dark",
        // 驗證元件的語言跟著頁面（它用的是地區的寫法）；之後再換頁面的語言時，已經畫出來的元件不會跟著換
        language: { "zh-Hant": "zh-tw", "zh-Hans": "zh-cn", ja: "ja", es: "es", en: "en" }[window.NBTV.lang] || "auto",
        callback: (token) => { state.token = token; state.turnstile = "ready"; render(); },
        "expired-callback": () => { state.token = null; render(); },
        "error-callback": () => { state.token = null; state.turnstile = "error"; render(); return true; },
      });
      render();
    };
    const script = document.createElement("script");
    script.src = "https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit&onload=nbtvTurnstileReady";
    script.async = true;
    script.onerror = () => { state.turnstile = "failed"; render(); };
    document.head.appendChild(script);
  }

  // 權杖只能用一次：送出去之後就請 widget 再給一個新的
  function renewToken() {
    state.token = null;
    if (state.widget !== null && window.turnstile) {
      try { window.turnstile.reset(state.widget); } catch (_) { /* 下次按的時候會看到驗證沒過 */ }
    }
  }

  // ------------------------------------------------------------ 上傳與查進度

  function listenerFields() {
    const out = {};
    const name = $("name").value.trim(), location = $("location").value.trim();
    if (name) out.name = name;
    if ($("date").value) out.date = $("date").value;
    if ($("time").value) out.time = $("time").value.slice(0, 5);
    if ($("freq").value.trim() !== "") out.freq_khz = Number($("freq").value);
    if (location) out.location = location;
    return out;
  }

  async function submit(event) {
    event.preventDefault();
    if ($("submit").disabled) return;
    state.formError = null;
    state.phase = "starting";
    render(true);
    let response;
    try {
      response = await fetch("/api/jobs", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ token: state.token || "", listener: listenerFields() }),
      });
    } catch (_) {
      return backToForm("network");
    }
    renewToken();
    let body = {};
    try { body = await response.json(); } catch (_) { /* 不是 JSON：當成伺服器出錯 */ }
    if (response.status !== 201) return backToForm(body.error || "internal", body);
    state.jobId = body.id;
    upload(body.upload);
  }

  function backToForm(code, vars) {
    state.phase = "idle";
    state.formError = { code, vars: vars || {} };
    render();
    $("form-error").scrollIntoView({ block: "nearest" });
  }

  function upload(url) {
    const request = new XMLHttpRequest();
    request.open("PUT", url);
    request.setRequestHeader("content-type", "audio/wav");
    request.upload.onprogress = (e) => {
      if (e.lengthComputable) { state.percent = Math.round((100 * e.loaded) / e.total); render(); }
    };
    request.onload = () => {
      if (request.status === 202) return poll();
      let body = {};
      try { body = JSON.parse(request.responseText); } catch (_) { /* 沒有內容 */ }
      finish({ kind: "error", code: body.error || "internal", vars: body });
    };
    request.onerror = () => finish({ kind: "error", code: "network" });
    state.phase = "uploading";
    state.percent = 0;
    render();
    request.send(state.file);
  }

  async function poll() {
    let failures = 0;
    for (;;) {
      let status;
      try {
        const response = await fetch("/api/jobs/" + state.jobId, { cache: "no-store" });
        if (response.status === 404) return finish({ kind: "error", code: "expired" });
        status = await response.json();
        failures = 0;
      } catch (_) {
        failures += 1;
        if (failures >= 6) return finish({ kind: "error", code: "network" });
        await sleep(2000);
        continue;
      }
      state.status = status;
      if (status.state === "done") return finish({ kind: "result", result: status.result });
      if (status.state === "failed") return finish({ kind: "error", code: (status.error || {}).code || "internal", vars: status.error });
      state.phase = status.state === "decoding" ? "decoding" : "queued";
      render();
      await sleep(1000);
    }
  }

  function finish(outcome) {
    state.outcome = outcome;
    state.phase = "done";
    render(true);
  }

  function again() {
    state.phase = "idle";
    state.outcome = null;
    state.status = null;
    state.file = null;
    state.check = null;
    state.formError = null;
    $("file").value = "";
    render();
    $("drop").scrollIntoView({ block: "center" });
    $("file").focus();
  }

  // ------------------------------------------------------------ 畫面

  function show(el, visible) { el.hidden = !visible; }

  function setError(el, code, vars) {
    show(el, Boolean(code));
    el.textContent = code ? errorText(code, vars) : "";
  }

  function renderForm() {
    const c = state.check;
    $("drop").classList.toggle("has-file", Boolean(state.file));
    $("drop-text").textContent = state.file ? state.file.name : t("form.file.drop");
    let meta = "";
    if (state.file && c && c.kind === "wav") {
      meta = t("file.meta", {
        duration: duration(c.seconds),
        rate: Math.round(c.rate / 10) / 100 + " kHz",
        bits: c.bits,
        channels: c.channels === 1 ? t("file.mono") : c.channels === 2 ? t("file.stereo") : t("file.channels", { n: c.channels }),
        size: megabytes(state.file.size),
      });
    } else if (state.file) {
      meta = t("file.meta.unknown", { size: megabytes(state.file.size) });
    }
    $("file-meta").textContent = meta;
    setError($("file-error"), c && c.kind === "error" ? c.code : null, c && c.vars);

    const widgetProblem = state.turnstile === "failed" ? "turnstile_load" : state.turnstile === "error" ? "turnstile_widget" : null;
    setError($("turnstile-error"), widgetProblem);
    setError($("form-error"), state.formError && state.formError.code, state.formError && state.formError.vars);
    const invalid = (state.formError && state.formError.code === "bad_request" && state.formError.vars.fields) || [];
    Object.entries(FIELD_IDS).forEach(([field, id]) => {
      if (invalid.includes(field)) $(id).setAttribute("aria-invalid", "true");
      else $(id).removeAttribute("aria-invalid");
    });

    const fileReady = Boolean(state.file) && (!c || c.kind !== "error");
    const humanReady = state.turnstile === "off" || Boolean(state.token);
    $("submit").disabled = !(state.config && fileReady && humanReady && state.phase === "idle");
  }

  function renderProgress() {
    const bar = $("bar");
    let title = t("progress.starting"), detail = "";
    bar.removeAttribute("value");
    if (state.phase === "uploading") {
      title = t("progress.uploading");
      detail = t("progress.uploading.detail", { pct: state.percent });
      bar.value = state.percent;
    } else if (state.phase === "queued") {
      const ahead = Math.max(0, ((state.status && state.status.position) || 1) - 1);
      title = t("progress.queued");
      detail = ahead === 0 ? t("progress.queued.next") : t("progress.queued.detail", { n: ahead });
    } else if (state.phase === "decoding") {
      const p = (state.status && state.status.progress) || {};
      title = t("progress.decoding");
      detail = p.frames ? t("progress.decoding.detail", { frames: p.frames }) : t("progress.decoding.none");
      if (p.seconds > 0) bar.value = Math.min(100, Math.round((100 * p.position) / p.seconds));
    }
    $("progress-title").textContent = title;
    $("progress-detail").textContent = detail;
  }

  function addStat(list, label, value) {
    const dt = document.createElement("dt"), dd = document.createElement("dd");
    dt.textContent = label;
    dd.textContent = value;
    list.append(dt, dd);
  }

  function renderStats(stats) {
    const list = $("stats");
    list.replaceChildren();
    if (stats.callsign) addStat(list, t("stats.station"), stats.callsign);
    addStat(list, t("stats.frames"), String(stats.frames));
    addStat(list, t("stats.callsign"), String(stats.callsign_frames));
    const modes = Object.entries(stats.modes || {}).map(([mode, n]) => t("stats.modes.item", { mode, n })).join("、");
    if (modes) addStat(list, t("stats.modes"), modes);
    if (stats.snr_median !== null && stats.snr_median !== undefined) {
      addStat(list, t("stats.snr"), t("stats.snr.value", { median: stats.snr_median.toFixed(1), best: stats.snr_best.toFixed(1) }));
    }
    if (stats.offset_hz !== null && stats.offset_hz !== undefined) {
      addStat(list, t("stats.offset"), (stats.offset_hz > 0 ? "+" : "") + stats.offset_hz.toFixed(0) + " Hz");
    }
    addStat(list, t("stats.seconds"), duration(stats.seconds));
    show(list, true);
  }

  function renderOutcome() {
    const o = state.outcome;
    const tips = $("result-tips");
    tips.replaceChildren();
    show(tips, false);
    ["card-link", "download", "verify-link", "keep-note", "stats", "sheet-box"].forEach((id) => show($(id), false));
    $("result").classList.remove("good", "bad");
    if (!o) return;

    if (o.kind === "error") {
      $("result").classList.add("bad");
      $("result-title").textContent = t(FILE_ERRORS.has(o.code) ? "error.failed.heading" : "error.server.heading");
      $("result-body").textContent = errorText(o.code, o.vars);
      return;
    }

    const result = o.result, stats = result.stats;
    // station：從這份錄音解出來的呼號（卡片就是發給這一台的），不是本站自己的呼號
    const vars = {
      frames: stats.frames, pictures: stats.pictures, callsign: stats.callsign_frames, station: stats.callsign || "",
    };
    if (result.ok && result.card) {
      $("result").classList.add("good");
      $("result-title").textContent = t("result.ok.heading");
      $("result-body").textContent = t("result.ok.body", vars);
      $("card-image").src = result.card.png;
      $("card-image").alt = t("result.card.alt", { serial: result.card.serial });
      $("card-link").href = result.card.png;
      $("download").href = result.card.png + "?download=1";
      $("verify-link").href = result.card.verify;
      ["card-link", "download", "verify-link", "keep-note"].forEach((id) => show($(id), true));
    } else {
      $("result").classList.add("bad");
      const reason = ["no_signal", "no_callsign", "too_few", "model_mismatch"].includes(result.reason) ? result.reason : "no_signal";
      $("result-title").textContent = t(`fail.${reason}.heading`, vars);
      $("result-body").textContent = t(`fail.${reason}.body`, vars);
      if (reason === "no_signal") {
        ["tip1", "tip2", "tip3"].forEach((name) => {
          const li = document.createElement("li");
          li.textContent = t("fail.no_signal." + name);
          tips.append(li);
        });
        show(tips, true);
      }
    }
    if (stats.reported > 0) renderStats(stats);
    if (result.sheet) {
      $("sheet").src = result.sheet;
      $("sheet").alt = t("sheet.alt", { n: stats.pictures });
      show($("sheet-box"), true);
    }
  }

  function render(moveFocus) {
    const working = ["starting", "uploading", "queued", "decoding"].includes(state.phase);
    show($("form"), state.phase === "idle");
    show($("progress"), working);
    show($("result"), state.phase === "done");
    renderForm();
    if (working) renderProgress();
    if (state.phase === "done") renderOutcome();
    if (moveFocus) {
      const heading = working ? $("progress-title") : $("result-title");
      heading.focus({ preventScroll: true });
      heading.closest("section").scrollIntoView({ block: "start" });
    }
  }

  // ------------------------------------------------------------ 開始

  async function start() {
    $("file").addEventListener("change", () => chooseFile($("file").files[0]));
    const drop = $("drop");
    ["dragenter", "dragover"].forEach((name) => drop.addEventListener(name, (e) => { e.preventDefault(); drop.classList.add("over"); }));
    ["dragleave", "drop"].forEach((name) => drop.addEventListener(name, (e) => { e.preventDefault(); drop.classList.remove("over"); }));
    drop.addEventListener("drop", (e) => {
      const file = e.dataTransfer && e.dataTransfer.files[0];
      if (file) chooseFile(file);
    });
    $("form").addEventListener("submit", submit);
    $("again").addEventListener("click", again);
    window.NBTV.onLanguage(() => render());

    try {
      const response = await fetch("/api/config", { cache: "no-store" });
      if (!response.ok) throw new Error("config");
      state.config = await response.json();
    } catch (_) {
      state.formError = { code: "network", vars: {} };
      render();
      return;
    }
    const config = state.config;
    Object.assign(window.NBTV.vars, {
      station: config.station,
      min: config.min_frames,
      minutes: Math.round(config.max_seconds / 60),
      mb: Math.floor(config.max_bytes / 1048576),
    });
    $("station").textContent = config.station;
    if (config.notice) {
      $("notice").textContent = config.notice;
      show($("notice"), true);
    }
    window.NBTV.apply();
    if (config.turnstile.enabled) loadTurnstile();
    render();
  }

  document.addEventListener("DOMContentLoaded", start);
})();
