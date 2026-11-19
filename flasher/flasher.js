// Dune Weaver web flasher: esptool-js over Web Serial, then table setup over
// the same USB port (config upload, WiFi/MQTT secrets, settings restore).
//
// Secrets typed here go to the board over USB and nowhere else: they are
// never put in a URL, stored, or written to a backup, and the log masks them.

import { ESPLoader, Transport } from "https://unpkg.com/esptool-js@0.4.5/bundle.js";
import { sendFile } from "./xmodem.js";

const $ = (id) => document.getElementById(id);
const SECRET_KEY = /pass|psk|secret|token|key|cred|auth/i;
const MASKED = new Set(["********", "<Authentication required>"]);

let manifest = null;
let busy = false;

// ---------------------------------------------------------------- logging

function log(msg, cls) {
  const el = $("log");
  const line = document.createElement("div");
  if (cls) line.className = cls;
  line.textContent = msg;
  el.appendChild(line);
  el.scrollTop = el.scrollHeight;
}

// `$Sta/Password=hunter2` -> `$Sta/Password=********`
function redact(line) {
  const m = /^(\$[^=\s]+)=(.*)$/.exec(line);
  if (m && SECRET_KEY.test(m[1])) return m[1] + "=********";
  return line;
}

function setStatus(text) { $("status").textContent = text; }

function setProgress(fraction) {
  $("progress").value = Math.max(0, Math.min(1, fraction));
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------------------------------------------------------------- serial link

// A byte queue over a Web Serial port, shared by line commands and XMODEM.
class SerialLink {
  constructor(port) {
    this.port = port;
    this.buf = [];
    this.waiters = [];
    this.reader = null;
    this.loop = null;
    this.closed = true;
  }

  async open() {
    for (let i = 0; ; i++) {
      try {
        await this.port.open({ baudRate: 115200, bufferSize: 8192 });
        break;
      } catch (e) {
        // A native-USB board drops off the bus while it reboots.
        if (i >= 20) throw e;
        await sleep(500);
      }
    }
    this.closed = false;
    this.loop = this.readLoop();
  }

  async readLoop() {
    while (!this.closed && this.port.readable) {
      this.reader = this.port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          for (const b of value) this.buf.push(b);
          this.wake();
        }
      } catch (e) {
        if (!this.closed) log("serial: " + e.message, "warn");
      } finally {
        this.reader.releaseLock();
        this.reader = null;
      }
      if (!this.closed) await sleep(100);
    }
  }

  wake() {
    const w = this.waiters;
    this.waiters = [];
    for (const f of w) f();
  }

  async readByte(timeoutMs) {
    const end = Date.now() + timeoutMs;
    while (this.buf.length === 0) {
      const left = end - Date.now();
      if (left <= 0) return -1;
      await new Promise((resolve) => {
        const t = setTimeout(resolve, left);
        this.waiters.push(() => { clearTimeout(t); resolve(); });
      });
    }
    return this.buf.shift();
  }

  // One line without its terminator, or null on timeout.
  async readLine(timeoutMs) {
    const end = Date.now() + timeoutMs;
    const bytes = [];
    for (;;) {
      const b = await this.readByte(Math.max(1, end - Date.now()));
      if (b < 0) {
        if (bytes.length) this.buf.unshift(...bytes);
        return null;
      }
      if (b === 0x0a) return new TextDecoder().decode(new Uint8Array(bytes)).replace(/\r$/, "");
      bytes.push(b);
    }
  }

  async write(data) {
    if (typeof data === "string") data = new TextEncoder().encode(data);
    const w = this.port.writable.getWriter();
    try { await w.write(data); } finally { w.releaseLock(); }
  }

  flushInput() { this.buf = []; }

  async resetBoard() {
    // EN low via RTS (DTR released so IO0 stays high = normal boot).
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: true });
    await sleep(120);
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
  }

  async close() {
    this.closed = true;
    try { if (this.reader) await this.reader.cancel(); } catch (e) { /* ignore */ }
    try { await this.loop; } catch (e) { /* ignore */ }
    try { await this.port.close(); } catch (e) { /* ignore */ }
  }
}

// ---------------------------------------------------------------- commands

// FluidNC's realtime characters must not appear raw in a value; its parser
// decodes these %XX forms (the same encoding `$S` uses when listing).
function encodeValue(v) {
  v = String(v);
  if (/[\r\n]/.test(v)) throw new Error("values cannot contain line breaks");
  return v.replace(/%/g, "%25").replace(/!/g, "%21").replace(/\?/g, "%3F").replace(/~/g, "%7E");
}

