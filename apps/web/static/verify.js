// 查驗頁：用序號查這張卡是不是本站發的，並列出離線查驗要用的根憑證指紋。
(() => {
  "use strict";

  const { t, duration } = window.NBTV;
  const $ = (id) => document.getElementById(id);
  const show = (el, visible) => { el.hidden = !visible; };

  let outcome = null;  // {kind: "checking" | "found" | "badsig" | "missing" | "format" | "network", data}

  function addRow(list, label, value) {
    const dt = document.createElement("dt"), dd = document.createElement("dd");
    dt.textContent = label;
    dd.textContent = value;
    list.append(dt, dd);
  }

  function render() {
    const panel = $("outcome");
    show(panel, Boolean(outcome));
    ["card-link", "record", "self-heading", "self-record"].forEach((id) => show($(id), false));
    panel.classList.remove("good", "bad");
    if (!outcome) return;
    const kind = outcome.kind;
    if (kind === "checking") {
      $("outcome-title").textContent = t("verify.checking");
      $("outcome-body").textContent = "";
      return;
    }
    if (kind === "network") {
      panel.classList.add("bad");
      $("outcome-title").textContent = t("error.server.heading");
      $("outcome-body").textContent = t("error.network");
      return;
    }
    if (kind === "missing" || kind === "format") {
      panel.classList.add("bad");
      $("outcome-title").textContent = t(`verify.${kind}.heading`);
      $("outcome-body").textContent = t(`verify.${kind}.body`);
      return;
    }

    const data = outcome.data, record = data.record;
    const reception = record.reception || {}, listener = record.listener || {};
    panel.classList.add(kind === "found" ? "good" : "bad");
    $("outcome-title").textContent = t(`verify.${kind}.heading`);
    $("outcome-body").textContent = t(`verify.${kind}.body`, { serial: data.serial });

    $("card-image").src = data.png;
    $("card-image").alt = t("verify.card.alt", { serial: data.serial });
    $("card-link").href = data.png;
    show($("card-link"), true);

    const list = $("record");
    list.replaceChildren();
    addRow(list, t("record.serial"), record.serial);
    addRow(list, t("record.station"), record.station);
    addRow(list, t("record.issued"), String(record.issued_at || "").replace("T", " ").replace("Z", ""));
    addRow(list, t("record.frames"), t("record.frames.value", { frames: reception.frames, callsign: reception.frames_with_callsign }));
    const modes = Object.entries(reception.modes || {}).map(([mode, n]) => t("stats.modes.item", { mode, n })).join("、");
    if (modes) addRow(list, t("stats.modes"), modes);
    if (typeof reception.snr_db_median === "number") {
      addRow(list, t("stats.snr"), t("stats.snr.value", {
        median: reception.snr_db_median.toFixed(1), best: Number(reception.snr_db_best).toFixed(1),
      }));
    }
    addRow(list, t("record.audio"), t("record.audio.value", { duration: duration(reception.audio_seconds || 0), hash: reception.audio_sha256 }));
    show(list, true);

    const self = $("self-record"), blank = t("record.blank");
    self.replaceChildren();
    addRow(self, t("record.name"), listener.name || blank);
    addRow(self, t("record.when"), [listener.date, listener.time_utc].filter(Boolean).join(" ") || blank);
    addRow(self, t("record.freq"), typeof listener.frequency_khz === "number" ? listener.frequency_khz + " kHz" : blank);
    addRow(self, t("record.location"), listener.location || blank);
    show($("self-heading"), true);
    show(self, true);
  }

  function setOutcome(next, focus) {
    outcome = next;
    render();
    if (focus) $("outcome-title").focus({ preventScroll: false });
  }

  function tidy(text) {
    const raw = text.toUpperCase().replace(/[\s-]/g, "");
    return /^[0-9A-Z]{12}$/.test(raw) ? `${raw.slice(0, 4)}-${raw.slice(4, 8)}-${raw.slice(8)}` : null;
  }

  async function lookup(text, fromForm) {
    const serial = tidy(text);
    if (!serial) return setOutcome({ kind: "format" }, fromForm);
    $("serial").value = serial;
    if (fromForm) history.replaceState(null, "", "/v/" + serial);
    setOutcome({ kind: "checking" });
    try {
      const response = await fetch("/api/cards/" + encodeURIComponent(serial), { cache: "no-store" });
      if (response.status === 404) return setOutcome({ kind: "missing" }, fromForm);
      if (!response.ok) throw new Error("lookup");
      const data = await response.json();
      setOutcome({ kind: data.signature_valid ? "found" : "badsig", data }, fromForm);
    } catch (_) {
      setOutcome({ kind: "network" }, fromForm);
    }
  }

  async function start() {
    $("lookup").addEventListener("submit", (event) => {
      event.preventDefault();
      lookup($("serial").value, true);
    });
    window.NBTV.onLanguage(render);

    const match = location.pathname.match(/^\/v\/([^/]+)\/?$/);
    if (match) lookup(decodeURIComponent(match[1]), false);

    try {
      const config = await (await fetch("/api/config", { cache: "no-store" })).json();
      window.NBTV.vars.station = config.station;
      $("station").textContent = config.station;
      $("fingerprint").textContent = config.root_sha256.replace(/(.{4})/g, "$1 ").trim();
      $("cert-name").textContent = `${config.station}-nbtv-root.pem`;
      show($("c2pa-note"), Boolean(config.c2pa));
      window.NBTV.apply();
    } catch (_) { /* 指紋顯示不出來，查驗序號的功能不受影響 */ }
  }

  document.addEventListener("DOMContentLoaded", start);
})();
