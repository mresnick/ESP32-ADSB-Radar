# ESP32-ADSB-Radar

Polls a local SkyAware/dump1090-fa instance's `aircraft.json` feed and renders
nearby aircraft radar-style (range rings, home marker, altitude-colored plane
icons + callsigns, nearby major airports as small dots) on a 1.28" round
GC9A01 LCD, over WiFi.

Heavily inspired by [MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar),
but rewritten for educational purposes, for compatibility with the larger
ESP32 DevKit V1 (vs. that project's hardware), and to focus on local
SkyAware/dump1090 integration rather than a cloud API.

## Hardware

Generic ESP32-WROOM DevKitC-style board:

| LCD pin | ESP32 GPIO |
|---|---|
| VCC | 3.3V |
| GND | GND |
| DIN (MOSI) | GPIO23 |
| CLK (SCLK) | GPIO18 |
| CS | GPIO5 |
| DC | GPIO27 |
| RST | GPIO26 |
| BL | GPIO25 |

## First-time setup (provisioning)

On first boot (or whenever there's no valid saved config), the device starts
an open WiFi access point named **`ADSBRadar-Setup`** and shows that on the
LCD. Connect a phone/laptop to it:

1. Either wait for the "sign in to network" captive-portal popup, or manually
   browse to `http://192.168.4.1/` if it doesn't appear (best-effort only —
   some OS versions don't reliably auto-detect it).
2. Fill in your home WiFi SSID/password, the SkyAware host/port (e.g.
   `10.0.1.178` / `8080`), your home latitude/longitude, the radar range in
   nautical miles (defaults to 20), the max number of aircraft to display
   at once (1-8, defaults to 8), the refresh interval in seconds (defaults
   to 3), which label to show under each aircraft (callsign, aircraft type,
   or tail number - defaults to callsign), and a username/password for the
   live config page below (defaults to `adsbradar`/`adsbradar` - change
   these if you'd rather not use the default).
3. Submit. The device saves the config to NVS and reboots into normal
   operation, connecting to your WiFi and starting the radar display.

**To force reconfiguration later**: hold the BOOT button (GPIO0) while
powering on/resetting the board — this re-enters provisioning mode even if a
valid config is already saved.

If a saved WiFi connection fails repeatedly on boot, the device automatically
falls back into provisioning mode so you can fix the credentials (this
fallback isn't persisted — a future power-cycle retries the saved WiFi first).

## Adjusting config without reprovisioning

Once the device is running normally (radar mode), the SkyAware host/port,
home latitude/longitude, radar range, max displayed aircraft, refresh
interval, and aircraft label can be changed from a browser on your home
network, without holding BOOT or dropping the radar display:

1. Find the device's current IP address — logged at boot (serial monitor) as
   `live config: http://<ip>/ (user=... pass=...)`, and also shown briefly
   on the LCD right after WiFi connects.
2. Browse to `http://<ip>/`. Your browser will prompt for a username and
   password — enter whatever you set during provisioning (`adsbradar`/
   `adsbradar` if you kept the defaults). Most browsers offer to save
   this, so you only need to do it once.
3. Adjust the form and submit. Changes are saved and take effect within one
   refresh cycle (a few seconds) — no reboot.

The WiFi SSID/password are **not** editable this way — a bad WiFi save
applied over WiFi could strand the device with no way to fix it short of
physical BOOT-button recovery, so WiFi changes still require the
provisioning flow above. The login uses standard HTTP Basic Auth with the
username/password set during provisioning (defaulting to `adsbradar`/
`adsbradar` there); a config saved before these fields existed has them
backfilled with that same default on first boot after an upgrade. There is
no other authentication, on the assumption that your home network itself is
a trusted boundary.

## Building

```sh
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

Requires ESP-IDF v5.x (uses `esp_http_client`, `esp_http_server`, `nvs_flash`).

### Flashing from the browser

No ESP-IDF install needed: [flash from your browser](https://mresnick.github.io/ESP32-ADSB-Radar/)
over USB using [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (Chrome
or Edge on desktop only - Web Serial isn't supported in Firefox/Safari). The
firmware there is built and published automatically by
`.github/workflows/deploy-web-installer.yml` on every push to `master`.

## Project layout

```
main/
  app_main.c                    boot decision tree: provisioning vs. radar mode
  Config/, LCD/, GUI/, Fonts/    vendored display driver stack (see License)
  config_store.c/.h              NVS-backed config (namespace "radarcfg")
  wifi_manager.c/.h               esp_wifi AP/STA lifecycle
  captive_dns.c/.h                minimal DNS responder for captive-portal detection
  provisioning_http_server.c/.h  AP-only config web form + save handler (WiFi setup)
  live_config_http_server.c/.h  always-on, Basic-Auth-gated web form for adjusting SkyAware host/range/etc. without reprovisioning
  http_post_utils.c/.h           shared POST-body-read + form field parsing, used by both HTTP servers above
  skyaware_client.c/.h            streams aircraft.json over HTTP, chunk by chunk (no whole-response buffering)
  aircraft_model.c/.h             incremental hand-written JSON scanner - parses straight out of HTTP chunks, emits one aircraft at a time via callback
  geo_math.c/.h                   haversine distance/bearing + polar-to-screen projection
  radar_view.c/.h                 GUI_Paint-based rendering
  airports.c/.h                   small static table of major-airport reference points, drawn as fixed markers
```

## License

MIT - see [LICENSE](LICENSE).

`main/Fonts/font_sans.c` embeds glyph data from
[dhepper/font8x8](https://github.com/dhepper/font8x8) (Public Domain).
`main/Config/`, `main/LCD/`, `main/GUI/`, and the rest of `main/Fonts/` are
copied from a Waveshare demo project for the display hardware; check with
Waveshare for terms if reusing that portion independently of this project.
