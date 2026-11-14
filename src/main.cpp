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
static void sendReply(const WakeResult &r, const String &trigger) {
  String title = String(DEVICE_NAME) + (r.keySent ? ": wake sent" : ": wake FAILED") + REPLY_TITLE_SUFFIX;
  String body;
  if (!r.keySent) body = "USB keyboard not ready (cable on native port? host not mounted).";
  else if (!r.wasSuspended) body = "Computer was already awake, key tapped.";
  else if (r.resumed) body = "Resumed from sleep in " + String(r.ms / 1000.0, 1) + "s, key tapped.";
  else body = "Wakeup signal sent, host did not resume (wake not enabled?).";
  if (trigger.length()) body += "\nTrigger: \"" + trigger.substring(0, 40) + "\"";
  body += "\nWiFi " + String(WiFi.RSSI()) + " dBm, up " + String(millis() / 60000) + " min";

  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(c, String("https://") + NTFY_HOST + "/" + NTFY_TOPIC)) return;
  http.addHeader("Title", title);
  if (strlen(NTFY_AUTH)) http.addHeader("Authorization", NTFY_AUTH);
  int code = http.POST(body);
  Serial0.printf("[%lu] reply -> %d\n", millis(), code);
  http.end();
}

struct WifiCred { const char *ssid; const char *pass; };
// Priority order: primary first, then backups. Empty SSID = slot unused.
static const WifiCred WIFI_NETS[] = {
  {WIFI_SSID, WIFI_PASS}, {WIFI_SSID2, WIFI_PASS2}, {WIFI_SSID3, WIFI_PASS3},
};
static constexpr uint32_t WIFI_TRY_MS = 15000;

static void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep delays/loses idle TCP traffic
  WiFi.setAutoReconnect(false);  // we cycle networks ourselves
  for (;;) {
    for (const WifiCred &n : WIFI_NETS) {
      if (!n.ssid[0]) continue;
      Serial0.printf("[%lu] wifi: trying %s\n", millis(), n.ssid);
      WiFi.disconnect();
      WiFi.begin(n.ssid, n.pass);
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TRY_MS) delay(250);
      if (WiFi.status() == WL_CONNECTED) {
        Serial0.printf("wifi ok %s on %s\n", WiFi.localIP().toString().c_str(), n.ssid);
        return;
      }
    }
  }
}

// Opens the ntfy JSON stream. Only messages published after connecting are delivered,
// so stale wake requests are never replayed.
static String lastTime;  // unix time of last ntfy event seen; on reconnect, replay anything missed since

static bool openStream() {
  ensureWifi();
  client.setInsecure();  // encrypted, broker cert not verified; use setCACert() to pin
  if (!client.connect(NTFY_HOST, 443)) { Serial0.printf("[%lu] ntfy connect failed\n", millis()); return false; }
  Serial0.printf("[%lu] ntfy stream open (since=%s)\n", millis(), lastTime.length() ? lastTime.c_str() : "now");
  client.printf("GET /%s/json%s%s HTTP/1.1\r\nHost: %s\r\nAccept: application/x-ndjson\r\n"
                "User-Agent: esp32-wake\r\nConnection: keep-alive\r\n",
                NTFY_TOPIC, lastTime.length() ? "?since=" : "", lastTime.c_str(), NTFY_HOST);
  if (strlen(NTFY_AUTH)) client.printf("Authorization: %s\r\n", NTFY_AUTH);
  client.print("\r\n");
  return true;
}

void setup() {
  Serial0.begin(115200);
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
  if (!client.connected()) {
    Serial0.printf("[%lu] stream down, reconnecting\n", millis());
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
          Serial0.printf("[%lu] wake (id %s)\n", millis(), id.c_str());
          WakeResult r = doWake();
          Serial0.printf("[%lu] wake done: suspended=%d resumed=%d key=%d %lums\n", millis(),
                         r.wasSuspended, r.resumed, r.keySent, (unsigned long)r.ms);
          sendReply(r, jsonField(line, "message"));
        }
      }
      String t = jsonNumber(line, "time");
      if (t.length()) lastTime = t;
    }
  } else if (millis() - lastRx > SILENCE_LIMIT_MS) {
    Serial0.printf("[%lu] no data for %lu ms, dropping stream\n", millis(), SILENCE_LIMIT_MS);
    client.stop();
  } else {
    delay(10);
  }
}
