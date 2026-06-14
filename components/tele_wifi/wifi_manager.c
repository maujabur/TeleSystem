#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"

#include "wifi_config.h"
#include "wifi_manager.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_PROVISIONING_BIT BIT1
#define PROVISIONING_CAPTIVE_URI "http://192.168.42.1/"

#ifdef CONFIG_WIFI_STA_MAX_RETRY
#define WIFI_STA_MAX_RETRY CONFIG_WIFI_STA_MAX_RETRY
#else
#define WIFI_STA_MAX_RETRY 3
#endif

#ifdef CONFIG_WIFI_APSTA_POLICY
#define WIFI_APSTA_POLICY_DEFAULT ((wifi_manager_apsta_policy_t)CONFIG_WIFI_APSTA_POLICY)
#else
#define WIFI_APSTA_POLICY_DEFAULT WIFI_MANAGER_APSTA_AUTO_TIMEOUT
#endif

#ifdef CONFIG_WIFI_APSTA_GRACE_PERIOD_S
#define WIFI_APSTA_GRACE_PERIOD_DEFAULT_S CONFIG_WIFI_APSTA_GRACE_PERIOD_S
#else
#define WIFI_APSTA_GRACE_PERIOD_DEFAULT_S 600
#endif

#define WIFI_APSTA_GRACE_PERIOD_MIN_S 30
#define WIFI_APSTA_GRACE_PERIOD_MAX_S 3600

static const char *TAG = "wifi-manager";

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static wifi_credentials_t s_credentials;
static char s_configured_provisioning_ssid[33];
static wifi_manager_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_sta_started_from_provisioning;
static bool s_keep_apsta_active;
static bool s_sta_should_connect;
static bool s_high_throughput_mode;
static int s_retry_num;
static int s_sta_max_retry = WIFI_STA_MAX_RETRY;
static wifi_manager_apsta_policy_t s_apsta_policy = WIFI_APSTA_POLICY_DEFAULT;
static uint32_t s_apsta_grace_period_s = WIFI_APSTA_GRACE_PERIOD_DEFAULT_S;
static TimerHandle_t s_apsta_drop_timer;
static TimerHandle_t s_sta_connect_timeout_timer;
#define STA_CONNECT_TIMEOUT_SECONDS 10

ESP_EVENT_DEFINE_BASE(WIFI_MANAGER_EVENT);

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data);
static esp_err_t ensure_wifi_stack_ready(void);
static esp_err_t configure_ap_dhcp_services(void);
static esp_err_t apply_wifi_power_save(bool sta_only);
static void apsta_drop_timer_cb(TimerHandle_t timer);
static void sta_connect_timeout_cb(TimerHandle_t timer);
static void set_provisioning_ssid(const char *ssid);
static esp_err_t scan_networks_internal(wifi_manager_network_t *networks,
                                        size_t max_networks,
                                        size_t *network_count,
                                        bool allow_while_connecting);
#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
static esp_err_t select_best_saved_credentials(const char *exclude_ssid,
                                               wifi_credentials_t *selected);
#endif

static const char *wifi_state_name(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_STATE_INIT:
        return "INIT";
    case WIFI_MANAGER_STATE_STA_CONNECTING:
        return "STA_CONNECTING";
    case WIFI_MANAGER_STATE_STA_CONNECTED:
        return "STA_CONNECTED";
    case WIFI_MANAGER_STATE_PROVISIONING_AP:
        return "PROVISIONING_AP";
    default:
        return "UNKNOWN";
    }
}

static bool wifi_transition_allowed(wifi_manager_state_t from, wifi_manager_state_t to)
{
    if (from == to) {
        return true;
    }

    switch (from) {
    case WIFI_MANAGER_STATE_INIT:
        return to == WIFI_MANAGER_STATE_STA_CONNECTING ||
               to == WIFI_MANAGER_STATE_PROVISIONING_AP;
    case WIFI_MANAGER_STATE_STA_CONNECTING:
        return to == WIFI_MANAGER_STATE_STA_CONNECTED ||
               to == WIFI_MANAGER_STATE_PROVISIONING_AP;
    case WIFI_MANAGER_STATE_STA_CONNECTED:
        return to == WIFI_MANAGER_STATE_STA_CONNECTING ||
               to == WIFI_MANAGER_STATE_PROVISIONING_AP;
    case WIFI_MANAGER_STATE_PROVISIONING_AP:
        return to == WIFI_MANAGER_STATE_STA_CONNECTING;
    default:
        return false;
    }
}

static bool apsta_policy_valid(wifi_manager_apsta_policy_t policy)
{
    return policy == WIFI_MANAGER_APSTA_ALWAYS_ON ||
           policy == WIFI_MANAGER_APSTA_AUTO_TIMEOUT ||
           policy == WIFI_MANAGER_APSTA_STA_ONLY;
}

static bool apsta_grace_valid(uint32_t grace_period_s)
{
    return grace_period_s >= WIFI_APSTA_GRACE_PERIOD_MIN_S &&
           grace_period_s <= WIFI_APSTA_GRACE_PERIOD_MAX_S;
}

static void set_status_wifi_ready(bool ready)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.wifi_ready = ready;
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_status_provisioning_active(bool active)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.provisioning_active = active;
    portEXIT_CRITICAL(&s_status_lock);
}

