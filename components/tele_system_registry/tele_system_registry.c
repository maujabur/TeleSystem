#include "tele_system_registry.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "firmware_ota.h"
#include "tele_config.h"
#include "tele_status.h"

#include "device_config.h"
#include "time_sync.h"
#include "vbat_monitor.h"
#include "wifi_manager.h"

#ifndef CONFIG_MQTT_HEARTBEAT_INTERVAL_S
#define CONFIG_MQTT_HEARTBEAT_INTERVAL_S 60
#endif

#define MQTT_CONFIG_HEARTBEAT_INTERVAL_NVS_KEY "m_hbint"
#define MQTT_HEARTBEAT_INTERVAL_MIN_S 15
#define MQTT_HEARTBEAT_INTERVAL_MAX_S 3600
#define TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS (TELE_CHANNEL_FLAG_MQTT | TELE_CHANNEL_FLAG_WEB)

#ifndef CONFIG_POWER_GOOD_GPIO_ENABLED
#define CONFIG_POWER_GOOD_GPIO_ENABLED 0
#endif

#ifndef CONFIG_POWER_GOOD_GPIO
#define CONFIG_POWER_GOOD_GPIO 6
#endif

#ifndef CONFIG_POWER_GOOD_ACTIVE_LEVEL
#define CONFIG_POWER_GOOD_ACTIVE_LEVEL 1
#endif

static const char *TAG = "tele-system-registry";

static bool s_status_fields_registered;
static bool s_config_fields_registered;
static tele_system_registry_config_t s_config;

static tele_config_field_t s_mqtt_config_fields[] = {
    {
        .id = TELE_SYSTEM_REGISTRY_CONFIG_ID_HEARTBEAT_INTERVAL,
        .nvs_key = MQTT_CONFIG_HEARTBEAT_INTERVAL_NVS_KEY,
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = CONFIG_MQTT_HEARTBEAT_INTERVAL_S,
        .min.u32 = MQTT_HEARTBEAT_INTERVAL_MIN_S,
        .max.u32 = MQTT_HEARTBEAT_INTERVAL_MAX_S,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = 0,
    },
};

static const char *wifi_state_name(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_STATE_INIT:
        return "init";
    case WIFI_MANAGER_STATE_STA_CONNECTING:
        return "sta_connecting";
    case WIFI_MANAGER_STATE_STA_CONNECTED:
        return "sta_connected";
    case WIFI_MANAGER_STATE_PROVISIONING_AP:
        return "provisioning_ap";
    default:
        return "unknown";
    }
}

static const char *status_read_wifi_state(void *ctx)
{
    wifi_manager_status_t wifi_status = {0};
    (void)ctx;

    if (wifi_manager_get_status(&wifi_status) != ESP_OK) {
        return "unknown";
    }
    return wifi_state_name(wifi_status.state);
}

static bool status_read_wifi_ready(void *ctx)
{
    wifi_manager_status_t wifi_status = {0};
    (void)ctx;

    return wifi_manager_get_status(&wifi_status) == ESP_OK && wifi_status.wifi_ready;
}

static const char *status_read_ssid(void *ctx)
{
    static char ssid[sizeof(((wifi_manager_status_t *)0)->ssid)] = {0};
    wifi_manager_status_t wifi_status = {0};
    (void)ctx;

    ssid[0] = '\0';
    if (wifi_manager_get_status(&wifi_status) == ESP_OK) {
        snprintf(ssid, sizeof(ssid), "%s", wifi_status.ssid);
    }
    return ssid;
}

static const char *status_read_ip(void *ctx)
{
    static char ip[sizeof(((wifi_manager_status_t *)0)->ip)] = {0};
    wifi_manager_status_t wifi_status = {0};
    (void)ctx;

    ip[0] = '\0';
    if (wifi_manager_get_status(&wifi_status) == ESP_OK) {
        snprintf(ip, sizeof(ip), "%s", wifi_status.ip);
    }
    return ip;
}

static int32_t status_read_rssi(void *ctx)
{
    wifi_manager_status_t wifi_status = {0};
    (void)ctx;

    if (wifi_manager_get_status(&wifi_status) != ESP_OK) {
        return 0;
    }
    return wifi_status.rssi;
}