function decodeValue(v) {
  return v.replace(/%(25|21|3F|7E)/gi, (_, h) => String.fromCharCode(parseInt(h, 16)));
}

// Send one line; collect output until ok / error:N.
async function command(link, line, timeoutMs = 8000, quiet = false) {
  if (!quiet) log("> " + redact(line), "tx");
  await link.write(line + "\n");
  const out = [];
  const end = Date.now() + timeoutMs;
  for (;;) {
    const l = await link.readLine(Math.max(1, end - Date.now()));
    if (l === null) throw new Error("no answer to " + redact(line));
    const t = l.trim();
    if (t === "ok") return { ok: true, lines: out };
    if (/^error:/.test(t)) {
      if (!quiet) log("  " + t, "err");
      return { ok: false, error: t, lines: out };
    }
    if (t.startsWith("<")) continue; // a status report answering an earlier '?'
    if (t.startsWith("[MSG:")) { if (!quiet) log("  " + t, "msg"); continue; }
    out.push(l);
  }
}

// Wait until the board answers and is Idle or Alarm (settings are idle-gated;
// a real config homes on boot first).
async function waitReady(link, timeoutMs) {
  const end = Date.now() + timeoutMs;
  let lastState = "";
  while (Date.now() < end) {
    await link.write("?");
    const until = Date.now() + 1000;
    while (Date.now() < until) {
      const l = await link.readLine(Math.max(1, until - Date.now()));
      if (l === null) break;
      const m = /^<([A-Za-z]+)/.exec(l.trim());
      if (m) {
        if (m[1] !== lastState) { lastState = m[1]; setStatus("Board state: " + m[1]); }
        if (m[1] === "Idle" || m[1] === "Alarm") {
          await sleep(300);
          link.flushInput();
          return m[1];
        }
      } else if (l.trim() && !/^ok$/.test(l.trim())) {
        log("  " + l, "msg");
      }
    }
  }
  throw new Error("the board did not become ready (last state: " + (lastState || "none") + ")");
}

async function setSetting(link, key, value, results) {
  const r = await command(link, "$" + key + "=" + encodeValue(value));
  results.push({ key, ok: r.ok, error: r.error });
  return r.ok;
}

// ---------------------------------------------------------------- config

function buildConfig(text, opts) {
  const nl = text.includes("\r\n") ? "\r\n" : "\n";
  const edit = (re, value, what) => {
    if (!re.test(text)) throw new Error("profile config has no " + what + " to set");
    text = text.replace(re, (_, head) => head + value);
  };
  if (opts.name) edit(/^(name: ).*$/m, opts.name, "name:");
  if (opts.hostname) edit(/^(hostname: ).*$/m, opts.hostname, "hostname:");
  if (opts.ySteps) edit(/(\r?\n  y:[ \t]*\r?\n    steps_per_mm: )[0-9.]+/, opts.ySteps, "y steps_per_mm");
  if (opts.leds) edit(/(\r?\n  num_leds: )[0-9]+/, opts.leds, "leds num_leds");
  return text.split(/\r?\n/).join(nl);
}

function readProfileValue(text, re) {
  const m = re.exec(text);
  return m ? m[1].trim() : "";
}

// ---------------------------------------------------------------- backups

// Accepts this page's backup JSON, a /sand_settings JSON object, or `$K=V`
// lines (e.g. pasted `$SC` output). Returns { settings: {key: plainValue},
// config }.
function parseBackup(text) {
  const t = text.trim();
  if (t.startsWith("{")) {
    const j = JSON.parse(t);
    if (j.type === "dune-weaver-backup") {
      return { settings: j.settings || {}, config: j.config_yaml || "" };
    }
    const settings = {};
    for (const [k, v] of Object.entries(j)) settings[k.replace(/^\$/, "")] = String(v);
    return { settings, config: "" };
  }
  const settings = {};
  for (const line of t.split(/\r?\n/)) {
    const m = /^\$([^=\s]+)=(.*)$/.exec(line.trim());
    if (m) settings[m[1]] = decodeValue(m[2]);
  }
  return { settings, config: "" };
}