static wifi_manager_state_t get_status_state(void)
{
    wifi_manager_state_t state = WIFI_MANAGER_STATE_INIT;

    portENTER_CRITICAL(&s_status_lock);
    state = s_status.state;
    portEXIT_CRITICAL(&s_status_lock);
    return state;
}

static bool get_status_provisioning_active(void)
{
    bool active = false;

    portENTER_CRITICAL(&s_status_lock);
    active = s_status.provisioning_active;
    portEXIT_CRITICAL(&s_status_lock);
    return active;
}

static void status_reset(void)
{
    portENTER_CRITICAL(&s_status_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = WIFI_MANAGER_STATE_INIT;
    s_status.sta_max_retry = s_sta_max_retry;
    s_status.apsta_policy = s_apsta_policy;
    s_status.apsta_grace_period_s = s_apsta_grace_period_s;
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_status_sta_max_retry(int retry)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.sta_max_retry = retry;
    portEXIT_CRITICAL(&s_status_lock);
}

static int get_sta_max_retry(void)
{
    int retry = WIFI_STA_MAX_RETRY;

    portENTER_CRITICAL(&s_status_lock);
    retry = s_sta_max_retry;
    portEXIT_CRITICAL(&s_status_lock);
    return retry;
}

static void increment_reconnect_attempts(void)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.sta_reconnect_attempts++;
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_status_apsta_policy(wifi_manager_apsta_policy_t policy)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.apsta_policy = policy;
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_status_apsta_grace(uint32_t grace_period_s)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.apsta_grace_period_s = grace_period_s;
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_status_apsta_auto_drop_pending(bool pending)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.apsta_auto_drop_pending = pending;
    portEXIT_CRITICAL(&s_status_lock);
}

static esp_err_t stop_apsta_auto_drop_timer(void)
{
    if (!s_apsta_drop_timer) {
        return ESP_OK;
    }

    if (xTimerIsTimerActive(s_apsta_drop_timer) != pdFALSE) {
        if (xTimerStop(s_apsta_drop_timer, 0) != pdPASS) {
            return ESP_FAIL;
        }
    }

    set_status_apsta_auto_drop_pending(false);
    return ESP_OK;
}

static esp_err_t start_apsta_auto_drop_timer(uint32_t grace_period_s)
{
    TickType_t ticks = pdMS_TO_TICKS(grace_period_s * 1000U);

    if (ticks == 0) {
        ticks = 1;
    }

    if (!s_apsta_drop_timer) {
        s_apsta_drop_timer = xTimerCreate("apsta_drop",
                                          ticks,
                                          pdFALSE,
                                          NULL,
                                          apsta_drop_timer_cb);
        if (!s_apsta_drop_timer) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTimerIsTimerActive(s_apsta_drop_timer) != pdFALSE) {
        if (xTimerStop(s_apsta_drop_timer, 0) != pdPASS) {
            return ESP_FAIL;
        }
    }

    if (xTimerChangePeriod(s_apsta_drop_timer, ticks, 0) != pdPASS) {
        return ESP_FAIL;
    }

    if (xTimerStart(s_apsta_drop_timer, 0) != pdPASS) {
        return ESP_FAIL;
    }

    set_status_apsta_auto_drop_pending(true);
    return ESP_OK;
}

static esp_err_t apply_apsta_policy_after_sta_connected(void)
{
    if (!s_sta_started_from_provisioning) {
        set_status_apsta_auto_drop_pending(false);
        return ESP_OK;
    }

    if (s_apsta_policy == WIFI_MANAGER_APSTA_ALWAYS_ON) {
        set_status_provisioning_active(true);
        set_status_apsta_auto_drop_pending(false);
        ESP_LOGI(TAG, "Politica APSTA: always_on (APSTA mantido)");
        return ESP_OK;
    }

    if (s_apsta_policy == WIFI_MANAGER_APSTA_STA_ONLY) {
        ESP_ERROR_CHECK(stop_apsta_auto_drop_timer());
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(apply_wifi_power_save(true));
        set_status_provisioning_active(false);
        set_provisioning_ssid(NULL);
        xEventGroupClearBits(s_wifi_event_group, WIFI_PROVISIONING_BIT);
        set_status_apsta_auto_drop_pending(false);
        ESP_LOGI(TAG, "Politica APSTA: sta_only (AP desligado apos conexao)");
        return ESP_OK;
    }

    set_status_provisioning_active(true);
    ESP_ERROR_CHECK(start_apsta_auto_drop_timer(s_apsta_grace_period_s));
    ESP_LOGI(TAG,
             "Politica APSTA: auto_timeout (%lu s)",
             (unsigned long)s_apsta_grace_period_s);
    return ESP_OK;
}

static void apsta_drop_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    if (get_status_state() != WIFI_MANAGER_STATE_STA_CONNECTED ||
        !get_status_provisioning_active()) {
        set_status_apsta_auto_drop_pending(false);
        return;
    }

    if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
        if (apply_wifi_power_save(true) != ESP_OK) {
            ESP_LOGW(TAG, "Falha ao ajustar power save apos desativar APSTA");
        }
        set_status_provisioning_active(false);
        set_provisioning_ssid(NULL);
        xEventGroupClearBits(s_wifi_event_group, WIFI_PROVISIONING_BIT);
        ESP_LOGI(TAG, "Janela APSTA encerrada; operando em STA-only");
    } else {
        ESP_LOGW(TAG, "Falha ao trocar APSTA para STA-only apos timeout");
    }

    set_status_apsta_auto_drop_pending(false);
}

