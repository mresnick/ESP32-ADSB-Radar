# ESP32-ADSB-Radar

Polls a local SkyAware/dump1090-fa instance's `aircraft.json` feed and renders
nearby aircraft radar-style (range rings, home marker, altitude-colored plane
icons + callsigns, nearby major airports as small dots) on the 1.28" round
GC9A01 LCD, over WiFi. The display driver stack (`Config/`, `LCD/`, `GUI/`,
`Fonts/`) was copied verbatim from a sibling Waveshare demo project - except
`Fonts/font_sans.c`, which is this project's own (see "Notes / gotchas"
below).

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
   nautical miles (defaults to 20), and a username/password for the live
   config page below (defaults to `adsbradar`/`adsbradar` - change these
   if you'd rather not use the default). Refresh interval (3s) is fixed,
   not user-configurable.
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
home latitude/longitude, and radar range can be changed from a browser on
your home network, without holding BOOT or dropping the radar display:

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

## Project layout

```
main/
  app_main.c                    boot decision tree: provisioning vs. radar mode
  Config/, LCD/, GUI/, Fonts/    display driver stack, copied verbatim from a sibling Waveshare demo
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

## Notes / gotchas

- The SkyAware response is never buffered in full — a base ESP32-WROOM has no
  PSRAM to spare on an unbounded feed, so `aircraft_model.c` parses directly
  out of each HTTP chunk as it arrives (`aircraft_model_scanner_feed`), one
  aircraft object at a time. There is no whole-response size cap; the only
  fixed buffer is `AIRCRAFT_SCAN_BUF_SIZE` (2KB, `aircraft_model.h`), sized to
  hold one in-progress JSON record, not the whole document.
- Every aircraft in the feed is parsed and ranked as it streams in
  (`on_aircraft_parsed` in `app_main.c`), keeping only the top `NEAREST_K`
  (= `MAX_DISPLAYED_AIRCRAFT`, 8) seen so far. Ranking happens during parsing
  rather than after, so the aircraft actually shown are always the nearest
  ones seen regardless of where they happen to fall in the server's JSON
  array order.
- The ranking key is distance plus a mild altitude penalty
  (`ranking_priority` in `app_main.c`), not raw distance alone - a plane low
  and close (e.g. on final approach) outranks a cruise-altitude plane that's
  only marginally closer, which matters most exactly when this is most
  useful: parked near a busy airport. True distance/bearing (never the
  penalized value) is still what's actually drawn and what decides the
  range cutoff. When more in-range aircraft exist than fit, a small "+N"
  indicator is drawn so a busy feed reads as "N more are hidden" rather
  than looking like a complete picture.
- At most 8 in-range, airborne aircraft are drawn at once, in
  altitude-descending (painter's-algorithm) z-order so overlapping labels
  stay legible (`MAX_DISPLAYED_AIRCRAFT` in `app_main.c`). Nearby major
  airports (`airports.c`) are drawn as small solid dots alongside them,
  with no code/name label, to keep the flight path clear of static text.
- Radar range (`range_nm`) can be changed at runtime via the live-config
  server (see above) and takes effect on the next refresh cycle - it's
  re-read every cycle in `app_main.c` rather than fixed at boot. The range
  rings sit at 25/50/75/100% of whatever `range_nm` is currently configured,
  so they stay evenly spaced regardless of range; the 50% ring is labeled
  with its actual distance, just outside the ring (`draw_ring_label` in
  `radar_view.c`) - the other rings aren't individually labeled since
  they're evenly spaced by construction and one readout is enough to infer
  the rest.
- A fetch that fails outright, a malformed JSON document, and a connection
  that closes before the document finishes are all treated as one uniform
  failure: `run_radar_loop` reboots after `MAX_CONSECUTIVE_FAILURES` (5) such
  failures in a row, rather than just the malformed-JSON case.
- The captive DNS responder is best-effort (answers every query with
  `192.168.4.1`); if a device doesn't auto-popup the config page, browse to
  `http://192.168.4.1/` manually.
- AP mode and STA mode are never run simultaneously — provisioning uses
  AP-only, normal operation uses STA-only, switched via a full `esp_restart()`
  after saving config rather than hot-switching WiFi modes.
- Visual layout (colors, label placement, sweep style) is intentionally
  minimal for v1: range rings, a center dot, and a small top-down plane
  icon (`draw_aircraft_icon` in `radar_view.c` - fuselage, main wings, tail
  wings, rotated to heading) + a callsign-only label per aircraft. Altitude
  is conveyed by the icon's color (a hue gradient matching SkyAware's own
  `ColorByAlt` scheme - see `altitude_color` in `radar_view.c`) rather than
  a numeric label.
- Aircraft that broadcast an ADS-B emitter category get a shape/size-
  differentiated icon instead of the generic plane silhouette: helicopters
  (category `A7`) draw as a rotor disc with a tail boom, large aircraft
  (`A3`-`A5` - airliner-class and heavier) draw the same plane silhouette
  scaled up, and light aircraft (`A1`/`A2`/`A6`) draw it scaled down
  (`classify_shape` in `app_main.c`, `draw_aircraft_icon` in `radar_view.c`).
  Not every aircraft broadcasts a category (older/lower ADS-B versions may
  omit it); those fall back to the standard-size plane silhouette.
- Text uses `Fonts/font_sans.c`, a real sans-serif bitmap
  font (glyph data from the Public Domain
  [dhepper/font8x8](https://github.com/dhepper/font8x8) project) rather
  than the serif "Courier New" family the rest of `Fonts/` vendors in, and
  is anti-aliased (supersampled against the source bitmap, see
  `draw_char_scaled` in `radar_view.c`) rather than a raw nearest-neighbor
  blow-up, to stay legible at the small sizes a 240x240 round display
  forces. Refine as desired.

## License

MIT - see [LICENSE](LICENSE).

`main/Fonts/font_sans.c` embeds glyph data from
[dhepper/font8x8](https://github.com/dhepper/font8x8) (Public Domain).
`main/Config/`, `main/LCD/`, `main/GUI/`, and the rest of `main/Fonts/` are
copied from a Waveshare demo project for the display hardware; check with
Waveshare for terms if reusing that portion independently of this project.
