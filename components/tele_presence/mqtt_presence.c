#include "mqtt_presence.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "firmware_ota.h"
#include "tele_ca_store.h"
#include "tele_ca_updater.h"
#include "tele_commands.h"
#include "tele_config.h"
#include "tele_mqtt.h"
#include "tele_status.h"

#include "device_config_store.h"
#include "firmware_version.h"
#include "time_sync.h"
#include "vbat_monitor.h"
#include "wifi_manager.h"

#ifndef CONFIG_MQTT_PRESENCE_ENABLED
#define CONFIG_MQTT_PRESENCE_ENABLED 0
#endif

#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI ""
#endif

#ifndef CONFIG_MQTT_USERNAME
#define CONFIG_MQTT_USERNAME ""
#endif

#ifndef CONFIG_MQTT_PASSWORD
#define CONFIG_MQTT_PASSWORD ""
#endif

#ifndef CONFIG_MQTT_BASE_TOPIC
#define CONFIG_MQTT_BASE_TOPIC "v1/device"
#endif

#ifndef CONFIG_MQTT_DEVICE_ID_PREFIX
#define CONFIG_MQTT_DEVICE_ID_PREFIX "ESP32-Device"
#endif

#ifndef CONFIG_MQTT_HEARTBEAT_INTERVAL_S
#define CONFIG_MQTT_HEARTBEAT_INTERVAL_S 60
#endif

#ifndef CONFIG_MQTT_KEEPALIVE_S
#define CONFIG_MQTT_KEEPALIVE_S 60
#endif

#ifndef CONFIG_MQTT_QOS_CRITICAL
#define CONFIG_MQTT_QOS_CRITICAL 1
#endif

#ifndef CONFIG_MQTT_QOS_TELEMETRY
#define CONFIG_MQTT_QOS_TELEMETRY 0
#endif

#define MQTT_CONFIG_ID_HEARTBEAT_INTERVAL "mqtt.heartbeat_interval_s"
#define MQTT_CONFIG_HEARTBEAT_INTERVAL_NVS_KEY "m_hbint"
#define MQTT_HEARTBEAT_INTERVAL_MIN_S 15
#define MQTT_HEARTBEAT_INTERVAL_MAX_S 3600

#ifndef CONFIG_POWER_GOOD_GPIO_ENABLED
#define CONFIG_POWER_GOOD_GPIO_ENABLED 0
#endif

#ifndef CONFIG_POWER_GOOD_GPIO
#define CONFIG_POWER_GOOD_GPIO 6
#endif

#ifndef CONFIG_POWER_GOOD_ACTIVE_LEVEL
#define CONFIG_POWER_GOOD_ACTIVE_LEVEL 1
#endif

static const char *TAG = "mqtt-presence";

static bool s_started;
static bool s_wifi_event_registered;
static bool s_status_fields_registered;
static bool s_config_fields_registered;
static bool s_commands_registered;

static const tele_command_arg_t s_update_manifest_args[] = {
    {
        .id = "manifest_url",
        .type = TELE_COMMAND_ARG_STRING,
        .required = true,
        .min_len = 1,
        .max_len = TELE_MANIFEST_URL_SIZE - 1,
    },
    {
        .id = "channel",
        .type = TELE_COMMAND_ARG_STRING,
        .required = false,
        .min_len = 1,
        .max_len = TELE_MANIFEST_CHANNEL_SIZE - 1,
    },
    {
        .id = "allow_same_version",
        .type = TELE_COMMAND_ARG_BOOL,
        .required = false,
    },
    {
        .id = "restart_on_success",
        .type = TELE_COMMAND_ARG_BOOL,
        .required = false,
    },
};

