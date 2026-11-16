// Flowerphile sensor bridge — M5Stack AtomS3 Lite (ESP32-S3).
//
// Listens to Govee H5074 Bluetooth adverts, averages each sensor over 15-minute windows,
// and once an hour writes the readings as a NEW file into a private GitHub repo:
//   readings/YYYY-MM-DD/HHMMSS-<dev>.jsonl   (UTC push time; one JSON object per line)
//   {"t":"2026-11-16T15:15:00Z","id":"A1B2","temp":24.31,"rh":58.20,"bat":87,"n":42}
// Files are only ever created, never rewritten, so a bug or a bad connection can't damage old data.
//
// Safety:
// - Settings (Wi-Fi, GitHub token, repo) arrive over USB only (setup page or any serial
//   terminal). No setup hotspot, no web server, nothing listening on the network.
// - Only fine-grained tokens (github_pat_...) are accepted; the token is never printed back.
// - Commit messages carry a number only ("36 readings").
//
// Robustness: NTP time, ~58 h RAM buffer when offline, Wi-Fi reconnect, BLE scan restart,
// 60 s watchdog, reboot on low memory.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <time.h>
#include "govee.h"
#include "roots.h"

#ifndef FW_VERSION
#define FW_VERSION ""
#endif

// ---------- hardware (AtomS3 Lite) ----------
static const int PIN_LED = 35;  // WS2812 under the button
static const int PIN_BTN = 41;  // front button, active low

// ---------- timing ----------
static const uint32_t WINDOW_S = 15 * 60;       // one averaged reading per sensor per window
static const uint32_t PUSH_EVERY_S = 60 * 60;   // push after the window that ends on the hour
static const uint32_t NO_SENSOR_WARN_S = 30 * 60;
static const uint32_t BLE_RESTART_S = 20 * 60;  // nothing heard for this long: restart the scan
static const uint32_t BLE_REBOOT_S = 2 * 3600;  // still nothing: reboot
static const uint32_t WIFI_KICK_S = 10 * 60;
static const uint32_t WDT_S = 60;

// ---------- sizes ----------
static const int MAX_SENSORS = 12;
static const int MAX_ALLOW = 12;
static const int BUF_CAP = 1400;           // records; 6 sensors x 4/h -> ~58 h
static const int MAX_LINES_PER_FILE = 150;

static const char* fwVersion() { return strlen(FW_VERSION) ? FW_VERSION : "dev"; }
static uint32_t upS() { return (uint32_t)(esp_timer_get_time() / 1000000ULL); }
static bool timeValid() { return time(nullptr) > 1700000000; }

// ---------- settings ----------
struct Config {
  String ssid, pass, token, repo, ids;
  bool led = true;      // short status blip every 10 s
  bool active = true;   // active BLE scan (the H5074 needs it to report reliably)
} cfg;
Preferences prefs;

