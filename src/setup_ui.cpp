#include "setup_ui.h"
#include "common.h"
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

static constexpr uint8_t BUTTON_PIN = 0;  // BOOT button; a normal input once the firmware runs
static constexpr uint32_t BTN_PORTAL_MS = 3000;
static constexpr uint32_t BTN_RESET_MS = 10000;
static constexpr uint32_t WIFI_TRY_MS = 15000;
static constexpr uint32_t RETRY_EVERY_MS = 30000;
static const char *AP_SSID = "wake-esp-setup";
static const char *AP_PASS = "wake-setup";

volatile bool portalRequested = false;

// ---------------------------------------------------------------- serial commands

static const char *HELP =
  "commands:\n"
  "  show                 saved config (passwords hidden) and WiFi state\n"
  "  set <key> [value]    save a value, no value = empty; applied after reboot\n"
  "  unset <key>          drop the saved value, back to the built-in default\n"
  "  portal               open the setup portal (AP wake-esp-setup)\n"
  "  reset                erase all saved config and reboot\n"
  "  reboot\n"
  "keys: ssid1 pass1 ssid2 pass2 ssid3 pass3 host topic auth name\n";

static int findKey(const String &n) {
  for (int i = 0; i < NKEYS; i++) if (n == KEYS[i].name) return i;
  return -1;
}

static void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? "" : line.substring(sp + 1);
  rest.trim();
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") {
    Log.print(HELP);
  } else if (cmd == "show") {
    Config c;
    configLoad(c);
    for (int i = 0; i < NKEYS; i++) {
      String shown = KEYS[i].secret ? (c.v[i].length() ? "(set)" : "(empty)") : c.v[i];
      Log.printf("  %-6s = %s  [%s]\n", KEYS[i].name, shown.c_str(), configStored((Key)i) ? "saved" : "default");
    }
    if (WiFi.status() == WL_CONNECTED)
      Log.printf("wifi: connected to '%s', ip %s, rssi %d\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else
      Log.printf("wifi: not connected (status %d)\n", (int)WiFi.status());
  } else if (cmd == "set" || cmd == "unset") {
    int s2 = rest.indexOf(' ');
    String kn = s2 < 0 ? rest : rest.substring(0, s2);
    String val = s2 < 0 ? "" : rest.substring(s2 + 1);  // keeps inner spaces; `set ssid1 My Net` works
    kn.toLowerCase();
    int k = findKey(kn);
    if (k < 0) { Log.printf("error: unknown key '%s' (see help)\n", kn.c_str()); return; }
    if (cmd == "unset") {
      configUnset((Key)k);
      Log.printf("ok: %s back to default; reboot to apply\n", KEYS[k].name);
      return;
    }
    if (k >= HOST) val.trim();
    String err = configValidate((Key)k, val);
    if (err.length()) { Log.printf("error: %s\n", err.c_str()); return; }
    configSave((Key)k, val);
    Log.printf("ok: %s saved; reboot to apply\n", KEYS[k].name);
  } else if (cmd == "portal") {
    Log.print("opening setup portal\n");
    portalRequested = true;
  } else if (cmd == "reset") {
    configClear();
    Log.print("saved config erased, rebooting\n");
    delay(300);
    ESP.restart();
  } else if (cmd == "reboot") {
    Log.print("rebooting\n");
    delay(300);
    ESP.restart();
  } else {
    Log.printf("error: unknown command '%s' (try help)\n", cmd.c_str());
  }
}

static void pollSerial() {
  static String bufUsb, bufUart;
  Stream *ports[2] = {&USBSerial, &Serial0};
  String *bufs[2] = {&bufUsb, &bufUart};
  for (int p = 0; p < 2; p++) {
    while (ports[p]->available()) {
      char ch = ports[p]->read();
      if (ch == '\n') {
        String line = *bufs[p];
        bufs[p]->clear();
        handleCommand(line);
      } else if (ch != '\r' && bufs[p]->length() < 400) {
        *bufs[p] += ch;
      }
    }
  }
}

// ---------------------------------------------------------------- BOOT button

static void pollButton() {
  static uint32_t downSince = 0;
  static bool warned = false;
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!downSince) downSince = millis();
    uint32_t held = millis() - downSince;
    if (held >= BTN_RESET_MS) {
      Log.print("BOOT held 10 s: erasing saved config, rebooting\n");
      configClear();
      delay(300);
      ESP.restart();
    } else if (held >= BTN_PORTAL_MS && !warned) {
      warned = true;
      Log.print("BOOT held: release for the setup portal, keep holding 10 s to erase saved config\n");
    }
  } else if (downSince) {
    uint32_t held = millis() - downSince;
    if (held >= BTN_PORTAL_MS) portalRequested = true;
    downSince = 0;
    warned = false;
  }
}

