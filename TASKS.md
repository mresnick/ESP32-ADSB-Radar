# Roadmap

Known gaps and feature ideas for the ESP32-ADSB-Radar project. Not in
priority order unless noted.

## Robustness / test coverage

- No automated coverage of `radar_view.c`'s label-placement/edge-clamping
  behavior - hard to host-test without a fake `GUI_Paint`, but a fake
  framebuffer + bounds-check stub could be worth adding if that logic needs
  to change again.
- `main/wifi_manager.c`'s STA reconnect retries unconditionally and forever,
  with no backoff, once a connection has succeeded at least once. Reasonable
  as a default; worth deciding whether a very long outage should eventually
  fall back to provisioning mode, or at least surface something
  distinguishable from "still trying."
- `main/live_config_http_server.c`'s only auth is HTTP Basic Auth with a
  username/password set at provisioning time (defaulting to
  `adsbradar`/`adsbradar`) - fine for a home LAN (and better than the
  query-string token this replaced, since browsers can save/autofill it),
  but there's still no way to change the credentials short of
  re-provisioning, and no mDNS/hostname (you need the device's current IP,
  logged at boot). Worth revisiting if this ever runs somewhere less
  trusted than a home network - Basic Auth over plain HTTP sends the
  password in a trivially-decoded (not encrypted) form on every request, and
  anyone who doesn't change the default has a guessable login.

## Feature ideas

### 1. Higher-resolution sans-serif font
`main/Fonts/font_sans.c` is currently 8x8px native resolution (from the
Public Domain [dhepper/font8x8](https://github.com/dhepper/font8x8)
project). A taller font asset (native 16-20px, more typical for GC9A01/
round-LCD projects) would read even more cleanly at small scale, but needs
sourcing/porting a new glyph set - bigger effort, not attempted here.

### 2. Reducing clutter near a busy airport
Altitude-aware ranking (`ranking_priority` in `app_main.c`) and the "+N
hidden" overflow indicator already help. Still open:
- Cluster/dedupe visually-overlapping dots when a large configured range
  puts several aircraft's screen positions within a few pixels of each
  other, rather than drawing full individual labels for each.

### 3. Broader airport coverage
`main/airports.c`'s table currently covers major US hubs, hand-typed from
well-known reference points (see the file's own header comment for accuracy
caveats - not survey-grade). Extending it to international/regional
airports is mechanical (it's a flat array); swapping in authoritative
coordinates (e.g. from [OurAirports.com](https://ourairports.com/)) would
help if precision ever matters more than "roughly the right spot on a
configured-range display."

### 4. Web flasher
Would need: a build step producing the flashable binaries (bootloader,
partition table, app) at their correct flash offsets, or a single merged
image via `esptool.py merge_bin`; a small static page + `manifest.json` for
something like [ESP Web Tools](https://esphome.github.io/esp-web-tools/)
(WebSerial-based, Chrome/Edge only); and a hosting decision (GitHub Pages or
elsewhere), plus whether to commit pre-built binaries or produce them via CI.

### 5. mDNS for the live-config server
`main/live_config_http_server.c` (see CLAUDE.md) is currently reached by IP
address only, logged at boot. Advertising it via mDNS (e.g.
`adsbradar.local`) would mean never needing to look up the current IP,
at the cost of a new component dependency (`mdns`) and its own Kconfig
surface - not attempted here.

### 6. Browser geolocation for lat/lon entry - tried, reverted
A "Use My Location" button (backed by `navigator.geolocation`) was added to
both the provisioning and live-config forms, then removed - it only works if
the browser treats the page as a "secure context," which plain HTTP on a
device IP typically isn't, so it was dead weight for virtually everyone.
Serving these over HTTPS instead (a self-signed cert, since there's no real
hostname to get a CA-signed one for) would fix that, but was deliberately
not pursued: for a project whose source/firmware is public, a single
baked-in cert+key would be extractable and reusable against any device
running it, so a *correct* implementation needs a per-device cert generated
and stored at first boot - `esp_https_server` plus mbedTLS X.509
generation/NVS storage, real complexity - and even then every new
browser/device hits a "connection not private" warning to click through.
Not worth it for one convenience button when manual lat/lon entry already
works.
