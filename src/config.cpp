#include "config.h"
#include <Preferences.h>

const KeyInfo KEYS[NKEYS] = {
  {"ssid1", false, WIFI_SSID},  {"pass1", true, WIFI_PASS},
  {"ssid2", false, WIFI_SSID2}, {"pass2", true, WIFI_PASS2},
  {"ssid3", false, WIFI_SSID3}, {"pass3", true, WIFI_PASS3},
  {"host", false, NTFY_HOST},   {"topic", false, NTFY_TOPIC},
  {"auth", true, NTFY_AUTH},    {"name", false, DEVICE_NAME},
};

static const char *NS = "wake";

void configLoad(Config &c) {
  Preferences p;
  p.begin(NS, true);
  for (int i = 0; i < NKEYS; i++)
    c.v[i] = p.isKey(KEYS[i].name) ? p.getString(KEYS[i].name, "") : String(KEYS[i].def);
  p.end();
}

bool configStored(Key k) {
  Preferences p;
  p.begin(NS, true);
  bool r = p.isKey(KEYS[k].name);
  p.end();
  return r;
}

static bool hasControl(const String &s) {
  for (size_t i = 0; i < s.length(); i++)
    if ((uint8_t)s[i] < 0x20 || s[i] == 0x7f) return true;
  return false;
}

String configValidate(Key k, const String &s) {
  if (hasControl(s)) return "control characters are not allowed";
  switch (k) {
    case SSID1: case SSID2: case SSID3:
      if (s.length() > 32) return "SSID is longer than 32 bytes";
      if (k == SSID1 && !s.length()) return "primary SSID is required";
      break;
    case PASS1: case PASS2: case PASS3:
      if (s.length() && (s.length() < 8 || s.length() > 63)) return "WiFi password must be 8-63 characters (or empty for an open network)";
      break;
    case HOST:
      if (!s.length()) return "host is required";
      for (size_t i = 0; i < s.length(); i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '-' || s[i] == ':')) return "host may only contain letters, digits, '.', '-' and ':'";
      break;
    case TOPIC:
      if (!s.length()) return "topic is required";
      if (s.length() > 64) return "topic is longer than 64 characters";
      for (size_t i = 0; i < s.length(); i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '-')) return "topic may only contain letters, digits, '_' and '-'";
      break;
    case AUTH:
      if (s.length() > 300) return "auth header is too long";
      break;
    case NAME:
      if (!s.length()) return "device name is required";
      if (s.length() > 40) return "device name is longer than 40 characters";
      break;
    default: break;
  }
  return "";
}

void configSave(Key k, const String &val) {
  Preferences p;
  p.begin(NS, false);
  p.putString(KEYS[k].name, val);
  p.end();
}

void configUnset(Key k) {
  Preferences p;
  p.begin(NS, false);
  p.remove(KEYS[k].name);
  p.end();
}

void configClear() {
  Preferences p;
  p.begin(NS, false);
  p.clear();
  p.end();
}