static void loadCfg() {
  prefs.begin("ffb", true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.token = prefs.getString("token", "");
  cfg.repo = prefs.getString("repo", "");
  cfg.ids = prefs.getString("ids", "");
  cfg.led = prefs.getBool("led", true);
  cfg.active = prefs.getBool("active", true);
  prefs.end();
}

static void saveCfg() {
  prefs.begin("ffb", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("token", cfg.token);
  prefs.putString("repo", cfg.repo);
  prefs.putString("ids", cfg.ids);
  prefs.putBool("led", cfg.led);
  prefs.putBool("active", cfg.active);
  prefs.end();
}

static bool configured() { return cfg.ssid.length() && cfg.token.length() && cfg.repo.length(); }

// ---------- sensors (written by the BLE task, read by loop) ----------
struct Sensor {
  bool used;
  bool allowed;
  char id[5];
  int64_t tSum, hSum;  // hundredths
  uint16_t n;
  uint8_t bat;
  int8_t rssi;
  int16_t lastT;
  uint16_t lastH;
  uint32_t lastUp;
  uint32_t total;
};
static Sensor sensors[MAX_SENSORS];
static char allowIds[MAX_ALLOW][5];
static int allowCount = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t lastAllowedUp = 0;  // last advert from a logged sensor
static volatile bool scanEnded = false;

static bool isAllowedLocked(const char* id) {
  if (allowCount == 0) return true;  // no list yet: log every H5074 heard
  for (int i = 0; i < allowCount; i++)
    if (strcmp(allowIds[i], id) == 0) return true;
  return false;
}

// Parse "A1B2, c3d4 ..." into allowIds and normalise cfg.ids.
static void applyAllowList() {
  char tmp[MAX_ALLOW][5];
  int cnt = 0;
  String norm;
  String s = cfg.ids;
  s.toUpperCase();
  int i = 0;
  while (i < (int)s.length() && cnt < MAX_ALLOW) {
    while (i < (int)s.length() && !isxdigit((unsigned char)s[i])) i++;
    int j = i;
    while (j < (int)s.length() && isxdigit((unsigned char)s[j])) j++;
    if (j - i == 4) {
      memcpy(tmp[cnt], s.c_str() + i, 4);
      tmp[cnt][4] = 0;
      if (norm.length()) norm += ",";
      norm += tmp[cnt];
      cnt++;
    }
    i = j;
  }
  portENTER_CRITICAL(&mux);
  memcpy(allowIds, tmp, sizeof tmp);
  allowCount = cnt;
  for (auto& se : sensors)
    if (se.used) se.allowed = isAllowedLocked(se.id);
  portEXIT_CRITICAL(&mux);
  cfg.ids = norm;
}

static void noteReading(const char* id, const GoveeReading& r, int8_t rssi) {
  uint32_t now = upS();
  portENTER_CRITICAL(&mux);
  int slot = -1, freeSlot = -1, oldest = -1;
  for (int i = 0; i < MAX_SENSORS; i++) {
    if (sensors[i].used) {
      if (strcmp(sensors[i].id, id) == 0) { slot = i; break; }
      // candidate to evict: a sensor we don't log and haven't heard for 10 min
      if (!sensors[i].allowed && now - sensors[i].lastUp > 600 &&
          (oldest < 0 || sensors[i].lastUp < sensors[oldest].lastUp))
        oldest = i;
    } else if (freeSlot < 0) {
      freeSlot = i;
    }
  }
  if (slot < 0) slot = freeSlot >= 0 ? freeSlot : oldest;
  if (slot >= 0) {
    Sensor& s = sensors[slot];
    if (!s.used || strcmp(s.id, id) != 0) {
      memset(&s, 0, sizeof s);
      s.used = true;
      memcpy(s.id, id, 5);
      s.allowed = isAllowedLocked(id);
    }
    s.lastT = r.t100;
    s.lastH = r.h100;
    s.bat = r.bat;
    s.rssi = rssi;
    s.lastUp = now;
    s.total++;
    if (s.allowed) {
      s.tSum += r.t100;
      s.hSum += r.h100;
      if (s.n < 65000) s.n++;
      lastAllowedUp = now;
    }
  }
  portEXIT_CRITICAL(&mux);
}

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    uint8_t cnt = dev->getManufacturerDataCount();
    for (uint8_t i = 0; i < cnt; i++) {
      std::string m = dev->getManufacturerData(i);
      GoveeReading r;
      if (!decodeH5074((const uint8_t*)m.data(), m.size(), r)) continue;
      char id[5];
      if (!idFromMac(dev->getAddress().toString().c_str(), id)) return;
      noteReading(id, r, dev->getRSSI());
      return;
    }
  }
  void onScanEnd(const NimBLEScanResults&, int) override { scanEnded = true; }
};
static ScanCB scanCB;

