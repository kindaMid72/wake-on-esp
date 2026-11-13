# PlatformIO pre-script: injects .env values as string macros (WIFI_SSID, NTFY_TOPIC, ...).
Import("env")
import os
path = os.path.join(env["PROJECT_DIR"], ".env")
if not os.path.exists(path):
    raise SystemExit("Missing .env: run `cp .env.example .env` and edit it")
vals = {}
for line in open(path):
    line = line.strip()
    if line and not line.startswith("#") and "=" in line:
        k, v = line.split("=", 1)
        vals[k.strip()] = v.strip()
for key in ("WIFI_SSID", "WIFI_PASS", "NTFY_HOST", "NTFY_TOPIC", "NTFY_AUTH", "DEVICE_NAME"):
    if key == "NTFY_AUTH":
        vals.setdefault(key, "")
    elif key == "DEVICE_NAME":
        vals.setdefault(key, "computer")
    elif not vals.get(key):
        raise SystemExit("%s missing in .env" % key)
    env.Append(CPPDEFINES=[(key, env.StringifyMacro(vals[key]))])
