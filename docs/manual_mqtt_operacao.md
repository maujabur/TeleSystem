# Manual de Operacao MQTT

Guia do grupo: [componentes_mqtt_config_status_commands.md](componentes_mqtt_config_status_commands.md).

## Objetivo

O firmware publica presenca, estado e telemetria via MQTT e aceita comandos
remotos simples. A implementacao reutilizavel vive em `components/tele_mqtt`;
`components/tele_presence/tele_presence.c` inicializa o transporte MQTT do
produto e `components/tele_system_registry` injeta os dados atuais do
TeleSystem.

## API publica de integracao

Um produto novo integra o nucleo MQTT preenchendo `tele_mqtt_config_t` e
chamando `tele_mqtt_start()`. O contrato minimo e:

```c
tele_mqtt_config_t config = {
    .broker_uri = CONFIG_MQTT_BROKER_URI,
    .base_topic = CONFIG_MQTT_BASE_TOPIC,
    .device_id_prefix = CONFIG_MQTT_DEVICE_ID_PREFIX,
    .firmware_version = APP_VERSION_STRING,
    .heartbeat_interval_s = CONFIG_MQTT_HEARTBEAT_INTERVAL_S,
    .keepalive_s = CONFIG_MQTT_KEEPALIVE_S,
    .qos_critical = CONFIG_MQTT_QOS_CRITICAL,
    .qos_telemetry = CONFIG_MQTT_QOS_TELEMETRY,
    .is_ready = produto_mqtt_ready,
    .build_timestamp = produto_build_timestamp,
    .build_technical_status = produto_build_technical_status,
    .restart = produto_restart,
};
```

Somente `broker_uri` e indispensavel para configurar o cliente. Na pratica,
projetos devem informar tambem `base_topic`, `device_id_prefix` e
`firmware_version` para manter identificacao consistente.

Callbacks opcionais:

- `is_ready`: atrasa o inicio do MQTT ate rede, hora ou outro prerequisito
  estar pronto; se ausente, o nucleo tenta iniciar imediatamente;
- `build_timestamp`: gera timestamps reais; se ausente ou falhar, o nucleo usa
  `1970-01-01T00:00:00Z`;
- `build_technical_status`: adiciona diagnostico especifico de produto ao
  comando `get_technical_status`;
- `restart`: executa reboot especifico do produto; se ausente, usa
  `esp_restart()`.

Os builders `build_state`, `build_heartbeat`, `build_config_manifest` e
`build_status_manifest` tambem sao opcionais. Quando ficam `NULL`, `tele_mqtt`
gera os payloads a partir dos registries `tele_status` e `tele_config`. Esse e
o caminho recomendado para projetos novos.

## Implementando em outro projeto do zero

Para reaproveitar este sistema em outro firmware, separe mentalmente duas
camadas:

- componentes base e reutilizaveis: `tele_channels`, `tele_config`,
  `tele_status`, `tele_commands`, `tele_core_commands`, `tele_mqtt`,
  `tele_manifest`, `tele_artifacts`, `tele_portal_ota` e
  `tele_portal_core`;
- componentes especificos deste produto: `tele_presence` e
  `tele_system_registry`.

Em um projeto novo, comece pelos componentes base. Use `tele_presence` apenas
como exemplo de bootstrap MQTT e crie um registry proprio para os status,
settings e handlers do seu produto.

Escolha o perfil de integracao antes de copiar arquivos:

- **MQTT minimo:** config/status/commands via MQTT, sem OTA remoto.
- **MQTT + updates por manifest:** adiciona comandos `artifacts/get`,
  `artifact/check`, `artifact/apply` e `artifact/status`.
- **Portal web + upload OTA local:** adiciona servidor HTTP, assets e o binding
  `tele_firmware_portal_ota`.

Fluxo esperado de inicializacao em um produto novo:

