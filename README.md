# wake-on-esp

Wake a sleeping computer from anywhere with an ESP32-S3 and [ntfy](https://ntfy.sh).

The ESP32-S3 plugs into the computer's USB port and appears as a USB keyboard. It joins your WiFi and listens to an ntfy topic. When a message arrives it signals USB remote wakeup and taps Left Shift, which wakes the computer from suspend. No port forwarding, no VPN, no server of your own: send the message from the ntfy phone app, `curl`, or any script.

After each wake the board posts a short status message to the same topic, for example:

```
Konz: wake sent (esp32)
Resumed from sleep in 1.2s, key tapped.
Trigger: "wake"
WiFi -58 dBm, up 42 min
```

This is not Wake-on-LAN. It does not send magic packets. It simulates a keypress, so it works on computers whose Ethernet/WiFi cannot wake the machine.

## Requirements
- ESP32-S3 board (for example ESP32-S3-DevKitC-1). Use the **native `USB` port**: it carries the keyboard, the debug log and flashing. The `UART`/`COM` port is optional.
- Computer that keeps USB powered during sleep and allows USB wake (Linux, Windows).
- 2.4 GHz WiFi with internet access.
- [PlatformIO](https://platformio.org/install/cli) (`pio` or `platformio`; the Makefile finds it on PATH, next to conda, or in `~/.platformio`). Needs PlatformIO Core 6.2.0 or newer with the `espressif32` platform; if `pio` reports `IncompatiblePlatform`, use another PlatformIO install via `make PIO=/path/to/platformio`.

## Setup
1. Configure:
   ```sh
   cp .env.example .env
   ```
   Edit `.env`: `WIFI_SSID`, `WIFI_PASS`, `DEVICE_NAME`, and a long random `NTFY_TOPIC`. Optional backup networks: `WIFI_SSID2`/`WIFI_PASS2` and `WIFI_SSID3`/`WIFI_PASS3`, used in order if the primary is out of range or has no internet. SSIDs may contain spaces; write them without quotes. On public ntfy.sh the topic name is the only secret: anyone who knows it can wake your computer. Generate one with `echo wake-$(head -c 24 /dev/urandom | base32 | tr -d '=' | tr A-Z a-z)`. For a protected topic or self-hosted server set `NTFY_HOST` and `NTFY_AUTH` (full header value, for example `Bearer tk_xxx`).
2. Linux only, once: `make setup` (needs sudo). It adds you to `dialout` and installs the USB wake rule. **Log out and back in afterwards**, otherwise flashing fails with `Permission denied: /dev/ttyACM0`.
3. Plug the board in through the native `USB` port and flash:
   ```sh
   make flash        # or: pio run -t upload
   ```
   If the board already runs this firmware, `make flash` reboots it into the bootloader by itself. If the upload cannot connect (blank/crashed board), hold **BOOT**, tap **RESET**, release **BOOT**, then run `make flash` again. After flashing, unplug and replug the cable if the board stays in bootloader mode (`lsusb` shows "USB JTAG/serial debug unit").
4. Watch the log: `make monitor` (WiFi attempts, IP, ntfy stream, wakes).
5. Suspend the computer, wait about 10 seconds, then send a message: `make test`, the ntfy app, or `curl -d wake https://ntfy.sh/<topic>`.

Subscribe to the same topic in the ntfy app to see the status messages.

## Computer configuration
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
| `WIFI_SSID`, `WIFI_PASS` | Primary WiFi credentials (2.4 GHz) |
| `WIFI_SSID2`, `WIFI_PASS2`, `WIFI_SSID3`, `WIFI_PASS3` | Optional backup networks, tried in order |
| `NTFY_HOST` | ntfy server, default `ntfy.sh` |
| `NTFY_TOPIC` | topic to listen to (keep it secret on public servers) |
| `NTFY_AUTH` | optional `Authorization` header value |
| `DEVICE_NAME` | name shown in status messages |

The wake key is `WAKE_KEY` in `src/main.cpp` (default Left Shift).

## How it works
- Holds an HTTPS stream to `https://<host>/<topic>/json`. ntfy sends a keepalive every 45 s; 75 s of silence forces a reconnect.
- If the connected WiFi has no internet (3 consecutive connect failures) it switches to the next configured network; after a WiFi drop it tries the primary first again.
- After a reconnect it asks ntfy for events since the last one it saw and de-duplicates by message id, so messages sent during a short drop are not lost.
- On a message: if the USB bus is suspended it calls remote wakeup, waits for resume, then taps the key.
- Messages ending in `(esp32)` in the title are the board's own status messages and are ignored.

## Troubleshooting
| Symptom | Check |
|---|---|
| Status says `wake FAILED`, keyboard not ready | Host has not mounted the USB keyboard (wrong port or cable) |
| Computer does not wake, no `power/wakeup` file | Reflash; descriptor must advertise remote wakeup |
| `power/wakeup` is `disabled` | Run `scripts/enable-wake-linux.sh`; check BIOS USB wake |
| No status messages | `make monitor`: which network connected, `ntfy connect failed` lines show DNS/TLS errors |
| `make flash`: `Permission denied: /dev/ttyACM0` | Not in `dialout` yet: `make setup`, then log out and in |
| `make flash`: `Failed to connect` | Hold BOOT, tap RESET, release BOOT, retry |
| `make: PlatformIO not found` | Install PlatformIO or `make PIO=/path/to/platformio flash` |
| Slow first connect after power-up | DNS/TLS retry loop; wait 30 s |

## Limits
- Does not wake from hibernate or shutdown on most computers (USB unpowered).
- The USB port must stay powered in sleep, and the ESP32 needs WiFi the whole time.
- TLS is encrypted but the server certificate is not verified (`client.setInsecure()`); pin a CA with `setCACert()` if that matters to you.
- Anyone who can publish to the topic can wake the computer. Use a long random topic or an authenticated server.

## License
MIT
