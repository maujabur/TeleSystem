#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tele_portal_core.h"
#include "tele_portal_logs.h"

static const char *TAG = "consumer-portal";
static uint32_t s_activity_count;

static void portal_activity_cb(void *ctx)
{
    (void)ctx;
    s_activity_count++;
}

static void portal_restart_cb(uint32_t delay_ms, void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "Restart solicitado pelo portal em %" PRIu32 " ms", delay_ms);
}

static esp_err_t api_ping_get_handler(httpd_req_t *req)
{
    char payload[128];
    int64_t uptime_ms = esp_timer_get_time() / 1000;

    tele_portal_core_note_activity();
    httpd_resp_set_type(req, "application/json");
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"ok\":true,\"uptime_ms\":%" PRId64 ",\"activity_count\":%" PRIu32 "}",
                       uptime_ms,
                       s_activity_count);
    if (len < 0 || len >= (int)sizeof(payload)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Payload invalido");
    }

    return httpd_resp_send(req, payload, len);
}

static esp_err_t register_example_routes(httpd_handle_t server)
{
    httpd_uri_t api_ping = {
        .uri = "/api/ping",
        .method = HTTP_GET,
        .handler = api_ping_get_handler,
    };

    return httpd_register_uri_handler(server, &api_ping);
}

void app_main(void)
{
    tele_portal_logs_init();

    const tele_portal_core_config_t portal_config = {
        .max_uri_handlers = 8,
        .max_open_sockets = 2,
        .socket_timeout_s = 5,
        .on_activity = portal_activity_cb,
        .restart = portal_restart_cb,
    };

    ESP_ERROR_CHECK(tele_portal_core_init(&portal_config));
    ESP_ERROR_CHECK(tele_portal_core_register_routes(register_example_routes));
    ESP_ERROR_CHECK(tele_portal_core_register_routes(tele_portal_logs_register_routes));
    ESP_ERROR_CHECK(tele_portal_core_start(false));

    ESP_LOGI(TAG, "Portal HTTP iniciado; rota de exemplo: GET /api/ping");
}