static uint32_t status_read_vbat_mv(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    (void)vbat_monitor_get_status(&vbat_status);
    return (uint32_t)vbat_status.vbat_mv;
}

static bool status_read_vbat_enabled(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    return vbat_monitor_get_status(&vbat_status) == ESP_OK && vbat_status.enabled;
}

static bool status_read_vbat_initialized(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    return vbat_monitor_get_status(&vbat_status) == ESP_OK && vbat_status.initialized;
}

static bool status_read_vbat_calibrated(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    return vbat_monitor_get_status(&vbat_status) == ESP_OK && vbat_status.calibrated;
}

static bool status_read_vbat_maintenance_mode(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    return vbat_monitor_get_status(&vbat_status) == ESP_OK && vbat_status.maintenance_mode;
}

static int32_t status_read_vbat_gpio(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    (void)vbat_monitor_get_status(&vbat_status);
    return vbat_status.gpio;
}

static int32_t status_read_vbat_raw_avg(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    (void)vbat_monitor_get_status(&vbat_status);
    return vbat_status.raw_avg;
}

static int32_t status_read_vbat_gpio_mv(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    (void)vbat_monitor_get_status(&vbat_status);
    return vbat_status.gpio_mv;
}

static int32_t status_read_vbat_nested_mv(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    (void)vbat_monitor_get_status(&vbat_status);
    return vbat_status.vbat_mv;
}

static uint32_t status_read_vbat_measurement_count(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    (void)vbat_monitor_get_status(&vbat_status);
    return vbat_status.measurement_count;
}

static bool status_read_vbat_shutdown_enabled(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    return vbat_monitor_get_status(&vbat_status) == ESP_OK && vbat_status.shutdown_enabled;
}

static bool status_read_vbat_shutdown_countdown_active(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    return vbat_monitor_get_status(&vbat_status) == ESP_OK && vbat_status.shutdown_countdown_active;
}

static int32_t status_read_vbat_shutdown_threshold_mv(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    (void)vbat_monitor_get_status(&vbat_status);
    return vbat_status.shutdown_threshold_mv;
}

static int32_t status_read_vbat_shutdown_debounce_ms(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    (void)ctx;

    (void)vbat_monitor_get_status(&vbat_status);
    return vbat_status.shutdown_debounce_ms;
}

static bool status_read_power_good_enabled(void *ctx)
{
    (void)ctx;
    return CONFIG_POWER_GOOD_GPIO_ENABLED;
}

static uint32_t status_read_power_good_gpio(void *ctx)
{
    (void)ctx;
    return CONFIG_POWER_GOOD_GPIO;
}

static uint32_t status_read_power_good_active_level(void *ctx)
{
    (void)ctx;
    return CONFIG_POWER_GOOD_ACTIVE_LEVEL;
}

static uint32_t status_read_heap_free(void *ctx)
{
    (void)ctx;
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}

static uint32_t status_read_uptime_s(void *ctx)
{
    (void)ctx;
    return esp_log_timestamp() / 1000;
}

static uint32_t status_read_heartbeat_interval_s(void *ctx)
{
    (void)ctx;
    if (s_config.get_heartbeat_interval_s) {
        return s_config.get_heartbeat_interval_s(s_config.ctx);
    }
    return s_config.heartbeat_interval_default_s > 0 ?
           s_config.heartbeat_interval_default_s : CONFIG_MQTT_HEARTBEAT_INTERVAL_S;
}

static bool status_read_time_synchronized(void *ctx)
{
    (void)ctx;
    return time_sync_is_synchronized();
}

static const char *status_read_ota_state(void *ctx)
{
    static char state[16] = {0};
    firmware_ota_status_t status = {0};
    (void)ctx;

    firmware_ota_get_status(&status);
    snprintf(state, sizeof(state), "%s", firmware_ota_state_name(status.state));
    return state;
}

static bool status_read_ota_in_progress(void *ctx)
{
    firmware_ota_status_t status = {0};
    (void)ctx;

    firmware_ota_get_status(&status);
    return status.in_progress;
}

static uint32_t status_read_ota_progress_pct(void *ctx)
{
    firmware_ota_status_t status = {0};
    (void)ctx;

    firmware_ota_get_status(&status);
    return status.progress_pct;
}

