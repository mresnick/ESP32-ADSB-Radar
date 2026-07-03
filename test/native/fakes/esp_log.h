#ifndef FAKE_ESP_LOG_H
#define FAKE_ESP_LOG_H

// Host-build stand-in for ESP-IDF's esp_log.h - just enough for
// aircraft_model.c to compile and run outside the firmware. Real device
// builds use the genuine ESP-IDF header; this one only exists under
// test/native/ and is never seen by idf.py build.
#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)

#endif