void uiInit() { pinMode(BUTTON_PIN, INPUT_PULLUP); }

void uiPoll() {
  pollButton();
  pollSerial();
}

void uiDelay(uint32_t ms) {
  uint32_t t0 = millis();
  do {
    uiPoll();
    if (ms) delay(ms < 10 ? ms : 10);
  } while (millis() - t0 < ms);
}

// ---------------------------------------------------------------- web portal

static String esc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    switch (s[i]) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += s[i];
    }
  }
  return o;
}

static const char PAGE_HEAD[] =
  "<!doctype html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'><title>wake-on-esp setup</title>"
  "<style>body{font:16px system-ui,sans-serif;max-width:30em;margin:1em auto;padding:0 1em}"
  "label{display:block;margin-top:.8em;font-weight:600}input{width:100%;box-sizing:border-box;padding:.5em;font-size:1em}"
  "input[type=checkbox]{width:auto}fieldset{margin-top:1em}small{color:#555;font-weight:400}"
  "button{margin-top:1.2em;padding:.7em 1.2em;font-size:1em}.err{color:#b00020}</style></head><body>";

static String field(const char *label, const char *name, const String &val, const char *type = "text",
                    const char *ph = "", const char *extra = "") {
  String s = "<label>";
  s += label;
  s += "<input name=" + String(name) + " type=" + type + " value=\"" + esc(val) + "\" placeholder=\"" + esc(ph) + "\" " + extra +
       " autocomplete=off autocapitalize=none spellcheck=false></label>";
  return s;
}

static String nearby;  // <option> list of scanned SSIDs

static void scanNetworks() {
  nearby = "";
  int n = WiFi.scanNetworks();
  String seen = "\n";
  for (int i = 0; i < n && i < 30; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || seen.indexOf("\n" + s + "\n") >= 0) continue;
    seen += s + "\n";
    nearby += "<option value=\"" + esc(s) + "\">";
  }
  WiFi.scanDelete();
}

static WebServer *server;
static uint32_t restartAt = 0;

static String formPage() {
  Config c;
  configLoad(c);
  String p = PAGE_HEAD;
  p += "<h1>wake-on-esp</h1>";
  if (WiFi.status() == WL_CONNECTED)
    p += "<p>Connected to <b>" + esc(WiFi.SSID()) + "</b>, " + WiFi.localIP().toString() + "</p>";
  else
    p += "<p>Not connected to WiFi.</p>";
  p += "<form method=post action=/save><datalist id=nets>" + nearby + "</datalist>";
  const char *titles[NETS] = {"Primary WiFi", "Backup WiFi 2", "Backup WiFi 3"};
  for (int i = 0; i < NETS; i++) {
    p += "<fieldset><legend>" + String(titles[i]) + "</legend>";
    p += field("SSID", KEYS[SSID1 + 2 * i].name, c.ssid(i), "text", i ? "(unused)" : "", "list=nets maxlength=32");
    p += field("Password <small>blank = keep; if you change the SSID, blank = open network</small>", KEYS[PASS1 + 2 * i].name, "",
               "password", c.pass(i).length() ? "(unchanged)" : "(none)", "maxlength=63");
    p += "</fieldset>";
  }
  p += "<fieldset><legend>ntfy</legend>";
  p += field("Server", "host", c.v[HOST], "text", "", "maxlength=80");
  p += field("Topic", "topic", c.v[TOPIC], "text", "", "maxlength=64");
  p += field("Authorization header <small>blank = keep</small>", "auth", "", "password", c.v[AUTH].length() ? "(unchanged)" : "(none)", "maxlength=300");
  p += "<label><input type=checkbox name=clear_auth value=1> remove saved authorization header</label>";
  p += "</fieldset><fieldset><legend>Device</legend>";
  p += field("Name shown in status messages", "name", c.v[NAME], "text", "", "maxlength=40");
  p += "</fieldset><button type=submit>Save and reboot</button></form>";
  p += "<form method=post action=/reset onsubmit=\"return confirm('Erase all saved settings?')\">"
       "<button type=submit>Erase saved settings</button></form></body></html>";
  return p;
}

static String msgPage(const String &title, const String &body, bool err) {
  String p = PAGE_HEAD;
  p += "<h1 class=" + String(err ? "err" : "ok") + ">" + esc(title) + "</h1>" + body;
  if (err) p += "<p><a href=/>Back</a></p>";
  p += "</body></html>";
  return p;
}

