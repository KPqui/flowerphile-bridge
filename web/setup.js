// USB setup for the Flowerphile bridge (Web Serial). Talks only to the serial port; no fetch/XHR here,
// and the page's Content-Security-Policy blocks requests to any other site anyway.
const $ = (id) => document.getElementById(id);
const enc = new TextEncoder();
let port = null, reader = null, readClosed = null, rxBuf = "", autoTimer = null, prefilled = false;

function log(text, cls) {
  const pre = $("log");
  const span = document.createElement("span");
  if (cls) span.className = cls;
  span.textContent = text + "\n";
  pre.appendChild(span);
  while (pre.childNodes.length > 400) pre.removeChild(pre.firstChild);
  pre.scrollTop = pre.scrollHeight;
}

function setConnected(on) {
  for (const id of ["sensors", "send", "status", "push", "disconnect"]) $(id).disabled = !on;
  $("connect").disabled = on;
  $("conn").textContent = on ? "connected" : "not connected";
  $("conn").className = "small " + (on ? "ok" : "");
  if (!on && autoTimer) { clearInterval(autoTimer); autoTimer = null; $("auto").checked = false; }
}

async function send(line) {
  if (!port || !port.writable) return;
  const w = port.writable.getWriter();
  try {
    const bytes = enc.encode(line + "\n");
    for (let i = 0; i < bytes.length; i += 64) {  // small chunks; the board reads them as they come
      await w.write(bytes.slice(i, i + 64));
      await new Promise((r) => setTimeout(r, 15));
    }
  } finally {
    w.releaseLock();
  }
}

async function connect() {
  if (!("serial" in navigator)) { log("This browser can't use USB serial. Use Chrome or Edge on a computer.", "bad"); return; }
  try {
    port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });  // Espressif
  } catch { return; }  // picker closed
  try {
    await port.open({ baudRate: 115200 });
  } catch (e) {
    log("Could not open the port (is the install dialog still open?): " + e.message, "bad");
    port = null;
    return;
  }
  try { await port.setSignals({ dataTerminalReady: false, requestToSend: false }); } catch {}
  setConnected(true);
  readLoop();
  await new Promise((r) => setTimeout(r, 400));
  await send("STATUS");
  await send("SENSORS");
}

async function readLoop() {
  const dec = new TextDecoderStream();
  readClosed = port.readable.pipeTo(dec.writable).catch(() => {});
  reader = dec.readable.getReader();
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      rxBuf += value;
      let i;
      while ((i = rxBuf.indexOf("\n")) >= 0) {
        const line = rxBuf.slice(0, i).replace(/\r$/, "");
        rxBuf = rxBuf.slice(i + 1);
        onLine(line);
      }
      if (rxBuf.length > 8000) rxBuf = "";
    }
  } catch (e) {
    log("USB read stopped: " + e.message, "bad");
  } finally {
    reader.releaseLock();
  }
  await disconnect(true);
}

async function disconnect(already) {
  if (!port) return;
  const p = port;
  port = null;
  try { if (!already && reader) await reader.cancel(); } catch {}
  try { await readClosed; } catch {}
  try { await p.close(); } catch {}
  setConnected(false);
}

function onLine(line) {
  if (!line.startsWith("@")) { if (line.trim()) log(line); return; }
  let d;
  try { d = JSON.parse(line.slice(1)); } catch { log(line); return; }
  if (d.type === "status") renderStatus(d);
  else if (d.type === "sensors") renderSensors(d.list || []);
  else if (d.type === "ok") log("✓ " + d.msg, "ok");
  else if (d.type === "push") {
    if (d.ok) log("✓ pushed " + d.lines + " reading(s) → " + d.path, "ok");
    else log("✗ push failed (" + d.code + "): " + (d.msg || ""), "bad");
    send("STATUS");
  }
  else if (d.type === "error") { log("✗ " + d.msg, "bad"); alert(d.msg); }
}

