#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include "tusb.h"
#include "common.h"
#include "config.h"
#include "setup_ui.h"

// Wake sequence: ask host to resume the bus (remote wakeup), then tap a key.
// Left Shift is harmless if the computer is already awake.
static constexpr uint8_t WAKE_KEY = KEY_LEFT_SHIFT;
// ntfy sends a keepalive every ~45 s; no bytes for this long = half-open connection, reconnect.
static constexpr uint32_t SILENCE_LIMIT_MS = 75000;

USBHIDKeyboard keyboard;
WiFiClientSecure client;
USBCDC USBSerial;
TeeLog Log;
static Config cfg;  // saved settings (NVS), falling back to the .env defaults

// Marks our own status messages so the subscriber loop ignores them (same topic, no echo loop).
static const char *REPLY_TITLE_SUFFIX = " (esp32)";

struct WakeResult {
  bool wasSuspended;
  bool resumed;
  bool keySent;
  uint32_t ms;
};

static WakeResult doWake() {
  WakeResult r{tud_suspended(), true, false, 0};
  uint32_t t0 = millis();
  if (r.wasSuspended) {
    tud_remote_wakeup();  // effective only if the host enabled wakeup for this device
    for (int i = 0; i < 50 && tud_suspended(); i++) delay(100);
    r.resumed = !tud_suspended();
    delay(300);
  }
  r.keySent = tud_mounted() && tud_hid_ready() && keyboard.press(WAKE_KEY) > 0;
  delay(50);
  keyboard.releaseAll();
  r.ms = millis() - t0;
  return r;
}

// Short status message back to the same ntfy topic: "<device name> woken" / "wake failed".
static void sendReply(const WakeResult &r) {
  String title = cfg.v[NAME] + (r.keySent ? ": wake sent" : ": wake FAILED") + REPLY_TITLE_SUFFIX;
  String body;
  if (!r.keySent) body = "USB keyboard not ready (cable on native port? host not mounted).";
  else if (!r.wasSuspended) body = "Computer was already awake, key tapped.";
  else if (r.resumed) body = "Resumed from sleep in " + String(r.ms / 1000.0, 1) + "s, key tapped.";
  else body = "Wakeup signal sent, host did not resume (wake not enabled?).";
  body += "\nWiFi " + String(WiFi.RSSI()) + " dBm, up " + String(millis() / 60000) + " min";

  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(c, String("https://") + cfg.v[HOST] + "/" + cfg.v[TOPIC])) return;
  http.addHeader("Title", title);
  if (cfg.v[AUTH].length()) http.addHeader("Authorization", cfg.v[AUTH]);
  int code = http.POST(body);
  Log.printf("[%lu] reply -> %d\n", millis(), code);
  http.end();
}

// Networks are cfg.ssid(0..NETS-1) in priority order: primary first, then backups. Empty SSID = slot unused.
static constexpr uint32_t WIFI_TRY_MS = 15000;
static constexpr uint32_t PORTAL_AFTER_MS = 30000;  // no network connects for this long: open the setup portal
static constexpr uint32_t PORTAL_MS = 300000;       // portal stays open this long (extended while a phone is connected)
static constexpr int NTFY_FAILS_BEFORE_SWITCH = 2;  // associated but no internet: move to the next network
static int wifiCur = 0;    // index of the network we are (or were last) on
static int wifiStart = 0;  // where the next ensureWifi() scan begins; 0 = primary first
static uint32_t offlineSince = 0;  // millis() of the first failed ntfy connect in the current outage; 0 = none

static constexpr uint32_t NET_TEST_MS = 5000;  // TCP connect timeout of the post-connect internet test

// Cheap reachability test of the ntfy host: DNS + plain TCP connect to :443 (no TLS handshake, nothing sent).
// Run right after a network is joined, so a captive/dead uplink is detected in seconds, not after ntfy retries.
// DNS gets a second try: the resolver can lag a moment behind the DHCP lease.
static bool hasInternet() {
  IPAddress ip;
  bool dns = false;
  for (int attempt = 0; attempt < 2 && !dns; attempt++) {
    if (attempt) uiDelay(1500);
    dns = WiFi.hostByName(cfg.v[HOST].c_str(), ip) == 1;
  }
  if (!dns) {
    Log.printf("[%lu] net test: cannot resolve '%s'\n", millis(), cfg.v[HOST].c_str());
    return false;
  }
  WiFiClient probe;
  bool ok = probe.connect(ip, 443, NET_TEST_MS);
  probe.stop();
  Log.printf("[%lu] net test: %s:443 %s\n", millis(), ip.toString().c_str(), ok ? "reachable" : "unreachable");
  return ok;
}

