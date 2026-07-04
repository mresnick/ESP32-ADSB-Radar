# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Building and flashing the firmware

Requires ESP-IDF v5.x. This repo targets a generic ESP32-WROOM DevKitC-style
board (ESP32 DevKit V1, USB-C) with **no PSRAM** - that constraint drives a lot
of design decisions in `main/` (see Architecture below and
`TROUBLESHOOTING.md`).

Standard ESP-IDF workflow (run the ESP-IDF `export.sh`/`export.ps1`/`export.bat`
for your install first if `idf.py` isn't already on PATH):

```sh
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

Must be run from a native shell (PowerShell/cmd on Windows) - `idf.py`
refuses to fully cooperate under Git Bash/MSYS ("MSys/Mingw is no longer
supported").

## Native (host-side) unit tests (no hardware, no ESP-IDF needed)

Two modules are plain C with no (or stubbed) ESP-IDF dependencies, so
they're tested independently under `test/native/` using the host's own
compiler rather than needing hardware - "native" in the PlatformIO/Zephyr
sense (runs on the dev machine, not the ESP32 target); plain ESP-IDF has no
built-in convention for this, so `test/native/` was chosen to be
recognizable to anyone coming from those ecosystems rather than inventing
new terminology:

```sh
gcc -std=c11 -I main -I test/native/fakes test/native/test_aircraft_model.c main/aircraft_model.c -o test_aircraft_model
./test_aircraft_model

gcc -std=c11 -I main test/native/test_geo_math.c main/geo_math.c -lm -o test_geo_math
./test_geo_math
```

`main/aircraft_model.c`'s JSON scanner needs the fake `esp_log.h` stub
(`-I test/native/fakes`) since it logs via `ESP_LOGx`; `main/geo_math.c`
only pulls in `<math.h>` (link with `-lm` on the host) so it needs no fakes
at all - but note it deliberately avoids `M_PI` (a GNU/POSIX `math.h`
extension that isn't visible under strict `-std=c11` on glibc without extra
feature-test macros) in favor of a locally-defined constant, precisely so it
stays host-testable under the same `-std=c11` flags used here.

A `test/native/CMakeLists.txt` also exists as an alternative that builds
both `test_aircraft_model` and `test_geo_math`, if you'd rather not invoke
`gcc` directly. On a machine with no native C compiler on PATH, run these
commands under WSL or another POSIX environment instead - see
CLAUDE.local.md for this machine's specific setup.

These tests are the primary way to validate changes to the scanner's
streaming/resumability logic (chunk-boundary handling, truncation vs.
malformed detection) and to the distance/bearing/projection math, without
flashing hardware. `test/native/test_aircraft_model.c` documents the
specific scanner scenarios covered (exhaustive chunk-split sweep,
byte-at-a-time feed, truncated/malformed/empty/overflow cases) and
`test/native/test_geo_math.c` documents the geo_math ones (cardinal
bearings, closed-form distance cross-checks, range clamping, the
north-up/clockwise rotation convention) -
extend the matching file, don't build a separate test setup, when adding
test cases for either.

There is no test setup for anything outside `aircraft_model.c`/`geo_math.c`
(the HTTP layer in `skyaware_client.c` is tied to `esp_http_client` and
isn't practically mockable, and `radar_view.c` draws through the vendored
GUI_Paint stack) - verify those changes with an on-device flash + serial
monitor smoke test instead.

## Architecture

**Boot flow** (`main/app_main.c`): `app_main()` loads config from NVS
(`config_store.c`, namespace `"radarcfg"`). No valid config, or the BOOT
button (GPIO0) held at power-on, enters **provisioning mode**
(`run_provisioning_mode`): WiFi AP + captive DNS + a config web form
(`provisioning_http_server.c`), saves to NVS, then reboots. Otherwise it
connects to WiFi (STA), starts `live_config_http_server.c` (see below), and
enters **radar mode** (`run_radar_loop`), which never returns. AP and STA
are never active simultaneously - switching between them always goes
through a full `esp_restart()`.

**Live config** (`main/live_config_http_server.c`): a second, always-on
HTTP server that runs only in STA/radar mode, separate from
`provisioning_http_server.c`'s AP-only one. It lets the SkyAware host/port,
home lat/lon, radar range, max displayed aircraft, refresh interval, and
aircraft label mode be changed from a browser on the home LAN
(`http://<device-ip>/`, gated by HTTP Basic Auth) without holding BOOT to
re-enter AP provisioning. Two things make this safe to keep simple:
- **WiFi credentials are not editable here at all** - only through the
  BOOT-button AP flow. A bad WiFi save applied over WiFi would strand the
  device with no way to reach it to fix it.
- **Changes apply by hot-reload, not reboot**: the server owns a
  mutex-guarded in-memory `radar_config_t` (seeded from the config loaded at
  boot), persists edits via `config_store_save`, and `run_radar_loop` calls
  `live_config_get_current` at the top of every refresh cycle rather than
  reading a single boot-time snapshot - see the `range_nm` bullet below for
  why this changes some of that loop's other assumptions too.

Every request must pass HTTP Basic Auth: `radar_config_t.live_cfg_username`/
`live_cfg_password`, set via `provisioning_http_server.c`'s form (defaulting
to `LIVE_CFG_DEFAULT_USERNAME`/`LIVE_CFG_DEFAULT_PASSWORD`, both
`"adsbradar"`, in `config_store.h`) and editable only through
re-provisioning, same as `wifi_ssid`/`wifi_pass` - `check_auth`
(`live_config_http_server.c`) decodes the `Authorization` header via
`mbedtls_base64_decode` (already in the dependency tree through
`esp_http_client`/esp-tls, and `mbedtls` is listed explicitly in
`main/CMakeLists.txt`'s `REQUIRES` since this is the first code in `main/`
to call into it directly) and compares both fields against `g_cfg`. A 401
response carries `WWW-Authenticate: Basic realm="ESP32-ADSB-Radar"` so browsers
prompt natively and can save/autofill the credentials, rather than requiring
them to be copy-pasted into the URL every time.
There's no other authentication, on the assumption that the home LAN is a
trusted boundary, same as the open AP `provisioning_http_server.c` already
assumes. A config saved before these fields existed has them blank;
`app_main.c` calls `config_store_ensure_live_cfg_auth` right after
`config_store_load` to backfill the same `"adsbradar"` default and
persist it on first boot after an upgrade, specifically so adding
live-config support (or upgrading from an older field set) never itself
requires a BOOT-button reprovision - note this only fills in whichever
field is actually blank, so a previously-set custom password is not
overwritten with the default. `main/http_post_utils.c` holds the POST-body-read and
x-www-form-urlencoded-parsing helpers shared between this and
`provisioning_http_server.c` (plus `http_send_error`, a trivial shared
400-response helper), and `config_store.c` holds `config_store_parse_port`/
`_lat`/`_lon`/`_range_nm` - shared field validation, used by both HTTP
servers' save handlers *and* by `config_store_load` itself, so a submitted
form and a value already sitting in NVS are held to exactly the same
bounds. These three functions/files are deliberately shared even though
`provisioning_http_server.c` and `live_config_http_server.c` are NOT merged
into one module (see TASKS.md item on this if it comes up again) - the
distinction is that parsing/validating a string into a number has no
security or persistence semantics attached to it, unlike auth or how a
save gets applied, which is where the two servers' behavior needs to stay
visibly different rather than unified behind a mode flag. `config_store_load`
enforces the same `home_lat`/`home_lon` range and `range_nm` upper bound as
both HTTP handlers. `config_store_parse_port` checks the parsed value
against `UINT16_MAX` *before* narrowing it to `uint16_t` - casting an
out-of-range value like `"65537"` to `uint16_t` first would silently wrap
it to `1`, which would then pass an `== 0` check and get accepted as a
valid-looking port.

