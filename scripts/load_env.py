# PlatformIO pre-script: injects .env values as string macros (WIFI_SSID, NTFY_TOPIC, ...).
# These are only first-boot defaults: values saved on the device (setup portal / serial) override them.
# .env is optional; without it the defaults are empty and the device opens its setup portal.
Import("env")
import os
path = os.path.join(env["PROJECT_DIR"], ".env")
vals = {}
if os.path.exists(path):
    for line in open(path):
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            vals[k.strip()] = v.strip()
defaults = {"NTFY_HOST": "ntfy.sh", "DEVICE_NAME": "computer"}
for key in ("WIFI_SSID", "WIFI_PASS", "WIFI_SSID2", "WIFI_PASS2", "WIFI_SSID3", "WIFI_PASS3",
            "NTFY_HOST", "NTFY_TOPIC", "NTFY_AUTH", "DEVICE_NAME"):
    val = vals.get(key) or defaults.get(key, "")
    env.Append(CPPDEFINES=[(key, env.StringifyMacro(val))])