static const tele_command_arg_t s_ca_manifest_args[] = {
    {
        .id = "manifest_url",
        .type = TELE_COMMAND_ARG_STRING,
        .required = true,
        .min_len = 1,
        .max_len = TELE_MANIFEST_URL_SIZE - 1,
    },
    {
        .id = "channel",
        .type = TELE_COMMAND_ARG_STRING,
        .required = false,
        .min_len = 1,
        .max_len = TELE_MANIFEST_CHANNEL_SIZE - 1,
    },
    {
        .id = "restart_on_update",
        .type = TELE_COMMAND_ARG_BOOL,
        .required = false,
    },
};

static const tele_command_t s_update_commands[] = {
    {
        .name = "ota_check",
        .label = "Verificar OTA",
        .description = "Consulta um manifest remoto de firmware sem aplicar a atualizacao.",
        .group = "updates",
        .flags = TELE_COMMAND_FLAG_MQTT,
        .args = s_update_manifest_args,
        .arg_count = 2,
    },
    {
        .name = "ota_apply",
        .label = "Aplicar OTA",
        .description = "Inicia OTA de firmware por manifest em streaming.",
        .group = "updates",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_MUTATING | TELE_COMMAND_FLAG_REBOOT_REQUIRED,
        .args = s_update_manifest_args,
        .arg_count = 4,
    },
    {
        .name = "ca_check",
        .label = "Verificar CA",
        .description = "Consulta um manifest remoto de bundle CA sem aplicar.",
        .group = "updates",
        .flags = TELE_COMMAND_FLAG_MQTT,
        .args = s_ca_manifest_args,
        .arg_count = 2,
    },
    {
        .name = "ca_apply",
        .label = "Aplicar CA",
        .description = "Baixa, verifica e ativa um bundle CA por manifest.",
        .group = "updates",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_MUTATING,
        .args = s_ca_manifest_args,
        .arg_count = 3,
    },
};