static const char *status_read_ota_target_version(void *ctx)
{
    static char target_version[sizeof(((firmware_ota_status_t *)0)->target_version)] = {0};
    firmware_ota_status_t status = {0};
    (void)ctx;

    firmware_ota_get_status(&status);
    snprintf(target_version, sizeof(target_version), "%s", status.target_version);
    return target_version;
}

static const char *status_read_ota_last_error(void *ctx)
{
    static char last_error[sizeof(((firmware_ota_status_t *)0)->last_error)] = {0};
    firmware_ota_status_t status = {0};
    (void)ctx;

    firmware_ota_get_status(&status);
    snprintf(last_error, sizeof(last_error), "%s", status.last_error);
    return last_error;
}

static const tele_status_field_t s_common_status_fields[] = {
    {
        .id = "wifi_state",
        .label = "Estado Wi-Fi",
        .description = "Estado atual da conexao Wi-Fi.",
        .group = "network",
        .type = TELE_STATUS_TYPE_STRING,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT,
        .read.string = status_read_wifi_state,
    },
    {
        .id = "wifi_ready",
        .label = "Wi-Fi pronto",
        .description = "Indica se a rede esta pronta para uso.",
        .group = "network",
        .type = TELE_STATUS_TYPE_BOOL,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE,
        .read.boolean = status_read_wifi_ready,
    },
    {
        .id = "ssid",
        .label = "SSID",
        .description = "Nome da rede Wi-Fi conectada.",
        .group = "network",
        .type = TELE_STATUS_TYPE_STRING,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE,
        .read.string = status_read_ssid,
    },
    {
        .id = "ip",
        .label = "IP",
        .description = "Endereco IP atual.",
        .group = "network",
        .type = TELE_STATUS_TYPE_STRING,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE,
        .read.string = status_read_ip,
    },
    {
        .id = "rssi",
        .label = "RSSI",
        .description = "Intensidade do sinal Wi-Fi.",
        .group = "network",
        .type = TELE_STATUS_TYPE_I32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT,
        .unit = "dBm",
        .read.i32 = status_read_rssi,
    },
    {
        .id = "vbat_mv",
        .label = "VBAT",
        .description = "Tensao de bateria medida.",
        .group = "power",
        .type = TELE_STATUS_TYPE_U32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT |
                 TELE_STATUS_FLAG_TECHNICAL,
        .unit = "mV",
        .read.u32 = status_read_vbat_mv,
    },
    {
        .id = "heap_free",
        .label = "Heap livre",
        .description = "Memoria heap livre.",
        .group = "memory",
        .type = TELE_STATUS_TYPE_U32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT |
                 TELE_STATUS_FLAG_TECHNICAL,
        .unit = "bytes",
        .read.u32 = status_read_heap_free,
    },
    {
        .id = "uptime_s",
        .label = "Uptime",
        .description = "Tempo desde o boot.",
        .group = "runtime",
        .type = TELE_STATUS_TYPE_U32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT |
                 TELE_STATUS_FLAG_TECHNICAL,
        .unit = "s",
        .read.u32 = status_read_uptime_s,
    },
    {
        .id = "heartbeat_interval_s",
        .label = "Intervalo heartbeat",
        .description = "Intervalo configurado para heartbeat MQTT.",
        .group = "runtime",
        .type = TELE_STATUS_TYPE_U32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_TECHNICAL,
        .unit = "s",
        .read.u32 = status_read_heartbeat_interval_s,
    },
    {
        .id = "time_synchronized",
        .label = "Horario sincronizado",
        .description = "Indica se NTP/sincronismo de tempo esta pronto.",
        .group = "time",
        .type = TELE_STATUS_TYPE_BOOL,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_TECHNICAL,
        .read.boolean = status_read_time_synchronized,
    },
    {
        .id = "ota_state",
        .label = "Estado OTA",
        .description = "Estado atual do subsistema OTA.",
        .group = "updates",
        .type = TELE_STATUS_TYPE_STRING,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT,
        .read.string = status_read_ota_state,
    },
    {
        .id = "ota_in_progress",
        .label = "OTA em progresso",
        .description = "Indica se ha atualizacao OTA em andamento.",
        .group = "updates",
        .type = TELE_STATUS_TYPE_BOOL,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT,
        .read.boolean = status_read_ota_in_progress,
    },
    {
        .id = "ota_progress_pct",
        .label = "Progresso OTA",
        .description = "Percentual de download/aplicacao OTA conhecido.",
        .group = "updates",
        .type = TELE_STATUS_TYPE_U32,
        .unit = "%",
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT,
        .read.u32 = status_read_ota_progress_pct,
    },
    {
        .id = "ota_target_version",
        .label = "Versao OTA alvo",
        .description = "Versao de firmware alvo quando uma OTA por manifest esta em andamento ou pendente.",
        .group = "updates",
        .type = TELE_STATUS_TYPE_STRING,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE,
        .read.string = status_read_ota_target_version,
    },
    {
        .id = "ota_last_error",
        .label = "Ultimo erro OTA",
        .description = "Ultimo erro reportado pelo subsistema OTA.",
        .group = "updates",
        .type = TELE_STATUS_TYPE_STRING,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_STATE,
        .read.string = status_read_ota_last_error,
    },
    {
        .id = "power_good.enabled",
        .label = "Power Good habilitado",
        .description = "Indica se o controle POWER_GOOD esta habilitado no firmware.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.boolean = status_read_power_good_enabled,
    },
    {
        .id = "power_good.gpio",
        .label = "Power Good GPIO",
        .description = "GPIO configurado para POWER_GOOD.",
        .group = "power",
        .type = TELE_STATUS_TYPE_U32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.u32 = status_read_power_good_gpio,
    },
    {
        .id = "power_good.active_level",
        .label = "Power Good nivel ativo",
        .description = "Nivel logico ativo do POWER_GOOD.",
        .group = "power",
        .type = TELE_STATUS_TYPE_U32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.u32 = status_read_power_good_active_level,
    },
    {
        .id = "vbat.enabled",
        .label = "VBAT habilitado",
        .description = "Indica se o monitoramento de bateria esta habilitado.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.boolean = status_read_vbat_enabled,
    },
    {
        .id = "vbat.initialized",
        .label = "VBAT inicializado",
        .description = "Indica se o monitor VBAT foi inicializado.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.boolean = status_read_vbat_initialized,
    },
    {
        .id = "vbat.calibrated",
        .label = "VBAT calibrado",
        .description = "Indica se a leitura VBAT esta usando calibracao ADC.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.boolean = status_read_vbat_calibrated,
    },
    {
        .id = "vbat.maintenance_mode",
        .label = "VBAT manutencao",
        .description = "Indica modo de manutencao por bateria desconectada.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.boolean = status_read_vbat_maintenance_mode,
    },
    {
        .id = "vbat.gpio",
        .label = "VBAT GPIO",
        .description = "GPIO de leitura VBAT.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.i32 = status_read_vbat_gpio,
    },
    {
        .id = "vbat.raw_avg",
        .label = "VBAT ADC raw",
        .description = "Media bruta da leitura ADC de VBAT.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.i32 = status_read_vbat_raw_avg,
    },
    {
        .id = "vbat.gpio_mv",
        .label = "VBAT GPIO mV",
        .description = "Tensao medida diretamente no GPIO VBAT.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .unit = "mV",
        .read.i32 = status_read_vbat_gpio_mv,
    },
    {
        .id = "vbat.vbat_mv",
        .label = "VBAT mV",
        .description = "Tensao estimada da bateria.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .unit = "mV",
        .read.i32 = status_read_vbat_nested_mv,
    },
    {
        .id = "vbat.measurement_count",
        .label = "VBAT amostras",
        .description = "Quantidade de medicoes VBAT realizadas.",
        .group = "power",
        .type = TELE_STATUS_TYPE_U32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.u32 = status_read_vbat_measurement_count,
    },
    {
        .id = "vbat.shutdown_enabled",
        .label = "VBAT shutdown habilitado",
        .description = "Indica se desligamento por bateria baixa esta habilitado.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.boolean = status_read_vbat_shutdown_enabled,
    },
    {
        .id = "vbat.shutdown_threshold_mv",
        .label = "VBAT threshold",
        .description = "Limiar de desligamento por bateria baixa.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .unit = "mV",
        .read.i32 = status_read_vbat_shutdown_threshold_mv,
    },
    {
        .id = "vbat.shutdown_debounce_ms",
        .label = "VBAT debounce",
        .description = "Tempo de debounce para desligamento por bateria baixa.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .unit = "ms",
        .read.i32 = status_read_vbat_shutdown_debounce_ms,
    },
    {
        .id = "vbat.shutdown_countdown_active",
        .label = "VBAT countdown ativo",
        .description = "Indica se o countdown de desligamento esta ativo.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .channel_flags = TELE_SYSTEM_REGISTRY_CHANNEL_FLAGS,
        .flags = TELE_STATUS_FLAG_TECHNICAL,
        .read.boolean = status_read_vbat_shutdown_countdown_active,
    },
};

