/*
 * ntp_time.c — SNTP client + filename timestamp helper.
 *
 * Uses the modern esp_netif_sntp API (IDF >=5.0). The client runs in the
 * background and we don't block on it: ntp_time_format() falls back to a
 * uptime-based string until the first successful sync.
 */

#include "ntp_time.h"
#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "ntp";

static volatile bool s_synced = false;
static bool          s_started = false;

static void on_sync(struct timeval *tv)
{
    (void)tv;
    if (!s_synced) {
        time_t    now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        ESP_LOGI(TAG, "first sync: %s UTC", buf);
    }
    s_synced = true;
}

void ntp_time_start(const char *server)
{
    if (s_started) {
        return;
    }

#ifdef CONFIG_BATEAR_TF_NTP_SERVER
    const char *host = (server && server[0]) ? server : CONFIG_BATEAR_TF_NTP_SERVER;
#else
    const char *host = (server && server[0]) ? server : "pool.ntp.org";
#endif

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(host);
    cfg.start                  = true;
    cfg.server_from_dhcp       = false;
    cfg.renew_servers_after_new_IP = true;
    cfg.sync_cb                = on_sync;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "SNTP started, server=%s (UTC)", host);
}

bool ntp_time_is_synced(void)
{
    return s_synced;
}

char *ntp_time_format(char *out, size_t out_sz)
{
    if (out == NULL || out_sz < 16) {
        if (out && out_sz) {
            out[0] = '\0';
        }
        return out;
    }

    if (s_synced) {
        time_t    now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(out, out_sz, "%Y%m%d-%H%M%S", &tm);
    } else {
        uint64_t ms = (uint64_t)(esp_timer_get_time() / 1000);
        snprintf(out, out_sz, "boot%09llu", (unsigned long long)ms % 1000000000ULL);
    }
    return out;
}