static void startScan() {
  NimBLEScan* sc = NimBLEDevice::getScan();
  sc->stop();
  sc->setScanCallbacks(&scanCB, true);  // want every advert, not just the first per device
  sc->setActiveScan(cfg.active);
  sc->setInterval(160);  // ms; window < interval leaves radio time for Wi-Fi
  sc->setWindow(80);
  sc->setMaxResults(0);  // don't keep a results list; callbacks only
  sc->start(0, false, true);  // 0 = scan forever
  scanEnded = false;
}

// ---------- readings buffer (loop task only) ----------
struct Rec {
  uint32_t t;     // epoch seconds, or uptime seconds if !epoch
  bool epoch;
  char id[5];
  int16_t t100;
  uint16_t h100;
  uint8_t bat;
  uint16_t n;
};
static Rec buf[BUF_CAP];
static int bufHead = 0, bufCount = 0;
static uint32_t droppedTotal = 0;

static Rec& recAt(int i) { return buf[(bufHead + i) % BUF_CAP]; }

static void recPush(const Rec& r) {
  if (bufCount == BUF_CAP) {  // full: drop the oldest
    bufHead = (bufHead + 1) % BUF_CAP;
    bufCount--;
    droppedTotal++;
  }
  buf[(bufHead + bufCount) % BUF_CAP] = r;
  bufCount++;
}

static void recPopFront(int k) {
  if (k > bufCount) k = bufCount;
  bufHead = (bufHead + k) % BUF_CAP;
  bufCount -= k;
}

// Records made before NTP time arrived carry uptime; convert them once time is known.
static void fixUptimeStamps() {
  if (!timeValid()) return;
  uint32_t nowE = (uint32_t)time(nullptr), nowU = upS();
  for (int i = 0; i < bufCount; i++) {
    Rec& r = recAt(i);
    if (!r.epoch) {
      r.t = nowE - (nowU - r.t);
      r.epoch = true;
    }
  }
}

static int closeWindow(uint32_t stamp, bool epoch) {
  Rec out[MAX_SENSORS];
  int k = 0;
  portENTER_CRITICAL(&mux);
  for (auto& s : sensors) {
    if (!s.used || !s.allowed || s.n == 0) continue;
    Rec& r = out[k++];
    r.t = stamp;
    r.epoch = epoch;
    memcpy(r.id, s.id, 5);
    r.t100 = (int16_t)(s.tSum / s.n);
    r.h100 = (uint16_t)(s.hSum / s.n);
    r.bat = s.bat;
    r.n = s.n;
    s.tSum = s.hSum = 0;
    s.n = 0;
  }
  portEXIT_CRITICAL(&mux);
  for (int i = 0; i < k; i++) recPush(out[i]);
  Serial.printf("window closed: %d reading(s), %d waiting to push\n", k, bufCount);
  return k;
}

// ---------- GitHub push ----------
static char devId[5];
static String batchPath;
static int batchLines = 0;
static uint32_t batchDropSnap = 0;
static bool batchMaybeLanded = false;  // an earlier attempt for this batch timed out / 5xx
static bool pushDue = false;
static uint32_t nextTryUp = 0;
static int failStreak = 0;
static bool authError = false;
static int lastCode = 0;
static String lastMsg;
static time_t lastOkEpoch = 0;
static uint32_t pushedTotal = 0;