static esp_err_t stop_sta_connect_timeout_timer(void)
{
    if (!s_sta_connect_timeout_timer) {
        return ESP_OK;
    }

    if (xTimerIsTimerActive(s_sta_connect_timeout_timer) != pdFALSE) {
        if (xTimerStop(s_sta_connect_timeout_timer, 0) != pdPASS) {
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static esp_err_t start_sta_connect_timeout_timer(void)
{
    TickType_t ticks = pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_SECONDS * 1000U);

    if (ticks == 0) {
        ticks = 1;
    }

    if (!s_sta_connect_timeout_timer) {
        s_sta_connect_timeout_timer = xTimerCreate("sta_connect_timeout",
                                                    ticks,
                                                    pdFALSE,
                                                    NULL,
                                                    sta_connect_timeout_cb);
        if (!s_sta_connect_timeout_timer) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTimerIsTimerActive(s_sta_connect_timeout_timer) != pdFALSE) {
        if (xTimerStop(s_sta_connect_timeout_timer, 0) != pdPASS) {
            return ESP_FAIL;
        }
    }

    if (xTimerChangePeriod(s_sta_connect_timeout_timer, ticks, 0) != pdPASS) {
        return ESP_FAIL;
    }

    if (xTimerStart(s_sta_connect_timeout_timer, 0) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void sta_connect_timeout_cb(TimerHandle_t timer)
{
    (void)timer;

    if (get_status_state() != WIFI_MANAGER_STATE_STA_CONNECTING) {
        return;
    }

    ESP_LOGW(TAG, "Timeout na conexao STA (timeout=%d s). Forcando desconexao para triggerar fallback.",
             STA_CONNECT_TIMEOUT_SECONDS);
    esp_wifi_disconnect();
}

static void set_status_ip(const esp_ip4_addr_t *ip)
{
    portENTER_CRITICAL(&s_status_lock);
    if (!ip) {
        s_status.ip[0] = '\0';
        portEXIT_CRITICAL(&s_status_lock);
        return;
    }

    snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(ip));
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_status_ssid(const char *ssid)
{
    portENTER_CRITICAL(&s_status_lock);
    memset(s_status.ssid, 0, sizeof(s_status.ssid));
    if (!ssid) {
        portEXIT_CRITICAL(&s_status_lock);
        return;
    }

    snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", ssid);
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_provisioning_ssid(const char *ssid)
{
    portENTER_CRITICAL(&s_status_lock);
    memset(s_status.provisioning_ssid, 0, sizeof(s_status.provisioning_ssid));
    if (!ssid) {
        portEXIT_CRITICAL(&s_status_lock);
        return;
    }

    snprintf(s_status.provisioning_ssid, sizeof(s_status.provisioning_ssid), "%s", ssid);
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_last_error(const char *message)
{
    portENTER_CRITICAL(&s_status_lock);
    memset(s_status.last_error, 0, sizeof(s_status.last_error));
    if (!message) {
        portEXIT_CRITICAL(&s_status_lock);
        return;
    }

    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s", message);
    portEXIT_CRITICAL(&s_status_lock);
}

static esp_err_t apply_wifi_power_save(bool sta_only)
{
    bool modem_sleep_configured = false;
    wifi_ps_type_t ps_type = WIFI_PS_NONE;

#ifdef CONFIG_WIFI_MODEM_SLEEP
    modem_sleep_configured = true;
    if (sta_only && !s_high_throughput_mode) {
        ps_type = WIFI_PS_MIN_MODEM;
    }
#endif

    esp_err_t err = esp_wifi_set_ps(ps_type);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Wi-Fi power save: %s (sta_only=%d, high_throughput=%d, modem_sleep_config=%d)",
                 ps_type == WIFI_PS_MIN_MODEM ? "MIN_MODEM" : "NONE",
                 sta_only,
                 s_high_throughput_mode,
                 modem_sleep_configured);
    }
    return err;
}

static void transition_state(wifi_manager_state_t state, int32_t event_id, const char *origin)
{
    wifi_manager_state_t previous = WIFI_MANAGER_STATE_INIT;

    portENTER_CRITICAL(&s_status_lock);
    previous = s_status.state;

    if (!wifi_transition_allowed(previous, state)) {
        s_status.invalid_transition_count++;
        portEXIT_CRITICAL(&s_status_lock);
        ESP_LOGW(TAG,
                 "Transicao Wi-Fi invalida ignorada: %s -> %s (%s)",
                 wifi_state_name(previous),
                 wifi_state_name(state),
                 origin ? origin : "sem_origem");
        return;
    }

    s_status.state = state;
    portEXIT_CRITICAL(&s_status_lock);
    esp_event_post(WIFI_MANAGER_EVENT, event_id, NULL, 0, portMAX_DELAY);
}

static esp_err_t ensure_scan_ready(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = ensure_wifi_stack_ready();

    if (err != ESP_OK) {
        return err;
    }

    if (!s_wifi_started) {
        return ESP_ERR_WIFI_NOT_STARTED;
    }

    err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        return err;
    }

    if (mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t ensure_tcpip_ready(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    return ESP_OK;
}

static esp_err_t ensure_wifi_stack_ready(void)
{
    esp_err_t err = ensure_tcpip_ready();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) {
            return ESP_FAIL;
        }
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            return ESP_FAIL;
        }
    }

    if (!s_wifi_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            return err;
        }

        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
        s_wifi_initialized = true;
    }

    return ESP_OK;
}

static esp_err_t configure_ap_dhcp_services(void)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_dns_info_t dns_info = {0};
    uint8_t offer_dns = 1;
    const char *captive_uri = PROVISIONING_CAPTIVE_URI;
    esp_err_t err = ESP_OK;

    if (!s_ap_netif) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }

    IP4_ADDR(&ip_info.ip, 192, 168, 42, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 42, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    err = esp_netif_set_ip_info(s_ap_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }

    memset(&dns_info, 0, sizeof(dns_info));
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    IP4_ADDR(&dns_info.ip.u_addr.ip4, 192, 168, 42, 1);

    err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_dhcps_option(s_ap_netif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns,
                                 sizeof(offer_dns));
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_dhcps_option(s_ap_netif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_CAPTIVEPORTAL_URI,
                                 (void *)captive_uri,
                                 strlen(captive_uri) + 1);
    if (err != ESP_OK) {
        return err;
    }

    return esp_netif_dhcps_start(s_ap_netif);
}

