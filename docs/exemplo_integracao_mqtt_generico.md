# Exemplo de Integracao MQTT Generica

Este exemplo mostra o minimo que um produto novo precisa fornecer para usar o
nucleo MQTT generico.

## 1. Definir defaults do produto

Os componentes genericos usam defaults neutros, como `v1/device` e
`ESP32-Device`. Cada produto deve sobrescrever isso no `sdkconfig.defaults` do
projeto:

```text
CONFIG_MQTT_PRESENCE_ENABLED=y
CONFIG_MQTT_BASE_TOPIC="v1/meu-produto"
CONFIG_MQTT_DEVICE_ID_PREFIX="MeuProduto"
CONFIG_WIFI_PROVISIONING_SSID="MeuProduto"
```

## 2. Registrar settings

Settings sao registrados em `tele_config`. Campos com flag
`TELE_CONFIG_FLAG_MQTT` entram em `meta/config` automaticamente.

```c
static const tele_config_field_t s_product_config[] = {
    {
        .id = "product.sample_interval_s",
        .nvs_key = "p_sample",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = 60,
        .min.u32 = 5,
        .max.u32 = 3600,
        .flags = TELE_CONFIG_FLAG_MQTT | TELE_CONFIG_FLAG_WEB,
    },
};

ESP_ERROR_CHECK(tele_config_register_fields(
    s_product_config,
    sizeof(s_product_config) / sizeof(s_product_config[0])));
```

## 3. Registrar status

Status e read-only. Campos com `TELE_STATUS_FLAG_STATE` entram em `state`;
campos com `TELE_STATUS_FLAG_HEARTBEAT` entram em `heartbeat`; campos com
`TELE_STATUS_FLAG_MQTT` entram em `meta/status`.

```c
static uint32_t read_uptime_s(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_log_timestamp() / 1000);
}

static const tele_status_field_t s_product_status[] = {
    {
        .id = "uptime_s",
        .label = "Uptime",
        .description = "Tempo desde o boot.",
        .group = "runtime",
        .type = TELE_STATUS_TYPE_U32,
        .unit = "s",
        .flags = TELE_STATUS_FLAG_STATE |
                 TELE_STATUS_FLAG_HEARTBEAT |
                 TELE_STATUS_FLAG_MQTT,
        .read.u32 = read_uptime_s,
    },
};

ESP_ERROR_CHECK(tele_status_register_fields(
    s_product_status,
    sizeof(s_product_status) / sizeof(s_product_status[0])));
```

## 4. Iniciar MQTT

O nucleo MQTT monta `state`, `heartbeat`, `meta/config`, `meta/status` e
`meta/commands` usando os registries. O produto so precisa fornecer callbacks
quando houver regra especifica.

```c
static bool product_mqtt_ready(void *ctx)
{
    (void)ctx;
    return product_network_ready() && product_time_ready();
}

static void product_restart(uint32_t delay_ms, void *ctx)
{
    (void)delay_ms;
    (void)ctx;
    esp_restart();
}

tele_mqtt_config_t mqtt_config = {
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
    .is_ready = product_mqtt_ready,
    .build_timestamp = product_build_timestamp,
    .restart = product_restart,
};

ESP_ERROR_CHECK(tele_mqtt_start(&mqtt_config));
```

## 5. Adicionar comandos de produto

Comandos pontuais entram em `tele_commands`. A execucao fica no callback
`handle_command` de `tele_mqtt_config_t`.

Use comando para acoes. Use setting para estado configuravel persistivel.
