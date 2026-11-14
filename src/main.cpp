#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include "tusb.h"

// Wake sequence: ask host to resume the bus (remote wakeup), then tap a key.
// Left Shift is harmless if the computer is already awake.
static constexpr uint8_t WAKE_KEY = KEY_LEFT_SHIFT;
// ntfy sends a keepalive every ~45 s; no bytes for this long = half-open connection, reconnect.
static constexpr uint32_t SILENCE_LIMIT_MS = 75000;

USBHIDKeyboard keyboard;
WiFiClientSecure client;

// Debug log goes to both the UART pins and the USB CDC port (/dev/ttyACM*), so no bridge is needed.
USBCDC USBSerial;
struct TeeLog : Print {
  size_t write(uint8_t c) override { Serial0.write(c); USBSerial.write(c); return 1; }
  size_t write(const uint8_t *b, size_t n) override { Serial0.write(b, n); USBSerial.write(b, n); return n; }
} Log;

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

// Short status message back to the same ntfy topic: "<DEVICE_NAME> woken" / "wake failed".
static void sendReply(const WakeResult &r) {
  String title = String(DEVICE_NAME) + (r.keySent ? ": wake sent" : ": wake FAILED") + REPLY_TITLE_SUFFIX;
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
  if (!http.begin(c, String("https://") + NTFY_HOST + "/" + NTFY_TOPIC)) return;
  http.addHeader("Title", title);
  if (strlen(NTFY_AUTH)) http.addHeader("Authorization", NTFY_AUTH);
  int code = http.POST(body);
  Log.printf("[%lu] reply -> %d\n", millis(), code);
  http.end();
}

struct WifiCred { const char *ssid; const char *pass; };
// Priority order: primary first, then backups. Empty SSID = slot unused.
static const WifiCred WIFI_NETS[] = {
  {WIFI_SSID, WIFI_PASS}, {WIFI_SSID2, WIFI_PASS2}, {WIFI_SSID3, WIFI_PASS3},
};
static constexpr uint32_t WIFI_TRY_MS = 15000;
static constexpr int NTFY_FAILS_BEFORE_SWITCH = 2;  // associated but no internet: move to the next network
static constexpr int NETS = sizeof(WIFI_NETS) / sizeof(WIFI_NETS[0]);
static int wifiCur = 0;    // index of the network we are (or were last) on
static int wifiStart = 0;  // where the next ensureWifi() scan begins; 0 = primary first

static void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep delays/loses idle TCP traffic
  WiFi.setAutoReconnect(false);  // we cycle networks ourselves
  for (;;) {
    for (int k = 0; k < NETS; k++) {
      int i = (wifiStart + k) % NETS;
      const WifiCred &n = WIFI_NETS[i];
      if (!n.ssid[0]) continue;
      Log.printf("[%lu] wifi: trying '%s'\n", millis(), n.ssid);
      WiFi.disconnect();
      delay(500);  // let the stack settle; an immediate begin() after a drop can time out silently
      WiFi.begin(n.ssid, n.pass);
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TRY_MS) delay(250);
      if (WiFi.status() == WL_CONNECTED) {
        wifiCur = i;
        wifiStart = 0;  // after a later drop, prefer the primary again
        Log.printf("[%lu] wifi ok %s on '%s' rssi %d\n", millis(), WiFi.localIP().toString().c_str(), n.ssid, WiFi.RSSI());
        return;
      }
      Log.printf("[%lu] wifi: '%s' failed, status=%d (1=no AP, 4=bad password/auth, 6=disconnected)\n", millis(), n.ssid, (int)WiFi.status());
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
  bool dnsOk = WiFi.hostByName(NTFY_HOST, ip) == 1;
  if (!dnsOk || !client.connect(NTFY_HOST, 443)) {  // hostname, not ip: TLS needs SNI
    char err[100] = "";
    client.lastError(err, sizeof err);
    Log.printf("[%lu] ntfy connect failed (%d/%d): ssid='%s' ip=%s gw=%s dns=%s resolve=%s tls_err='%s' heap=%u\n", millis(),
               fails + 1, NTFY_FAILS_BEFORE_SWITCH, WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
               WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(),
               dnsOk ? ip.toString().c_str() : "FAIL", err, (unsigned)ESP.getFreeHeap());
    if (++fails >= NTFY_FAILS_BEFORE_SWITCH) {
      fails = 0;
      wifiStart = (wifiCur + 1) % NETS;
      Log.printf("[%lu] no internet on '%s', switching network\n", millis(), WiFi.SSID().c_str());
      WiFi.disconnect();
    }
    return false;
  }
  fails = 0;
  Log.printf("[%lu] ntfy stream open (since=%s)\n", millis(), lastTime.length() ? lastTime.c_str() : "now");
  client.printf("GET /%s/json%s%s HTTP/1.1\r\nHost: %s\r\nAccept: application/x-ndjson\r\n"
                "User-Agent: esp32-wake\r\nConnection: keep-alive\r\n",
                NTFY_TOPIC, lastTime.length() ? "?since=" : "", lastTime.c_str(), NTFY_HOST);
  if (strlen(NTFY_AUTH)) client.printf("Authorization: %s\r\n", NTFY_AUTH);
  client.print("\r\n");
  return true;
}

void setup() {
  Serial0.begin(115200);
  USBSerial.setTxTimeoutMs(0);  // never block when no monitor is attached
  USBSerial.begin();
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
    if (!openStream()) { delay(3000); return; }
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
    delay(10);
  }
}