static esp_err_t stop_wifi_if_started(void)
{
    if (!s_wifi_started) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED) {
        s_wifi_started = false;
        return ESP_OK;
    }

    return err;
}

static bool load_credentials_from_menuconfig(wifi_credentials_t *cfg)
{
    if (!cfg || CONFIG_ESP_WIFI_SSID[0] == '\0') {
        return false;
    }

    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->ssid, sizeof(cfg->ssid), "%s", CONFIG_ESP_WIFI_SSID);
    snprintf(cfg->password, sizeof(cfg->password), "%s", CONFIG_ESP_WIFI_PASSWORD);
    cfg->provisioned = true;
    return true;
}

static esp_err_t load_active_credentials(wifi_credentials_t *cfg)
{
    esp_err_t err = ESP_OK;

#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
    err = select_best_saved_credentials(NULL, cfg);
    if (err == ESP_OK) {
        return ESP_OK;
    }
#endif

    err = wifi_config_load(cfg);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (load_credentials_from_menuconfig(cfg)) {
        ESP_LOGW(TAG, "Usando credenciais Wi-Fi do menuconfig como fallback");
        return ESP_OK;
    }

    return err;
}

#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
static esp_err_t select_best_saved_credentials(const char *exclude_ssid,
                                               wifi_credentials_t *selected)
{
    wifi_saved_network_t *saved = NULL;
    wifi_manager_network_t *visible = NULL;
    size_t saved_count = 0;
    size_t visible_count = 0;
    int best_idx = -1;
    int best_rssi = -127;
    bool scan_ok = false;
    esp_err_t err = ESP_OK;
    size_t i = 0;

    if (!selected) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(selected, 0, sizeof(*selected));

    saved = (wifi_saved_network_t *)calloc(WIFI_CONFIG_MAX_SAVED_NETWORKS, sizeof(*saved));
    visible = (wifi_manager_network_t *)calloc(WIFI_MANAGER_MAX_SCAN_RESULTS, sizeof(*visible));
    if (!saved || !visible) {
        free(saved);
        free(visible);
        return ESP_ERR_NO_MEM;
    }

    err = wifi_config_load_saved_networks(saved,
                                          WIFI_CONFIG_MAX_SAVED_NETWORKS,
                                          &saved_count);
    if (err != ESP_OK) {
        free(saved);
        free(visible);
        return err;
    }
    if (saved_count == 0) {
        free(saved);
        free(visible);
        return ESP_ERR_NOT_FOUND;
    }

    err = scan_networks_internal(visible,
                                 WIFI_MANAGER_MAX_SCAN_RESULTS,
                                 &visible_count,
                                 false);
    scan_ok = err == ESP_OK;

    if (scan_ok) {
        for (i = 0; i < saved_count; ++i) {
            size_t j = 0;
            bool is_visible = false;
            int candidate_rssi = -127;

            if (exclude_ssid && strcmp(saved[i].ssid, exclude_ssid) == 0) {
                continue;
            }

            for (j = 0; j < visible_count; ++j) {
                if (strcmp(saved[i].ssid, visible[j].ssid) == 0) {
                    is_visible = true;
                    candidate_rssi = visible[j].rssi;
                    break;
                }
            }

            if (!is_visible) {
                continue;
            }

            if (best_idx < 0 ||
                saved[i].priority > saved[best_idx].priority ||
                (saved[i].priority == saved[best_idx].priority && candidate_rssi > best_rssi)) {
                best_idx = (int)i;
                best_rssi = candidate_rssi;
            }
        }
    }

    if (best_idx < 0) {
        for (i = 0; i < saved_count; ++i) {
            if (exclude_ssid && strcmp(saved[i].ssid, exclude_ssid) == 0) {
                continue;
            }
            if (best_idx < 0 || saved[i].priority > saved[best_idx].priority) {
                best_idx = (int)i;
            }
        }
    }

    if (best_idx < 0) {
        free(saved);
        free(visible);
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(selected->ssid, sizeof(selected->ssid), "%s", saved[best_idx].ssid);
    snprintf(selected->password, sizeof(selected->password), "%s", saved[best_idx].password);
    selected->provisioned = selected->ssid[0] != '\0';

    free(saved);
    free(visible);
    return selected->provisioned ? ESP_OK : ESP_ERR_NOT_FOUND;
}
#endif

static void build_ap_ssid(char *ssid, size_t ssid_size)
{
    uint8_t mac[6] = {0};

    if (!ssid || ssid_size == 0) {
        return;
    }

    memset(ssid, 0, ssid_size);

    if (s_configured_provisioning_ssid[0] != '\0') {
        snprintf(ssid, ssid_size, "%s", s_configured_provisioning_ssid);
        return;
    }

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, ssid_size, "ESP32-Device-%02X%02X", mac[4], mac[5]);
}