`save_post_handler` re-renders the full config form (via a shared
`render_form` helper, also used by the plain GET) with a "Saved." banner
instead of sending a bare confirmation string, so submitting the form
doesn't feel like navigating to an unrelated page. This is why
`live_config_http_server_start` sets `httpd_config_t.stack_size` to 8192
instead of leaving `HTTPD_DEFAULT_CONFIG`'s 4096 - a handler holding both
its POST-body/field buffers *and* `render_form`'s ~1KB HTML output in the
same stack frame overflowed the default under real load-testing (a
`LoadProhibited` panic, not just a close call) - all of a function's locals
reserve stack space for the whole call, regardless of which line declares
them or how late they're actually written to. If `save_post_handler` or
`render_form` grow further, re-check this margin on real hardware rather
than assuming a build success proves it's still large enough - this class
of bug is invisible at compile time.

Lat/lon entry is manual-only: browsers only expose `navigator.geolocation`
on a "secure context" (HTTPS or `localhost`), and neither server serves
HTTPS, so a "Use My Location" button isn't viable here. See TASKS.md for
why HTTPS itself wasn't pursued.

Both forms' `<head>` also carries a `viewport` meta tag and a small inline
`<style>` block (larger touch-friendly `input`s, a readable base font size,
`max-width` so it doesn't stretch edge-to-edge on a tablet/desktop) - without
the viewport tag specifically, mobile browsers render at a desktop-width
virtual viewport and shrink the whole page to fit, forcing pinch-zooming
to read or fill in the form. `live_config_http_server.c`'s copy
lives inside `render_form`'s `snprintf` call, which matters for one
non-obvious reason: a literal `%` in the CSS (`width:100%`) has to be
written as `%%` there, since `snprintf` would otherwise try to parse it as
the start of a conversion specifier - undefined behavior for an
unrecognized one. `provisioning_http_server.c`'s `FORM_HTML` has no such
requirement (it's sent as a raw compile-time string, never passed through a
`printf`-family function), so don't copy the `%%` habit over there by
reflex.

**Fetch/parse pipeline is fully streaming, with zero heap allocation** - this
is the single most important thing to understand before touching
`skyaware_client.c` or `aircraft_model.c`:

- `skyaware_client_fetch` (`skyaware_client.c`) does not buffer the HTTP
  response at all. It forwards each `esp_http_client` receive chunk directly
  to a caller-supplied callback as it arrives.
- `aircraft_model_scanner_t` (`aircraft_model.c`) is fed those chunks
  (`aircraft_model_scanner_feed`) and parses incrementally: a small state
  machine re-attempts each structural "step" (one key, one aircraft object,
  etc.) from the start of a fixed ~2KB carry buffer (`AIRCRAFT_SCAN_BUF_SIZE`)
  every time more data arrives, compacting consumed bytes out on success.
  There is **no whole-document size cap** - only a per-record buffer, because
  `parse_aircraft_object` resets its output struct on every attempt, so
  restarting a step from scratch on a chunk boundary is always safe.
- Every scanner primitive (`extract_string`, `extract_number`, `skip_value`,
  `skip_container`, `parse_aircraft_object`) returns a 3-way
  `STEP_OK` / `STEP_NEED_MORE` / `STEP_ERROR` result rather than `bool`,
  because "ran off the end of the currently-buffered bytes" (more may still
  be coming) must be distinguished from "hit a genuinely invalid byte."
  Getting this reclassification wrong at any primitive is the most likely
  source of streaming bugs - the chunk-boundary sweep in
  `test/native/test_aircraft_model.c` exists specifically to catch that
  class of bug.
- `aircraft_model.c` emits one `aircraft_t` per completed object via
  callback and stays deliberately ignorant of `geo_math.h`/`radar_config_t` -
  it has no concept of distance or relevance. It does parse `category` (the
  raw ADS-B emitter-category code, e.g. `"A7"`) into `aircraft_t`, but only
  as an opaque string - interpreting what a code means is `app_main.c`'s
  `classify_shape()`, not this module's concern, same reasoning as
  `ranking_priority` living in `app_main.c` rather than here.
- `classify_shape()` (`app_main.c`) maps `category` to a coarse
  `aircraft_shape_t` (`radar_view.h`) - `AIRCRAFT_SHAPE_ROTORCRAFT` (A7),
  `AIRCRAFT_SHAPE_LARGE` (A3-A5), `AIRCRAFT_SHAPE_LIGHT` (A1/A2/A6), or
  `AIRCRAFT_SHAPE_UNKNOWN` for anything else (including a blank category -
  not every aircraft broadcasts one). `radar_view.c`'s `draw_aircraft_icon`
  dispatches on this: rotorcraft get a genuinely different silhouette
  (`draw_helicopter_icon` - a rotor-blade X + tail boom, not a resized
  plane), while large/light aircraft get the same fixed-wing silhouette
  scaled up/down by `PLANE_LARGE_SCALE`/`PLANE_LIGHT_SCALE` rather than
  separate geometries - `AIRCRAFT_SHAPE_UNKNOWN` stays at the neutral 1.0
  scale (no size claim for an aircraft that didn't report a category at
  all). `AIRCRAFT_ICON_MAX_REACH_PX` takes the max of `PLANE_MAX_REACH_PX`
  (the plane silhouette's own reach - `PLANE_LARGE_SCALE` is what grows
  this, since `PLANE_LIGHT_SCALE` only shrinks it) and `HELI_MAX_REACH_PX`,
  so `TARGET_LABEL_GAP_PX` clears whichever icon was actually drawn.
- `resolve_label()` (`app_main.c`) picks what `radar_target_t.label`
  (`radar_view.h`) shows per `radar_config_t.label_mode` -
  `RADAR_LABEL_CALLSIGN` (default), `RADAR_LABEL_AIRCRAFT_TYPE` (SkyAware's
  `"t"` field, e.g. `"B738"`), `RADAR_LABEL_TAIL_NUMBER` (`"r"`, e.g.
  `"N12345"`), or `RADAR_LABEL_NONE` (no label at all). Both `t`/`r` come
  from SkyAware's own aircraft database lookup, not the aircraft's
  broadcast, so either can be blank even when callsign isn't -
  `resolve_label()` falls back to `aircraft_t.flight` (itself already
  guaranteed non-empty, falling back to ICAO hex - see `aircraft_model.c`)
  whenever the requested field is blank for a given aircraft, so an empty
  `label` unambiguously means `RADAR_LABEL_NONE` rather than "field
  missing." `radar_view.c` stays entirely unaware of `label_mode` itself -
  it just skips drawing a label at all when `label` is empty, and otherwise
  draws whatever string is in it.
- `app_main.c`'s `on_aircraft_parsed` callback does the ranking: it computes
  distance for every aircraft as it streams in and keeps only the top
  `NEAREST_K` (= `MAX_DISPLAYED_AIRCRAFT`, 8) seen so far, in a small
  insertion-sorted buffer. This is deliberate - ranking during parse means an
  aircraft's position in the server's JSON array can never cause a relevant
  plane to be dropped in favor of a less relevant one.
- The insertion-sort key is `ranking_priority` (distance plus a mild
  per-1000ft altitude penalty), not raw distance - see the comment above
  that function for the reasoning. `dist_nm` on each kept aircraft is always
  the true, unpenalized great-circle distance; only the *selection* of which
  `NEAREST_K` get kept uses the penalized value. Don't let `priority` leak
  into the range cutoff (`> nctx->cfg->range_nm`) or the drawn position -
  both must stay geometrically accurate.
- `nearest_ctx_t.total_in_range` counts every in-range airborne aircraft
  seen, independent of the `NEAREST_K` cap - `run_radar_loop` uses
  `total_in_range - shown` to tell `radar_view_draw_radar` how many aircraft
  are being hidden, so a busy feed shows a "+N" indicator instead of quietly
  looking complete.
- `range_nm` (like `sky_host`/`sky_port`/`home_lat`/`home_lon`/`max_aircraft`/
  `refresh_interval_sec`/`label_mode`) can change at runtime via `live_config_http_server.c` -
  `run_radar_loop` re-reads all of these from `live_config_get_current` at
  the top of every refresh cycle rather than trusting a single boot-time
  snapshot. This is a *user-driven* edit through an explicit form, not
  automatic/algorithmic - if automatic range adjustment (e.g. based on live
  traffic density) is ever added, keep it as its own separate value rather
  than overloading `cfg.range_nm`, since live-config edits and that kind of
  automatic adjustment would otherwise fight over the same field.
- Fetch-level failure, malformed JSON, and a connection that closes before
  the document reaches a clean end are all treated as one uniform failure
  case in `run_radar_loop`, incrementing a single `consecutive_failures`
  counter that triggers `esp_restart()` after `MAX_CONSECUTIVE_FAILURES` (5)
  in a row.
- Nearby major airports (`airports.c`, a small hand-curated `const` table -
  see its header comment for scope/accuracy caveats) are filtered by
  `build_airport_markers`, called once per refresh cycle in `run_radar_loop`
  (cheap - a linear scan of a short table - and necessary now that
  `home_lat`/`home_lon`/`range_nm` can change at runtime via live-config)
  and drawn as `radar_marker_t` points distinct from aircraft
  `radar_target_t`s - `draw_marker_dot` (`radar_view.c`) is a
  small solid circle, no label and no heading/plane icon.
  `radar_marker_t` intentionally carries no label field: a code/name label
  would sit in the flight path and get covered by aircraft labels passing
  over it.
- Range rings are drawn at 25/50/75/100% of the configured `range_nm` so
  they stay evenly spaced regardless of what range is configured. Only the
  50% ring gets a distance label (`draw_ring_label` in `radar_view.c`,
  called once with `RING_FRACTIONS[1]`) - rings are evenly spaced by
  construction, so one readout is enough to infer the rest, and
  labeling all of them was visually crowded. The label sits just *outside*
  the ring along the positive-x (east) axis.

`TROUBLESHOOTING.md` documents the full reasoning behind this design - read
it before changing the parsing/fetch path, since several non-obvious
constraints (no PSRAM, per-record vs. whole-document buffering) are easy to
accidentally undo.

**Display driver stack** (`main/Config/`, `main/LCD/`, `main/GUI/`,
`main/Fonts/`) was copied verbatim from a sibling Waveshare demo project -
treat it as a vendored dependency, not application logic. **Exception:**
`main/Fonts/font_sans.c` is not part of that vendored set despite living in
the same directory - it's project-specific (currently sourced from the
Public Domain `dhepper/font8x8`, see its own file header for why and for
provenance), used because every actual Waveshare font in `main/Fonts/`
(`Font8`/`12`/`16`/`20`/`24`) turned out to be the same serif "Courier New"
design at different sizes. One vendored behavior matters for `radar_view.c`
even so: `Paint_DrawRectangle` and
`Paint_DrawLine` (`main/GUI/GUI_Paint.c`) reject the *entire* call if any
single coordinate falls outside the canvas, rather than clipping it -
unlike `Paint_SetPixel`, which is safely per-pixel-bounded. Anything in
`radar_view.c` that computes a rectangle/line origin from a target's raw
projected screen position (`draw_target_label` is the current one -
`draw_marker_dot` has no rect/label to draw, so it doesn't need this) has
to clamp that origin to stay fully on-canvas itself; trusting the
projection alone lets labels vanish outright for targets near the edge of
the round display (see `draw_target_label`'s clamp comment).