static void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep delays/loses idle TCP traffic
  WiFi.setAutoReconnect(false);  // we cycle networks ourselves
  uint32_t since = millis();
  for (;;) {
    if (portalRequested) {
      runPortal(PORTAL_MS, false, nullptr);
      offlineSince = 0;
      since = millis();
      if (WiFi.status() == WL_CONNECTED) return;
    }
    bool any = false, noNet = false;  // noNet: joined at least one network this round, none had internet
    for (int k = 0; k < NETS && !portalRequested; k++) {
      int i = (wifiStart + k) % NETS;
      const String &ssid = cfg.ssid(i);
      if (!ssid.length()) continue;
      any = true;
      Log.printf("[%lu] wifi: trying '%s'\n", millis(), ssid.c_str());
      WiFi.disconnect();
      uiDelay(500);  // let the stack settle; an immediate begin() after a drop can time out silently
      WiFi.begin(ssid.c_str(), cfg.pass(i).c_str());
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TRY_MS && !portalRequested) uiDelay(250);
      if (WiFi.status() == WL_CONNECTED) {
        Log.printf("[%lu] wifi ok %s on '%s' rssi %d\n", millis(), WiFi.localIP().toString().c_str(), ssid.c_str(), WiFi.RSSI());
        if (hasInternet()) {
          wifiCur = i;
          wifiStart = 0;  // after a later drop, prefer the primary again
          return;
        }
        noNet = true;
        Log.printf("[%lu] wifi: '%s' has no internet, trying next\n", millis(), ssid.c_str());
        continue;
      }
      if (!portalRequested)
        Log.printf("[%lu] wifi: '%s' failed, status=%d (1=no AP, 4=bad password/auth, 6=disconnected)\n", millis(), ssid.c_str(), (int)WiFi.status());
    }
    if (portalRequested) continue;
    if (!any || noNet || millis() - since >= PORTAL_AFTER_MS) {
      Log.printf("[%lu] wifi: %s, opening setup portal\n", millis(),
                 !any ? "no network configured" : noNet ? "no network has internet" : "no network reachable");
      int net = 0;
      bool joined = runPortal(PORTAL_MS, any, &net);
      offlineSince = 0;
      since = millis();
      // The portal exits as soon as a retry joins a network; accept it only if it really has internet.
      if (joined && hasInternet()) { wifiCur = net; wifiStart = 0; return; }
    }
  }
}

// Opens the ntfy JSON stream. Only messages published after connecting are delivered,
// so stale wake requests are never replayed.
static String lastTime;  // unix time of last ntfy event seen; on reconnect, replay anything missed since