static esp_err_t configure_and_start_ap(void)
{
    wifi_config_t ap_config = {0};
    esp_err_t err = ensure_wifi_stack_ready();

    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(stop_sta_connect_timeout_timer());
    err = stop_wifi_if_started();
    if (err != ESP_OK) {
        return err;
    }

    build_ap_ssid((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(apply_wifi_power_save(false));
    ESP_ERROR_CHECK(configure_ap_dhcp_services());
    s_wifi_started = true;
    s_sta_should_connect = false;
    ESP_ERROR_CHECK(stop_apsta_auto_drop_timer());

    set_status_ssid((char *)ap_config.ap.ssid);
    set_provisioning_ssid((char *)ap_config.ap.ssid);
    set_status_ip(NULL);
    set_status_wifi_ready(false);
    set_status_provisioning_active(true);
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    xEventGroupSetBits(s_wifi_event_group, WIFI_PROVISIONING_BIT);
    transition_state(WIFI_MANAGER_STATE_PROVISIONING_AP,
                     WIFI_MANAGER_EVENT_PROVISIONING_STARTED,
                     "configure_and_start_ap");

    ESP_LOGW(TAG, "AP de provisionamento ativo");
    ESP_LOGI(TAG, "Modo Wi-Fi ativo: AP");
    return ESP_OK;
}

static esp_err_t configure_and_start_sta(const wifi_credentials_t *credentials)
{
    wifi_config_t wifi_config = {0};
    esp_err_t err = ESP_OK;
    bool keep_provisioning_ap_active = get_status_provisioning_active();

    if (!credentials || credentials->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = ensure_wifi_stack_ready();
    if (err != ESP_OK) {
        return err;
    }

    strncpy((char *)wifi_config.sta.ssid, credentials->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, credentials->password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = credentials->password[0] == '\0' ?
        WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    if (!keep_provisioning_ap_active) {
        err = stop_wifi_if_started();
        if (err != ESP_OK) {
            return err;
        }
    }

    s_sta_should_connect = true;
    ESP_ERROR_CHECK(stop_apsta_auto_drop_timer());
    ESP_ERROR_CHECK(esp_wifi_set_mode(keep_provisioning_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    ESP_LOGI(TAG, "Modo Wi-Fi selecionado: %s",
             keep_provisioning_ap_active ? "APSTA" : "STA");
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    if (!keep_provisioning_ap_active || !s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
    ESP_ERROR_CHECK(apply_wifi_power_save(!keep_provisioning_ap_active));
    s_sta_started_from_provisioning = keep_provisioning_ap_active;

    set_status_ssid(credentials->ssid);
    set_status_ip(NULL);
    set_last_error(NULL);
    set_status_wifi_ready(false);
    set_status_provisioning_active(keep_provisioning_ap_active);
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    if (!keep_provisioning_ap_active) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_PROVISIONING_BIT);
    }
    transition_state(WIFI_MANAGER_STATE_STA_CONNECTING,
                     WIFI_MANAGER_EVENT_STA_CONNECTING,
                     "configure_and_start_sta");

    ESP_ERROR_CHECK(start_sta_connect_timeout_timer());

    ESP_LOGI(TAG,
             "Conectando no Wi-Fi (%s)",
             keep_provisioning_ap_active ? "mantendo AP de provisionamento ativo" : "AP de provisionamento fechado");
    return ESP_OK;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_sta_should_connect) {
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_manager_state_t current_state = get_status_state();
        wifi_manager_state_t next_state = current_state == WIFI_MANAGER_STATE_PROVISIONING_AP ?
            WIFI_MANAGER_STATE_PROVISIONING_AP : WIFI_MANAGER_STATE_STA_CONNECTING;

        ESP_ERROR_CHECK(stop_sta_connect_timeout_timer());
        set_status_wifi_ready(false);
        set_status_ip(NULL);
        set_status_apsta_auto_drop_pending(false);
        transition_state(next_state,
                         WIFI_MANAGER_EVENT_STA_DISCONNECTED,
                         "wifi_event_sta_disconnected");

        if (current_state == WIFI_MANAGER_STATE_PROVISIONING_AP) {
            return;
        }

        if (s_retry_num < get_sta_max_retry()) {
            s_retry_num++;
            increment_reconnect_attempts();
            ESP_LOGW(TAG,
                     "Wi-Fi desconectado, tentando novamente (%d/%d)",
                     s_retry_num,
                     get_sta_max_retry());
            esp_wifi_connect();
        } else {
            set_last_error("Nao foi possivel conectar na rede informada.");
            s_sta_should_connect = false;
#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
            {
                wifi_credentials_t fallback = {0};
                esp_err_t fallback_err = select_best_saved_credentials(s_credentials.ssid, &fallback);
                if (fallback_err == ESP_OK && fallback.provisioned) {
                    ESP_LOGW(TAG,
                             "Falha na rede atual (%s). Tentando fallback para rede salva (%s)",
                             s_credentials.ssid,
                             fallback.ssid);
                    s_credentials = fallback;
                    set_last_error("Rede atual falhou. Tentando rede salva alternativa.");
                    if (configure_and_start_sta(&s_credentials) == ESP_OK) {
                        return;
                    }
                    ESP_LOGE(TAG, "Falha ao iniciar fallback para rede alternativa");
                }
            }
#endif
            ESP_LOGE(TAG, "Falha ao conectar no Wi-Fi salvo, entrando em AP de provisionamento");
            if (configure_and_start_ap() != ESP_OK) {
                ESP_LOGE(TAG, "Nao foi possivel iniciar AP de provisionamento");
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_ERROR_CHECK(stop_sta_connect_timeout_timer());
        s_retry_num = 0;
        set_status_wifi_ready(true);
        set_status_ip(&event->ip_info.ip);
        set_last_error(NULL);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        transition_state(WIFI_MANAGER_STATE_STA_CONNECTED,
                 WIFI_MANAGER_EVENT_STA_CONNECTED,
                 "ip_event_sta_got_ip");

        ESP_ERROR_CHECK(apply_apsta_policy_after_sta_connected());
        ESP_LOGI(TAG,
                 "Modo Wi-Fi ativo: %s",
                 get_status_provisioning_active() ? "APSTA" : "STA");
        s_sta_started_from_provisioning = false;

        ESP_LOGI(TAG, "IP obtido: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_manager_start(void)
{
    return wifi_manager_start_with_config(NULL);
}

esp_err_t wifi_manager_start_with_config(const wifi_manager_config_t *config)
{
    esp_err_t err = ESP_OK;

    if (s_started) {
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        return ESP_ERR_NO_MEM;
    }

    status_reset();
    if (config && config->sta_max_retry > 0) {
        s_sta_max_retry = config->sta_max_retry;
    } else {
        s_sta_max_retry = WIFI_STA_MAX_RETRY;
    }

    if (config && apsta_policy_valid(config->apsta_policy)) {
        s_apsta_policy = config->apsta_policy;
    } else {
        s_apsta_policy = WIFI_APSTA_POLICY_DEFAULT;
        if (!apsta_policy_valid(s_apsta_policy)) {
            s_apsta_policy = WIFI_MANAGER_APSTA_AUTO_TIMEOUT;
        }
    }

    if (config && apsta_grace_valid(config->apsta_grace_period_s)) {
        s_apsta_grace_period_s = config->apsta_grace_period_s;
    } else {
        s_apsta_grace_period_s = WIFI_APSTA_GRACE_PERIOD_DEFAULT_S;
        if (!apsta_grace_valid(s_apsta_grace_period_s)) {
            s_apsta_grace_period_s = 600;
        }
    }

    set_status_sta_max_retry(s_sta_max_retry);
    set_status_apsta_policy(s_apsta_policy);
    set_status_apsta_grace(s_apsta_grace_period_s);

    memset(s_configured_provisioning_ssid, 0, sizeof(s_configured_provisioning_ssid));
    if (config && config->provisioning_ssid) {
        snprintf(s_configured_provisioning_ssid, sizeof(s_configured_provisioning_ssid), "%s", config->provisioning_ssid);
    }
    s_keep_apsta_active = s_apsta_policy == WIFI_MANAGER_APSTA_ALWAYS_ON;
    s_started = true;

    if (config && config->force_provisioning) {
        ESP_LOGW(TAG, "Provisionamento Wi-Fi forcado pela configuracao de boot");
        return configure_and_start_ap();
    }

    err = load_active_credentials(&s_credentials);
    if (err == ESP_OK && s_credentials.provisioned) {
        ESP_LOGI(TAG, "Credenciais Wi-Fi conhecidas encontradas, iniciando STA sem AP");
        return configure_and_start_sta(&s_credentials);
    }

    ESP_LOGW(TAG, "Nenhuma credencial Wi-Fi provisionada, iniciando AP de provisionamento");
    return configure_and_start_ap();
}

esp_err_t wifi_manager_wait_until_ready(TickType_t timeout_ticks)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_PROVISIONING_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           timeout_ticks);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    if (bits & WIFI_PROVISIONING_BIT) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_apply_wifi_credentials(const char *ssid, const char *password)
{
    wifi_credentials_t new_credentials = {0};
    esp_err_t err = ESP_OK;

    if (!ssid || ssid[0] == '\0' || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(ssid) > WIFI_CONFIG_SSID_MAX_LEN || strlen(password) > WIFI_CONFIG_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(new_credentials.ssid, sizeof(new_credentials.ssid), "%s", ssid);
    snprintf(new_credentials.password, sizeof(new_credentials.password), "%s", password);
    new_credentials.provisioned = true;

    err = wifi_config_save(&new_credentials);
    if (err != ESP_OK) {
        return err;
    }

    s_credentials = new_credentials;
    esp_event_post(WIFI_MANAGER_EVENT, WIFI_MANAGER_EVENT_CREDENTIALS_UPDATED, NULL, 0, portMAX_DELAY);
    return configure_and_start_sta(&s_credentials);
}

esp_err_t wifi_manager_set_provisioning_ssid(const char *ssid)
{
    wifi_config_t ap_config = {0};
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_manager_state_t state = WIFI_MANAGER_STATE_INIT;
    size_t ssid_len = 0;
    esp_err_t err = ESP_OK;

    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ssid_len = strlen(ssid);
    if (ssid_len >= sizeof(s_configured_provisioning_ssid) ||
        ssid_len > sizeof(ap_config.ap.ssid)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(s_configured_provisioning_ssid, 0, sizeof(s_configured_provisioning_ssid));
    memcpy(s_configured_provisioning_ssid, ssid, ssid_len);

    if (!s_started || !s_wifi_initialized || !s_wifi_started) {
        return ESP_OK;
    }

    if (!get_status_provisioning_active()) {
        return ESP_OK;
    }

    if (esp_wifi_get_mode(&mode) != ESP_OK ||
        (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA)) {
        return ESP_OK;
    }

    memset(&ap_config, 0, sizeof(ap_config));
    memcpy(ap_config.ap.ssid, s_configured_provisioning_ssid, ssid_len);
    ap_config.ap.ssid_len = ssid_len;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Falha ao atualizar SSID de provisioning em runtime: %s",
                 esp_err_to_name(err));
        return err;
    }

    set_provisioning_ssid((char *)ap_config.ap.ssid);
    state = get_status_state();
    if (state == WIFI_MANAGER_STATE_PROVISIONING_AP) {
        set_status_ssid((char *)ap_config.ap.ssid);
    }

    ESP_LOGI(TAG, "SSID de provisionamento atualizado em runtime: %s", (char *)ap_config.ap.ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_reconnect_sta(void)
{
    esp_err_t err = ESP_OK;

    if (!s_started || !s_wifi_initialized || !s_wifi_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_credentials.ssid[0] == '\0') {
        return ESP_ERR_WIFI_SSID;
    }

    s_retry_num = 0;
    s_sta_should_connect = true;
    set_status_wifi_ready(false);
    set_status_ip(NULL);
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    transition_state(WIFI_MANAGER_STATE_STA_CONNECTING,
                     WIFI_MANAGER_EVENT_STA_CONNECTING,
                     "wifi_manager_reconnect_sta");

    ESP_LOGW(TAG, "Recuperacao Wi-Fi solicitada: reconectando STA");
    err = esp_wifi_disconnect();
    if (err != ESP_OK &&
        err != ESP_ERR_WIFI_NOT_CONNECT &&
        err != ESP_ERR_WIFI_CONN) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(250));
    err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_CONN) {
        return ESP_OK;
    }
    return err;
}

esp_err_t wifi_manager_set_sta_max_retry(int retry)
{
    if (retry <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_sta_max_retry = retry;
    set_status_sta_max_retry(retry);
    ESP_LOGI(TAG, "Retry maximo STA atualizado em runtime para %d", retry);
    return ESP_OK;
}

esp_err_t wifi_manager_set_apsta_policy(wifi_manager_apsta_policy_t policy,
                                        uint32_t grace_period_s)
{
    if (!apsta_policy_valid(policy) || !apsta_grace_valid(grace_period_s)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_apsta_policy = policy;
    s_apsta_grace_period_s = grace_period_s;
    s_keep_apsta_active = policy == WIFI_MANAGER_APSTA_ALWAYS_ON;
    set_status_apsta_policy(policy);
    set_status_apsta_grace(grace_period_s);

    if (get_status_state() == WIFI_MANAGER_STATE_STA_CONNECTED &&
        get_status_provisioning_active()) {
        if (policy == WIFI_MANAGER_APSTA_STA_ONLY) {
            ESP_ERROR_CHECK(stop_apsta_auto_drop_timer());
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(apply_wifi_power_save(true));
            set_status_provisioning_active(false);
            set_provisioning_ssid(NULL);
            xEventGroupClearBits(s_wifi_event_group, WIFI_PROVISIONING_BIT);
            set_status_apsta_auto_drop_pending(false);
        } else if (policy == WIFI_MANAGER_APSTA_AUTO_TIMEOUT) {
            ESP_ERROR_CHECK(start_apsta_auto_drop_timer(grace_period_s));
        } else {
            ESP_ERROR_CHECK(stop_apsta_auto_drop_timer());
        }
    }

    ESP_LOGI(TAG,
             "Politica APSTA atualizada em runtime: policy=%d grace=%lu",
             (int)policy,
             (unsigned long)grace_period_s);
    return ESP_OK;
}

esp_err_t wifi_manager_note_portal_activity(void)
{
    if (s_apsta_policy != WIFI_MANAGER_APSTA_AUTO_TIMEOUT) {
        return ESP_OK;
    }

    if (get_status_state() != WIFI_MANAGER_STATE_STA_CONNECTED ||
        !get_status_provisioning_active()) {
        return ESP_OK;
    }

    esp_err_t err = start_apsta_auto_drop_timer(s_apsta_grace_period_s);
    if (err == ESP_OK) {
        ESP_LOGD(TAG,
                 "Atividade de portal: janela APSTA renovada (%lu s)",
                 (unsigned long)s_apsta_grace_period_s);
    }
    return err;
}

esp_err_t wifi_manager_set_high_throughput_mode(bool enabled)
{
    bool sta_only = get_status_state() == WIFI_MANAGER_STATE_STA_CONNECTED &&
                    !get_status_provisioning_active();

    if (s_high_throughput_mode == enabled) {
        return ESP_OK;
    }

    s_high_throughput_mode = enabled;
    ESP_LOGI(TAG, "Modo high throughput Wi-Fi: %s", enabled ? "ON" : "OFF");

    if (!s_wifi_initialized || !s_wifi_started) {
        return ESP_OK;
    }

    return apply_wifi_power_save(sta_only);
}

esp_err_t wifi_manager_get_status(wifi_manager_status_t *status)
{
    wifi_ap_record_t ap_info = {0};
    bool sta_connected = false;
    int rssi = 0;

    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    sta_connected = get_status_state() == WIFI_MANAGER_STATE_STA_CONNECTED;
    if (sta_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    portENTER_CRITICAL(&s_status_lock);
    s_status.rssi = rssi;
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

esp_err_t wifi_manager_scan_networks(wifi_manager_network_t *networks,
                                     size_t max_networks,
                                     size_t *network_count)
{
    return scan_networks_internal(networks, max_networks, network_count, false);
}

esp_err_t wifi_manager_list_saved_networks(wifi_manager_saved_network_t *networks,
                                           size_t max_networks,
                                           size_t *network_count)
{
    wifi_saved_network_t saved[WIFI_CONFIG_MAX_SAVED_NETWORKS] = {0};
    size_t saved_count = 0;
    size_t i = 0;
    esp_err_t err = ESP_OK;

    if (!networks || !network_count || max_networks == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *network_count = 0;
    err = wifi_config_load_saved_networks(saved,
                                          WIFI_CONFIG_MAX_SAVED_NETWORKS,
                                          &saved_count);
    if (err != ESP_OK) {
        return err;
    }

    if (saved_count > max_networks) {
        saved_count = max_networks;
    }

    for (i = 0; i < saved_count; ++i) {
        snprintf(networks[i].ssid, sizeof(networks[i].ssid), "%s", saved[i].ssid);
        networks[i].priority = saved[i].priority;
    }

    *network_count = saved_count;
    return saved_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_manager_set_saved_network_priority(const char *ssid, int32_t priority)
{
    wifi_saved_network_t saved[WIFI_CONFIG_MAX_SAVED_NETWORKS] = {0};
    size_t saved_count = 0;
    size_t i = 0;
    esp_err_t err = ESP_OK;

    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = wifi_config_load_saved_networks(saved,
                                          WIFI_CONFIG_MAX_SAVED_NETWORKS,
                                          &saved_count);
    if (err != ESP_OK) {
        return err;
    }

    for (i = 0; i < saved_count; ++i) {
        if (strcmp(saved[i].ssid, ssid) == 0) {
            saved[i].priority = priority;
            return wifi_config_upsert_saved_network(&saved[i]);
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_manager_remove_saved_network(const char *ssid)
{
    return wifi_config_remove_saved_network(ssid);
}

static esp_err_t scan_networks_internal(wifi_manager_network_t *networks,
                                        size_t max_networks,
                                        size_t *network_count,
                                        bool allow_while_connecting)
{
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    wifi_ap_record_t *ap_records = NULL;
    uint16_t ap_count = WIFI_MANAGER_MAX_SCAN_RESULTS;
    esp_err_t err = ESP_OK;
    size_t i = 0;

    if (!networks || !network_count || max_networks == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *network_count = 0;

    ap_records = (wifi_ap_record_t *)calloc(WIFI_MANAGER_MAX_SCAN_RESULTS, sizeof(*ap_records));
    if (!ap_records) {
        return ESP_ERR_NO_MEM;
    }

    if (!allow_while_connecting && get_status_state() == WIFI_MANAGER_STATE_STA_CONNECTING) {
        free(ap_records);
        return ESP_ERR_INVALID_STATE;
    }

    err = ensure_scan_ready();
    if (err != ESP_OK) {
        free(ap_records);
        return err;
    }

    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        free(ap_records);
        return err;
    }

    if (ap_count > max_networks) {
        ap_count = (uint16_t)max_networks;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        free(ap_records);
        return err;
    }

    for (i = 0; i < ap_count; ++i) {
        size_t j = 0;
        bool merged = false;

        if (ap_records[i].ssid[0] == '\0') {
            continue;
        }

        for (j = 0; j < *network_count; ++j) {
            if (strcmp(networks[j].ssid, (const char *)ap_records[i].ssid) == 0) {
                if (ap_records[i].rssi > networks[j].rssi) {
                    networks[j].rssi = ap_records[i].rssi;
                    networks[j].auth_required = ap_records[i].authmode != WIFI_AUTH_OPEN;
                }
                merged = true;
                break;
            }
        }

        if (merged || *network_count >= max_networks) {
            continue;
        }

        snprintf(networks[*network_count].ssid,
                 sizeof(networks[*network_count].ssid),
                 "%s",
                 (const char *)ap_records[i].ssid);
        networks[*network_count].rssi = ap_records[i].rssi;
        networks[*network_count].auth_required = ap_records[i].authmode != WIFI_AUTH_OPEN;
        (*network_count)++;
    }

    free(ap_records);
    return ESP_OK;
}
