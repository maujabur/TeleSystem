#include "tele_mqtt.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef TELE_MQTT_HOST_TEST
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "tele_commands.h"
#include "tele_config.h"
#include "tele_core_commands.h"
#include "tele_status.h"
#else
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag))
#define ESP_LOGE(tag, fmt, ...) ((void)(tag))
typedef void *esp_mqtt_client_handle_t;
typedef int esp_mqtt_transport_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#endif

#define TELE_MQTT_TOPIC_BUF_SIZE 128
#define TELE_MQTT_DEVICE_ID_SIZE 64
#define TELE_MQTT_TS_BUF_SIZE 40
#define TELE_MQTT_JSON_BUF_SIZE 512
#define TELE_MQTT_CMD_IN_PAYLOAD_BUF_SIZE 1024
#define TELE_MQTT_SHARED_PAYLOAD_BUF_SIZE 1024
#define TELE_MQTT_HEARTBEAT_INTERVAL_MIN_S 15
#define TELE_MQTT_HEARTBEAT_INTERVAL_MAX_S 3600
#define TELE_MQTT_CONFIG_ID_HEARTBEAT_INTERVAL "mqtt.heartbeat_interval_s"
#define TELE_MQTT_SESSION_ID_SIZE 48
#define TELE_MQTT_START_RETRY_DELAY_MS 1000

#ifndef CONFIG_TELE_MQTT_MAX_SHARED_SUBSCRIPTIONS
#define CONFIG_TELE_MQTT_MAX_SHARED_SUBSCRIPTIONS 8
#endif

#ifndef CONFIG_TELE_MQTT_SHARED_TOPIC_SUFFIX_MAX_LEN
#define CONFIG_TELE_MQTT_SHARED_TOPIC_SUFFIX_MAX_LEN 64
#endif

static const char *TAG = "tele-mqtt";

static tele_mqtt_config_t s_config;
static bool s_connected;
static esp_mqtt_client_handle_t s_client;

