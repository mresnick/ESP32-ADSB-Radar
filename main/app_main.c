#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "DEV_Config.h"
#include "GUI_Paint.h"
#include "LCD_1in28.h"

#include "aircraft_model.h"
#include "airports.h"
#include "captive_dns.h"
#include "config_store.h"
#include "geo_math.h"
#include "live_config_http_server.h"
#include "provisioning_http_server.h"
#include "radar_view.h"
#include "skyaware_client.h"
#include "wifi_manager.h"

static const char *TAG = "app_main";

// How many in-range aircraft to draw is user-configurable at runtime
// (radar_config_t.max_aircraft, 1..MAX_AIRCRAFT_CAP - see config_store.h)
// rather than fixed, but the static arrays below still need a compile-time
// size, so they're sized to the hard ceiling (MAX_AIRCRAFT_CAP) regardless
// of what's actually configured.
// The aircraft scanner emits every aircraft in the feed (no server-order
// cutoff); nearest_ctx_t.nearest[] holds however many of the nearest-so-far
// cfg->max_aircraft asks for, ranked as they stream in. No slack needed
// beyond that - ranking happens continuously during parse, so the kept set
// IS the shown set (the old design collected an arbitrary server-order
// prefix, then filtered and truncated by distance afterward, which is what
// let far-array-position aircraft crowd out genuinely nearby ones).
// Seconds between SkyAware fetches is also user-configurable at runtime
// (radar_config_t.refresh_interval_sec, REFRESH_INTERVAL_MIN_SEC..
// REFRESH_INTERVAL_MAX_SEC - see config_store.h) - fetches only take
// 0.2-2.5s in practice, so REFRESH_INTERVAL_DEFAULT_SEC (3s) leaves plenty
// of margin without hammering the SkyAware server, but someone who wants a
// faster/slower radar can now adjust it.
#define STA_CONNECT_TIMEOUT_MS 20000
// If we go this many cycles in a row without a usable radar frame - whether
// from an HTTP-level failure, a malformed/truncated feed, or a connection
// that closed before the document finished - something is wrong at a level
// below our retry logic (e.g. heap fragmentation after long uptime, or the
// feed persistently exceeding what a single record's scan buffer can hold).
// A clean reboot is a safer bet than staying stuck showing a stale/empty
// radar forever.
#define MAX_CONSECUTIVE_FAILURES 5
// g_major_airports (main/airports.c) is a short hand-curated list, so in
// practice at most one or two entries are ever within range_nm of a given
// home position - this just bounds the static array defensively, not a
// tuning knob anyone needs to raise.
#define MAX_AIRPORT_MARKERS 6

// Painter's-algorithm z-order: draw highest-altitude (background) targets
// first so lower-altitude, closer-to-the-ground targets are drawn on top
// and stay legible when labels overlap. Unknown-altitude targets sort as
// if highest, so a target with a known altitude always wins the overlap.
static int cmp_altitude_desc(const void *a, const void *b)
{
    const radar_target_t *ta = a;
    const radar_target_t *tb = b;
    int alt_a = ta->has_altitude ? ta->altitude_ft : INT32_MAX;
    int alt_b = tb->has_altitude ? tb->altitude_ft : INT32_MAX;
    return alt_b - alt_a;
}