static void isoTime(uint32_t e, char* out, size_t cap) {
  time_t tt = e;
  struct tm tmv;
  gmtime_r(&tt, &tmv);
  strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static void newBatch() {
  time_t now = time(nullptr);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  char p[64];
  strftime(p, sizeof p, "readings/%Y-%m-%d/%H%M%S", &tmv);
  batchPath = String(p) + "-" + devId + ".jsonl";
  batchLines = min(bufCount, MAX_LINES_PER_FILE);
  batchDropSnap = droppedTotal;
  batchMaybeLanded = false;
}

static String buildContent() {
  String c;
  c.reserve(batchLines * 84);
  char ts[24], tv[16], hv[16], line[160];
  for (int i = 0; i < batchLines; i++) {
    Rec& r = recAt(i);
    isoTime(r.t, ts, sizeof ts);
    fmtHundredths(r.t100, tv, sizeof tv);
    fmtHundredths(r.h100, hv, sizeof hv);
    snprintf(line, sizeof line, "{\"t\":\"%s\",\"id\":\"%s\",\"temp\":%s,\"rh\":%s,\"bat\":%u,\"n\":%u}\n",
             ts, r.id, tv, hv, (unsigned)r.bat, (unsigned)r.n);
    c += line;
  }
  return c;
}

// Returns true when the batch is safely in the repo.
static bool pushOnce() {
  if (batchLines == 0 || droppedTotal != batchDropSnap) newBatch();
  if (batchLines == 0) return true;

  String content = buildContent();
  size_t need = 4 * ((content.length() + 2) / 3) + 1, olen = 0;
  uint8_t* b64 = (uint8_t*)malloc(need);
  if (!b64) { lastMsg = "out of memory"; return false; }
  mbedtls_base64_encode(b64, need, &olen, (const uint8_t*)content.c_str(), content.length());
  b64[olen] = 0;
  content = String();  // free early

  String body;
  body.reserve(olen + 64);
  body = "{\"message\":\"";
  body += batchLines;
  body += " readings\",\"content\":\"";
  body += (const char*)b64;
  body += "\"}";
  free(b64);

  WiFiClientSecure client;
  client.setCACert(ROOT_CAS);
  client.setTimeout(20);
  HTTPClient http;
  http.setTimeout(20000);
  http.setConnectTimeout(15000);
  String url = "https://api.github.com/repos/" + cfg.repo + "/contents/" + batchPath;
  if (!http.begin(client, url)) { lastMsg = "http begin failed"; return false; }
  http.addHeader("Authorization", "Bearer " + cfg.token);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  http.addHeader("User-Agent", "flowerphile-bridge");
  http.addHeader("Content-Type", "application/json");
  esp_task_wdt_reset();
  int code = http.PUT((uint8_t*)body.c_str(), body.length());
  esp_task_wdt_reset();
  String resp = code > 0 ? http.getString() : http.errorToString(code);
  http.end();
  lastCode = code;
  lastMsg = resp.substring(0, 160);

  bool ok = false;
  if (code == 201 || code == 200) {
    ok = true;
  } else if (code == 422 && batchMaybeLanded) {
    ok = true;  // file exists: our earlier, unanswered attempt went through
  } else if (code == 422) {
    batchPath.replace(".jsonl", "b.jsonl");  // name clash with a different file: rename, retry
  } else if (code < 0 || code >= 500) {
    batchMaybeLanded = true;
  }
  authError = (code == 401 || code == 404 || (code == 403 && resp.indexOf("rate limit") < 0));

  Serial.printf("push %s: %d lines -> %d\n", batchPath.c_str(), batchLines, code);
  {
    JsonDocument d;  // tell the setup page right away, so it doesn't have to guess when to look
    d["type"] = "push";
    d["ok"] = ok;
    d["code"] = code;
    d["lines"] = batchLines;
    d["path"] = batchPath;
    if (!ok) d["msg"] = lastMsg;
    Serial.print('@');
    serializeJson(d, Serial);
    Serial.println();
  }
  if (ok) {
    recPopFront(batchLines);
    pushedTotal += batchLines;
    batchLines = 0;
    lastOkEpoch = time(nullptr);
  }
  return ok;
}

static void maybePush() {
  if (!pushDue || bufCount == 0) { pushDue = false; return; }
  if (!configured() || WiFi.status() != WL_CONNECTED || !timeValid()) return;
  uint32_t now = upS();
  if (now < nextTryUp) return;
  fixUptimeStamps();
  if (pushOnce()) {
    failStreak = 0;
    nextTryUp = bufCount > 0 ? now + 5 : 0;  // backlog: next file soon
    pushDue = bufCount > 0;
  } else {
    failStreak++;
    uint32_t backoff = min<uint32_t>(300u << min(failStreak - 1, 4), 3600u);  // 5,10,20,40,60 min
    nextTryUp = now + backoff;
  }
}

// ---------- Wi-Fi ----------
static uint32_t wifiDownSince = 0, wifiLastKick = 0;

static void wifiStart() {
  WiFi.disconnect(true);
  if (!cfg.ssid.length()) return;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("flowerphile-bridge");
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  wifiLastKick = upS();
}

static void wifiWatch() {
  if (!cfg.ssid.length()) return;
  uint32_t now = upS();
  if (WiFi.status() == WL_CONNECTED) { wifiDownSince = 0; return; }
  if (!wifiDownSince) wifiDownSince = now;
  if (now - wifiDownSince > WIFI_KICK_S && now - wifiLastKick > WIFI_KICK_S) {
    Serial.println("wifi: still down, restarting connection");
    wifiStart();
  }
}

// ---------- LED + button ----------
struct Rgb { uint8_t r, g, b; };
static Rgb statusColor() {
  if (!configured()) return {0, 0, 255};                                   // blue: needs setup
  if (authError) return {255, 0, 0};                                       // red: token/repo problem
  if (WiFi.status() != WL_CONNECTED || !timeValid()) return {255, 140, 0}; // amber: no Wi-Fi/time
  if (lastAllowedUp == 0 || upS() - lastAllowedUp > NO_SENSOR_WARN_S) return {180, 0, 255}; // purple: no sensors
  if (failStreak >= 3) return {255, 0, 0};                                 // red: pushes failing
  return {0, 255, 0};                                                      // green: all good
}
static uint32_t ledLast = 0xFFFFFFFF;
static void led(Rgb c, uint8_t bright) {
  uint8_t r = (uint16_t)c.r * bright / 255, g = (uint16_t)c.g * bright / 255, b = (uint16_t)c.b * bright / 255;
  uint32_t packed = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (packed == ledLast) return;  // only talk to the LED when the colour changes
  ledLast = packed;
  neopixelWrite(PIN_LED, r, g, b);
}

static uint32_t btnDownMs = 0;
static bool btnPushFired = false;
static void ledAndButton() {
  uint32_t ms = millis();
  bool down = digitalRead(PIN_BTN) == LOW;
  if (down) {
    if (!btnDownMs) { btnDownMs = ms; btnPushFired = false; }
    if (!btnPushFired && ms - btnDownMs > 5000) {  // hold 5 s: push now
      btnPushFired = true;
      Serial.println("button: push now");
      closeWindow(timeValid() ? (uint32_t)time(nullptr) : upS(), timeValid());
      pushDue = true;
      nextTryUp = 0;
    }
    if (btnPushFired) led({255, 255, 255}, 40);
    else led(statusColor(), 40);  // held: show status steadily, also with blips off
    return;
  }
  btnDownMs = 0;
  bool blip = cfg.led && (ms % 10000) < 80;
  if (!configured()) blip = (ms % 2000) < 150;  // setup mode: blink every 2 s
  led(blip ? statusColor() : Rgb{0, 0, 0}, 12);
}

// ---------- USB commands ----------
static void reply(JsonDocument& d) {
  Serial.print('@');
  serializeJson(d, Serial);
  Serial.println();
}
static void replyErr(const char* msg) {
  JsonDocument d;
  d["type"] = "error";
  d["msg"] = msg;
  reply(d);
}

static bool validRepo(const String& r) {
  int slash = r.indexOf('/');
  if (slash <= 0 || slash != r.lastIndexOf('/') || slash == (int)r.length() - 1) return false;
  for (char c : r)
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '/')) return false;
  return true;
}

