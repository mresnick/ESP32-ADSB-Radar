# Troubleshooting

## JSON parsing and heap exhaustion

The SkyAware `aircraft.json` feed can become large, and a base ESP32-WROOM
board has no PSRAM available for a full DOM-style JSON parser.

This project avoids those memory issues by using a hand-written, zero-allocation
*incremental* JSON scanner in `main/aircraft_model.c` (`aircraft_model_scanner_t`)
instead of cJSON or another full tree parser. `main/skyaware_client.c` streams
HTTP response chunks straight into the scanner as they arrive
(`aircraft_model_scanner_feed`) rather than buffering the whole response first
- there is no whole-document size cap. The only fixed buffer is
`AIRCRAFT_SCAN_BUF_SIZE` (2KB, `main/aircraft_model.h`), which only needs to
hold one in-progress JSON record (a single aircraft object plus a little
headroom), not the entire feed. A single record that somehow exceeds that
(a pathological/corrupt feed, or dump1090's per-aircraft schema growing
enough fields to need more room) fails cleanly as `AIRCRAFT_SCAN_MALFORMED`
rather than overflowing.

An earlier version of this project buffered the entire HTTP response (capped
at 64KB) before parsing it in one pass, and separately capped the number of
aircraft it would parse out of that buffer. Both caps were real problems: the
byte cap meant a response that grew just past it failed the *entire* fetch
with no partial credit, and the aircraft-count cap stopped at whatever
happened to come first in the server's JSON array - which had nothing to do
with which aircraft were actually nearby. Neither exists anymore: every
aircraft in the feed gets parsed and ranked by real distance as it streams in
(`on_aircraft_parsed` in `main/app_main.c`), keeping only the nearest
`NEAREST_K` seen so far, regardless of array order.

## Failure/recovery behavior

A fetch that fails at the HTTP level, a genuinely malformed JSON document,
and a connection that closes before the document finishes are all treated as
one uniform failure in `run_radar_loop` (`main/app_main.c`) - each increments
the same `consecutive_failures` counter, and the device reboots after
`MAX_CONSECUTIVE_FAILURES` (5) in a row, on the theory that something is
wrong at a level below the normal retry loop (heap fragmentation after long
uptime, a persistently oversized single record, etc.) and a clean reboot is
safer than getting stuck showing a stale or empty radar indefinitely.

## HTTP client `buffer_size` and a resolved corruption bug

While still on the buffer-then-parse design (see above), responses
occasionally came back with a short fragment repeated dozens of times
mid-document - a real corruption, not just truncation. Raising
`esp_http_client_config_t.buffer_size` from the default (512) to 8192
didn't fix it, and the fix was reverted back to the default while that was
being investigated further.

That investigation is closed: the switch to the fully incremental scanner
(`aircraft_model.c`) removed the class of bug this could have been.
`buffer_size` only controls how `esp_http_client` chunks bytes on their way
to `HTTP_EVENT_ON_DATA`; the old design cared about that because it appended
chunks into one whole-response buffer position-by-position, so a bug in
that bookkeeping could plausibly duplicate a fragment. The current scanner
re-attempts each structural step from byte 0 of its own small carry buffer
on every call (see "Fetch/parse pipeline" in `CLAUDE.md`) and is
consequently indifferent to how the underlying chunks are sliced.
`buffer_size` stays at the default in `skyaware_client.c` because there's no
longer a reason to move it - not because the original bug is still being
chased.

## Build-related note

If you hit parse failures during development, verify that the target device is
an ESP32-WROOM without PSRAM. This project is designed around that memory
constraint - it no longer depends on the SkyAware feed staying under a fixed
total size, only on individual aircraft records staying under
`AIRCRAFT_SCAN_BUF_SIZE`.