static esp_err_t tele_system_registry_register_status_fields(void)
{
    if (s_status_fields_registered) {
        return ESP_OK;
    }

    esp_err_t err = tele_status_register_fields(s_common_status_fields,
                                                sizeof(s_common_status_fields) /
                                                sizeof(s_common_status_fields[0]));
    if (err == ESP_OK) {
        s_status_fields_registered = true;
    }
    return err;
}

static esp_err_t tele_system_registry_register_config_fields(void)
{
    if (s_config_fields_registered) {
        return ESP_OK;
    }

    s_mqtt_config_fields[0].default_value.u32 =
        s_config.heartbeat_interval_default_s > 0 ?
        s_config.heartbeat_interval_default_s : CONFIG_MQTT_HEARTBEAT_INTERVAL_S;

    esp_err_t err = tele_config_register_fields(s_mqtt_config_fields,
                                                sizeof(s_mqtt_config_fields) /
                                                    sizeof(s_mqtt_config_fields[0]));
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_config_fields_registered = true;
        return ESP_OK;
    }
    return err;
}

uint32_t tele_system_registry_get_effective_heartbeat_interval_s(uint32_t fallback_s)
{
    tele_config_value_t value = {0};
    bool from_nvs = false;
    esp_err_t err = tele_config_get_effective(TELE_SYSTEM_REGISTRY_CONFIG_ID_HEARTBEAT_INTERVAL,
                                              &value,
                                              NULL,
                                              0,
                                              &from_nvs);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Heartbeat MQTT efetivo: %" PRIu32 "s (%s)",
                 value.u32,
                 from_nvs ? "nvs" : "default");
        return value.u32;
    }

    ESP_LOGW(TAG,
             "Falha ao ler heartbeat MQTT efetivo (%s); usando default %" PRIu32 "s",
             esp_err_to_name(err),
             fallback_s);
    return fallback_s;
}

