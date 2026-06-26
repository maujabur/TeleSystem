#include "tele_portal_core.h"

#include "esp_log.h"

#define TELE_PORTAL_CORE_MAX_ROUTE_REGISTRARS 8
#define TELE_PORTAL_CORE_DEFAULT_MAX_URI_HANDLERS 48
#define TELE_PORTAL_CORE_DEFAULT_MAX_OPEN_SOCKETS 4
#define TELE_PORTAL_CORE_DEFAULT_SOCKET_TIMEOUT_S 5

static const char *TAG = "tele-portal-core";
static httpd_handle_t s_server;
static tele_portal_core_config_t s_config;
static tele_portal_core_routes_register_fn s_route_registrars[TELE_PORTAL_CORE_MAX_ROUTE_REGISTRARS];
static size_t s_route_registrar_count;

static size_t bounded_or_default(size_t value, size_t default_value)
{
    return value > 0 ? value : default_value;
}

static uint32_t bounded_timeout_or_default(uint32_t value)
{
    return value > 0 ? value : TELE_PORTAL_CORE_DEFAULT_SOCKET_TIMEOUT_S;
}

esp_err_t tele_portal_core_init(const tele_portal_core_config_t *config)
{
    if (config) {
        s_config = *config;
    } else {
        s_config = (tele_portal_core_config_t){0};
    }

    return ESP_OK;
}

esp_err_t tele_portal_core_register_routes(tele_portal_core_routes_register_fn register_fn)
{
    if (!register_fn) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_route_registrar_count; ++i) {
        if (s_route_registrars[i] == register_fn) {
            return ESP_OK;
        }
    }

    if (s_route_registrar_count >= TELE_PORTAL_CORE_MAX_ROUTE_REGISTRARS) {
        return ESP_ERR_NO_MEM;
    }

    s_route_registrars[s_route_registrar_count++] = register_fn;

    if (s_server) {
        return register_fn(s_server);
    }

    return ESP_OK;
}

esp_err_t tele_portal_core_start(bool captive_mode)
{
    (void)captive_mode;

    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();

    http_config.max_uri_handlers = (int)bounded_or_default(
        s_config.max_uri_handlers,
        TELE_PORTAL_CORE_DEFAULT_MAX_URI_HANDLERS
    );

    http_config.max_open_sockets = (int)bounded_or_default(
        s_config.max_open_sockets,
        TELE_PORTAL_CORE_DEFAULT_MAX_OPEN_SOCKETS
    );

    http_config.lru_purge_enable = true;

    http_config.recv_wait_timeout =
        (uint16_t)bounded_timeout_or_default(s_config.socket_timeout_s);

    http_config.send_wait_timeout =
        (uint16_t)bounded_timeout_or_default(s_config.socket_timeout_s);

    http_config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG,
             "Servidor HTTP: max_open_sockets=%d max_uri_handlers=%d "
             "lru_purge=%s timeout=%ds wildcard=sim",
             http_config.max_open_sockets,
             http_config.max_uri_handlers,
             http_config.lru_purge_enable ? "sim" : "nao",
             (int)http_config.recv_wait_timeout);

    esp_err_t err = httpd_start(&s_server, &http_config);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar servidor HTTP: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < s_route_registrar_count; ++i) {
        err = s_route_registrars[i](s_server);

        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "Falha ao registrar rotas %u: %s",
                     (unsigned)i,
                     esp_err_to_name(err));

            httpd_stop(s_server);
            s_server = NULL;

            return err;
        }
    }

    return ESP_OK;
}

esp_err_t tele_portal_core_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(s_server);
    if (err == ESP_OK) {
        s_server = NULL;
    }
    return err;
}

bool tele_portal_core_is_running(void)
{
    return s_server != NULL;
}

void tele_portal_core_note_activity(void)
{
    if (s_config.on_activity) {
        s_config.on_activity(s_config.ctx);
    }
}

void tele_portal_core_request_restart(uint32_t delay_ms)
{
    if (s_config.restart) {
        s_config.restart(delay_ms, s_config.ctx);
    }
}