```c
ESP_ERROR_CHECK(nvs_flash_init());
ESP_ERROR_CHECK(esp_event_loop_create_default());
ESP_ERROR_CHECK(esp_netif_init());

ESP_ERROR_CHECK(product_network_start());

ESP_ERROR_CHECK(product_register_config());
ESP_ERROR_CHECK(product_register_status());
ESP_ERROR_CHECK(product_register_commands());

/* Somente se usar OTA/updates por manifest. */
ESP_ERROR_CHECK(firmware_ota_init());
ESP_ERROR_CHECK(firmware_ota_register_artifact());
ESP_ERROR_CHECK(tele_artifacts_register_commands());

ESP_ERROR_CHECK(product_telemetry_start());
ESP_ERROR_CHECK(web_portal_start(false));          /* se usar portal HTTP */
```

Regra de ouro: componentes `tele_*` genericos nao devem conhecer sensores,
atuadores, GPIOs, nomes comerciais ou politica de produto. O projeto novo deve
fornecer esses detalhes por registries e callbacks.

### 1. Leve os componentes necessarios

Para o perfil MQTT minimo, copie ou inclua no workspace do novo projeto:

```text
components/tele_channels
components/tele_config
components/tele_status
components/tele_commands
components/tele_core_commands
components/tele_mqtt
managed_components/espressif__cjson
managed_components/espressif__mqtt
```

Para MQTT + updates por manifest, adicione:

```text
components/tele_manifest
components/tele_artifacts
components/tele_system
```

`tele_system` fornece `firmware_ota`. Se o novo projeto nao quiser levar bateria,
power-good ou outros arquivos de sistema, mantenha pelo menos
`firmware_ota.c`, `firmware_ota.h` e um `firmware_version.h` proprio.

Para portal web + upload OTA local, adicione:

```text
components/tele_portal
components/tele_portal_assets
components/tele_portal_captive
components/tele_portal_config
components/tele_portal_core
components/tele_portal_logs
components/tele_portal_ota
components/tele_portal_status
components/tele_portal_wifi
components/tele_firmware_portal_ota
components/tele_wifi
firmware_assets/web
```

Se o produto tiver outro backend de update, use `tele_portal_ota` diretamente
com callbacks proprios e nao inclua `tele_firmware_portal_ota`.

Se o projeto usar o gerenciador de componentes do ESP-IDF, `cJSON` e `mqtt`
podem vir como dependencias em vez de serem copiados como `managed_components`.

No `CMakeLists.txt` do componente que faz o bootstrap do produto, declare pelo
menos:

```cmake
idf_component_register(
    SRCS "app_telemetry.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_netif nvs_flash
             tele_channels tele_config tele_status tele_commands tele_mqtt
             espressif__cjson
)
```

`tele_mqtt` ja depende de `tele_core_commands`, portanto os comandos base
`ping`, `get_state`, `get_technical_status`, `config/get`, `commands/get`,
`config/set`, `config/reset` e `apply_and_reboot` sao registrados durante
`tele_mqtt_start()`.

Os snippets abaixo pressupõem os includes usuais do modulo de bootstrap:
`string.h`, `cJSON.h`, `esp_check.h`, `esp_event.h`, `esp_netif.h`,
`nvs_flash.h`, `tele_config.h`, `tele_status.h`, `tele_commands.h` e
`tele_mqtt.h`.

### 2. Inicialize a base do ESP-IDF

Antes de registrar settings ou iniciar MQTT, inicialize o que o seu produto
usa como infraestrutura:

```c
ESP_ERROR_CHECK(nvs_flash_init());
ESP_ERROR_CHECK(esp_event_loop_create_default());
ESP_ERROR_CHECK(esp_netif_init());

/* Inicialize Wi-Fi, Ethernet, modem ou outro transporte IP do produto. */
```

`tele_config` usa NVS para overrides. `tele_mqtt` so deve conectar quando o
produto tiver rede pronta; use o callback `is_ready` para bloquear o start ate
esse prerequisito estar satisfeito.

### 3. Registre settings reutilizaveis

Cada setting e um `tele_config_field_t`. Use `channel_flags` para dizer por
quais transportes ele aparece e `flags` apenas para comportamento do campo.