async function downloadBackup() {
  const port = await navigator.serial.requestPort();
  const link = new SerialLink(port);
  await link.open();
  try {
    setStatus("Waiting for the board…");
    await link.write("\n");
    await waitReady(link, 30000);
    const sc = await command(link, "$SC", 10000, true);
    if (!sc.ok) throw new Error("$SC failed: " + sc.error);
    const settings = {};
    let skipped = 0;
    for (const line of sc.lines) {
      const m = /^\$([^=\s]+)=(.*)$/.exec(line.trim());
      // `$/path=` lines are runtime config, which config.yaml already holds.
      if (!m || m[1].startsWith("/")) continue;
      const v = decodeValue(m[2]);
      if (MASKED.has(v) || SECRET_KEY.test(m[1])) { skipped++; continue; }
      settings[m[1]] = v;
    }
    const cfg = await command(link, "$LocalFS/Show=config.yaml", 15000, true);
    const backup = {
      type: "dune-weaver-backup",
      created: new Date().toISOString(),
      note: "Passwords are never included; re-enter them when restoring.",
      settings,
      config_yaml: cfg.ok ? cfg.lines.join("\n") + "\n" : "",
    };
    const host = settings.Hostname || "table";
    const blob = new Blob([JSON.stringify(backup, null, 2)], { type: "application/json" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "dune-weaver-backup-" + host.replace(/[^\w.-]/g, "_") + ".json";
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    log("Backup saved: " + Object.keys(settings).length + " settings" +
        (cfg.ok ? " + config.yaml" : " (no config.yaml)") +
        (skipped ? ", " + skipped + " password setting(s) left out" : ""), "ok");
    setStatus("Backup downloaded.");
  } finally {
    await link.close();
  }
}

// ---------------------------------------------------------------- flashing

function toBinaryString(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) {
    s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  }
  return s;
}

async function sha256Hex(bytes) {
  const d = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function fetchImage(img) {
  const r = await fetch("fw/" + img.path, { cache: "no-cache" });
  if (!r.ok) throw new Error("download failed: " + img.path + " (" + r.status + ")");
  const bytes = new Uint8Array(await r.arrayBuffer());
  if (bytes.length !== img.size || (await sha256Hex(bytes)) !== img.sha256) {
    throw new Error("checksum mismatch for " + img.path + "; reload the page and try again");
  }
  return bytes;
}

async function flash(port, target, fresh) {
  const names = fresh ? ["bootloader", "partitions", "bootapp", "firmware", "filesystem"]
                      : ["bootapp", "firmware"];
  const files = [];
  for (const n of names) {
    const img = target.images[n];
    setStatus("Downloading " + n + "…");
    files.push({ data: toBinaryString(await fetchImage(img)), address: img.offset, name: n });
  }

  const transport = new Transport(port, true);
  const terminal = {
    clean() {},
    writeLine(s) { log(s, "esp"); },
    write(s) { if (s.trim()) log(s, "esp"); },
  };
  const loader = new ESPLoader({
    transport, baudrate: parseInt($("baud").value, 10), terminal, romBaudrate: 115200,
  });
  try {
    setStatus("Connecting to the chip (hold BOOT if it does not answer)…");
    await loader.main();
    const chip = loader.chip.CHIP_NAME;
    if (chip !== target.chip) {
      throw new Error("this board is an " + chip + ", not the " + target.chip +
                      " you picked; choose the other board and try again");
    }
    const total = files.reduce((a, f) => a + f.data.length, 0);
    let done = 0;
    setStatus(fresh ? "Erasing and writing (about 2 minutes)…" : "Writing firmware…");
    await loader.writeFlash({
      fileArray: files.map(({ data, address }) => ({ data, address })),
      flashSize: "keep", flashMode: "keep", flashFreq: "keep",
      eraseAll: fresh, compress: true,
      reportProgress: (i, written, size) => {
        const before = files.slice(0, i).reduce((a, f) => a + f.data.length, 0);
        done = before + (size ? (written / size) * files[i].data.length : 0);
        setProgress(done / total);
      },
    });
    setProgress(1);
    log("Flash written.", "ok");
  } finally {
    try { await transport.disconnect(); } catch (e) { /* ignore */ }
  }
}

// ---------------------------------------------------------------- setup

function formSettings() {
  const s = [];
  const val = (id) => $(id).value.trim();
  if (val("hostname")) s.push(["Hostname", val("hostname")]);
  if (val("ssid")) {
    s.push(["Sta/SSID", val("ssid")]);
    s.push(["Sta/Password", $("wifipass").value]);
  }
  if ($("mqtt").checked) {
    if (!val("broker")) throw new Error("MQTT needs a broker address");
    s.push(["MQTT/Broker", val("broker")]);
    s.push(["MQTT/Port", val("mqttport") || "1883"]);
    s.push(["MQTT/User", val("mqttuser")]);
    if ($("mqttpass").value) s.push(["MQTT/Password", $("mqttpass").value]);
    s.push(["MQTT/Enable", "On"]);
  }
  return s;
}

async function setupBoard(port, plan) {
  const link = new SerialLink(port);
  await link.open();
  const results = [];
  try {
    if (plan.reset) await link.resetBoard();
    setStatus("Waiting for the board to boot…");
    await sleep(1500);
    await link.write("\n");
    await waitReady(link, 60000);

    if (plan.config) {
      setStatus("Uploading config.yaml…");
      await link.write("$Xmodem/Receive=config.yaml\n");
      await sendFile(link, new TextEncoder().encode(plan.config),
                     (n, total) => setProgress(n / total));
      // The receiver reports the result and answers ok.
      const end = Date.now() + 5000;
      let ok = false;
      while (Date.now() < end && !ok) {
        const l = await link.readLine(Math.max(1, end - Date.now()));
        if (l === null) break;
        if (l.trim() === "ok") ok = true;
        else if (/^error:/.test(l.trim())) throw new Error("config upload failed: " + l.trim());
        else if (l.trim()) log("  " + l.trim(), "msg");
      }
      log("config.yaml uploaded (" + plan.config.length + " bytes).", "ok");
      if (plan.configFilename) await setSetting(link, "Config/Filename", "config.yaml", results);
    }

    setStatus("Saving settings…");
    for (const [k, v] of plan.settings) await setSetting(link, k, v, results);

    const restore = Object.entries(plan.restore || {});
    if (plan.config || restore.length) {
      setStatus("Restarting the board (a table with homing will home now)…");
      log("> $Bye", "tx");
      await link.write("$Bye\n");
      await sleep(3000);
      link.flushInput();
      await link.write("\n");
      await waitReady(link, 180000);
    }

    if (restore.length) {
      setStatus("Restoring " + restore.length + " settings…");
      const formKeys = new Set(plan.settings.map(([k]) => k.toLowerCase()));
      for (const [k, v] of restore) {
        if (MASKED.has(v) || formKeys.has(k.toLowerCase())) continue;
        if (plan.config && /^Config\/Filename$/i.test(k)) continue;
        await setSetting(link, k, v, results);
      }
      // Settings that resize hardware (LED count/order) apply on the next boot.
      log("> $Bye", "tx");
      await link.write("$Bye\n");
    }
  } finally {
    await link.close();
  }
  return results;
}

// ---------------------------------------------------------------- UI

function selectedTarget() { return manifest.targets[$("board").value]; }

function profilesFor(mcu) { return manifest.profiles.filter((p) => p.mcu === mcu); }

function fillProfiles() {
  const sel = $("profile");
  sel.innerHTML = "";
  const none = new Option("No table config (keep current / test drive)", "");
  sel.add(none);
  for (const p of profilesFor($("board").value)) sel.add(new Option(p.label, p.id));
  const fresh = $("install").value === "fresh";
  sel.value = fresh && sel.options.length > 1 ? sel.options[1].value : "";
  onProfile();
}

async function onProfile() {
  const p = manifest.profiles.find((x) => x.id === $("profile").value);
  $("profileFields").hidden = !p;
  if (!p) return;
  const text = await (await fetch("fw/" + p.config, { cache: "no-cache" })).text();
  $("ySteps").value = p.ySteps || readProfileValue(text, /\n  y:[ \t]*\r?\n    steps_per_mm: ([0-9.]+)/);
  $("leds").value = p.leds || readProfileValue(text, /\n  num_leds: ([0-9]+)/);
  if (!$("hostname").value || $("hostname").dataset.auto === "1") {
    $("hostname").value = p.hostname || readProfileValue(text, /^hostname: (.*)$/m);
    $("hostname").dataset.auto = "1";
  }
}

function setBusy(b) {
  busy = b;
  for (const id of ["go", "backup"]) $(id).disabled = b;
}

async function run() {
  if (busy) return;
  const fresh = $("install").value === "fresh";
  const target = selectedTarget();
  let plan;
  try {
    plan = { settings: formSettings(), restore: null, config: "" };
    const p = manifest.profiles.find((x) => x.id === $("profile").value);
    let backup = null;
    const file = $("restore").files[0];
    if (file) {
      backup = parseBackup(await file.text());
      plan.restore = backup.settings;
      log("Restore file: " + Object.keys(backup.settings).length + " settings" +
          (backup.config ? " + config.yaml" : ""), "msg");
    }
    if (backup && backup.config && $("useBackupConfig").checked) {
      plan.config = backup.config;
    } else if (p) {
      const text = await (await fetch("fw/" + p.config, { cache: "no-cache" })).text();
      plan.config = buildConfig(text, {
        name: p.name, hostname: $("hostname").value.trim() || p.hostname,
        ySteps: $("ySteps").value.trim(), leds: $("leds").value.trim(),
      });
    }
    plan.configFilename = !!plan.config;
  } catch (e) {
    log("Error: " + e.message, "err");
    return;
  }

  if (fresh && !confirm("Fresh install erases the whole board: settings, WiFi, " +
                        "playlists stored on the board and its config. Continue?")) return;

  setBusy(true);
  $("log").innerHTML = "";
  setProgress(0);
  let port;
  try {
    port = await navigator.serial.requestPort();
    if (!$("skipFlash").checked) await flash(port, target, fresh);
    const needSetup = plan.config || plan.settings.length || (plan.restore && Object.keys(plan.restore).length);
    if (needSetup) {
      const results = await setupBoard(port, { ...plan, reset: !$("skipFlash").checked });
      const failed = results.filter((r) => !r.ok);
      log("Settings: " + (results.length - failed.length) + " saved" +
          (failed.length ? ", " + failed.length + " failed: " +
           failed.map((r) => r.key + " (" + r.error + ")").join(", ") : ""), failed.length ? "warn" : "ok");
    } else if (!$("skipFlash").checked) {
      const link = new SerialLink(port);
      await link.open();
      await link.resetBoard();
      await link.close();
    }
    setStatus("Done. The table restarts and joins your WiFi in about 30 seconds.");
    log("Done.", "ok");
  } catch (e) {
    if (e && e.name === "NotFoundError") setStatus("No port selected.");
    else { log("Error: " + (e && e.message ? e.message : e), "err"); setStatus("Failed — see the log."); }
  } finally {
    // Clear secrets from the page once they have been used.
    $("wifipass").value = "";
    $("mqttpass").value = "";
    setBusy(false);
  }
}

async function init() {
  if (!("serial" in navigator)) {
    $("unsupported").hidden = false;
    $("app").hidden = true;
    return;
  }
  manifest = await (await fetch("fw/manifest.json", { cache: "no-cache" })).json();
  $("version").textContent = manifest.version + " (" + manifest.commit + ", built " + manifest.built + ")";
  if (/^[\w.-]+\/[\w.-]+$/.test(manifest.repo || "")) {
    $("source").href = "https://github.com/" + manifest.repo;
    $("source").textContent = manifest.repo;
    $("sourceLine").hidden = false;
  }
  const board = $("board");
  for (const [mcu, t] of Object.entries(manifest.targets)) board.add(new Option(t.label, mcu));
  board.onchange = fillProfiles;
  $("install").onchange = () => {
    const fresh = $("install").value === "fresh";
    $("freshNote").hidden = !fresh;
    fillProfiles();
  };
  $("profile").onchange = onProfile;
  $("hostname").oninput = () => { $("hostname").dataset.auto = "0"; };
  $("mqtt").onchange = () => { $("mqttFields").hidden = !$("mqtt").checked; };
  $("restore").onchange = async () => {
    const f = $("restore").files[0];
    $("backupConfigRow").hidden = true;
    if (!f) return;
    try {
      const b = parseBackup(await f.text());
      $("backupConfigRow").hidden = !b.config;
      log("Restore file has " + Object.keys(b.settings).length + " settings" +
          (b.config ? " and a config.yaml" : ""), "msg");
    } catch (e) {
      log("That file is not a backup: " + e.message, "err");
      $("restore").value = "";
    }
  };
  $("go").onclick = run;
  $("backup").onclick = async () => {
    if (busy) return;
    setBusy(true);
    try { await downloadBackup(); } catch (e) {
      if (e && e.name !== "NotFoundError") log("Error: " + e.message, "err");
    } finally { setBusy(false); }
  };
  fillProfiles();
}

init().catch((e) => log("Could not load the firmware list: " + e.message, "err"));