cJSON *tele_system_registry_build_technical_status(void)
{
    vbat_monitor_status_t vbat_status = {0};
    int64_t now_ms = (int64_t)esp_log_timestamp();
    cJSON *result = cJSON_CreateObject();
    cJSON *vbat_json = NULL;
    cJSON *power_good_json = NULL;

    if (!result) {
        return NULL;
    }

    cJSON_AddNumberToObject(result, "uptime_seconds", (double)(now_ms / 1000));
    cJSON_AddBoolToObject(result, "time_synchronized", time_sync_is_synchronized());
    cJSON_AddNumberToObject(result, "heap_free", (double)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(result,
                            "heartbeat_interval_s",
                            (double)status_read_heartbeat_interval_s(NULL));

    power_good_json = cJSON_AddObjectToObject(result, "power_good");
    if (power_good_json) {
        cJSON_AddBoolToObject(power_good_json, "enabled", CONFIG_POWER_GOOD_GPIO_ENABLED);
        cJSON_AddNumberToObject(power_good_json, "gpio", CONFIG_POWER_GOOD_GPIO);
        cJSON_AddNumberToObject(power_good_json, "active_level", CONFIG_POWER_GOOD_ACTIVE_LEVEL);
    }

    if (vbat_monitor_get_status(&vbat_status) == ESP_OK) {
        vbat_json = cJSON_AddObjectToObject(result, "vbat");
        if (vbat_json) {
            cJSON_AddBoolToObject(vbat_json, "enabled", vbat_status.enabled);
            cJSON_AddBoolToObject(vbat_json, "initialized", vbat_status.initialized);
            cJSON_AddBoolToObject(vbat_json, "calibrated", vbat_status.calibrated);
            cJSON_AddNumberToObject(vbat_json, "gpio", vbat_status.gpio);
            cJSON_AddNumberToObject(vbat_json, "raw_avg", vbat_status.raw_avg);
            cJSON_AddNumberToObject(vbat_json, "gpio_mv", vbat_status.gpio_mv);
            cJSON_AddNumberToObject(vbat_json, "vbat_mv", vbat_status.vbat_mv);
            cJSON_AddNumberToObject(vbat_json, "last_measurement_ms", (double)vbat_status.last_measurement_ms);
            cJSON_AddStringToObject(vbat_json, "last_moment", vbat_monitor_moment_name(vbat_status.last_moment));
            cJSON_AddNumberToObject(vbat_json, "measurement_count", (double)vbat_status.measurement_count);
            cJSON_AddBoolToObject(vbat_json, "shutdown_enabled", vbat_status.shutdown_enabled);
            cJSON_AddNumberToObject(vbat_json, "shutdown_threshold_mv", vbat_status.shutdown_threshold_mv);
            cJSON_AddNumberToObject(vbat_json, "shutdown_debounce_ms", vbat_status.shutdown_debounce_ms);
            cJSON_AddBoolToObject(vbat_json, "shutdown_countdown_active", vbat_status.shutdown_countdown_active);
            cJSON_AddNumberToObject(vbat_json,
                                    "shutdown_below_threshold_since_ms",
                                    (double)vbat_status.shutdown_below_threshold_since_ms);
            cJSON_AddNumberToObject(vbat_json,
                                    "shutdown_countdown_elapsed_ms",
                                    vbat_status.shutdown_countdown_active &&
                                    vbat_status.shutdown_below_threshold_since_ms > 0 ?
                                    (double)(now_ms - vbat_status.shutdown_below_threshold_since_ms) :
                                    0.0);
        }
    }

    return result;
}

static esp_err_t tele_system_registry_apply_config_field(const tele_config_field_t *field,
                                                        const tele_config_value_t *value,
                                                        void *ctx)
{
    (void)ctx;

    if (!field || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(field->id, DEVICE_CONFIG_ID_PROVISIONING_SSID) == 0) {
        esp_err_t err = wifi_manager_set_provisioning_ssid(value->string);
        return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
    }
    if (strcmp(field->id, DEVICE_CONFIG_ID_STA_MAX_RETRY) == 0) {
        esp_err_t err = wifi_manager_set_sta_max_retry((int)value->u32);
        return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
    }
    if (strcmp(field->id, TELE_SYSTEM_REGISTRY_CONFIG_ID_HEARTBEAT_INTERVAL) == 0) {
        if (s_config.apply_heartbeat_interval_s) {
            return s_config.apply_heartbeat_interval_s(value->u32, s_config.ctx);
        }
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t tele_system_registry_register(const tele_system_registry_config_t *config)
{
    if (config) {
        s_config = *config;
    } else {
        s_config = (tele_system_registry_config_t){0};
    }

    esp_err_t err = device_config_register_fields();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar configuracoes do dispositivo: %s", esp_err_to_name(err));
        return err;
    }

    err = tele_system_registry_register_config_fields();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar configuracoes de sistema: %s", esp_err_to_name(err));
        return err;
    }

    err = tele_config_set_apply_handler(DEVICE_CONFIG_ID_PROVISIONING_SSID,
                                        tele_system_registry_apply_config_field,
                                        NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar apply handler para SSID de provisionamento: %s", esp_err_to_name(err));
        return err;
    }

    err = tele_config_set_apply_handler(DEVICE_CONFIG_ID_STA_MAX_RETRY,
                                        tele_system_registry_apply_config_field,
                                        NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar apply handler para retry STA: %s", esp_err_to_name(err));
        return err;
    }

    err = tele_config_set_apply_handler(TELE_SYSTEM_REGISTRY_CONFIG_ID_HEARTBEAT_INTERVAL,
                                        tele_system_registry_apply_config_field,
                                        NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar apply handler para heartbeat: %s", esp_err_to_name(err));
        return err;
    }

    err = tele_system_registry_register_status_fields();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar status de sistema: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