```c
static const tele_config_field_t s_product_config[] = {
    {
        .id = "pump.target_temp_c",
        .nvs_key = "p_temp",
        .type = TELE_CONFIG_TYPE_I32,
        .default_value.i32 = 92,
        .min.i32 = 70,
        .max.i32 = 100,
        .channel_flags = TELE_CHANNEL_FLAG_MQTT | TELE_CHANNEL_FLAG_WEB,
        .flags = TELE_CONFIG_FLAG_REBOOT_REQUIRED,
    },
};

static esp_err_t apply_product_config(const tele_config_field_t *field,
                                      const tele_config_value_t *value,
                                      void *ctx)
{
    (void)ctx;

    if (strcmp(field->id, "pump.target_temp_c") == 0) {
        return product_set_target_temp(value->i32);
    }
    return ESP_OK;
}

static esp_err_t product_register_config(void)
{
    ESP_RETURN_ON_ERROR(tele_config_register_fields(
                            s_product_config,
                            sizeof(s_product_config) / sizeof(s_product_config[0])),
                        TAG,
                        "falha ao registrar config");

    return tele_config_set_apply_handler("pump.target_temp_c",
                                         apply_product_config,
                                         NULL);
}
```

Quando um comando `config/set` ou `config/reset` chega por MQTT, o nucleo valida
o tipo, grava o override em NVS e chama o apply handler quando existir.

### 4. Registre status e telemetria

Cada status e um `tele_status_field_t`. Os flags `STATE`, `HEARTBEAT` e
`TECHNICAL` definem em quais payloads o campo aparece.

```c
static int32_t read_target_temp(void *ctx)
{
    (void)ctx;
    return product_get_target_temp();
}

static int32_t read_current_temp(void *ctx)
{
    (void)ctx;
    return product_get_current_temp();
}

static const tele_status_field_t s_product_status[] = {
    {
        .id = "pump.target_temp_c",
        .label = "Temperatura alvo",
        .description = "Setpoint atual de temperatura.",
        .group = "pump",
        .type = TELE_STATUS_TYPE_I32,
        .unit = "C",
        .channel_flags = TELE_CHANNEL_FLAG_MQTT | TELE_CHANNEL_FLAG_WEB,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_TECHNICAL,
        .read.i32 = read_target_temp,
    },
    {
        .id = "pump.current_temp_c",
        .label = "Temperatura atual",
        .description = "Temperatura medida no bloco.",
        .group = "pump",
        .type = TELE_STATUS_TYPE_I32,
        .unit = "C",
        .channel_flags = TELE_CHANNEL_FLAG_MQTT,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT,
        .read.i32 = read_current_temp,
    },
};

static esp_err_t product_register_status(void)
{
    return tele_status_register_fields(
        s_product_status,
        sizeof(s_product_status) / sizeof(s_product_status[0]));
}
```

Com os builders de payload em `NULL`, `tele_mqtt` publica `state`,
`heartbeat` e `meta/status` a partir desse registry.

### 5. Registre comandos de dominio

Use `tele_commands` apenas para acoes pontuais. Settings persistentes devem
ficar em `tele_config`.

```c
static esp_err_t handle_prime_pump(const char *cmd_name,
                                   const cJSON *args,
                                   cJSON **out_result,
                                   const char **out_error,
                                   uint32_t channel_flags,
                                   void *ctx)
{
    (void)cmd_name;
    (void)args;
    (void)channel_flags;
    (void)ctx;

    esp_err_t err = product_prime_pump();
    if (err != ESP_OK) {
        *out_error = "prime_failed";
        return err;
    }

    *out_result = cJSON_CreateObject();
    if (!*out_result) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(*out_result, "started", true);
    return ESP_OK;
}

static const tele_command_t s_product_commands[] = {
    {
        .name = "pump/prime",
        .label = "Escorvar bomba",
        .description = "Aciona a bomba por um ciclo curto.",
        .group = "pump",
        .channel_flags = TELE_CHANNEL_FLAG_MQTT | TELE_CHANNEL_FLAG_WEB,
        .flags = TELE_COMMAND_FLAG_MUTATING,
        .handler = handle_prime_pump,
    },
};

static esp_err_t product_register_commands(void)
{
    return tele_commands_register(
        s_product_commands,
        sizeof(s_product_commands) / sizeof(s_product_commands[0]));
}
```