static void cmdStatus() {
  JsonDocument d;
  d["type"] = "status";
  d["fw"] = fwVersion();
  d["dev"] = devId;
  d["uptime"] = upS();
  d["configured"] = configured();
  d["ssid"] = cfg.ssid;
  d["wifi"] = WiFi.status() == WL_CONNECTED;
  if (WiFi.status() == WL_CONNECTED) { d["ip"] = WiFi.localIP().toString(); d["wifi_rssi"] = WiFi.RSSI(); }
  d["time_ok"] = timeValid();
  if (timeValid()) { char ts[24]; isoTime((uint32_t)time(nullptr), ts, sizeof ts); d["time"] = ts; }
  d["repo"] = cfg.repo;
  d["token_set"] = cfg.token.length() > 0;  // the token itself is never printed
  d["ids"] = cfg.ids;
  d["led"] = cfg.led;
  d["active_scan"] = cfg.active;
  d["waiting"] = bufCount;
  d["dropped"] = droppedTotal;
  d["pushed"] = pushedTotal;
  d["last_code"] = lastCode;
  d["last_msg"] = lastMsg;
  if (lastOkEpoch) { char ts[24]; isoTime((uint32_t)lastOkEpoch, ts, sizeof ts); d["last_ok"] = ts; }
  d["auth_error"] = authError;
  d["heap"] = ESP.getFreeHeap();
  reply(d);
}