const LABELS = [
  ["fw", "Firmware"], ["dev", "Bridge ID"], ["configured", "Set up"], ["ssid", "Wi-Fi"], ["wifi", "Wi-Fi connected"],
  ["ip", "IP"], ["wifi_rssi", "Wi-Fi signal (dBm)"], ["time", "Time (UTC)"], ["repo", "Repo"], ["token_set", "Key stored"],
  ["ids", "Logged IDs"], ["waiting", "Readings waiting"], ["pushed", "Pushed since boot"], ["last_ok", "Last good push"],
  ["last_code", "Last push result"], ["last_msg", "Last reply"], ["auth_error", "Key/repo problem"], ["dropped", "Dropped (buffer full)"],
  ["uptime", "Uptime (s)"], ["heap", "Free memory"],
];

function renderStatus(d) {
  const dl = $("statusList");
  dl.textContent = "";
  for (const [k, label] of LABELS) {
    if (!(k in d)) continue;
    const dt = document.createElement("dt");
    dt.textContent = label;
    const dd = document.createElement("dd");
    let v = d[k];
    if (typeof v === "boolean") v = v ? "yes" : "no";
    if (k === "last_code") v = v === 201 ? "201 (saved)" : v === 0 ? "none yet" : String(v);
    if (k === "ids" && !v) v = "all H5074 heard";
    dd.textContent = String(v);
    if ((k === "auth_error" && d[k]) || (k === "wifi" && !d[k] && d.configured)) dd.className = "bad";
    dl.append(dt, dd);
  }
  if (!prefilled) {  // fill the form with what the bridge already has (never the password or key)
    prefilled = true;
    if (d.ssid) $("ssid").value = d.ssid;
    if (d.repo) $("repo").value = d.repo;
    if (d.ids) $("ids").value = d.ids;
    $("led").checked = d.led !== false;
  }
}

function ago(s) {
  if (s < 60) return s + " s ago";
  if (s < 3600) return Math.round(s / 60) + " min ago";
  return (s / 3600).toFixed(1) + " h ago";
}

function renderSensors(list) {
  const tb = $("sensorTable").tBodies[0];
  tb.textContent = "";
  if (!list.length) {
    const tr = tb.insertRow();
    const td = tr.insertCell();
    td.colSpan = 7;
    td.className = "small";
    td.textContent = "No H5074 heard yet. Give it a minute; bring one closer if needed.";
    return;
  }
  list.sort((a, b) => b.rssi - a.rssi);
  for (const s of list) {
    const tr = tb.insertRow();
    for (const v of [s.id, s.temp, s.rh, s.bat + " %", s.rssi + " dBm", ago(s.age), s.logged ? "yes" : "no"]) {
      tr.insertCell().textContent = String(v);
    }
    tr.title = "Click to add/remove this ID in the settings list";
    tr.style.cursor = "pointer";
    tr.onclick = () => toggleId(s.id);
  }
}

function toggleId(id) {
  const ids = $("ids").value.toUpperCase().split(/[^0-9A-F]+/).filter((x) => x.length === 4);
  const i = ids.indexOf(id);
  if (i >= 0) ids.splice(i, 1); else ids.push(id);
  $("ids").value = ids.join(", ");
}

$("connect").onclick = connect;
$("disconnect").onclick = () => disconnect(false);
$("sensors").onclick = () => send("SENSORS");
$("status").onclick = () => send("STATUS");
$("push").onclick = async () => {
  await send("PUSH");
  log("pushing… (takes 10–20 s; the result appears here)");
  document.querySelector("details").open = true;
};
$("auto").onchange = (e) => {
  if (autoTimer) { clearInterval(autoTimer); autoTimer = null; }
  if (e.target.checked) autoTimer = setInterval(() => send("SENSORS"), 5000);
};

$("cfg").onsubmit = async (e) => {
  e.preventDefault();
  const c = { repo: $("repo").value.trim(), ids: $("ids").value.trim(), led: $("led").checked };
  const ssid = $("ssid").value, pass = $("pass").value, token = $("token").value.trim();
  if (ssid) c.ssid = ssid;
  if (pass) c.pass = pass;
  if (token) {
    if (!token.startsWith("github_pat_")) { alert("Use a fine-grained key (starts with github_pat_), limited to the sensors repo."); return; }
    c.token = token;
  }
  await send("CFG " + JSON.stringify(c));
  $("pass").value = "";
  $("token").value = "";
  log("settings sent (password/key not shown)");
};

if (!("serial" in navigator)) log("This browser can't use USB serial. Use Chrome or Edge on a computer.", "bad");