O dispatcher deduplica comandos mutaveis por `cmd_id`. Para Serial ou LoRa,
basta criar um adapter que monte `tele_command_request_t` com
`required_channel_flags = TELE_CHANNEL_FLAG_SERIAL` ou
`TELE_CHANNEL_FLAG_LORA` e serialize `tele_command_response_t`.

### 6. Inicie o MQTT

Depois de registrar config, status e comandos de dominio, chame
`tele_mqtt_start()`:

```c
static bool product_mqtt_ready(void *ctx)
{
    (void)ctx;
    return product_network_ready() && product_time_ready();
}

static bool product_timestamp(char *buffer, size_t buffer_len, void *ctx)
{
    (void)ctx;
    return product_build_iso_timestamp(buffer, buffer_len) == ESP_OK;
}

static void product_restart(uint32_t delay_ms, void *ctx)
{
    (void)ctx;
    product_schedule_restart(delay_ms);
}

esp_err_t product_telemetry_start(void)
{
    ESP_RETURN_ON_ERROR(product_register_config(), TAG, "config");
    ESP_RETURN_ON_ERROR(product_register_status(), TAG, "status");
    ESP_RETURN_ON_ERROR(product_register_commands(), TAG, "commands");

    const tele_mqtt_config_t mqtt_config = {
        .broker_uri = CONFIG_PRODUCT_MQTT_BROKER_URI,
        .username = CONFIG_PRODUCT_MQTT_USERNAME,
        .password = CONFIG_PRODUCT_MQTT_PASSWORD,
        .base_topic = CONFIG_PRODUCT_MQTT_BASE_TOPIC,
        .device_id_prefix = CONFIG_PRODUCT_DEVICE_ID_PREFIX,
        .firmware_version = APP_VERSION_STRING,
        .heartbeat_interval_s = CONFIG_PRODUCT_HEARTBEAT_INTERVAL_S,
        .keepalive_s = CONFIG_PRODUCT_MQTT_KEEPALIVE_S,
        .qos_critical = 1,
        .qos_telemetry = 0,
        .is_ready = product_mqtt_ready,
        .build_timestamp = product_timestamp,
        .restart = product_restart,
    };

    return tele_mqtt_start(&mqtt_config);
}
```

Se algum prerequisito ficar pronto depois, chame
`tele_mqtt_start_client_if_ready()` no evento correspondente. O componente
tambem chama essa funcao internamente em alguns eventos Wi-Fi/IP, mas projetos
com Ethernet, modem, LoRa gateway ou outra pilha devem sinalizar a prontidao no
seu proprio adapter.

### 7. Escolha o que e generico e o que e produto

Regras praticas para manter isolamento:

- `tele_config`, `tele_status`, `tele_commands`, `tele_core_commands` e
  `tele_mqtt` nao devem conhecer sensores, atuadores, GPIOs ou politica de
  produto;
- um componente de produto registra campos e callbacks, como
  `product_system_registry`;
- adapters de transporte so traduzem protocolo: MQTT, HTTP, Serial ou LoRa
  entram em registries comuns;
- `channel_flags` define onde algo aparece; `flags` define comportamento;
- nao duplique comandos como `config/set` em cada transporte. Reuse
  `tele_commands_execute()`.

### 8. Habilite updates por manifest, se necessario

Para expor `artifacts/get`, `artifact/check`, `artifact/apply` e
`artifact/status` por MQTT, registre os handlers de artefato antes de iniciar o
MQTT:

```c
#include "firmware_ota.h"
#include "tele_artifacts.h"

static esp_err_t product_updates_start(void)
{
    ESP_RETURN_ON_ERROR(firmware_ota_init(), TAG, "firmware ota");
    ESP_RETURN_ON_ERROR(firmware_ota_register_artifact(), TAG, "artifact firmware");
    return tele_artifacts_register_commands();
}
```

O projeto deve fornecer `components/tele_system/include/firmware_version.h` com
a versao do firmware:

```c
#define APP_VERSION_SEMVER "1.0.0"
#define APP_VERSION_LABEL "Meu Produto"
#define APP_BUILD_ID "1.0.0-local"
#define APP_VERSION_STRING APP_VERSION_SEMVER " " APP_VERSION_LABEL
```

O manifest publicado pelo servidor de updates deve apontar para o `.bin`, tipo
`firmware`, canal e SHA-256. Gere esse arquivo com
`tools/update_artifacts/generate_manifest.py` ou com uma ferramenta equivalente
que preserve o mesmo schema.

Quando o novo produto tambem atualizar outros artefatos, crie outro componente
adapter e registre um `tele_artifact_handler_t`. O MQTT nao muda: os comandos
genericos continuam chamando o registry `tele_artifacts`.

### 9. Habilite portal web e upload OTA local, se necessario

Para usar o portal HTTP padrao, inicialize o firmware OTA e inicie o agregador
`tele_portal` depois que a rede/AP do produto estiver pronta:

```c
#include "firmware_ota.h"
#include "web_portal.h"

static esp_err_t product_portal_start(void)
{
    ESP_RETURN_ON_ERROR(firmware_ota_init(), TAG, "firmware ota");
    return web_portal_start(false);
}
```

Se `product_updates_start()` ja inicializou `firmware_ota`, nao chame
`firmware_ota_init()` novamente; mantenha uma unica inicializacao no bootstrap
do produto.

O agregador `tele_portal` registra as rotas de status, config, Wi-Fi, logs,
assets e OTA local. O upload OTA e conectado por
`tele_firmware_portal_ota`, que adapta `tele_portal_ota` para
`firmware_ota_upload_begin()`, `firmware_ota_upload_write()`,
`firmware_ota_upload_finalize()`, `firmware_ota_upload_abort()` e
`firmware_ota_get_status()`.

Se o produto nao usa `firmware_ota`, implemente outro binding:

```c
static const tele_portal_ota_config_t ota_config = {
    .begin = product_ota_begin,
    .write = product_ota_write,
    .finalize = product_ota_finalize,
    .abort = product_ota_abort,
    .status = product_ota_status,
    .restart_delay_ms = 1200,
};

ESP_ERROR_CHECK(tele_portal_ota_init(&ota_config));
ESP_ERROR_CHECK(tele_portal_ota_register_routes());
```

Essa separacao permite reaproveitar o portal em outro embarcado sem levar a
implementacao de OTA do TeleSystem.

## Identificacao do dispositivo

O `device_id` segue o formato:

```text
{CONFIG_MQTT_DEVICE_ID_PREFIX}-{ultimos_3_bytes_do_mac}
```

Com os defaults atuais:

```text
ESP32-Device-5112D0
```

Cada boot tambem gera um `session_id`, usado para diferenciar conexoes da mesma
placa ao longo do tempo.

## Base topic

Base topic:

```text
{CONFIG_MQTT_BASE_TOPIC}/{device_id}
```

Default atual:

```text
v1/telesystem/{device_id}
```

Topicos usados:

```text
v1/telesystem/{device_id}/availability
v1/telesystem/{device_id}/seen
v1/telesystem/{device_id}/state
v1/telesystem/{device_id}/heartbeat
v1/telesystem/{device_id}/event
v1/telesystem/{device_id}/meta/config
v1/telesystem/{device_id}/meta/status
v1/telesystem/{device_id}/meta/commands
v1/telesystem/{device_id}/cmd/in
v1/telesystem/{device_id}/cmd/out
```

## Publicacoes automaticas

### availability

Payload retido para online/offline. O mesmo topico e usado como LWT.

