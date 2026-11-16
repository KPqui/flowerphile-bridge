# Flowerphile bridge

Firmware for an **M5Stack AtomS3 Lite** that listens to Govee **H5074** thermo-hygrometers over
Bluetooth and writes their readings into a private GitHub repo once an hour. Part of
[Flowerphile](https://kpqui.github.io/tank-log/).

- Flashing and setup page: **https://kpqui.github.io/flowerphile-bridge/** (Chrome or Edge, USB).
- Built by GitHub Actions on every push to `main`: decoder tests → PlatformIO build → merged image →
  GitHub Pages (with ESP Web Tools 10.5.2 served from the same site).

## What it does

1. Scans for H5074 adverts (manufacturer data `88 EC 00 tt tt hh hh bb ..`, little-endian, 1/100).
2. Averages each sensor over 15-minute windows aligned to the clock (UTC).
3. After the window that ends on the hour, creates a new file in the sensors repo:

```
readings/2026-11-16/150002-ab12.jsonl
{"t":"2026-11-16T15:00:00Z","id":"A1B2","temp":24.31,"rh":58.20,"bat":87,"n":412}
```

`t` = end of the window (UTC), `id` = last 4 hex digits of the sensor's address (= its name
`Govee_H5074_A1B2`), `n` = adverts averaged. The folder date is the push date; always filter by `t`.
Readers should de-duplicate on `(t, id)`: after a lost connection a batch can, rarely, arrive twice.

## Safety

- **No network setup surface.** Settings (Wi-Fi, key, repo, sensor IDs) go in over the USB cable only,
  via the setup page or any serial terminal. No hotspot, no web server on the device.
- **Key scope.** Only fine-grained keys (`github_pat_…`) are accepted. Give it access to the
  sensors repo only, *Contents: read and write*, nothing else. It is stored on the device and never
  printed back. Physical access to the board could still extract it, which is why the scope matters.
- **Append-only.** The device only creates files; it never edits or deletes.
- **No plant data.** Commit messages are `N readings`.
- The setup page has a Content-Security-Policy that only allows its own origin and makes no requests
  with what you type.

## Status light

green ok · amber no Wi-Fi / no time · purple no logged sensor heard for 30 min ·
red key/repo problem or 3 failed pushes · blue needs settings (blinks). Hold the button to see it;
hold 5 s to push now. Blinks can be turned off in settings.

## USB commands (115200 baud, one per line)

`STATUS`, `SENSORS`, `PUSH`, `CFG {"ssid":"…","pass":"…","token":"github_pat_…","repo":"owner/name","ids":"A1B2,C3D4","led":true}`,
`FORGET YES` (erase settings), `REBOOT`. Replies start with `@` followed by JSON.

## Limits

- 2.4 GHz Wi-Fi only.
- RAM buffer ≈ 58 h for six sensors; a power cut loses readings not yet pushed (at most ~1 h normally).
- The H5074 needs active scanning to report reliably (Home Assistant users hit this with passive
  scanning); active is the default.
- For the older ATOM Lite (ESP32-PICO-D4) change the board and pins in `platformio.ini`/`main.cpp`.
