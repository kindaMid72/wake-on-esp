#pragma once
#include <Arduino.h>

// Runtime configuration, stored in NVS (flash). Values never set on the device fall back to the
// compile-time defaults taken from .env (see scripts/load_env.py).
enum Key { SSID1, PASS1, SSID2, PASS2, SSID3, PASS3, HOST, TOPIC, AUTH, NAME, NKEYS };

struct KeyInfo {
  const char *name;  // serial command name and NVS key
  bool secret;       // never printed or echoed back
  const char *def;   // compile-time default
};
extern const KeyInfo KEYS[NKEYS];

struct Config {
  String v[NKEYS];
  String &ssid(int i) { return v[SSID1 + 2 * i]; }
  String &pass(int i) { return v[PASS1 + 2 * i]; }
};
static constexpr int NETS = 3;

void configLoad(Config &c);                       // NVS value, else compile-time default
bool configStored(Key k);                         // true if a value is saved on the device
String configValidate(Key k, const String &val);  // "" if valid, else the reason
void configSave(Key k, const String &val);
void configUnset(Key k);                          // back to the compile-time default
void configClear();                               // forget everything saved on the device