```json
{
  "device_id": "ESP32-Device-5112D0",
  "fw": "0.3.31 TeleSystem portal OTA adapter",
  "session_id": "20260609T120000Z-5112D0",
  "status": "online",
  "reason": "mqtt_connected",
  "ts": "2026-06-09T12:00:00Z"
}
```

### seen

Ultimo contato conhecido, retido no broker. Este topico permite que o Control
Center reinicie e ainda saiba quando o dispositivo foi visto pela ultima vez,
sem gravar nada em NVS/flash.

```json
{
  "device_id": "ESP32-Device-5112D0",
  "fw": "0.3.31 TeleSystem portal OTA adapter",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:01:00Z",
  "last_seen_ts": "2026-06-09T12:01:00Z",
  "reason": "heartbeat"
}
```

### state

Snapshot retido com conectividade, bateria e dados tecnicos curtos.

```json
{
  "device_id": "ESP32-Device-5112D0",
  "fw": "0.3.31 TeleSystem portal OTA adapter",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:00:00Z",
  "wifi_state": "sta_connected",
  "wifi_ready": true,
  "ssid": "Mau",
  "ip": "192.168.15.14",
  "rssi": -39,
  "vbat_mv": 0,
  "heap_free": 8195536,
  "uptime_s": 61,
  "heartbeat_interval_s": 60,
  "time_synchronized": true,
  "ota_state": "idle",
  "ota_in_progress": false,
  "ota_progress_pct": 0,
  "ota_target_version": "",
  "ota_last_error": ""
}
```

### heartbeat

Telemetria periodica, sem retenção.

```json
{
  "device_id": "ESP32-Device-5112D0",
  "fw": "0.3.31 TeleSystem portal OTA adapter",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:01:00Z",
  "uptime_s": 60,
  "rssi": -40,
  "heap_free": 8195536,
  "vbat_mv": 0,
  "wifi_state": "sta_connected",
  "ota_state": "idle",
  "ota_in_progress": false,
  "ota_progress_pct": 0
}
```

### meta/status

Manifesto retido dos campos de status conhecidos pelo firmware. O Control
Center usa `group`, `label` e `description` para organizar a exibicao e mostrar
ajuda por hover.

```json
{
  "device_id": "ESP32-Device-5112D0",
  "fw": "0.3.31 TeleSystem portal OTA adapter",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:00:00Z",
  "registry_revision": 1,
  "fields": [
    {
      "id": "rssi",
      "label": "RSSI",
      "description": "Intensidade do sinal Wi-Fi.",
      "group": "network",
      "type": "i32",
      "unit": "dBm",
      "channels": [
        {"channel": "mqtt"}
      ],
      "flags": [
        {"flag": "state"},
        {"flag": "heartbeat"}
      ]
    }
  ]
}
```

### meta/config

Manifesto retido dos campos configuraveis conhecidos pelo firmware. Nesta
primeira fase, o payload fica em topico unico e descreve id, tipo, origem,
default, valor efetivo, limites, choices opcionais de enum e flags de cada
campo exposto por MQTT.

```json
{
  "device_id": "ESP32-Device-5112D0",
  "fw": "0.3.31 TeleSystem portal OTA adapter",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:00:00Z",
  "registry_revision": 1,
  "fields": [
    {
      "id": "wifi.sta_max_retry",
      "type": "u32",
      "source": "default",
      "default": 3,
      "value": 3,
      "min": 1,
      "max": 20,
      "channels": [
        {"channel": "web"},
        {"channel": "mqtt"}
      ],
      "flags": [
        {"flag": "runtime_apply"}
      ]
    },
    {
      "id": "wifi.apsta_policy",
      "type": "enum",
      "source": "default",
      "default": 1,
      "value": 1,
      "min": 0,
      "max": 2,
      "choices": [
        {"value": 0, "label": "always_on"},
        {"value": 1, "label": "auto_timeout"},
        {"value": 2, "label": "sta_only"}
      ],
      "channels": [
        {"channel": "web"},
        {"channel": "mqtt"}
      ],
      "flags": [
        {"flag": "reboot_required"}
      ]
    }
  ]
}
```

