# Componentes De Sistema

Este grupo contem infraestrutura local do firmware: versao, OTA, energia,
bateria e LED.

## Componentes

### `components/tele_system`

Inclui:

- `firmware_ota.c`: upload OTA, OTA por URL e OTA por manifest;
- `firmware_version.h`: versao normalizada do app;
- `vbat_monitor.c`: medicao e politica de bateria;
- `power_good.c`: controle de alimentacao de perifericos.

### `components/tele_signal`

Define tipos compartilhados de sinal fisico: `tele_signal_color_t`,
`tele_signal_effect_t`, IDs de efeito (`off`, `solid`, `blink`, `alternate`,
`breath`, `heartbeat`, `pulse`) e targets.

### `components/status_led`

Driver/stub de LED WS28xx. Ele nao conhece estados semanticos de aplicacao.
Recebe apenas `tele_signal_effect_t`:

```c
tele_signal_effect_t effect = {
    .id = TELE_SIGNAL_EFFECT_BLINK,
    .color_a = {.red = 0x00, .green = 0x40, .blue = 0xFF},
    .time_a_ms = 250,
    .time_b_ms = 750,
    .brightness = 80,
    .target_mask = TELE_SIGNAL_TARGET_ALL,
};
ESP_ERROR_CHECK(status_led_apply_effect(&effect));
```

### `components/tele_indicator`

Registry generico de indicadores logicos. Registra outputs, fontes e eventos.
Eventos ativos sao arbitrados por prioridade e recencia. Eventos com
`duration_ms > 0` sao limpos automaticamente.

Exemplo de uso pela aplicacao:

```c
tele_indicator_raise("wifi", "wifi.connecting");
tele_indicator_raise("battery", "battery.low");
tele_indicator_clear_source("wifi");
```

Manual completo: [tele_indicator.md](tele_indicator.md).

## Versao De Firmware

`firmware_version.h` define:

```c
#define APP_VERSION_SEMVER "0.3.31"
#define APP_VERSION_LABEL "TeleSystem portal OTA adapter"
#define APP_BUILD_ID "0.3.31-local"
#define APP_VERSION_STRING APP_VERSION_SEMVER " " APP_VERSION_LABEL
```

Use `APP_VERSION_SEMVER` para comparacao de update. Use
`APP_VERSION_STRING` para logs, MQTT e UI.

## OTA Local E Remoto

APIs principais:

```c
esp_err_t firmware_ota_init(void);
esp_err_t firmware_ota_register_artifact(void);
esp_err_t firmware_ota_start(const char *url);
esp_err_t firmware_ota_upload_begin(void);
esp_err_t firmware_ota_upload_write(const uint8_t *data, size_t data_len);
esp_err_t firmware_ota_upload_finalize(void);
void firmware_ota_upload_abort(void);
void firmware_ota_get_status(firmware_ota_status_t *status);
```

Uso:

- portal usa `firmware_ota_upload_*`;
- `firmware_ota_register_artifact()` registra `artifact_type = "firmware"` no
  registry generico `tele_artifacts`;
- comandos MQTT/portal usam `artifact/check` e `artifact/apply`;
- rotina principal futura pode chamar `tele_artifacts_check()` e
  `tele_artifacts_apply()` depois de Wi-Fi pronto.

## Status OTA

`firmware_ota_status_t` inclui:

- estado e flags: `state`, `in_progress`, `restart_pending`;
- versoes: `current_version`, `target_version`, `build_id`;
- origem: `url`, `manifest_url`, `artifact_url`;
- erro: `last_error`;
- particoes: `running_partition`, `next_update_partition`;
- progresso: `bytes_written`, `total_size`, `progress_pct`.

## Energia E Bateria

`vbat_monitor` mede VBAT e pode acionar shutdown por bateria baixa.

`power_good` controla uma GPIO para ligar/desligar perifericos externos. No
boot, `main/main.c`:

1. inicializa VBAT;
2. mede bateria se configurado;
3. inicializa power-good;
4. entra em deep sleep se a bateria estiver critica;
5. liga perifericos quando seguro.

## Inicializacao Atual

Trecho de alto nivel:

```c
tele_portal_logs_init();
ESP_ERROR_CHECK(vbat_monitor_init());
ESP_ERROR_CHECK(power_good_init());
ESP_ERROR_CHECK(tele_ca_store_init());
ESP_ERROR_CHECK(firmware_ota_init());
ESP_ERROR_CHECK(register_portal_ota_routes());
ESP_ERROR_CHECK(connectivity_controller_start());
ESP_ERROR_CHECK(mqtt_presence_start());
```

## Como Adicionar Rotina De Sistema

1. Mantenha regra de negocio em componente de dominio.
2. Inicialize o componente em `main/main.c`.
3. Se depender de rede, rode em task propria depois de
   `connectivity_controller_start()` e `wifi_manager_wait_until_ready()`.
4. Exponha status via `tele_status` quando for util para portal/MQTT.