static const tele_config_field_t s_mqtt_config_fields[] = {
    {
        .id = MQTT_CONFIG_ID_HEARTBEAT_INTERVAL,
        .nvs_key = MQTT_CONFIG_HEARTBEAT_INTERVAL_NVS_KEY,
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = CONFIG_MQTT_HEARTBEAT_INTERVAL_S,
        .min.u32 = MQTT_HEARTBEAT_INTERVAL_MIN_S,
        .max.u32 = MQTT_HEARTBEAT_INTERVAL_MAX_S,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT,
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

static bool mqtt_presence_ready(void *ctx)
{
    wifi_manager_status_t wifi_status = {0};
    (void)ctx;

    return wifi_manager_get_status(&wifi_status) == ESP_OK &&
           wifi_status.state == WIFI_MANAGER_STATE_STA_CONNECTED &&
           wifi_status.wifi_ready &&
           time_sync_is_synchronized();
}

static bool mqtt_presence_build_timestamp(char *buffer, size_t buffer_len, void *ctx)
{
    (void)ctx;
    return time_sync_format_utc_now(buffer, buffer_len);
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
    return tele_mqtt_get_heartbeat_interval_s();
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
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.string = status_read_wifi_state,
    },
    {
        .id = "wifi_ready",
        .label = "Wi-Fi pronto",
        .description = "Indica se a rede esta pronta para uso.",
        .group = "network",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.boolean = status_read_wifi_ready,
    },
    {
        .id = "ssid",
        .label = "SSID",
        .description = "Nome da rede Wi-Fi conectada.",
        .group = "network",
        .type = TELE_STATUS_TYPE_STRING,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.string = status_read_ssid,
    },
    {
        .id = "ip",
        .label = "IP",
        .description = "Endereco IP atual.",
        .group = "network",
        .type = TELE_STATUS_TYPE_STRING,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.string = status_read_ip,
    },
    {
        .id = "rssi",
        .label = "RSSI",
        .description = "Intensidade do sinal Wi-Fi.",
        .group = "network",
        .type = TELE_STATUS_TYPE_I32,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .unit = "dBm",
        .read.i32 = status_read_rssi,
    },
    {
        .id = "vbat_mv",
        .label = "VBAT",
        .description = "Tensao de bateria medida.",
        .group = "power",
        .type = TELE_STATUS_TYPE_U32,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT |
                 TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .unit = "mV",
        .read.u32 = status_read_vbat_mv,
    },
    {
        .id = "heap_free",
        .label = "Heap livre",
        .description = "Memoria heap livre.",
        .group = "memory",
        .type = TELE_STATUS_TYPE_U32,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT |
                 TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .unit = "bytes",
        .read.u32 = status_read_heap_free,
    },
    {
        .id = "uptime_s",
        .label = "Uptime",
        .description = "Tempo desde o boot.",
        .group = "runtime",
        .type = TELE_STATUS_TYPE_U32,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT |
                 TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .unit = "s",
        .read.u32 = status_read_uptime_s,
    },
    {
        .id = "heartbeat_interval_s",
        .label = "Intervalo heartbeat",
        .description = "Intervalo configurado para heartbeat MQTT.",
        .group = "runtime",
        .type = TELE_STATUS_TYPE_U32,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .unit = "s",
        .read.u32 = status_read_heartbeat_interval_s,
    },
    {
        .id = "time_synchronized",
        .label = "Horario sincronizado",
        .description = "Indica se NTP/sincronismo de tempo esta pronto.",
        .group = "time",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.boolean = status_read_time_synchronized,
    },
    {
        .id = "ota_state",
        .label = "Estado OTA",
        .description = "Estado atual do subsistema OTA.",
        .group = "updates",
        .type = TELE_STATUS_TYPE_STRING,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.string = status_read_ota_state,
    },
    {
        .id = "ota_in_progress",
        .label = "OTA em progresso",
        .description = "Indica se ha atualizacao OTA em andamento.",
        .group = "updates",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.boolean = status_read_ota_in_progress,
    },
    {
        .id = "ota_progress_pct",
        .label = "Progresso OTA",
        .description = "Percentual de download/aplicacao OTA conhecido.",
        .group = "updates",
        .type = TELE_STATUS_TYPE_U32,
        .unit = "%",
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.u32 = status_read_ota_progress_pct,
    },
    {
        .id = "ota_target_version",
        .label = "Versao OTA alvo",
        .description = "Versao de firmware alvo quando uma OTA por manifest esta em andamento ou pendente.",
        .group = "updates",
        .type = TELE_STATUS_TYPE_STRING,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.string = status_read_ota_target_version,
    },
    {
        .id = "ota_last_error",
        .label = "Ultimo erro OTA",
        .description = "Ultimo erro reportado pelo subsistema OTA.",
        .group = "updates",
        .type = TELE_STATUS_TYPE_STRING,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.string = status_read_ota_last_error,
    },
    {
        .id = "power_good.enabled",
        .label = "Power Good habilitado",
        .description = "Indica se o controle POWER_GOOD esta habilitado no firmware.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.boolean = status_read_power_good_enabled,
    },
    {
        .id = "power_good.gpio",
        .label = "Power Good GPIO",
        .description = "GPIO configurado para POWER_GOOD.",
        .group = "power",
        .type = TELE_STATUS_TYPE_U32,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.u32 = status_read_power_good_gpio,
    },
    {
        .id = "power_good.active_level",
        .label = "Power Good nivel ativo",
        .description = "Nivel logico ativo do POWER_GOOD.",
        .group = "power",
        .type = TELE_STATUS_TYPE_U32,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.u32 = status_read_power_good_active_level,
    },
    {
        .id = "vbat.enabled",
        .label = "VBAT habilitado",
        .description = "Indica se o monitoramento de bateria esta habilitado.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.boolean = status_read_vbat_enabled,
    },
    {
        .id = "vbat.initialized",
        .label = "VBAT inicializado",
        .description = "Indica se o monitor VBAT foi inicializado.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.boolean = status_read_vbat_initialized,
    },
    {
        .id = "vbat.calibrated",
        .label = "VBAT calibrado",
        .description = "Indica se a leitura VBAT esta usando calibracao ADC.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.boolean = status_read_vbat_calibrated,
    },
    {
        .id = "vbat.maintenance_mode",
        .label = "VBAT manutencao",
        .description = "Indica modo de manutencao por bateria desconectada.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.boolean = status_read_vbat_maintenance_mode,
    },
    {
        .id = "vbat.gpio",
        .label = "VBAT GPIO",
        .description = "GPIO de leitura VBAT.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.i32 = status_read_vbat_gpio,
    },
    {
        .id = "vbat.raw_avg",
        .label = "VBAT ADC raw",
        .description = "Media bruta da leitura ADC de VBAT.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.i32 = status_read_vbat_raw_avg,
    },
    {
        .id = "vbat.gpio_mv",
        .label = "VBAT GPIO mV",
        .description = "Tensao medida diretamente no GPIO VBAT.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .unit = "mV",
        .read.i32 = status_read_vbat_gpio_mv,
    },
    {
        .id = "vbat.vbat_mv",
        .label = "VBAT mV",
        .description = "Tensao estimada da bateria.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .unit = "mV",
        .read.i32 = status_read_vbat_nested_mv,
    },
    {
        .id = "vbat.measurement_count",
        .label = "VBAT amostras",
        .description = "Quantidade de medicoes VBAT realizadas.",
        .group = "power",
        .type = TELE_STATUS_TYPE_U32,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.u32 = status_read_vbat_measurement_count,
    },
    {
        .id = "vbat.shutdown_enabled",
        .label = "VBAT shutdown habilitado",
        .description = "Indica se desligamento por bateria baixa esta habilitado.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.boolean = status_read_vbat_shutdown_enabled,
    },
    {
        .id = "vbat.shutdown_threshold_mv",
        .label = "VBAT threshold",
        .description = "Limiar de desligamento por bateria baixa.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .unit = "mV",
        .read.i32 = status_read_vbat_shutdown_threshold_mv,
    },
    {
        .id = "vbat.shutdown_debounce_ms",
        .label = "VBAT debounce",
        .description = "Tempo de debounce para desligamento por bateria baixa.",
        .group = "power",
        .type = TELE_STATUS_TYPE_I32,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .unit = "ms",
        .read.i32 = status_read_vbat_shutdown_debounce_ms,
    },
    {
        .id = "vbat.shutdown_countdown_active",
        .label = "VBAT countdown ativo",
        .description = "Indica se o countdown de desligamento esta ativo.",
        .group = "power",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT | TELE_STATUS_FLAG_WEB,
        .read.boolean = status_read_vbat_shutdown_countdown_active,
    },
};

static esp_err_t mqtt_presence_register_status_fields(void)
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

static esp_err_t mqtt_presence_register_config_fields(void)
{
    if (s_config_fields_registered) {
        return ESP_OK;
    }

    esp_err_t err = tele_config_register_fields(s_mqtt_config_fields,
                                                sizeof(s_mqtt_config_fields) /
                                                    sizeof(s_mqtt_config_fields[0]));
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_config_fields_registered = true;
        return ESP_OK;
    }
    return err;
}