static void cmdSensors() {
  Sensor snap[MAX_SENSORS];
  portENTER_CRITICAL(&mux);
  memcpy(snap, sensors, sizeof snap);
  portEXIT_CRITICAL(&mux);
  uint32_t now = upS();
  JsonDocument d;
  d["type"] = "sensors";
  JsonArray a = d["list"].to<JsonArray>();
  for (auto& s : snap) {
    if (!s.used) continue;
    JsonObject o = a.add<JsonObject>();
    char tv[16], hv[16];
    fmtHundredths(s.lastT, tv, sizeof tv);
    fmtHundredths(s.lastH, hv, sizeof hv);
    o["id"] = s.id;
    o["temp"] = serialized(String(tv));
    o["rh"] = serialized(String(hv));
    o["bat"] = s.bat;
    o["rssi"] = s.rssi;
    o["age"] = now - s.lastUp;
    o["adverts"] = s.total;
    o["logged"] = s.allowed;
  }
  reply(d);
}

static void cmdCfg(const String& json) {
  JsonDocument d;
  if (deserializeJson(d, json)) { replyErr("settings were not valid JSON"); return; }
  Config next = cfg;
  if (d["ssid"].is<const char*>()) next.ssid = d["ssid"].as<const char*>();
  if (d["pass"].is<const char*>()) next.pass = d["pass"].as<const char*>();
  if (d["token"].is<const char*>() && strlen(d["token"].as<const char*>()) > 0) next.token = d["token"].as<const char*>();
  if (d["repo"].is<const char*>()) next.repo = d["repo"].as<const char*>();
  if (d["ids"].is<const char*>()) next.ids = d["ids"].as<const char*>();
  if (d["led"].is<bool>()) next.led = d["led"].as<bool>();
  if (d["active"].is<bool>()) next.active = d["active"].as<bool>();
  next.repo.trim();
  next.token.trim();
  if (next.ssid.length() > 32 || next.pass.length() > 63) { replyErr("Wi-Fi name or password too long"); return; }
  if (next.token.length() && !next.token.startsWith("github_pat_")) {
    replyErr("only fine-grained tokens (github_pat_...) are accepted, so the key can be limited to one repo");
    return;
  }
  if (next.repo.length() && !validRepo(next.repo)) { replyErr("repo must look like owner/name"); return; }
  bool wifiChanged = next.ssid != cfg.ssid || next.pass != cfg.pass;
  bool scanChanged = next.active != cfg.active;
  if (next.token != cfg.token || next.repo != cfg.repo) { authError = false; failStreak = 0; nextTryUp = 0; }
  cfg = next;
  applyAllowList();
  saveCfg();
  if (wifiChanged) wifiStart();
  if (scanChanged) startScan();
  JsonDocument r;
  r["type"] = "ok";
  r["msg"] = "settings saved";
  reply(r);
  cmdStatus();
}

