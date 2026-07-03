#ifndef LIVE_CONFIG_HTTP_SERVER_H
#define LIVE_CONFIG_HTTP_SERVER_H

#include "config_store.h"

// Starts a small always-on HTTP server (in STA mode, alongside the radar
// loop) for adjusting the SkyAware host/port, home lat/lon, and radar range
// without holding BOOT to re-enter AP provisioning. WiFi credentials are
// deliberately not editable here - a bad WiFi save applied over WiFi would
// strand the device with no way to fix it short of physical BOOT-button
// recovery, defeating the point of a remote config endpoint.
//
// Every request must pass HTTP Basic Auth with the config's
// live_cfg_username/live_cfg_password (see config_store.h - set via
// provisioning, defaulting to LIVE_CFG_DEFAULT_USERNAME/PASSWORD there, and
// backfilled for upgrades by config_store_ensure_live_cfg_auth) - browsers
// natively prompt for this and can save/autofill it, unlike a token that
// has to be copied into the URL each time. There is no other
// authentication; this assumes the home LAN itself is a trusted boundary,
// the same assumption main/provisioning_http_server.c's open AP already
// makes.
//
// `initial_cfg` seeds the live copy this server edits and persists - call
// once, after STA connects and after config_store_ensure_live_cfg_auth has
// run.
void live_config_http_server_start(const radar_config_t *initial_cfg);

// Copies the current live-editable config into *out. Cheap (a small
// fixed-size struct copy under a mutex, no allocation) - safe to call once
// per radar refresh cycle so config edits apply without a reboot. Must be
// called after live_config_http_server_start.
void live_config_get_current(radar_config_t *out);

#endif
