/*
 * http_api.h — REST API + OTA for Wired Detector
 *
 * Endpoints (served on CONFIG_BATEAR_HTTP_PORT, default 8080):
 *   GET  /api/info     device metadata
 *   GET  /api/status   current detection state
 *   POST /api/ota      firmware upload (OTA)
 *   POST /api/config   update NVS keys (JSON)
 *   POST /api/reboot   restart device
 */
#pragma once

#include "drone_detector.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of the latest detection event, updated by EthMqttTask */
typedef struct {
    bool     drone_detected;
    uint8_t  detector_id;
    uint8_t  rms_db;
    uint16_t f0_bin;
    float    confidence;
    int64_t  timestamp;
} HttpDroneStatus_t;

/* Update the shared status (called from EthMqttTask main loop) */
void http_api_update_status(const HttpDroneStatus_t *status);

/* Start the HTTP server; call after Ethernet is up */
void http_api_start(void);

#ifdef __cplusplus
}
#endif