static esp_err_t mqtt_presence_register_commands(void)
{
    if (s_commands_registered) {
        return ESP_OK;
    }

    esp_err_t err = tele_commands_register(s_update_commands,
                                           sizeof(s_update_commands) /
                                               sizeof(s_update_commands[0]));
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_commands_registered = true;
        return ESP_OK;
    }
    return err;
}

static const char *json_string_arg(const cJSON *args, const char *id, bool required)
{
    const cJSON *item = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, id) : NULL;
    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        return required ? NULL : "";
    }
    return item->valuestring;
}

static bool json_bool_arg(const cJSON *args, const char *id, bool default_value)
{
    const cJSON *item = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, id) : NULL;
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_value;
}

static cJSON *build_artifact_check_result(const tele_manifest_artifact_t *artifact,
                                          const char *current_version,
                                          bool available)
{
    cJSON *result = cJSON_CreateObject();
    if (!result || !artifact) {
        cJSON_Delete(result);
        return NULL;
    }

    cJSON_AddStringToObject(result, "current_version", current_version ? current_version : "");
    cJSON_AddBoolToObject(result, "available", available);
    cJSON_AddStringToObject(result, "target_version", artifact->version);
    cJSON_AddStringToObject(result, "build_id", artifact->build_id);
    cJSON_AddStringToObject(result, "artifact_type", artifact->artifact_type);
    cJSON_AddStringToObject(result, "channel", artifact->channel);
    cJSON_AddNumberToObject(result, "size", (double)artifact->size);
    cJSON_AddBoolToObject(result, "critical", artifact->critical);
    if (artifact->url_count > 0) {
        cJSON_AddStringToObject(result, "artifact_url", artifact->urls[0]);
    }
    return result;
}