No Control Center, as flags de aplicacao tambem orientam a cor do nome do
campo em Settings:

- verde: campo com `runtime_apply`, aplicado em runtime por callback;
- laranja: campo com `reboot_required`, salvo agora e efetivado apos reboot;
- cor normal: campo apenas armazenado, sem callback runtime nem reboot
  declarado.

### meta/commands

Manifesto retido dos comandos remotos conhecidos pelo firmware. O Control
Center usa este payload para descobrir comandos, argumentos e se o comando e
mutavel ou relacionado a reboot.

```json
{
  "device_id": "ESP32-Device-5112D0",
  "fw": "0.3.31 TeleSystem portal OTA adapter",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:00:00Z",
  "registry_revision": 1,
  "commands": [
    {
      "name": "config/set",
      "label": "Salvar configuracao",
      "description": "Atualiza um campo configuravel exposto por MQTT.",
      "group": "config",
      "mutating": true,
      "reboot_required": false,
      "internal": false,
      "channels": [
        {"channel": "mqtt"},
        {"channel": "web"}
      ],
      "flags": [
        {"flag": "mutating"}
      ],
      "args": [
        {"id": "id", "type": "string", "required": true, "min_len": 1, "max_len": 48},
        {"id": "value", "type": "any", "required": true}
      ]
    }
  ]
}
```

### event

Eventos discretos de firmware.

```json
{
  "device_id": "ESP32-Device-5112D0",
  "fw": "0.3.31 TeleSystem portal OTA adapter",
  "session_id": "20260609T120000Z-5112D0",
  "event": "boot",
  "message": "mqtt_online",
  "ts": "2026-06-09T12:00:00Z"
}
```

## Comandos

Comandos entram em:

```text
v1/telesystem/{device_id}/cmd/in
```

Respostas saem em:

```text
v1/telesystem/{device_id}/cmd/out
```

Formato base:

```json
{
  "cmd_id": "c1",
  "name": "ping"
}
```

Resposta base:

```json
{
  "device_id": "ESP32-Device-5112D0",
  "fw": "0.3.31 TeleSystem portal OTA adapter",
  "session_id": "20260609T120000Z-5112D0",
  "cmd_id": "c1",
  "ok": true,
  "ts": "2026-06-09T12:00:10Z",
  "result": {}
}
```

## Comandos disponiveis

### ping

```json
{"cmd_id":"c1","name":"ping"}
```

### get_state

```json
{"cmd_id":"c2","name":"get_state"}
```

### config/get

Retorna o manifesto de configuracao equivalente ao payload retido de
`meta/config`.

```json
{"cmd_id":"cfg-get-1","name":"config/get"}
```

### commands/get

Retorna o manifesto de comandos equivalente ao payload retido de
`meta/commands`.

```json
{"cmd_id":"cmds-get-1","name":"commands/get"}
```

### get_technical_status

Retorna uptime, sincronismo de tempo, heap, power-good e VBAT.

```json
{"cmd_id":"t1","name":"get_technical_status"}
```

### config/set

Atualiza um campo configuravel exposto por `meta/config`.

```json
{
  "cmd_id": "cfg1",
  "name": "config/set",
  "args": {
    "id": "wifi.sta_max_retry",
    "value": 5
  }
}
```

Resposta bem-sucedida:

```json
{
  "cmd_id": "cfg1",
  "ok": true,
  "result": {
    "id": "wifi.sta_max_retry",
    "stored": true,
    "applied": true,
    "requires_reboot": false
  }
}
```

Regras:

- `id` deve existir no registry e estar exposto no canal `mqtt`;
- `value` deve combinar com o tipo declarado em `meta/config`;
- cada comando atualiza um campo por vez;
- todos os valores passam por `tele_config_update_value()`;
- campos com `runtime_apply` sao aplicados por callback opcional antes de persistir;
- campos com `reboot_required` sao persistidos como override e entram em vigor no proximo boot ou apos comando de reboot;
- apos sucesso, o firmware republica `meta/config` retido;
- comandos mutaveis usam deduplicacao por `cmd_id`.