static void run_provisioning_mode(void)
{
    radar_view_draw_status("CONNECT TO WIFI", "ADSBRADAR-SETUP");

    wifi_manager_start_ap("ADSBRadar-Setup", NULL);
    captive_dns_start();
    provisioning_http_server_start();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

typedef struct {
    aircraft_t ac;
    double dist_nm;   // true distance - what's actually drawn/reported
    double priority;  // ranking key used only to pick which K get shown
} ranked_aircraft_t;

typedef struct {
    const radar_config_t *cfg;
    ranked_aircraft_t nearest[MAX_AIRCRAFT_CAP];  // ascending by priority; nearest[0] ranks first
    int count;                                     // valid entries, <= cfg->max_aircraft
    int total_in_range;                            // every in-range airborne aircraft seen, uncapped
} nearest_ctx_t;

// A mild per-1000ft penalty added to true distance to produce the ranking
// key. Someone running this near a busy airport cares more about a plane
// 8nm out on final at 1,500ft than a cruise-altitude plane 6nm out passing
// overhead at 35,000ft - pure nearest-first ranking treats those as
// "the second one wins," which buries exactly the traffic that's actually
// relevant. This nudges low-altitude traffic up without overriding
// distance outright: a plane has to be meaningfully closer at high altitude
// to still beat a low one (35,000ft costs it the equivalent of ~5.3nm here),
// but a small altitude difference at comparable distance doesn't flip the
// order unpredictably. Unknown altitude gets no penalty - a plane that
// hasn't broadcast alt_baro yet is at least as likely to be low (e.g. just
// airborne) as high, so guessing against it would bias the wrong way.
#define ALTITUDE_PENALTY_NM_PER_1000FT 0.15

static double ranking_priority(const aircraft_t *ac, double dist_nm)
{
    if (!ac->has_altitude) {
        return dist_nm;
    }
    return dist_nm + (ac->altitude_ft / 1000.0) * ALTITUDE_PENALTY_NM_PER_1000FT;
}

// Invoked once per aircraft as aircraft_model_scanner_feed parses the feed
// incrementally - the whole document gets scanned (bounded only by the
// scanner's per-record buffer, not by an aircraft count), so ranking here,
// as each one arrives, means a relevant plane can never lose out to a less
// relevant one purely because of where it landed in the server's JSON
// array. `nearest[]` is sorted by `priority` (distance + altitude penalty,
// see ranking_priority) rather than raw distance, but `dist_nm` itself is
// always the true great-circle distance - only used for the range cutoff,
// the on-screen position, and the altitude z-order, never overridden.
static void on_aircraft_parsed(const aircraft_t *ac, void *user_ctx)
{
    nearest_ctx_t *nctx = (nearest_ctx_t *)user_ctx;
    if (!ac->has_position || ac->on_ground) {
        return;
    }
    double dist_nm = geo_math_distance_nm(nctx->cfg->home_lat, nctx->cfg->home_lon, ac->lat, ac->lon);
    if (dist_nm > nctx->cfg->range_nm) {
        return;
    }
    nctx->total_in_range++;
    double priority = ranking_priority(ac, dist_nm);
    int max_aircraft = nctx->cfg->max_aircraft;

    if (nctx->count < max_aircraft) {
        int i = nctx->count;
        while (i > 0 && nctx->nearest[i - 1].priority > priority) {
            nctx->nearest[i] = nctx->nearest[i - 1];
            i--;
        }
        nctx->nearest[i].ac = *ac;
        nctx->nearest[i].dist_nm = dist_nm;
        nctx->nearest[i].priority = priority;
        nctx->count++;
    } else if (priority < nctx->nearest[max_aircraft - 1].priority) {
        int i = max_aircraft - 1;
        while (i > 0 && nctx->nearest[i - 1].priority > priority) {
            nctx->nearest[i] = nctx->nearest[i - 1];
            i--;
        }
        nctx->nearest[i].ac = *ac;
        nctx->nearest[i].dist_nm = dist_nm;
        nctx->nearest[i].priority = priority;
    }
    // else: already holding max_aircraft higher-priority aircraft - discard.
}

// Maps a raw ADS-B emitter-category code (SkyAware's "category" field, e.g.
// "A7") to the coarse icon-shape bucket radar_view.c draws. Category codes
// per the ADS-B spec: A1/A2 light/small, A3-A5 large/high-vortex-large/
// heavy, A6 high-performance, A7 rotorcraft; B/C/D codes cover gliders,
// UAVs, surface vehicles, and obstacles - none of which get a distinct icon
// here. Not every aircraft broadcasts this at all (older/lower ADS-B
// versions may omit it) - a blank category and any code not listed above
// both fall through to AIRCRAFT_SHAPE_UNKNOWN, which draws the same generic
// silhouette this project always has.
static aircraft_shape_t classify_shape(const char *category)
{
    if (strcmp(category, "A7") == 0) {
        return AIRCRAFT_SHAPE_ROTORCRAFT;
    }
    if (strcmp(category, "A3") == 0 || strcmp(category, "A4") == 0 || strcmp(category, "A5") == 0) {
        return AIRCRAFT_SHAPE_LARGE;
    }
    if (strcmp(category, "A1") == 0 || strcmp(category, "A2") == 0 || strcmp(category, "A6") == 0) {
        return AIRCRAFT_SHAPE_LIGHT;
    }
    return AIRCRAFT_SHAPE_UNKNOWN;
}

// Thin adapter: skyaware_client_fetch calls this once per HTTP receive
// chunk; it just forwards into the aircraft scanner, which is what
// actually knows how to resume parsing across chunk boundaries.
static void on_http_chunk(const char *data, size_t len, void *user_ctx)
{
    aircraft_model_scanner_feed((aircraft_model_scanner_t *)user_ctx, data, len);
}

// Airports themselves are static, but which ones fall within range_nm can
// change if the live-config server (live_config_http_server.c) adjusts
// home_lat/home_lon/range_nm - so this is recomputed every refresh cycle
// rather than once at startup. The table is short (main/airports.c) and
// this only runs once per refresh_interval_sec, so the cost is negligible.
// Returns the number of markers written to `out` (<= MAX_AIRPORT_MARKERS).
static int build_airport_markers(const radar_config_t *cfg, radar_marker_t *out)
{
    int n = 0;
    for (size_t i = 0; i < g_major_airports_count && n < MAX_AIRPORT_MARKERS; i++) {
        const airport_t *ap = &g_major_airports[i];
        double dist_nm = geo_math_distance_nm(cfg->home_lat, cfg->home_lon, ap->lat, ap->lon);
        if (dist_nm > cfg->range_nm) {
            continue;
        }
        out[n].dist_nm = dist_nm;
        out[n].bearing_deg = geo_math_bearing_deg(cfg->home_lat, cfg->home_lon, ap->lat, ap->lon);
        n++;
    }
    return n;
}

// Takes the initial config by value (it's a small fixed-size struct) since
// each iteration below refreshes its own local copy from
// live_config_http_server.c rather than reading a single boot-time
// snapshot - that's what lets the live-config server's edits (SkyAware
// host/port, home lat/lon, radar range, max aircraft, refresh interval)
// take effect without a reboot.
static void run_radar_loop(radar_config_t cfg)
{
    // static: the scanner (~2KB) and targets array would otherwise sit on
    // app_main's stack, which is too small for them (see the earlier
    // LCD_1IN28_Clear stack-overflow bug in the sibling ESP32/ demo project
    // for the same class of issue) - run_radar_loop never returns and is
    // never re-entered concurrently, so static storage is safe here.
    static aircraft_model_scanner_t scanner;
    static radar_target_t targets[MAX_AIRCRAFT_CAP];
    static radar_marker_t markers[MAX_AIRPORT_MARKERS];
    int consecutive_failures = 0;

    while (1) {
        live_config_get_current(&cfg);
        int marker_count = build_airport_markers(&cfg, markers);

        nearest_ctx_t nctx = { .cfg = &cfg, .count = 0 };
        aircraft_model_scanner_init(&scanner, on_aircraft_parsed, &nctx);

        esp_err_t err = skyaware_client_fetch(cfg.sky_host, cfg.sky_port, on_http_chunk, &scanner);

        ESP_LOGD(TAG, "heap: free=%u largest_8bit=%u largest_dma=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

        aircraft_scan_outcome_t outcome = aircraft_model_scanner_outcome(&scanner);

        if (err == ESP_OK && outcome == AIRCRAFT_SCAN_COMPLETE) {
            consecutive_failures = 0;

            int shown = nctx.count;
            for (int i = 0; i < shown; i++) {
                const aircraft_t *ac = &nctx.nearest[i].ac;
                targets[i].dist_nm = nctx.nearest[i].dist_nm;
                targets[i].bearing_deg = geo_math_bearing_deg(cfg.home_lat, cfg.home_lon, ac->lat, ac->lon);
                targets[i].heading_deg = ac->track;
                targets[i].altitude_ft = ac->altitude_ft;
                targets[i].has_altitude = ac->has_altitude;
                targets[i].shape = classify_shape(ac->category);
                strncpy(targets[i].callsign, ac->flight, sizeof(targets[i].callsign) - 1);
                targets[i].callsign[sizeof(targets[i].callsign) - 1] = '\0';
            }

            // Only the final, already-selected nearest set needs z-order -
            // bearing/z-order for candidates that didn't make the cut would
            // have been wasted work, so it's never computed for them.
            qsort(targets, shown, sizeof(targets[0]), cmp_altitude_desc);

            int hidden = nctx.total_in_range - shown;
            radar_view_draw_radar(cfg.range_nm, markers, marker_count, targets, shown, hidden > 0 ? hidden : 0);
            ESP_LOGI(TAG, "rendered %d aircraft in range (%lu total in feed)",
                     shown, aircraft_model_scanner_total_emitted(&scanner));
        } else {
            consecutive_failures++;

            if (err != ESP_OK) {
                ESP_LOGW(TAG, "SkyAware fetch failed: %s", esp_err_to_name(err));
                radar_view_draw_status("SKYAWARE FETCH FAILED", "RETRYING");
            } else if (outcome == AIRCRAFT_SCAN_MALFORMED) {
                ESP_LOGW(TAG, "malformed aircraft.json (%lu bytes fed, %lu aircraft emitted before failure)",
                         scanner.total_bytes_fed, aircraft_model_scanner_total_emitted(&scanner));
                radar_view_draw_status("SKYAWARE RESPONSE", "MALFORMED - RETRYING");
            } else {
                // Transport reported success but the document never reached
                // a clean end - the connection closed mid-transfer. Aircraft
                // that completed before the cutoff were still emitted via
                // callback and already factored into nctx, but the frame as
                // a whole is untrustworthy (we don't know what else was in
                // the part we never received), so it's discarded rather
                // than rendered.
                ESP_LOGW(TAG, "truncated aircraft.json (%lu bytes fed, %lu aircraft emitted before cutoff)",
                         scanner.total_bytes_fed, aircraft_model_scanner_total_emitted(&scanner));
                radar_view_draw_status("SKYAWARE RESPONSE", "INCOMPLETE - RETRYING");
            }

            if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                ESP_LOGE(TAG, "%d consecutive failures - rebooting to recover", consecutive_failures);
                esp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS((int)(cfg.refresh_interval_sec * 1000)));
    }
}

void app_main(void)
{
    DEV_ModuleInit();
    LCD_1IN28_Init(HORIZONTAL);
    LCD_1IN28_Clear(BLACK);
    LCD_SetBacklight(1023);
    radar_view_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_config_t boot_btn_cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_btn_cfg);
    bool force_reconfig = (gpio_get_level(GPIO_NUM_0) == 0);

    radar_config_t cfg;
    bool have_cfg = (config_store_load(&cfg) == ESP_OK);

    // esp_netif/esp_event are used by both AP (provisioning) and STA (radar)
    // modes, and each can only be initialized once per boot - do it here,
    // unconditionally, rather than in each mode's own setup function, since
    // a failed STA attempt falls back into provisioning mode within the same
    // boot (see below).
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (force_reconfig || !have_cfg) {
        ESP_LOGI(TAG, "entering provisioning mode (force=%d have_cfg=%d)", force_reconfig, have_cfg);
        run_provisioning_mode();
        return;
    }

    // A config saved before live_config_http_server.c existed (or before
    // its username field existed) won't have these - default them now so
    // live-config support never requires holding BOOT to re-provision just
    // to pick up a new field.
    config_store_ensure_live_cfg_auth(&cfg);
    config_store_ensure_max_aircraft(&cfg);
    config_store_ensure_refresh_interval(&cfg);

    radar_view_draw_status("CONNECTING TO WIFI", NULL);

    if (wifi_manager_start_sta_and_wait(cfg.wifi_ssid, cfg.wifi_pass, STA_CONNECT_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "STA connect failed, falling back to provisioning mode");
        radar_view_draw_status("WIFI FAILED", "STARTING SETUP AP");
        run_provisioning_mode();
        return;
    }

    char ip_str[16] = "?";
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (sta_netif != NULL && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }
    ESP_LOGI(TAG, "live config: http://%s/ (user=%s pass=%s)", ip_str, cfg.live_cfg_username, cfg.live_cfg_password);
    radar_view_draw_status("LIVE CONFIG AT", ip_str);
    vTaskDelay(pdMS_TO_TICKS(3000));

    live_config_http_server_start(&cfg);
    run_radar_loop(cfg);
}