static esp_err_t handle_ota_check_command(const cJSON *args,
                                          cJSON **out_result,
                                          const char **out_error)
{
    const char *manifest_url = json_string_arg(args, "manifest_url", true);
    const char *channel = json_string_arg(args, "channel", false);
    bool allow_same_version = json_bool_arg(args, "allow_same_version", false);
    tele_manifest_artifact_t artifact = {0};

    if (!manifest_url) {
        *out_error = "missing_manifest_url";
        return ESP_ERR_INVALID_ARG;
    }

    const firmware_ota_manifest_config_t config = {
        .manifest_url = manifest_url,
        .channel = channel[0] != '\0' ? channel : NULL,
        .allow_same_version = allow_same_version,
        .restart_on_success = false,
    };
    esp_err_t err = firmware_ota_check_manifest(&config, &artifact);
    if (err != ESP_OK) {
        *out_error = "ota_check_failed";
        return err;
    }

    *out_result = build_artifact_check_result(&artifact,
                                              APP_VERSION_SEMVER,
                                              strcmp(artifact.version, APP_VERSION_SEMVER) != 0 ||
                                              allow_same_version);
    return *out_result ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t handle_ota_apply_command(const cJSON *args,
                                          cJSON **out_result,
                                          const char **out_error)
{
    const char *manifest_url = json_string_arg(args, "manifest_url", true);
    const char *channel = json_string_arg(args, "channel", false);

    if (!manifest_url) {
        *out_error = "missing_manifest_url";
        return ESP_ERR_INVALID_ARG;
    }

    const firmware_ota_manifest_config_t config = {
        .manifest_url = manifest_url,
        .channel = channel[0] != '\0' ? channel : NULL,
        .allow_same_version = json_bool_arg(args, "allow_same_version", false),
        .restart_on_success = json_bool_arg(args, "restart_on_success", true),
    };
    esp_err_t err = firmware_ota_start_manifest(&config);
    if (err != ESP_OK) {
        *out_error = "ota_apply_start_failed";
        return err;
    }

    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(result, "started", true);
    cJSON_AddStringToObject(result, "manifest_url", manifest_url);
    cJSON_AddStringToObject(result, "channel", channel);
    cJSON_AddBoolToObject(result, "restart_on_success", config.restart_on_success);
    *out_result = result;
    return ESP_OK;
}

static esp_err_t handle_ca_check_command(const cJSON *args,
                                         cJSON **out_result,
                                         const char **out_error)
{
    const char *manifest_url = json_string_arg(args, "manifest_url", true);
    const char *channel = json_string_arg(args, "channel", false);
    char current_version[64] = {0};
    tele_manifest_artifact_t artifact = {0};

    if (!manifest_url) {
        *out_error = "missing_manifest_url";
        return ESP_ERR_INVALID_ARG;
    }

    const tele_ca_updater_config_t config = {
        .manifest_url = manifest_url,
        .channel = channel[0] != '\0' ? channel : NULL,
        .restart_on_update = false,
    };
    esp_err_t err = tele_ca_updater_check(&config, &artifact);
    if (err != ESP_OK) {
        *out_error = "ca_check_failed";
        return err;
    }

    (void)tele_ca_store_get_version(current_version, sizeof(current_version));
    *out_result = build_artifact_check_result(&artifact,
                                              current_version,
                                              strcmp(current_version, artifact.version) != 0);
    return *out_result ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t handle_ca_apply_command(const cJSON *args,
                                         cJSON **out_result,
                                         const char **out_error)
{
    const char *manifest_url = json_string_arg(args, "manifest_url", true);
    const char *channel = json_string_arg(args, "channel", false);
    tele_manifest_run_result_t run_result = {0};

    if (!manifest_url) {
        *out_error = "missing_manifest_url";
        return ESP_ERR_INVALID_ARG;
    }

    const tele_ca_updater_config_t config = {
        .manifest_url = manifest_url,
        .channel = channel[0] != '\0' ? channel : NULL,
        .restart_on_update = json_bool_arg(args, "restart_on_update", false),
    };
    esp_err_t err = tele_ca_updater_apply(&config, &run_result);
    if (err != ESP_OK) {
        *out_error = "ca_apply_failed";
        return err;
    }

    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(result, "result", (double)run_result.result);
    cJSON_AddStringToObject(result, "selected_url", run_result.selected_url);
    cJSON_AddNumberToObject(result, "bytes_received", (double)run_result.bytes_received);
    cJSON_AddStringToObject(result, "message", run_result.message);
    *out_result = result;
    return ESP_OK;
}

static bool mqtt_presence_is_mutating_command(const char *cmd_name,
                                              const cJSON *args,
                                              void *ctx)
{
    (void)args;
    (void)ctx;
    return cmd_name &&
           (strcmp(cmd_name, "ota_apply") == 0 ||
            strcmp(cmd_name, "ca_apply") == 0);
}

static esp_err_t mqtt_presence_handle_command(const char *cmd_name,
                                              const cJSON *args,
                                              cJSON **out_result,
                                              const char **out_error,
                                              void *ctx)
{
    (void)ctx;

    if (!cmd_name || !out_result || !out_error) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_result = NULL;
    *out_error = NULL;

    if (strcmp(cmd_name, "ota_check") == 0) {
        return handle_ota_check_command(args, out_result, out_error);
    }
    if (strcmp(cmd_name, "ota_apply") == 0) {
        return handle_ota_apply_command(args, out_result, out_error);
    }
    if (strcmp(cmd_name, "ca_check") == 0) {
        return handle_ca_check_command(args, out_result, out_error);
    }
    if (strcmp(cmd_name, "ca_apply") == 0) {
        return handle_ca_apply_command(args, out_result, out_error);
    }

    return ESP_ERR_NOT_FOUND;
}

static uint32_t mqtt_presence_effective_heartbeat_interval_s(void)
{
    tele_config_value_t value = {0};
    bool from_nvs = false;
    esp_err_t err = tele_config_get_effective(MQTT_CONFIG_ID_HEARTBEAT_INTERVAL,
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
             (uint32_t)CONFIG_MQTT_HEARTBEAT_INTERVAL_S);
    return CONFIG_MQTT_HEARTBEAT_INTERVAL_S;
}

static cJSON *mqtt_presence_build_technical_status(void *ctx)
{
    vbat_monitor_status_t vbat_status = {0};
    int64_t now_ms = (int64_t)esp_log_timestamp();
    cJSON *result = cJSON_CreateObject();
    cJSON *vbat_json = NULL;
    cJSON *power_good_json = NULL;
    (void)ctx;

    if (!result) {
        return NULL;
    }

    cJSON_AddNumberToObject(result, "uptime_seconds", (double)(now_ms / 1000));
    cJSON_AddBoolToObject(result, "time_synchronized", time_sync_is_synchronized());
    cJSON_AddNumberToObject(result, "heap_free", (double)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(result,
                            "heartbeat_interval_s",
                            (double)tele_mqtt_get_heartbeat_interval_s());

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

static esp_err_t mqtt_presence_apply_config_field(const tele_config_field_t *field,
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
    if (strcmp(field->id, MQTT_CONFIG_ID_HEARTBEAT_INTERVAL) == 0) {
        return tele_mqtt_apply_heartbeat_interval_s(value->u32);
    }
    return ESP_OK;
}

static void mqtt_presence_restart(uint32_t delay_ms, void *ctx)
{
    (void)delay_ms;
    (void)ctx;
    esp_restart();
}

static void mqtt_presence_wifi_event_handler(void *arg,
                                             esp_event_base_t event_base,
                                             int32_t event_id,
                                             void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_MANAGER_EVENT_STA_CONNECTED) {
        (void)tele_mqtt_start_client_if_ready();
    }
}

esp_err_t mqtt_presence_start(void)
{
#if !CONFIG_MQTT_PRESENCE_ENABLED
    return ESP_OK;
#else
    tele_mqtt_config_t config = {
        .broker_uri = CONFIG_MQTT_BROKER_URI,
        .username = CONFIG_MQTT_USERNAME,
        .password = CONFIG_MQTT_PASSWORD,
        .base_topic = CONFIG_MQTT_BASE_TOPIC,
        .device_id_prefix = CONFIG_MQTT_DEVICE_ID_PREFIX,
        .firmware_version = APP_VERSION_STRING,
        .heartbeat_interval_s = CONFIG_MQTT_HEARTBEAT_INTERVAL_S,
        .keepalive_s = CONFIG_MQTT_KEEPALIVE_S,
        .qos_critical = CONFIG_MQTT_QOS_CRITICAL,
        .qos_telemetry = CONFIG_MQTT_QOS_TELEMETRY,
        .is_ready = mqtt_presence_ready,
        .build_timestamp = mqtt_presence_build_timestamp,
        .build_technical_status = mqtt_presence_build_technical_status,
        .is_mutating_command = mqtt_presence_is_mutating_command,
        .handle_command = mqtt_presence_handle_command,
        .restart = mqtt_presence_restart,
    };

    if (s_started) {
        return ESP_OK;
    }

    if (CONFIG_MQTT_BROKER_URI[0] == '\0') {
        ESP_LOGW(TAG, "MQTT habilitado sem broker URI; modulo nao iniciado");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = device_config_store_register_fields();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar configuracoes MQTT: %s", esp_err_to_name(err));
        return err;
    }
    err = mqtt_presence_register_config_fields();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar configuracoes do MQTT presence: %s", esp_err_to_name(err));
        return err;
    }
    err = tele_config_set_apply_handler(DEVICE_CONFIG_ID_PROVISIONING_SSID,
                                        mqtt_presence_apply_config_field,
                                        NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar apply handler para SSID de provisionamento: %s", esp_err_to_name(err));
        return err;
    }
    err = tele_config_set_apply_handler(DEVICE_CONFIG_ID_STA_MAX_RETRY,
                                        mqtt_presence_apply_config_field,
                                        NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar apply handler para retry STA: %s", esp_err_to_name(err));
        return err;
    }
    err = tele_config_set_apply_handler(MQTT_CONFIG_ID_HEARTBEAT_INTERVAL,
                                        mqtt_presence_apply_config_field,
                                        NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar apply handler para heartbeat MQTT: %s", esp_err_to_name(err));
        return err;
    }
    config.heartbeat_interval_s = mqtt_presence_effective_heartbeat_interval_s();

    err = mqtt_presence_register_status_fields();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar status MQTT: %s", esp_err_to_name(err));
        return err;
    }

    err = mqtt_presence_register_commands();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar comandos MQTT do produto: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_wifi_event_registered) {
        err = esp_event_handler_register(WIFI_MANAGER_EVENT,
                                         ESP_EVENT_ANY_ID,
                                         mqtt_presence_wifi_event_handler,
                                         NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao registrar eventos Wi-Fi para MQTT: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_event_registered = true;
    }

    err = tele_mqtt_start(&config);
    if (err != ESP_OK) {
        return err;
    }

    s_started = true;
    return ESP_OK;
#endif
}
