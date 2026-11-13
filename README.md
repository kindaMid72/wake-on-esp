# wake-on-esp

Wake a sleeping laptop from anywhere with an ESP32-S3 and [ntfy](https://ntfy.sh).

The ESP32-S3 plugs into the laptop's USB port and appears as a USB keyboard. It joins your WiFi and listens to an ntfy topic. When a message arrives it signals USB remote wakeup and taps Left Shift, which wakes the laptop from suspend. No port forwarding, no VPN, no server of your own: send the message from the ntfy phone app, `curl`, or any script.

After each wake the board posts a short status message to the same topic, for example:

```
Konz: wake sent (esp32)
Resumed from sleep in 1.2s, key tapped.
Trigger: "wake"
WiFi -58 dBm, up 42 min
```

This is not Wake-on-LAN. It does not send magic packets. It simulates a keypress, so it works on laptops whose Ethernet/WiFi cannot wake the machine.

## Requirements
- ESP32-S3 board with **two USB-C ports** (for example ESP32-S3-DevKitC-1): `UART`/`COM` (serial bridge) and `USB` (native).
- Laptop that keeps USB powered during sleep and allows USB wake (Linux, Windows).
- 2.4 GHz WiFi with internet access.
- [PlatformIO](https://platformio.org/install/cli).

## Setup
1. Configure:
   ```sh
   cp .env.example .env
   ```
   Edit `.env`: `WIFI_SSID`, `WIFI_PASS`, `DEVICE_NAME`, and a long random `NTFY_TOPIC`. On public ntfy.sh the topic name is the only secret: anyone who knows it can wake your laptop. Generate one with `echo wake-$(head -c 24 /dev/urandom | base32 | tr -d '=' | tr A-Z a-z)`. For a protected topic or self-hosted server set `NTFY_HOST` and `NTFY_AUTH` (full header value, for example `Bearer tk_xxx`).
2. Flash through the **UART/COM** port (auto-reset works there):
   ```sh
   make flash        # or: pio run -t upload
   ```
   Linux needs serial access: `make setup` adds you to `dialout` (log out and in afterwards).
3. Move the cable to the **native USB** port. Keep the UART cable plugged in as well if you want serial logs (`make monitor`).
4. Allow the laptop to be woken by the board (see below).
5. Suspend the laptop, wait about 10 seconds, then send a message: `make test`, the ntfy app, or `curl -d wake https://ntfy.sh/<topic>`.

Subscribe to the same topic in the ntfy app to see the status messages.

## Laptop configuration
The firmware advertises USB remote wakeup. The OS must also allow it.

**Linux**
```sh
sudo scripts/enable-wake-linux.sh      # or: make setup
cat /sys/bus/usb/devices/*/power/wakeup   # the Espressif (303a) device must say "enabled"
```
This installs a udev rule setting `power/wakeup=enabled` for vendor `303a`. `/proc/acpi/wakeup` must list your XHCI controller as `*enabled`. If the `wakeup` file is missing for the device, it does not advertise remote wakeup (wrong firmware or wrong port).

**Windows**: Device Manager, Keyboards, the HID keyboard, Properties, Power Management, tick "Allow this device to wake the computer". Flash with `pio run -t upload`; `make` is not required.

**BIOS/UEFI**: enable USB wake or "USB power in sleep", and disable deep-sleep power saving (ErP) if present.

## Configuration reference (`.env`)
| Key | Meaning |
|---|---|
| `WIFI_SSID`, `WIFI_PASS` | WiFi credentials (2.4 GHz) |
| `NTFY_HOST` | ntfy server, default `ntfy.sh` |
| `NTFY_TOPIC` | topic to listen to (keep it secret on public servers) |
| `NTFY_AUTH` | optional `Authorization` header value |
| `DEVICE_NAME` | name shown in status messages |

The wake key is `WAKE_KEY` in `src/main.cpp` (default Left Shift).

## How it works
- Holds an HTTPS stream to `https://<host>/<topic>/json`. ntfy sends a keepalive every 45 s; 75 s of silence forces a reconnect.
- After a reconnect it asks ntfy for events since the last one it saw and de-duplicates by message id, so messages sent during a short drop are not lost.
- On a message: if the USB bus is suspended it calls remote wakeup, waits for resume, then taps the key.
- Messages ending in `(esp32)` in the title are the board's own status messages and are ignored.

## Troubleshooting
| Symptom | Check |
|---|---|
| Status says `wake FAILED`, keyboard not ready | Cable is on the UART port instead of the native `USB` port |
| Laptop does not wake, no `power/wakeup` file | Reflash; descriptor must advertise remote wakeup |
| `power/wakeup` is `disabled` | Run `scripts/enable-wake-linux.sh`; check BIOS USB wake |
| No status messages | Serial log (`make monitor`): WiFi up? `ntfy stream open`? |
| Slow first connect after power-up | DNS/TLS retry loop; wait 30 s |

## Limits
- Does not wake from hibernate or shutdown on most laptops (USB unpowered).
- The USB port must stay powered in sleep, and the ESP32 needs WiFi the whole time.
- TLS is encrypted but the server certificate is not verified (`client.setInsecure()`); pin a CA with `setCACert()` if that matters to you.
- Anyone who can publish to the topic can wake the laptop. Use a long random topic or an authenticated server.

## License
MIT
