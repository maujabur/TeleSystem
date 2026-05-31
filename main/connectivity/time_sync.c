#include "time_sync.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "time-sync";

#ifndef CONFIG_TIME_SYNC_ENABLED
#define CONFIG_TIME_SYNC_ENABLED 1
#endif

#ifndef CONFIG_TIME_SYNC_TIMEZONE
#define CONFIG_TIME_SYNC_TIMEZONE "<-03>3"
#endif

#ifndef CONFIG_TIME_SYNC_MIN_VALID_YEAR
#define CONFIG_TIME_SYNC_MIN_VALID_YEAR 2024
#endif

#ifndef CONFIG_TIME_SYNC_RETRY_ATTEMPTS
#define CONFIG_TIME_SYNC_RETRY_ATTEMPTS 3
#endif

#ifndef CONFIG_TIME_SYNC_WAIT_PER_ATTEMPT_MS
#define CONFIG_TIME_SYNC_WAIT_PER_ATTEMPT_MS 5000
#endif

#ifndef CONFIG_TIME_SYNC_RETRY_DELAY_MS
#define CONFIG_TIME_SYNC_RETRY_DELAY_MS 60000
#endif

#ifndef CONFIG_TIME_SYNC_IDLE_DELAY_MS
#define CONFIG_TIME_SYNC_IDLE_DELAY_MS 10000
#endif

#ifndef CONFIG_TIME_SYNC_SERVER_0
#define CONFIG_TIME_SYNC_SERVER_0 "pool.ntp.org"
#endif

#ifndef CONFIG_TIME_SYNC_SERVER_1
#define CONFIG_TIME_SYNC_SERVER_1 "time.google.com"
#endif

#ifndef CONFIG_TIME_SYNC_SERVER_2
#define CONFIG_TIME_SYNC_SERVER_2 "time.cloudflare.com"
#endif

static const char *s_ntp_servers[] = {
    CONFIG_TIME_SYNC_SERVER_0,
    CONFIG_TIME_SYNC_SERVER_1,
    CONFIG_TIME_SYNC_SERVER_2,
};

static bool s_initialized;
static bool s_network_connected;
static bool s_sntp_started;
static bool s_timezone_applied;
static uint32_t s_generation;
static size_t s_next_server_index;
static TaskHandle_t s_task_handle;

static bool time_sync_apply_timezone_once(void)
{
    if (s_timezone_applied) {
        return true;
    }

    if (setenv("TZ", CONFIG_TIME_SYNC_TIMEZONE, 1) != 0) {
        return false;
    }

    tzset();
    s_timezone_applied = true;
    return true;
}

static bool time_sync_is_valid_now(void)
{
    time_t now = 0;
    struct tm now_tm = {0};

    time(&now);
    if (now <= 0) {
        return false;
    }

    gmtime_r(&now, &now_tm);
    return (now_tm.tm_year + 1900) >= CONFIG_TIME_SYNC_MIN_VALID_YEAR;
}

