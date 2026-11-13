#!/bin/sh
# Allow the ESP32-S3 keyboard (Espressif VID 303a) to wake this machine from suspend.
# Installs a udev rule and applies it to the connected device. Run with sudo (make setup).
set -e
echo 'ACTION=="add|change", SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{power/wakeup}="enabled"' \
  > /etc/udev/rules.d/90-esp32-wake.rules
udevadm control --reload
udevadm trigger --subsystem-match=usb --action=change
for d in /sys/bus/usb/devices/*; do
  [ "$(cat "$d/idVendor" 2>/dev/null)" = 303a ] && echo "$d wakeup=$(cat "$d/power/wakeup" 2>&1)"
done
grep -E "XHC" /proc/acpi/wakeup || true