static bool openStream() {
  static int fails = 0;
  ensureWifi();
  client.setInsecure();  // encrypted, broker cert not verified; use setCACert() to pin
  IPAddress ip;
  // DNS first: a network with a dead uplink fails here in seconds instead of stalling in connect().
  bool dnsOk = WiFi.hostByName(cfg.v[HOST].c_str(), ip) == 1;
  if (!dnsOk || !client.connect(cfg.v[HOST].c_str(), 443)) {  // hostname, not ip: TLS needs SNI
    char err[100] = "";
    client.lastError(err, sizeof err);
    Log.printf("[%lu] ntfy connect failed (%d/%d): ssid='%s' ip=%s gw=%s dns=%s resolve=%s tls_err='%s' heap=%u\n", millis(),
               fails + 1, NTFY_FAILS_BEFORE_SWITCH, WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
               WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(),
               dnsOk ? ip.toString().c_str() : "FAIL", err, (unsigned)ESP.getFreeHeap());
    if (!offlineSince) offlineSince = millis();
    if (millis() - offlineSince >= PORTAL_AFTER_MS) {
      // Associated but the internet stays unreachable (also after cycling all networks): let the user fix it.
      Log.printf("[%lu] no internet for %lu s, opening setup portal\n", millis(), (unsigned long)((millis() - offlineSince) / 1000));
      fails = 0;
      client.stop();
      runPortal(PORTAL_MS, false, nullptr);  // returns on timeout; saving reboots
      offlineSince = 0;
      return false;
    }
    if (++fails >= NTFY_FAILS_BEFORE_SWITCH) {
      fails = 0;
      wifiStart = (wifiCur + 1) % NETS;
      Log.printf("[%lu] no internet on '%s', switching network\n", millis(), WiFi.SSID().c_str());
      WiFi.disconnect();
    }
    return false;
  }
  fails = 0;
  offlineSince = 0;
  Log.printf("[%lu] ntfy stream open (since=%s)\n", millis(), lastTime.length() ? lastTime.c_str() : "now");
  client.printf("GET /%s/json%s%s HTTP/1.1\r\nHost: %s\r\nAccept: application/x-ndjson\r\n"
                "User-Agent: esp32-wake\r\nConnection: keep-alive\r\n",
                cfg.v[TOPIC].c_str(), lastTime.length() ? "?since=" : "", lastTime.c_str(), cfg.v[HOST].c_str());
  if (cfg.v[AUTH].length()) client.printf("Authorization: %s\r\n", cfg.v[AUTH].c_str());
  client.print("\r\n");
  return true;
}

void setup() {
  Serial0.begin(115200);
  USBSerial.setTxTimeoutMs(0);  // never block when no monitor is attached
  USBSerial.begin();
  uiInit();
  configLoad(cfg);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Log.printf("[%lu] wifi disconnected, reason=%d (2=auth expired, 15=4way handshake timeout/bad password, 201=no AP, 202=auth fail, 205=connection fail)\n",
               millis(), info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  keyboard.begin();
  USB.usbAttributes(TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP);  // bus-powered + remote wakeup capable
  USB.begin();
}

static String jsonField(const String &line, const char *key) {
  String k = String("\"") + key + "\":\"";
  int i = line.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  int j = line.indexOf('"', i);
  return j < 0 ? "" : line.substring(i, j);
}

static String jsonNumber(const String &line, const char *key) {
  String k = String("\"") + key + "\":";
  int i = line.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  int j = i;
  while (j < (int)line.length() && isDigit(line[j])) j++;
  return line.substring(i, j);
}

void loop() {
  uiPoll();
  if (portalRequested) {
    client.stop();
    runPortal(PORTAL_MS, false, nullptr);  // returns on timeout; saving reboots
    offlineSince = 0;
  }
  if (WiFi.status() != WL_CONNECTED) client.stop();  // network lost: fall through to reconnect (tries all networks)
  static uint32_t lastRx = millis();
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 15000) {
    lastBeat = millis();
    Log.printf("[%lu] alive: wifi=%d rssi=%d stream=%d\n", millis(), (int)WiFi.status(), WiFi.RSSI(), client.connected());
  }
  if (!client.connected()) {
    Log.printf("[%lu] stream down, reconnecting\n", millis());
    client.stop();
    if (!openStream()) { uiDelay(3000); return; }
    lastRx = millis();
  }
  if (client.available()) {
    lastRx = millis();
    String line = client.readStringUntil('\n');
    String id = jsonField(line, "id");
    String event = jsonField(line, "event");
    if (id.length()) {
      if (event == "message" && line.indexOf(REPLY_TITLE_SUFFIX) < 0) {
        // Dedupe: the same id can be replayed after a reconnect.
        static String lastWakeId;
        if (id != lastWakeId) {
          lastWakeId = id;
          Log.printf("[%lu] wake (id %s)\n", millis(), id.c_str());
          WakeResult r = doWake();
          Log.printf("[%lu] wake done: suspended=%d resumed=%d key=%d %lums\n", millis(),
                         r.wasSuspended, r.resumed, r.keySent, (unsigned long)r.ms);
          sendReply(r);
        }
      }
      String t = jsonNumber(line, "time");
      if (t.length()) lastTime = t;
    }
  } else if (millis() - lastRx > SILENCE_LIMIT_MS) {
    Log.printf("[%lu] no data for %lu ms, dropping stream\n", millis(), SILENCE_LIMIT_MS);
    client.stop();
  } else {
    uiDelay(10);
  }
}