#ifndef TELE_MQTT_HOST_TEST
static bool s_started;
static bool s_client_started;
static bool s_restart_requested;
static uint32_t s_restart_at_ms;
static uint32_t s_last_wait_log_ms;
static uint32_t s_heartbeat_interval_s;
static TaskHandle_t s_heartbeat_task;
static esp_mqtt_transport_t s_broker_transport;
static uint32_t s_broker_port;
static char s_broker_host[96];
static char s_device_id[TELE_MQTT_DEVICE_ID_SIZE];
static char s_session_id[TELE_MQTT_SESSION_ID_SIZE];
static char s_lwt_payload[TELE_MQTT_JSON_BUF_SIZE];
static char s_topic_availability[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_seen[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_heartbeat[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_state[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_event[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_meta_config[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_meta_status[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_meta_commands[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_cmd_in[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_cmd_out[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_cmd_in_payload_buf[TELE_MQTT_CMD_IN_PAYLOAD_BUF_SIZE];
#endif
static char s_shared_payload_buf[TELE_MQTT_SHARED_PAYLOAD_BUF_SIZE];

typedef struct {
    bool used;
    char suffix[CONFIG_TELE_MQTT_SHARED_TOPIC_SUFFIX_MAX_LEN + 1];
    char full_topic[TELE_MQTT_TOPIC_BUF_SIZE];
    int qos;
    tele_mqtt_shared_handler_t handler;
    void *ctx;
} tele_mqtt_shared_subscription_t;

static tele_mqtt_shared_subscription_t s_shared_subscriptions[CONFIG_TELE_MQTT_MAX_SHARED_SUBSCRIPTIONS];
static SemaphoreHandle_t s_shared_subscriptions_mutex;

#ifdef TELE_MQTT_HOST_TEST
static char s_host_last_subscribed_topic[TELE_MQTT_TOPIC_BUF_SIZE];
static int s_host_last_subscribed_qos;
static int s_host_subscribe_count;
static char s_host_last_published_topic[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_host_last_published_payload[TELE_MQTT_SHARED_PAYLOAD_BUF_SIZE];
static int s_host_last_published_qos;
static bool s_host_last_published_retain;
static int s_host_lock_take_count;
static int s_host_lock_give_count;
#endif

#ifndef TELE_MQTT_HOST_TEST
static void publish_config_manifest(void);
static void publish_commands_manifest(void);
#endif
static void subscribe_shared_topics(void);

#ifndef TELE_MQTT_HOST_TEST
static const char *str_or_empty(const char *value)
{
    return value ? value : "";
}

static const char *firmware_version(void)
{
    return s_config.firmware_version && s_config.firmware_version[0] != '\0' ?
           s_config.firmware_version : "unknown";
}
#endif

static const char *base_topic(void)
{
    return s_config.base_topic && s_config.base_topic[0] != '\0' ?
           s_config.base_topic : "v1/telesystem";
}

static bool mqtt_qos_is_valid(int qos)
{
    return qos >= 0 && qos <= 2;
}

static esp_err_t ensure_shared_subscriptions_mutex(void)
{
#ifdef TELE_MQTT_HOST_TEST
    if (!s_shared_subscriptions_mutex) {
        s_shared_subscriptions_mutex = (SemaphoreHandle_t)0x1;
    }
    return ESP_OK;
#else
    if (s_shared_subscriptions_mutex) {
        return ESP_OK;
    }

    s_shared_subscriptions_mutex = xSemaphoreCreateMutex();
    return s_shared_subscriptions_mutex ? ESP_OK : ESP_ERR_NO_MEM;
#endif
}

static esp_err_t lock_shared_subscriptions(void)
{
    esp_err_t err = ensure_shared_subscriptions_mutex();
    if (err != ESP_OK) {
        return err;
    }

#ifdef TELE_MQTT_HOST_TEST
    s_host_lock_take_count++;
    return ESP_OK;
#else
    return xSemaphoreTake(s_shared_subscriptions_mutex, portMAX_DELAY) == pdTRUE ?
           ESP_OK : ESP_FAIL;
#endif
}

static void unlock_shared_subscriptions(void)
{
    if (!s_shared_subscriptions_mutex) {
        return;
    }

#ifdef TELE_MQTT_HOST_TEST
    s_host_lock_give_count++;
#else
    xSemaphoreGive(s_shared_subscriptions_mutex);
#endif
}

static esp_err_t validate_shared_suffix(const char *topic_suffix)
{
    if (!topic_suffix || topic_suffix[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (topic_suffix[0] == '/') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(topic_suffix);
    if (len > CONFIG_TELE_MQTT_SHARED_TOPIC_SUFFIX_MAX_LEN || topic_suffix[len - 1] == '/') {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < len; ++i) {
        char c = topic_suffix[i];
        if (c == '+' || c == '#') {
            return ESP_ERR_INVALID_ARG;
        }
        if (c == '/' && topic_suffix[i + 1] == '/') {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

static esp_err_t build_shared_topic(const char *topic_suffix, char *out_topic, size_t out_topic_len)
{
    int written = 0;

    if (!out_topic || out_topic_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (validate_shared_suffix(topic_suffix) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(out_topic, out_topic_len, "%s/_shared/%s", base_topic(), topic_suffix);
    if (written < 0 || (size_t)written >= out_topic_len || (size_t)written >= TELE_MQTT_TOPIC_BUF_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static tele_mqtt_shared_subscription_t *find_shared_subscription_by_suffix(const char *topic_suffix)
{
    for (size_t i = 0; i < CONFIG_TELE_MQTT_MAX_SHARED_SUBSCRIPTIONS; ++i) {
        if (s_shared_subscriptions[i].used &&
            strcmp(s_shared_subscriptions[i].suffix, topic_suffix) == 0) {
            return &s_shared_subscriptions[i];
        }
    }
    return NULL;
}

static tele_mqtt_shared_subscription_t *find_shared_subscription_by_topic(const char *topic, size_t topic_len)
{
    for (size_t i = 0; i < CONFIG_TELE_MQTT_MAX_SHARED_SUBSCRIPTIONS; ++i) {
        tele_mqtt_shared_subscription_t *subscription = &s_shared_subscriptions[i];
        if (!subscription->used) {
            continue;
        }

        size_t full_topic_len = strlen(subscription->full_topic);
        if (full_topic_len == topic_len &&
            strncmp(topic, subscription->full_topic, topic_len) == 0) {
            return subscription;
        }
    }
    return NULL;
}

static tele_mqtt_shared_subscription_t *find_free_shared_subscription(void)
{
    for (size_t i = 0; i < CONFIG_TELE_MQTT_MAX_SHARED_SUBSCRIPTIONS; ++i) {
        if (!s_shared_subscriptions[i].used) {
            return &s_shared_subscriptions[i];
        }
    }
    return NULL;
}

static esp_err_t refresh_shared_subscription_topic(tele_mqtt_shared_subscription_t *subscription)
{
    if (!subscription || !subscription->used) {
        return ESP_ERR_INVALID_ARG;
    }

    return build_shared_topic(subscription->suffix,
                              subscription->full_topic,
                              sizeof(subscription->full_topic));
}

static esp_err_t refresh_shared_subscription_topics(void)
{
    esp_err_t lock_err = lock_shared_subscriptions();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    for (size_t i = 0; i < CONFIG_TELE_MQTT_MAX_SHARED_SUBSCRIPTIONS; ++i) {
        if (!s_shared_subscriptions[i].used) {
            continue;
        }

        esp_err_t err = refresh_shared_subscription_topic(&s_shared_subscriptions[i]);
        if (err != ESP_OK) {
            unlock_shared_subscriptions();
            return err;
        }
    }

    unlock_shared_subscriptions();
    return ESP_OK;
}

static int mqtt_subscribe_raw(const char *topic, int qos)
{
#ifdef TELE_MQTT_HOST_TEST
    snprintf(s_host_last_subscribed_topic, sizeof(s_host_last_subscribed_topic), "%s", topic ? topic : "");
    s_host_last_subscribed_qos = qos;
    s_host_subscribe_count++;
    return s_host_subscribe_count;
#else
    if (!s_client || !topic) {
        return -1;
    }
    return esp_mqtt_client_subscribe(s_client, topic, qos);
#endif
}

static int mqtt_publish_raw(const char *topic, const char *payload, int qos, bool retain)
{
#ifdef TELE_MQTT_HOST_TEST
    snprintf(s_host_last_published_topic, sizeof(s_host_last_published_topic), "%s", topic ? topic : "");
    snprintf(s_host_last_published_payload,
             sizeof(s_host_last_published_payload),
             "%s",
             payload ? payload : "");
    s_host_last_published_qos = qos;
    s_host_last_published_retain = retain;
    return 1;
#else
    if (!s_client || !topic || !payload) {
        return -1;
    }
    return esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
#endif
}

static void subscribe_shared_topic(tele_mqtt_shared_subscription_t *subscription)
{
    int msg_id = -1;
    char suffix[CONFIG_TELE_MQTT_SHARED_TOPIC_SUFFIX_MAX_LEN + 1] = {0};
    char full_topic[TELE_MQTT_TOPIC_BUF_SIZE] = {0};
    int qos = 0;

    if (!subscription) {
        return;
    }

    if (lock_shared_subscriptions() != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao travar registry de topicos compartilhados");
        return;
    }
    if (!subscription->used) {
        unlock_shared_subscriptions();
        return;
    }
    if (refresh_shared_subscription_topic(subscription) != ESP_OK) {
        unlock_shared_subscriptions();
        ESP_LOGW(TAG, "Falha ao atualizar topico compartilhado | suffix=%s", subscription->suffix);
        return;
    }
    snprintf(suffix, sizeof(suffix), "%s", subscription->suffix);
    snprintf(full_topic, sizeof(full_topic), "%s", subscription->full_topic);
    qos = subscription->qos;
    unlock_shared_subscriptions();

    ESP_LOGI(TAG,
             "shared subscribe suffix=%s topic=%s qos=%d",
             suffix,
             full_topic,
             qos);
    msg_id = mqtt_subscribe_raw(full_topic, qos);
    if (msg_id < 0) {
        ESP_LOGW(TAG,
                 "Falha ao assinar topico MQTT compartilhado | suffix=%s | topic=%s | qos=%d | rc=%d",
                 suffix,
                 full_topic,
                 qos,
                 msg_id);
    }
}

static void subscribe_shared_topics(void)
{
    if (refresh_shared_subscription_topics() != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao atualizar topicos compartilhados antes de assinar");
        return;
    }

    for (size_t i = 0; i < CONFIG_TELE_MQTT_MAX_SHARED_SUBSCRIPTIONS; ++i) {
        subscribe_shared_topic(&s_shared_subscriptions[i]);
    }
}

#ifdef TELE_MQTT_HOST_TEST
static bool dispatch_shared_topic_data(const char *topic,
                                       size_t topic_len,
                                       const char *payload,
                                       size_t payload_len)
{
    tele_mqtt_shared_handler_t handler = NULL;
    void *ctx = NULL;
    char full_topic[TELE_MQTT_TOPIC_BUF_SIZE] = {0};

    if (!topic || !payload) {
        return false;
    }
    if (topic_len >= TELE_MQTT_TOPIC_BUF_SIZE || payload_len >= sizeof(s_shared_payload_buf)) {
        ESP_LOGW(TAG,
                 "Payload/topico compartilhado grande demais | topic_len=%u | payload_len=%u",
                 (unsigned)topic_len,
                 (unsigned)payload_len);
        return false;
    }

    if (lock_shared_subscriptions() != ESP_OK) {
        return false;
    }

    tele_mqtt_shared_subscription_t *subscription = find_shared_subscription_by_topic(topic, topic_len);
    if (subscription) {
        handler = subscription->handler;
        ctx = subscription->ctx;
        snprintf(full_topic, sizeof(full_topic), "%s", subscription->full_topic);
    }
    unlock_shared_subscriptions();

    if (!handler) {
        return false;
    }

    memcpy(s_shared_payload_buf, payload, payload_len);
    s_shared_payload_buf[payload_len] = '\0';
    esp_err_t err = handler(full_topic, s_shared_payload_buf, payload_len, ctx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Handler de topico compartilhado retornou erro | topic=%s | err=%s",
                 full_topic,
                 esp_err_to_name(err));
    }
    return true;
}
#endif

esp_err_t tele_mqtt_subscribe_shared(const char *topic_suffix,
                                     int qos,
                                     tele_mqtt_shared_handler_t handler,
                                     void *ctx)
{
    char full_topic[TELE_MQTT_TOPIC_BUF_SIZE] = {0};
    tele_mqtt_shared_subscription_t *slot = NULL;
    esp_err_t lock_err = ESP_OK;
    bool should_subscribe = false;

    if (!handler || !mqtt_qos_is_valid(qos)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (build_shared_topic(topic_suffix, full_topic, sizeof(full_topic)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_err = lock_shared_subscriptions();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    if (find_shared_subscription_by_suffix(topic_suffix)) {
        unlock_shared_subscriptions();
        return ESP_ERR_INVALID_STATE;
    }

    slot = find_free_shared_subscription();
    if (!slot) {
        unlock_shared_subscriptions();
        return ESP_ERR_NO_MEM;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    snprintf(slot->suffix, sizeof(slot->suffix), "%s", topic_suffix);
    snprintf(slot->full_topic, sizeof(slot->full_topic), "%s", full_topic);
    slot->qos = qos;
    slot->handler = handler;
    slot->ctx = ctx;
    should_subscribe = s_connected;
    unlock_shared_subscriptions();

    ESP_LOGI(TAG, "shared register suffix=%s qos=%d", topic_suffix, qos);
    if (should_subscribe) {
        subscribe_shared_topic(slot);
    }

    return ESP_OK;
}

esp_err_t tele_mqtt_publish_shared(const char *topic_suffix,
                                   const char *payload,
                                   int qos,
                                   bool retain)
{
    char full_topic[TELE_MQTT_TOPIC_BUF_SIZE] = {0};
    int msg_id = -1;

    if (!payload || !mqtt_qos_is_valid(qos)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (build_shared_topic(topic_suffix, full_topic, sizeof(full_topic)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_connected || !s_client) {
        return ESP_ERR_INVALID_STATE;
    }

    msg_id = mqtt_publish_raw(full_topic, payload, qos, retain);
    if (msg_id < 0) {
        ESP_LOGW(TAG,
                 "Falha ao publicar topico MQTT compartilhado | topic=%s | qos=%d | retain=%d",
                 full_topic,
                 qos,
                 retain ? 1 : 0);
        return ESP_FAIL;
    }

    return ESP_OK;
}

#ifdef TELE_MQTT_HOST_TEST
void tele_mqtt_host_test_reset(void)
{
    memset(&s_config, 0, sizeof(s_config));
    memset(s_shared_subscriptions, 0, sizeof(s_shared_subscriptions));
    memset(s_host_last_subscribed_topic, 0, sizeof(s_host_last_subscribed_topic));
    memset(s_host_last_published_topic, 0, sizeof(s_host_last_published_topic));
    memset(s_host_last_published_payload, 0, sizeof(s_host_last_published_payload));
    s_connected = false;
    s_client = NULL;
    s_host_last_subscribed_qos = 0;
    s_host_subscribe_count = 0;
    s_host_last_published_qos = 0;
    s_host_last_published_retain = false;
    s_host_lock_take_count = 0;
    s_host_lock_give_count = 0;
    s_shared_subscriptions_mutex = NULL;
}

void tele_mqtt_host_test_set_base_topic(const char *base_topic_value)
{
    s_config.base_topic = base_topic_value;
}

void tele_mqtt_host_test_set_connected(bool connected)
{
    bool was_connected = s_connected;
    s_connected = connected;
    s_client = connected ? (esp_mqtt_client_handle_t)0x1 : NULL;
    if (connected && !was_connected) {
        subscribe_shared_topics();
    }
}

esp_err_t tele_mqtt_host_test_build_shared_topic(const char *topic_suffix,
                                                 char *out_topic,
                                                 size_t out_topic_len)
{
    return build_shared_topic(topic_suffix, out_topic, out_topic_len);
}

size_t tele_mqtt_host_test_shared_subscription_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < CONFIG_TELE_MQTT_MAX_SHARED_SUBSCRIPTIONS; ++i) {
        if (s_shared_subscriptions[i].used) {
            count++;
        }
    }
    return count;
}

int tele_mqtt_host_test_subscribe_count(void)
{
    return s_host_subscribe_count;
}

const char *tele_mqtt_host_test_last_subscribed_topic(void)
{
    return s_host_last_subscribed_topic;
}

int tele_mqtt_host_test_last_subscribed_qos(void)
{
    return s_host_last_subscribed_qos;
}

const char *tele_mqtt_host_test_last_published_topic(void)
{
    return s_host_last_published_topic;
}

const char *tele_mqtt_host_test_last_published_payload(void)
{
    return s_host_last_published_payload;
}

int tele_mqtt_host_test_last_published_qos(void)
{
    return s_host_last_published_qos;
}

bool tele_mqtt_host_test_last_published_retain(void)
{
    return s_host_last_published_retain;
}

bool tele_mqtt_host_test_dispatch_data(const char *topic, const char *payload, size_t payload_len)
{
    return dispatch_shared_topic_data(topic, strlen(topic), payload, payload_len);
}

int tele_mqtt_host_test_lock_take_count(void)
{
    return s_host_lock_take_count;
}

int tele_mqtt_host_test_lock_give_count(void)
{
    return s_host_lock_give_count;
}
#endif

#ifndef TELE_MQTT_HOST_TEST

static int qos_critical(void)
{
    return s_config.qos_critical >= 0 ? s_config.qos_critical : 1;
}

static int qos_telemetry(void)
{
    return s_config.qos_telemetry >= 0 ? s_config.qos_telemetry : 0;
}

static uint32_t keepalive_s(void)
{
    return s_config.keepalive_s > 0 ? s_config.keepalive_s : 60;
}

static bool is_ready(void)
{
    return !s_config.is_ready || s_config.is_ready(s_config.ctx);
}

static void normalize_id_prefix(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;

    if (!dst || dst_len == 0) {
        return;
    }

    dst[0] = '\0';

    if (!src || src[0] == '\0') {
        snprintf(dst, dst_len, "device");
        return;
    }

    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; ++si) {
        unsigned char c = (unsigned char)src[si];
        if (isalnum(c) || c == '-' || c == '_') {
            dst[di++] = (char)c;
            continue;
        }

        if (c == ' ' || c == '.') {
            dst[di++] = '-';
        }
    }

    if (di == 0) {
        snprintf(dst, dst_len, "device");
        return;
    }

    dst[di] = '\0';
}

static bool parse_broker_uri(const char *uri,
                             char *host,
                             size_t host_len,
                             uint32_t *port,
                             esp_mqtt_transport_t *transport)
{
    const char *host_start = NULL;
    const char *host_end = NULL;
    size_t host_size = 0;
    uint32_t parsed_port = 0;

    if (!uri || !host || host_len == 0 || !port || !transport) {
        return false;
    }

    if (strncmp(uri, "mqtts://", 8) == 0) {
        host_start = uri + 8;
        *transport = MQTT_TRANSPORT_OVER_SSL;
        parsed_port = 8883;
    } else if (strncmp(uri, "mqtt://", 7) == 0) {
        host_start = uri + 7;
        *transport = MQTT_TRANSPORT_OVER_TCP;
        parsed_port = 1883;
    } else {
        return false;
    }

    if (!host_start || *host_start == '\0') {
        return false;
    }

    host_end = host_start;
    while (*host_end != '\0' && *host_end != ':' && *host_end != '/') {
        host_end++;
    }

    host_size = (size_t)(host_end - host_start);
    if (host_size == 0 || host_size >= host_len) {
        return false;
    }

    memcpy(host, host_start, host_size);
    host[host_size] = '\0';

    if (*host_end == ':') {
        const char *port_start = host_end + 1;
        parsed_port = 0;
        while (*port_start >= '0' && *port_start <= '9') {
            parsed_port = (parsed_port * 10u) + (uint32_t)(*port_start - '0');
            port_start++;
        }
        if (parsed_port == 0) {
            return false;
        }
    }

    *port = parsed_port;
    return true;
}

static void build_timestamp(char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0) {
        return;
    }

    if (!s_config.build_timestamp ||
        !s_config.build_timestamp(buffer, buffer_len, s_config.ctx)) {
        snprintf(buffer, buffer_len, "1970-01-01T00:00:00Z");
    }
}

static void ensure_session_id(void)
{
    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    time_t now = 0;
    struct tm now_tm = {0};

    if (s_session_id[0] != '\0') {
        return;
    }

    time(&now);
    if (now > 0 && gmtime_r(&now, &now_tm) != NULL && (now_tm.tm_year + 1900) >= 2024) {
        snprintf(s_session_id,
                 sizeof(s_session_id),
                 "%04d%02d%02dT%02d%02d%02dZ-%02X%02X%02X",
                 now_tm.tm_year + 1900,
                 now_tm.tm_mon + 1,
                 now_tm.tm_mday,
                 now_tm.tm_hour,
                 now_tm.tm_min,
                 now_tm.tm_sec,
                 mac_err == ESP_OK ? (unsigned)mac[3] : 0u,
                 mac_err == ESP_OK ? (unsigned)mac[4] : 0u,
                 mac_err == ESP_OK ? (unsigned)mac[5] : 0u);
        return;
    }

    snprintf(s_session_id,
             sizeof(s_session_id),
             "boot-%08" PRIX32 "-%02X%02X%02X",
             esp_log_timestamp(),
             mac_err == ESP_OK ? (unsigned)mac[3] : 0u,
             mac_err == ESP_OK ? (unsigned)mac[4] : 0u,
             mac_err == ESP_OK ? (unsigned)mac[5] : 0u);
}

static void generate_device_id(void)
{
    uint8_t mac[6] = {0};
    char prefix[40] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);

    normalize_id_prefix(prefix, sizeof(prefix), s_config.device_id_prefix);

    if (err == ESP_OK) {
        snprintf(s_device_id,
                 sizeof(s_device_id),
                 "%s-%02X%02X%02X",
                 prefix,
                 (unsigned)mac[3],
                 (unsigned)mac[4],
                 (unsigned)mac[5]);
    } else {
        snprintf(s_device_id, sizeof(s_device_id), "%s-UNKNOWN", prefix);
    }

    ESP_LOGI(TAG, "Device ID MQTT: %s (prefixo=%s)", s_device_id, prefix);
}

static void build_topics(void)
{
    snprintf(s_topic_availability,
             sizeof(s_topic_availability),
             "%s/%s/availability",
             base_topic(),
             s_device_id);
    snprintf(s_topic_seen, sizeof(s_topic_seen), "%s/%s/seen", base_topic(), s_device_id);
    snprintf(s_topic_heartbeat, sizeof(s_topic_heartbeat), "%s/%s/heartbeat", base_topic(), s_device_id);
    snprintf(s_topic_state, sizeof(s_topic_state), "%s/%s/state", base_topic(), s_device_id);
    snprintf(s_topic_event, sizeof(s_topic_event), "%s/%s/event", base_topic(), s_device_id);
    snprintf(s_topic_meta_config, sizeof(s_topic_meta_config), "%s/%s/meta/config", base_topic(), s_device_id);
    snprintf(s_topic_meta_status, sizeof(s_topic_meta_status), "%s/%s/meta/status", base_topic(), s_device_id);
    snprintf(s_topic_meta_commands,
             sizeof(s_topic_meta_commands),
             "%s/%s/meta/commands",
             base_topic(),
             s_device_id);
    snprintf(s_topic_cmd_in, sizeof(s_topic_cmd_in), "%s/%s/cmd/in", base_topic(), s_device_id);
    snprintf(s_topic_cmd_out, sizeof(s_topic_cmd_out), "%s/%s/cmd/out", base_topic(), s_device_id);
}

static int publish_if_connected(const char *topic, const char *payload, int qos, int retain)
{
    if (!s_connected || !s_client || !topic || !payload) {
        return -1;
    }

    return esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
}

static void add_common_fields(cJSON *root, const char *ts)
{
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "fw", firmware_version());
    cJSON_AddStringToObject(root, "session_id", s_session_id[0] != '\0' ? s_session_id : "unknown");
    if (ts) {
        cJSON_AddStringToObject(root, "ts", ts);
    }
}

static int publish_json(const char *topic, cJSON *payload, int qos, int retain)
{
    char *payload_text = NULL;
    int msg_id = -1;

    if (!payload) {
        return -1;
    }

    payload_text = cJSON_PrintUnformatted(payload);
    if (payload_text) {
        msg_id = publish_if_connected(topic, payload_text, qos, retain);
        if (msg_id < 0) {
            ESP_LOGW(TAG,
                     "Falha ao publicar MQTT | topic=%s | qos=%d | retain=%d",
                     topic ? topic : "(null)",
                     qos,
                     retain);
        }
        cJSON_free(payload_text);
    }
    return msg_id;
}

static int publish_json_with_common(const char *topic, cJSON *payload, int qos, int retain)
{
    char ts[TELE_MQTT_TS_BUF_SIZE] = {0};

    if (!payload) {
        return -1;
    }

    build_timestamp(ts, sizeof(ts));
    add_common_fields(payload, ts);
    return publish_json(topic, payload, qos, retain);
}

static void publish_command_reply(const char *cmd_id, bool ok, const char *error, const cJSON *result)
{
    char ts[TELE_MQTT_TS_BUF_SIZE] = {0};
    cJSON *root = cJSON_CreateObject();
    char *payload = NULL;
    int msg_id = -1;

    if (!root) {
        return;
    }

    build_timestamp(ts, sizeof(ts));
    add_common_fields(root, ts);
    cJSON_AddStringToObject(root, "cmd_id", cmd_id ? cmd_id : "unknown");
    cJSON_AddBoolToObject(root, "ok", ok);
    if (error && error[0] != '\0') {
        cJSON_AddStringToObject(root, "error", error);
    }

    if (result) {
        cJSON *dup = cJSON_Duplicate((cJSON *)result, 1);
        if (dup) {
            cJSON_AddItemToObject(root, "result", dup);
        }
    }

    payload = cJSON_PrintUnformatted(root);
    if (!payload) {
        ESP_LOGE(TAG, "Falha ao serializar resposta de comando | cmd_id=%s", cmd_id ? cmd_id : "unknown");
        cJSON_Delete(root);
        return;
    }

    msg_id = publish_if_connected(s_topic_cmd_out, payload, qos_critical(), 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG,
                 "Falha ao publicar cmd/out | cmd_id=%s | ok=%d | len=%u | rc=%d",
                 cmd_id ? cmd_id : "unknown",
                 ok,
                 (unsigned)strlen(payload),
                 msg_id);
    } else {
        ESP_LOGI(TAG,
                 "cmd/out publicado | cmd_id=%s | ok=%d | len=%u | msg_id=%d",
                 cmd_id ? cmd_id : "unknown",
                 ok,
                 (unsigned)strlen(payload),
                 msg_id);
    }

    cJSON_free(payload);
    cJSON_Delete(root);
}

static void schedule_restart(uint32_t delay_ms)
{
    uint32_t bounded_delay_ms = delay_ms;

    if (bounded_delay_ms < 100) {
        bounded_delay_ms = 100;
    }
    if (bounded_delay_ms > 10000) {
        bounded_delay_ms = 10000;
    }

    s_restart_requested = true;
    s_restart_at_ms = esp_log_timestamp() + bounded_delay_ms;
    ESP_LOGW(TAG, "Reboot agendado via MQTT em %u ms", (unsigned)bounded_delay_ms);
}

static cJSON *build_status_fields_payload(tele_status_flags_t flags)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return NULL;
    }

    if (tele_status_add_fields_to_json(result, TELE_CHANNEL_FLAG_MQTT, flags) != ESP_OK) {
        cJSON_Delete(result);
        return NULL;
    }

    return result;
}

static cJSON *build_default_config_manifest(void)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return NULL;
    }

    if (tele_config_add_manifest_to_json(result, TELE_CHANNEL_FLAG_MQTT) != ESP_OK) {
        cJSON_Delete(result);
        return NULL;
    }

    return result;
}

static cJSON *build_default_status_manifest(void)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return NULL;
    }

    if (tele_status_add_manifest_to_json(result, TELE_CHANNEL_FLAG_MQTT) != ESP_OK) {
        cJSON_Delete(result);
        return NULL;
    }

    return result;
}

static cJSON *build_state_payload(void)
{
    return s_config.build_state ?
           s_config.build_state(s_config.ctx) :
           build_status_fields_payload(TELE_STATUS_FLAG_STATE);
}

static cJSON *build_heartbeat_payload(void)
{
    return s_config.build_heartbeat ?
           s_config.build_heartbeat(s_config.ctx) :
           build_status_fields_payload(TELE_STATUS_FLAG_HEARTBEAT);
}

static cJSON *build_config_manifest_payload(void)
{
    return s_config.build_config_manifest ?
           s_config.build_config_manifest(s_config.ctx) :
           build_default_config_manifest();
}

static cJSON *build_status_manifest_payload(void)
{
    return s_config.build_status_manifest ?
           s_config.build_status_manifest(s_config.ctx) :
           build_default_status_manifest();
}

static cJSON *build_core_state(void *ctx)
{
    (void)ctx;
    return build_state_payload();
}

static cJSON *build_core_config_manifest(void *ctx)
{
    (void)ctx;
    return build_config_manifest_payload();
}

static cJSON *build_core_technical_status(void *ctx)
{
    (void)ctx;
    return s_config.build_technical_status ?
           s_config.build_technical_status(s_config.ctx) : cJSON_CreateObject();
}

static void handle_core_config_changed(void *ctx)
{
    (void)ctx;
    publish_config_manifest();
}

static void handle_core_restart(uint32_t delay_ms, void *ctx)
{
    (void)ctx;
    schedule_restart(delay_ms);
}

static void handle_command_payload(const char *payload)
{
    cJSON *root = NULL;
    cJSON *cmd_id_item = NULL;
    cJSON *name_item = NULL;
    cJSON *args = NULL;
    const char *cmd_id = "unknown";
    const char *cmd_name = NULL;
    tele_command_response_t response = {0};

    if (!payload) {
        return;
    }

    root = cJSON_Parse(payload);
    if (!root) {
        publish_command_reply("unknown", false, "invalid_json", NULL);
        return;
    }

    cmd_id_item = cJSON_GetObjectItemCaseSensitive(root, "cmd_id");
    if (cJSON_IsString(cmd_id_item) && cmd_id_item->valuestring) {
        cmd_id = cmd_id_item->valuestring;
    }

    name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name_item) || !name_item->valuestring) {
        cJSON_Delete(root);
        publish_command_reply(cmd_id, false, "missing_name", NULL);
        return;
    }
    cmd_name = name_item->valuestring;
    args = cJSON_GetObjectItemCaseSensitive(root, "args");
    ESP_LOGI(TAG, "Comando MQTT recebido | name=%s | cmd_id=%s", cmd_name, cmd_id);

    const tele_command_request_t request = {
        .cmd_id = cmd_id,
        .name = cmd_name,
        .args = args,
        .required_channel_flags = TELE_CHANNEL_FLAG_MQTT,
    };
    esp_err_t err = tele_commands_execute(&request, &response);
    if (err == ESP_OK) {
        publish_command_reply(cmd_id, response.ok, response.error, response.result);
        tele_commands_response_cleanup(&response);
        cJSON_Delete(root);
        return;
    }

    cJSON_Delete(root);
    publish_command_reply(cmd_id, false, "command_dispatch_failed", NULL);
}

static void publish_availability(const char *status_text, const char *reason)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return;
    }

    cJSON_AddStringToObject(payload, "status", status_text ? status_text : "unknown");
    cJSON_AddStringToObject(payload, "reason", reason ? reason : "runtime");
    publish_json_with_common(s_topic_availability, payload, qos_critical(), 1);
    cJSON_Delete(payload);
}

static void publish_state_snapshot(void)
{
    cJSON *payload = build_state_payload();
    if (!payload) {
        return;
    }

    publish_json_with_common(s_topic_state, payload, qos_critical(), 1);
    cJSON_Delete(payload);
}

static void publish_status_manifest(void)
{
    cJSON *payload = build_status_manifest_payload();
    if (!payload) {
        return;
    }

    publish_json_with_common(s_topic_meta_status, payload, qos_critical(), 1);
    cJSON_Delete(payload);
}

static void publish_config_manifest(void)
{
    cJSON *payload = build_config_manifest_payload();
    if (!payload) {
        return;
    }

    publish_json_with_common(s_topic_meta_config, payload, qos_critical(), 1);
    cJSON_Delete(payload);
}

static void publish_commands_manifest(void)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return;
    }

    if (tele_commands_add_manifest_to_json(payload, TELE_CHANNEL_FLAG_MQTT) == ESP_OK) {
        publish_json_with_common(s_topic_meta_commands, payload, qos_critical(), 1);
    }
    cJSON_Delete(payload);
}

static void publish_seen(const char *reason)
{
    char ts[TELE_MQTT_TS_BUF_SIZE] = {0};
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return;
    }

    build_timestamp(ts, sizeof(ts));
    add_common_fields(payload, ts);
    cJSON_AddStringToObject(payload, "last_seen_ts", ts);
    cJSON_AddStringToObject(payload, "reason", reason ? reason : "runtime");
    publish_json(s_topic_seen, payload, qos_critical(), 1);
    cJSON_Delete(payload);
}

static void publish_heartbeat(void)
{
    cJSON *payload = build_heartbeat_payload();
    if (!payload) {
        return;
    }

    publish_json_with_common(s_topic_heartbeat, payload, qos_telemetry(), 0);
    cJSON_Delete(payload);
    publish_seen("heartbeat");
}

esp_err_t tele_mqtt_publish_event(const char *event_name, const char *message)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(payload, "event", event_name ? event_name : "event");
    cJSON_AddStringToObject(payload, "message", message ? message : "");
    publish_json_with_common(s_topic_event, payload, qos_critical(), 0);
    cJSON_Delete(payload);
    return ESP_OK;
}

static void heartbeat_task(void *arg)
{
    (void)arg;

    while (true) {
        if (s_restart_requested) {
            uint32_t now_ms = esp_log_timestamp();
            if (now_ms >= s_restart_at_ms) {
                ESP_LOGW(TAG, "Executando reboot agendado via MQTT");
                if (s_config.restart) {
                    s_config.restart(0, s_config.ctx);
                } else {
                    esp_restart();
                }
            }
        }

        if (!s_client_started) {
            (void)tele_mqtt_start_client_if_ready();
            vTaskDelay(pdMS_TO_TICKS(TELE_MQTT_START_RETRY_DELAY_MS));
            continue;
        }

        if (s_connected) {
            publish_heartbeat();
        }
        vTaskDelay(pdMS_TO_TICKS(s_heartbeat_interval_s * 1000));
    }
}

static bool handle_shared_event_data(esp_mqtt_event_handle_t event)
{
    tele_mqtt_shared_subscription_t *subscription = NULL;
    tele_mqtt_shared_handler_t handler = NULL;
    void *ctx = NULL;
    char full_topic[TELE_MQTT_TOPIC_BUF_SIZE] = {0};
    int total_len = 0;
    int offset = 0;

    if (!event || !event->topic || !event->data || event->topic_len <= 0 || event->data_len < 0) {
        return false;
    }

    if (lock_shared_subscriptions() != ESP_OK) {
        return false;
    }

    subscription = find_shared_subscription_by_topic(event->topic, (size_t)event->topic_len);
    if (!subscription) {
        unlock_shared_subscriptions();
        return false;
    }
    handler = subscription->handler;
    ctx = subscription->ctx;
    snprintf(full_topic, sizeof(full_topic), "%s", subscription->full_topic);
    unlock_shared_subscriptions();

    total_len = event->total_data_len > 0 ? event->total_data_len : event->data_len;
    offset = event->current_data_offset;
    if (total_len <= 0 || offset < 0 ||
        (size_t)total_len >= sizeof(s_shared_payload_buf) ||
        (size_t)(offset + event->data_len) >= sizeof(s_shared_payload_buf)) {
        ESP_LOGW(TAG,
                 "Payload compartilhado invalido/grande demais | topic=%s | total=%d | offset=%d | len=%d",
                 full_topic,
                 total_len,
                 offset,
                 event->data_len);
        return true;
    }

    if (offset == 0) {
        memset(s_shared_payload_buf, 0, sizeof(s_shared_payload_buf));
    }

    memcpy(s_shared_payload_buf + offset, event->data, (size_t)event->data_len);
    if (offset + event->data_len >= total_len) {
        s_shared_payload_buf[total_len] = '\0';
        esp_err_t err = handler(full_topic,
                                s_shared_payload_buf,
                                (size_t)total_len,
                                ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Handler de topico compartilhado retornou erro | topic=%s | err=%s",
                     full_topic,
                     esp_err_to_name(err));
        }
    }

    return true;
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT conectado | broker=%s", str_or_empty(s_config.broker_uri));
        esp_mqtt_client_subscribe(s_client, s_topic_cmd_in, qos_critical());
        subscribe_shared_topics();
        publish_availability("online", "mqtt_connected");
        publish_config_manifest();
        publish_status_manifest();
        publish_commands_manifest();
        publish_state_snapshot();
        publish_heartbeat();
        (void)tele_mqtt_publish_event("boot", "mqtt_online");
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT desconectado");
        break;

    case MQTT_EVENT_DATA:
        if (handle_shared_event_data(event)) {
            break;
        }
        if (event && event->topic && event->data &&
            event->topic_len == (int)strlen(s_topic_cmd_in) &&
            strncmp(event->topic, s_topic_cmd_in, event->topic_len) == 0) {
            int total_len = event->total_data_len > 0 ? event->total_data_len : event->data_len;
            int offset = event->current_data_offset;

            if (total_len <= 0 || offset < 0 || event->data_len < 0) {
                break;
            }

            if ((size_t)total_len >= sizeof(s_cmd_in_payload_buf)) {
                if (offset == 0) {
                    publish_command_reply("unknown", false, "payload_too_large", NULL);
                }
                break;
            }

            if (offset == 0) {
                memset(s_cmd_in_payload_buf, 0, sizeof(s_cmd_in_payload_buf));
            }

            if ((size_t)(offset + event->data_len) >= sizeof(s_cmd_in_payload_buf)) {
                publish_command_reply("unknown", false, "payload_too_large", NULL);
                break;
            }

            memcpy(s_cmd_in_payload_buf + offset, event->data, (size_t)event->data_len);
            if (offset + event->data_len >= total_len) {
                s_cmd_in_payload_buf[total_len] = '\0';
                handle_command_payload(s_cmd_in_payload_buf);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        if (event && event->error_handle) {
            ESP_LOGW(TAG,
                     "MQTT erro | type=%d | connect_rc=%d | tls_esp_err=%s(0x%x) | tls_stack_err=0x%x | cert_flags=0x%x | sock_errno=%d",
                     (int)event->error_handle->error_type,
                     (int)event->error_handle->connect_return_code,
                     esp_err_to_name(event->error_handle->esp_tls_last_esp_err),
                     (unsigned)event->error_handle->esp_tls_last_esp_err,
                     (unsigned)event->error_handle->esp_tls_stack_err,
                     (unsigned)event->error_handle->esp_tls_cert_verify_flags,
                     event->error_handle->esp_transport_sock_errno);
        } else {
            ESP_LOGW(TAG, "MQTT erro de transporte/protocolo (sem detalhes)");
        }
        break;

    default:
        break;
    }
}

esp_err_t tele_mqtt_start_client_if_ready(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {0};

    if (s_client_started) {
        return ESP_OK;
    }

    if (!is_ready()) {
        uint32_t now_ms = esp_log_timestamp();
        if (s_last_wait_log_ms == 0 || (now_ms - s_last_wait_log_ms) >= 10000) {
            ESP_LOGI(TAG, "Aguardando prerequisitos para iniciar MQTT");
            s_last_wait_log_ms = now_ms;
        }
        return ESP_OK;
    }

    ensure_session_id();

    snprintf(s_lwt_payload,
             sizeof(s_lwt_payload),
             "{\"device_id\":\"%s\",\"fw\":\"%s\",\"session_id\":\"%s\",\"status\":\"offline\",\"reason\":\"lwt\"}",
             s_device_id,
             firmware_version(),
             s_session_id);

    mqtt_cfg.broker.address.hostname = s_broker_host;
    mqtt_cfg.broker.address.port = s_broker_port;
    mqtt_cfg.broker.address.transport = s_broker_transport;
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt_cfg.session.keepalive = keepalive_s();
    mqtt_cfg.session.last_will.topic = s_topic_availability;
    mqtt_cfg.session.last_will.msg = s_lwt_payload;
    mqtt_cfg.session.last_will.qos = qos_critical();
    mqtt_cfg.session.last_will.retain = true;

    if (s_config.username && s_config.username[0] != '\0') {
        mqtt_cfg.credentials.username = s_config.username;
    }
    if (s_config.password && s_config.password[0] != '\0') {
        mqtt_cfg.credentials.authentication.password = s_config.password;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Falha ao inicializar cliente MQTT");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client,
                                                   ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler,
                                                   NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar handler MQTT: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar cliente MQTT: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    s_client_started = true;
    ESP_LOGI(TAG,
             "Cliente MQTT iniciado | host=%s | port=%" PRIu32 " | session_id=%s",
             s_broker_host,
             s_broker_port,
             s_session_id);
    return ESP_OK;
}

esp_err_t tele_mqtt_start(const tele_mqtt_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_OK;
    }
    if (!config->broker_uri || config->broker_uri[0] == '\0') {
        ESP_LOGW(TAG, "MQTT habilitado sem broker URI; modulo nao iniciado");
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;
    esp_err_t shared_topics_err = refresh_shared_subscription_topics();
    if (shared_topics_err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao atualizar topicos compartilhados: %s", esp_err_to_name(shared_topics_err));
        return shared_topics_err;
    }
    s_heartbeat_interval_s = config->heartbeat_interval_s > 0 ? config->heartbeat_interval_s : 60;
    const tele_core_commands_config_t commands_config = {
        .build_state = build_core_state,
        .build_config_manifest = build_core_config_manifest,
        .build_technical_status = build_core_technical_status,
        .config_changed = handle_core_config_changed,
        .restart = handle_core_restart,
    };
    esp_err_t commands_err = tele_core_commands_register(&commands_config);
    if (commands_err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar comandos MQTT: %s", esp_err_to_name(commands_err));
        return commands_err;
    }
    generate_device_id();
    build_topics();

    if (!parse_broker_uri(config->broker_uri,
                          s_broker_host,
                          sizeof(s_broker_host),
                          &s_broker_port,
                          &s_broker_transport)) {
        ESP_LOGE(TAG,
                 "broker_uri invalida: %s (esperado mqtt://host:port ou mqtts://host:port)",
                 config->broker_uri);
        return ESP_ERR_INVALID_ARG;
    }

    BaseType_t task_ok = xTaskCreate(heartbeat_task,
                                     "tele_mqtt",
                                     4096,
                                     NULL,
                                     tskIDLE_PRIORITY + 1,
                                     &s_heartbeat_task);
    if (task_ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    (void)tele_mqtt_start_client_if_ready();
    ESP_LOGI(TAG,
             "MQTT inicializado | device_id=%s | base_topic=%s | broker=%s:%" PRIu32,
             s_device_id,
             base_topic(),
             s_broker_host,
             s_broker_port);
    return ESP_OK;
}

uint32_t tele_mqtt_get_heartbeat_interval_s(void)
{
    return s_heartbeat_interval_s;
}

esp_err_t tele_mqtt_apply_heartbeat_interval_s(uint32_t interval_s)
{
    if (interval_s < TELE_MQTT_HEARTBEAT_INTERVAL_MIN_S ||
        interval_s > TELE_MQTT_HEARTBEAT_INTERVAL_MAX_S) {
        return ESP_ERR_INVALID_ARG;
    }
    s_heartbeat_interval_s = interval_s;
    return ESP_OK;
}

bool tele_mqtt_is_connected(void)
{
    return s_connected;
}
#endif