static void time_sync_task(void *arg)
{
    (void)arg;

    while (true) {
        bool should_sync = s_network_connected;
        uint32_t generation = s_generation;

        if (!should_sync) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_TIME_SYNC_IDLE_DELAY_MS));
            continue;
        }

        if (time_sync_is_valid_now()) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_TIME_SYNC_IDLE_DELAY_MS));
            continue;
        }

        bool synced = false;

        for (int attempt = 0; attempt < CONFIG_TIME_SYNC_RETRY_ATTEMPTS; ++attempt) {
            if (!s_network_connected || generation != s_generation) {
                break;
            }

            size_t server_index = s_next_server_index;
            const char *server = s_ntp_servers[server_index];
            s_next_server_index = (s_next_server_index + 1) % (sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0]));

            if (s_sntp_started) {
                esp_sntp_stop();
                s_sntp_started = false;
            }

            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, server);
            esp_sntp_init();
            s_sntp_started = true;

            ESP_LOGI(TAG,
                     "Tentando sincronizacao NTP (%d/%d) com servidor %s",
                     attempt + 1,
                     CONFIG_TIME_SYNC_RETRY_ATTEMPTS,
                     server);

            int waited_ms = 0;
            while (waited_ms < CONFIG_TIME_SYNC_WAIT_PER_ATTEMPT_MS) {
                if (!s_network_connected || generation != s_generation) {
                    break;
                }

                if (time_sync_is_valid_now()) {
                    synced = true;
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(500));
                waited_ms += 500;
            }

            if (synced) {
                ESP_LOGI(TAG, "Horario sincronizado com sucesso via NTP");
                break;
            }

            ESP_LOGW(TAG, "Falha/timeout ao sincronizar com servidor %s", server);
        }

        if (!synced && s_network_connected && generation == s_generation) {
            ESP_LOGW(TAG,
                     "Sem sincronizacao NTP apos %d tentativas; novo ciclo em %d ms",
                     CONFIG_TIME_SYNC_RETRY_ATTEMPTS,
                     CONFIG_TIME_SYNC_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_TIME_SYNC_RETRY_DELAY_MS));
        }
    }
}

esp_err_t time_sync_init(void)
{
#if !CONFIG_TIME_SYNC_ENABLED
    return ESP_OK;
#else
    if (s_initialized) {
        return ESP_OK;
    }

    s_initialized = true;
    s_network_connected = false;
    s_sntp_started = false;
    s_timezone_applied = false;
    s_generation = 0;
    s_next_server_index = 0;

    if (!time_sync_apply_timezone_once()) {
        s_initialized = false;
        return ESP_FAIL;
    }

    BaseType_t ok = xTaskCreate(time_sync_task,
                                "time_sync",
                                4096,
                                NULL,
                                tskIDLE_PRIORITY + 1,
                                &s_task_handle);
    if (ok != pdPASS) {
        s_initialized = false;
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Modulo de sincronizacao NTP inicializado | timezone=%s",
             CONFIG_TIME_SYNC_TIMEZONE);
    return ESP_OK;
#endif
}

void time_sync_on_network_connected(void)
{
    if (!s_initialized || !CONFIG_TIME_SYNC_ENABLED) {
        return;
    }

    s_network_connected = true;
    s_generation++;
}

void time_sync_on_network_disconnected(void)
{
    if (!s_initialized || !CONFIG_TIME_SYNC_ENABLED) {
        return;
    }

    s_network_connected = false;
    s_generation++;

    if (s_sntp_started) {
        esp_sntp_stop();
        s_sntp_started = false;
    }
}

bool time_sync_is_synchronized(void)
{
    if (!CONFIG_TIME_SYNC_ENABLED) {
        return false;
    }

    return time_sync_is_valid_now();
}

const char *time_sync_timezone(void)
{
    return CONFIG_TIME_SYNC_TIMEZONE;
}

bool time_sync_format_local_now(char *buffer, size_t buffer_len)
{
    time_t now = 0;
    struct tm now_tm = {0};
    size_t written = 0;

    if (!buffer || buffer_len == 0 || !time_sync_is_valid_now()) {
        return false;
    }

    if (!time_sync_apply_timezone_once()) {
        return false;
    }

    time(&now);
    localtime_r(&now, &now_tm);
    written = strftime(buffer, buffer_len, "%Y-%m-%dT%H:%M:%S%z", &now_tm);
    return written > 0;
}

bool time_sync_format_utc_now(char *buffer, size_t buffer_len)
{
    time_t now = 0;
    struct tm now_tm = {0};
    size_t written = 0;

    if (!buffer || buffer_len == 0 || !time_sync_is_valid_now()) {
        return false;
    }

    time(&now);
    gmtime_r(&now, &now_tm);
    written = strftime(buffer, buffer_len, "%Y-%m-%dT%H:%M:%SZ", &now_tm);
    return written > 0;
}