static void handleSave() {
  Config cur, nw;
  configLoad(cur);
  nw = cur;
  // SSIDs and passwords are taken verbatim (spaces are legal); other fields are trimmed.
  for (int i = 0; i < NETS; i++) {
    const char *sn = KEYS[SSID1 + 2 * i].name, *pn = KEYS[PASS1 + 2 * i].name;
    if (!server->hasArg(sn)) continue;
    nw.ssid(i) = server->arg(sn);
    String pw = server->arg(pn);
    if (!nw.ssid(i).length()) nw.pass(i) = "";                      // slot cleared
    else if (nw.ssid(i) != cur.ssid(i)) nw.pass(i) = pw;            // new network: blank means open
    else if (pw.length()) nw.pass(i) = pw;                          // same network: blank keeps the old one
  }
  for (int k = HOST; k < NKEYS; k++) {
    if (!server->hasArg(KEYS[k].name)) continue;
    String v = server->arg(KEYS[k].name);
    v.trim();
    if (k == AUTH) {
      if (v.length()) nw.v[k] = v;
      else if (server->hasArg("clear_auth")) nw.v[k] = "";
    } else {
      nw.v[k] = v;
    }
  }
  String errs;
  for (int k = 0; k < NKEYS; k++) {
    String e = configValidate((Key)k, nw.v[k]);
    if (e.length()) errs += "<li>" + esc(String(KEYS[k].name) + ": " + e) + "</li>";
  }
  if (errs.length()) {
    server->send(400, "text/html", msgPage("Not saved", "<ul>" + errs + "</ul>", true));
    return;
  }
  for (int k = 0; k < NKEYS; k++)
    if (nw.v[k] != cur.v[k]) { configSave((Key)k, nw.v[k]); Log.printf("config: %s saved\n", KEYS[k].name); }
  server->send(200, "text/html", msgPage("Saved", "<p>Rebooting and connecting to the new settings. You can close this page.</p>", false));
  restartAt = millis() + 1500;
}

static void handleReset() {
  configClear();
  Log.print("config: all saved values erased\n");
  server->send(200, "text/html", msgPage("Erased", "<p>Rebooting with built-in defaults.</p>", false));
  restartAt = millis() + 1500;
}

static void redirectHome() {
  server->sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  server->send(302, "text/plain", "");
}

bool runPortal(uint32_t ms, bool exitOnWifi, int *connectedNet) {
  Config c;
  configLoad(c);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Log.printf("[%lu] portal: AP '%s' (password '%s') at %s, page also on the station IP\n", millis(), AP_SSID, AP_PASS,
             WiFi.softAPIP().toString().c_str());
  scanNetworks();

  WebServer web(80);
  DNSServer dns;
  server = &web;
  restartAt = 0;
  web.on("/", HTTP_GET, [] { server->send(200, "text/html", formPage()); });
  web.on("/save", HTTP_POST, handleSave);
  web.on("/reset", HTTP_POST, handleReset);
  web.onNotFound(redirectHome);  // captive-portal probes (generate_204, hotspot-detect, ...) land on the form
  web.begin();
  dns.start(53, "*", WiFi.softAPIP());

  bool connected = false;
  uint32_t t0 = millis(), lastTry = millis(), tryStart = 0;
  bool trying = false;
  int tryIdx = 0;
  while (millis() - t0 < ms) {
    dns.processNextRequest();
    web.handleClient();
    uiPoll();
    if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
    bool busy = WiFi.softAPgetStationNum() > 0;
    if (busy) t0 = millis();
    if (exitOnWifi) {
      // Changing STA state can hop the AP channel and drop the phone, so only retry when nobody is on the AP.
      if (trying && (busy || WiFi.status() == WL_CONNECTED || millis() - tryStart > WIFI_TRY_MS)) {
        trying = false;
        lastTry = millis();
        if (WiFi.status() == WL_CONNECTED) { connected = true; if (connectedNet) *connectedNet = tryIdx; break; }
        WiFi.disconnect();
      }
      if (!trying && !busy && millis() - lastTry > RETRY_EVERY_MS) {
        for (int k = 0; k < NETS; k++) {
          int i = (tryIdx + 1 + k) % NETS;
          if (!c.ssid(i).length()) continue;
          tryIdx = i;
          Log.printf("[%lu] portal: retrying wifi '%s'\n", millis(), c.ssid(i).c_str());
          WiFi.begin(c.ssid(i).c_str(), c.pass(i).c_str());
          trying = true;
          tryStart = millis();
          break;
        }
        if (!trying) lastTry = millis();
      }
    }
    delay(2);
  }
  web.stop();
  dns.stop();
  server = nullptr;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalRequested = false;
  Log.printf("[%lu] portal: closed (%s)\n", millis(), connected ? "wifi connected" : "timeout");
  return connected;
}