### config/reset

Remove o override NVS de um campo e volta ao default efetivo. Se o campo tiver
callback `runtime_apply`, o default tambem e aplicado em runtime.

```json
{
  "cmd_id": "cfg-reset-1",
  "name": "config/reset",
  "args": {
    "id": "wifi.sta_max_retry"
  }
}
```

Resposta bem-sucedida:

```json
{
  "cmd_id": "cfg-reset-1",
  "ok": true,
  "result": {
    "id": "wifi.sta_max_retry",
    "stored": false,
    "applied": true,
    "requires_reboot": false
  }
}
```

Depois de sucesso, o firmware republica `meta/config` retido.

### apply_and_reboot

Agenda reboot curto depois do ACK.

```json
{"cmd_id":"r1","name":"apply_and_reboot","args":{"delay_ms":800}}
```

### artifacts/get

Lista os tipos de artefato registrados no firmware.

```json
{
  "cmd_id": "artifacts-1",
  "name": "artifacts/get",
  "args": {}
}
```

Resposta inclui `registry_revision` e `artifacts`, com `artifact_type`,
`label`, `mode`, `default_restart_on_success` e `status_available`.

### artifact/check

Consulta um manifest remoto de qualquer artefato registrado sem aplicar.

```json
{
  "cmd_id": "ota-check-1",
  "name": "artifact/check",
  "args": {
    "artifact_type": "firmware",
    "manifest_url": "https://updates.example.com/telesystem/pilot/manifest.json",
    "channel": "pilot"
  }
}
```

Resposta bem-sucedida inclui `current_version`, `available`,
`target_version`, `build_id`, `artifact_type`, `mode`, `size`, `critical` e
`artifact_url`.

### artifact/status

Consulta estado local e progresso de um artefato registrado.

```json
{
  "cmd_id": "ota-status-1",
  "name": "artifact/status",
  "args": {
    "artifact_type": "firmware"
  }
}
```

Resposta inclui `state`, `current_version`, `target_version`, `last_error`,
`in_progress`, `bytes_done`, `total_size` e `progress_pct`.

### artifact/apply para firmware

Inicia OTA de firmware por manifest em streaming. O ACK confirma apenas que a
task foi iniciada; progresso e resultado ficam no status OTA.

```json
{
  "cmd_id": "ota-apply-1",
  "name": "artifact/apply",
  "args": {
    "artifact_type": "firmware",
    "manifest_url": "https://updates.example.com/telesystem/pilot/manifest.json",
    "channel": "pilot",
    "restart_on_success": true
  }
}
```

### artifact/apply para CA

Baixa, valida e ativa um bundle CA por manifest.

```json
{
  "cmd_id": "ca-apply-1",
  "name": "artifact/apply",
  "args": {
    "artifact_type": "ca_bundle",
    "manifest_url": "https://updates.example.com/ca/stable/bundle_ca.manifest.json",
    "channel": "stable",
    "restart_on_success": false
  }
}
```

## Extensao por produto

Novos comandos de dominio devem registrar `tele_command_t` com handler. Novos
tipos de arquivo atualizaveis por manifest devem registrar um handler em
`components/tele_artifacts`, reaproveitando `artifact/check` e
`artifact/apply`.

- `.handler`, para executar o comando e devolver `result`;
- `TELE_COMMAND_FLAG_MUTATING`, para comandos que alteram estado e precisam de
  deduplicacao por `cmd_id`;
- canais de exposicao, como `TELE_CHANNEL_FLAG_MQTT` ou
  `TELE_CHANNEL_FLAG_WEB`, para limitar quais transportes podem executar.

O componente `tele_mqtt` permanece responsavel por topicos, conexao, JSON e
ACK/NACK. A execucao e a deduplicacao de comandos mutaveis ficam em
`tele_commands_execute()`, que tambem pode ser chamado por portal HTTP ou
serial.