static void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
  if (line.startsWith("CFG ")) cmdCfg(line.substring(4));
  else if (line == "STATUS") cmdStatus();
  else if (line == "SENSORS") cmdSensors();
  else if (line == "PUSH") {
    // close the current window early so there is something to send, then push
    closeWindow(timeValid() ? (uint32_t)time(nullptr) : upS(), timeValid());
    pushDue = true;
    nextTryUp = 0;
    JsonDocument r; r["type"] = "ok"; r["msg"] = "push queued"; reply(r);
  } else if (line == "FORGET YES") {
    prefs.begin("ffb", false); prefs.clear(); prefs.end();
    JsonDocument r; r["type"] = "ok"; r["msg"] = "settings erased, rebooting"; reply(r);
    Serial.flush(); delay(300); ESP.restart();
  } else if (line == "REBOOT") {
    Serial.flush(); delay(200); ESP.restart();
  } else replyErr("unknown command (CFG, STATUS, SENSORS, PUSH, FORGET YES, REBOOT)");
}

static String rx;
static void serialPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (rx.length()) handleLine(rx); rx = ""; }
    else if (rx.length() < 1500) rx += c;
  }
}

// ---------- main ----------
static uint32_t nextWindowEpoch = 0, windowStartUp = 0;
static uint32_t lastScanKick = 0;

void setup() {
  Serial.setRxBufferSize(2048);
  Serial.setTxTimeoutMs(0);  // never stall when no PC is attached
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);
  led({255, 255, 255}, 20);

  esp_task_wdt_init(WDT_S, true);
  esp_task_wdt_add(NULL);

  uint64_t mac = ESP.getEfuseMac();
  snprintf(devId, sizeof devId, "%02x%02x", (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));

  loadCfg();
  applyAllowList();

  NimBLEDevice::init("");
  startScan();

  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
  wifiStart();

  delay(300);
  Serial.printf("\nFlowerphile bridge %s (dev %s) %s\n", fwVersion(), devId,
                configured() ? "configured" : "waiting for settings over USB");
}

void loop() {
  esp_task_wdt_reset();
  serialPoll();
  wifiWatch();

  uint32_t up = upS();

  // 15-minute windows, aligned to the clock once NTP time is known
  if (timeValid()) {
    uint32_t now = (uint32_t)time(nullptr);
    if (!nextWindowEpoch) nextWindowEpoch = (now / WINDOW_S + 1) * WINDOW_S;
    if (now >= nextWindowEpoch) {
      uint32_t stamp = nextWindowEpoch;
      closeWindow(stamp, true);
      if (stamp % PUSH_EVERY_S == 0) { pushDue = true; nextTryUp = 0; }
      nextWindowEpoch = (now / WINDOW_S + 1) * WINDOW_S;
    }
  } else if (up - windowStartUp >= WINDOW_S) {
    closeWindow(up, false);
    windowStartUp = up;
  }

  maybePush();

  // Bluetooth health
  if (scanEnded && up - lastScanKick > 5) { Serial.println("ble: scan stopped, restarting"); startScan(); lastScanKick = up; }
  uint32_t quiet = up - (lastAllowedUp ? lastAllowedUp : 0);
  if (lastAllowedUp && quiet > BLE_RESTART_S && up - lastScanKick > BLE_RESTART_S) {
    Serial.println("ble: no sensors heard for 20 min, restarting scan");
    startScan();
    lastScanKick = up;
  }
  if (lastAllowedUp && quiet > BLE_REBOOT_S) { Serial.println("ble: silent for 2 h, rebooting"); delay(100); ESP.restart(); }

  if (ESP.getFreeHeap() < 16000) { Serial.println("low memory, rebooting"); delay(100); ESP.restart(); }

  ledAndButton();
  delay(20);
}
